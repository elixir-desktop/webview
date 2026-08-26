# desktop_webview

Native desktop webview host for [elixir-desktop](https://github.com/elixir-desktop/desktop).

The host is a stand-alone native binary per platform. It listens on localhost TCP and
speaks **JSON-RPC 2.0** (length-prefixed frames). The Elixir package implements
`Desktop.Platform` behaviours and connects as the client. In packaged apps the host
spawns the BEAM release; in development an existing BEAM can launch the host with
`--edw-no-beam`.

| Platform | Engine | Status |
|----------|--------|--------|
| macOS | WKWebView | Primary — see [docs/status/macos.md](docs/status/macos.md) |
| Windows | WebView2 | Done — [docs/status/windows.md](docs/status/windows.md) |
| Linux | WebKitGTK | Usable — [docs/status/linux.md](docs/status/linux.md) |

## Quick start (development)

```elixir
# mix.exs
{:desktop_webview, "~> 0.1"}

# config/config.exs
config :desktop, :backend, DesktopWebview.Backend
config :desktop, :menu_adapter, DesktopWebview.Menu.Adapter
```

Build the native host for your OS (or use a vendored/CI binary):

```bash
./scripts/build_macos.sh   # macOS → priv/native/macos/DesktopWebView
./scripts/build_linux.sh   # Linux → priv/native/linux/DesktopWebView
mix test          # unit
mix test.e2e      # needs host binary; starts with --edw-test-rpc
```

## Packaged layout (macOS)

```
MyApp.app/
  Contents/MacOS/DesktopWebView
  Contents/Resources/DesktopWebView.ini   # optional
  Contents/Resources/beam/bin/<app>
```

Host flags use the `--edw-*` prefix; all other argv is forwarded to the BEAM app.
See [docs/packaging.md](docs/packaging.md).

## Documentation

- [Protocol](docs/protocol.md) — framing, methods, behavioral semantics, test RPC
- [Porting](docs/porting.md) — checklist for Windows / Linux hosts
- [Packaging](docs/packaging.md) — ini, argv, layouts, binaries
- [Desktop integration](docs/desktop-integration.md)
- [AGENTS.md](AGENTS.md) — contributor / agent rules

## License

MIT
