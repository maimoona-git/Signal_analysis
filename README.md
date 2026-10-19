# PS 26147: signal analysis in the browser

SIH 2026 prototype for NTRO's PS 26147. It ingests raw `.IQ` / `.wav` / `.sigmf-data` captures,
identifies the modulation with an ML model, demodulates, de-interleaves, FEC-decodes and correlates
the bitstream to find header/payload boundaries, showing every stage: a waterfall, a constellation
and a results panel.

Beyond the PS's core requirements, it also targets the PS's own stated real-world problem --
signals "recorded from different sensors and different locations" where "parameters may vary":
pulse-shape-aware timing recovery (RRC vs. unshaped/NRZ), blind samples-per-symbol estimation, and
blind wideband channel finding (locating and extracting a signal that isn't at baseband within a
wider capture). See [app/pipeline/README.md](app/pipeline/README.md)'s "Real-world signal support"
sections for how each one works and what was verified.

This is the native PyQt6 desktop app **re-platformed to run entirely in the browser**: the same
C++ DSP/FEC core compiled to WebAssembly, the GUI as Qt Widgets compiled to WebAssembly, and the
classifier exported to ONNX and run with onnxruntime-web. No server, no Python at runtime; nothing
leaves the machine. It needs the host to serve its files (it has no service worker, so it
is not an installable offline app), but once loaded, analysis makes no network requests.

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

365 automated tests plus 608 recorded comparisons against the original native app. Every ported
function is compared with the original's own output, including where it errs. Since the
gap-list work, five recorded pipeline scenarios deliberately differ from the original (bugs fixed
in the convolutional-interleaver trim, the header pattern, and one-shot messages the original's
auto-detect missed), and the GUI panel adds status and source lines the original never had. [tests/COVERAGE.md](tests/COVERAGE.md) maps
each of the native app's 118 tests to what covers it here.

### Features added since the original native app

Beyond the direct port, this project adds: soft-decision (LLR) decoding, a blind
rank-analysis view (finds a linear block code's length and rate from the bitstream
alone), decode-driven parameter search (tries every interleaver x FEC x bit-offset
combination and ranks by actual decode correctness), a frame-raster view (frame
length, sync word, counter/payload columns), matched-filter timing and carrier
recovery for real (not just synthetic) captures, a configurable transmit pulse shape
(RRC vs. unshaped/NRZ) for that recovery, blind samples-per-symbol estimation, and
blind wideband channel finding (locating and extracting a signal that is not at
baseband within a wider capture). See [app/pipeline/README.md](app/pipeline/README.md)
for how each one works and what was verified.

## Things to know

- **The shipped model is only as good as the native prototype's weights.** It misclassifies some
  modulations (`ml/README.md`). The port reproduces the original's outputs exactly; better weights
  need retraining in the native project and a re-export.
- **The port is faithful, including the original's limits:** convolutional interleaver + FEC does not
  recover the message, and the blind detector misidentifies some streams (`app/pipeline/README.md`).
- **Only synthetic captures are committed.** `.iq`/`.wav`/`.sigmf-data`/`.sigmf-meta` files are
  gitignored everywhere except `tests/data/` (recorded fixtures) and `demo/` (synthetic demo
  captures, see `demo/README.md`) -- a real off-air recording used while testing locally should
  never end up in the repo's history.
- **Big files:** a raw capture over 128 MiB is never read whole. The page reads one 128 MiB slice
  (about 16M float32 samples) with `Blob.slice`, the panel says which samples of the file that is,
  and "Previous window" / "Next window" step through the rest. Verified in real Chrome with a
  300 MB file; a 2 GB file has not been tried. The earlier "512 MB in ~2 s" timing predates all of
  this and has not been re-measured. A .wav is always read whole.
- **Auto-detect has a size limit:** above 300,000 demodulated bits (or 2M samples for the retries)
  it is skipped and the panel says so, because trying hundreds of interleaver and FEC candidates on
  a stream that large took 46 s on 16M samples. A smaller window, or explicit settings, still work.
- **The page stays responsive during analysis:** the pipeline runs in a Web Worker (a second
  WASM instance), so a 5 s search on an undecodable capture no longer freezes the UI -- verified
  in real Chrome (state queries answered in 1 ms throughout). The searches are still capped at
  5 s. If a browser cannot start the worker the app falls back to analysing in the page, which
  does block it for that long. The result crosses between the two instances as one buffer, so a
  128 MiB window briefly exists several times over in memory.
- Development history and decisions are recorded in [CLAUDE.md](CLAUDE.md).

## Exporting results

"Export Result..." saves the last analysis as `<capture>_analysis.json` (via the browser's
download, nothing is uploaded). The file records the capture's file name, sample rate and where
it came from, the modulation and confidence, every parameter used or estimated, the decode
status (verified / structured / unverified), and the decoded message. Treat it with the same
care as the capture itself: the message and file name are in it in clear text. The app keeps
no log of analyses and sends nothing off the machine; everything runs in the browser tab.

## License

[GPLv3](LICENSE). Qt is statically linked into the WASM build; under Qt's own
LGPL terms, a static build is only free to distribute if the project using it
is released under a GPL-compatible license, which is why this one is GPLv3
rather than something more permissive.
