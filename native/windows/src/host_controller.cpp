#include "host_controller.hpp"
#include "win_util.hpp"

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sstream>
#include <vector>

namespace {

constexpr const wchar_t* kHostClass = L"DesktopWebViewHost";

struct RequestMsg {
  HostController* self;
  jsonutil::Json id;
  std::string method;
  jsonutil::Json params;
  RpcServer::ReplyFn reply;
};

}  // namespace

HostController::HostController(HostConfig config) : config_(std::move(config)) {}

HostController::~HostController() {
  quit_initiated_ = true;
  if (respawn_timer_id_) {
    KillTimer(hwnd_, respawn_timer_id_);
    respawn_timer_id_ = 0;
  }
  clear_beam_watch();
  if (beam_process_) {
    TerminateProcess(beam_process_, 0);
    CloseHandle(beam_process_);
    beam_process_ = nullptr;
  }
  for (auto& [_, tray] : trays_) {
    if (tray.registered) Shell_NotifyIconW(NIM_DELETE, &tray.nid);
  }
  for (auto& [_, icon] : icons_) {
    if (icon.icon) DestroyIcon(icon.icon);
  }
  for (auto& [_, menu] : menus_) destroy_menu_entry(menu);
  windows_.clear();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

bool HostController::start() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = HostController::HostWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kHostClass;
  RegisterClassExW(&wc);

  hwnd_ = CreateWindowExW(0, kHostClass, L"DesktopWebViewHost", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                          GetModuleHandleW(nullptr), this);
  if (!hwnd_) {
    fprintf(stderr, "edw: failed to create host window (%lu)\n", GetLastError());
    return false;
  }

  server_.set_request_handler([this](jsonutil::Json id, const std::string& method,
                                     jsonutil::Json params, RpcServer::ReplyFn reply) {
    auto* msg = new RequestMsg{this, std::move(id), method, std::move(params), std::move(reply)};
    PostMessageW(hwnd_, WM_EDW_REQUEST, 0, reinterpret_cast<LPARAM>(msg));
  });

  server_.set_disconnect_handler([this]() { PostMessageW(hwnd_, WM_EDW_DISCONNECT, 0, 0); });

  if (!server_.start(config_.host, config_.port, hwnd_)) return false;

  if (config_.beam_enabled && !config_.no_beam) {
    spawn_beam();
  }
  return true;
}

LRESULT CALLBACK HostController::HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  HostController* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    self = static_cast<HostController*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<HostController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
  if (msg == WM_NCCREATE) self->hwnd_ = hwnd;
  return self->on_host_message(hwnd, msg, wParam, lParam);
}

