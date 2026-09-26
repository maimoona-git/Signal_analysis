#!/usr/bin/env bash
# Copies the onnxruntime-web runtime into <dest> (default build-wasm/dist/vendor/ort)
# so the app loads it from its own origin: no CDN, works offline / air-gapped.
# Only the plain CPU (wasm) runtime is taken; the webgl/webgpu/jsep/asyncify
# variants are not used. Run `npm install` first.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$REPO_DIR/build-wasm/dist/vendor/ort}"
SRC="$REPO_DIR/node_modules/onnxruntime-web/dist"

[ -d "$SRC" ] || { echo "onnxruntime-web not installed: run 'npm install' in $REPO_DIR" >&2; exit 1; }
mkdir -p "$DEST"
for f in ort.wasm.min.mjs ort-wasm-simd-threaded.mjs ort-wasm-simd-threaded.wasm; do
  cp "$SRC/$f" "$DEST/$f"
done
echo "vendored onnxruntime-web $(node -p "JSON.parse(require('fs').readFileSync('$REPO_DIR/node_modules/onnxruntime-web/package.json')).version") into $DEST"
