#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

struct ErrorDto {
    std::string message;
};

void to_json(json &j, const ErrorDto &e);
