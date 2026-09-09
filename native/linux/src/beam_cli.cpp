#include "beam_cli.hpp"

#include <glib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

std::string read_trimmed(const std::string& path) {
  std::ifstream in(path);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string s = ss.str();
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  return s.substr(i);
}

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

std::optional<std::string> first_dir_prefix(const std::string& dir, const std::string& prefix) {
  GDir* gdir = g_dir_open(dir.c_str(), 0, nullptr);
  if (!gdir) return std::nullopt;
  std::vector<std::string> names;
  const gchar* name;
  while ((name = g_dir_read_name(gdir))) {
    if (std::strncmp(name, prefix.c_str(), prefix.size()) == 0) names.emplace_back(name);
  }
  g_dir_close(gdir);
  if (names.empty()) return std::nullopt;
  std::sort(names.begin(), names.end());
  return names.front();
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
    if (n.size() >= 4 && (n.substr(n.size() - 4) == ".bat" || n.substr(n.size() - 4) == ".cmd")) continue;
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

std::string base64_encode(const std::string& in) {
  gchar* enc = g_base64_encode(reinterpret_cast<const guchar*>(in.data()), in.size());
  std::string out = enc ? enc : "";
  g_free(enc);
  return out;
}

struct NodeSpec {
  std::string name;
  bool short_name = false;
};

struct VmArgs {
  std::optional<NodeSpec> node;
  std::optional<std::string> cookie;
};

std::optional<std::string> vm_args_path(const std::string& beam_dir) {
  auto releases = join_path(beam_dir, "releases");
  auto start_erl = read_trimmed(join_path(releases, "start_erl.data"));
  if (!start_erl.empty()) {
    std::istringstream ss(start_erl);
    std::string erts, vsn;
    if (ss >> erts >> vsn) {
      auto p = join_path(join_path(releases, vsn), "vm.args");
      if (file_exists(p)) return p;
    }
  }
  GDir* gdir = g_dir_open(releases.c_str(), 0, nullptr);
  if (!gdir) return std::nullopt;
  std::vector<std::string> names;
  const gchar* name;
  while ((name = g_dir_read_name(gdir))) {
    if (name[0] == '.') continue;
    names.emplace_back(name);
  }
  g_dir_close(gdir);
  std::sort(names.begin(), names.end());
  for (auto& n : names) {
    auto p = join_path(join_path(releases, n), "vm.args");
    if (file_exists(p)) return p;
  }
  return std::nullopt;
}

VmArgs parse_vm_args(const std::string& path) {
  VmArgs out;
  std::ifstream in(path);
  if (!in) return out;
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= line.size() || line[i] == '#') continue;
    std::istringstream ss(line.substr(i));
    std::string tok;
    std::vector<std::string> toks;
    while (ss >> tok) toks.push_back(tok);
    for (size_t t = 0; t + 1 < toks.size(); t++) {
      if (toks[t] == "-sname")
        out.node = NodeSpec{toks[t + 1], true};
      else if (toks[t] == "-name")
        out.node = NodeSpec{toks[t + 1], false};
      else if (toks[t] == "-setcookie")
        out.cookie = toks[t + 1];
    }
  }
  return out;
}

std::optional<std::string> find_erl_call(const std::string& beam_dir) {
  if (auto erts = first_dir_prefix(beam_dir, "erts-")) {
    auto p = join_path(join_path(beam_dir, *erts), "bin/erl_call");
    if (g_file_test(p.c_str(), G_FILE_TEST_IS_EXECUTABLE)) return p;
  }
  auto lib = join_path(beam_dir, "lib");
  if (auto ei = first_dir_prefix(lib, "erl_interface-")) {
    auto p = join_path(join_path(lib, *ei), "bin/erl_call");
    if (g_file_test(p.c_str(), G_FILE_TEST_IS_EXECUTABLE)) return p;
  }
  gchar* found = g_find_program_in_path("erl_call");
  if (found) {
    std::string p = found;
    g_free(found);
    return p;
  }
  return std::nullopt;
}

std::optional<std::string> find_cookie(const HostConfig& cfg, const std::string& beam_dir) {
  if (cfg.beam_cookie && !cfg.beam_cookie->empty()) return *cfg.beam_cookie;
  if (cfg.beam_cookie_file && !cfg.beam_cookie_file->empty()) {
    std::string path = is_absolute(*cfg.beam_cookie_file)
                           ? *cfg.beam_cookie_file
                           : join_path(cfg.resources_root(), *cfg.beam_cookie_file);
    auto t = read_trimmed(path);
    if (!t.empty()) return t;
  }
  auto from_file = read_trimmed(join_path(join_path(beam_dir, "releases"), "COOKIE"));
  if (!from_file.empty()) return from_file;
  if (auto vm = vm_args_path(beam_dir)) {
    auto parsed = parse_vm_args(*vm);
    if (parsed.cookie) return parsed.cookie;
  }
  return std::nullopt;
}

std::optional<NodeSpec> find_node(const HostConfig& cfg, const std::string& beam_dir) {
  if (cfg.beam_node && !cfg.beam_node->empty()) {
    bool short_name = cfg.beam_node->find('@') == std::string::npos;
    return NodeSpec{*cfg.beam_node, short_name};
  }
  if (auto vm = vm_args_path(beam_dir)) {
    auto parsed = parse_vm_args(*vm);
    if (parsed.node) return parsed.node;
  }
  return std::nullopt;
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
  auto beam_dir = resolved_beam_dir(cfg);
  auto erl = find_erl_call(beam_dir);
  if (!erl) {
    fprintf(stderr, "edw: erl_call not found under %s or PATH\n", beam_dir.c_str());
    return 1;
  }
  auto cookie = find_cookie(cfg, beam_dir);
  if (!cookie) {
    fprintf(stderr, "edw: cookie not found (ini cookie/cookie_file, releases/COOKIE, or vm.args)\n");
    return 1;
  }
  auto node = find_node(cfg, beam_dir);
  if (!node) {
    fprintf(stderr, "edw: node not found (ini [beam] node or vm.args -name/-sname)\n");
    return 1;
  }
  auto b64 = base64_encode(expr);
  char out_path[] = "/tmp/edw-rpc-out-XXXXXX";
  int out_fd = mkstemp(out_path);
  if (out_fd < 0) {
    fprintf(stderr, "edw: failed to create rpc output file\n");
    return 1;
  }
  close(out_fd);
  std::string erlang = "Bin = base64:decode(<<\"" + b64 +
                       "\">>),\n{Val, _} = 'Elixir.Code':eval_string(Bin),\n"
                       "Inspect = 'Elixir.Kernel':inspect(Val),\n"
                       "ok = file:write_file(<<\"" +
                       std::string(out_path) + "\">>, Inspect).\n";
  std::vector<std::string> argv{*erl, "-c", *cookie, "-r", "-no_result_term"};
  if (node->short_name) {
    argv.push_back("-sname");
  } else {
    argv.push_back("-name");
  }
  argv.push_back(node->name);
  argv.push_back("-e");
  int code = spawn_argv(argv, resolved_working_dir(cfg), cfg.extra_env, &erlang);
  if (code == 0) {
    std::ifstream in(out_path);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.empty()) {
      fprintf(stderr, "edw: erl_call succeeded but wrote no result file\n");
      unlink(out_path);
      return 1;
    }
    if (text.back() != '\n') text.push_back('\n');
    fwrite(text.data(), 1, text.size(), stdout);
    fflush(stdout);
    fwrite(text.data(), 1, text.size(), stderr);
    fflush(stderr);
  }
  unlink(out_path);
  return code;
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
