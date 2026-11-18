#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

enum class Lifetime { Reconnect, Coupled };
enum class Instances { Multi, Single };

struct HostConfig {
  bool no_beam = false;
  uint16_t port = 0;
  std::string host = "127.0.0.1";
  std::optional<std::string> config_path;
  Lifetime lifetime = Lifetime::Reconnect;
  bool test_rpc = false;
  std::optional<std::string> beam_path;
  std::optional<std::string> beam_app;
  std::vector<std::string> beam_args{"start"};
  std::optional<std::string> beam_working_dir;
  bool beam_enabled = true;
  // Host-driven BEAM restart (packaged host-first; replaces heart).
  bool restart_beam = true;
  int restart_max_attempts = 0;
  uint32_t restart_backoff_ms = 500;
  std::optional<std::string> rpc_expr;
  bool recover = false;
  std::optional<std::string> recovery_script;
  int recovery_after = 3;
  std::optional<std::string> beam_node;
  std::optional<std::string> beam_cookie;
  std::optional<std::string> beam_cookie_file;
  std::map<std::string, std::string> extra_env;
  std::vector<std::string> forwarded_argv;
  Instances instances = Instances::Multi;
  std::optional<std::string> instance_id;
  // Optional path to a splash logo (PNG/ICO). Relative paths resolve against
  // resources_root(). When unset, the splash falls back to the PE/module icon.
  std::optional<std::string> splash_icon;
  // Shared with BEAM (EDW_APP_USER_MODEL_ID) so Task Manager groups both processes.
  // From ini [windows] app_user_model_id or --edw-app-user-model-id=.
  std::optional<std::string> app_user_model_id;

  static HostConfig parse(int argc, char** argv);

  std::string resources_root() const;
  std::optional<std::string> resolve_ini_path() const;
  std::string resolved_instance_id() const;
  std::string exe_basename() const;

 private:
  void apply_ini();
};
