#!/usr/bin/env bash
# Configures and builds the WASM target into build-wasm/; the servable app
# ends up in build-wasm/dist/. Extra arguments are passed to the configure step.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
QT_VERSION="${QT_VERSION:-6.8.3}"
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
QT_DIR="${QT_DIR:-$HOME/Qt}"

case "$(uname -s)" in
  Darwin) QT_HOST_SUBDIR=macos ;;
  *)      QT_HOST_SUBDIR=gcc_64 ;;
esac

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

"$QT_DIR/$QT_VERSION/wasm_singlethread/bin/qt-cmake" \
  -S "$REPO_DIR" -B "$REPO_DIR/build-wasm" \
  -DQT_HOST_PATH="$QT_DIR/$QT_VERSION/$QT_HOST_SUBDIR" "$@"
cmake --build "$REPO_DIR/build-wasm" -j