LRESULT HostController::on_host_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == RpcServer::WM_EDW_SOCKET) {
    server_.on_socket_event(static_cast<SOCKET>(wParam), WSAGETSELECTEVENT(lParam),
                            WSAGETSELECTERROR(lParam));
    return 0;
  }
  if (msg == WM_EDW_REQUEST) {
    auto* m = reinterpret_cast<RequestMsg*>(lParam);
    handle_request(std::move(m->id), m->method, std::move(m->params), std::move(m->reply));
    delete m;
    return 0;
  }
  if (msg == WM_EDW_DISCONNECT) {
    client_disconnected();
    return 0;
  }
  if (msg == WM_EDW_BEAM_EXIT) {
    beam_did_exit();
    return 0;
  }
  if (msg == WM_EDW_RESPAWN) {
    respawn_timer_id_ = 0;
    if (config_.beam_enabled && !config_.no_beam) {
      spawn_beam();
    }
    return 0;
  }
  if (msg == WM_TIMER && wParam == 1) {
    KillTimer(hwnd, 1);
    respawn_timer_id_ = 0;
    if (config_.beam_enabled && !config_.no_beam) {
      spawn_beam();
    }
    return 0;
  }
  if (msg == WM_EDW_TRAY) {
    if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP) {
      // Find tray by uID
      UINT id = static_cast<UINT>(wParam);
      for (auto& [tid, tray] : trays_) {
        if (tray.nid.uID == id) {
          jsonutil::Json o{{"tray_id", tid}};
          server_.notify("event.tray.click", std::move(o));
          if (lParam == WM_RBUTTONUP && !tray.menu_id.empty()) {
            auto it = menus_.find(tray.menu_id);
            if (it != menus_.end() && it->second.menu) {
              POINT pt{};
              GetCursorPos(&pt);
              SetForegroundWindow(hwnd);
              TrackPopupMenu(it->second.menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            }
          }
          break;
        }
      }
    }
    return 0;
  }
  if (msg == WM_COMMAND) {
    on_menu_command(LOWORD(wParam));
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool HostController::on_menu_command(UINT cmd) {
  for (auto& [mid, ment] : menus_) {
    auto it = ment.onclicks.find(cmd);
    if (it == ment.onclicks.end()) continue;
    jsonutil::Json o{{"menu_id", mid}, {"onclick", it->second}};
    server_.notify("event.menu.click", std::move(o));
    return true;
  }
  return false;
}

void HostController::client_disconnected() {
  reset_session();
  if (config_.lifetime == Lifetime::Coupled || config_.no_beam) {
    quit_initiated_ = true;
    clear_beam_watch();
    if (beam_process_) {
      TerminateProcess(beam_process_, 0);
      CloseHandle(beam_process_);
      beam_process_ = nullptr;
    }
    ExitProcess(0);
  }
}

void HostController::clear_beam_watch() {
  if (beam_wait_) {
    UnregisterWaitEx(beam_wait_, INVALID_HANDLE_VALUE);
    beam_wait_ = nullptr;
  }
}

void HostController::watch_beam_process() {
  clear_beam_watch();
  if (!beam_process_ || !hwnd_) return;
  // RegisterWaitForSingleObject callback runs on a thread pool thread; bounce
  // back to the UI thread via PostMessage so we can respawn safely.
  HANDLE wait = nullptr;
  if (!RegisterWaitForSingleObject(
          &wait, beam_process_,
          [](PVOID ctx, BOOLEAN /*timed_out*/) {
            auto* self = static_cast<HostController*>(ctx);
            if (self && self->hwnd_) {
              PostMessageW(self->hwnd_, WM_EDW_BEAM_EXIT, 0, 0);
            }
          },
          this, INFINITE, WT_EXECUTEONLYONCE)) {
    fprintf(stderr, "edw: RegisterWaitForSingleObject failed (%lu)\n", GetLastError());
    return;
  }
  beam_wait_ = wait;
}

void HostController::beam_did_exit() {
  clear_beam_watch();
  if (beam_process_) {
    CloseHandle(beam_process_);
    beam_process_ = nullptr;
  }
  reset_session();
  if (respawn_timer_id_) {
    KillTimer(hwnd_, respawn_timer_id_);
    respawn_timer_id_ = 0;
  }
  if (quit_initiated_) return;
  if (expected_beam_exit_) {
    expected_beam_exit_ = false;
    return;
  }
  if (should_respawn_beam()) {
    schedule_beam_respawn();
  }
}

bool HostController::should_respawn_beam() {
  if (!config_.restart_beam) return false;
  if (config_.restart_max_attempts > 0 &&
      beam_restart_attempts_ >= config_.restart_max_attempts) {
    fprintf(stderr, "edw: beam exited; restart limit reached, terminating host\n");
    PostQuitMessage(1);
    return false;
  }
  return true;
}

void HostController::schedule_beam_respawn() {
  beam_restart_attempts_ += 1;
  int shift = (std::min)(beam_restart_attempts_ - 1, 4);
  uint32_t multiplier = static_cast<uint32_t>(1) << shift;
  uint32_t backoff = (std::min)(config_.restart_backoff_ms * multiplier, 5000u);
  respawn_timer_id_ = SetTimer(hwnd_, 1, backoff, nullptr);
  if (!respawn_timer_id_) {
    fprintf(stderr, "edw: SetTimer failed; respawning immediately\n");
    spawn_beam();
  }
}

void HostController::reset_session() {
  for (auto& [_, tray] : trays_) {
    if (tray.registered) Shell_NotifyIconW(NIM_DELETE, &tray.nid);
  }
  trays_.clear();
  windows_.clear();
  webviews_.clear();
  for (auto& [_, menu] : menus_) destroy_menu_entry(menu);
  menus_.clear();
  for (auto& [_, icon] : icons_) {
    if (icon.icon) DestroyIcon(icon.icon);
  }
  icons_.clear();
  permission_policy_.clear();
  initialized_ = false;
}

std::string HostController::next_id(const std::string& prefix) {
  id_counter_++;
  return prefix + std::to_string(id_counter_);
}

void HostController::spawn_beam() {
  auto root = config_.resources_root();
  std::string beam_dir =
      config_.beam_path
          ? (is_absolute_path(*config_.beam_path) ? *config_.beam_path : join_path(root, *config_.beam_path))
          : join_path(root, "beam");
  std::string app_name = config_.beam_app.value_or("");
  if (app_name.empty()) {
    std::string bin = join_path(beam_dir, "bin");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((bin + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      std::string first_any;
      std::string first_bat;
      do {
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        if (first_any.empty()) first_any = name;
        auto lower = name;
        for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (first_bat.empty() && lower.size() >= 4 &&
            (lower.substr(lower.size() - 4) == ".bat" || lower.substr(lower.size() - 4) == ".cmd")) {
          first_bat = name;
        }
      } while (FindNextFileA(h, &fd));
      FindClose(h);
      app_name = !first_bat.empty() ? first_bat : first_any;
    }
  }
  if (app_name.empty()) {
    fprintf(stderr, "edw: no beam app_name and no bin script found in %s\n", beam_dir.c_str());
    return;
  }
  std::string script = join_path(join_path(beam_dir, "bin"), app_name);
  // Mix releases ship both a Unix `bin/<app>` shell script and `bin/<app>.bat`.
  // Prefer the batch file when present so CreateProcess does not attempt to
  // execute the extensionless shell script (Windows error 193).
  if (file_exists(script + ".bat"))
    script += ".bat";
  else if (file_exists(script + ".cmd"))
    script += ".cmd";
  else if (!file_exists(script)) {
    fprintf(stderr, "edw: beam script not found: %s\n", script.c_str());
    return;
  }
  std::string wd =
      config_.beam_working_dir
          ? (is_absolute_path(*config_.beam_working_dir) ? *config_.beam_working_dir
                                                    : join_path(root, *config_.beam_working_dir))
          : beam_dir;

  auto lower_script = script;
  for (auto& c : lower_script) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  bool is_batch = lower_script.size() >= 4 && (lower_script.substr(lower_script.size() - 4) == ".bat" ||
                                               lower_script.substr(lower_script.size() - 4) == ".cmd");

  std::ostringstream cmd;
  if (is_batch) {
    // CreateProcess cannot launch .bat directly; go through cmd.exe.
    cmd << "cmd.exe /c \"" << script << "\"";
  } else {
    cmd << '"' << script << '"';
  }
  for (auto& a : config_.beam_args) cmd << ' ' << a;
  for (auto& a : config_.forwarded_argv) cmd << ' ' << a;
  std::string cmdline = cmd.str();

  // Build environment block
  std::map<std::string, std::string> env;
  LPWCH strings = GetEnvironmentStringsW();
  if (strings) {
    for (LPWCH p = strings; *p; p += wcslen(p) + 1) {
      std::string entry = wide_to_utf8(p);
      auto eq = entry.find('=');
      if (eq != std::string::npos) env[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    FreeEnvironmentStringsW(strings);
  }
  env["EDW_PORT"] = std::to_string(server_.port());
  env["EDW_HOST"] = config_.host;
  for (auto& [k, v] : config_.extra_env) env[k] = v;

  std::wstring env_block;
  for (auto& [k, v] : env) {
    env_block += utf8_to_wide(k + "=" + v);
    env_block.push_back(L'\0');
  }
  env_block.push_back(L'\0');

  // OTP 26's user/logger need a real console (NUL triggers `nouser`; missing
  // handles crash logger). Give BEAM a hidden private console.
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::wstring wcmd = utf8_to_wide(cmdline);
  std::wstring wwd = utf8_to_wide(wd);
  std::vector<wchar_t> mutable_cmd(wcmd.begin(), wcmd.end());
  mutable_cmd.push_back(L'\0');

  clear_beam_watch();
  if (beam_process_) {
    CloseHandle(beam_process_);
    beam_process_ = nullptr;
  }

  DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE;
  if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, flags, env_block.data(),
                      wwd.c_str(), &si, &pi)) {
    fprintf(stderr, "edw: failed to spawn beam (%lu): %s\n", GetLastError(), cmdline.c_str());
    return;
  }
  CloseHandle(pi.hThread);
  beam_process_ = pi.hProcess;
  beam_restart_attempts_ = 0;
  watch_beam_process();
}

void HostController::handle_request(jsonutil::Json id, const std::string& method,
                                    jsonutil::Json params, RpcServer::ReplyFn reply) {
  if (method.rfind("test.", 0) == 0) {
    if (!config_.test_rpc) {
      reply(jsonutil::rpc_error(id, -32003, "Test RPC disabled"));
      return;
    }
    bool async = false;
    auto result = handle_test(method, params, id, &async, reply);
    if (!async) reply(result);
    return;
  }

  if (method != "initialize" && !initialized_) {
    reply(jsonutil::rpc_error(id, -32001, "Not initialized"));
    return;
  }

  jsonutil::Json out;
  int err_code = 0;
  std::string err_msg;
  if (!dispatch_ok(method, params, &out, &err_code, &err_msg)) {
    reply(jsonutil::rpc_error(id, err_code, err_msg));
  } else {
    reply(jsonutil::rpc_ok(id, std::move(out)));
  }
}

bool HostController::dispatch_ok(const std::string& method, const jsonutil::Json& params,
                                 jsonutil::Json* out_result, int* err_code, std::string* err_msg) {
  try {
    *out_result = dispatch(method, params);
    return true;
  } catch (const HostError& e) {
    *err_code = e.code;
    *err_msg = e.message;
    return false;
  } catch (const std::exception& e) {
    *err_code = -32603;
    *err_msg = e.what();
    return false;
  }
}

WebWindow* HostController::require_window(const jsonutil::Json& params) {
  auto id = jsonutil::get_string(params, "window_id");
  if (!id) throw HostError{-32002, "unknown window"};
  auto it = windows_.find(*id);
  if (it == windows_.end()) throw HostError{-32002, "unknown window"};
  return it->second.get();
}

WebWindow* HostController::require_web(const jsonutil::Json& params) {
  auto id = jsonutil::get_string(params, "webview_id");
  if (id) {
    auto wit = webviews_.find(*id);
    if (wit == webviews_.end()) throw HostError{-32002, "unknown webview"};
    auto it = windows_.find(wit->second);
    if (it == windows_.end()) throw HostError{-32002, "unknown webview"};
    return it->second.get();
  }
  if (jsonutil::get_string(params, "window_id")) return require_window(params);
  throw HostError{-32002, "unknown webview"};
}

void HostController::wire_window(WebWindow* w) {
  w->set_handlers(
      [this](const std::string& window_id) {
        server_.notify("event.window.close_requested", jsonutil::Json{{"window_id", window_id}});
      },
      [this](const std::string& window_id, bool focused) {
        server_.notify(focused ? "event.window.focus" : "event.window.blur",
                       jsonutil::Json{{"window_id", window_id}});
      },
      [this](const std::string& webview_id, const std::string& url) {
        server_.notify("event.webview.new_window",
                       jsonutil::Json{{"webview_id", webview_id}, {"url", url}});
        open_external(url);
      },
      [this](const std::string& webview_id, const std::string& url, bool error,
             const std::string& message) {
        if (error) {
          server_.notify("event.webview.error",
                         jsonutil::Json{{"webview_id", webview_id}, {"message", message}});
        } else {
          server_.notify("event.webview.finished",
                         jsonutil::Json{{"webview_id", webview_id}, {"url", url}});
        }
      },
      [this](const std::string& origin, const std::string& type, const std::string& webview_id,
             ICoreWebView2PermissionRequestedEventArgs* args) {
        handle_permission(origin, type, webview_id, args);
      });
  w->set_menu_command_handler([this](UINT cmd) { return on_menu_command(cmd); });
}

void HostController::open_external(const std::string& url) {
  ShellExecuteW(nullptr, L"open", utf8_to_wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void HostController::handle_permission(const std::string& origin, const std::string& type,
                                       const std::string& webview_id,
                                       ICoreWebView2PermissionRequestedEventArgs* args) {
  auto policy = permission_policy_[origin][type];
  if (policy.empty()) policy = "ask";

  ICoreWebView2Deferral* deferral = nullptr;
  args->GetDeferral(&deferral);

  auto finish = [args, deferral](COREWEBVIEW2_PERMISSION_STATE state) {
    args->put_State(state);
    if (deferral) {
      deferral->Complete();
      deferral->Release();
    }
    args->Release();
  };

  if (policy == "allow") {
    finish(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
    return;
  }
  if (policy == "deny") {
    finish(COREWEBVIEW2_PERMISSION_STATE_DENY);
    return;
  }

  jsonutil::Json params{{"origin", origin}, {"type", type}, {"webview_id", webview_id}};
  server_.request("permission.request", std::move(params), [finish](jsonutil::Json result) {
    auto decision = jsonutil::get_string(result, "decision").value_or("ask");
    if (decision == "allow")
      finish(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
    else if (decision == "deny")
      finish(COREWEBVIEW2_PERMISSION_STATE_DENY);
    else
      finish(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
  });
}

jsonutil::Json HostController::window_open(const jsonutil::Json& params) {
  auto title = jsonutil::get_string(params, "title").value_or("Desktop");
  int width = static_cast<int>(jsonutil::get_int(params, "width").value_or(800));
  int height = static_cast<int>(jsonutil::get_int(params, "height").value_or(600));
  auto window_id = next_id("w");
  auto webview_id = next_id("v");
  auto win = std::make_unique<WebWindow>(window_id, webview_id, title, width, height);
  wire_window(win.get());
  if (auto mw = jsonutil::get_int(params, "min_width")) {
    auto mh = jsonutil::get_int(params, "min_height").value_or(0);
    win->set_min_size(static_cast<int>(*mw), static_cast<int>(mh));
  }
  win->show();
  webviews_[webview_id] = window_id;
  windows_[window_id] = std::move(win);
  return jsonutil::Json{{"window_id", window_id}, {"webview_id", webview_id}};
}

jsonutil::Json HostController::window_close(const jsonutil::Json& params) {
  auto* w = require_window(params);
  auto wid = w->window_id();
  for (auto it = webviews_.begin(); it != webviews_.end();) {
    if (it->second == wid)
      it = webviews_.erase(it);
    else
      ++it;
  }
  windows_.erase(wid);
  return true;
}

void HostController::destroy_menu_entry(MenuEntry& entry) {
  if (entry.menu) {
    DestroyMenu(entry.menu);
    entry.menu = nullptr;
  }
  entry.onclicks.clear();
}

void HostController::append_menu_node(HMENU menu, const jsonutil::Json& node, MenuEntry& entry) {
  if (!node.is_object()) return;
  auto tag = jsonutil::get_string(node, "tag").value_or("");
  if (tag == "hr") {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    return;
  }

  const jsonutil::Json* attrs = jsonutil::get(node, "attrs");
  auto label_from_children = [&]() -> std::string {
    if (auto* kids = jsonutil::get(node, "children"); kids && kids->is_array() && !kids->empty()) {
      if ((*kids)[0].is_string()) return (*kids)[0].get<std::string>();
    }
    if (attrs) return jsonutil::get_string(*attrs, "label").value_or("");
    return "";
  };

  if (tag == "menu") {
    std::string label =
        attrs ? jsonutil::get_string(*attrs, "label").value_or(label_from_children())
              : label_from_children();
    HMENU sub = CreatePopupMenu();
    if (auto* kids = jsonutil::get(node, "children"); kids && kids->is_array()) {
      for (auto& child : *kids) append_menu_node(sub, child, entry);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), utf8_to_wide(label).c_str());
    return;
  }

  if (tag == "item") {
    std::string label = label_from_children();
    std::string onclick =
        attrs ? jsonutil::get_string(*attrs, "onclick").value_or("") : "";
    UINT cmd = next_menu_cmd_++;
    entry.onclicks[cmd] = onclick;
    AppendMenuW(menu, MF_STRING, cmd, utf8_to_wide(label).c_str());
  }
}

void HostController::build_menu_into(HMENU menu, const jsonutil::Json& dom, MenuEntry& entry) {
  const jsonutil::Json* children = nullptr;
  if (dom.is_object()) {
    auto tag = jsonutil::get_string(dom, "tag").value_or("");
    if (tag == "menubar" || tag == "menu") {
      children = jsonutil::get(dom, "children");
    }
  }
  if (!children && dom.is_array()) children = &dom;
  if (!children || !children->is_array()) return;
  for (auto& child : *children) append_menu_node(menu, child, entry);
}

jsonutil::Json HostController::menu_create(const jsonutil::Json& params) {
  auto id = next_id("m");
  MenuEntry entry;
  bool as_menubar = false;
  if (auto* dom = jsonutil::get(params, "dom"); dom && dom->is_object()) {
    as_menubar = jsonutil::get_string(*dom, "tag").value_or("") == "menubar";
  }
  entry.menu = as_menubar ? CreateMenu() : CreatePopupMenu();
  if (auto* dom = jsonutil::get(params, "dom")) build_menu_into(entry.menu, *dom, entry);
  menus_[id] = std::move(entry);
  return jsonutil::Json{{"menu_id", id}};
}

jsonutil::Json HostController::menu_update(const jsonutil::Json& params) {
  auto id = jsonutil::get_string(params, "menu_id");
  if (!id) throw HostError{-32602, "menu_id"};
  auto it = menus_.find(*id);
  if (it == menus_.end()) throw HostError{-32002, "unknown menu"};
  destroy_menu_entry(it->second);
  bool as_menubar = false;
  if (auto* dom = jsonutil::get(params, "dom"); dom && dom->is_object()) {
    as_menubar = jsonutil::get_string(*dom, "tag").value_or("") == "menubar";
  }
  it->second.menu = as_menubar ? CreateMenu() : CreatePopupMenu();
  if (auto* dom = jsonutil::get(params, "dom")) build_menu_into(it->second.menu, *dom, it->second);
  return true;
}

void HostController::update_tray_icon(TrayEntry& tray) {
  HICON icon = LoadIconW(nullptr, IDI_APPLICATION);
  if (!tray.icon_id.empty()) {
    auto it = icons_.find(tray.icon_id);
    if (it != icons_.end() && it->second.icon) icon = it->second.icon;
  }
  tray.nid.hIcon = icon;
  if (tray.registered) {
    Shell_NotifyIconW(NIM_MODIFY, &tray.nid);
  }
}

jsonutil::Json HostController::tray_create(const jsonutil::Json& params) {
  auto id = next_id("tray");
  TrayEntry t;
  t.icon_id = jsonutil::get_string(params, "icon_id").value_or("");
  t.menu_id = jsonutil::get_string(params, "menu_id").value_or("");
  memset(&t.nid, 0, sizeof(t.nid));
  t.nid.cbSize = sizeof(NOTIFYICONDATAW);
  t.nid.hWnd = hwnd_;
  t.nid.uID = static_cast<UINT>(id_counter_);
  t.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  t.nid.uCallbackMessage = WM_EDW_TRAY;
  wcsncpy_s(t.nid.szTip, L"DesktopWebView", _TRUNCATE);
  update_tray_icon(t);
  if (Shell_NotifyIconW(NIM_ADD, &t.nid)) t.registered = true;
  trays_[id] = std::move(t);
  return jsonutil::Json{{"tray_id", id}};
}

jsonutil::Json HostController::icon_create(const jsonutil::Json& params) {
  auto id = next_id("icon");
  IconEntry icon;
  if (auto path = jsonutil::get_string(params, "path")) {
    icon.icon = static_cast<HICON>(LoadImageW(nullptr, utf8_to_wide(*path).c_str(), IMAGE_ICON, 0, 0,
                                              LR_LOADFROMFILE | LR_DEFAULTSIZE));
  }
  // png_base64 accepted for protocol compatibility; decoding deferred (status: partial).
  icons_[id] = std::move(icon);
  return jsonutil::Json{{"icon_id", id}};
}

jsonutil::Json HostController::notification_show(const jsonutil::Json& params) {
  auto id = jsonutil::get_string(params, "id").value_or(next_id("n"));
  auto title = jsonutil::get_string(params, "title").value_or("");
  auto message = jsonutil::get_string(params, "message").value_or("");
  // Best-effort: use a tray balloon if any tray exists; otherwise stderr.
  if (!trays_.empty()) {
    auto& tray = trays_.begin()->second;
    tray.nid.uFlags = NIF_INFO | NIF_ICON | NIF_MESSAGE | NIF_TIP;
    wcsncpy_s(tray.nid.szInfoTitle, utf8_to_wide(title).c_str(), _TRUNCATE);
    wcsncpy_s(tray.nid.szInfo, utf8_to_wide(message).c_str(), _TRUNCATE);
    tray.nid.dwInfoFlags = NIIF_INFO;
    if (tray.registered) Shell_NotifyIconW(NIM_MODIFY, &tray.nid);
  } else {
    fprintf(stderr, "notification: %s: %s\n", title.c_str(), message.c_str());
  }
  return jsonutil::Json{{"notification_id", id}};
}

jsonutil::Json HostController::dispatch(const std::string& method, const jsonutil::Json& params) {
  const jsonutil::Json& p = params.is_object() ? params : jsonutil::Json::object();

  if (method == "initialize") {
    initialized_ = true;
    return jsonutil::Json{
        {"protocol_version", 1},
        {"platform", "windows"},
        {"capabilities",
         {{"window", true},
          {"webview", true},
          {"menu", true},
          {"tray", true},
          {"notification", true},
          {"permission", true},
          {"media", true},
          {"test_rpc", config_.test_rpc}}}};
  }

  if (method == "window.open") return window_open(p);
  if (method == "window.close" || method == "window.destroy") return window_close(p);
  if (method == "window.show") {
    auto* w = require_window(p);
    bool show = jsonutil::get_bool(p, "show").value_or(true);
    if (show)
      w->show();
    else
      w->hide();
    return true;
  }
  if (method == "window.hide") {
    require_window(p)->hide();
    return true;
  }
  if (method == "window.set_title") {
    require_window(p)->set_title(jsonutil::get_string(p, "title").value_or(""));
    return true;
  }
  if (method == "window.set_min_size") {
    require_window(p)->set_min_size(static_cast<int>(jsonutil::get_int(p, "width").value_or(0)),
                                    static_cast<int>(jsonutil::get_int(p, "height").value_or(0)));
    return true;
  }
  if (method == "window.set_icon") {
    auto* w = require_window(p);
    if (auto icon_id = jsonutil::get_string(p, "icon_id")) {
      auto it = icons_.find(*icon_id);
      if (it != icons_.end()) w->set_icon(it->second.icon);
    }
    return true;
  }
  if (method == "window.set_menubar") {
    auto* w = require_window(p);
    if (auto menu_id = jsonutil::get_string(p, "menu_id")) {
      auto it = menus_.find(*menu_id);
      if (it != menus_.end()) w->set_menubar(it->second.menu);
    }
    return true;
  }
  if (method == "window.iconize") {
    require_window(p)->iconize(jsonutil::get_bool(p, "iconize").value_or(false));
    return true;
  }
  if (method == "window.shown") return require_window(p)->shown();
  if (method == "window.active") return require_window(p)->active();
  if (method == "window.raise") {
    require_window(p)->raise();
    return true;
  }
  if (method == "window.close_veto") {
    require_window(p);
    return true;
  }

  if (method == "webview.load_url") {
    auto* w = require_web(p);
    auto url = jsonutil::get_string(p, "url");
    if (!url) throw HostError{-32602, "url required"};
    w->load_url(*url);
    return true;
  }
  if (method == "webview.reload") {
    require_web(p)->reload();
    return true;
  }
  if (method == "webview.current_url") return require_web(p)->current_url();
  if (method == "webview.rebuild") {
    auto* w = require_window(p);
    auto new_id = next_id("v");
    for (auto it = webviews_.begin(); it != webviews_.end();) {
      if (it->second == w->window_id())
        it = webviews_.erase(it);
      else
        ++it;
    }
    auto vid = w->rebuild(new_id);
    webviews_[vid] = w->window_id();
    return jsonutil::Json{{"webview_id", vid}};
  }
  if (method == "webview.set_context_menu") {
    require_web(p)->set_context_menu_enabled(jsonutil::get_bool(p, "enabled").value_or(false));
    return true;
  }

  if (method == "menu.create") return menu_create(p);
  if (method == "menu.update") return menu_update(p);
  if (method == "menu.destroy") {
    if (auto id = jsonutil::get_string(p, "menu_id")) {
      auto it = menus_.find(*id);
      if (it != menus_.end()) {
        destroy_menu_entry(it->second);
        menus_.erase(it);
      }
    }
    return true;
  }
  if (method == "menu.set_apple") return true;

  if (method == "tray.create") return tray_create(p);
  if (method == "tray.set_icon") {
    auto id = jsonutil::get_string(p, "tray_id");
    if (!id || trays_.find(*id) == trays_.end()) return false;
    trays_[*id].icon_id = jsonutil::get_string(p, "icon_id").value_or("");
    update_tray_icon(trays_[*id]);
    return true;
  }
  if (method == "tray.set_menu") {
    auto id = jsonutil::get_string(p, "tray_id");
    if (!id || trays_.find(*id) == trays_.end()) return false;
    trays_[*id].menu_id = jsonutil::get_string(p, "menu_id").value_or("");
    return true;
  }
  if (method == "tray.destroy") {
    if (auto id = jsonutil::get_string(p, "tray_id")) {
      auto it = trays_.find(*id);
      if (it != trays_.end()) {
        if (it->second.registered) Shell_NotifyIconW(NIM_DELETE, &it->second.nid);
        trays_.erase(it);
      }
    }
    return true;
  }

  if (method == "notification.show") return notification_show(p);
  if (method == "notification.close") return true;

  if (method == "icon.create") return icon_create(p);
  if (method == "icon.destroy") {
    if (auto id = jsonutil::get_string(p, "icon_id")) {
      auto it = icons_.find(*id);
      if (it != icons_.end()) {
        if (it->second.icon) DestroyIcon(it->second.icon);
        icons_.erase(it);
      }
    }
    return true;
  }

  if (method == "system.open_url") {
    if (auto url = jsonutil::get_string(p, "url")) open_external(*url);
    return true;
  }
  if (method == "system.locale") {
    wchar_t buf[128];
    if (GetUserDefaultLocaleName(buf, 128) > 0) {
      std::string loc = wide_to_utf8(buf);
      for (auto& c : loc) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      for (auto& c : loc)
        if (c == '-') c = '_';
      return loc;
    }
    return "en_us";
  }
  if (method == "system.os_description") {
    OSVERSIONINFOEXW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
#pragma warning(push)
#pragma warning(disable : 4996)
    GetVersionExW(reinterpret_cast<LPOSVERSIONINFOW>(&vi));
#pragma warning(pop)
    std::ostringstream ss;
    ss << "Windows " << vi.dwMajorVersion << "." << vi.dwMinorVersion << " (build "
       << vi.dwBuildNumber << ")";
    return ss.str();
  }
  if (method == "system.set_permission_policy") {
    auto origin = jsonutil::get_string(p, "origin");
    if (!origin) throw HostError{-32602, "origin"};
    auto& map = permission_policy_[*origin];
    if (auto c = jsonutil::get_string(p, "camera")) map["camera"] = *c;
    if (auto m = jsonutil::get_string(p, "microphone")) map["microphone"] = *m;
    return true;
  }

  if (method == "system.prepare_quit") {
    // Elixir is about to exit intentionally (Updater restart or clean quit).
    // Treat the forthcoming BEAM exit as expected so we do not respawn.
    expected_beam_exit_ = true;
    quit_initiated_ = true;
    return true;
  }

  if (method == "dialog.choose_file") return dialog_choose(p, false);
  if (method == "dialog.choose_directory") return dialog_choose(p, true);
  if (method == "dialog.prompt") return dialog_prompt(p);

  throw HostError{-32601, "Method not found: " + method};
}

jsonutil::Json HostController::dialog_choose(const jsonutil::Json& params, bool directories) {
  IFileOpenDialog* dialog = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog));
  if (FAILED(hr) || !dialog) throw HostError{-32603, "FileOpenDialog unavailable"};

  DWORD options = 0;
  dialog->GetOptions(&options);
  options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
  if (directories) options |= FOS_PICKFOLDERS;
  dialog->SetOptions(options);

  if (auto title = jsonutil::get_string(params, "title")) {
    dialog->SetTitle(utf8_to_wide(*title).c_str());
  }
  if (auto path = jsonutil::get_string(params, "default_path"); path && !path->empty()) {
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(utf8_to_wide(*path).c_str(), nullptr,
                                              IID_PPV_ARGS(&folder))) &&
        folder) {
      dialog->SetFolder(folder);
      folder->Release();
    }
  }

  hr = dialog->Show(hwnd_);
  if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    dialog->Release();
    return nullptr;
  }
  if (FAILED(hr)) {
    dialog->Release();
    throw HostError{-32603, "dialog failed"};
  }

  IShellItem* item = nullptr;
  hr = dialog->GetResult(&item);
  dialog->Release();
  if (FAILED(hr) || !item) return nullptr;

  PWSTR file_path = nullptr;
  hr = item->GetDisplayName(SIGDN_FILESYSPATH, &file_path);
  item->Release();
  if (FAILED(hr) || !file_path) return nullptr;

  std::string path = wide_to_utf8(file_path);
  CoTaskMemFree(file_path);
  return jsonutil::Json{{"path", path}};
}

namespace {

struct PromptState {
  std::wstring title;
  std::wstring message;
  std::wstring value;
  bool accepted = false;
};

INT_PTR CALLBACK PromptDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_INITDIALOG: {
      state = reinterpret_cast<PromptState*>(lParam);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
      SetWindowTextW(hwnd, state->title.c_str());
      SetDlgItemTextW(hwnd, 1001, state->message.c_str());
      SetDlgItemTextW(hwnd, 1002, state->value.c_str());
      return TRUE;
    }
    case WM_COMMAND:
      if (LOWORD(wParam) == IDOK) {
        wchar_t buf[1024];
        GetDlgItemTextW(hwnd, 1002, buf, 1024);
        if (state) {
          state->value = buf;
          state->accepted = true;
        }
        EndDialog(hwnd, IDOK);
        return TRUE;
      }
      if (LOWORD(wParam) == IDCANCEL) {
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
      }
      break;
  }
  return FALSE;
}

}  // namespace

