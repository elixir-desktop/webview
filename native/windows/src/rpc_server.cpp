#include "rpc_server.hpp"

#include <cstdio>
#include <cstring>

RpcServer::RpcServer() = default;

RpcServer::~RpcServer() {
  close_connection();
  if (listen_sock_ != INVALID_SOCKET) {
    closesocket(listen_sock_);
    listen_sock_ = INVALID_SOCKET;
  }
  if (wsa_started_) {
    WSACleanup();
    wsa_started_ = false;
  }
}

bool RpcServer::start(const std::string& host, uint16_t port, HWND notify_hwnd) {
  notify_hwnd_ = notify_hwnd;
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    fprintf(stderr, "edw: WSAStartup failed\n");
    return false;
  }
  wsa_started_ = true;

  int family = AF_INET;
  if (host.find(':') != std::string::npos) family = AF_INET6;

  listen_sock_ = socket(family, SOCK_STREAM, IPPROTO_TCP);
  if (listen_sock_ == INVALID_SOCKET) {
    fprintf(stderr, "edw: socket create failed\n");
    return false;
  }

  BOOL reuse = TRUE;
  setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));

  if (family == AF_INET) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
      fprintf(stderr, "edw: invalid host %s\n", host.c_str());
      return false;
    }
    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      fprintf(stderr, "edw: failed to bind %s:%u\n", host.c_str(), port);
      return false;
    }
  } else {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    if (inet_pton(AF_INET6, host.c_str(), &addr.sin6_addr) != 1) {
      fprintf(stderr, "edw: invalid host %s\n", host.c_str());
      return false;
    }
    if (bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      fprintf(stderr, "edw: failed to bind %s:%u\n", host.c_str(), port);
      return false;
    }
  }

  if (listen(listen_sock_, 4) != 0) {
    fprintf(stderr, "edw: listen failed\n");
    return false;
  }

  sockaddr_storage local{};
  int local_len = sizeof(local);
  if (getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
    if (local.ss_family == AF_INET) {
      port_ = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
    } else if (local.ss_family == AF_INET6) {
      port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
    }
  }

  if (WSAAsyncSelect(listen_sock_, notify_hwnd_, WM_EDW_SOCKET, FD_ACCEPT) != 0) {
    fprintf(stderr, "edw: WSAAsyncSelect listen failed\n");
    return false;
  }

  fprintf(stdout, "listening %u\n", port_);
  fflush(stdout);
  return true;
}

void RpcServer::on_socket_event(SOCKET s, int event, int error) {
  if (error != 0) {
    if (s == client_sock_) {
      close_connection();
      if (on_disconnect_) on_disconnect_();
    }
    return;
  }
  if (s == listen_sock_ && (event & FD_ACCEPT)) {
    accept_client();
    return;
  }
  if (s == client_sock_) {
    if (event & FD_READ) on_readable();
    if (event & FD_CLOSE) {
      close_connection();
      if (on_disconnect_) on_disconnect_();
    }
  }
}

void RpcServer::accept_client() {
  SOCKET s = accept(listen_sock_, nullptr, nullptr);
  if (s == INVALID_SOCKET) return;
  if (client_sock_ != INVALID_SOCKET) {
    close_connection();
  }
  client_sock_ = s;
  buffer_.clear();
  WSAAsyncSelect(client_sock_, notify_hwnd_, WM_EDW_SOCKET, FD_READ | FD_CLOSE);
}

void RpcServer::on_readable() {
  if (client_sock_ == INVALID_SOCKET) return;
  uint8_t buf[64 * 1024];
  for (;;) {
    int n = recv(client_sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
    if (n > 0) {
      buffer_.insert(buffer_.end(), buf, buf + n);
      continue;
    }
    if (n == 0) {
      close_connection();
      if (on_disconnect_) on_disconnect_();
      return;
    }
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) break;
    close_connection();
    if (on_disconnect_) on_disconnect_();
    return;
  }
  drain();
}

void RpcServer::drain() {
  constexpr uint32_t kMaxFrame = 16u * 1024u * 1024u;
  size_t off = 0;
  while (buffer_.size() - off >= 4) {
    uint32_t len = 0;
    memcpy(&len, buffer_.data() + off, 4);
    len = ntohl(len);
    if (len > kMaxFrame) {
      fprintf(stderr, "edw: frame too large (%u)\n", len);
      close_connection();
      if (on_disconnect_) on_disconnect_();
      return;
    }
    if (buffer_.size() - off < 4u + len) break;
    std::string payload(reinterpret_cast<char*>(buffer_.data() + off + 4), len);
    off += 4 + len;
    handle_payload(payload);
  }
  if (off > 0) buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(off));
}

void RpcServer::handle_payload(const std::string& payload) {
  auto root = jsonutil::parse(payload);
  if (root.is_null() || !root.is_object()) return;

  bool has_method = root.contains("method");
  bool has_id = root.contains("id");
  bool has_result_or_error = root.contains("result") || root.contains("error");

  if (has_method && has_id) {
    auto method = jsonutil::get_string(root, "method").value_or("");
    if (method.empty()) return;
    auto id = root["id"];
    jsonutil::Json params = root.contains("params") ? root["params"] : jsonutil::Json();
    if (on_request_) {
      on_request_(id, method, params, [this](jsonutil::Json response) { send_json(std::move(response)); });
    }
    return;
  }

  if (has_method && !has_id) return;

  if (has_result_or_error && has_id) {
    auto key = jsonutil::id_key(root["id"]);
    auto it = pending_.find(key);
    if (it != pending_.end()) {
      auto cb = std::move(it->second);
      pending_.erase(it);
      jsonutil::Json result =
          root.contains("result") ? root["result"] : jsonutil::Json(nullptr);
      cb(result);
    }
  }
}

void RpcServer::send_json(jsonutil::Json node) {
  send_raw(jsonutil::stringify(node));
}

void RpcServer::notify(const std::string& method, jsonutil::Json params) {
  send_json(jsonutil::rpc_notification(method, std::move(params)));
}

void RpcServer::request(const std::string& method, jsonutil::Json params, PendingCallback cb) {
  int id_num = next_outbound_id_++;
  jsonutil::Json id = id_num;
  pending_[jsonutil::id_key(id)] = std::move(cb);
  send_json(jsonutil::rpc_request(id, method, std::move(params)));
}

void RpcServer::send_raw(const std::string& json) {
  if (client_sock_ == INVALID_SOCKET) return;
  uint32_t len = htonl(static_cast<uint32_t>(json.size()));
  auto send_all = [this](const char* data, int size) {
    int sent = 0;
    while (sent < size) {
      int n = send(client_sock_, data + sent, size - sent, 0);
      if (n <= 0) return false;
      sent += n;
    }
    return true;
  };
  if (!send_all(reinterpret_cast<const char*>(&len), 4) ||
      !send_all(json.data(), static_cast<int>(json.size()))) {
    fprintf(stderr, "edw: write failed\n");
  }
}

void RpcServer::close_connection() {
  if (client_sock_ != INVALID_SOCKET) {
    closesocket(client_sock_);
    client_sock_ = INVALID_SOCKET;
  }
  buffer_.clear();
  auto pending = std::move(pending_);
  pending_.clear();
  for (auto& [_, cb] : pending) {
    if (cb) cb(nullptr);
  }
}
