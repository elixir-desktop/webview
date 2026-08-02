# macOS host (WKWebView)

Swift + AppKit + WKWebView executable `DesktopWebView`.

## Build

```bash
# from repo root
./scripts/build_macos.sh

# or
cd native/macos && swift build -c release --arch arm64 --arch x86_64
```

Output is copied to `priv/native/macos/DesktopWebView` (universal when both archs build).

## Run (dev / E2E)

```bash
./priv/native/macos/DesktopWebView --edw-no-beam --edw-test-rpc --edw-port=0
# prints: listening <port>
```

## Structure

| File | Role |
|------|------|
| `Package.swift` | SPM package |
| `Sources/DesktopWebView/` | Host sources |

Do not add XCTest targets; exercise the binary from Elixir E2E (`mix test.e2e`).
