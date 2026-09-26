#!/usr/bin/env bash
# Re-stages the static site (build-wasm/dist) without recompiling. The WASM build already
# does this on every build (target `sigapp_dist`); use this after `npm install`, or after
# editing web/, ml/models/ or the ORT runtime, when nothing needs rebuilding.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$REPO_DIR/build-wasm/CMakeCache.txt" ] || { echo "no WASM build: run scripts/build-wasm.sh first" >&2; exit 1; }
# shellcheck disable=SC1091
source "${EMSDK_DIR:-$HOME/emsdk}/emsdk_env.sh" >/dev/null 2>&1
cmake --build "$REPO_DIR/build-wasm" --target sigapp_dist
echo "static site staged in $REPO_DIR/build-wasm/dist (serve with scripts/serve.sh)"
