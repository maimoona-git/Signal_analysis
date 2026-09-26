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

## 2. Native build

```bash
cmake -B build -DPython3_EXECUTABLE=/path/to/python-with-pybind11
cmake --build build
ctest --test-dir build --output-on-failure
```

`-DPython3_EXECUTABLE` is only needed for the pybind11 module. Without
pybind11 the module is skipped and the library and tests still build.

## 3. WASM build

```bash
scripts/build-wasm.sh
ctest --test-dir build-wasm --output-on-failure   # runs the tests under node
scripts/serve.sh                                  # http://localhost:8080/
```

`build-wasm.sh` sources the emsdk environment and configures with Qt's
`qt-cmake`, which chainloads the Emscripten toolchain file. The servable app is
staged in `build-wasm/dist/`: `index.html` (from `/web`), `sigapp.js`,
`sigapp.wasm` and `qtloader.js`. Run `ctest` in a shell where the emsdk
environment is active (`source ~/emsdk/emsdk_env.sh`) so it can find node.

## Threading

The WASM build is **single-threaded** (the `wasm_singlethread` Qt kit, and no
`-pthread`). Multi-threaded WASM needs `SharedArrayBuffer`, which browsers only
enable when the page is served with `Cross-Origin-Opener-Policy` and
`Cross-Origin-Embedder-Policy` headers. Plain static hosts don't send those.
The single-threaded build runs from any static file server as-is. If DSP work
ever needs to leave the UI thread, the plan is a Web Worker running a second
WASM instance, not in-module threads.
