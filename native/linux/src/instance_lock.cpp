#include "instance_lock.hpp"

#include "json_util.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <glib.h>

namespace {

constexpr int kTimeoutMs = 15000;

std::string tmp_dir() {
  const char* tmp = std::getenv("TMPDIR");
  if (tmp && tmp[0]) return tmp;
  return "/tmp";
}

bool write_all(int fd, const void* data, size_t n) {
  const char* p = static_cast<const char*>(data);
  size_t left = n;
  while (left) {
    ssize_t w = write(fd, p, left);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (w == 0) return false;
    p += w;
    left -= static_cast<size_t>(w);
  }
  return true;
}

bool read_all_timeout(int fd, void* data, size_t n, int timeout_ms) {
  char* p = static_cast<char*>(data);
  size_t left = n;
  while (left) {
    pollfd pfd{fd, POLLIN, 0};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return false;
    ssize_t r = read(fd, p, left);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;
    p += r;
    left -= static_cast<size_t>(r);
  }
  return true;
}

bool write_frame(int fd, const std::string& json) {
  uint32_t len = htonl(static_cast<uint32_t>(json.size()));
  return write_all(fd, &len, 4) && write_all(fd, json.data(), json.size());
}

bool read_frame(int fd, std::string* out, int timeout_ms) {
  uint32_t nlen = 0;
  if (!read_all_timeout(fd, &nlen, 4, timeout_ms)) return false;
  uint32_t len = ntohl(nlen);
  if (len == 0 || len > 8 * 1024 * 1024) return false;
  out->assign(len, '\0');
  return read_all_timeout(fd, out->data(), len, timeout_ms);
}

bool can_connect(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  close(fd);
  return rc == 0;
}

int connect_path(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  timeval tv{};
  tv.tv_sec = kTimeoutMs / 1000;
  tv.tv_usec = (kTimeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

JsonNode* make_request(const std::string& method, JsonNode* params) {
  JsonNode* id = jsonutil::int_node(1);
  JsonNode* req = jsonutil::rpc_request(id, method, params);
  json_node_free(id);
  return req;
}

int rpc_call(const std::string& instance_id, const std::string& method, JsonNode* params,
             JsonNode** out_result, std::string* err_out) {
  int fd = connect_path(InstanceLock::socket_path(instance_id));
  if (fd < 0) {
    if (params) json_node_free(params);
    if (err_out) *err_out = "no running single-instance host";
    return 1;
  }
  JsonNode* req = make_request(method, params);
  std::string json = jsonutil::stringify(req);
  json_node_free(req);
  if (!write_frame(fd, json)) {
    close(fd);
    if (err_out) *err_out = "control socket write failed";
    return 1;
  }
  std::string payload;
  if (!read_frame(fd, &payload, kTimeoutMs)) {
    close(fd);
    if (err_out) *err_out = "control socket read failed";
    return 1;
  }
  close(fd);
  JsonNode* root = jsonutil::parse(payload);
  if (!root) {
    if (err_out) *err_out = "control socket parse failed";
    return 1;
  }
  JsonObject* obj = jsonutil::as_object(root);
  if (obj && json_object_has_member(obj, "error")) {
    JsonObject* err = jsonutil::as_object(json_object_get_member(obj, "error"));
    std::string msg = "instance request failed";
    if (auto m = jsonutil::object_get_string(err, "message")) msg = *m;
    json_node_free(root);
    if (err_out) *err_out = msg;
    return 1;
  }
  if (out_result && obj && json_object_has_member(obj, "result")) {
    *out_result = json_node_copy(json_object_get_member(obj, "result"));
  }
  json_node_free(root);
  return 0;
}

void run_on_main_sync(const std::function<void()>& fn) {
  if (g_main_context_is_owner(g_main_context_default())) {
    fn();
    return;
  }
  struct Wait {
    std::function<void()> fn;
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
  };
  auto w = std::make_shared<Wait>();
  w->fn = fn;
  auto* raw = new std::shared_ptr<Wait>(w);
  g_idle_add(
      +[](gpointer data) -> gboolean {
        auto* pw = static_cast<std::shared_ptr<Wait>*>(data);
        auto w = *pw;
        delete pw;
        w->fn();
        {
          std::lock_guard<std::mutex> lock(w->mu);
          w->done = true;
        }
        w->cv.notify_one();
        return G_SOURCE_REMOVE;
      },
      raw);
  std::unique_lock<std::mutex> lock(w->mu);
  w->cv.wait_for(lock, std::chrono::milliseconds(kTimeoutMs), [&] { return w->done; });
}

}  // namespace

InstanceLock::~InstanceLock() {
  running_ = false;
  if (listen_fd_ >= 0) {
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    listen_fd_ = -1;
  }
  if (thread_.joinable()) thread_.join();
  if (!path_.empty()) unlink(path_.c_str());
}

std::string InstanceLock::sanitize_id(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char c : raw) {
    if (std::isalnum(c) || c == '.' || c == '_' || c == '-')
      out.push_back(static_cast<char>(c));
    else
      out.push_back('_');
  }
  if (out.size() > 64) out.resize(64);
  return out.empty() ? "DesktopWebView" : out;
}

std::string InstanceLock::socket_path(const std::string& instance_id) {
  return tmp_dir() + "/edw-" + std::to_string(static_cast<long>(getuid())) + "-" +
         sanitize_id(instance_id) + ".sock";
}

bool InstanceLock::try_serve(const std::string& instance_id) {
  path_ = socket_path(instance_id);
  if (can_connect(path_)) return false;
  unlink(path_.c_str());

  listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return false;

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path_.c_str());
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (listen(listen_fd_, 8) != 0) {
    close(listen_fd_);
    listen_fd_ = -1;
    unlink(path_.c_str());
    return false;
  }
  return true;
}

