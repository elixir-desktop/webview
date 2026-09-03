# Linux host (WebKitGTK)

C++ + **GTK 4** + **WebKitGTK 6** executable `DesktopWebView`.

## Distro packages (Ubuntu 24.04 / noble)

Build:

```text
build-essential cmake pkg-config
libgtk-4-dev
libwebkitgtk-6.0-dev
libnotify-dev
libjson-glib-dev
```

Runtime (same SONAMEs): `libgtk-4-1`, `libwebkitgtk-6.0-4`, `libnotify4`,
`libjson-glib-1.0-0`.

Baseline verified in CI: GTK **4.14**, WebKitGTK **2.52** (`webkitgtk-6.0`).

Tray: AppIndicator libraries are GTK 3 and are not linked into this GTK 4 process.
`tray.*` RPC succeeds with an in-memory tray; StatusNotifierItem integration is
tracked as partial in [status/linux.md](../../docs/status/linux.md).

## Build

```bash
# from repo root
./scripts/build_linux.sh

# or
cmake -S native/linux -B native/linux/build -DCMAKE_BUILD_TYPE=Release
cmake --build native/linux/build
```

Output is copied to `priv/native/linux/DesktopWebView` for local/CI use.
Release artifact name: `DesktopWebView-linux-x86_64` (see [packaging.md](../../docs/packaging.md)).

## Run (dev / E2E)

```bash
./priv/native/linux/DesktopWebView --edw-no-beam --edw-test-rpc --edw-port=0
# prints: listening <port>
```

Headless CI needs a display (`xvfb-run` or a pre-set `DISPLAY`).

## HTML file inputs

WebKitGTK's asynchronous `run-file-chooser` default handler serves
`<input type="file">`. It handles single files, multiple files, directories,
and cancellation without using the `dialog.choose_file` RPC.

The shared E2E checks the fixture DOM, but it cannot drive a native picker.
Manually verify selection and cancellation with
`test/fixtures/file_input.html`. The chooser uses the GTK desktop portal when
the desktop session provides one, so the host needs an interactive display for
manual checks.

## Structure

| Path | Role |
|------|------|
| `CMakeLists.txt` | CMake project |
| `src/` | Host sources (config, RPC, windows, dispatch) |

Do not add a native unit-test suite; exercise the binary from Elixir E2E (`mix test.e2e`).

## Specs

- [Porting guide](../../docs/porting.md)
- [Protocol](../../docs/protocol.md)
- [Packaging](../../docs/packaging.md)
- [Status](../../docs/status/linux.md)
