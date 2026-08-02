# Linux host (WebKitGTK)

Not implemented yet. This directory will hold the WebKitGTK host.

See [docs/status/linux.md](../../docs/status/linux.md) for the feature matrix.

Contract (same as macOS):

- Native process listens on TCP; Elixir connects
- JSON-RPC 2.0 over 4-byte length-prefixed frames (`docs/protocol.md`)
- `--edw-*` host flags; remaining argv forwarded to BEAM
- Large binaries distributed via GitHub Releases (not Hex `priv/`)
