#include "error.hpp"

void to_json(json &j, const ErrorDto &e) { j = json{{"message", e.message}}; }
