#pragma once
#include "config_manager.hpp"
#include <functional>
#include <httplib.h>

std::function<void(httplib::Server &server, const int port,
                   const char *spotify_callback_url, const char *frontend_url)>
make_http_server(const char* app_version, ConfigManager &config_manager);
