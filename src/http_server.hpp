#pragma once
#include "config_manager.hpp"
#include <functional>
#include <httplib.h>

std::function<void(httplib::Server &server, const int port,
                   const char *spotify_callback_url, const char *frontend_url)>
make_http_server(ConfigManager &config_manager);
