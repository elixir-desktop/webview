# Porting a native host (Windows / Linux / …)

This document is the checklist for implementing a new `DesktopWebView` host.
The Elixir side is shared; only code under `native/<platform>/` changes.

Reference implementation: [`native/macos/`](../native/macos/) (Swift + WKWebView).
Do **not** copy macOS UI code into other platforms — share only the protocol.

## Required reading

1. [protocol.md](protocol.md) — wire format, methods, **behavioral semantics**
2. [packaging.md](packaging.md) — CLI, ini, layouts, release artifacts
3. [status/\<platform\>.md](status/macos.md) — feature matrix for your target
4. [AGENTS.md](../AGENTS.md) — repo rules (E2E-only testing, `--edw-*`, etc.)

## Non-negotiable contract

| Rule | Detail |
|------|--------|
| Roles | Native **listens**; Elixir **connects** |
| Framing | 4-byte big-endian length + UTF-8 JSON-RPC 2.0 |
| Discovery | Print exactly one line: `listening <port>` (stdout) |
| Flags | Parse/strip `--edw-*`; forward the rest to BEAM |
| Env for BEAM | Set `EDW_PORT`, `EDW_HOST` when spawning |
| Lifetime | Default `reconnect`; support `coupled` |
| Session reset | On client disconnect, host-owned BEAM exit, TCP replace, and `initialize`: destroy trays, windows, webviews, menus, icons, notifications, permission policy. Do not quit the host on TCP replace. |
| Tests | No native unit-test suite as source of truth — pass Elixir E2E |
| Status | Mark `docs/status/<platform>.md` rows `done` only when E2E covers them |

## Suggested implementation order

1. **Process shell** — argv (`--edw-*`), ini discovery, TCP listen, `listening <port>`, `--edw-no-beam`
2. **JSON-RPC loop** — length-prefixed frames; reject non-`initialize` until initialized
3. **`initialize`** — return `protocol_version: 1`, `platform`, `capabilities`
4. **One window + webview** — `window.open`, `webview.load_url` / `reload` / `current_url`
5. **Close veto** — native close → `event.window.close_requested`; do not destroy until Elixir says so
6. **Multi-window** — resource ids on one TCP connection
7. **Menus / tray / icons / notifications** — including a default `Edit`
   submenu (Undo, Redo, Cut, Copy, Paste, Delete, Select All) with the standard
   `Cmd+X/C/V/A` accelerators. Without it, web engines do not receive copy/paste
   keyboard events.
8. **Permissions + mic/camera** (hybrid policy)
9. **OS events** — reopen / open URL / open file where the OS supports them
10. **Packaged BEAM spawn** + **CI artifact** on tag draft releases
11. **Test RPC** behind `--edw-test-rpc`; run shared E2E

## HTML file chooser

`<input type="file">` is required on every platform. It is separate from
`dialog.choose_file`, which is an explicit Elixir RPC. Follow the semantics in
[protocol.md](protocol.md) and use the platform hook below.

| Platform | Hook | Required integration |
|----------|------|----------------------|
| macOS | `WKUIDelegate.webView(_:runOpenPanelWith:initiatedByFrame:completionHandler:)` | Map `WKOpenPanelParameters` to the native panel. Pass selected URLs to the completion handler, or `nil` on cancel. |
| Windows | WebView2's built-in file picker | Keep the WebView2 UI thread and message loop active. WebView2 has no native file-chooser event for this input; do not replace it with `dialog.choose_file` or CDP file injection. |
| Linux | WebKitGTK `run-file-chooser` default handler | Keep WebKitGTK's asynchronous default handler enabled, or provide an equivalent handler that completes the request with selected paths or cancellation. |

The shared E2E checks the fixture's DOM contract only. It cannot drive a native
picker or inject a `FileList`; selection, cancellation, multiple files, and
directory selection need manual checks until a supported platform test hook exists.

## Toolchain expectations

### Windows (`native/windows/`)

- Language: C++ or C# (team choice); UI via **Win32/WinUI + WebView2**
- SDK: [Microsoft Edge WebView2](https://developer.microsoft.com/microsoft-edge/webview2/) Evergreen Runtime (document bootstrapper needs)
- Build: MSVC; produce `DesktopWebView.exe` (x64 required; arm64 optional later)
- Artifact name (release): `DesktopWebView-windows-x64.exe` (+ `.sha256`)

### Linux (`native/linux/`)

- Language: C/C++ (or Rust if isolated to this directory)
- UI: **GTK 4 + WebKitGTK** (WebKitGTK 2.40+ recommended; document exact distro packages in the platform README)
- Build: produce ELF `DesktopWebView` (x86_64 required; aarch64 optional later)
- Artifact name (release): `DesktopWebView-linux-x86_64` (+ `.sha256`)
- Tray: prefer StatusNotifierItem / AppIndicator where available; document fallback

`menu.set_apple` is **`n/a`** on Windows and Linux — implement as a successful no-op (`true`) so Elixir can call it unconditionally.

## Binary resolver (Elixir)

Until download plumbing lands, local/CI sets:

```bash
export DESKTOP_WEBVIEW_BINARY=/path/to/DesktopWebView   # or .exe
```

Target layout after release automation:

| OS | Path / fetch |
|----|----------------|
| macOS | `priv/native/macos/DesktopWebView` (vendored) |
| Windows | GitHub Release asset `DesktopWebView-windows-x64.exe` → cache |
| Linux | GitHub Release asset `DesktopWebView-linux-x86_64` → cache |

## E2E conformance gate

Shared suite: `test/e2e/e2e_test.exs` (tag `:e2e`).

Host must be started with `--edw-no-beam --edw-test-rpc`.

Before flipping a status row to `done`, the corresponding E2E (or an added E2E) must pass on that OS. Minimum gate for calling a port “usable”:

| Area | Covered today by |
|------|------------------|
| RPC + test channel | `test.ping`, `test.echo` |
| Window + navigation | `window open load reload and list` |
| Multi-window | `multi-window` |
| Menu / tray / icon / notification | `menu create and notification` |
| Session reset | `session reset wipes tray and windows` |
| Permissions + JS eval | `permission policy and simulate` |
| HTML file input DOM contract | `HTML file input fixture exposes chooser semantics` |
| Locale / OS string | `system locale and os_description` |

Platform-specific asserts (e.g. `caps["platform"] == "macos"`) must be generalized when the second host lands — use `:os.type()` / host `initialize.platform`.

Add OS matrix jobs in `.github/workflows/ci.yml` when the binary builds; do not add stub jobs that always fail.

## Status matrix discipline

Update `docs/status/<platform>.md` as you go:

- `todo` — not started
- `partial` — implemented but no E2E (or known gaps)
- `done` — E2E green for that feature on this OS
- `n/a` — not applicable (e.g. Apple menu on Windows)

## PR expectations

- Keep all UI in `native/<platform>/`
- Extend Elixir only for binary download / OS detection if needed
- Update this doc if you discover a cross-platform semantic that was missing
- Link CI artifact names in [packaging.md](packaging.md) when adding release jobs
