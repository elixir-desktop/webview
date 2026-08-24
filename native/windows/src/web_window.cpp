#include "web_window.hpp"
#include "win_util.hpp"

#include <cstdio>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
constexpr const wchar_t* kClassName = L"DesktopWebViewWindow";
bool g_class_registered = false;
}  // namespace

void WebWindow::register_class() {
  if (g_class_registered) return;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WebWindow::WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);
  g_class_registered = true;
}

std::string WebWindow::origin_from_url(const std::string& url) const {
  if (url.empty()) return "http://127.0.0.1";
  auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) return url;
  auto host_start = scheme_end + 3;
  auto path = url.find_first_of("/?#", host_start);
  return path == std::string::npos ? url : url.substr(0, path);
}

WebWindow::WebWindow(const std::string& window_id, const std::string& webview_id,
                     const std::string& title, int width, int height)
    : window_id_(window_id), webview_id_(webview_id), title_(title) {
  register_class();
  hwnd_ = CreateWindowExW(0, kClassName, utf8_to_wide(title).c_str(), WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr,
                          GetModuleHandleW(nullptr), this);
  create_webview();
}

WebWindow::~WebWindow() {
  fail_pending_evals("webview destroyed");
  if (webview_) {
    if (nav_completed_token_.value)
      webview_->remove_NavigationCompleted(nav_completed_token_);
    if (new_window_token_.value) webview_->remove_NewWindowRequested(new_window_token_);
    if (permission_token_.value) webview_->remove_PermissionRequested(permission_token_);
  }
  if (controller_) {
    controller_->Close();
    controller_ = nullptr;
  }
  webview_ = nullptr;
  env_ = nullptr;
  if (hwnd_) {
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

LRESULT CALLBACK WebWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  WebWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    self = static_cast<WebWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<WebWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }

  if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

  switch (msg) {
    case WM_CLOSE:
      if (self->on_close_) self->on_close_(self->window_id_);
      return 0;
    case WM_SIZE:
      self->resize_webview();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
      if (self->min_width_ > 0) mmi->ptMinTrackSize.x = self->min_width_;
      if (self->min_height_ > 0) mmi->ptMinTrackSize.y = self->min_height_;
      return 0;
    }
    case WM_ACTIVATE:
      if (self->on_focus_) {
        self->on_focus_(self->window_id_, LOWORD(wParam) != WA_INACTIVE);
      }
      return 0;
    case WM_COMMAND:
      if (self->on_menu_command_ && self->on_menu_command_(LOWORD(wParam))) return 0;
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void WebWindow::create_webview() {
  webview_ready_ = false;
  controller_ = nullptr;
  webview_ = nullptr;

  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  std::wstring user_data = std::wstring(temp) + L"DesktopWebView\\" + utf8_to_wide(window_id_);

  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) {
              fprintf(stderr,
                      "edw: WebView2 environment failed (0x%08lx). Is the Evergreen Runtime "
                      "installed?\n",
                      static_cast<unsigned long>(result));
              fail_pending_evals("webview2 runtime missing");
              return result;
            }
            env_ = env;
            return env->CreateCoreWebView2Controller(
                hwnd_,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(result) || !controller) {
                        fprintf(stderr, "edw: WebView2 controller failed (0x%08lx)\n",
                                static_cast<unsigned long>(result));
                        fail_pending_evals("webview2 controller failed");
                        return result;
                      }
                      controller_ = controller;
                      controller_->get_CoreWebView2(&webview_);
                      webview_ready_ = true;
                      resize_webview();
                      attach_handlers();
                      navigate_if_ready();
                      flush_pending_evals();
                      return S_OK;
                    })
                    .Get());
          })
          .Get());

  if (FAILED(hr)) {
    fprintf(stderr,
            "edw: CreateCoreWebView2EnvironmentWithOptions failed (0x%08lx). Is the Evergreen "
            "Runtime installed?\n",
            static_cast<unsigned long>(hr));
    fail_pending_evals("webview2 runtime missing");
  }
}

void WebWindow::attach_handlers() {
  if (!webview_) return;

  webview_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL ok = FALSE;
            args->get_IsSuccess(&ok);
            if (ok) {
              LPWSTR raw = nullptr;
              sender->get_Source(&raw);
              std::string url = wide_to_utf8(raw);
              if (raw) CoTaskMemFree(raw);
              if (on_nav_) on_nav_(webview_id_, url, false, "");
            } else {
              COREWEBVIEW2_WEB_ERROR_STATUS status{};
              args->get_WebErrorStatus(&status);
              if (on_nav_)
                on_nav_(webview_id_, "", true, "navigation failed: " + std::to_string(status));
            }
            flush_pending_evals();
            return S_OK;
          })
          .Get(),
      &nav_completed_token_);

  webview_->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            LPWSTR raw = nullptr;
            args->get_Uri(&raw);
            std::string url = wide_to_utf8(raw);
            if (raw) CoTaskMemFree(raw);
            args->put_Handled(TRUE);
            if (on_new_window_) on_new_window_(webview_id_, url);
            return S_OK;
          })
          .Get(),
      &new_window_token_);

  webview_->add_PermissionRequested(
      Callback<ICoreWebView2PermissionRequestedEventHandler>(
          [this](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            if (!on_permission_) return S_OK;
            COREWEBVIEW2_PERMISSION_KIND kind{};
            args->get_PermissionKind(&kind);
            std::string type = "microphone";
            if (kind == COREWEBVIEW2_PERMISSION_KIND_CAMERA) type = "camera";
            else if (kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE) type = "microphone";
            LPWSTR raw = nullptr;
            args->get_Uri(&raw);
            std::string origin = origin_from_url(wide_to_utf8(raw));
            if (raw) CoTaskMemFree(raw);
            args->AddRef();
            on_permission_(origin, type, webview_id_, args);
            return S_OK;
          })
          .Get(),
      &permission_token_);

  ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
    settings->put_AreDefaultContextMenusEnabled(context_menu_enabled_ ? TRUE : FALSE);
  }
}

