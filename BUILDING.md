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
node). The ORT runtime is served from the app's own origin (no CDN, so no third-party
requests; it still needs the host to serve it) and is 14 MB, by far the largest asset. To regenerate the model or the
fixtures see `ml/README.md`.

The pipeline port (`app/pipeline/`, see its README) is tested against 600+ cases
recorded from the original native code (`tests/data/pipeline/`); regenerate them with
`~/signal-project/signal/.venv/bin/python ml/make_pipeline_fixtures.py --native-repo ~/signal-project/signal`
(the native venv, since it also records the original PyQt6 panel's text) only if the native behaviour is
deliberately changed.

## 6. The GUI

The browser app is the Qt Widgets GUI compiled to WASM (`app/gui`, see `app/README.md`):

```bash
scripts/build-wasm.sh          # builds the app (build-wasm/dist gets sigapp.js/.wasm, qtloader.js, index.html)
scripts/stage-web.sh           # adds the ML module, the model and the onnxruntime-web runtime (needs `npm install`)
scripts/serve.sh               # http://localhost:8080/  (any static server works; no special headers)
scripts/browser-gui-test.sh    # end to end in headless Chrome: clicks, typing, a real file chooser
```

The WASM build stages the whole static site in `build-wasm/dist` on every build (target
`sigapp_dist`, from scratch each time so nothing stale survives). `scripts/stage-web.sh`
re-stages without recompiling, e.g. after `npm install` or editing `web/`. Then:

```bash
scripts/verify-dist.sh         # the site as a deployer meets it (also a ctest, `test_static_site`)
```

which checks the exact file set (10 files), that nothing from the build machine leaked in and every URL
is relative, prints sizes, and re-runs the browser test on a *relocated copy* served by a stock
`python3 -m http.server`.

Drop a `.wav` / `.IQ` file on the page or use "Open File...". The widget tests run on a
desktop Qt under the offscreen platform; configure the native build with your Qt:

```bash
cmake -B build -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/macos     # the host Qt installed by setup-toolchain.sh
cmake --build build && ctest --test-dir build               # adds test_gui
```

Without a Qt the native build simply skips `/app/gui`.

### What gets deployed

| | raw | gzip |
| --- | ---: | ---: |
| `sigapp.wasm` (Qt Widgets + the C++ pipeline) | 14.4 MB | 5.4 MB |
| `vendor/ort/ort-wasm-simd-threaded.wasm` (onnxruntime-web CPU runtime) | 14.2 MB | 3.7 MB |
| everything else (loader JS, page, model 360 KB) | 0.7 MB | 0.4 MB |
| **total, 10 files** | **29.4 MB** | **9.4 MB** |

Size-optimising the final link (`-Oz`) was measured and saves only 3.6% because the bulk is Qt's own
prebuilt libraries, so `-O2` is kept. Let the host compress (any does); the app works from any path.

### Limits

The browser holds a capture, its decoded samples and its spectrogram at the same time. Measured in
headless Chrome, before the gap-list work added windowing (first 16M samples only) and the
automatic searches -- not re-measured since: a 64 MB capture analysed in ~0.2 s, 512 MB in ~2 s,
and 768 MB did not fit (the heap is capped at 4 GB); the app then reports "out of memory: this capture is too large..."
and keeps working. The decoded message shown in the results panel is capped at 10,000 characters
(Qt's layout of one enormous line took minutes); the full text is not otherwise affected.

## Threading

The WASM build is **single-threaded** (the `wasm_singlethread` Qt kit, and no
`-pthread`). Multi-threaded WASM needs `SharedArrayBuffer`, which browsers only
enable when the page is served with `Cross-Origin-Opener-Policy` and
`Cross-Origin-Embedder-Policy` headers. Plain static hosts don't send those.
The single-threaded build runs from any static file server as-is. DSP work does
leave the UI thread, as that plan said it would: the analysis runs in a Web Worker
(`web/js/analysis-worker.js`) that loads a second WASM instance, the bridge module
(`sigbridge.js`/`sigbridge.wasm`, the whole pipeline without Qt), plus the ONNX
classifier. Options go to the worker, and the result comes back, in the binary format
of `app/pipeline/pipeline_io.hpp` (unit-tested natively); the Qt app takes the result
as one transferred buffer. No threads inside either module, so still no COOP/COEP
headers. If the page cannot start a worker, the app falls back to running the same
analysis inside its own module (`WasmBackend`), blocking the UI while it does.
