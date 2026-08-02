#include "host_controller.hpp"

#include "json_util.hpp"

#include <cctype>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fstream>
#include <sys/utsname.h>
#include <unistd.h>

namespace {

std::string join_path(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

bool is_absolute(const std::string& p) { return !p.empty() && p[0] == '/'; }

std::vector<uint8_t> base64_decode(const std::string& in) {
  gsize len = 0;
  guchar* data = g_base64_decode(in.c_str(), &len);
  std::vector<uint8_t> out;
  if (data) {
    out.assign(data, data + len);
    g_free(data);
  }
  return out;
}

JsonObject* params_obj(JsonNode* params) {
  if (!params) return nullptr;
  return jsonutil::as_object(params);
}

}  // namespace

HostController::HostController(HostConfig config) : config_(std::move(config)) {}

HostController::~HostController() {
  if (beam_pid_ > 0) {
    kill(beam_pid_, SIGTERM);
    beam_pid_ = 0;
  }
  for (auto& [_, icon] : icons_) {
    if (icon.texture) g_object_unref(icon.texture);
  }
  windows_.clear();
}

bool HostController::start() {
  notify_init("DesktopWebView");

  server_.set_request_handler([this](JsonNode* id, const std::string& method, JsonNode* params,
                                     RpcServer::ReplyFn reply) {
    // Marshal onto GTK main thread
    struct Ctx {
      HostController* self;
      JsonNode* id;
      std::string method;
      JsonNode* params;
      RpcServer::ReplyFn reply;
    };
    auto* ctx = new Ctx{this, id, method, params, std::move(reply)};
    g_idle_add(
        +[](gpointer data) -> gboolean {
          auto* ctx = static_cast<Ctx*>(data);
          ctx->self->handle_request(ctx->id, ctx->method, ctx->params, ctx->reply);
          // id/params ownership transferred into handle_request free paths
          delete ctx;
          return G_SOURCE_REMOVE;
        },
        ctx);
  });

  server_.set_disconnect_handler([this]() {
    g_idle_add(
        +[](gpointer data) -> gboolean {
          static_cast<HostController*>(data)->client_disconnected();
          return G_SOURCE_REMOVE;
        },
        this);
  });

  if (!server_.start(config_.host, config_.port)) return false;

  if (config_.beam_enabled && !config_.no_beam) {
    spawn_beam();
  }
  return true;
}

void HostController::client_disconnected() {
  if (config_.lifetime == Lifetime::Coupled) {
    if (beam_pid_ > 0) {
      kill(beam_pid_, SIGTERM);
      beam_pid_ = 0;
    }
    exit(0);
  }
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
          ? (is_absolute(*config_.beam_path) ? *config_.beam_path : join_path(root, *config_.beam_path))
          : join_path(root, "beam");
  std::string app_name = config_.beam_app.value_or("");
  if (app_name.empty()) {
    // first script in bin/
    GDir* dir = g_dir_open(join_path(beam_dir, "bin").c_str(), 0, nullptr);
    if (dir) {
      const gchar* name;
      while ((name = g_dir_read_name(dir))) {
        if (name[0] == '.') continue;
        app_name = name;
        break;
      }
      g_dir_close(dir);
    }
  }
  if (app_name.empty()) {
    fprintf(stderr, "edw: no beam app_name and no bin script found in %s\n", beam_dir.c_str());
    return;
  }
  std::string script = join_path(join_path(beam_dir, "bin"), app_name);
  std::string wd =
      config_.beam_working_dir
          ? (is_absolute(*config_.beam_working_dir) ? *config_.beam_working_dir
                                                    : join_path(root, *config_.beam_working_dir))
          : beam_dir;

  std::vector<std::string> argv_store;
  argv_store.push_back(script);
  for (auto& a : config_.beam_args) argv_store.push_back(a);
  for (auto& a : config_.forwarded_argv) argv_store.push_back(a);
  std::vector<char*> argv;
  for (auto& s : argv_store) argv.push_back(s.data());
  argv.push_back(nullptr);

  std::vector<std::string> env_store;
  for (char** e = environ; e && *e; ++e) env_store.emplace_back(*e);
  env_store.push_back("EDW_PORT=" + std::to_string(server_.port()));
  env_store.push_back("EDW_HOST=" + config_.host);
  for (auto& [k, v] : config_.extra_env) env_store.push_back(k + "=" + v);
  std::vector<char*> envp;
  for (auto& s : env_store) envp.push_back(s.data());
  envp.push_back(nullptr);

  GError* err = nullptr;
  if (!g_spawn_async(wd.c_str(), argv.data(), envp.data(), G_SPAWN_DEFAULT, nullptr, nullptr,
                     &beam_pid_, &err)) {
    fprintf(stderr, "edw: failed to spawn beam: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    beam_pid_ = 0;
  }
}

void HostController::handle_request(JsonNode* id, const std::string& method, JsonNode* params,
                                    RpcServer::ReplyFn reply) {
  auto free_params = [&]() {
    if (params) json_node_free(params);
    if (id) json_node_free(id);
  };

  if (method.rfind("test.", 0) == 0) {
    if (!config_.test_rpc) {
      reply(jsonutil::rpc_error(id, -32003, "Test RPC disabled"));
      free_params();
      return;
    }
    bool async = false;
    JsonNode* result = handle_test(method, params, id, &async);
    if (!async) {
      if (result) {
        // handle_test returns full response for sync
        reply(result);
      }
      free_params();
    } else {
      // async path copied id; free originals
      free_params();
    }
    return;
  }

  if (method != "initialize" && !initialized_) {
    reply(jsonutil::rpc_error(id, -32001, "Not initialized"));
    free_params();
    return;
  }

  JsonNode* out = nullptr;
  int err_code = 0;
  std::string err_msg;
  if (!dispatch_ok(method, params, &out, &err_code, &err_msg)) {
    reply(jsonutil::rpc_error(id, err_code, err_msg));
  } else {
    reply(jsonutil::rpc_ok(id, out));
  }
  free_params();
}

bool HostController::dispatch_ok(const std::string& method, JsonNode* params, JsonNode** out_result,
                                 int* err_code, std::string* err_msg) {
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

WebWindow* HostController::require_window(JsonObject* params) {
  auto id = jsonutil::object_get_string(params, "window_id");
  if (!id) throw HostError{-32002, "unknown window"};
  auto it = windows_.find(*id);
  if (it == windows_.end()) throw HostError{-32002, "unknown window"};
  return it->second.get();
}

WebWindow* HostController::require_web(JsonObject* params) {
  auto id = jsonutil::object_get_string(params, "webview_id");
  if (id) {
    auto wit = webviews_.find(*id);
    if (wit == webviews_.end()) throw HostError{-32002, "unknown webview"};
    auto it = windows_.find(wit->second);
    if (it == windows_.end()) throw HostError{-32002, "unknown webview"};
    return it->second.get();
  }
  if (jsonutil::object_get_string(params, "window_id")) return require_window(params);
  throw HostError{-32002, "unknown webview"};
}

void HostController::wire_window(WebWindow* w) {
  w->set_handlers(
      [this](const std::string& window_id) {
        JsonObject* o = jsonutil::object_new();
        json_object_set_string_member(o, "window_id", window_id.c_str());
        JsonNode* n = json_node_alloc();
        json_node_init_object(n, o);
        json_object_unref(o);
        server_.notify("event.window.close_requested", n);
      },
      [this](const std::string& window_id, bool focused) {
        JsonObject* o = jsonutil::object_new();
        json_object_set_string_member(o, "window_id", window_id.c_str());
        JsonNode* n = json_node_alloc();
        json_node_init_object(n, o);
        json_object_unref(o);
        server_.notify(focused ? "event.window.focus" : "event.window.blur", n);
      },
      [this](const std::string& webview_id, const std::string& url) {
        JsonObject* o = jsonutil::object_new();
        json_object_set_string_member(o, "webview_id", webview_id.c_str());
        json_object_set_string_member(o, "url", url.c_str());
        JsonNode* n = json_node_alloc();
        json_node_init_object(n, o);
        json_object_unref(o);
        server_.notify("event.webview.new_window", n);
        open_external(url);
      },
      [this](const std::string& webview_id, const std::string& url, bool error,
             const std::string& message) {
        JsonObject* o = jsonutil::object_new();
        json_object_set_string_member(o, "webview_id", webview_id.c_str());
        if (error) {
          json_object_set_string_member(o, "message", message.c_str());
          JsonNode* n = json_node_alloc();
          json_node_init_object(n, o);
          json_object_unref(o);
          server_.notify("event.webview.error", n);
        } else {
          json_object_set_string_member(o, "url", url.c_str());
          JsonNode* n = json_node_alloc();
          json_node_init_object(n, o);
          json_object_unref(o);
          server_.notify("event.webview.finished", n);
        }
      },
      [this](const std::string& origin, const std::string& type, const std::string& webview_id,
             WebKitPermissionRequest* request) {
        handle_permission(origin, type, webview_id, request);
      });
}

void HostController::open_external(const std::string& url) {
  g_app_info_launch_default_for_uri(url.c_str(), nullptr, nullptr);
}

void HostController::handle_permission(const std::string& origin, const std::string& type,
                                       const std::string& webview_id,
                                       WebKitPermissionRequest* request) {
  auto policy = permission_policy_[origin][type];
  if (policy.empty()) policy = "ask";
  if (policy == "allow") {
    webkit_permission_request_allow(request);
    g_object_unref(request);
    return;
  }
  if (policy == "deny") {
    webkit_permission_request_deny(request);
    g_object_unref(request);
    return;
  }

  JsonObject* o = jsonutil::object_new();
  json_object_set_string_member(o, "origin", origin.c_str());
  json_object_set_string_member(o, "type", type.c_str());
  json_object_set_string_member(o, "webview_id", webview_id.c_str());
  JsonNode* params = json_node_alloc();
  json_node_init_object(params, o);
  json_object_unref(o);

  server_.request("permission.request", params, [request](JsonNode* result) {
    std::string decision = "ask";
    if (auto* obj = jsonutil::as_object(result)) {
      decision = jsonutil::object_get_string(obj, "decision").value_or("ask");
    }
    if (decision == "allow") {
      webkit_permission_request_allow(request);
    } else if (decision == "deny") {
      webkit_permission_request_deny(request);
    } else {
      // ask → leave to engine; allow triggers OS/portal prompts when applicable
      webkit_permission_request_allow(request);
    }
    g_object_unref(request);
  });
}

JsonNode* HostController::window_open(JsonObject* params) {
  auto title = jsonutil::object_get_string(params, "title").value_or("Desktop");
  int width = static_cast<int>(jsonutil::object_get_int(params, "width").value_or(800));
  int height = static_cast<int>(jsonutil::object_get_int(params, "height").value_or(600));
  auto window_id = next_id("w");
  auto webview_id = next_id("v");
  auto win = std::make_unique<WebWindow>(window_id, webview_id, title, width, height);
  wire_window(win.get());
  if (auto mw = jsonutil::object_get_int(params, "min_width")) {
    auto mh = jsonutil::object_get_int(params, "min_height").value_or(0);
    win->set_min_size(static_cast<int>(*mw), static_cast<int>(mh));
  }
  win->show();
  webviews_[webview_id] = window_id;
  windows_[window_id] = std::move(win);

  JsonObject* o = jsonutil::object_new();
  json_object_set_string_member(o, "window_id", window_id.c_str());
  json_object_set_string_member(o, "webview_id", webview_id.c_str());
  JsonNode* n = json_node_alloc();
  json_node_init_object(n, o);
  json_object_unref(o);
  return n;
}

JsonNode* HostController::window_close(JsonObject* params) {
  auto* w = require_window(params);
  auto wid = w->window_id();
  for (auto it = webviews_.begin(); it != webviews_.end();) {
    if (it->second == wid)
      it = webviews_.erase(it);
    else
      ++it;
  }
  windows_.erase(wid);
  return jsonutil::bool_node(true);
}

void HostController::append_menu_node(GMenu* menu, JsonNode* node, MenuEntry& entry,
                                      const std::string& menu_id) {
  JsonObject* obj = jsonutil::as_object(node);
  if (!obj) return;
  auto tag = jsonutil::object_get_string(obj, "tag").value_or("");
  if (tag == "hr") {
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(g_menu_new()));
    return;
  }
  JsonObject* attrs = nullptr;
  if (auto* a = jsonutil::object_get(obj, "attrs")) attrs = jsonutil::as_object(a);

  auto label_from_children = [&]() -> std::string {
    if (auto* kids = jsonutil::as_array(jsonutil::object_get(obj, "children"))) {
      if (json_array_get_length(kids) > 0) {
        if (auto s = jsonutil::as_string(json_array_get_element(kids, 0))) return *s;
      }
    }
    if (attrs) return jsonutil::object_get_string(attrs, "label").value_or("");
    return "";
  };

  if (tag == "menu") {
    std::string label =
        attrs ? jsonutil::object_get_string(attrs, "label").value_or(label_from_children())
              : label_from_children();
    GMenu* sub = g_menu_new();
    if (auto* kids = jsonutil::as_array(jsonutil::object_get(obj, "children"))) {
      guint n = json_array_get_length(kids);
      for (guint i = 0; i < n; i++) {
        append_menu_node(sub, json_array_get_element(kids, i), entry, menu_id);
      }
    }
    g_menu_append_submenu(menu, label.c_str(), G_MENU_MODEL(sub));
    g_object_unref(sub);
    return;
  }

  if (tag == "item") {
    std::string label = label_from_children();
    std::string onclick =
        attrs ? jsonutil::object_get_string(attrs, "onclick").value_or("") : "";
    std::string action = "item" + std::to_string(entry.onclicks.size() + 1);
    std::string detailed = entry.action_group_name + "." + action;
    entry.onclicks[action] = onclick;
    auto* simple = g_simple_action_new(action.c_str(), nullptr);
    g_signal_connect(
        simple, "activate",
        G_CALLBACK(+[](GSimpleAction* act, GVariant*, gpointer user) {
          auto* self = static_cast<HostController*>(user);
          const gchar* name = g_action_get_name(G_ACTION(act));
          // Find menu entry containing this action — scan menus
          for (auto& [mid, ment] : self->menus_) {
            auto it = ment.onclicks.find(name);
            if (it == ment.onclicks.end()) continue;
            JsonObject* o = jsonutil::object_new();
            json_object_set_string_member(o, "menu_id", mid.c_str());
            json_object_set_string_member(o, "onclick", it->second.c_str());
            JsonNode* n = json_node_alloc();
            json_node_init_object(n, o);
            json_object_unref(o);
            self->server_.notify("event.menu.click", n);
            break;
          }
        }),
        this);
    g_action_map_add_action(G_ACTION_MAP(entry.action_group), G_ACTION(simple));
    g_object_unref(simple);
    g_menu_append(menu, label.c_str(), detailed.c_str());
  }
}

void HostController::build_menu_into(GMenu* menu, JsonNode* dom, MenuEntry& entry,
                                     const std::string& menu_id) {
  if (!dom) return;
  JsonObject* root = jsonutil::as_object(dom);
  JsonArray* children = nullptr;
  if (root) {
    auto tag = jsonutil::object_get_string(root, "tag").value_or("");
    if (tag == "menubar" || tag == "menu") {
      children = jsonutil::as_array(jsonutil::object_get(root, "children"));
    }
  }
  if (!children) children = jsonutil::as_array(dom);
  if (!children) return;
  guint n = json_array_get_length(children);
  for (guint i = 0; i < n; i++) {
    append_menu_node(menu, json_array_get_element(children, i), entry, menu_id);
  }
}

JsonNode* HostController::menu_create(JsonObject* params) {
  auto id = next_id("m");
  MenuEntry entry;
  entry.menu = g_menu_new();
  entry.action_group_name = "edw" + id;
  entry.action_group = g_simple_action_group_new();
  build_menu_into(entry.menu, jsonutil::object_get(params, "dom"), entry, id);
  // Insert action group on default display / app — attach to each window when set
  menus_[id] = std::move(entry);

  JsonObject* o = jsonutil::object_new();
  json_object_set_string_member(o, "menu_id", id.c_str());
  JsonNode* n = json_node_alloc();
  json_node_init_object(n, o);
  json_object_unref(o);
  return n;
}

JsonNode* HostController::menu_update(JsonObject* params) {
  auto id = jsonutil::object_get_string(params, "menu_id");
  if (!id) throw HostError{-32602, "menu_id"};
  auto it = menus_.find(*id);
  if (it == menus_.end()) throw HostError{-32002, "unknown menu"};
  if (it->second.menu) g_object_unref(it->second.menu);
  if (it->second.action_group) g_object_unref(it->second.action_group);
  it->second.menu = g_menu_new();
  it->second.action_group = g_simple_action_group_new();
  it->second.onclicks.clear();
  it->second.action_group_name = "edw" + *id;
  build_menu_into(it->second.menu, jsonutil::object_get(params, "dom"), it->second, *id);
  return jsonutil::bool_node(true);
}

JsonNode* HostController::tray_create(JsonObject* params) {
  auto id = next_id("tray");
  TrayEntry t;
  t.icon_id = jsonutil::object_get_string(params, "icon_id").value_or("");
  t.menu_id = jsonutil::object_get_string(params, "menu_id").value_or("");
  trays_[id] = std::move(t);
  // StatusNotifierItem / AppIndicator is GTK3-linked; keep in-memory tray for protocol
  // conformance and document fallback in README.
  JsonObject* o = jsonutil::object_new();
  json_object_set_string_member(o, "tray_id", id.c_str());
  JsonNode* n = json_node_alloc();
  json_node_init_object(n, o);
  json_object_unref(o);
  return n;
}

JsonNode* HostController::icon_create(JsonObject* params) {
  auto id = next_id("icon");
  IconEntry icon;
  if (auto path = jsonutil::object_get_string(params, "path")) {
    icon.path = *path;
    GError* err = nullptr;
    icon.texture = gdk_texture_new_from_filename(path->c_str(), &err);
    if (err) {
      g_error_free(err);
      icon.texture = nullptr;
    }
  } else if (auto b64 = jsonutil::object_get_string(params, "png_base64")) {
    icon.png = base64_decode(*b64);
    if (!icon.png.empty()) {
      GBytes* bytes = g_bytes_new(icon.png.data(), icon.png.size());
      GError* err = nullptr;
      icon.texture = gdk_texture_new_from_bytes(bytes, &err);
      g_bytes_unref(bytes);
      if (err) {
        g_error_free(err);
        icon.texture = nullptr;
      }
    }
  }
  icons_[id] = std::move(icon);
  JsonObject* o = jsonutil::object_new();
  json_object_set_string_member(o, "icon_id", id.c_str());
  JsonNode* n = json_node_alloc();
  json_node_init_object(n, o);
  json_object_unref(o);
  return n;
}

JsonNode* HostController::notification_show(JsonObject* params) {
  auto id = jsonutil::object_get_string(params, "id").value_or(next_id("n"));
  auto title = jsonutil::object_get_string(params, "title").value_or("");
  auto message = jsonutil::object_get_string(params, "message").value_or("");
  NotifyNotification* n =
      notify_notification_new(title.c_str(), message.c_str(), nullptr);
  GError* err = nullptr;
  if (!notify_notification_show(n, &err)) {
    fprintf(stderr, "notification: %s: %s\n", title.c_str(), message.c_str());
    if (err) g_error_free(err);
  }
  g_object_unref(n);
  JsonObject* o = jsonutil::object_new();
  json_object_set_string_member(o, "notification_id", id.c_str());
  JsonNode* node = json_node_alloc();
  json_node_init_object(node, o);
  json_object_unref(o);
  return node;
}

JsonNode* HostController::dispatch(const std::string& method, JsonNode* params) {
  JsonObject* p = params_obj(params);

  if (method == "initialize") {
    initialized_ = true;
    JsonObject* caps = jsonutil::object_new();
    json_object_set_boolean_member(caps, "window", TRUE);
    json_object_set_boolean_member(caps, "webview", TRUE);
    json_object_set_boolean_member(caps, "menu", TRUE);
    json_object_set_boolean_member(caps, "tray", TRUE);
    json_object_set_boolean_member(caps, "notification", TRUE);
    json_object_set_boolean_member(caps, "permission", TRUE);
    json_object_set_boolean_member(caps, "media", TRUE);
    json_object_set_boolean_member(caps, "test_rpc", config_.test_rpc);

    JsonObject* o = jsonutil::object_new();
    json_object_set_int_member(o, "protocol_version", 1);
    json_object_set_string_member(o, "platform", "linux");
    JsonNode* caps_node = json_node_alloc();
    json_node_init_object(caps_node, caps);
    json_object_unref(caps);
    json_object_set_member(o, "capabilities", caps_node);
    JsonNode* n = json_node_alloc();
    json_node_init_object(n, o);
    json_object_unref(o);
    return n;
  }

  if (method == "window.open") return window_open(p);
  if (method == "window.close" || method == "window.destroy") return window_close(p);
  if (method == "window.show") {
    auto* w = require_window(p);
    bool show = jsonutil::object_get_bool(p, "show").value_or(true);
    if (show)
      w->show();
    else
      w->hide();
    return jsonutil::bool_node(true);
  }
  if (method == "window.hide") {
    require_window(p)->hide();
    return jsonutil::bool_node(true);
  }
  if (method == "window.set_title") {
    auto* w = require_window(p);
    w->set_title(jsonutil::object_get_string(p, "title").value_or(""));
    return jsonutil::bool_node(true);
  }
  if (method == "window.set_min_size") {
    auto* w = require_window(p);
    w->set_min_size(static_cast<int>(jsonutil::object_get_int(p, "width").value_or(0)),
                    static_cast<int>(jsonutil::object_get_int(p, "height").value_or(0)));
    return jsonutil::bool_node(true);
  }
  if (method == "window.set_icon") {
    auto* w = require_window(p);
    if (auto icon_id = jsonutil::object_get_string(p, "icon_id")) {
      auto it = icons_.find(*icon_id);
      if (it != icons_.end()) w->set_icon(it->second.texture);
    }
    return jsonutil::bool_node(true);
  }
  if (method == "window.set_menubar") {
    auto* w = require_window(p);
    if (auto menu_id = jsonutil::object_get_string(p, "menu_id")) {
      auto it = menus_.find(*menu_id);
      if (it != menus_.end()) {
        gtk_widget_insert_action_group(GTK_WIDGET(w->window()), it->second.action_group_name.c_str(),
                                       G_ACTION_GROUP(it->second.action_group));
        w->set_menubar(G_MENU_MODEL(it->second.menu));
      }
    }
    return jsonutil::bool_node(true);
  }
  if (method == "window.iconize") {
    require_window(p)->iconize(jsonutil::object_get_bool(p, "iconize").value_or(false));
    return jsonutil::bool_node(true);
  }
  if (method == "window.shown") return jsonutil::bool_node(require_window(p)->shown());
  if (method == "window.active") return jsonutil::bool_node(require_window(p)->active());
  if (method == "window.raise") {
    require_window(p)->raise();
    return jsonutil::bool_node(true);
  }
  if (method == "window.close_veto") {
    require_window(p);
    return jsonutil::bool_node(true);
  }

  if (method == "webview.load_url") {
    auto* w = require_web(p);
    auto url = jsonutil::object_get_string(p, "url");
    if (!url) throw HostError{-32602, "url required"};
    w->load_url(*url);
    return jsonutil::bool_node(true);
  }
  if (method == "webview.reload") {
    require_web(p)->reload();
    return jsonutil::bool_node(true);
  }
  if (method == "webview.current_url") {
    return jsonutil::string_node(require_web(p)->current_url());
  }
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
    JsonObject* o = jsonutil::object_new();
    json_object_set_string_member(o, "webview_id", vid.c_str());
    JsonNode* n = json_node_alloc();
    json_node_init_object(n, o);
    json_object_unref(o);
    return n;
  }
  if (method == "webview.set_context_menu") {
    require_web(p)->set_context_menu_enabled(
        jsonutil::object_get_bool(p, "enabled").value_or(false));
    return jsonutil::bool_node(true);
  }

  if (method == "menu.create") return menu_create(p);
  if (method == "menu.update") return menu_update(p);
  if (method == "menu.destroy") {
    if (auto id = jsonutil::object_get_string(p, "menu_id")) {
      auto it = menus_.find(*id);
      if (it != menus_.end()) {
        if (it->second.menu) g_object_unref(it->second.menu);
        if (it->second.action_group) g_object_unref(it->second.action_group);
        menus_.erase(it);
      }
    }
    return jsonutil::bool_node(true);
  }
  if (method == "menu.set_apple") {
    // n/a on Linux — successful no-op
    return jsonutil::bool_node(true);
  }

  if (method == "tray.create") return tray_create(p);
  if (method == "tray.set_icon") {
    auto id = jsonutil::object_get_string(p, "tray_id");
    if (!id || trays_.find(*id) == trays_.end()) return jsonutil::bool_node(false);
    trays_[*id].icon_id = jsonutil::object_get_string(p, "icon_id").value_or("");
    return jsonutil::bool_node(true);
  }
  if (method == "tray.set_menu") {
    auto id = jsonutil::object_get_string(p, "tray_id");
    if (!id || trays_.find(*id) == trays_.end()) return jsonutil::bool_node(false);
    trays_[*id].menu_id = jsonutil::object_get_string(p, "menu_id").value_or("");
    return jsonutil::bool_node(true);
  }
  if (method == "tray.destroy") {
    if (auto id = jsonutil::object_get_string(p, "tray_id")) trays_.erase(*id);
    return jsonutil::bool_node(true);
  }

  if (method == "notification.show") return notification_show(p);
  if (method == "notification.close") {
    // libnotify has no stable close-by-id without keeping handles; no-op success
    return jsonutil::bool_node(true);
  }

  if (method == "icon.create") return icon_create(p);
  if (method == "icon.destroy") {
    if (auto id = jsonutil::object_get_string(p, "icon_id")) {
      auto it = icons_.find(*id);
      if (it != icons_.end()) {
        if (it->second.texture) g_object_unref(it->second.texture);
        icons_.erase(it);
      }
    }
    return jsonutil::bool_node(true);
  }

  if (method == "system.open_url") {
    if (auto url = jsonutil::object_get_string(p, "url")) open_external(*url);
    return jsonutil::bool_node(true);
  }
  if (method == "system.locale") {
    const gchar* lang = g_getenv("LANG");
    if (!lang || !*lang) lang = setlocale(LC_ALL, nullptr);
    std::string loc = lang ? lang : "en_US";
    // normalize to lowercase identifier-ish
    for (auto& c : loc) c = static_cast<char>(tolower(c));
    auto dot = loc.find('.');
    if (dot != std::string::npos) loc = loc.substr(0, dot);
    return jsonutil::string_node(loc);
  }
  if (method == "system.os_description") {
    struct utsname u {};
    uname(&u);
    std::string desc = std::string("Linux ") + u.release;
    return jsonutil::string_node(desc);
  }
  if (method == "system.set_permission_policy") {
    auto origin = jsonutil::object_get_string(p, "origin");
    if (!origin) throw HostError{-32602, "origin"};
    auto& map = permission_policy_[*origin];
    if (auto c = jsonutil::object_get_string(p, "camera")) map["camera"] = *c;
    if (auto m = jsonutil::object_get_string(p, "microphone")) map["microphone"] = *m;
    return jsonutil::bool_node(true);
  }

  throw HostError{-32601, "Method not found: " + method};
}

JsonNode* HostController::handle_test(const std::string& method, JsonNode* params, JsonNode* id,
                                      bool* async_reply) {
  *async_reply = false;
  if (method == "test.ping") {
    return jsonutil::rpc_ok(id, jsonutil::string_node("pong"));
  }
  if (method == "test.echo") {
    return jsonutil::rpc_ok(id, params ? json_node_copy(params) : jsonutil::null_node());
  }
  if (method == "test.capabilities") {
    JsonObject* caps = jsonutil::object_new();
    json_object_set_boolean_member(caps, "window", TRUE);
    json_object_set_boolean_member(caps, "webview", TRUE);
    json_object_set_boolean_member(caps, "menu", TRUE);
    json_object_set_boolean_member(caps, "tray", TRUE);
    json_object_set_boolean_member(caps, "notification", TRUE);
    json_object_set_boolean_member(caps, "permission", TRUE);
    json_object_set_boolean_member(caps, "media", TRUE);
    json_object_set_boolean_member(caps, "test_rpc", TRUE);
    JsonNode* n = json_node_alloc();
    json_node_init_object(n, caps);
    json_object_unref(caps);
    return jsonutil::rpc_ok(id, n);
  }
  if (method == "test.window.list") {
    std::vector<JsonNode*> items;
    for (auto& [_, w] : windows_) {
      JsonObject* o = jsonutil::object_new();
      json_object_set_string_member(o, "window_id", w->window_id().c_str());
      json_object_set_string_member(o, "webview_id", w->webview_id().c_str());
      const gchar* title = gtk_window_get_title(w->window());
      json_object_set_string_member(o, "title", title ? title : "");
      json_object_set_string_member(o, "url", w->current_url().c_str());
      JsonNode* n = json_node_alloc();
      json_node_init_object(n, o);
      json_object_unref(o);
      items.push_back(n);
    }
    return jsonutil::rpc_ok(id, jsonutil::array_node(items));
  }
  if (method == "test.webview.eval") {
    JsonObject* p = params_obj(params);
    auto wv = jsonutil::object_get_string(p, "webview_id");
    auto script = jsonutil::object_get_string(p, "script");
    if (!wv || !script) return jsonutil::rpc_error(id, -32002, "unknown webview");
    auto wit = webviews_.find(*wv);
    if (wit == webviews_.end()) return jsonutil::rpc_error(id, -32002, "unknown webview");
    auto it = windows_.find(wit->second);
    if (it == windows_.end()) return jsonutil::rpc_error(id, -32002, "unknown webview");
    *async_reply = true;
    JsonNode* id_keep = json_node_copy(id);
    it->second->eval_js(*script, [this, id_keep](JsonNode* result) {
      server_.send_node(jsonutil::rpc_ok(id_keep, result));
      json_node_free(id_keep);
    });
    return nullptr;
  }
  if (method == "test.permission.simulate") {
    JsonObject* p = params_obj(params);
    auto origin = jsonutil::object_get_string(p, "origin").value_or("http://127.0.0.1");
    auto type = jsonutil::object_get_string(p, "type").value_or("microphone");
    std::string wv;
    if (auto v = jsonutil::object_get_string(p, "webview_id"))
      wv = *v;
    else if (!windows_.empty())
      wv = windows_.begin()->second->webview_id();

    JsonObject* o = jsonutil::object_new();
    json_object_set_string_member(o, "origin", origin.c_str());
    json_object_set_string_member(o, "type", type.c_str());
    json_object_set_string_member(o, "webview_id", wv.c_str());
    JsonNode* req_params = json_node_alloc();
    json_node_init_object(req_params, o);
    json_object_unref(o);
    server_.request("permission.request", req_params, [](JsonNode*) {});
    return jsonutil::rpc_ok(id, jsonutil::bool_node(true));
  }
  if (method == "test.disconnect") {
    server_.close_connection();
    return jsonutil::rpc_ok(id, jsonutil::bool_node(true));
  }
  if (method == "test.crash") {
    _exit(2);
  }
  return jsonutil::rpc_error(id, -32601, "Unknown test method");
}
