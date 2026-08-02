#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$ROOT/priv/native/linux"
BUILD_DIR="$ROOT/native/linux/build"
mkdir -p "$OUT_DIR" "$BUILD_DIR"

cmake -S "$ROOT/native/linux" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

cp -f "$BUILD_DIR/DesktopWebView" "$OUT_DIR/DesktopWebView"
chmod +x "$OUT_DIR/DesktopWebView"
echo "Built $OUT_DIR/DesktopWebView"
file "$OUT_DIR/DesktopWebView" || true