void InstanceLock::set_handlers(ActivateFn activate, EvalFn eval) {
  activate_ = std::move(activate);
  eval_ = std::move(eval);
}

void InstanceLock::start() {
  if (listen_fd_ < 0 || running_) return;
  running_ = true;
  thread_ = std::thread([this] { accept_loop(); });
}

void InstanceLock::accept_loop() {
  while (running_) {
    int fd = accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (!running_) break;
      if (errno == EINTR) continue;
      break;
    }
    handle_client(fd);
    close(fd);
  }
}

void InstanceLock::handle_client(int fd) {
  std::string payload;
  if (!read_frame(fd, &payload, kTimeoutMs)) return;
  JsonNode* root = jsonutil::parse(payload);
  if (!root) return;
  JsonObject* obj = jsonutil::as_object(root);
  if (!obj) {
    json_node_free(root);
    return;
  }
  auto method = jsonutil::object_get_string(obj, "method").value_or("");
  JsonNode* id = json_object_has_member(obj, "id")
                     ? json_node_copy(json_object_get_member(obj, "id"))
                     : jsonutil::int_node(1);
  JsonObject* params = jsonutil::as_object(json_object_get_member(obj, "params"));

  if (method == "instance.activate") {
    std::vector<std::string> argv;
    if (params) {
      JsonNode* arrn = json_object_get_member(params, "argv");
      JsonArray* arr = jsonutil::as_array(arrn);
      if (arr) {
        guint n = json_array_get_length(arr);
        for (guint i = 0; i < n; i++) {
          if (auto s = jsonutil::as_string(json_array_get_element(arr, i))) argv.push_back(*s);
        }
      }
    }
    json_node_free(root);
    if (activate_) {
      run_on_main_sync([this, argv] { activate_(argv); });
    }
    JsonNode* resp = jsonutil::rpc_ok(id, jsonutil::bool_node(true));
    write_frame(fd, jsonutil::stringify(resp));
    json_node_free(resp);
    json_node_free(id);
    return;
  }

  if (method == "instance.eval") {
    std::string expr;
    if (auto e = jsonutil::object_get_string(params, "expr")) expr = *e;
    json_node_free(root);
    if (expr.empty()) {
      JsonNode* resp = jsonutil::rpc_error(id, -32602, "expr required");
      write_frame(fd, jsonutil::stringify(resp));
      json_node_free(resp);
      json_node_free(id);
      return;
    }
    if (!eval_) {
      JsonNode* resp = jsonutil::rpc_error(id, -32000, "no initialized Elixir client");
      write_frame(fd, jsonutil::stringify(resp));
      json_node_free(resp);
      json_node_free(id);
      return;
    }

    struct EvalWait {
      bool ok = false;
      std::string text;
      std::mutex mu;
      std::condition_variable cv;
      bool done = false;
    };
    auto wait = std::make_shared<EvalWait>();

    run_on_main_sync([this, expr, wait] {
      eval_(expr, [wait](bool ok, std::string text) {
        {
          std::lock_guard<std::mutex> lock(wait->mu);
          wait->ok = ok;
          wait->text = std::move(text);
          wait->done = true;
        }
        wait->cv.notify_one();
      });
    });

    {
      std::unique_lock<std::mutex> lock(wait->mu);
      if (!wait->cv.wait_for(lock, std::chrono::milliseconds(kTimeoutMs),
                             [&] { return wait->done; })) {
        JsonNode* resp = jsonutil::rpc_error(id, -32000, "rpc.eval timed out");
        write_frame(fd, jsonutil::stringify(resp));
        json_node_free(resp);
        json_node_free(id);
        return;
      }
    }

    if (!wait->ok) {
      JsonNode* resp =
          jsonutil::rpc_error(id, -32000, wait->text.empty() ? "rpc.eval failed" : wait->text);
      write_frame(fd, jsonutil::stringify(resp));
      json_node_free(resp);
      json_node_free(id);
      return;
    }
    JsonObject* ro = jsonutil::object_new();
    json_object_set_string_member(ro, "inspect", wait->text.c_str());
    JsonNode* result = json_node_alloc();
    json_node_init_object(result, ro);
    json_object_unref(ro);
    JsonNode* resp = jsonutil::rpc_ok(id, result);
    write_frame(fd, jsonutil::stringify(resp));
    json_node_free(resp);
    json_node_free(id);
    return;
  }

  json_node_free(root);
  JsonNode* resp = jsonutil::rpc_error(id, -32601, "Method not found");
  write_frame(fd, jsonutil::stringify(resp));
  json_node_free(resp);
  json_node_free(id);
}

