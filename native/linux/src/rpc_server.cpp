#include "rpc_server.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>

struct ReadCtx {
  RpcServer* server;
  GInputStream* stream;
  guint8* buf;
  gsize size;
};

RpcServer::RpcServer() = default;

RpcServer::~RpcServer() {
  close_connection();
  if (service_) {
    g_socket_service_stop(service_);
    if (incoming_handler_id_) g_signal_handler_disconnect(service_, incoming_handler_id_);
    g_object_unref(service_);
    service_ = nullptr;
  }
  if (cancellable_) {
    g_object_unref(cancellable_);
    cancellable_ = nullptr;
  }
}

bool RpcServer::start(const std::string& host, uint16_t port) {
  GError* err = nullptr;
  service_ = g_socket_service_new();

  GSocketFamily family = G_SOCKET_FAMILY_IPV4;
  if (host.find(':') != std::string::npos) family = G_SOCKET_FAMILY_IPV6;

  GSocket* sock = g_socket_new(family, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, &err);
  if (!sock) {
    fprintf(stderr, "edw: socket create failed: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    return false;
  }
  GInetAddress* inet = g_inet_address_new_from_string(host.c_str());
  if (!inet) {
    fprintf(stderr, "edw: invalid host %s\n", host.c_str());
    g_object_unref(sock);
    return false;
  }
  GSocketAddress* addr = G_SOCKET_ADDRESS(g_inet_socket_address_new(inet, port));
  g_object_unref(inet);

  if (!g_socket_bind(sock, addr, TRUE, &err)) {
    fprintf(stderr, "edw: failed to bind %s:%u: %s\n", host.c_str(), port,
            err ? err->message : "unknown");
    if (err) g_error_free(err);
    g_object_unref(addr);
    g_object_unref(sock);
    return false;
  }
  g_object_unref(addr);

  if (!g_socket_listen(sock, &err)) {
    fprintf(stderr, "edw: listen failed: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    g_object_unref(sock);
    return false;
  }

  GSocketAddress* local = g_socket_get_local_address(sock, nullptr);
  if (local && G_IS_INET_SOCKET_ADDRESS(local)) {
    port_ = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(local));
  }
  if (local) g_object_unref(local);

  if (!g_socket_listener_add_socket(G_SOCKET_LISTENER(service_), sock, nullptr, &err)) {
    fprintf(stderr, "edw: add_socket failed: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    g_object_unref(sock);
    return false;
  }
  g_object_unref(sock);

  incoming_handler_id_ = g_signal_connect(
      service_, "incoming",
      G_CALLBACK(+[](GSocketService*, GSocketConnection* conn, GObject*, gpointer user) -> gboolean {
        static_cast<RpcServer*>(user)->accept(conn);
        return TRUE;
      }),
      this);

  g_socket_service_start(service_);
  fprintf(stdout, "listening %u\n", port_);
  fflush(stdout);
  return true;
}

void RpcServer::accept(GSocketConnection* conn) {
  bool replacing = connection_ != nullptr;
  if (connection_) {
    close_connection();
  }
  connection_ = G_SOCKET_CONNECTION(g_object_ref(conn));
  buffer_.clear();
  if (!cancellable_) cancellable_ = g_cancellable_new();
  g_cancellable_reset(cancellable_);

  GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(connection_));
  on_readable(in);
  if (replacing && on_session_end_) on_session_end_();
}

void RpcServer::on_readable(GInputStream* stream) {
  if (!connection_) return;
  auto* ctx = new ReadCtx;
  ctx->server = this;
  ctx->stream = stream;
  ctx->size = 64 * 1024;
  ctx->buf = new guint8[ctx->size];

  g_input_stream_read_async(
      stream, ctx->buf, ctx->size, G_PRIORITY_DEFAULT, cancellable_,
      +[](GObject* source, GAsyncResult* res, gpointer user_data) {
        auto* ctx = static_cast<ReadCtx*>(user_data);
        GError* err = nullptr;
        gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), res, &err);
        if (n > 0) {
          ctx->server->buffer_.insert(ctx->server->buffer_.end(), ctx->buf, ctx->buf + n);
          ctx->server->drain();
          ctx->server->on_readable(ctx->stream);
        } else {
          bool cancelled = err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          if (err) g_error_free(err);
          if (!cancelled && ctx->server->connection_) {
            ctx->server->close_connection();
            if (ctx->server->on_disconnect_) ctx->server->on_disconnect_();
          }
        }
        delete[] ctx->buf;
        delete ctx;
      },
      ctx);
}

