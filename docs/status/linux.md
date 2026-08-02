# Status — Linux (WebKitGTK)

States: `done` | `partial` | `todo` | `n/a`

Host: GTK 4 + WebKitGTK 6 (`native/linux/`). Binary delivery via GitHub Releases
(not Hex `priv/`); local/CI uses `./scripts/build_linux.sh` →
`priv/native/linux/DesktopWebView` or `DESKTOP_WEBVIEW_BINARY`.

| Feature | Status | Notes |
|---------|--------|-------|
| TCP listen + `listening <port>` | done | |
| `initialize` + capabilities | done | `platform: "linux"` |
| `--edw-no-beam` / argv strip | done | |
| Ini + defaults | done | beside executable |
| Lifetime reconnect | done | default |
| Lifetime coupled | done | |
| Window open/show/hide/title/min size | done | |
| Multi-window (ids) | done | |
| Close veto → Elixir | done | |
| Raise / iconize / shown / active | done | |
| Webview loadURL / reload / current URL | done | |
| Webview rebuild | done | |
| New window → external open | done | |
| Context menu disable | done | |
| Menubar from DOM | done | GtkPopoverMenuBar + GMenu |
| Tray / status item | done | In-memory tray for RPC; visual SNI/AppIndicator still TBD |
| Apple menu | n/a | successful no-op |
| Notifications | done | libnotify (falls back to stderr log) |
| Icons from path / PNG | done | |
| OS events (reopen, open url/file) | partial | `system.open_url` works; desktop-file reopen/open-file TBD |
| Locale / os_description | done | |
| Permission policy hybrid | done | |
| Microphone in webview | done | E2E via test RPC + fixture |
| Camera in webview | done | E2E via test RPC + fixture |
| Test RPC channel | done | `--edw-test-rpc` |
| Release artifact download | todo | |
| CI build | done | ubuntu-latest + xvfb |
