#include "instance_lock.hpp"

#include "json_util.hpp"
#include "win_util.hpp"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace {

constexpr DWORD kTimeoutMs = 15000;

uint32_t to_be32(uint32_t x) {
  return ((x & 0xffu) << 24) | ((x & 0xff00u) << 8) | ((x & 0xff0000u) >> 8) | (x >> 24);
}

uint32_t from_be32(uint32_t x) { return to_be32(x); }

std::string windows_uid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return "0";
  DWORD len = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &len);
  if (len == 0) {
    CloseHandle(token);
    return "0";
  }
  std::vector<uint8_t> buf(len);
  DWORD rid = 0;
  if (GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
    auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
    PUCHAR count = GetSidSubAuthorityCount(tu->User.Sid);
    if (count && *count > 0) {
      rid = *GetSidSubAuthority(tu->User.Sid, static_cast<DWORD>(*count - 1));
    }
  }
  CloseHandle(token);
  return std::to_string(rid);
}

bool write_all(HANDLE h, const void* data, size_t n) {
  const char* p = static_cast<const char*>(data);
  size_t left = n;
  while (left) {
    DWORD w = 0;
    if (!WriteFile(h, p, static_cast<DWORD>(left), &w, nullptr) || w == 0) return false;
    p += w;
    left -= w;
  }
  return true;
}

bool read_all(HANDLE h, void* data, size_t n) {
  char* p = static_cast<char*>(data);
  size_t left = n;
  while (left) {
    DWORD r = 0;
    if (!ReadFile(h, p, static_cast<DWORD>(left), &r, nullptr) || r == 0) return false;
    p += r;
    left -= r;
  }
  return true;
}

bool write_frame(HANDLE h, const std::string& json) {
  uint32_t len = to_be32(static_cast<uint32_t>(json.size()));
  return write_all(h, &len, 4) && write_all(h, json.data(), json.size());
}

bool read_frame(HANDLE h, std::string* out) {
  uint32_t nlen = 0;
  if (!read_all(h, &nlen, 4)) return false;
  uint32_t len = from_be32(nlen);
  if (len == 0 || len > 8 * 1024 * 1024) return false;
  out->assign(len, '\0');
  return read_all(h, out->data(), len);
}

HANDLE connect_pipe(const std::wstring& name) {
  DWORD start = GetTickCount();
  while (GetTickCount() - start < kTimeoutMs) {
    HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (h != INVALID_HANDLE_VALUE) return h;
    if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND) return INVALID_HANDLE_VALUE;
    WaitNamedPipeW(name.c_str(), 200);
  }
  return INVALID_HANDLE_VALUE;
}

int rpc_call(const std::string& instance_id, const std::string& method, jsonutil::Json params,
             jsonutil::Json* out_result, std::string* err_out) {
  HANDLE h = connect_pipe(InstanceLock::pipe_name(instance_id));
  if (h == INVALID_HANDLE_VALUE) {
    if (err_out) *err_out = "no running single-instance host";
    return 1;
  }
  jsonutil::Json req = jsonutil::rpc_request(1, method, std::move(params));
  std::string json = jsonutil::stringify(req);
  if (!write_frame(h, json)) {
    CloseHandle(h);
    if (err_out) *err_out = "control pipe write failed";
    return 1;
  }
  std::string payload;
  if (!read_frame(h, &payload)) {
    CloseHandle(h);
    if (err_out) *err_out = "control pipe read failed";
    return 1;
  }
  CloseHandle(h);
  auto root = jsonutil::parse(payload);
  if (!root.is_object()) {
    if (err_out) *err_out = "control pipe parse failed";
    return 1;
  }
  if (root.contains("error")) {
    std::string msg = "instance request failed";
    if (root["error"].is_object()) {
      if (auto m = jsonutil::get_string(root["error"], "message")) msg = *m;
    }
    if (err_out) *err_out = msg;
    return 1;
  }
  if (out_result && root.contains("result")) *out_result = root["result"];
  return 0;
}

}  // namespace

InstanceLock::~InstanceLock() {
  running_ = false;
  if (stop_event_) SetEvent(stop_event_);
  if (!instance_id_.empty()) {
    HANDLE wake = CreateFileW(pipe_name(instance_id_).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
  }
  if (thread_.joinable()) thread_.join();
  if (mutex_) {
    ReleaseMutex(mutex_);
    CloseHandle(mutex_);
    mutex_ = nullptr;
  }
  if (stop_event_) {
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }
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

std::wstring InstanceLock::mutex_name(const std::string& instance_id) {
  return utf8_to_wide("Local\\edw-" + sanitize_id(instance_id));
}

std::wstring InstanceLock::pipe_name(const std::string& instance_id) {
  return utf8_to_wide("\\\\.\\pipe\\edw-" + windows_uid() + "-" + sanitize_id(instance_id));
}

bool InstanceLock::try_serve(const std::string& instance_id) {
  instance_id_ = instance_id;
  mutex_ = CreateMutexW(nullptr, TRUE, mutex_name(instance_id).c_str());
  if (!mutex_) return false;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex_);
    mutex_ = nullptr;
    return false;
  }
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  return true;
}

