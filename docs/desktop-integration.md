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

## Permissions

Hybrid policy (see `docs/protocol.md`): set defaults with
`system.set_permission_policy`, handle inbound `permission.request` RPC, and still
satisfy OS privacy prompts. Platform packaging notes are in `docs/packaging.md`.

## Porting another OS

See [docs/porting.md](porting.md).
