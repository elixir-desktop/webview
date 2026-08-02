#pragma once

#include <json-glib/json-glib.h>

#include <optional>
#include <string>
#include <vector>

namespace jsonutil {

JsonNode* parse(const std::string& text);
std::string stringify(JsonNode* node);
std::string stringify_object(JsonObject* obj);

JsonNode* null_node();
JsonNode* bool_node(bool v);
JsonNode* int_node(gint64 v);
JsonNode* double_node(double v);
JsonNode* string_node(const std::string& s);
JsonNode* array_node(const std::vector<JsonNode*>& items);
JsonObject* object_new();

bool is_null(JsonNode* n);
std::optional<bool> as_bool(JsonNode* n);
std::optional<gint64> as_int(JsonNode* n);
std::optional<std::string> as_string(JsonNode* n);
JsonObject* as_object(JsonNode* n);
JsonArray* as_array(JsonNode* n);

JsonNode* object_get(JsonObject* obj, const char* key);
std::optional<std::string> object_get_string(JsonObject* obj, const char* key);
std::optional<bool> object_get_bool(JsonObject* obj, const char* key);
std::optional<gint64> object_get_int(JsonObject* obj, const char* key);

// Steal ownership of node into a response envelope; caller owns returned node.
JsonNode* rpc_ok(JsonNode* id, JsonNode* result);
JsonNode* rpc_error(JsonNode* id, int code, const std::string& message);
JsonNode* rpc_notification(const std::string& method, JsonNode* params);
JsonNode* rpc_request(JsonNode* id, const std::string& method, JsonNode* params);

std::string id_key(JsonNode* id);

}  // namespace jsonutil