jsonutil::Json HostController::dialog_prompt(const jsonutil::Json& params) {
  PromptState state;
  state.title = utf8_to_wide(jsonutil::get_string(params, "title").value_or(""));
  state.message = utf8_to_wide(jsonutil::get_string(params, "message").value_or(""));
  state.value = utf8_to_wide(jsonutil::get_string(params, "default_value").value_or(""));

  // In-memory dialog template: label, edit, OK, Cancel.
  std::vector<uint8_t> bytes;
  bytes.reserve(1024);
  auto align4 = [&]() {
    while (bytes.size() % 4) bytes.push_back(0);
  };
  auto append_words = [&](std::initializer_list<WORD> words) {
    for (WORD w : words) {
      bytes.push_back(static_cast<uint8_t>(w & 0xff));
      bytes.push_back(static_cast<uint8_t>((w >> 8) & 0xff));
    }
  };
  auto append_wstring = [&](const wchar_t* s) {
    while (*s) {
      append_words({static_cast<WORD>(*s)});
      ++s;
    }
    append_words({0});
  };

  DLGTEMPLATE header{};
  header.style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
  header.cdit = 4;
  header.cx = 220;
  header.cy = 90;
  const auto header_off = bytes.size();
  bytes.resize(header_off + sizeof(DLGTEMPLATE));
  memcpy(bytes.data() + header_off, &header, sizeof(header));
  append_words({0, 0});  // menu, class
  append_wstring(L"");   // title

  auto add_control = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD class_atom,
                         const wchar_t* text) {
    align4();
    DLGITEMTEMPLATE item{};
    item.style = style | WS_CHILD | WS_VISIBLE;
    item.x = x;
    item.y = y;
    item.cx = cx;
    item.cy = cy;
    item.id = id;
    const auto off = bytes.size();
    bytes.resize(off + sizeof(DLGITEMTEMPLATE));
    memcpy(bytes.data() + off, &item, sizeof(item));
    append_words({0xFFFF, class_atom});
    append_wstring(text);
    append_words({0});  // creation data
  };

  add_control(SS_LEFT, 8, 8, 200, 24, 1001, 0x0082, L"");
  add_control(ES_LEFT | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 36, 200, 14, 1002, 0x0081, L"");
  add_control(BS_DEFPUSHBUTTON | WS_TABSTOP, 100, 60, 50, 14, IDOK, 0x0080, L"OK");
  add_control(BS_PUSHBUTTON | WS_TABSTOP, 156, 60, 50, 14, IDCANCEL, 0x0080, L"Cancel");

  INT_PTR result = DialogBoxIndirectParamW(
      GetModuleHandleW(nullptr), reinterpret_cast<DLGTEMPLATE*>(bytes.data()), hwnd_, PromptDlgProc,
      reinterpret_cast<LPARAM>(&state));
  if (result != IDOK || !state.accepted) return nullptr;
  return jsonutil::Json{{"value", wide_to_utf8(state.value.c_str())}};
}

