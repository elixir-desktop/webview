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

## HTML file inputs and Finder drops

`<input type="file">` uses the `WKUIDelegate` open-panel callback in
`WebWindow.swift`. The callback maps single, multiple, directory, and cancel
actions to WebKit's `FileList`. `FileDropWebView.swift` registers file URLs and
file promises and installs a runtime bridge on WebKit's private content view.
The bridge preserves WebKit's original drag methods after accepting Finder
drops.

The shared E2E checks the fixture DOM, but it cannot drive the macOS picker or
Finder. Manually verify selection, cancellation, multiple files, directory
selection, and Finder drops with `test/fixtures/file_input.html`, including
after `webview.rebuild`.

## Structure

| File | Role |
|------|------|
| `Package.swift` | SPM package |
| `Sources/DesktopWebView/` | Host sources |

Do not add XCTest targets; exercise the binary from Elixir E2E (`mix test.e2e`).
