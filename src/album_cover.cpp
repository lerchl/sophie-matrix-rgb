#include "album_cover.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <iostream>

bool split_url(const std::string &url, std::string *origin, std::string *path) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        return false;

    const auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        *origin = url;
        *path = "/";
    } else {
        *origin = url.substr(0, path_start);
        *path = url.substr(path_start);
    }
    return true;
}

bool download_to_memory(const std::string &url, std::string *out) {
    std::string origin, path;
    if (!split_url(url, &origin, &path)) {
        std::cerr << "Could not parse URL: " << url << "\n";
        return false;
    }

    httplib::Client client(origin);
    client.set_follow_location(true);
    client.set_connection_timeout(10);
    client.set_read_timeout(15);

    auto res = client.Get(path);
    if (!res) {
        std::cerr << "HTTP request failed: " << httplib::to_string(res.error())
                  << "\n";
        return false;
    }
    if (res->status < 200 || res->status >= 300) {
        std::cerr << "HTTP error: " << res->status << "\n";
        return false;
    }

    *out = std::move(res->body);
    return true;
}

bool decode_image(const std::string &bytes, Image *img) {
    int w, h, channels;
    const auto *raw = reinterpret_cast<const uint8_t *>(bytes.data());
    uint8_t *data = stbi_load_from_memory(raw, static_cast<int>(bytes.size()),
                                          &w, &h, &channels, 3 /* force RGB */);
    if (!data) {
        std::cerr << "stb_image decode failed: " << stbi_failure_reason()
                  << "\n";
        return false;
    }
    img->width = w;
    img->height = h;
    img->pixels.assign(data, data + (static_cast<size_t>(w) * h * 3));
    stbi_image_free(data);
    return true;
}

void draw_resized_square(const Image &src, int dst_size, int offset_x,
                         int offset_y, rgb_matrix::FrameCanvas *canvas) {
    const float scale_x = static_cast<float>(src.width) / dst_size;
    const float scale_y = static_cast<float>(src.height) / dst_size;

    for (int y = 0; y < dst_size; ++y) {
        for (int x = 0; x < dst_size; ++x) {
            const float sx = x * scale_x;
            const float sy = y * scale_y;

            const int x0 = std::min(static_cast<int>(sx), src.width - 2);
            const int y0 = std::min(static_cast<int>(sy), src.height - 2);
            const float fx = sx - x0;
            const float fy = sy - y0;

            auto lerp = [](float a, float b, float t) {
                return a + (b - a) * t;
            };

            uint8_t rgb[3];
            for (int c = 0; c < 3; ++c) {
                const float p00 = src.pixels[(y0 * src.width + x0) * 3 + c];
                const float p10 = src.pixels[(y0 * src.width + x0 + 1) * 3 + c];
                const float p01 =
                    src.pixels[((y0 + 1) * src.width + x0) * 3 + c];
                const float p11 =
                    src.pixels[((y0 + 1) * src.width + x0 + 1) * 3 + c];
                const float top = lerp(p00, p10, fx);
                const float bottom = lerp(p01, p11, fx);
                rgb[c] =
                    static_cast<uint8_t>(std::round(lerp(top, bottom, fy)));
            }

            canvas->SetPixel(offset_x + x, offset_y + y, rgb[0], rgb[1],
                             rgb[2]);
        }
    }
}

