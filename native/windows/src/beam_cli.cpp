#include "beam_cli.hpp"
#include "win_util.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace beamcli {
namespace {

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
  return is_absolute_path(*cfg.beam_path) ? *cfg.beam_path : join_path(root, *cfg.beam_path);
}

std::string resolved_working_dir(const HostConfig& cfg) {
  auto beam = resolved_beam_dir(cfg);
  if (!cfg.beam_working_dir || cfg.beam_working_dir->empty()) return beam;
  return is_absolute_path(*cfg.beam_working_dir) ? *cfg.beam_working_dir
                                                 : join_path(cfg.resources_root(), *cfg.beam_working_dir);
}

std::vector<std::string> list_dir(const std::string& dir) {
  std::vector<std::string> names;
  WIN32_FIND_DATAA fd{};
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return names;
  do {
    if (fd.cFileName[0] == '.') continue;
    names.emplace_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  std::sort(names.begin(), names.end());
  return names;
}

std::optional<std::string> first_dir_prefix(const std::string& dir, const std::string& prefix) {
  for (auto& n : list_dir(dir)) {
    if (n.rfind(prefix, 0) == 0) return n;
  }
  return std::nullopt;
}

std::optional<std::string> resolve_app_name(const HostConfig& cfg) {
  if (cfg.beam_app && !cfg.beam_app->empty()) return *cfg.beam_app;
  auto bin = join_path(resolved_beam_dir(cfg), "bin");
  std::string first_any;
  std::string first_bat;
  for (auto& name : list_dir(bin)) {
    if (first_any.empty()) first_any = name;
    auto lower = name;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (first_bat.empty() && lower.size() >= 4 &&
        (lower.substr(lower.size() - 4) == ".bat" || lower.substr(lower.size() - 4) == ".cmd")) {
      first_bat = name;
    }
  }
  if (!first_bat.empty()) return first_bat;
  if (!first_any.empty()) return first_any;
  return std::nullopt;
}

std::optional<std::string> resolve_recovery_script(const HostConfig& cfg) {
  if (!cfg.recovery_script || cfg.recovery_script->empty()) return std::nullopt;
  std::string path = is_absolute_path(*cfg.recovery_script)
                         ? *cfg.recovery_script
                         : join_path(cfg.resources_root(), *cfg.recovery_script);
  if (!file_exists(path)) return std::nullopt;
  return path;
}

std::string eval_file_expr(const std::string& script_path) {
  std::string posix = script_path;
  for (char& c : posix)
    if (c == '\\') c = '/';
  // ~s|...| avoids nested " so cmd.exe /s /c quoting stays intact.
  return "Code.eval_file(~s|" + posix + "|)";
}

std::string base64_encode(const std::string& in) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(tbl[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
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
  for (auto& n : list_dir(releases)) {
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
    auto p = join_path(join_path(join_path(beam_dir, *erts), "bin"), "erl_call.exe");
    if (file_exists(p)) return p;
    p = join_path(join_path(join_path(beam_dir, *erts), "bin"), "erl_call");
    if (file_exists(p)) return p;
  }
  auto lib = join_path(beam_dir, "lib");
  if (auto ei = first_dir_prefix(lib, "erl_interface-")) {
    auto p = join_path(join_path(join_path(lib, *ei), "bin"), "erl_call.exe");
    if (file_exists(p)) return p;
  }
  char buf[MAX_PATH];
  if (SearchPathA(nullptr, "erl_call.exe", nullptr, MAX_PATH, buf, nullptr)) return std::string(buf);
  if (SearchPathA(nullptr, "erl_call", nullptr, MAX_PATH, buf, nullptr)) return std::string(buf);
  return std::nullopt;
}

std::optional<std::string> find_cookie(const HostConfig& cfg, const std::string& beam_dir) {
  if (cfg.beam_cookie && !cfg.beam_cookie->empty()) return *cfg.beam_cookie;
  if (cfg.beam_cookie_file && !cfg.beam_cookie_file->empty()) {
    std::string path = is_absolute_path(*cfg.beam_cookie_file)
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

std::wstring env_block(const std::map<std::string, std::string>& extra) {
  std::map<std::string, std::string> env;
  LPWCH strings = GetEnvironmentStringsW();
  if (strings) {
    for (LPWCH p = strings; *p; p += wcslen(p) + 1) {
      std::string entry = wide_to_utf8(p);
      auto eq = entry.find('=');
      if (eq != std::string::npos) env[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    FreeEnvironmentStringsW(strings);
  }
  for (auto& [k, v] : extra) env[k] = v;
  std::wstring block;
  for (auto& [k, v] : env) {
    block += utf8_to_wide(k + "=" + v);
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

int spawn_cmd(const std::string& cmdline, const std::string& wd,
              const std::map<std::string, std::string>& extra, const std::string* stdin_data,
              bool new_console) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE stdin_r = nullptr, stdin_w = nullptr;
  if (stdin_data) {
    if (!CreatePipe(&stdin_r, &stdin_w, &sa, 0)) return 1;
    SetHandleInformation(stdin_w, HANDLE_FLAG_INHERIT, 0);
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  if (stdin_data) {
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = stdin_r;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  }
  PROCESS_INFORMATION pi{};
  std::wstring wcmd = utf8_to_wide(cmdline);
  std::vector<wchar_t> mutable_cmd(wcmd.begin(), wcmd.end());
  mutable_cmd.push_back(L'\0');
  std::wstring wwd = utf8_to_wide(wd);
  auto env = env_block(extra);
  DWORD flags = CREATE_UNICODE_ENVIRONMENT;
  flags |= new_console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
  if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, flags, env.data(),
                      wwd.empty() ? nullptr : wwd.c_str(), &si, &pi)) {
    fprintf(stderr, "edw: spawn failed (%lu): %s\n", GetLastError(), cmdline.c_str());
    if (stdin_r) CloseHandle(stdin_r);
    if (stdin_w) CloseHandle(stdin_w);
    return 1;
  }
  if (stdin_r) CloseHandle(stdin_r);
  if (stdin_data && stdin_w) {
    DWORD written = 0;
    WriteFile(stdin_w, stdin_data->data(), static_cast<DWORD>(stdin_data->size()), &written, nullptr);
    CloseHandle(stdin_w);
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}

bool is_batch(const std::string& script) {
  auto lower = script;
  for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return lower.size() >= 4 &&
         (lower.substr(lower.size() - 4) == ".bat" || lower.substr(lower.size() - 4) == ".cmd");
}

std::string resolve_bin_script(const HostConfig& cfg) {
  auto app = resolve_app_name(cfg);
  if (!app) return {};
  auto script = join_path(join_path(resolved_beam_dir(cfg), "bin"), *app);
  if (file_exists(script + ".bat")) return script + ".bat";
  if (file_exists(script + ".cmd")) return script + ".cmd";
  if (file_exists(script)) return script;
  return {};
}

std::string comspec_path() {
  char buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableA("COMSPEC", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return "cmd.exe";
  return std::string(buf, n);
}

}  // namespace

int run_recover(const HostConfig& cfg) {
  auto script_path = resolve_recovery_script(cfg);
  if (!script_path) {
    fprintf(stderr, "edw: recovery_script is missing or not a file\n");
    return 1;
  }
  auto bin = resolve_bin_script(cfg);
  if (bin.empty()) {
    fprintf(stderr, "edw: no beam app_name and no bin script found in %s\n",
            resolved_beam_dir(cfg).c_str());
    return 1;
  }
  auto expr = eval_file_expr(*script_path);
  char tmp_dir[MAX_PATH];
  char tmp_file[MAX_PATH];
  if (!GetTempPathA(MAX_PATH, tmp_dir) || !GetTempFileNameA(tmp_dir, "edw", 0, tmp_file)) {
    fprintf(stderr, "edw: failed to create recovery cmd file\n");
    return 1;
  }
  std::string cmd_path = std::string(tmp_file) + ".cmd";
  DeleteFileA(tmp_file);
  {
    std::ofstream out(cmd_path, std::ios::binary);
    if (!out) {
      fprintf(stderr, "edw: failed to write recovery cmd file\n");
      return 1;
    }
    out << "@echo off\r\n";
    if (is_batch(bin)) {
      out << "call \"" << bin << "\" eval \"" << expr << "\"\r\n";
    } else {
      out << '"' << bin << "\" eval \"" << expr << "\"\r\n";
    }
  }
  std::string cmdline = "\"" + comspec_path() + "\" /c \"" + cmd_path + "\"";
  int code = spawn_cmd(cmdline, resolved_working_dir(cfg), cfg.extra_env, nullptr, true);
  DeleteFileA(cmd_path.c_str());
  return code;
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
  char tmp_dir[MAX_PATH];
  char out_path[MAX_PATH];
  if (!GetTempPathA(MAX_PATH, tmp_dir) ||
      !GetTempFileNameA(tmp_dir, "edw", 0, out_path)) {
    fprintf(stderr, "edw: failed to create rpc output file\n");
    return 1;
  }
  std::string out_posix = out_path;
  for (char& c : out_posix)
    if (c == '\\') c = '/';
  std::string erlang = "Bin = base64:decode(<<\"" + b64 +
                       "\">>),\n{Val, _} = 'Elixir.Code':eval_string(Bin),\n"
                       "Inspect = 'Elixir.Kernel':inspect(Val),\n"
                       "ok = file:write_file(<<\"" + out_posix + "\">>, Inspect).\n";
  std::ostringstream cmd;
  cmd << '"' << *erl << "\" -c \"" << *cookie << "\" -r -no_result_term ";
  if (node->short_name)
    cmd << "-sname ";
  else
    cmd << "-name ";
  cmd << '"' << node->name << "\" -e";
  int code = spawn_cmd(cmd.str(), resolved_working_dir(cfg), cfg.extra_env, &erlang, false);
  if (code == 0) {
    std::ifstream in(out_path);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.empty()) {
      fprintf(stderr, "edw: erl_call succeeded but wrote no result file\n");
      DeleteFileA(out_path);
      return 1;
    }
    if (text.back() != '\n') text.push_back('\n');
    fwrite(text.data(), 1, text.size(), stdout);
    fflush(stdout);
    fwrite(text.data(), 1, text.size(), stderr);
    fflush(stderr);
  }
  DeleteFileA(out_path);
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
