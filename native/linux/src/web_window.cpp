#include "web_window.hpp"

#include "json_util.hpp"

#include <cstdio>

namespace {

std::string permission_type_name(WebKitPermissionRequest* request) {
  if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
    auto* um = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request);
    // Prefer microphone if both; WebKit exposes video/audio device flags.
    if (webkit_user_media_permission_is_for_video_device(um)) return "camera";
    if (webkit_user_media_permission_is_for_audio_device(um)) return "microphone";
    return "microphone";
  }
  if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(request)) return "microphone";
  return "microphone";
}

std::string origin_from_webview(WebKitWebView* view) {
  const gchar* uri = webkit_web_view_get_uri(view);
  if (!uri || !*uri) return "http://127.0.0.1";
  GUri* guri = g_uri_parse(uri, G_URI_FLAGS_NONE, nullptr);
  if (!guri) return "http://127.0.0.1";
  const gchar* scheme = g_uri_get_scheme(guri);
  const gchar* host = g_uri_get_host(guri);
  gint port = g_uri_get_port(guri);
  std::string out;
  if (scheme && host) {
    out = std::string(scheme) + "://" + host;
    if (port > 0) out += ":" + std::to_string(port);
  } else {
    out = uri;
  }
  g_uri_unref(guri);
  return out;
}

}  // namespace

WebWindow::WebWindow(const std::string& window_id, const std::string& webview_id,
                     const std::string& title, int width, int height)
    : window_id_(window_id), webview_id_(webview_id) {
  window_ = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(window_, title.c_str());
  gtk_window_set_default_size(window_, width, height);

  root_ = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_window_set_child(window_, GTK_WIDGET(root_));

  create_webview();

  g_signal_connect(
      window_, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer user) -> gboolean {
        auto* self = static_cast<WebWindow*>(user);
        if (self->on_close_) self->on_close_(self->window_id_);
        return TRUE;  // veto
      }),
      this);

  g_signal_connect(window_, "notify::is-active", G_CALLBACK(+[](GObject* obj, GParamSpec*,
                                                                gpointer user) {
                     auto* self = static_cast<WebWindow*>(user);
                     gboolean active = FALSE;
                     g_object_get(obj, "is-active", &active, nullptr);
                     if (self->on_focus_) self->on_focus_(self->window_id_, active);
                   }),
                   this);
}

WebWindow::~WebWindow() {
  if (window_) {
    gtk_window_destroy(window_);
    window_ = nullptr;
    webview_ = nullptr;
    root_ = nullptr;
    menubar_ = nullptr;
  }
}

void WebWindow::create_webview() {
  webview_ = WEBKIT_WEB_VIEW(webkit_web_view_new());
  gtk_widget_set_hexpand(GTK_WIDGET(webview_), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(webview_), TRUE);
  gtk_box_append(root_, GTK_WIDGET(webview_));
  attach_webview_signals();
}

void WebWindow::attach_webview_signals() {
  g_signal_connect(webview_, "create",
                   G_CALLBACK(+[](WebKitWebView*, WebKitNavigationAction* action,
                                  gpointer user) -> GtkWidget* {
                     auto* self = static_cast<WebWindow*>(user);
                     WebKitURIRequest* req = webkit_navigation_action_get_request(action);
                     const gchar* uri = req ? webkit_uri_request_get_uri(req) : nullptr;
                     if (uri && self->on_new_window_) {
                       self->on_new_window_(self->webview_id_, uri);
                     }
                     return nullptr;
                   }),
                   this);

  g_signal_connect(webview_, "permission-request",
                   G_CALLBACK(+[](WebKitWebView* view, WebKitPermissionRequest* request,
                                  gpointer user) -> gboolean {
                     auto* self = static_cast<WebWindow*>(user);
                     if (!self->on_permission_) return FALSE;
                     auto type = permission_type_name(request);
                     auto origin = origin_from_webview(view);
                     g_object_ref(request);
                     self->on_permission_(origin, type, self->webview_id_, request);
                     return TRUE;
                   }),
                   this);

  g_signal_connect(webview_, "load-changed",
                   G_CALLBACK(+[](WebKitWebView* view, WebKitLoadEvent event, gpointer user) {
                     if (event != WEBKIT_LOAD_FINISHED) return;
                     auto* self = static_cast<WebWindow*>(user);
                     const gchar* uri = webkit_web_view_get_uri(view);
                     if (self->on_nav_) self->on_nav_(self->webview_id_, uri ? uri : "", false, "");
                   }),
                   this);

  g_signal_connect(webview_, "load-failed",
                   G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent, gchar*, GError* error,
                                  gpointer user) -> gboolean {
                     auto* self = static_cast<WebWindow*>(user);
                     if (self->on_nav_) {
                       self->on_nav_(self->webview_id_, "", true,
                                     error ? error->message : "load failed");
                     }
                     return FALSE;
                   }),
                   this);

  g_signal_connect(webview_, "context-menu",
                   G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu*, WebKitHitTestResult*,
                                  gpointer user) -> gboolean {
                     auto* self = static_cast<WebWindow*>(user);
                     return self->context_menu_enabled_ ? FALSE : TRUE;
                   }),
                   this);

  g_signal_connect(webview_, "decide-policy",
                   G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* decision,
                                  WebKitPolicyDecisionType type, gpointer user) -> gboolean {
                     if (type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) return FALSE;
                     auto* self = static_cast<WebWindow*>(user);
                     auto* nav = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
                     WebKitNavigationAction* action =
                         webkit_navigation_policy_decision_get_navigation_action(nav);
                     WebKitURIRequest* req = webkit_navigation_action_get_request(action);
                     const gchar* uri = req ? webkit_uri_request_get_uri(req) : nullptr;
                     if (uri && self->on_new_window_) self->on_new_window_(self->webview_id_, uri);
                     webkit_policy_decision_ignore(decision);
                     return TRUE;
                   }),
                   this);
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

