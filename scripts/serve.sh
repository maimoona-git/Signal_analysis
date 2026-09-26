#!/usr/bin/env bash
# Serves the built WASM app from a plain static server (no special headers:
# the build is single-threaded, so COOP/COEP are not needed).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-8080}"
echo "Serving $REPO_DIR/build-wasm/dist at http://localhost:$PORT/"
exec python3 -m http.server "$PORT" -d "$REPO_DIR/build-wasm/dist"
