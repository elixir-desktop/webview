# Status — Windows (WebView2)

States: `done` | `partial` | `todo` | `n/a`

C++ / Win32 / WebView2 host under [`native/windows/`](../../native/windows/).
Local/CI binary: `priv/native/windows/DesktopWebView.exe`.
Release asset: `DesktopWebView-windows-x64.exe` (GitHub Releases; not Hex `priv/`).

| Feature | Status | Notes |
|---------|--------|-------|
| TCP listen + `listening <port>` | done | E2E launcher discovery |
| `initialize` + capabilities | done | E2E setup |
| `--edw-no-beam` / argv strip | done | E2E |
| Ini + defaults | done | Same semantics as Linux/macOS |
| Lifetime reconnect | done | Packaged mode (keep listening) |
| Lifetime coupled | done | Exit on disconnect; `--edw-no-beam` exits |
| Window open/show/hide/title/min size | done | E2E window test |
| Multi-window (ids) | done | E2E |
| Close veto → Elixir | done | `WM_CLOSE` → `event.window.close_requested` |
| Raise / iconize / shown / active | done | Raise covered by E2E |
| Webview loadURL / reload / current URL | done | E2E |
| Webview rebuild | done | E2E |
| New window → external open | done | `ShellExecute` + event |
| Context menu disable | done | WebView2 settings |
| Menubar from DOM | done | E2E menu create |
| Tray / status item | done | `Shell_NotifyIcon`; E2E tray create |
| Apple menu | n/a | Successful no-op |
| Notifications | done | Balloon via tray when present; E2E |
| Icons from path / PNG | partial | Path/ICO via `LoadImage`; PNG→HICON deferred |
| OS events (reopen, open url/file) | partial | `system.open_url` done; OS reopen/file events not wired |
| Locale / os_description | done | E2E |
| Permission policy hybrid | done | E2E simulate + policy |
| Microphone in webview | done | Permission RPC + WebView2 kinds |
| Camera in webview | done | Permission RPC + WebView2 kinds |
| Native dialogs | done | IFileOpenDialog + Win32 prompt |
| Host-driven BEAM restart | done | `restart_beam` ini + process wait |
| Test RPC channel | done | E2E |
| Release artifact download | todo | Elixir fetch/cache still pending |
| Ad-hoc / CI signing | todo | Authenticode via desktop_deployment later |
