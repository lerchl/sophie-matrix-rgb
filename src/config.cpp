#include "config.hpp"
#include <chrono>
#include <cstdint>
#include <string>

NLOHMANN_JSON_SERIALIZE_ENUM(Mode, {
                                       {FULL, "FULL"},
                                       {VINYL, "VINYL"},
                                   })

int Time::toMinutes() const { return hour * 60 + minute; }

void from_json(const json &j, Time &t) {
    t.hour = j.at("hour").get<int>();
    t.minute = j.at("minute").get<int>();
}

void to_json(json &j, const Time &t) {
    j = json{{"hour", t.hour}, {"minute", t.minute}};
}

bool BlackoutWindow::isDuringBlackout(const Time &time) const {
    if (override) {
        return false;
    }
    const int s = start.toMinutes();
    const int e = end.toMinutes();
    const int t = time.toMinutes();
    if (s <= e) {
        return t >= s && t < e;
    }
    return t >= s || t < e;
}

void from_json(const json &j, BlackoutWindow &b) {
    j.at("start").get_to(b.start);
    j.at("end").get_to(b.end);
    j.at("override").get_to(b.override);
}

void to_json(json &j, const BlackoutWindow &b) {
    j = json{
        {"start", b.start},
        {"end", b.end},
        {"override", b.override},
    };
}

void from_json(const json &j, SpotifyAuthorization &s) {
    j.at("accessToken").get_to(s.access_token);
    j.at("refreshToken").get_to(s.refresh_token);
    const auto expires = j.at("expiresAt").get<std::int64_t>();
    s.expires_at =
        std::chrono::system_clock::time_point{std::chrono::seconds{expires}};
}

void to_json(json &j, const SpotifyAuthorization &s) {
    const auto expires = std::chrono::duration_cast<std::chrono::seconds>(
                             s.expires_at.time_since_epoch())
                             .count();
    j = json{
        {"accessToken", s.access_token},
        {"refreshToken", s.refresh_token},
        {"expiresAt", expires},
    };
}

void from_json(const json &j, SpotifyApiCredentials &s) {
    if (j.contains("id") && !j.at("id").is_null()) {
        s.id = j.at("id").get<std::string>();
    } else {
        s.id = std::nullopt;
    }
    if (j.contains("secret") && !j.at("secret").is_null()) {
        s.secret = j.at("secret").get<std::string>();
    } else {
        s.secret = std::nullopt;
    }
}

void to_json(json &j, const SpotifyApiCredentials &s) {
    j = json{
        {"id", s.id.has_value() ? json(*s.id) : json(nullptr)},
        {"secret", s.secret.has_value() ? json(*s.secret) : json(nullptr)},
    };
}
void from_json(const json &j, Configuration &c) {
    j.at("mode").get_to(c.mode);
    j.at("brightness").get_to(c.brightness);
    j.at("blackoutWindow").get_to(c.blackout_window);

    if (j.contains("spotifyAuthorization") &&
        !j.at("spotifyAuthorization").is_null()) {
        c.spotify_authorization =
            j.at("spotifyAuthorization").get<SpotifyAuthorization>();
    } else {
        c.spotify_authorization = std::nullopt;
    }

    if (j.contains("spotifyApiCredentials") &&
        !j.at("spotifyApiCredentials").is_null()) {
        c.spotify_api_credentials =
            j.at("spotifyApiCredentials").get<SpotifyApiCredentials>();
    } else {
        c.spotify_api_credentials = std::nullopt;
    }
}

void to_json(json &j, const Configuration &c) {
    j = json{
        {"mode", c.mode},
        {"brightness", c.brightness},
        {"blackoutWindow", c.blackout_window},
    };
    j["spotifyAuthorization"] = c.spotify_authorization.has_value()
                                    ? json(*c.spotify_authorization)
                                    : json(nullptr);
    j["spotifyApiCredentials"] = c.spotify_api_credentials.has_value()
                                     ? json(*c.spotify_api_credentials)
                                     : json(nullptr);
}

void to_json(json &j, const SpotifyApiCredentialsViewDto &s) {
    j = json{
        {"id", s.id.has_value() ? json(*s.id) : json(nullptr)},
        {"secretSet", s.secret_set},
    };
}
void to_json(json &j, const ConfigurationViewDto &v) {
    j = json{};

    j["brightness"] = v.brightness;
    j["mode"] = v.mode;
    j["blackoutWindow"] = v.blackout_window;
    j["spotifyAuthorized"] = v.spotify_authorized;
    j["spotifyApiCredentials"] = v.spotify_api_credentials;
}

void from_json(const json &j, ConfigurationPatchDto &p) {
    if (j.contains("brightness")) {
        p.brightness.provided = true;
        p.brightness.value = j.at("brightness").get<int>();
    }

    if (j.contains("mode")) {
        p.mode.provided = true;
        p.mode.value = j.at("mode").get<Mode>();
    }

    if (j.contains("blackoutWindow")) {
        p.blackout_window.provided = true;
        p.blackout_window.value = j.at("blackoutWindow").get<BlackoutWindow>();
    }

    if (j.contains("spotifyApiCredentialsId")) {
        p.spotify_api_id.provided = true;

        if (!j.at("spotifyApiCredentialsId").is_null()) {
            p.spotify_api_id.value =
                j.at("spotifyApiCredentialsId").get<std::string>();
        }
    }

    if (j.contains("spotifyApiCredentialsSecret")) {
        p.spotify_api_secret.provided = true;

        if (!j.at("spotifyApiCredentialsSecret").is_null()) {
            p.spotify_api_secret.value =
                j.at("spotifyApiCredentialsSecret").get<std::string>();
        }
    }
}
