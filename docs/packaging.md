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

## Windows layout

Default when the host runs as a normal Win32 process (installer or portable zip):

```
MyApp/
  DesktopWebView.exe
  DesktopWebView.ini              # optional, beside the exe
  beam/
    bin/
      my_app.bat                  # or my_app (escript/release)
    ...
```

- Ini discovery: `--edw-config` → `DesktopWebView.ini` beside the executable.
- Relative `beam.path` / `working_dir` resolve against the directory containing
  `DesktopWebView.exe`.
- Forwarded argv and `EDW_PORT` / `EDW_HOST` are unchanged.
- Release asset name: `DesktopWebView-windows-x64.exe` (see Binaries).
- WebView2: document Evergreen Runtime dependency in the app installer; the host
  should fail with a clear stderr message if the runtime is missing.
- Microphone / camera: declare app capabilities in the packaged manifest /
  privacy settings as required by the target Windows version; the hybrid
  permission RPC still applies on top.

## Linux layout

Default flat layout (also suitable for AppImage / AppDir with the same relative
paths):

```
MyApp/
  DesktopWebView                  # host binary
  DesktopWebView.ini              # optional
  beam/
    bin/
      my_app
    ...
```

- Ini discovery: `--edw-config` → `DesktopWebView.ini` beside the executable.
- Relative paths resolve against the executable’s directory.
- Release asset name: `DesktopWebView-linux-x86_64`.
- Prefer shipping against a documented WebKitGTK/GTK baseline (note distro
  packages in `native/linux/README.md`).
- Tray: StatusNotifierItem when available.
- Microphone / camera: PipeWire/Pulse + portal prompts may appear; hybrid
  permission policy still applies. Flatpak/snap portals need extra packaging
  notes when those formats are supported.

## Config discovery

1. `--edw-config=/path/to.ini`
2. `DesktopWebView.ini` beside the executable
3. macOS only: `Contents/Resources/DesktopWebView.ini` (app bundle)

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

| Platform | Delivery | Artifact name |
|----------|----------|---------------|
| macOS | Universal binary in Hex `priv/native/macos/DesktopWebView` | `DesktopWebView-macos-universal` (also on GitHub Releases) |
| Windows | GitHub Releases (draft on tag); **not** in Hex `priv/` | `DesktopWebView-windows-x64.exe` + `.sha256` |
| Linux | GitHub Releases (draft on tag); **not** in Hex `priv/` | `DesktopWebView-linux-x86_64` + `.sha256` |

CI ad-hoc signs the macOS binary (`codesign -s -`). Full Developer ID / notarization
and Windows Authenticode are expected later via `desktop_deployment`.

Tag workflow (`.github/workflows/release.yml`) should attach every built platform
asset to the same draft release. Elixir download/cache for Win/Linux is TBD;
until then use `DESKTOP_WEBVIEW_BINARY` / `config :desktop_webview, :binary`.

## Media / privacy packaging

### macOS (Info.plist)

For microphone / camera inside WKWebView, the packaged app’s Info.plist must include:

- `NSMicrophoneUsageDescription`
- `NSCameraUsageDescription`

### Windows

Ensure the application identity used at runtime can access microphone/camera
under Windows privacy settings. WebView2 may show its own permission UI when
policy is `ask`.

### Linux

Ensure the desktop entry / sandbox (if any) allows device access. WebKitGTK
permission requests must still honor the hybrid RPC policy.
