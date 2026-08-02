# Windows host (WebView2)

Not implemented yet. This directory will hold the Microsoft Edge WebView2 host.

See [docs/status/windows.md](../../docs/status/windows.md) for the feature matrix.

Contract (same as macOS):

- Native process listens on TCP; Elixir connects
- JSON-RPC 2.0 over 4-byte length-prefixed frames (`docs/protocol.md`)
- `--edw-*` host flags; remaining argv forwarded to BEAM
- Large binaries distributed via GitHub Releases (not Hex `priv/`)