void RpcServer::drain() {
  while (buffer_.size() >= 4) {
    uint32_t len = 0;
    memcpy(&len, buffer_.data(), 4);
    len = ntohl(len);
    if (buffer_.size() < 4u + len) return;
    std::string payload(reinterpret_cast<char*>(buffer_.data() + 4), len);
    buffer_.erase(buffer_.begin(), buffer_.begin() + 4 + static_cast<std::ptrdiff_t>(len));
    handle_payload(payload);
  }
}

void RpcServer::handle_payload(const std::string& payload) {
  JsonNode* root = jsonutil::parse(payload);
  if (!root) return;
  JsonObject* obj = jsonutil::as_object(root);
  if (!obj) {
    json_node_free(root);
    return;
  }

  bool has_method = json_object_has_member(obj, "method");
  bool has_id = json_object_has_member(obj, "id");
  bool has_result_or_error =
      json_object_has_member(obj, "result") || json_object_has_member(obj, "error");

  if (has_method && has_id) {
    auto method = jsonutil::object_get_string(obj, "method").value_or("");
    JsonNode* id_copy = json_node_copy(json_object_get_member(obj, "id"));
    JsonNode* params_copy = json_object_has_member(obj, "params")
                                ? json_node_copy(json_object_get_member(obj, "params"))
                                : nullptr;
    json_node_free(root);
    if (on_request_) {
      on_request_(id_copy, method, params_copy, [this](JsonNode* response) { send_node(response); });
    } else {
      json_node_free(id_copy);
      if (params_copy) json_node_free(params_copy);
    }
    return;
  }

  if (has_method && !has_id) {
    json_node_free(root);
    return;
  }

  if (has_result_or_error && has_id) {
    JsonNode* id = json_object_get_member(obj, "id");
    auto key = jsonutil::id_key(id);
    auto it = pending_.find(key);
    if (it != pending_.end()) {
      auto cb = std::move(it->second);
      pending_.erase(it);
      JsonNode* result = json_object_has_member(obj, "result")
                             ? json_node_copy(json_object_get_member(obj, "result"))
                             : nullptr;
      json_node_free(root);
      cb(result);
      if (result) json_node_free(result);
      return;
    }
  }

  json_node_free(root);
}

void RpcServer::send_node(JsonNode* node) {
  if (!node) return;
  auto s = jsonutil::stringify(node);
  json_node_free(node);
  send_raw(s);
}

void RpcServer::notify(const std::string& method, JsonNode* params) {
  send_node(jsonutil::rpc_notification(method, params));
}

void RpcServer::request(const std::string& method, JsonNode* params, PendingCallback cb) {
  int id_num = next_outbound_id_++;
  JsonNode* id = jsonutil::int_node(id_num);
  pending_[jsonutil::id_key(id)] = std::move(cb);
  // rpc_request copies id; free our temporary after
  JsonNode* id_for_req = json_node_copy(id);
  send_node(jsonutil::rpc_request(id_for_req, method, params));
  json_node_free(id_for_req);
  json_node_free(id);
}

void RpcServer::send_raw(const std::string& json) {
  if (!connection_) return;
  uint32_t len = htonl(static_cast<uint32_t>(json.size()));
  GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(connection_));
  GError* err = nullptr;
  gsize written = 0;
  if (!g_output_stream_write_all(out, &len, 4, &written, nullptr, &err) ||
      !g_output_stream_write_all(out, json.data(), json.size(), &written, nullptr, &err)) {
    if (err) {
      fprintf(stderr, "edw: write failed: %s\n", err->message);
      g_error_free(err);
    }
  }
}

void RpcServer::close_connection() {
  if (cancellable_) g_cancellable_cancel(cancellable_);
  if (connection_) {
    g_io_stream_close(G_IO_STREAM(connection_), nullptr, nullptr);
    g_object_unref(connection_);
    connection_ = nullptr;
  }
  buffer_.clear();
}
