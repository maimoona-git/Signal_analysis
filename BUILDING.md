# Building PS 26147 (native + WASM)

One CMake source tree builds two targets:

- **Native**: the `sigcore_core` C++ library, the optional pybind11 `sigcore`
  module (same Python API as the native desktop app), and the C++ tests.
- **WASM**: the same `sigcore_core` library compiled with Emscripten, the Qt
  Widgets GUI (`sigapp`) compiled with Qt for WebAssembly, and the same C++
  tests run under node.

## Prerequisites

- macOS or Linux, CMake 3.21+, Python 3, git, node (for WASM tests; emsdk
  also ships one)
- Native only: `liquid-dsp` (`brew install liquid-dsp` on macOS)

## 1. Install the WASM toolchain (once)

```bash
scripts/setup-toolchain.sh
```

This installs, outside the repo:

| Tool | Version | Location |
| --- | --- | --- |
| Emscripten SDK | 3.1.56 (the version Qt 6.8 is built against) | `~/emsdk` |
| Qt for WebAssembly, single-threaded | 6.8.3 | `~/Qt/6.8.3/wasm_singlethread` |
| Host Qt (needed by Qt's WASM build tools) | 6.8.3 | `~/Qt/6.8.3/macos` (or `gcc_64`) |

Qt is installed with [aqtinstall](https://github.com/miurahr/aqtinstall), so no
Qt account or GUI installer is needed. Override the versions or locations with
`EMSDK_VERSION`, `QT_VERSION`, `EMSDK_DIR` and `QT_DIR`. The Emscripten version
must match the Qt release: see Qt's "Qt for WebAssembly" docs before bumping
either.

## 2. Cross-compile liquid-dsp for WASM (once)

```bash
scripts/build-liquid-wasm.sh
```

Clones liquid-dsp v1.8.2 (the version Homebrew provides for the native build)
into `~/wasm-deps/liquid-dsp`, builds a static `libliquid.a` with Emscripten and
installs it to `~/wasm-deps/install` (override with `WASM_DEPS_DIR`; point CMake
elsewhere with `-DSIG_LIQUID_WASM_PREFIX=...`).

**FFT: fftw is not cross-compiled.** liquid-dsp carries its own FFT (radix-2,
mixed-radix, Rader and a plain-DFT fallback) and uses it whenever fftw is absent,
so the WASM build configures with `FIND_FFTW=OFF`. `tests/core/test_dsp.cpp`
checks that internal FFT against a reference DFT for power-of-two, composite and
prime sizes, and runs the same assertions on the native (fftw-backed) build.
SIMD and threads are also off: the SIMD paths are x86/NEON intrinsics and the
build is single-threaded.

## 3. Native build

```bash
cmake -B build -DPython3_EXECUTABLE=/path/to/python-with-pybind11
cmake --build build
ctest --test-dir build --output-on-failure
```

`-DPython3_EXECUTABLE` is only needed for the pybind11 module. Without
pybind11 the module is skipped and the library and tests still build.

## 4. WASM build

```bash
scripts/build-wasm.sh
ctest --test-dir build-wasm --output-on-failure   # runs the tests under node
scripts/serve.sh                                  # http://localhost:8080/
```

The WASM build also produces `build-wasm/bridge/sigbridge.js` + `.wasm` (the
Embind bridge, see `bridge/README.md`). `ctest` runs its JS test suite with
`node --test` (any node >= 20; `File`/`Blob` are used as fake browser inputs), so
`node` must be on `PATH`; without it that one test is skipped with a warning.

C++ exceptions are enabled for the whole WASM build (`-fexceptions`), because
`sigcore` reports errors by throwing and Emscripten otherwise compiles `throw`
into an uncatchable abort.

`build-wasm.sh` sources the emsdk environment and configures with Qt's
`qt-cmake`, which chainloads the Emscripten toolchain file. The servable app is
staged in `build-wasm/dist/`: `index.html` (from `/web`), `sigapp.js`,
`sigapp.wasm` and `qtloader.js`. Run `ctest` in a shell where the emsdk
environment is active (`source ~/emsdk/emsdk_env.sh`) so it can find node.

## 5. ML model (browser inference)

The exported model is committed (`ml/models/`), so building the app needs no
Python. For the JS side:

```bash
npm install                        # onnxruntime-web 1.30.0 (lockfile committed)
scripts/vendor-ort.sh              # copies the CPU wasm runtime to build-wasm/dist/vendor/ort
scripts/browser-smoke.sh           # headless Chrome: full chain on 11 fixtures vs the original Python
```

`ctest` also runs `tests/bridge/onnx.test.mjs` (real model + real bridge under
node). The ORT runtime is served from the app's own origin (no CDN, works
offline) and is 14 MB, by far the largest asset. To regenerate the model or the
fixtures see `ml/README.md`.

## Threading

The WASM build is **single-threaded** (the `wasm_singlethread` Qt kit, and no
`-pthread`). Multi-threaded WASM needs `SharedArrayBuffer`, which browsers only
enable when the page is served with `Cross-Origin-Opener-Policy` and
`Cross-Origin-Embedder-Policy` headers. Plain static hosts don't send those.
The single-threaded build runs from any static file server as-is. If DSP work
ever needs to leave the UI thread, the plan is a Web Worker running a second
WASM instance, not in-module threads.
