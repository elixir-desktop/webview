# Windows host (WebView2)

Not implemented yet. This directory will hold the Microsoft Edge WebView2 host.

## Specs

- [Porting guide](../../docs/porting.md) — checklist, toolchain, E2E gate
- [Protocol](../../docs/protocol.md) — JSON-RPC + behavioral semantics
- [Packaging](../../docs/packaging.md) — Windows layout and release artifact names
- [Status](../../docs/status/windows.md) — feature matrix

## Contract (summary)

- Native process listens on TCP; Elixir connects
- JSON-RPC 2.0 over 4-byte length-prefixed frames
- `--edw-*` host flags; remaining argv forwarded to BEAM
- Binary: `DesktopWebView-windows-x64.exe` via GitHub Releases (not Hex `priv/`)
- `menu.set_apple` → successful no-op
