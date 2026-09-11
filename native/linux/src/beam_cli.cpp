#include "beam_cli.hpp"

#include "instance_lock.hpp"

#include <glib.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace beamcli {
namespace {

std::string join_path(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

bool is_absolute(const std::string& p) { return !p.empty() && p[0] == '/'; }

bool file_exists(const std::string& path) { return g_file_test(path.c_str(), G_FILE_TEST_EXISTS); }

std::string resolved_beam_dir(const HostConfig& cfg) {
  auto root = cfg.resources_root();
  if (!cfg.beam_path || cfg.beam_path->empty()) return join_path(root, "beam");
  return is_absolute(*cfg.beam_path) ? *cfg.beam_path : join_path(root, *cfg.beam_path);
}

std::string resolved_working_dir(const HostConfig& cfg) {
  auto beam = resolved_beam_dir(cfg);
  if (!cfg.beam_working_dir || cfg.beam_working_dir->empty()) return beam;
  return is_absolute(*cfg.beam_working_dir) ? *cfg.beam_working_dir
                                            : join_path(cfg.resources_root(), *cfg.beam_working_dir);
}

std::optional<std::string> resolve_app_name(const HostConfig& cfg) {
  if (cfg.beam_app && !cfg.beam_app->empty()) return *cfg.beam_app;
  auto bin = join_path(resolved_beam_dir(cfg), "bin");
  GDir* gdir = g_dir_open(bin.c_str(), 0, nullptr);
  if (!gdir) return std::nullopt;
  std::optional<std::string> found;
  const gchar* name;
  while ((name = g_dir_read_name(gdir))) {
    if (name[0] == '.') continue;
    std::string n = name;
    if (n.size() >= 4 && (n.substr(n.size() - 4) == ".bat" || n.substr(n.size() - 4) == ".cmd"))
      continue;
    found = n;
    break;
  }
  g_dir_close(gdir);
  return found;
}

std::optional<std::string> resolve_recovery_script(const HostConfig& cfg) {
  if (!cfg.recovery_script || cfg.recovery_script->empty()) return std::nullopt;
  std::string path = is_absolute(*cfg.recovery_script)
                         ? *cfg.recovery_script
                         : join_path(cfg.resources_root(), *cfg.recovery_script);
  if (!g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) return std::nullopt;
  return path;
}

std::string eval_file_expr(const std::string& script_path) {
  std::string posix = script_path;
  for (char& c : posix)
    if (c == '\\') c = '/';
  std::string escaped;
  for (char c : posix) {
    if (c == '\\' || c == '"') escaped.push_back('\\');
    escaped.push_back(c);
  }
  return "Code.eval_file(\"" + escaped + "\")";
}

int spawn_argv(const std::vector<std::string>& argv, const std::string& wd,
               const std::map<std::string, std::string>& extra_env, const std::string* stdin_data) {
  std::vector<char*> cargv;
  for (auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
  cargv.push_back(nullptr);

  std::vector<std::string> env_store;
  for (char** e = environ; e && *e; ++e) env_store.emplace_back(*e);
  for (auto& [k, v] : extra_env) env_store.push_back(k + "=" + v);
  std::vector<char*> envp;
  for (auto& s : env_store) envp.push_back(s.data());
  envp.push_back(nullptr);

  GPid pid = 0;
  gint stdin_fd = -1;
  GError* err = nullptr;
  GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD;
  if (!g_spawn_async_with_pipes(wd.c_str(), cargv.data(), envp.data(), flags, nullptr, nullptr, &pid,
                                stdin_data ? &stdin_fd : nullptr, nullptr, nullptr, &err)) {
    fprintf(stderr, "edw: spawn failed: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    return 1;
  }
  if (stdin_data && stdin_fd >= 0) {
    std::string payload = *stdin_data;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
    const char* p = payload.data();
    size_t left = payload.size();
    while (left) {
      ssize_t n = write(stdin_fd, p, left);
      if (n <= 0) break;
      p += n;
      left -= static_cast<size_t>(n);
    }
    close(stdin_fd);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  g_spawn_close_pid(pid);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 1;
}

}  // namespace

int run_recover(const HostConfig& cfg) {
  auto script = resolve_recovery_script(cfg);
  if (!script) {
    fprintf(stderr, "edw: recovery_script is missing or not a file\n");
    return 1;
  }
  auto app = resolve_app_name(cfg);
  if (!app) {
    fprintf(stderr, "edw: no beam app_name and no bin script found in %s\n",
            resolved_beam_dir(cfg).c_str());
    return 1;
  }
  auto bin = join_path(join_path(resolved_beam_dir(cfg), "bin"), *app);
  if (!file_exists(bin)) {
    fprintf(stderr, "edw: beam script not found: %s\n", bin.c_str());
    return 1;
  }
  std::vector<std::string> argv{bin, "eval", eval_file_expr(*script)};
  return spawn_argv(argv, resolved_working_dir(cfg), cfg.extra_env, nullptr);
}

int run_rpc(const HostConfig& cfg, const std::string& expr) {
  if (cfg.instances != Instances::Single) {
    fprintf(stderr, "edw: --edw-rpc requires a running single-instance host\n");
    return 1;
  }
  std::string inspect;
  int code = InstanceLock::client_eval(cfg.resolved_instance_id(), expr, &inspect);
  if (code != 0) return code;
  if (inspect.empty() || inspect.back() != '\n') inspect.push_back('\n');
  fwrite(inspect.data(), 1, inspect.size(), stdout);
  fflush(stdout);
  fwrite(inspect.data(), 1, inspect.size(), stderr);
  fflush(stderr);
  return 0;
}

bool maybe_run_exclusive(const HostConfig& cfg, int* exit_code) {
  if (cfg.rpc_expr && cfg.recover) {
    fprintf(stderr, "edw: --edw-rpc and --edw-recover are mutually exclusive\n");
    *exit_code = 1;
    return true;
  }
  if (cfg.rpc_expr) {
    if (cfg.rpc_expr->empty()) {
      fprintf(stderr, "edw: --edw-rpc requires an Elixir expression\n");
      *exit_code = 1;
      return true;
    }
    *exit_code = run_rpc(cfg, *cfg.rpc_expr);
    return true;
  }
  if (cfg.recover) {
    *exit_code = run_recover(cfg);
    return true;
  }
  return false;
}

}  // namespace beamcli
