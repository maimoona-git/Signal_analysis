#!/usr/bin/env bash
# Cross-compiles liquid-dsp to a static WASM library (libliquid.a) with
# Emscripten. Installs to $WASM_DEPS_DIR/install (default ~/wasm-deps/install),
# which core/CMakeLists.txt picks up via SIG_LIQUID_WASM_PREFIX.
#
# FFT decision: fftw is NOT cross-compiled. liquid-dsp ships its own FFT
# (radix-2, mixed-radix, Rader, DFT fallback) that it uses whenever fftw is
# absent, so we build with FIND_FFTW=OFF. SIMD and threads are off as well:
# the build is single-threaded and the SIMD paths are x86/NEON intrinsics.
set -euo pipefail

LIQUID_VERSION="${LIQUID_VERSION:-v1.8.2}"   # matches Homebrew's liquid-dsp used natively
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
WASM_DEPS_DIR="${WASM_DEPS_DIR:-$HOME/wasm-deps}"
SRC_DIR="$WASM_DEPS_DIR/liquid-dsp"
PREFIX="$WASM_DEPS_DIR/install"

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

mkdir -p "$WASM_DEPS_DIR"
if [ ! -d "$SRC_DIR" ]; then
  git clone --depth 1 --branch "$LIQUID_VERSION" https://github.com/jgaeddert/liquid-dsp "$SRC_DIR"
fi

emcmake cmake -S "$SRC_DIR" -B "$SRC_DIR/build-wasm" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
  -DBUILD_EXAMPLES=OFF -DBUILD_AUTOTESTS=OFF -DBUILD_BENCHMARKS=OFF \
  -DENABLE_SIMD=OFF -DFIND_SIMD=OFF \
  -DFIND_FFTW=OFF -DFIND_THREADS=OFF \
  -DENABLE_LOGGING=OFF -DENABLE_COLOR=OFF -DENABLE_TIMESTAMPS=OFF \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$SRC_DIR/build-wasm" -j
cmake --install "$SRC_DIR/build-wasm"

echo "liquid-dsp $LIQUID_VERSION (WASM, internal FFT) installed to $PREFIX"
