#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

enum class Lifetime { Reconnect, Coupled };

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
  std::map<std::string, std::string> extra_env;
  std::vector<std::string> forwarded_argv;
  // Host-driven BEAM restart policy (replaces heart on host-first bundles).
  bool restart_beam = true;
  int restart_max_attempts = 0;
  uint32_t restart_backoff_ms = 500;

  static HostConfig parse(int argc, char** argv);

  std::string resources_root() const;
  std::optional<std::string> resolve_ini_path() const;

 private:
  void apply_ini();
};
