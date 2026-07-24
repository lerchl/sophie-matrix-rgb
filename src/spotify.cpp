#include "spotify.hpp"
#include "base64.hpp"
#include "error.hpp"

#include <atomic>
#include <condition_variable>
#include <httplib.h>
#include <iostream>
#include <optional>

constexpr auto kSpotifyAccountsHost = "https://accounts.spotify.com";
constexpr auto kSpotifyApiHost = "https://api.spotify.com";
constexpr auto kRefreshBuffer = std::chrono::seconds(60);

void spotifyPollLoop(
    std::condition_variable &app_cv, std::mutex &app_mutex,
    const std::atomic<bool> &app_running, ConfigManager &config_manager,
    std::atomic<std::shared_ptr<std::optional<CurrentlyPlaying>>>
        &currently_playing) {

    while (app_running) {
        std::cout << "loop" << std::endl;
        auto config = config_manager.get();

        const bool has_auth = config->spotify_authorization.has_value();
        const bool has_creds =
            config->spotify_api_credentials.has_value() &&
            config->spotify_api_credentials->id.has_value() &&
            config->spotify_api_credentials->secret.has_value();

        if (!has_auth || !has_creds) {
            std::cout
                << "Not authorized with a spotify account or spotify api "
                   "credentials not event set, trying again in 15 seconds..."
                << std::endl;
            std::unique_lock<std::mutex> lock(app_mutex);
            app_cv.wait_for(lock, std::chrono::seconds(15),
                            [&app_running] { return !app_running.load(); });
            continue;
        }

        SpotifyAuthorization auth = *config->spotify_authorization;
        SpotifyApiCredentials creds = *config->spotify_api_credentials;

        bool refresh_ok = true;
        if (auth.expires_at <=
            std::chrono::system_clock::now() + kRefreshBuffer) {
            httplib::Client accounts_client(kSpotifyAccountsHost);
            accounts_client.set_connection_timeout(10);
            accounts_client.set_read_timeout(10);

            const std::string credentials = *creds.id + ":" + *creds.secret;

            httplib::Headers headers = {
                {"Authorization", "Basic " + base64_encode(credentials)},
            };
            const std::string body =
                "grant_type=refresh_token&refresh_token=" + auth.refresh_token;

            auto res =
                accounts_client.Post("/api/token", headers, body,
                                     "application/x-www-form-urlencoded");

            if (!res || res->status != 200) {
                std::cerr << "Spotify token refresh failed"
                          << (res ? (", status " + std::to_string(res->status))
                                  : "")
                          << std::endl;
                refresh_ok = false;
            } else {
                try {
                    const auto tokens = json::parse(res->body);
                    auth.access_token =
                        tokens.at("access_token").get<std::string>();
                    // Spotify doesn't always rotate the refresh_token — keep
                    // the old one if it's not present in the response.
                    if (tokens.contains("refresh_token")) {
                        auth.refresh_token =
                            tokens.at("refresh_token").get<std::string>();
                    }
                    const int expires_in = tokens.at("expires_in").get<int>();
                    auth.expires_at = std::chrono::system_clock::now() +
                                      std::chrono::seconds(expires_in);
                    config_manager.patch(auth); // persist refreshed tokens
                } catch (const std::exception &e) {
                    std::cerr << "Failed to parse Spotify refresh response: "
                              << e.what() << std::endl;
                    refresh_ok = false;
                }
            }
        }

        if (!refresh_ok) {
            // Likely a revoked/invalid refresh_token — back off rather than
            // retrying every 5s against a broken state.
            std::unique_lock<std::mutex> lock(app_mutex);
            app_cv.wait_for(lock, std::chrono::seconds(15),
                            [&app_running] { return !app_running.load(); });
            continue;
        }

        // --- Fetch currently-playing ---
        httplib::Client api_client(kSpotifyApiHost);
        api_client.set_connection_timeout(10);
        api_client.set_read_timeout(10);

        httplib::Headers headers = {
            {"Authorization", "Bearer " + auth.access_token}};
        auto res = api_client.Get("/v1/me/player/currently-playing", headers);

        if (!res) {
            std::cerr << "Spotify currently-playing request failed: "
                      << httplib::to_string(res.error()) << std::endl;
            // Keep last published value; try again next cycle.
        } else if (res->status == 204) {
            currently_playing.store(
                std::make_shared<std::optional<CurrentlyPlaying>>(
                    CurrentlyPlaying{/*is_paused=*/true,
                                     /*album_cover_url=*/std::nullopt}));
        } else if (res->status == 401) {
            // Token rejected despite passing our expiry check — e.g. user
            // revoked access on Spotify's side. Let next cycle sort it out.
            std::cerr << "Spotify currently-playing: unauthorized (401)"
                      << std::endl;
        } else if (res->status != 200) {
            std::cerr << "Spotify currently-playing failed, status "
                      << res->status << std::endl;
        } else {
            try {
                const auto body = json::parse(res->body);

                CurrentlyPlaying dto;
                dto.is_paused = !body.value("is_playing", false);
                dto.album_cover_url = std::nullopt;

                if (body.contains("item") && !body.at("item").is_null()) {
                    const auto &item = body.at("item");
                    if (item.contains("album") &&
                        item.at("album").contains("images")) {
                        const auto &images = item.at("album").at("images");

                        const nlohmann::json *smallest = nullptr;
                        for (const auto &img : images) {
                            int width = img.at("width").get<int>();
                            if (width < 64) {
                                continue;
                            }
                            if (smallest == nullptr ||
                                width < smallest->at("width").get<int>()) {
                                smallest = &img;
                            }
                        }

                        if (smallest != nullptr) {
                            dto.album_cover_url =
                                smallest->at("url").get<std::string>();
                        }
                    }
                }

                if (dto.album_cover_url) {
                    std::cout << *dto.album_cover_url << std::endl;
                } else {
                    std::cout << "(no album cover)" << std::endl;
                }

                currently_playing.store(
                    std::make_shared<std::optional<CurrentlyPlaying>>(dto));
            } catch (const std::exception &e) {
                std::cerr << "Failed to parse Spotify currently-playing "
                             "response: "
                          << e.what() << std::endl;
            }
        }

        std::unique_lock<std::mutex> lock(app_mutex);
        app_cv.wait_for(lock, std::chrono::seconds(5),
                        [&app_running] { return !app_running.load(); });
    }
}
