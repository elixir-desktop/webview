# Linux host (WebKitGTK)

Not implemented yet. This directory will hold the WebKitGTK host.

## Specs

- [Porting guide](../../docs/porting.md) — checklist, toolchain, E2E gate
- [Protocol](../../docs/protocol.md) — JSON-RPC + behavioral semantics
- [Packaging](../../docs/packaging.md) — Linux layout and release artifact names
- [Status](../../docs/status/linux.md) — feature matrix

## Contract (summary)

- Native process listens on TCP; Elixir connects
- JSON-RPC 2.0 over 4-byte length-prefixed frames
- `--edw-*` host flags; remaining argv forwarded to BEAM
- Binary: `DesktopWebView-linux-x86_64` via GitHub Releases (not Hex `priv/`)
- `menu.set_apple` → successful no-op
- Document GTK / WebKitGTK package versions here when the host lands
