#!/usr/bin/env bash
# Runs web/ml-smoke.html in headless Chrome against a plain static server and
# fails unless every fixture capture classifies like the original Python.
# Needs: a WASM build (scripts/build-wasm.sh), `npm install`, Google Chrome.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$REPO_DIR/build-wasm/smoke"
PORT="${PORT:-8791}"
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
[ -x "$CHROME" ] || CHROME="$(command -v google-chrome || command -v chromium || true)"
[ -x "$CHROME" ] || { echo "Chrome not found (set CHROME=...)" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/models" "$STAGE/fixtures"
cp "$REPO_DIR/web/ml-smoke.html" "$STAGE/index.html"
cp "$REPO_DIR/build-wasm/bridge/sigbridge.js" "$REPO_DIR/build-wasm/bridge/sigbridge.wasm" "$STAGE/"
cp "$REPO_DIR/bridge/js/sigbridge.mjs" "$REPO_DIR/web/js/onnx-classifier.mjs" "$STAGE/"
cp "$REPO_DIR"/ml/models/modulation_classifier.{onnx,json} "$STAGE/models/"
cp "$REPO_DIR"/tests/data/ml/*.iq "$REPO_DIR/tests/data/ml/expected.json" "$STAGE/fixtures/"
"$REPO_DIR/scripts/vendor-ort.sh" "$STAGE/vendor/ort" >/dev/null

python3 -m http.server "$PORT" -d "$STAGE" >/dev/null 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true' EXIT
sleep 1

DOM="$("$CHROME" --headless=new --disable-gpu --no-sandbox --virtual-time-budget=600000 \
        --dump-dom "http://localhost:$PORT/" 2>/dev/null)"
echo "$DOM" | sed -n '/<pre/,/<\/pre>/p' | sed -e 's/<[^>]*>//g' -e 's/&lt;/</g' -e 's/&gt;/>/g' -e 's/&amp;/\&/g'
echo "$DOM" | grep -q 'SMOKE PASS' || { echo "browser smoke test FAILED" >&2; exit 1; }
