#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <webkit/webkit.h>

#include <functional>
#include <optional>
#include <string>

class HostController;

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
      WebKitPermissionRequest* request)>;

  WebWindow(const std::string& window_id, const std::string& webview_id, const std::string& title,
            int width, int height);
  ~WebWindow();

  const std::string& window_id() const { return window_id_; }
  const std::string& webview_id() const { return webview_id_; }
  GtkWindow* window() const { return window_; }
  WebKitWebView* webview() const { return webview_; }
  const std::optional<std::string>& last_url() const { return last_url_; }

  void set_handlers(CloseHandler on_close, FocusHandler on_focus, NewWindowHandler on_new_window,
                    NavHandler on_nav, PermissionHandler on_permission);

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
  void set_icon(GdkTexture* texture);
  void set_menubar(GMenuModel* model);

  void eval_js(const std::string& script,
               std::function<void(JsonNode* /*owned result or null*/)> cb);

 private:
  void attach_webview_signals();
  void create_webview();

  std::string window_id_;
  std::string webview_id_;
  GtkWindow* window_ = nullptr;
  GtkBox* root_ = nullptr;
  GtkPopoverMenuBar* menubar_ = nullptr;
  WebKitWebView* webview_ = nullptr;
  std::optional<std::string> last_url_;
  bool context_menu_enabled_ = false;

  CloseHandler on_close_;
  FocusHandler on_focus_;
  NewWindowHandler on_new_window_;
  NavHandler on_nav_;
  PermissionHandler on_permission_;
};
