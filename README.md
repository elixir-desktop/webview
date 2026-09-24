# desktop_webview

> **No human commits are accepted in this project. Only agent code.**

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

HTML `<input type="file">` support and file-manager drag-and-drop are required on
all three hosts. Finder, Explorer, and Linux file-manager drops must reach the
page as normal web drag events with a usable `DataTransfer.files` list. The
shared E2E checks the fixture's DOM contract; native picker and file-drop checks
remain manual. These features are separate from the `dialog.choose_file` RPC.
See [docs/protocol.md](docs/protocol.md).

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

## Agent development

Implementation is done by Cursor agents. A commit is accepted only when it is
agent-authored (`Cursor Agent <cursoragent@cursor.com>`). Humans file the task
and review the pull request; they do not land implementation commits.

Four workflows are in use:

| Workflow | Where it starts | What it produces |
|----------|-----------------|------------------|
| Cloud implementation | Cursor web or CLI cloud agent | Code on a `cursor/<description>-<suffix>` branch, commits, and a draft pull request |
| Bugbot fix | “Fix in Web” from a review comment on an open pull request | A follow-up commit on that pull request’s existing branch |
| Desktop plan | Cursor desktop, asked for a plan | An implementation plan only. No branch and no commits |
| Explore | Spawned by a parent agent | A read-only report. No branch and no commits |

Cloud implementation follows [AGENTS.md](AGENTS.md):

1. Read `AGENTS.md` and the docs the change touches (`docs/protocol.md`, packaging, porting, status).
2. Branch from the current base as `cursor/<description>-<suffix>`.
3. Change the protocol doc and both sides (Elixir and the native host) in the same change. Production methods do not depend on `test.*`.
4. Prove behavior with the shared Elixir suite: `mix test` for units, and `mix test.e2e` (or the platform job in [`.github/workflows/ci.yml`](.github/workflows/ci.yml)) against a built host. Mark a status-matrix row `done` only after that E2E covers it.
5. Commit, push, and open a draft pull request. Watch CI (Elixir unit, plus macOS, Linux, and Windows host E2E) and push follow-up commits until it is green.

Bugbot fix uses the same rules on the branch that is already under review. Desktop plan and Explore stop before any commit.

## Documentation

- [Protocol](docs/protocol.md) — framing, methods, behavioral semantics, test RPC
- [Porting](docs/porting.md) — checklist for Windows / Linux hosts
- [Packaging](docs/packaging.md) — ini, argv, layouts, binaries
- [`--edw-rpc`](docs/specs/feature-edw-rpc.md) — one-shot Elixir via control socket + `rpc.eval`
- [Single-instance](docs/specs/feature-single-instance.md) — host-owned lock and second-launch activate
- [BEAM restart / `--edw-recover`](docs/specs/feature-beam-restart.md)
- [Desktop integration](docs/desktop-integration.md)
- [AGENTS.md](AGENTS.md) — contributor / agent rules

## License

MIT
