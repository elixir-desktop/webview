#pragma once

#include "win_prefix.hpp"

#include <optional>
#include <string>

// Small centered boot UI for packaged (host-first) Windows launches.
class SplashWindow {
 public:
  SplashWindow();
  ~SplashWindow();

  SplashWindow(const SplashWindow&) = delete;
  SplashWindow& operator=(const SplashWindow&) = delete;

  // icon_path: optional filesystem path (PNG/ICO). Empty/nullopt uses the
  // embedded PE/module icon so the host stays app-agnostic.
  void show(const std::wstring& status, const std::optional<std::wstring>& icon_path = std::nullopt);
  void set_status(const std::wstring& status);
  void dismiss();
  bool visible() const { return hwnd_ != nullptr; }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  static void register_class();
  LRESULT on_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  void paint(HWND hwnd);
  void load_icon(const std::optional<std::wstring>& icon_path);

  HWND hwnd_ = nullptr;
  HICON icon_ = nullptr;
  bool owns_icon_ = false;
  std::wstring status_;
};
