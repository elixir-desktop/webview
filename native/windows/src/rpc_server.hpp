#pragma once

#include "win_prefix.hpp"

#include "json_util.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

class RpcServer {
 public:
  using ReplyFn = std::function<void(jsonutil::Json response)>;
  using RequestHandler =
      std::function<void(jsonutil::Json id, const std::string& method, jsonutil::Json params,
                         ReplyFn reply)>;
  using DisconnectHandler = std::function<void()>;
  using PendingCallback = std::function<void(jsonutil::Json result)>;

  RpcServer();
  ~RpcServer();

  bool start(const std::string& host, uint16_t port, HWND notify_hwnd);
  uint16_t port() const { return port_; }

  void set_request_handler(RequestHandler h) { on_request_ = std::move(h); }
  void set_disconnect_handler(DisconnectHandler h) { on_disconnect_ = std::move(h); }

  void on_socket_event(SOCKET s, int event, int error);
  void send_json(jsonutil::Json node);
  void notify(const std::string& method, jsonutil::Json params);
  void request(const std::string& method, jsonutil::Json params, PendingCallback cb);
  void close_connection();

  static constexpr UINT WM_EDW_SOCKET = WM_APP + 1;

 private:
  void accept_client();
  void on_readable();
  void drain();
  void handle_payload(const std::string& payload);
  void send_raw(const std::string& json);

  HWND notify_hwnd_ = nullptr;
  SOCKET listen_sock_ = INVALID_SOCKET;
  SOCKET client_sock_ = INVALID_SOCKET;
  std::vector<uint8_t> buffer_;
  uint16_t port_ = 0;
  int next_outbound_id_ = 1;
  std::map<std::string, PendingCallback> pending_;
  RequestHandler on_request_;
  DisconnectHandler on_disconnect_;
  bool wsa_started_ = false;
};
