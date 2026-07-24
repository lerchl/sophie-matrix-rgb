#pragma once

#include "config_manager.hpp"
#include <condition_variable>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

using json = nlohmann::json;

struct CurrentlyPlaying {
    bool is_paused;
    std::optional<std::string> album_cover_url;

    bool operator==(const CurrentlyPlaying &) const = default;
};

void spotifyPollLoop(
    std::condition_variable &app_cv, std::mutex &app_mutex,
    const std::atomic<bool> &app_running, ConfigManager &config_manager,
    std::atomic<std::shared_ptr<std::optional<CurrentlyPlaying>>>
        &currently_playing);
