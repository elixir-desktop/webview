# Packaging

## Roles

| Mode | Who starts first | BEAM | Flags |
|------|------------------|------|-------|
| Packaged | Native host | Spawned by host | default (no `--edw-no-beam`) |
| Development | Elixir / mix | Already running | `--edw-no-beam` |
| E2E tests | Elixir | Already running | `--edw-no-beam --edw-test-rpc` |

Native **always** binds TCP; Elixir **always** connects.

## macOS `.app` defaults

```
MyApp.app/
  Contents/
    MacOS/DesktopWebView          # host binary
    Resources/
      DesktopWebView.ini          # optional
      beam/bin/<app>              # release start script
      beam/...
    Info.plist                    # include mic/camera usage strings when needed
```

Relative paths in the ini are resolved from `Contents/Resources/` when running
inside a bundle, otherwise from the directory containing the executable.

## Config discovery

1. `--edw-config=/path/to.ini`
2. `DesktopWebView.ini` beside the executable
3. `Contents/Resources/DesktopWebView.ini` (app bundle)

### Example ini

```ini
[beam]
path = beam
app_name = my_app
args = start
working_dir = beam
enabled = true

[network]
host = 127.0.0.1
port = 0

[lifetime]
mode = reconnect

[env]
# Extra environment for the BEAM child
# FOO = bar
```

## CLI (`--edw-*`)

All host options use the `edw` prefix. They are **stripped** before remaining
argv is forwarded to the BEAM release.

| Flag | Meaning |
|------|---------|
| `--edw-no-beam` | Do not spawn BEAM (dev / E2E) |
| `--edw-port=N` | Listen port (`0` = ephemeral) |
| `--edw-host=ADDR` | Bind address (default `127.0.0.1`) |
| `--edw-config=PATH` | Ini path |
| `--edw-lifetime=reconnect\|coupled` | Process coupling (default `reconnect`) |
| `--edw-test-rpc` | Enable `test.*` JSON-RPC methods |
| `--edw-beam-path=DIR` | Override beam release directory |
| `--edw-beam-app=NAME` | Override release script name |

Forwarded argv example:

```bash
DesktopWebView --edw-port=0 -- --foo bar
# BEAM receives: --foo bar
# (and EDW_PORT / EDW_HOST in the environment)
```

## Lifetime

- **`reconnect` (default):** host keeps listening after BEAM/client disconnect.
  Elixir may reconnect and call `initialize` again. Window state may be reset
  depending on host implementation; E2E asserts documented behavior.
- **`coupled`:** client disconnect → host exits; host exit → BEAM child is terminated.

## Binaries

| Platform | Delivery |
|----------|----------|
| macOS | Universal binary in Hex package `priv/native/macos/DesktopWebView` |
| Windows / Linux | Downloaded from GitHub Releases (draft on tag); not shipped in Hex `priv/` |

CI ad-hoc signs the macOS binary (`codesign -s -`). Full Developer ID / notarization
is expected later via `desktop_deployment`.

## Info.plist (media)

For microphone / camera inside WKWebView, the packaged app’s Info.plist must include:

- `NSMicrophoneUsageDescription`
- `NSCameraUsageDescription`
