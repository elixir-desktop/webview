# Status — macOS (WKWebView)

States: `done` | `partial` | `todo` | `n/a`

A row becomes `done` only when covered by Elixir E2E (or explicitly documented as
manual-only with justification).

| Feature | Status | Notes |
|---------|--------|-------|
| TCP listen + `listening <port>` | done | |
| `initialize` + capabilities | done | |
| `--edw-no-beam` / argv strip | done | |
| Ini + defaults | done | |
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
| Menubar from DOM | done | |
| Default Edit menu (Cmd+C/V/X/A) | done | E2E via test.menu.list |
| Tray / status item | done | |
| Apple menu | done | |
| Notifications | done | |
| Icons from path / PNG | done | |
| OS events (reopen, open url/file) | done | |
| Locale / os_description | done | |
| Permission policy hybrid | done | |
| Microphone in webview | done | E2E via test RPC + fixture |
| Camera in webview | done | E2E via test RPC + fixture |
| Native dialogs (`dialog.choose_file/dir`) | done | `NSOpenPanel` (manual; blocks RPC) |
| HTML `<input type=file>` and Finder drag-and-drop | partial | `WKUIDelegate` open-panel hook and private WebKit drag bridge; native picker and Finder checks pending |
| Dialog prompt | done | `NSAlert` + text field (manual) |
| EventBridge Env/Window/Menu | done | Elixir unit coverage |
| Test RPC channel | done | `--edw-test-rpc` |
| Universal binary in priv | done | CI |
| Ad-hoc codesign | done | |
