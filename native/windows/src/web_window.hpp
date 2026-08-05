#pragma once

#include "json_util.hpp"
#include "win_prefix.hpp"

#include <WebView2.h>
#include <wrl.h>
#include <wrl/event.h>

#include <functional>
#include <optional>
#include <string>

class WebWindow {
 public:
  using CloseHandler = std::function<void(const std::string& window_id)>;
  using FocusHandler = std::function<void(const std::string& window_id, bool focused)>;
  using NewWindowHandler =
      std::function<void(const std::string& webview_id, const std::string& url)>;
  using NavHandler = std::function<void(const std::string& webview_id, const std::string& url,
                                        bool error, const std::string& message)>;
  using PermissionHandler = std::function<void(
      const std::string& origin, const std::string& type, const std::string& webview_id,
      ICoreWebView2PermissionRequestedEventArgs* args)>;

  WebWindow(const std::string& window_id, const std::string& webview_id, const std::string& title,
            int width, int height);
  ~WebWindow();

  const std::string& window_id() const { return window_id_; }
  const std::string& webview_id() const { return webview_id_; }
  HWND hwnd() const { return hwnd_; }
  const std::string& title() const { return title_; }
  const std::optional<std::string>& last_url() const { return last_url_; }

  void set_handlers(CloseHandler on_close, FocusHandler on_focus, NewWindowHandler on_new_window,
                    NavHandler on_nav, PermissionHandler on_permission);
  void set_menu_command_handler(std::function<bool(UINT)> handler);

  void load_url(const std::string& url);
  void reload();
  std::string current_url() const;
  std::string rebuild(const std::string& new_webview_id);
  void set_context_menu_enabled(bool enabled);
  void set_title(const std::string& title);
  void set_min_size(int width, int height);
  void show();
  void hide();
  void raise();
  void iconize(bool iconize);
  bool shown() const;
  bool active() const;
  void set_icon(HICON icon);
  void set_menubar(HMENU menu);

  void eval_js(const std::string& script, std::function<void(jsonutil::Json)> cb);

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  static void register_class();

 private:
  void create_webview();
  void attach_handlers();
  void resize_webview();
  void navigate_if_ready();
  std::string origin_from_url(const std::string& url) const;

  std::string window_id_;
  std::string webview_id_;
  std::string title_;
  HWND hwnd_ = nullptr;
  int min_width_ = 0;
  int min_height_ = 0;
  bool context_menu_enabled_ = false;
  std::optional<std::string> last_url_;
  bool webview_ready_ = false;

  Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
  Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
  Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
  EventRegistrationToken nav_completed_token_{};
  EventRegistrationToken new_window_token_{};
  EventRegistrationToken permission_token_{};

  CloseHandler on_close_;
  FocusHandler on_focus_;
  NewWindowHandler on_new_window_;
  NavHandler on_nav_;
  PermissionHandler on_permission_;
  std::function<bool(UINT)> on_menu_command_;
};
