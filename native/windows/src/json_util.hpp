#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace jsonutil {

using Json = nlohmann::json;

Json parse(const std::string& text);
std::string stringify(const Json& node);

Json rpc_ok(const Json& id, Json result);
Json rpc_error(const Json& id, int code, const std::string& message);
Json rpc_notification(const std::string& method, Json params);
Json rpc_request(const Json& id, const std::string& method, Json params);

std::string id_key(const Json& id);

std::optional<std::string> get_string(const Json& obj, const char* key);
std::optional<bool> get_bool(const Json& obj, const char* key);
std::optional<int64_t> get_int(const Json& obj, const char* key);
const Json* get(const Json& obj, const char* key);

}  // namespace jsonutil
