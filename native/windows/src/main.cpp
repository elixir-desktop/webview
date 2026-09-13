#include "beam_cli.hpp"
#include "config.hpp"
#include "host_controller.hpp"
#include "instance_lock.hpp"
#include "web_window.hpp"
#include "win_util.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>


namespace {

std::vector<std::string> argv_utf8() {
  int argc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> out;
  if (!wargv) return out;
  for (int i = 0; i < argc; i++) out.push_back(wide_to_utf8(wargv[i]));
  LocalFree(wargv);
  return out;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) {
    fprintf(stderr, "edw: CoInitializeEx failed\n");
    return 1;
  }

  auto args = argv_utf8();
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args.size());
  for (auto& s : args) argv_ptrs.push_back(s.data());

  auto config = HostConfig::parse(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
  int exclusive = 0;
  if (beamcli::maybe_run_exclusive(config, &exclusive)) {
    CoUninitialize();
    return exclusive;
  }

  std::unique_ptr<InstanceLock> lock;
  if (config.instances == Instances::Single) {
    lock = std::make_unique<InstanceLock>();
    if (!lock->try_serve(config.resolved_instance_id())) {
      int code = InstanceLock::client_activate(config.resolved_instance_id(), config.forwarded_argv);
      CoUninitialize();
      return code;
    }
  }

  WebWindow::register_class();

  auto host = std::make_unique<HostController>(std::move(config));
  if (lock) {
    HostController* h = host.get();
    lock->set_handlers([h](const std::vector<std::string>& argv) { h->activate_from_argv(argv); },
                       [h](const std::string& expr, InstanceLock::EvalDone done) {
                         h->eval_rpc(expr, std::move(done));
                       });
    lock->start();
  }
  if (!host->start()) {
    fprintf(stderr, "failed to start host\n");
    CoUninitialize();
    return 1;
  }

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  CoUninitialize();
  return msg.message == WM_QUIT ? static_cast<int>(msg.wParam) : 0;
}
