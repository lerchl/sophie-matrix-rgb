#pragma once

#include <nlohmann/json.hpp>
#include <optional>

using json = nlohmann::json;

enum Mode { FULL, VINYL };

struct Time {
    int hour;
    int minute;

    int toMinutes() const;
};
void from_json(const json &j, Time &t);
void to_json(json &j, const Time &t);

struct BlackoutWindow {
    Time start;
    Time end;
    bool override;

    bool isDuringBlackout(const Time &) const;
};
void from_json(const json &j, BlackoutWindow &b);
void to_json(json &j, const BlackoutWindow &b);

struct SpotifyAuthorization {
    std::string access_token;
    std::string refresh_token;
    std::chrono::system_clock::time_point expires_at;
};
void from_json(const json &j, SpotifyAuthorization &s);
void to_json(json &j, const SpotifyAuthorization &s);

struct SpotifyApiCredentials {
    std::optional<std::string> id;
    std::optional<std::string> secret;
};
void from_json(const json &j, SpotifyApiCredentials &s);
void to_json(json &j, const SpotifyApiCredentials &s);

struct Configuration {
    Mode mode;
    int brightness;
    BlackoutWindow blackout_window;
    std::optional<SpotifyAuthorization> spotify_authorization;
    std::optional<SpotifyApiCredentials> spotify_api_credentials;
};
void from_json(const json &j, Configuration &c);
void to_json(json &j, const Configuration &c);

struct SpotifyApiCredentialsViewDto {
    std::optional<std::string> id;
    bool secret_set;
};
void to_json(json &j, const SpotifyApiCredentialsViewDto &s);

struct ConfigurationViewDto {
    int brightness;
    Mode mode;
    BlackoutWindow blackout_window;
    bool spotify_authorized;
    SpotifyApiCredentialsViewDto spotify_api_credentials;
};
void to_json(json &j, const ConfigurationViewDto &v);

template <typename T> struct PatchField {
    bool provided = false;
    std::optional<T> value;
};

struct ConfigurationPatchDto {
    PatchField<int> brightness;
    PatchField<Mode> mode;
    PatchField<BlackoutWindow> blackout_window;
    PatchField<std::string> spotify_api_id;
    PatchField<std::string> spotify_api_secret;
};
void from_json(const json &j, ConfigurationPatchDto &p);
