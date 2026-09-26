# app/pipeline: C++ port of the native app's orchestration

Plain C++ on top of `sigcore_core` (no Qt, no Embind), built and tested natively
and under WASM. One file per module of the native Python app:

| Native (Python) | Here | Notes |
| --- | --- | --- |
| `io.py` | `signal_io` | takes bytes + filename (no path); format by extension, like the original |
| `demod.py` | `demod_dispatch` | scheme *names* -> `sigcore` modulators/demodulators |
| `interleave.py` | `interleave_dispatch` | `InterleaverSpec` replaces the `**params` dict |
| `fec.py` | `fec_dispatch` | bits <-> bytes packing identical to `np.packbits` |
| `correlate.py` | `correlate` | autocorrelation is exact (packed XOR+popcount), bit-identical to numpy |
| `detect.py` | `detect` | same candidate order, scoring, tie-breaks |
| `pipeline.py` | `pipeline` | **split at the ML step**, see below |

## The ML split

The original calls the classifier in the middle of one synchronous function. Here
the classifier is asynchronous JS, so `pipeline_prepare` (ingest + spectrogram) and
`pipeline_finish` (demod ... correlate) are separate, and `bridge/`'s
`run_pipeline_async` runs the ML call-out between them. `run_pipeline` chains
both given a known `Classification`, which is what the C++ tests use.

## How it is validated (not by inspection)

`ml/make_pipeline_fixtures.py` runs the ORIGINAL native code over 600+ cases and
records every output, including errors, in `tests/data/pipeline/*.rec`:

| Records | C++ test | JS test |
| --- | --- | --- |
| 139 correlate | `test_correlate` | |
| 251 fec + 44 interleave | `test_dispatch` | |
| 90 detect (full FEC x interleaver grid, noise, degenerate sizes, parameter sweeps) | `test_detect` | |
| 82 pipeline scenarios (demo captures with the real classifier, all modulations, WAV, noisy, auto-detect for every pairing, 15 error cases) | `test_pipeline` | `tests/bridge/pipeline.test.mjs` (through the real bridge and ONNX model) |

Bit arrays must match exactly; floating-point values match exactly except the
spectrogram's dB data (compared by mean, since native fftw and WASM's internal FFT
differ in the last float bits).

## Faithful to the original, including its limits

The port reproduces the original's behaviour, not an idealised version of it:

- **Convolutional interleaver + FEC does not recover the message.** After
  auto-detection the pipeline re-deinterleaves the *untrimmed* bits, while
  `detect_params` scored the candidate with the convolutional flush prefix
  removed, so the FEC decoder sees misaligned data (measured: 189 bit errors in
  378, i.e. chance). The detector also misidentifies some LDPC/RS streams behind
  a convolutional interleaver (e.g. LDPC found as "no FEC", RS not found).
- **LDPC** cannot correct single-bit errors at codeword positions 7, 9, 13, 15
  (see `tests/core/test_fec.cpp`), which affects what the FEC round-trip gate accepts.
- Format is chosen by file extension (`.wav`/`.iq` only). The bridge's plain
  `ingest` is more lenient (it sniffs content); the pipeline keeps the original's rules.

Fixing any of these is a deliberate decision to diverge from the native app;
do it in both projects, then regenerate the fixtures.