int InstanceLock::client_activate(const std::string& instance_id,
                                  const std::vector<std::string>& argv) {
  JsonArray* arr = json_array_new();
  for (auto& a : argv) json_array_add_string_element(arr, a.c_str());
  JsonObject* params = jsonutil::object_new();
  json_object_set_array_member(params, "argv", arr);
  JsonNode* pn = json_node_alloc();
  json_node_init_object(pn, params);
  json_object_unref(params);
  std::string err;
  int code = rpc_call(instance_id, "instance.activate", pn, nullptr, &err);
  if (code != 0) {
    fprintf(stderr, "edw: instance.activate failed: %s\n", err.c_str());
  }
  return code;
}

int InstanceLock::client_eval(const std::string& instance_id, const std::string& expr,
                             std::string* inspect_out) {
  JsonObject* params = jsonutil::object_new();
  json_object_set_string_member(params, "expr", expr.c_str());
  JsonNode* pn = json_node_alloc();
  json_node_init_object(pn, params);
  json_object_unref(params);
  JsonNode* result = nullptr;
  std::string err;
  int code = rpc_call(instance_id, "instance.eval", pn, &result, &err);
  if (code != 0) {
    fprintf(stderr, "edw: instance.eval failed: %s\n", err.c_str());
    if (result) json_node_free(result);
    return code;
  }
  JsonObject* obj = jsonutil::as_object(result);
  auto inspect = jsonutil::object_get_string(obj, "inspect");
  if (!inspect) {
    fprintf(stderr, "edw: instance.eval missing inspect\n");
    if (result) json_node_free(result);
    return 1;
  }
  if (inspect_out) *inspect_out = *inspect;
  if (result) json_node_free(result);
  return 0;
}