void WebWindow::resize_webview() {
  if (!controller_ || !hwnd_) return;
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  controller_->put_Bounds(rc);
}

void WebWindow::navigate_if_ready() {
  if (!webview_ready_ || !webview_ || !last_url_) return;
  webview_->Navigate(utf8_to_wide(*last_url_).c_str());
}

void WebWindow::set_handlers(CloseHandler on_close, FocusHandler on_focus,
                             NewWindowHandler on_new_window, NavHandler on_nav,
                             PermissionHandler on_permission) {
  on_close_ = std::move(on_close);
  on_focus_ = std::move(on_focus);
  on_new_window_ = std::move(on_new_window);
  on_nav_ = std::move(on_nav);
  on_permission_ = std::move(on_permission);
}

void WebWindow::set_menu_command_handler(std::function<bool(UINT)> handler) {
  on_menu_command_ = std::move(handler);
}

void WebWindow::load_url(const std::string& url) {
  last_url_ = url;
  navigate_if_ready();
}

void WebWindow::reload() {
  if (webview_ready_ && webview_) webview_->Reload();
}

std::string WebWindow::current_url() const {
  if (webview_ready_ && webview_) {
    LPWSTR raw = nullptr;
    if (SUCCEEDED(webview_->get_Source(&raw)) && raw) {
      std::string url = wide_to_utf8(raw);
      CoTaskMemFree(raw);
      if (!url.empty() && url != "about:blank") return url;
    }
  }
  return last_url_.value_or("");
}

std::string WebWindow::rebuild(const std::string& new_webview_id) {
  if (webview_) {
    if (nav_completed_token_.value)
      webview_->remove_NavigationCompleted(nav_completed_token_);
    if (new_window_token_.value) webview_->remove_NewWindowRequested(new_window_token_);
    if (permission_token_.value) webview_->remove_PermissionRequested(permission_token_);
    nav_completed_token_ = {};
    new_window_token_ = {};
    permission_token_ = {};
  }
  if (controller_) {
    controller_->Close();
    controller_ = nullptr;
  }
  webview_ = nullptr;
  webview_id_ = new_webview_id;
  create_webview();
  return webview_id_;
}

void WebWindow::set_context_menu_enabled(bool enabled) {
  context_menu_enabled_ = enabled;
  if (webview_) {
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
      settings->put_AreDefaultContextMenusEnabled(enabled ? TRUE : FALSE);
    }
  }
}

void WebWindow::set_title(const std::string& title) {
  title_ = title;
  if (hwnd_) SetWindowTextW(hwnd_, utf8_to_wide(title).c_str());
}

void WebWindow::set_min_size(int width, int height) {
  min_width_ = width;
  min_height_ = height;
}

void WebWindow::show() {
  if (hwnd_) ShowWindow(hwnd_, SW_SHOW);
}

void WebWindow::hide() {
  if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void WebWindow::raise() {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
}

void WebWindow::iconize(bool iconize) {
  if (!hwnd_) return;
  ShowWindow(hwnd_, iconize ? SW_MINIMIZE : SW_RESTORE);
}

bool WebWindow::shown() const { return hwnd_ && IsWindowVisible(hwnd_); }

bool WebWindow::active() const { return hwnd_ && GetForegroundWindow() == hwnd_; }

void WebWindow::set_icon(HICON icon) {
  if (!hwnd_ || !icon) return;
  SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
  SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
}

void WebWindow::set_menubar(HMENU menu) {
  if (hwnd_) SetMenu(hwnd_, menu);
}

void WebWindow::run_eval(const std::string& script, std::function<void(jsonutil::Json)> cb) {
  auto cb_ptr = std::make_shared<std::function<void(jsonutil::Json)>>(std::move(cb));
  webview_->ExecuteScript(
      utf8_to_wide(script).c_str(),
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
          [cb_ptr](HRESULT error, LPCWSTR result_json) -> HRESULT {
            if (FAILED(error)) {
              (*cb_ptr)(jsonutil::Json{{"error", "execute script failed"}});
              return S_OK;
            }
            auto text = wide_to_utf8(result_json);
            auto parsed = jsonutil::parse(text.empty() ? "null" : text);
            if (parsed.is_null() && !text.empty() && text != "null") {
              (*cb_ptr)(jsonutil::Json{{"error", "invalid script result json"}});
            } else {
              (*cb_ptr)(parsed);
            }
            return S_OK;
          })
          .Get());
}

void WebWindow::flush_pending_evals() {
  if (!webview_ready_ || !webview_ || pending_evals_.empty()) return;
  auto queued = std::move(pending_evals_);
  pending_evals_.clear();
  for (auto& [script, cb] : queued) run_eval(script, std::move(cb));
}

void WebWindow::fail_pending_evals(const std::string& message) {
  auto queued = std::move(pending_evals_);
  pending_evals_.clear();
  for (auto& [_, cb] : queued) cb(jsonutil::Json{{"error", message}});
}

void WebWindow::eval_js(const std::string& script, std::function<void(jsonutil::Json)> cb) {
  if (!webview_ready_ || !webview_) {
    pending_evals_.emplace_back(script, std::move(cb));
    return;
  }
  run_eval(script, std::move(cb));
}
