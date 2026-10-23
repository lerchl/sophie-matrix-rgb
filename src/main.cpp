#include "graphics.h"
#include "http_server.hpp"
#include "led-matrix.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <getopt.h>
#include <httplib.h>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>

#include "album_cover.hpp"
#include "config.hpp"
#include "config_manager.hpp"
#include "http_server.hpp"
#include "spotify.hpp"

using json = nlohmann::json;

std::condition_variable app_cv;
std::mutex app_mutex;
std::atomic<bool> app_running{true};

rgb_matrix::RGBMatrix *matrix;
httplib::Server http_server;
std::thread http_server_thread;
std::thread spotify_job_thread;

static void interrupt_handler(int) {
    app_running = false;
    app_cv.notify_all();

    http_server.stop();
    http_server_thread.join();
    spotify_job_thread.join();

    delete matrix;

    std::cout << std::endl;
    exit(0);
}

static int usage(const char *progname) {
    fprintf(stderr, "usage: %s [options]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "\t-s <settings-file>   : Config file path to use (will be "
                    "created if it does not exist).\n");
    fprintf(stderr, "\t-c <url>             : Url for spotify callback.\n");
    fprintf(stderr, "\t-f <url>             : Url to frontend.\n");
    fprintf(stderr,
            "\t-p <port>            : Port to listen on Default: 8080.\n");
    rgb_matrix::PrintMatrixFlags(stderr);
    return 1;
}

Time nowTime() {
    auto now = std::chrono::system_clock::now();

    std::chrono::zoned_time zt{std::chrono::current_zone(), now};
    auto local = zt.get_local_time();

    auto since_midnight = local - std::chrono::floor<std::chrono::days>(local);

    auto h = std::chrono::duration_cast<std::chrono::hours>(since_midnight);
    auto m =
        std::chrono::duration_cast<std::chrono::minutes>(since_midnight - h);

    return Time{static_cast<int>(h.count()), static_cast<int>(m.count())};
}

int main(int argc, char *argv[]) {
    rgb_matrix::RGBMatrix::Options matrix_options;
    // Use the custom pinout by default; still overridable with
    // --led-gpio-mapping=<name>.
    matrix_options.hardware_mapping = "sophie";
    rgb_matrix::RuntimeOptions runtime_opt;

    if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrix_options,
                                           &runtime_opt)) {
        return usage(argv[0]);
    }

    rgb_matrix::Color bg_color(0, 0, 0);

    int port = 8080;
    const char *config_file_path = NULL;
    const char *spotify_callback_url = NULL;
    const char *frontend_url = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:s:c:f:")) != -1) {
        switch (opt) {
        case 'p':
            port = std::stoi(optarg);
            break;
        case 's':
            config_file_path = strdup(optarg);
            break;
        case 'c':
            spotify_callback_url = strdup(optarg);
            break;
        case 'f':
            frontend_url = strdup(optarg);
            break;
        default:
            return usage(argv[0]);
        }
    }

    if (config_file_path == NULL) {
        fprintf(stderr, "Need to specify a config file path with -s\n");
        return usage(argv[0]);
    }

    if (spotify_callback_url == NULL) {
        fprintf(stderr, "Need to specify the spotify callback url with -c\n");
        return usage(argv[0]);
    }

    if (frontend_url == NULL) {
        fprintf(stderr, "Need to specify the frontend url with -f\n");
        return usage(argv[0]);
    }

    auto config_manager = ConfigManager(config_file_path);

    matrix =
        rgb_matrix::RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
    if (matrix == NULL) {
        return 1;
    }

    auto *offscreen = matrix->CreateFrameCanvas();

    signal(SIGTERM, interrupt_handler);
    signal(SIGINT, interrupt_handler);

    std::atomic<std::shared_ptr<std::optional<CurrentlyPlaying>>>
        currently_playing =
            std::make_shared<std::optional<CurrentlyPlaying>>(std::nullopt);

    auto run_http_server = make_http_server(config_manager);
    http_server_thread = std::thread([&run_http_server, port,
                                      spotify_callback_url, frontend_url]() {
        run_http_server(http_server, port, spotify_callback_url, frontend_url);
    });
    spotify_job_thread = std::thread([&]() {
        spotifyPollLoop(app_cv, app_mutex, app_running, config_manager,
                        currently_playing);
    });

    bool lastFrameInBlackoutWindow = false;
    std::optional<CurrentlyPlaying> last_currently_playing;
    Image current_album_art;

    for (;;) {
        auto current_config = config_manager.get();

        if (matrix->brightness() != current_config->brightness) {
            matrix->SetBrightness(current_config->brightness);
        }

        if (current_config->blackout_window.isDuringBlackout(nowTime())) {
            if (!lastFrameInBlackoutWindow) {
                std::cout << std::format("{0:%F_%T} - Entered blackout window.",
                                         std::chrono::system_clock::now())
                          << std::endl;
                lastFrameInBlackoutWindow = true;
            }

            offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);
            offscreen = matrix->SwapOnVSync(offscreen);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        if (lastFrameInBlackoutWindow) {
            std::cout << std::format("{0:%F_%T} - Exited blackout window.",
                                     std::chrono::system_clock::now())
                      << std::endl;
            lastFrameInBlackoutWindow = false;
        }

        auto current_currently_playing =
            currently_playing.load(std::memory_order_acquire);

        offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

        if (current_currently_playing->has_value() &&
            current_currently_playing->value().album_cover_url.has_value() &&
            (!last_currently_playing.has_value() ||
             !last_currently_playing.value().album_cover_url.has_value() ||
             last_currently_playing.value().album_cover_url.value() !=
                 current_currently_playing->value().album_cover_url.value())) {
            std::string raw_bytes;
            if (!download_to_memory(
                    current_currently_playing->value().album_cover_url.value(),
                    &raw_bytes)) {
                std::cerr << "Failed to download image from "
                          << current_currently_playing->value()
                                 .album_cover_url.value()
                          << std::endl;
                current_album_art = Image();
            }
            if (!decode_image(raw_bytes, &current_album_art)) {
                std::cerr << "Failed to decode image from "
                          << current_currently_playing->value()
                                 .album_cover_url.value()
                          << std::endl;
                current_album_art = Image();
            }
        }

        static float album_art_angle = 0.0f;
        static auto album_art_last_update = std::chrono::steady_clock::now();
        const float rotations_per_second = 0.05f; // tune to taste

        auto now = std::chrono::steady_clock::now();
        float delta_seconds =
            std::chrono::duration<float>(now - album_art_last_update).count();
        album_art_last_update = now;

        bool is_playing = current_currently_playing->has_value() &&
                          !current_currently_playing->value().is_paused;

        if (is_playing) {
            album_art_angle += 2.0f * static_cast<float>(M_PI) *
                               rotations_per_second * delta_seconds;
            if (album_art_angle > 2.0f * static_cast<float>(M_PI)) {
                album_art_angle -= 2.0f * static_cast<float>(M_PI);
            }
        }

        if (current_currently_playing->has_value() &&
            current_album_art.width > 0) {
            if (current_config->mode == FULL) {
                draw_resized_square(current_album_art, 64, 0, 0, offscreen);
            } else if (current_config->mode == VINYL) {
                draw_spinning_circle(current_album_art, album_art_angle, 64, 0,
                                     0, offscreen, 2.0f);
            }
        }

        last_currently_playing = *current_currently_playing;
        offscreen = matrix->SwapOnVSync(offscreen);
    }
}