void InstanceLock::set_handlers(ActivateFn activate, EvalFn eval) {
  activate_ = std::move(activate);
  eval_ = std::move(eval);
}

void InstanceLock::start() {
  if (!mutex_ || running_) return;
  running_ = true;
  thread_ = std::thread([this] { accept_loop(); });
}

void InstanceLock::accept_loop() {
  auto name = pipe_name(instance_id_);
  while (running_) {
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 1000, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (!running_) break;
      Sleep(50);
      continue;
    }
    BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected || !running_) {
      CloseHandle(pipe);
      break;
    }
    handle_client(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

void InstanceLock::handle_client(HANDLE pipe) {
  std::string payload;
  if (!read_frame(pipe, &payload)) return;
  auto root = jsonutil::parse(payload);
  if (!root.is_object()) return;
  auto method = jsonutil::get_string(root, "method").value_or("");
  jsonutil::Json id = root.contains("id") ? root["id"] : jsonutil::Json(1);
  jsonutil::Json params = root.contains("params") ? root["params"] : jsonutil::Json::object();

  if (method == "instance.activate") {
    std::vector<std::string> argv;
    if (params.contains("argv") && params["argv"].is_array()) {
      for (auto& v : params["argv"]) {
        if (v.is_string()) argv.push_back(v.get<std::string>());
      }
    }
    if (activate_) activate_(argv);
    write_frame(pipe, jsonutil::stringify(jsonutil::rpc_ok(id, true)));
    return;
  }

  if (method == "instance.eval") {
    auto expr = jsonutil::get_string(params, "expr").value_or("");
    if (expr.empty()) {
      write_frame(pipe, jsonutil::stringify(jsonutil::rpc_error(id, -32602, "expr required")));
      return;
    }
    if (!eval_) {
      write_frame(pipe, jsonutil::stringify(jsonutil::rpc_error(id, -32000, "no initialized Elixir client")));
      return;
    }
    struct EvalWait {
      bool ok = false;
      std::string text;
      std::mutex mu;
      std::condition_variable cv;
      bool done = false;
    };
    auto ew = std::make_shared<EvalWait>();
    eval_(expr, [ew](bool ok, std::string text) {
      {
        std::lock_guard<std::mutex> lock(ew->mu);
        ew->ok = ok;
        ew->text = std::move(text);
        ew->done = true;
      }
      ew->cv.notify_one();
    });
    {
      std::unique_lock<std::mutex> lock(ew->mu);
      if (!ew->cv.wait_for(lock, std::chrono::milliseconds(kTimeoutMs), [&] { return ew->done; })) {
        write_frame(pipe, jsonutil::stringify(jsonutil::rpc_error(id, -32000, "rpc.eval timed out")));
        return;
      }
    }
    if (!ew->ok) {
      write_frame(pipe, jsonutil::stringify(
                            jsonutil::rpc_error(id, -32000, ew->text.empty() ? "rpc.eval failed" : ew->text)));
      return;
    }
    write_frame(pipe, jsonutil::stringify(jsonutil::rpc_ok(id, jsonutil::Json{{"inspect", ew->text}})));
    return;
  }

  write_frame(pipe, jsonutil::stringify(jsonutil::rpc_error(id, -32601, "Method not found")));
}

int InstanceLock::client_activate(const std::string& instance_id,
                                  const std::vector<std::string>& argv) {
  jsonutil::Json arr = jsonutil::Json::array();
  for (auto& a : argv) arr.push_back(a);
  std::string err;
  int code = rpc_call(instance_id, "instance.activate", jsonutil::Json{{"argv", arr}}, nullptr, &err);
  if (code != 0) fprintf(stderr, "edw: instance.activate failed: %s\n", err.c_str());
  return code;
}

int InstanceLock::client_eval(const std::string& instance_id, const std::string& expr,
                             std::string* inspect_out) {
  jsonutil::Json result;
  std::string err;
  int code = rpc_call(instance_id, "instance.eval", jsonutil::Json{{"expr", expr}}, &result, &err);
  if (code != 0) {
    fprintf(stderr, "edw: instance.eval failed: %s\n", err.c_str());
    return code;
  }
  auto inspect = jsonutil::get_string(result, "inspect");
  if (!inspect) {
    fprintf(stderr, "edw: instance.eval missing inspect\n");
    return 1;
  }
  if (inspect_out) *inspect_out = *inspect;
  return 0;
}
