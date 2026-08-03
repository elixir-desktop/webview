# Desktop integration

## Selecting the backend

```elixir
# config/config.exs
config :desktop, :backend, DesktopWebview.Backend
config :desktop, :menu_adapter, DesktopWebview.Menu.Adapter
```

`Desktop.Window`, `Desktop.Menu`, and `Desktop.Env` keep working unchanged.
This package depends on `:desktop` and implements `Desktop.Platform.*` behaviours.

## Companion changes in `desktop`

`Desktop.Platform.Menu.adapter/1` must honor an optional configured adapter:

```elixir
config :desktop, :menu_adapter, DesktopWebview.Menu.Adapter
```

When set, that module is used instead of inferring Wx / Json / DBus / Browser.
See the corresponding PR on `elixir-desktop/desktop`.

## Dev vs packaged

- **Dev:** `DesktopWebview.Launcher` starts `priv/native/.../DesktopWebView` with
  `--edw-no-beam`, parses `listening <port>`, connects.
- **Packaged:** OS launches the host; host spawns the release; Elixir connects using
  `EDW_PORT`.

Override binary path:

```elixir
config :desktop_webview, :binary, "/path/to/DesktopWebView"
# or env DESKTOP_WEBVIEW_BINARY
```

## Capabilities

```elixir
DesktopWebview.Backend.capabilities()
# %{
#   window: true,
#   content: :native,
#   notification: :native,
#   menu: :native,
#   taskbar: true
# }
```

## Event bridge

`DesktopWebview.EventBridge` owns the Transport event subscription when the
backend is active. It translates host notifications into elixir-desktop messages:

| Host event | Delivery |
|------------|----------|
| `event.window.close_requested` | `GenServer.cast(window, :close_window)` |
| `event.window.focus` | `GenServer.cast(window, :frame_activated)` |
| `event.system.open_url` | `Desktop.Env.notify_subscribers({:open_url, [url]})` |
| `event.system.open_file` | `Desktop.Env.notify_subscribers({:open_file, [path]})` |
| `event.system.reopen` | `{:reopen_app, []}` to `Desktop.Env` |
| `event.menu.click` | `GenServer.cast(menu, {:trigger_event, onclick})` |
| `event.webview.new_window` | `system.open_url` (external browser) |

Do **not** subscribe `Desktop.Env` directly to Transport — raw `{:edw_event, ...}`
messages are not in the Env contract.

## Dialogs

```elixir
DesktopWebview.Dialog.choose_file(title: "Pick a file", default_path: path)
DesktopWebview.Dialog.choose_directory(title: "Pick a folder")
DesktopWebview.Dialog.prompt("Title", "Message", "default")
```

## Permissions

Hybrid policy (see `docs/protocol.md`): set defaults with
`system.set_permission_policy`, handle inbound `permission.request` RPC, and still
satisfy OS privacy prompts. Platform packaging notes are in `docs/packaging.md`.

`DesktopWebview.Backend.init_env/0` grants WebKit capture (`permission.request` →
`allow`) because the desktop host owns the page. macOS still requires
`NSMicrophoneUsageDescription` / `NSCameraUsageDescription` (embedded in the
host binary via `native/macos/Info.plist`) and a System Settings approval for
the `DesktopWebView` process. Without that TCC grant, CallLive `getUserMedia`
fails as `audio_denied` even when WebKit is allowed.
## Porting another OS

See [docs/porting.md](porting.md).
