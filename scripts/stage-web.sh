#!/usr/bin/env bash
# Completes build-wasm/dist (started by the WASM build: sigapp.js/.wasm, qtloader.js,
# index.html) into a servable app: the ML classifier module, the exported model and
# the onnxruntime-web runtime. Needs `npm install` (for the ORT runtime).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DIST="${1:-$REPO_DIR/build-wasm/dist}"

[ -f "$DIST/sigapp.wasm" ] || { echo "no WASM build in $DIST: run scripts/build-wasm.sh first" >&2; exit 1; }
mkdir -p "$DIST/js" "$DIST/models"
cp "$REPO_DIR/web/js/onnx-classifier.mjs" "$DIST/js/"
cp "$REPO_DIR"/ml/models/modulation_classifier.{onnx,json} "$DIST/models/"
"$REPO_DIR/scripts/vendor-ort.sh" "$DIST/vendor/ort"
echo "web app staged in $DIST (serve with scripts/serve.sh)"