void WebWindow::load_url(const std::string& url) {
  last_url_ = url;
  webkit_web_view_load_uri(webview_, url.c_str());
}

void WebWindow::reload() { webkit_web_view_reload(webview_); }

std::string WebWindow::current_url() const {
  const gchar* uri = webkit_web_view_get_uri(webview_);
  if (uri && *uri) return uri;
  return last_url_.value_or("");
}

std::string WebWindow::rebuild(const std::string& new_webview_id) {
  if (webview_) {
    gtk_box_remove(root_, GTK_WIDGET(webview_));
    webview_ = nullptr;
  }
  webview_id_ = new_webview_id;
  create_webview();
  if (last_url_) webkit_web_view_load_uri(webview_, last_url_->c_str());
  return webview_id_;
}

void WebWindow::set_context_menu_enabled(bool enabled) { context_menu_enabled_ = enabled; }

void WebWindow::set_title(const std::string& title) { gtk_window_set_title(window_, title.c_str()); }

void WebWindow::set_min_size(int width, int height) {
  gtk_widget_set_size_request(GTK_WIDGET(window_), width, height);
}

void WebWindow::show() { gtk_window_present(window_); }

void WebWindow::hide() { gtk_widget_set_visible(GTK_WIDGET(window_), FALSE); }

void WebWindow::raise() {
  gtk_window_present(window_);
}

void WebWindow::iconize(bool iconize) {
  if (iconize) {
    gtk_window_minimize(window_);
  } else {
    gtk_window_unminimize(window_);
  }
}

bool WebWindow::shown() const { return gtk_widget_get_visible(GTK_WIDGET(window_)); }

bool WebWindow::active() const { return gtk_window_is_active(window_); }

void WebWindow::set_icon(GdkTexture* texture) {
  if (!texture) return;
  gtk_window_set_icon_name(window_, nullptr);
  // GTK4 prefers paintable via GdkSurface — best-effort default icon name left unset.
  (void)texture;
}

void WebWindow::set_menubar(GMenuModel* model) {
  if (menubar_) {
    gtk_box_remove(root_, GTK_WIDGET(menubar_));
    menubar_ = nullptr;
  }
  if (!model) return;
  menubar_ = GTK_POPOVER_MENU_BAR(gtk_popover_menu_bar_new_from_model(model));
  gtk_box_prepend(root_, GTK_WIDGET(menubar_));
}

void WebWindow::eval_js(const std::string& script,
                        std::function<void(JsonNode*)> cb) {
  struct EvalCtx {
    std::function<void(JsonNode*)> cb;
  };
  auto* ctx = new EvalCtx{std::move(cb)};
  webkit_web_view_evaluate_javascript(
      webview_, script.c_str(), -1, nullptr, nullptr, nullptr,
      +[](GObject* source, GAsyncResult* res, gpointer user_data) {
        auto* ctx = static_cast<EvalCtx*>(user_data);
        GError* err = nullptr;
        JSCValue* value =
            webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), res, &err);
        JsonNode* out = nullptr;
        if (err) {
          JsonObject* o = jsonutil::object_new();
          json_object_set_string_member(o, "error", err->message);
          out = json_node_alloc();
          json_node_init_object(out, o);
          json_object_unref(o);
          g_error_free(err);
        } else if (!value || jsc_value_is_undefined(value) || jsc_value_is_null(value)) {
          out = jsonutil::null_node();
        } else if (jsc_value_is_string(value)) {
          gchar* s = jsc_value_to_string(value);
          out = jsonutil::string_node(s ? s : "");
          g_free(s);
        } else if (jsc_value_is_boolean(value)) {
          out = jsonutil::bool_node(jsc_value_to_boolean(value));
        } else if (jsc_value_is_number(value)) {
          out = jsonutil::double_node(jsc_value_to_double(value));
        } else {
          gchar* s = jsc_value_to_string(value);
          out = jsonutil::string_node(s ? s : "");
          g_free(s);
        }
        if (value) g_object_unref(value);
        ctx->cb(out);
        delete ctx;
      },
      ctx);
}
