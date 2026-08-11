#include "json_util.hpp"

namespace jsonutil {

Json parse(const std::string& text) {
  try {
    return Json::parse(text);
  } catch (...) {
    return nullptr;
  }
}

std::string stringify(const Json& node) { return node.dump(); }

Json rpc_ok(const Json& id, Json result) {
  return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json rpc_error(const Json& id, int code, const std::string& message) {
  return Json{{"jsonrpc", "2.0"},
              {"id", id},
              {"error", {{"code", code}, {"message", message}}}};
}

Json rpc_notification(const std::string& method, Json params) {
  if (params.is_null()) params = Json::object();
  return Json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

Json rpc_request(const Json& id, const std::string& method, Json params) {
  if (params.is_null()) params = Json::object();
  return Json{
      {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

std::string id_key(const Json& id) {
  if (id.is_null()) return "null";
  if (id.is_string()) return std::string("s:") + id.get<std::string>();
  if (id.is_number_integer()) return "n:" + std::to_string(id.get<int64_t>());
  if (id.is_number_float()) return "n:" + std::to_string(static_cast<int64_t>(id.get<double>()));
  return "x";
}

std::optional<std::string> get_string(const Json& obj, const char* key) {
  if (!obj.is_object() || !obj.contains(key) || !obj[key].is_string()) return std::nullopt;
  return obj[key].get<std::string>();
}

std::optional<bool> get_bool(const Json& obj, const char* key) {
  if (!obj.is_object() || !obj.contains(key) || !obj[key].is_boolean()) return std::nullopt;
  return obj[key].get<bool>();
}

std::optional<int64_t> get_int(const Json& obj, const char* key) {
  if (!obj.is_object() || !obj.contains(key)) return std::nullopt;
  const auto& v = obj[key];
  if (v.is_number_integer()) return v.get<int64_t>();
  if (v.is_number_float()) return static_cast<int64_t>(v.get<double>());
  return std::nullopt;
}

const Json* get(const Json& obj, const char* key) {
  if (!obj.is_object() || !obj.contains(key)) return nullptr;
  return &obj[key];
}

}  // namespace jsonutil
