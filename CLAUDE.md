# PS 26147 — WASM/Browser Signal Analysis Tool

SIH 2026 prototype for NTRO's PS 26147, re-platformed from the native PyQt6
desktop app into a browser app: Qt Widgets compiled to WebAssembly, running
entirely client-side (no server-side processing).

This covers the **application itself** only — the Qt-WASM GUI, DSP/FEC core,
ML classifier, and pipeline. Marketing/informational pages (landing page,
`/about`, `/docs`) are out of scope here and will be built separately.

## Tech stack

- **Core DSP + FEC engine**: same C++ source as the native build (`liquid-dsp`
  based), cross-compiled with Emscripten (`emcc`) to WebAssembly instead of a
  native `.so`.
- **GUI**: Qt Widgets in C++, compiled via Qt-for-WebAssembly, rendering to an
  HTML `<canvas>`. No PyQt6, no pyqtgraph — plotting (waterfall,
  constellation) is custom-drawn with `QPainter`.
- **ML parameter classifier**: trained PyTorch weights exported to ONNX
  (training stays in Python, offline); inference at runtime via
  `onnxruntime-web` in JavaScript.
- **C++ ↔ JS bridge**: Embind, used for (a) passing browser-provided file
  bytes into the WASM module, and (b) the C++ pipeline calling out to the JS
  ML inference and getting results back. This bridge is a first-class part of
  the architecture, not an afterthought — treat it as its own deliverable.
- **No Python at runtime.** Python only exists in `/ml` as dev-time training
  tooling.

## Architecture (pipeline order)

Browser file input (JS) → bytes into WASM (Embind) → DSP core / spectral
analysis (C++/WASM) → ML classification (Embind call out to
`onnxruntime-web`, result back into C++) → Demodulation (C++/WASM) →
De-interleaving (C++/WASM) → FEC decoding (C++/WASM) → Bitstream correlation
(C++/WASM) → GUI (Qt Widgets/WASM, renders results at every stage).

## Key architectural decision: threading

Qt-for-WebAssembly is single-threaded by default. True WASM threads need
`SharedArrayBuffer` plus `Cross-Origin-Opener-Policy`/`Cross-Origin-Embedder-
Policy` headers, which plain static hosts (GitHub Pages, etc.) don't set by
default. Default to a **single-threaded build** unless a specific deployment
target with the right headers is confirmed. If DSP processing needs to stay
off the UI thread to keep the GUI responsive, use a Web Worker running a
second WASM instance rather than relying on in-module threads — note this
explicitly if it becomes necessary rather than silently degrading UI
responsiveness.

## Repo layout

- `/core` — C++ DSP/FEC engine, shared between native and WASM builds where
  possible; CMake toolchain file for the Emscripten target
- `/bridge` — Embind bindings: file-bytes-in, ML-call-out, results-back
- `/app` — C++ pipeline orchestration + Qt Widgets GUI (WASM target)
- `/ml` — model definition, training scripts (Python, dev-time only), ONNX
  export script
- `/tests` — native/WASM test suite (C++ test framework and/or JS test runner
  driving the compiled WASM module)
- `/web` — minimal static shell (`index.html`, loader JS) to host the
  compiled app; does not include the separate marketing/landing pages

## Working conventions

- **No git commits.** Never run `git add` or `git commit`. At the end of each
  phase, print the exact `git add <files>` command for the files touched in
  that phase, plus one suggested one-line commit message. The user commits
  manually.
- **One phase at a time.** Follow the phase breakdown in the build prompt.
  After finishing a phase's deliverables, stop, summarize what was built,
  print the `git add` line + commit message, and wait for explicit approval
  before starting the next phase.
- Reuse existing C++ algorithm source from the native build wherever
  possible; only the build target, bindings, and GUI are new.
- Every new module gets at least a minimal test in `/tests`, runnable against
  the WASM build.
- Do not touch or scaffold the landing/marketing pages (`/`, `/about`,
  `/docs`) — that's handled separately.

## Build & run

Full details in `BUILDING.md`.

- Toolchain (once): `scripts/setup-toolchain.sh` (Emscripten 3.1.56 in
  `~/emsdk`, Qt 6.8.3 `wasm_singlethread` + host Qt in `~/Qt`, via aqtinstall),
  then `scripts/build-liquid-wasm.sh` (liquid-dsp 1.8.2 → `~/wasm-deps/install`,
  internal FFT, no fftw)
- WASM build: `scripts/build-wasm.sh` (qt-cmake chainloading Emscripten →
  `build-wasm/`, servable app staged in `build-wasm/dist/`)
- Native build: `cmake -B build && cmake --build build` (add
  `-DPython3_EXECUTABLE=<python with pybind11>` for the pybind module)