jsonutil::Json HostController::handle_test(const std::string& method, const jsonutil::Json& params,
                                           const jsonutil::Json& id, bool* async_reply,
                                           RpcServer::ReplyFn reply) {
  *async_reply = false;
  if (method == "test.ping") return jsonutil::rpc_ok(id, "pong");
  if (method == "test.echo")
    return jsonutil::rpc_ok(id, params.is_null() ? jsonutil::Json(nullptr) : params);
  if (method == "test.capabilities") {
    return jsonutil::rpc_ok(
        id, jsonutil::Json{{"window", true},
                           {"webview", true},
                           {"menu", true},
                           {"tray", true},
                           {"notification", true},
                           {"permission", true},
                           {"media", true},
                           {"test_rpc", true}});
  }
  if (method == "test.window.list") {
    jsonutil::Json items = jsonutil::Json::array();
    for (auto& [_, w] : windows_) {
      items.push_back({{"window_id", w->window_id()},
                       {"webview_id", w->webview_id()},
                       {"title", w->title()},
                       {"url", w->current_url()}});
    }
    return jsonutil::rpc_ok(id, items);
  }
  if (method == "test.tray.list") {
    jsonutil::Json items = jsonutil::Json::array();
    for (auto& [tid, _] : trays_) {
      items.push_back({{"tray_id", tid}});
    }
    return jsonutil::rpc_ok(id, items);
  }
  if (method == "test.session.reset") {
    reset_session();
    return jsonutil::rpc_ok(id, true);
  }
  if (method == "test.webview.eval") {
    auto wv = jsonutil::get_string(params, "webview_id");
    auto script = jsonutil::get_string(params, "script");
    if (!wv || !script) return jsonutil::rpc_error(id, -32002, "unknown webview");
    auto wit = webviews_.find(*wv);
    if (wit == webviews_.end()) return jsonutil::rpc_error(id, -32002, "unknown webview");
    auto it = windows_.find(wit->second);
    if (it == windows_.end()) return jsonutil::rpc_error(id, -32002, "unknown webview");
    *async_reply = true;
    jsonutil::Json id_keep = id;
    it->second->eval_js(*script, [this, id_keep, reply](jsonutil::Json result) {
      reply(jsonutil::rpc_ok(id_keep, std::move(result)));
    });
    return nullptr;
  }
  if (method == "test.permission.simulate") {
    auto origin = jsonutil::get_string(params, "origin").value_or("http://127.0.0.1");
    auto type = jsonutil::get_string(params, "type").value_or("microphone");
    std::string wv;
    if (auto v = jsonutil::get_string(params, "webview_id"))
      wv = *v;
    else if (!windows_.empty())
      wv = windows_.begin()->second->webview_id();
    server_.request("permission.request",
                    jsonutil::Json{{"origin", origin}, {"type", type}, {"webview_id", wv}},
                    [](jsonutil::Json) {});
    return jsonutil::rpc_ok(id, true);
  }
  if (method == "test.disconnect") {
    server_.close_connection();
    return jsonutil::rpc_ok(id, true);
  }
  if (method == "test.crash") {
    ExitProcess(2);
  }
  return jsonutil::rpc_error(id, -32601, "Unknown test method");
}
