# Windows host (WebView2)

C++17 + **Win32** + **WebView2** executable `DesktopWebView.exe`.

## Prerequisites

- Visual Studio 2022 with MSVC x64 toolchain (`vcvars64.bat`)
- CMake 3.16+
- [Microsoft Edge WebView2 Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)
  (the host prints a clear stderr error if the runtime is missing)

CMake fetches `Microsoft.Web.WebView2` and `nlohmann/json` during configure.

## Build

```powershell
# from repo root (x64 Developer PowerShell, or let the script call vcvars64)
.\scripts\build_windows.ps1
```

Output is copied to `priv\native\windows\DesktopWebView.exe` for local/CI use.
Release artifact name: `DesktopWebView-windows-x64.exe` (see [packaging.md](../../docs/packaging.md)).

## Run (dev / E2E)

```powershell
.\priv\native\windows\DesktopWebView.exe --edw-no-beam --edw-test-rpc --edw-port=0
# prints: listening <port>
```

```powershell
$env:DESKTOP_WEBVIEW_BINARY = "$PWD\priv\native\windows\DesktopWebView.exe"
mix test.e2e
```

## Structure

| Path | Role |
|------|------|
| `CMakeLists.txt` | CMake project + WebView2 / JSON deps |
| `src/` | Host sources (config, RPC, windows, dispatch) |

Do not add a native unit-test suite; exercise the binary from Elixir E2E (`mix test.e2e`).

`menu.set_apple` is a successful no-op on Windows.

Microphone / camera: hybrid permission RPC applies; packaged apps may also need
Windows privacy capability declarations (see [packaging.md](../../docs/packaging.md)).

## Specs

- [Porting guide](../../docs/porting.md)
- [Protocol](../../docs/protocol.md)
- [Packaging](../../docs/packaging.md)
- [Status](../../docs/status/windows.md)
