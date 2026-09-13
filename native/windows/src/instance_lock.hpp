#pragma once

#include "win_prefix.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class InstanceLock {
 public:
  using ActivateFn = std::function<void(const std::vector<std::string>& argv)>;
  using EvalDone = std::function<void(bool ok, std::string inspect_or_err)>;
  using EvalFn = std::function<void(const std::string& expr, EvalDone done)>;

  InstanceLock() = default;
  ~InstanceLock();

  InstanceLock(const InstanceLock&) = delete;
  InstanceLock& operator=(const InstanceLock&) = delete;

  bool try_serve(const std::string& instance_id);
  void set_handlers(ActivateFn activate, EvalFn eval);
  void start();

  static int client_activate(const std::string& instance_id,
                             const std::vector<std::string>& argv);
  static int client_eval(const std::string& instance_id, const std::string& expr,
                         std::string* inspect_out);

  static std::wstring pipe_name(const std::string& instance_id);
  static std::wstring mutex_name(const std::string& instance_id);
  static std::string sanitize_id(const std::string& raw);

 private:
  void accept_loop();
  void handle_client(HANDLE pipe);

  HANDLE mutex_ = nullptr;
  HANDLE stop_event_ = nullptr;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::string instance_id_;
  ActivateFn activate_;
  EvalFn eval_;
};