- Local serve: `scripts/serve.sh [port]` (plain static server on `dist/`)
- JS deps: `npm install` (onnxruntime-web, puppeteer-core); browser checks:
  `scripts/browser-smoke.sh` (ML chain), `scripts/browser-gui-test.sh` (the GUI);
  after `scripts/build-wasm.sh` run `scripts/stage-web.sh` to make `build-wasm/dist` servable
- Widget tests need a desktop Qt: `cmake -B build -DCMAKE_PREFIX_PATH=~/Qt/6.8.3/macos`
- Run tests: `ctest --test-dir build` (native) or
  `ctest --test-dir build-wasm` (WASM, runs under node; needs emsdk env; also
  runs the JS bridge suite)
- Tests use the dependency-free harness in `tests/harness.hpp`; register new
  ones with `sig_add_test` in `tests/CMakeLists.txt`.

## Status

Phase 1 done: toolchain, dual native/WASM CMake, Qt hello-world in the
browser. `/core` source copied from `~/signal-project/signal/core`.
Phase 2 done: liquid-dsp cross-compiled to WASM (internal FFT, no fftw); all of
`/core` (including `dsp.cpp`, `demod.cpp`) builds for WASM; `test_dsp` verifies
FFT/PSD/spectrogram/filter output on both targets. WASM build uses
`-fexceptions`.
Phase 3 done: demod (17 linear schemes + FSK), all 5 interleavers, and Viterbi /
RS / concatenated / LDPC round-trip tests run on both targets
(`test_demod`, `test_interleave`, `test_fec`), with golden hashes pinning the
native output so any native/WASM divergence fails the WASM run.
Known core limitation (inherited, deliberately not fixed): the LDPC
bit-flipping decoder cannot correct a single-bit error at codeword positions
7, 9, 13, 15 of a block (see the comment in `tests/core/test_fec.cpp`).
Phase 4 done: Embind bridge in `/bridge` (contract in `bridge/README.md`).
Bytes in: `ingestFile` -> new in-memory `parse_wav`/`parse_iq`/`ingest_bytes` in
`/core` (path loaders now wrap them; native pybind API unchanged). ML out:
continuation-passing `classify_async` (no Asyncify: it would instrument the
whole Qt app), validated result, JS errors surfaced as `Error`. Tested by C++
tests (`test_ingest`, `test_ml_result`) and a node JS suite
(`tests/bridge/bridge.test.mjs`).
Phase 5 done: `ml/export_onnx.py` (deterministic, verified vs PyTorch) ->
`ml/models/modulation_classifier.{onnx,json}` (softmax in the graph);
`make_spectrogram_image` ported to C++ (`core/src/ml_preprocess.cpp`, checked
against fixtures made by the ORIGINAL Python in `tests/data/ml/`);
`web/js/onnx-classifier.mjs` (onnxruntime-web, single-threaded) registered as
the bridge classifier; `classifyCapture` runs bytes -> C++ -> ONNX -> C++.
End-to-end matches the original to 1.4e-6 on 11 captures, in node and in real
headless Chrome (`scripts/browser-smoke.sh`). NOTE: the shipped weights are the
native prototype's and misclassify qpsk/psk8/qam16/qam64 fixtures; the port
reproduces that faithfully (see `ml/README.md`).
Phase 6 done: `app/pipeline/` (C++ port of pipeline/detect/fec/interleave/correlate/
io/demod; see its README). Pipeline split at the ML step (`pipeline_prepare` /
`pipeline_finish`); `sigbridge::run_pipeline_async` + Embind `runPipeline` stitch the
async ML call between them. Validated by differential tests against 600+ cases
recorded from the ORIGINAL native code (`tests/data/pipeline/`, generated by
`ml/make_pipeline_fixtures.py`): C++ (native + WASM) and JS through the real bridge
+ ONNX model, and headless Chrome. Faithfully reproduces native limits: convolutional
interleaver + FEC does not recover the message, and the detector misidentifies some
streams (see `app/pipeline/README.md`).
Phase 7 done: Qt Widgets GUI in `app/gui/` (main window, results panel, QPainter
waterfall + constellation, see `app/README.md`) running in the browser: "Open File..." is a
real `<input type="file">` feeding bytes to the app through the bridge; analysis runs
in the WASM module with the ML call-out to onnxruntime-web (shell: `web/index.html`,
`scripts/stage-web.sh`). Results panel text is byte-identical to the original PyQt panel
(72 scenarios). Tested by widget tests on desktop Qt (offscreen) and end to end in
headless Chrome (real clicks/typing/file chooser; `scripts/browser-gui-test.sh`).