void draw_spinning_circle(const Image &src, float angle_radians, int dst_size,
                          int offset_x, int offset_y,
                          rgb_matrix::FrameCanvas *canvas, float padding) {
    if (src.width < 2 || src.height < 2 || src.pixels.empty()) {
        return;
    }

    const float cos_a = std::cos(angle_radians);
    const float sin_a = std::sin(angle_radians);

    const float src_cx = src.width / 2.0f;
    const float src_cy = src.height / 2.0f;
    const float dst_c = dst_size / 2.0f;

    // Full disc radius (outer edge of the vinyl).
    const float radius = dst_size / 2.0f;

    // Album art ends here; the band between art_radius and radius is the
    // matte outer rim (its width is controlled by `padding`).
    const float art_radius = std::max(0.0f, radius - padding);

    // Matte inner ring around the spindle hole; art starts outside of it.
    const float hole_radius = radius * 0.05f;
    const float inner_ring_radius =
        std::max(hole_radius + 1.0f, radius * 0.20f);

    // Map the source so its shortest side fits the art area.
    const float scale = std::min(src.width, src.height) / (art_radius * 2.0f);

    // Matte vinyl colors: dark, low contrast, no specular shine.
    const float matte_base[3] = {22.0f, 22.0f, 24.0f};
    const float matte_lip[3] = {44.0f, 44.0f, 47.0f}; // subtle bevel highlight

    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    auto clamp01 = [](float v) { return std::min(1.0f, std::max(0.0f, v)); };

    for (int y = 0; y < dst_size; ++y) {
        for (int x = 0; x < dst_size; ++x) {
            const float dx = x - dst_c + 0.5f;
            const float dy = y - dst_c + 0.5f;
            const float dist = std::sqrt(dx * dx + dy * dy);

            // Cheap reject outside the disc / inside the hole (with 0.5px
            // margin for anti-aliasing).
            if (dist > radius + 0.5f || dist < hole_radius - 0.5f) {
                continue;
            }

            // Coverage of the whole disc (anti-aliased outer edge and hole
            // edge).
            const float disc_cov = clamp01(radius - dist + 0.5f) *
                                   clamp01(dist - hole_radius + 0.5f);
            if (disc_cov <= 0.0f) {
                continue;
            }

            // Coverage of the album art (between inner ring and outer rim).
            float art_cov = clamp01(art_radius - dist + 0.5f) *
                            clamp01(dist - inner_ring_radius + 0.5f);

            // --- Matte color with a faint lip at the outer edge and the hole
            // edge.
            const float lip_outer = clamp01(1.0f - (radius - dist));
            const float lip_hole = clamp01(1.0f - (dist - hole_radius));
            const float lip = std::max(lip_outer, lip_hole) * 0.6f;

            float color[3];
            for (int c = 0; c < 3; ++c) {
                color[c] = lerp(matte_base[c], matte_lip[c], lip);
            }

            // --- Album art, rotated.
            if (art_cov > 0.0f) {
                const float rx = dx * cos_a + dy * sin_a;
                const float ry = -dx * sin_a + dy * cos_a;

                const float sx = src_cx + rx * scale;
                const float sy = src_cy + ry * scale;

                if (sx < 0.0f || sy < 0.0f || sx >= src.width - 1 ||
                    sy >= src.height - 1) {
                    art_cov = 0.0f; // fall back to matte
                } else {
                    const int x0 = static_cast<int>(sx);
                    const int y0 = static_cast<int>(sy);
                    const float fx = sx - x0;
                    const float fy = sy - y0;

                    for (int c = 0; c < 3; ++c) {
                        const float p00 =
                            src.pixels[(y0 * src.width + x0) * 3 + c];
                        const float p10 =
                            src.pixels[(y0 * src.width + x0 + 1) * 3 + c];
                        const float p01 =
                            src.pixels[((y0 + 1) * src.width + x0) * 3 + c];
                        const float p11 =
                            src.pixels[((y0 + 1) * src.width + x0 + 1) * 3 + c];

                        const float top = lerp(p00, p10, fx);
                        const float bottom = lerp(p01, p11, fx);
                        const float art = lerp(top, bottom, fy);

                        color[c] = lerp(color[c], art, art_cov);
                    }
                }
            }

            // Fade by disc coverage (blends toward black on the LED panel).
            uint8_t rgb[3];
            for (int c = 0; c < 3; ++c) {
                rgb[c] = static_cast<uint8_t>(
                    std::round(clamp01(color[c] * disc_cov / 255.0f) * 255.0f));
            }

            canvas->SetPixel(offset_x + x, offset_y + y, rgb[0], rgb[1],
                             rgb[2]);
        }
    }
}
