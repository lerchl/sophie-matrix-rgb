#pragma once
#include "config.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

class ConfigManager {
  public:
    explicit ConfigManager(std::string path);

    bool patch(const ConfigurationPatchDto &patch);
    bool
    patch(const std::optional<SpotifyAuthorization> &spotify_authorization);

    std::shared_ptr<const Configuration> get() const;

  private:
    std::mutex write_mutex_;
    std::string path_;
    std::atomic<std::shared_ptr<Configuration>> config_;

    Configuration load() const;
    void save(const Configuration &c) const;
};
