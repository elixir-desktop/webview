#include "json_util.hpp"

namespace jsonutil {
namespace {

JsonNode* take_id_copy(JsonNode* id) {
  if (!id) return null_node();
  return json_node_copy(id);
}

JsonNode* empty_object_node() {
  JsonObject* o = object_new();
  JsonNode* n = json_node_alloc();
  json_node_init_object(n, o);
  json_object_unref(o);
  return n;
}

}  // namespace

JsonNode* parse(const std::string& text) {
  JsonParser* parser = json_parser_new();
  GError* err = nullptr;
  if (!json_parser_load_from_data(parser, text.c_str(), static_cast<gssize>(text.size()), &err)) {
    if (err) g_error_free(err);
    g_object_unref(parser);
    return nullptr;
  }
  JsonNode* root = json_parser_steal_root(parser);
  g_object_unref(parser);
  return root;
}

std::string stringify(JsonNode* node) {
  if (!node) return "null";
  JsonGenerator* gen = json_generator_new();
  json_generator_set_root(gen, node);
  gchar* data = json_generator_to_data(gen, nullptr);
  std::string out = data ? data : "null";
  g_free(data);
  g_object_unref(gen);
  return out;
}

std::string stringify_object(JsonObject* obj) {
  JsonNode* n = json_node_alloc();
  json_node_init_object(n, obj);
  auto s = stringify(n);
  json_node_free(n);
  return s;
}

JsonNode* null_node() {
  JsonNode* n = json_node_alloc();
  json_node_init_null(n);
  return n;
}

JsonNode* bool_node(bool v) {
  JsonNode* n = json_node_alloc();
  json_node_init_boolean(n, v);
  return n;
}

JsonNode* int_node(gint64 v) {
  JsonNode* n = json_node_alloc();
  json_node_init_int(n, v);
  return n;
}

JsonNode* double_node(double v) {
  JsonNode* n = json_node_alloc();
  json_node_init_double(n, v);
  return n;
}

JsonNode* string_node(const std::string& s) {
  JsonNode* n = json_node_alloc();
  json_node_init_string(n, s.c_str());
  return n;
}

JsonNode* array_node(const std::vector<JsonNode*>& items) {
  JsonArray* arr = json_array_new();
  for (auto* item : items) {
    json_array_add_element(arr, item);
  }
  JsonNode* n = json_node_alloc();
  json_node_init_array(n, arr);
  json_array_unref(arr);
  return n;
}

JsonObject* object_new() { return json_object_new(); }

bool is_null(JsonNode* n) { return !n || JSON_NODE_HOLDS_NULL(n); }

std::optional<bool> as_bool(JsonNode* n) {
  if (!n || !JSON_NODE_HOLDS_VALUE(n)) return std::nullopt;
  if (json_node_get_value_type(n) != G_TYPE_BOOLEAN) return std::nullopt;
  return json_node_get_boolean(n);
}

std::optional<gint64> as_int(JsonNode* n) {
  if (!n || !JSON_NODE_HOLDS_VALUE(n)) return std::nullopt;
  GType t = json_node_get_value_type(n);
  if (t == G_TYPE_INT64 || t == G_TYPE_INT) return json_node_get_int(n);
  if (t == G_TYPE_DOUBLE) return static_cast<gint64>(json_node_get_double(n));
  return std::nullopt;
}

std::optional<std::string> as_string(JsonNode* n) {
  if (!n || !JSON_NODE_HOLDS_VALUE(n)) return std::nullopt;
  if (json_node_get_value_type(n) != G_TYPE_STRING) return std::nullopt;
  const gchar* s = json_node_get_string(n);
  return s ? std::string(s) : std::string();
}

JsonObject* as_object(JsonNode* n) {
  if (!n || !JSON_NODE_HOLDS_OBJECT(n)) return nullptr;
  return json_node_get_object(n);
}

JsonArray* as_array(JsonNode* n) {
  if (!n || !JSON_NODE_HOLDS_ARRAY(n)) return nullptr;
  return json_node_get_array(n);
}

JsonNode* object_get(JsonObject* obj, const char* key) {
  if (!obj || !json_object_has_member(obj, key)) return nullptr;
  return json_object_get_member(obj, key);
}

std::optional<std::string> object_get_string(JsonObject* obj, const char* key) {
  return as_string(object_get(obj, key));
}

std::optional<bool> object_get_bool(JsonObject* obj, const char* key) {
  return as_bool(object_get(obj, key));
}

std::optional<gint64> object_get_int(JsonObject* obj, const char* key) {
  return as_int(object_get(obj, key));
}

JsonNode* rpc_ok(JsonNode* id, JsonNode* result) {
  JsonObject* obj = object_new();
  json_object_set_string_member(obj, "jsonrpc", "2.0");
  json_object_set_member(obj, "id", take_id_copy(id));
  json_object_set_member(obj, "result", result ? result : null_node());
  JsonNode* n = json_node_alloc();
  json_node_init_object(n, obj);
  json_object_unref(obj);
  return n;
}

JsonNode* rpc_error(JsonNode* id, int code, const std::string& message) {
  JsonObject* err = object_new();
  json_object_set_int_member(err, "code", code);
  json_object_set_string_member(err, "message", message.c_str());
  JsonNode* err_node = json_node_alloc();
  json_node_init_object(err_node, err);
  json_object_unref(err);

  JsonObject* obj = object_new();
  json_object_set_string_member(obj, "jsonrpc", "2.0");
  json_object_set_member(obj, "id", take_id_copy(id));
  json_object_set_member(obj, "error", err_node);

  JsonNode* n = json_node_alloc();
  json_node_init_object(n, obj);
  json_object_unref(obj);
  return n;
}

JsonNode* rpc_notification(const std::string& method, JsonNode* params) {
  JsonObject* obj = object_new();
  json_object_set_string_member(obj, "jsonrpc", "2.0");
  json_object_set_string_member(obj, "method", method.c_str());
  json_object_set_member(obj, "params", params ? params : empty_object_node());
  JsonNode* n = json_node_alloc();
  json_node_init_object(n, obj);
  json_object_unref(obj);
  return n;
}

JsonNode* rpc_request(JsonNode* id, const std::string& method, JsonNode* params) {
  JsonObject* obj = object_new();
  json_object_set_string_member(obj, "jsonrpc", "2.0");
  json_object_set_member(obj, "id", take_id_copy(id));
  json_object_set_string_member(obj, "method", method.c_str());
  json_object_set_member(obj, "params", params ? params : empty_object_node());
  JsonNode* n = json_node_alloc();
  json_node_init_object(n, obj);
  json_object_unref(obj);
  return n;
}

std::string id_key(JsonNode* id) {
  if (!id || JSON_NODE_HOLDS_NULL(id)) return "null";
  if (JSON_NODE_HOLDS_VALUE(id)) {
    if (json_node_get_value_type(id) == G_TYPE_STRING) {
      return std::string("s:") + json_node_get_string(id);
    }
    if (json_node_get_value_type(id) == G_TYPE_INT64 || json_node_get_value_type(id) == G_TYPE_INT) {
      return "n:" + std::to_string(json_node_get_int(id));
    }
    if (json_node_get_value_type(id) == G_TYPE_DOUBLE) {
      return "n:" + std::to_string(static_cast<gint64>(json_node_get_double(id)));
    }
  }
  return "x";
}

}  // namespace jsonutil
