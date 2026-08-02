# AGENTS.md — desktop_webview

Guidelines for humans and coding agents working in this repository.

## What this repo is

Hex package `:desktop_webview`: native desktop webview hosts plus an Elixir
backend that implements `Desktop.Platform.*` for [elixir-desktop](https://github.com/elixir-desktop/desktop).

Apps keep using `Desktop.Window` / `Desktop.Menu`. They select this backend with:

```elixir
config :desktop, :backend, DesktopWebview.Backend
config :desktop, :menu_adapter, DesktopWebview.Menu.Adapter
```

## Layout

| Path | Purpose |
|------|---------|
| `lib/desktop_webview/` | Elixir transport, launcher, backend, menu adapter |
| `native/macos/` | Swift + AppKit + WKWebView host (primary) |
| `native/windows/` | WebView2 host (scaffold until implemented) |
| `native/linux/` | WebKitGTK host (scaffold until implemented) |
| `docs/` | Protocol, packaging, **porting**, integration, per-platform status |
| `test/` | Unit + Elixir E2E (drives the native binary) |
| `priv/native/` | Vendored macOS universal binary (CI-produced) |

## Hard rules

1. **Do not call `:wx*` from this package.** All UI goes through JSON-RPC to the native host.
2. **Do not teach `Desktop.Window` about WKWebView.** Integrate only via `Desktop.Platform` behaviours.
3. **Host flags use `--edw-*`.** Strip them before forwarding argv to BEAM.
4. **Native always listens; Elixir always connects** (prod and dev).
5. **No native unit-test frameworks** (no XCTest, etc.) as the source of truth. Extend the shared Elixir E2E suite instead. Test-only RPC (`test.*`) is allowed when gated by `--edw-test-rpc`.
6. **Status matrices are authoritative.** Mark a feature `done` on a platform only when Elixir E2E covers it.
7. **Per-platform native code stays isolated.** Do not share Swift/C++/GTK UI code across `native/*` until a deliberate shared core exists.

## Protocol ownership

Wire format, method names, and **behavioral semantics** live in `docs/protocol.md`.
Change the doc and both sides (Elixir + native) together. Production methods must
not depend on `test.*` methods.

New platform hosts follow `docs/porting.md` and may only mark status rows `done`
when Elixir E2E covers them.

## Launch modes

- **Packaged:** host starts, binds TCP, spawns BEAM from default `.app` / ini layout.
- **Dev:** Elixir launches the binary with `--edw-no-beam`, reads `listening <port>` from stdout, connects.
- **Lifetime default:** host survives BEAM disconnect (reconnect). Opt-in `--edw-lifetime=coupled`.

## Companion `desktop` changes

Menu adapter selection for third-party backends requires hooks in `elixir-desktop/desktop`
(`config :desktop, :menu_adapter, ...`). Keep those changes minimal and documented in
`docs/desktop-integration.md`.
