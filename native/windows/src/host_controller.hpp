#pragma once

#include "win_prefix.hpp"
#include "config.hpp"
#include "rpc_server.hpp"
#include "web_window.hpp"

#include <map>
#include <memory>
#include <string>

struct HostError {
  int code;
  std::string message;
};

struct MenuEntry {
  HMENU menu = nullptr;
  std::map<UINT, std::string> onclicks;  // command id -> onclick
};

struct TrayEntry {
  std::string icon_id;
  std::string menu_id;
  NOTIFYICONDATAW nid{};
  bool registered = false;
};

struct IconEntry {
  HICON icon = nullptr;
};

class HostController {
 public:
  explicit HostController(HostConfig config);
  ~HostController();

  bool start();
  RpcServer& server() { return server_; }
  HWND hwnd() const { return hwnd_; }

  static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  static constexpr UINT WM_EDW_REQUEST = WM_APP + 10;
  static constexpr UINT WM_EDW_DISCONNECT = WM_APP + 11;
  static constexpr UINT WM_EDW_TRAY = WM_APP + 12;

 private:
  void client_disconnected();
  void reset_session();
  void spawn_beam();
  std::string next_id(const std::string& prefix);

  void handle_request(jsonutil::Json id, const std::string& method, jsonutil::Json params,
                      RpcServer::ReplyFn reply);
  jsonutil::Json dispatch(const std::string& method, const jsonutil::Json& params);
  bool dispatch_ok(const std::string& method, const jsonutil::Json& params, jsonutil::Json* out_result,
                   int* err_code, std::string* err_msg);

  jsonutil::Json handle_test(const std::string& method, const jsonutil::Json& params,
                             const jsonutil::Json& id, bool* async_reply, RpcServer::ReplyFn reply);

  WebWindow* require_window(const jsonutil::Json& params);
  WebWindow* require_web(const jsonutil::Json& params);

  jsonutil::Json window_open(const jsonutil::Json& params);
  jsonutil::Json window_close(const jsonutil::Json& params);
  jsonutil::Json menu_create(const jsonutil::Json& params);
  jsonutil::Json menu_update(const jsonutil::Json& params);
  void build_menu_into(HMENU menu, const jsonutil::Json& dom, MenuEntry& entry);
  void append_menu_node(HMENU menu, const jsonutil::Json& node, MenuEntry& entry);
  jsonutil::Json tray_create(const jsonutil::Json& params);
  jsonutil::Json icon_create(const jsonutil::Json& params);
  jsonutil::Json notification_show(const jsonutil::Json& params);
  void update_tray_icon(TrayEntry& tray);
  void destroy_menu_entry(MenuEntry& entry);

  void open_external(const std::string& url);
  void handle_permission(const std::string& origin, const std::string& type,
                         const std::string& webview_id,
                         ICoreWebView2PermissionRequestedEventArgs* args);
  void wire_window(WebWindow* w);
  LRESULT on_host_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  bool on_menu_command(UINT cmd);

  HostConfig config_;
  RpcServer server_;
  HWND hwnd_ = nullptr;
  bool initialized_ = false;
  int id_counter_ = 0;
  UINT next_menu_cmd_ = 1000;
  std::map<std::string, std::unique_ptr<WebWindow>> windows_;
  std::map<std::string, std::string> webviews_;
  std::map<std::string, MenuEntry> menus_;
  std::map<std::string, TrayEntry> trays_;
  std::map<std::string, IconEntry> icons_;
  std::map<std::string, std::map<std::string, std::string>> permission_policy_;
  HANDLE beam_process_ = nullptr;
};
