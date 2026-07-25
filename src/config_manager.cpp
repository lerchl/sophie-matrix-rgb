#include "config_manager.hpp"
#include "config.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;

const Configuration default_configuration =
    Configuration{.mode = FULL,
                  .brightness = 80,
                  .blackout_window = {.start = {.hour = 0, .minute = 0},
                                      .end = {.hour = 0, .minute = 0},
                                      .override = false},
                  .spotify_authorization = std::nullopt,
                  .spotify_api_credentials = std::nullopt};

ConfigManager::ConfigManager(std::string path) : path_(std::move(path)) {
    config_.store(std::make_shared<Configuration>(load()));
}

std::shared_ptr<const Configuration> ConfigManager::get() const {
    return config_.load();
}

bool ConfigManager::patch(const ConfigurationPatchDto &patch) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    auto current = config_.load();
    auto next = std::make_shared<Configuration>(*current);

    if (patch.brightness.provided) {
        if (!patch.brightness.value) {
            return false;
        }

        if (*patch.brightness.value < 0 || *patch.brightness.value > 100) {
            return false;
        }

        next->brightness = *patch.brightness.value;
    }

    if (patch.mode.provided) {
        if (!patch.mode.value) {
            return false;
        }

        next->mode = *patch.mode.value;
    }

    if (patch.blackout_window.provided) {
        if (!patch.blackout_window.value) {
            return false;
        }

        next->blackout_window = *patch.blackout_window.value;
    }

    if (patch.spotify_api_id.provided || patch.spotify_api_secret.provided) {
        SpotifyApiCredentials updated =
            next->spotify_api_credentials.value_or(SpotifyApiCredentials{});

        if (patch.spotify_api_id.provided) {
            if (patch.spotify_api_id.value) {
                updated.id = *patch.spotify_api_id.value;
            } else {
                updated.id = std::nullopt;
            }
        }

        if (patch.spotify_api_secret.provided) {
            if (patch.spotify_api_secret.value) {
                updated.secret = *patch.spotify_api_secret.value;
            } else {
                updated.secret = std::nullopt;
            }
        }

        // Remove credentials object if both fields are empty
        if (!updated.id && !updated.secret) {
            next->spotify_api_credentials = std::nullopt;
        } else {
            next->spotify_api_credentials = updated;
        }

        // Any credential modification invalidates the authorization
        next->spotify_authorization = std::nullopt;
    }

    config_.store(next);
    save(*next);

    return true;
}

bool ConfigManager::patch(
    const std::optional<SpotifyAuthorization> &spotify_authorization) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    auto current = config_.load();
    auto next = std::make_shared<Configuration>(*current);

    next->spotify_authorization = spotify_authorization;

    config_.store(next);
    save(*next);

    return true;
}

Configuration ConfigManager::load() const {
    if (!std::filesystem::is_regular_file(path_)) {
        return default_configuration;
    }

    std::ifstream f(path_);
    if (!f) {
        return default_configuration;
    }

    json j;
    f >> j;

    return j.get<Configuration>();
}

void ConfigManager::save(const Configuration &c) const {
    json j = c;
    std::string tmp = path_ + ".tmp";

    {
        std::ofstream f(tmp);
        if (!f.is_open()) {
            throw std::runtime_error("Cannot open " + tmp);
        }
        f << j.dump(4);
        f.flush();
        if (!f) {
            throw std::runtime_error("Failed writing config file");
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    if (ec) {
        throw std::runtime_error("Rename failed: " + ec.message());
    }
}
