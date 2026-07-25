#include "base64.hpp"
#include "config.hpp"
#include "config_manager.hpp"
#include "error.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <sstream>
#include <string>

using json = nlohmann::json;

constexpr auto kSpotifyAuthorizeUrl = "https://accounts.spotify.com/authorize";
constexpr auto kSpotifyAccountsHost = "https://accounts.spotify.com";
constexpr auto kSpotifyScopes = "user-read-playback-state";

SpotifyApiCredentialsViewDto
to_view_dto(const std::optional<SpotifyApiCredentials> &creds) {
    if (!creds) {
        return SpotifyApiCredentialsViewDto{.id = std::nullopt,
                                            .secret_set = false};
    }
    return SpotifyApiCredentialsViewDto{
        .id = creds->id, .secret_set = creds->secret.has_value()};
}

std::function<void(httplib::Server &, const int, const char *, const char *)>
make_http_server(ConfigManager &config_manager) {
    return [&config_manager](httplib::Server &server, const int port,
                             const char *spotify_callback_url,
                             const char *frontend_url) {
        server.set_default_headers({
            {"Access-Control-Allow-Origin", "*"},
            {"Access-Control-Allow-Methods",
             "GET, POST, PUT, PATCH, DELETE, OPTIONS"},
            {"Access-Control-Allow-Headers", "Content-Type"},
        });
        server.Options(".*", [](const httplib::Request &,
                                httplib::Response &res) { res.status = 204; });

        server.Post("/shutdown", [](const httplib::Request &,
                                    httplib::Response &res) {
            res.status = 204;

            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                std::system("systemctl poweroff -i");
            }).detach();
        });

        server.Get("/configuration", [&config_manager](const httplib::Request &,
                                                       httplib::Response &res) {
            auto config = config_manager.get();
            ConfigurationViewDto view{
                .brightness = config->brightness,
                .mode = config->mode,
                .blackout_window = config->blackout_window,
                .spotify_authorized = config->spotify_authorization.has_value(),
                .spotify_api_credentials =
                    to_view_dto(config->spotify_api_credentials)};
            json j = view;
            res.status = 200;
            res.set_content(j.dump(), "application/json");
        });

        server.Patch("/configuration", [&config_manager](
                                           const httplib::Request &req,
                                           httplib::Response &res) {
            try {
                auto patch = json::parse(req.body).get<ConfigurationPatchDto>();
                if (config_manager.patch(patch)) {
                    auto config = config_manager.get();
                    ConfigurationViewDto view{
                        .brightness = config->brightness,
                        .mode = config->mode,
                        .blackout_window = config->blackout_window,
                        .spotify_authorized =
                            config->spotify_authorization.has_value(),
                        .spotify_api_credentials =
                            to_view_dto(config->spotify_api_credentials)};
                    json j = view;
                    res.status = 200;
                    res.set_content(j.dump(), "application/json");
                } else {
                    res.status = 400;
                }
            } catch (const json::parse_error &e) {
                res.status = 400;
            } catch (const std::exception &e) {
                std::cerr << "PATCH /configuration failed: " << e.what()
                          << std::endl;
                res.status = 500;
            }
        });

        // ------------------------------------------------------------------
        // OAuth: /authorize + /callback (Authorization Code flow)
        //
        // The redirect URI must be registered *exactly* as-is in the
        // Spotify app dashboard. Loopback IP literals are the recommended
        // form for a device that isn't a public web app, so we build it
        // from the port this server is actually listening on.
        // ------------------------------------------------------------------

        const char *state = "PatVJNV8tGz5tKq2c7osQ5YT76io36yS";

        server.Get("/authorize", [&](const httplib::Request &,
                                     httplib::Response &res) {
            auto current_config = config_manager.get();
            if (!current_config->spotify_api_credentials.has_value() ||
                !current_config->spotify_api_credentials->id.has_value() ||
                !current_config->spotify_api_credentials->secret.has_value()) {
                ErrorDto error =
                    ErrorDto{.message = "Spotify-API Logindaten sind nicht "
                                        "vollständig ausgefüllt."};

                json j = error;
                res.status = 409;
                res.set_content(j.dump(), "application/json");
                return;
            }

            std::ostringstream url;
            url << kSpotifyAuthorizeUrl << "?response_type=code"
                << "&client_id=" << *current_config->spotify_api_credentials->id
                << "&scope=" << kSpotifyScopes
                << "&redirect_uri=" << spotify_callback_url
                << "&state=" << state;

            res.status = 302;
            res.set_header("Location", url.str());
        });

        server.Get("/callback", [&](const httplib::Request &req,
                                    httplib::Response &res) {
            auto current_config = config_manager.get();
            std::cout << "=== Spotify OAuth Callback ===" << std::endl;

            if (req.has_param("error")) {
                std::cerr << "Spotify authorization denied: "
                          << req.get_param_value("error") << std::endl;

                res.status = 400;
                res.set_content("Autorisierung abgelehnt: " +
                                    req.get_param_value("error"),
                                "text/plain");
                return;
            }

            if (!req.has_param("code") || !req.has_param("state")) {
                std::cerr << "Missing OAuth callback parameters" << std::endl;

                res.status = 400;
                res.set_content("Fehlende Parameter im Callback.",
                                "text/plain");
                return;
            }

            const std::string code = req.get_param_value("code");
            const std::string state = req.get_param_value("state");

            std::cout << "OAuth state received: " << state << std::endl;

            std::cout << "Authorization code received (length: " << code.size()
                      << ")" << std::endl;

            httplib::Client accounts_client(kSpotifyAccountsHost);

            accounts_client.set_connection_timeout(10);
            accounts_client.set_read_timeout(10);

            if (!current_config->spotify_api_credentials.has_value() ||
                !current_config->spotify_api_credentials->id.has_value() ||
                !current_config->spotify_api_credentials->secret.has_value()) {
                ErrorDto error =
                    ErrorDto{.message = "Spotify-API Logindaten sind nicht "
                                        "vollständig ausgefüllt."};

                json j = error;
                res.status = 409;
                res.set_content(j.dump(), "application/json");
                return;
            }

            const std::string credentials =
                std::string(*current_config->spotify_api_credentials->id) +
                ":" +
                std::string(*current_config->spotify_api_credentials->secret);

            httplib::Headers headers = {
                {
                    "Authorization",
                    "Basic " + base64_encode(credentials),
                },
            };

            const std::string body = "grant_type=authorization_code"
                                     "&code=" +
                                     code +
                                     "&redirect_uri=" + spotify_callback_url;

            std::cout << "Requesting Spotify access token..." << std::endl;

            auto token_res =
                accounts_client.Post("/api/token", headers, body,
                                     "application/x-www-form-urlencoded");

            if (!token_res) {
                std::cerr << "Spotify HTTP error: "
                          << httplib::to_string(token_res.error()) << std::endl;

                res.status = 502;
                res.set_content("Token-Austausch mit Spotify fehlgeschlagen.",
                                "text/plain");
                return;
            }

            std::cout << "Spotify response status: " << token_res->status
                      << std::endl;

            if (token_res->status != 200) {
                std::cerr << "Spotify token exchange failed." << std::endl;

                res.status = 502;
                res.set_content("Token-Austausch mit Spotify fehlgeschlagen.",
                                "text/plain");
                return;
            }

            try {
                const auto tokens = json::parse(token_res->body);

                const std::string access_token = tokens.at("access_token");

                const std::string refresh_token = tokens.at("refresh_token");

                const int expires_in = tokens.at("expires_in");

                const auto expires_at = std::chrono::system_clock::now() +
                                        std::chrono::seconds(expires_in);

                config_manager.patch(SpotifyAuthorization{
                    access_token,
                    refresh_token,
                    expires_at,
                });

                std::cout << "Spotify authorization successful." << std::endl;

                std::cout << "Token expires in " << expires_in << " seconds."
                          << std::endl;

            } catch (const std::exception &e) {
                std::cerr << "Failed to parse Spotify token response: "
                          << e.what() << std::endl;

                res.status = 500;
                res.set_content(
                    "Antwort von Spotify konnte nicht verarbeitet werden.",
                    "text/plain");
                return;
            }

            // Redirect user back to frontend
            res.status = 302;
            res.set_header("Location", frontend_url);
        });

        server.listen("0.0.0.0", port);
    };
}
