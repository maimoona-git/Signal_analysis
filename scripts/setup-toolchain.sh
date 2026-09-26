#!/usr/bin/env bash
# Installs the WASM toolchain outside the repo:
#   - Emscripten SDK (pinned to the version Qt 6.8 is built against)
#   - Qt 6.8 for WebAssembly (single-threaded) + the host Qt it requires,
#     via aqtinstall (no Qt account needed)
# Re-running is safe: finished steps are skipped.
set -euo pipefail

EMSDK_VERSION="${EMSDK_VERSION:-3.1.56}"
QT_VERSION="${QT_VERSION:-6.8.3}"
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
QT_DIR="${QT_DIR:-$HOME/Qt}"
AQT_VENV="${AQT_VENV:-$HOME/.qt-aqt-venv}"

case "$(uname -s)" in
  Darwin) QT_HOST_OS=mac;   QT_HOST_ARCH=clang_64;   QT_HOST_SUBDIR=macos ;;
  Linux)  QT_HOST_OS=linux; QT_HOST_ARCH=linux_gcc_64; QT_HOST_SUBDIR=gcc_64 ;;
  *) echo "unsupported host OS" >&2; exit 1 ;;
esac

if [ ! -d "$EMSDK_DIR" ]; then
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi
"$EMSDK_DIR/emsdk" install "$EMSDK_VERSION"
"$EMSDK_DIR/emsdk" activate "$EMSDK_VERSION"

if [ ! -x "$AQT_VENV/bin/aqt" ]; then
  python3 -m venv "$AQT_VENV"
  "$AQT_VENV/bin/pip" install -q aqtinstall
fi
if [ ! -d "$QT_DIR/$QT_VERSION/$QT_HOST_SUBDIR" ]; then
  "$AQT_VENV/bin/aqt" install-qt "$QT_HOST_OS" desktop "$QT_VERSION" "$QT_HOST_ARCH" -O "$QT_DIR"
fi
if [ ! -d "$QT_DIR/$QT_VERSION/wasm_singlethread" ]; then
  "$AQT_VENV/bin/aqt" install-qt all_os wasm "$QT_VERSION" wasm_singlethread -O "$QT_DIR"
fi
# aqt unpacks the all_os WASM package without execute bits on its scripts.
chmod +x "$QT_DIR/$QT_VERSION/wasm_singlethread/bin/"* "$QT_DIR/$QT_VERSION/wasm_singlethread/libexec/"* 2>/dev/null || true

echo "Toolchain ready: Emscripten $EMSDK_VERSION in $EMSDK_DIR, Qt $QT_VERSION in $QT_DIR"
