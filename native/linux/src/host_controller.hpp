#pragma once

#include "config.hpp"
#include "rpc_server.hpp"
#include "web_window.hpp"

#include <gtk/gtk.h>
#include <libnotify/notify.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

struct HostError {
  int code;
  std::string message;
};

struct MenuEntry {
  GMenu* menu = nullptr;
  // action name -> onclick string
  std::map<std::string, std::string> onclicks;
  std::string action_group_name;
  GSimpleActionGroup* action_group = nullptr;
};

struct TrayEntry {
  std::string icon_id;
  std::string menu_id;
};

struct IconEntry {
  std::string path;
  std::vector<uint8_t> png;
  GdkTexture* texture = nullptr;
};

class HostController {
 public:
  explicit HostController(HostConfig config);
  ~HostController();

  bool start();
  RpcServer& server() { return server_; }

 private:
  void client_disconnected();
  void spawn_beam();
  std::string next_id(const std::string& prefix);

  void handle_request(JsonNode* id, const std::string& method, JsonNode* params,
                      RpcServer::ReplyFn reply);
  JsonNode* dispatch(const std::string& method, JsonNode* params);  // may throw HostError via optional
  bool dispatch_ok(const std::string& method, JsonNode* params, JsonNode** out_result,
                   int* err_code, std::string* err_msg);

  JsonNode* handle_test(const std::string& method, JsonNode* params, JsonNode* id,
                        bool* async_reply);

  WebWindow* require_window(JsonObject* params);
  WebWindow* require_web(JsonObject* params);

  JsonNode* window_open(JsonObject* params);
  JsonNode* window_close(JsonObject* params);
  JsonNode* menu_create(JsonObject* params);
  JsonNode* menu_update(JsonObject* params);
  void build_menu_into(GMenu* menu, JsonNode* dom, MenuEntry& entry, const std::string& menu_id);
  void append_menu_node(GMenu* menu, JsonNode* node, MenuEntry& entry, const std::string& menu_id);
  JsonNode* tray_create(JsonObject* params);
  JsonNode* icon_create(JsonObject* params);
  JsonNode* notification_show(JsonObject* params);

  void open_external(const std::string& url);
  void handle_permission(const std::string& origin, const std::string& type,
                         const std::string& webview_id, WebKitPermissionRequest* request);
  void wire_window(WebWindow* w);

  HostConfig config_;
  RpcServer server_;
  bool initialized_ = false;
  int id_counter_ = 0;
  std::map<std::string, std::unique_ptr<WebWindow>> windows_;
  std::map<std::string, std::string> webviews_;  // webview_id -> window_id
  std::map<std::string, MenuEntry> menus_;
  std::map<std::string, TrayEntry> trays_;
  std::map<std::string, IconEntry> icons_;
  std::map<std::string, std::map<std::string, std::string>> permission_policy_;
  GPid beam_pid_ = 0;
  GtkApplication* app_ = nullptr;
};
