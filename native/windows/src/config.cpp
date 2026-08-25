#include "config.hpp"
#include "win_util.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

class Ini {
 public:
  static Ini parse(const std::string& text) {
    Ini ini;
    std::string current = "default";
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
      std::string line = raw;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
      size_t start = 0;
      while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) start++;
      line = line.substr(start);
      if (line.empty() || line[0] == '#' || line[0] == ';') continue;
      if (line.front() == '[' && line.back() == ']') {
        current = line.substr(1, line.size() - 2);
        for (auto& c : current) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        continue;
      }
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string key = line.substr(0, eq);
      std::string val = line.substr(eq + 1);
      auto trim = [](std::string& s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
      };
      trim(key);
      trim(val);
      ini.sections_[current][key] = val;
    }
    return ini;
  }

  std::optional<std::string> get(const std::string& section, const std::string& key) const {
    std::string sec = section;
    for (auto& c : sec) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    auto it = sections_.find(sec);
    if (it == sections_.end()) return std::nullopt;
    auto kit = it->second.find(key);
    if (kit == it->second.end()) return std::nullopt;
    return kit->second;
  }

  std::map<std::string, std::string> section(const std::string& name) const {
    std::string sec = name;
    for (auto& c : sec) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    auto it = sections_.find(sec);
    if (it == sections_.end()) return {};
    return it->second;
  }

 private:
  std::map<std::string, std::map<std::string, std::string>> sections_;
};

}  // namespace

HostConfig HostConfig::parse(int argc, char** argv) {
  HostConfig cfg;
  bool passthrough = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (passthrough || a == "--") {
      if (a == "--") {
        passthrough = true;
        continue;
      }
      cfg.forwarded_argv.push_back(a);
      continue;
    }
    if (a.rfind("--edw-", 0) == 0) {
      std::string body = a.substr(6);
      if (body == "no-beam") {
        cfg.no_beam = true;
      } else if (body == "test-rpc") {
        cfg.test_rpc = true;
      } else if (body.rfind("port=", 0) == 0) {
        cfg.port = static_cast<uint16_t>(std::stoi(body.substr(5)));
      } else if (body.rfind("host=", 0) == 0) {
        cfg.host = body.substr(5);
      } else if (body.rfind("config=", 0) == 0) {
        cfg.config_path = body.substr(7);
      } else if (body.rfind("lifetime=", 0) == 0) {
        auto v = body.substr(9);
        cfg.lifetime = (v == "coupled") ? Lifetime::Coupled : Lifetime::Reconnect;
      } else if (body.rfind("beam-path=", 0) == 0) {
        cfg.beam_path = body.substr(10);
      } else if (body.rfind("beam-app=", 0) == 0) {
        cfg.beam_app = body.substr(9);
      } else {
        fprintf(stderr, "unknown --edw flag: %s\n", a.c_str());
      }
      continue;
    }
    cfg.forwarded_argv.push_back(a);
  }
  cfg.apply_ini();
  if (cfg.no_beam) cfg.beam_enabled = false;
  return cfg;
}

std::string HostConfig::resources_root() const {
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return dirname_of(buf);
  return ".";
}

std::optional<std::string> HostConfig::resolve_ini_path() const {
  if (config_path) return *config_path;
  auto beside = join_path(resources_root(), "DesktopWebView.ini");
  if (file_exists(beside)) return beside;
  return std::nullopt;
}

void HostConfig::apply_ini() {
  auto path = resolve_ini_path();
  if (!path) return;
  std::ifstream in(*path);
  if (!in) return;
  std::ostringstream ss;
  ss << in.rdbuf();
  auto ini = Ini::parse(ss.str());
  if (auto v = ini.get("network", "host")) host = *v;
  if (auto v = ini.get("network", "port")) port = static_cast<uint16_t>(std::stoi(*v));
  if (auto v = ini.get("lifetime", "mode")) {
    lifetime = (*v == "coupled") ? Lifetime::Coupled : Lifetime::Reconnect;
  }
  if (auto v = ini.get("beam", "enabled")) {
    beam_enabled = !(*v == "false" || *v == "0");
  }
  if (auto v = ini.get("beam", "path")) beam_path = *v;
  if (auto v = ini.get("beam", "app_name")) beam_app = *v;
  if (auto v = ini.get("beam", "args")) {
    beam_args.clear();
    std::istringstream args(*v);
    std::string tok;
    while (args >> tok) beam_args.push_back(tok);
  }
  if (auto v = ini.get("beam", "working_dir")) beam_working_dir = *v;
  if (auto v = ini.get("lifetime", "restart_beam")) {
    restart_beam = !(*v == "false" || *v == "0");
  }
  if (auto v = ini.get("lifetime", "restart_max_attempts")) {
    restart_max_attempts = std::stoi(*v);
  }
  if (auto v = ini.get("lifetime", "restart_backoff_ms")) {
    restart_backoff_ms = static_cast<uint32_t>(std::stoul(*v));
  }
  for (auto& [k, v] : ini.section("env")) {
    extra_env[k] = v;
  }
}
