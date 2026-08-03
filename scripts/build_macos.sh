#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$ROOT/priv/native/macos"
mkdir -p "$OUT_DIR"
cd "$ROOT/native/macos"

# Try universal via Apple's swift driver; fall back to host arch.
if swift build -c release --arch arm64 --arch x86_64; then
  if [[ -x .build/apple/Products/Release/DesktopWebView ]]; then
    BIN=".build/apple/Products/Release/DesktopWebView"
  else
    BIN="$(swift build -c release --arch arm64 --arch x86_64 --show-bin-path)/DesktopWebView"
  fi
else
  echo "Universal build failed; building host architecture only" >&2
  swift build -c release
  BIN="$(swift build -c release --show-bin-path)/DesktopWebView"
fi

cp -f "$BIN" "$OUT_DIR/DesktopWebView"
chmod +x "$OUT_DIR/DesktopWebView"
# Embedding __info_plist (mic/camera usage) invalidates any prior signature;
# ad-hoc sign so macOS will load the binary (otherwise SIGKILL / Invalid Page).
codesign --force --sign - --identifier io.elixirdesktop.desktopwebview \
  "$OUT_DIR/DesktopWebView"
echo "Built $OUT_DIR/DesktopWebView"
file "$OUT_DIR/DesktopWebView" || true
codesign -dv "$OUT_DIR/DesktopWebView" 2>&1 | head -8 || true
