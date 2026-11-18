#include "splash_window.hpp"
#include "win_util.hpp"

namespace {
constexpr const wchar_t* kSplashClass = L"DesktopWebViewSplash";
constexpr int kSplashWidth = 360;
constexpr int kSplashHeight = 240;
constexpr int kIconSize = 128;
constexpr COLORREF kSplashBg = RGB(220, 220, 220);
bool g_class_registered = false;
HBRUSH g_bg_brush = nullptr;

HBRUSH splash_bg_brush() {
  if (!g_bg_brush) g_bg_brush = CreateSolidBrush(kSplashBg);
  return g_bg_brush;
}
}  // namespace

void SplashWindow::register_class() {
  if (g_class_registered) return;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  // CS_DROPSHADOW: system drop shadow on borderless WS_POPUP (DWM composition).
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = SplashWindow::WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = splash_bg_brush();
  wc.lpszClassName = kSplashClass;
  wc.hIcon = extract_module_icon();
  wc.hIconSm = extract_module_icon();
  RegisterClassExW(&wc);
  g_class_registered = true;
}

SplashWindow::SplashWindow() = default;

SplashWindow::~SplashWindow() { dismiss(); }

void SplashWindow::load_icon(const std::optional<std::wstring>& icon_path) {
  if (icon_ && owns_icon_) DestroyIcon(icon_);
  icon_ = nullptr;
  owns_icon_ = false;

  if (icon_path && !icon_path->empty()) {
    icon_ = load_hicon_from_file(*icon_path);
    if (icon_) {
      owns_icon_ = true;
      return;
    }
  }
  icon_ = extract_module_icon();
  owns_icon_ = icon_ != nullptr;
}

void SplashWindow::show(const std::wstring& status, const std::optional<std::wstring>& icon_path) {
  status_ = status;
  if (hwnd_) {
    if (icon_path) load_icon(icon_path);
    InvalidateRect(hwnd_, nullptr, TRUE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return;
  }

  register_class();
  load_icon(icon_path);

  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int x = work.left + ((work.right - work.left) - kSplashWidth) / 2;
  int y = work.top + ((work.bottom - work.top) - kSplashHeight) / 2;

  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kSplashClass, L"", WS_POPUP, x, y,
                          kSplashWidth, kSplashHeight, nullptr, nullptr, GetModuleHandleW(nullptr),
                          this);
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);
}

void SplashWindow::set_status(const std::wstring& status) {
  status_ = status;
  if (!hwnd_) return;
  InvalidateRect(hwnd_, nullptr, TRUE);
  UpdateWindow(hwnd_);
}

void SplashWindow::dismiss() {
  if (hwnd_) {
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (icon_ && owns_icon_) DestroyIcon(icon_);
  icon_ = nullptr;
  owns_icon_ = false;
}

LRESULT CALLBACK SplashWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  SplashWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    self = static_cast<SplashWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<SplashWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
  return self->on_message(hwnd, msg, wParam, lParam);
}

LRESULT SplashWindow::on_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_PAINT:
      paint(hwnd);
      return 0;
    case WM_CLOSE:
      // Splash is not user-dismissible; host owns lifetime.
      return 0;
    case WM_DESTROY:
      hwnd_ = nullptr;
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

void SplashWindow::paint(HWND hwnd) {
  PAINTSTRUCT ps{};
  HDC hdc = BeginPaint(hwnd, &ps);

  RECT client{};
  GetClientRect(hwnd, &client);
  FillRect(hdc, &client, splash_bg_brush());

  if (icon_) {
    int icon_x = (client.right - kIconSize) / 2;
    int icon_y = 28;
    DrawIconEx(hdc, icon_x, icon_y, icon_, kIconSize, kIconSize, 0, nullptr, DI_NORMAL);
  }

  RECT text_rc{16, 28 + kIconSize + 16, client.right - 16, client.bottom - 16};
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(32, 32, 32));
  DrawTextW(hdc, status_.c_str(), static_cast<int>(status_.size()), &text_rc,
            DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

  EndPaint(hwnd, &ps);
}
