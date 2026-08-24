#pragma once

#include "json_util.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <gio/gio.h>

class RpcServer {
 public:
  using ReplyFn = std::function<void(JsonNode* response_node)>;
  using RequestHandler = std::function<void(JsonNode* id, const std::string& method, JsonNode* params, ReplyFn reply)>;
  using DisconnectHandler = std::function<void()>;
  using SessionEndHandler = std::function<void()>;
  using PendingCallback = std::function<void(JsonNode* result)>;

  RpcServer();
  ~RpcServer();

  bool start(const std::string& host, uint16_t port);
  uint16_t port() const { return port_; }

  void set_request_handler(RequestHandler h) { on_request_ = std::move(h); }
  void set_disconnect_handler(DisconnectHandler h) { on_disconnect_ = std::move(h); }
  void set_session_end_handler(SessionEndHandler h) { on_session_end_ = std::move(h); }

  void send_node(JsonNode* node);  // takes ownership
  void notify(const std::string& method, JsonNode* params);  // takes ownership of params
  void request(const std::string& method, JsonNode* params, PendingCallback cb);  // takes params
  void close_connection();

 private:
  void accept(GSocketConnection* conn);
  void on_readable(GInputStream* stream);
  void drain();
  void handle_payload(const std::string& payload);
  void send_raw(const std::string& json);

  GSocketService* service_ = nullptr;
  GSocketConnection* connection_ = nullptr;
  GCancellable* cancellable_ = nullptr;
  std::vector<uint8_t> buffer_;
  uint16_t port_ = 0;
  int next_outbound_id_ = 1;
  std::map<std::string, PendingCallback> pending_;
  RequestHandler on_request_;
  DisconnectHandler on_disconnect_;
  SessionEndHandler on_session_end_;
  gulong incoming_handler_id_ = 0;
};
