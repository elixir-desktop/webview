#include "config.hpp"
#include "host_controller.hpp"
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
  WebWindow::register_class();

  auto host = std::make_unique<HostController>(std::move(config));
  if (!host->start()) {
    fprintf(stderr, "failed to start host\n");
    CoUninitialize();
    return 1;
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  CoUninitialize();
  return 0;
}
