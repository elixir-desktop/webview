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

## HTML file inputs and Explorer drops

Normal HTML file inputs use WebView2's built-in Windows file picker:

- `<input type="file">` selects one file by default.
- `<input type="file" multiple>` can select multiple files.
- If the user cancels, the input receives no new files and the page receives no new selection.
- No host C++ change, JSON-RPC call, CDP interception, or custom picker is needed. The page reads selected files through the normal HTML `input.files` API.

File-manager drops are also required. Preserve WebView2's normal drag handling
so Explorer file drops reach the page as a `drop` event with
`dataTransfer.files`. Do not replace page drops with `dialog.choose_file` or a
host-only drop handler.

The picker needs an interactive Windows desktop and a working Microsoft Edge
WebView2 Evergreen Runtime. It cannot show from a service, a headless run, or a
non-interactive session. Runtime or Windows security restrictions can also
prevent native UI from appearing, so test with a current runtime in a normal
desktop session. Test Explorer drops in the same session.

This browser feature is separate from the [`dialog.choose_file` JSON-RPC
method](../../docs/protocol.md#dialog). That method is an explicit host dialog
request for code that needs a path returned through the Elixir transport.

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
