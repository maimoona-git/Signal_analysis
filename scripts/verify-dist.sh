#!/usr/bin/env bash
# Verifies the static site the way a deployer would meet it. Checks that
#   1. it is exactly the intended set of files (nothing missing, nothing extra),
#   2. nothing from the build machine leaked in, and the page's own URLs are all relative,
#   3. a *copy* of it, moved out of the repo and served by a stock `python3 -m http.server`
#      (no special headers, no custom MIME map), passes the full browser test.
# Usage: scripts/verify-dist.sh [dist-dir]     (default: build-wasm/dist; needs Chrome and `npm install`)
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$(cd "${1:-$REPO_DIR/build-wasm/dist}" && pwd)"
fail() { echo "FAIL: $*" >&2; exit 1; }

EXPECTED="css/fonts.css
css/tokens.css
fonts/ibm-plex-mono-latin-400-normal.woff2
fonts/ibm-plex-mono-latin-500-normal.woff2
fonts/ibm-plex-mono-latin-600-normal.woff2
fonts/ibm-plex-sans-latin-400-normal.woff2
fonts/ibm-plex-sans-latin-500-normal.woff2
fonts/ibm-plex-sans-latin-600-normal.woff2
index.html
js/onnx-classifier.mjs
models/modulation_classifier.json
models/modulation_classifier.onnx
qtloader.js
sigapp.js
sigapp.wasm
vendor/ort/ort-wasm-simd-threaded.mjs
vendor/ort/ort-wasm-simd-threaded.wasm
vendor/ort/ort.wasm.min.mjs"

echo "== 1. the file set"
ACTUAL="$(cd "$DIST" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)"
if [ "$ACTUAL" != "$EXPECTED" ]; then
  diff <(echo "$EXPECTED") <(echo "$ACTUAL") || true
  fail "$DIST is not exactly the expected set of files"
fi
echo "   exactly $(echo "$EXPECTED" | wc -l | tr -d ' ') files, no strays"

echo "== 2. hygiene"
# Nothing from THIS machine may be in the output: its home directory or the repo path. (Qt's
# prebuilt libraries carry a couple of Qt-CI source paths like C:/Users/qt/work/..., and
# Emscripten's in-memory filesystem is called /home/web_user; neither is ours, so the check
# names this machine's paths rather than matching /Users/ or /home/ in general.)
LEAKS="$(cd "$DIST" && grep -rIn -e "$HOME" -e "$REPO_DIR" . 2>/dev/null || true)"
[ -z "$LEAKS" ] || fail "this machine's paths in text files: $(echo "$LEAKS" | cut -c1-120 | head -3)"
grep -qa -e "$HOME" -e "$REPO_DIR" "$DIST/sigapp.wasm" && fail "this machine's paths in sigapp.wasm"
grep -qa -e "$HOME" -e "$REPO_DIR" "$DIST/vendor/ort/ort-wasm-simd-threaded.wasm" && fail "this machine's paths in the ORT runtime"
ABSOLUTE="$(cd "$DIST" && grep -nE "(src|href)=\"(/|https?:)|import[^;]*from ['\"](/|https?:)" index.html js/*.mjs || true)"
[ -z "$ABSOLUTE" ] || fail "absolute URLs in the page's own code (breaks hosting under a sub-path): $ABSOLUTE"
echo "   no paths from this machine in any file; every URL in index.html and js/ is relative"

echo "== 3. sizes"
printf "   %-42s %12s %12s\n" file raw gzip
while read -r f; do
  printf "   %-42s %12d %12d\n" "$f" "$(stat -f%z "$DIST/$f" 2>/dev/null || stat -c%s "$DIST/$f")" "$(gzip -9 -c "$DIST/$f" | wc -c)"
done <<<"$EXPECTED"
printf "   %-42s %12d %12d\n" TOTAL "$(cd "$DIST" && cat $EXPECTED | wc -c)" "$(cd "$DIST" && tar cf - $EXPECTED | gzip -9 | wc -c)"

echo "== 4. relocated copy on a stock static server, full browser test"
WORK="$(mktemp -d)"
cp -R "$DIST" "$WORK/site"
PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
python3 -m http.server "$PORT" --bind 127.0.0.1 -d "$WORK/site" >/dev/null 2>&1 &
SERVER=$!
disown "$SERVER" 2>/dev/null || true      # so bash does not announce the server's termination at exit
trap 'kill $SERVER 2>/dev/null || true; rm -rf "$WORK"' EXIT
for _ in $(seq 1 50); do curl -sf -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 0.1; done
echo "   content types this server sends:"
for f in index.html sigapp.js sigapp.wasm js/onnx-classifier.mjs vendor/ort/ort.wasm.min.mjs vendor/ort/ort-wasm-simd-threaded.wasm models/modulation_classifier.onnx; do
  printf "     %-42s %s\n" "$f" "$(curl -sI "http://127.0.0.1:$PORT/$f" | tr -d '\r' | awk -F': ' 'tolower($1)=="content-type"{print $2}')"
done
SIG_ORIGIN="http://127.0.0.1:$PORT/" SIG_DIST="$WORK/site" node --test "$REPO_DIR/tests/gui/browser-gui.test.mjs" 2>&1 \
  | grep -E '^\s*(ok|not ok)|^# (pass|fail|skipped)' | sed 's/^/   /'
echo "== verified: $DIST"
