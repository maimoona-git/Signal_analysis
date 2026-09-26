# PS 26147: signal analysis in the browser

SIH 2026 prototype for NTRO's PS 26147. It ingests raw `.IQ` / `.wav` captures, identifies the
modulation with an ML model, demodulates, de-interleaves, FEC-decodes and correlates the bitstream
to find header/payload boundaries, showing every stage: a waterfall, a constellation and a
results panel.

This is the native PyQt6 desktop app **re-platformed to run entirely in the browser**: the same
C++ DSP/FEC core compiled to WebAssembly, the GUI as Qt Widgets compiled to WebAssembly, and the
classifier exported to ONNX and run with onnxruntime-web. No server, no Python at runtime; nothing
leaves the machine, and it works offline.

## Run it

```bash
scripts/setup-toolchain.sh        # once: Emscripten 3.1.56 + Qt 6.8.3 for WebAssembly (no Qt account needed)
scripts/build-liquid-wasm.sh      # once: liquid-dsp for WASM
npm install                       # once: onnxruntime-web (+ puppeteer-core for the browser tests)
scripts/build-wasm.sh             # builds the app and stages the static site in build-wasm/dist
scripts/serve.sh                  # http://localhost:8080/ : drop a .wav or .IQ file on the page
```

`build-wasm/dist` is the whole product: 10 files, 29 MB (9 MB gzipped), all references relative.
Host it on any static file server (GitHub Pages, S3, nginx, `python3 -m http.server`), under any path.
No special headers are needed (the build is single-threaded). The server must send `.wasm` as
`application/wasm` and `.mjs` as JavaScript, which every mainstream host does. See
[BUILDING.md](BUILDING.md) for details.

## How it fits together

```
 browser file  ──▶  JS shell (web/)  ──▶  WASM module (Qt Widgets GUI + C++ pipeline)
 <input type=file>       │                     ingest ▶ spectrogram ▶ ML ▶ demod ▶ de-interleave
 or drag-and-drop        │                                            │      ▶ FEC decode ▶ correlate
                         └── onnxruntime-web ◀── Embind bridge ◀──────┘  (async call-out, result back)
```

| Directory | What is in it |
| --- | --- |
| [core/](core) | The C++ DSP and FEC engine (liquid-dsp based), shared by the native and WASM builds |
| [app/pipeline/](app/pipeline) | C++ port of the native app's orchestration: blind detection, correlation, dispatch, pipeline |
| [app/gui/](app/gui) | The Qt Widgets GUI; see [app/README.md](app/README.md) |
| [bridge/](bridge) | The Embind bridge: file bytes in, ML call-out to JS, results back; see [bridge/README.md](bridge/README.md) |
| [ml/](ml) | Dev-time Python: model definition, ONNX export, test-fixture generators; see [ml/README.md](ml/README.md) |
| [web/](web) | The page shell and the JS that runs the model |
| [tests/](tests) | The test suite and its fixtures; see [tests/COVERAGE.md](tests/COVERAGE.md) |
| [scripts/](scripts) | Toolchain setup, builds, serving, verification |

## Testing

```bash
cmake -B build -DCMAKE_PREFIX_PATH=~/Qt/6.8.3/macos && cmake --build build && ctest --test-dir build   # native + GUI widgets
scripts/build-wasm.sh && ctest --test-dir build-wasm      # WASM under node, JS suites, real Chrome, static-site check
```

205 automated tests plus 608 recorded comparisons against the original native app. Every ported
function is compared with the original's own output, including where it errs, and the results
panel's text is byte-identical to the original GUI's. [tests/COVERAGE.md](tests/COVERAGE.md) maps
each of the native app's 118 tests to what covers it here.

## Things to know

- **The shipped model is only as good as the native prototype's weights.** It misclassifies some
  modulations (`ml/README.md`). The port reproduces the original's outputs exactly; better weights
  need retraining in the native project and a re-export.
- **The port is faithful, including the original's limits:** convolutional interleaver + FEC does not
  recover the message, and the blind detector misidentifies some streams (`app/pipeline/README.md`).
- **Size limit:** captures up to at least 512 MB analyse in about 2 s; 768 MB does not fit in the
  browser's memory and fails with a clear message.
- Development history and decisions are recorded in [CLAUDE.md](CLAUDE.md).
