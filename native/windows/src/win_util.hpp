#pragma once

#include "win_prefix.hpp"

#include <gdiplus.h>

#include <cctype>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <string>

inline std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  return out;
}

inline std::string wide_to_utf8(const wchar_t* s) {
  if (!s || !*s) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  return out;
}

inline std::string dirname_of(const std::string& path) {
  auto pos = path.find_last_of("/\\");
  if (pos == std::string::npos) return ".";
  if (pos == 0) return path.substr(0, 1);
  return path.substr(0, pos);
}

inline std::string join_path(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  char last = a.back();
  if (last == '/' || last == '\\') return a + b;
  return a + "\\" + b;
}

inline bool file_exists(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool is_absolute_path(const std::string& p) {
  if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') return true;
  return p.size() >= 2 && p[0] == '\\' && p[1] == '\\';
}

inline bool ends_with_ignore_case(const std::wstring& s, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) {
    wchar_t a = towlower(s[s.size() - n + i]);
    wchar_t b = towlower(suffix[i]);
    if (a != b) return false;
  }
  return true;
}

// LoadImage(IMAGE_ICON) only accepts .ico. App icons are often PNG (e.g. diode.png);
// use GDI+ so window/taskbar icons are not left blank.
inline HICON load_hicon_from_file(const std::wstring& path) {
  if (path.empty()) return nullptr;

  HICON icon = static_cast<HICON>(
      LoadImageW(nullptr, path.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
  if (icon) return icon;

  if (!ends_with_ignore_case(path, L".png") && !ends_with_ignore_case(path, L".jpg") &&
      !ends_with_ignore_case(path, L".jpeg") && !ends_with_ignore_case(path, L".bmp") &&
      !ends_with_ignore_case(path, L".gif")) {
    return nullptr;
  }

  static ULONG_PTR gdiplus_token = 0;
  static bool gdiplus_ready = false;
  if (!gdiplus_ready) {
    Gdiplus::GdiplusStartupInput input;
    gdiplus_ready = Gdiplus::GdiplusStartup(&gdiplus_token, &input, nullptr) == Gdiplus::Ok;
  }
  if (!gdiplus_ready) return nullptr;

  Gdiplus::Bitmap bitmap(path.c_str());
  if (bitmap.GetLastStatus() != Gdiplus::Ok) return nullptr;
  HICON from_png = nullptr;
  if (bitmap.GetHICON(&from_png) != Gdiplus::Ok) return nullptr;
  return from_png;
}

inline HICON extract_module_icon() {
  wchar_t module[MAX_PATH]{};
  HINSTANCE inst = GetModuleHandleW(nullptr);
  if (!GetModuleFileNameW(inst, module, MAX_PATH)) return nullptr;
  HICON icon = ExtractIconW(inst, module, 0);
  // ExtractIcon returns 1 when the file has no icons.
  if (!icon || icon == reinterpret_cast<HICON>(static_cast<uintptr_t>(1))) return nullptr;
  return icon;
}
