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
records every output, including errors (and, for the GUI, the text the original
results panel shows), in `tests/data/pipeline/*.rec`:

| Records | C++ test | JS test |
| --- | --- | --- |
| 139 correlate | `test_correlate` | |
| 251 fec + 44 interleave | `test_dispatch` | |
| 90 detect (full FEC x interleaver grid, noise, degenerate sizes, parameter sweeps) | `test_detect` | |
| 84 pipeline scenarios (demo captures with the real classifier, all modulations, WAV, noisy, auto-detect for every pairing, 12 cases where the original raises) | `test_pipeline` | `tests/bridge/pipeline.test.mjs` (through the real bridge and ONNX model) |

In the browser the ML model needs at least 64 samples (one FFT window), so 5 of the
original's successful scenarios (captures of 8, 9, 10, 32 and 48 samples, which its stub
classifier never looked at) are rejected there; `pipeline.test.mjs` expects exactly that.

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

## Feature 4 (SIH26147 extra feature): frame raster view

`raster.hpp`/`raster.cpp` find the repeating frame length (autocorrelation, smallest
statistically-real peak — a fixed lag beats noise once its correlation clears an
absolute floor that scales with `1/sqrt(n)`, not a fraction of the strongest peak
anywhere in range: a counter field's own LSB *decorrelates* the stream at the true
period and correlates at even multiples of it, so demanding a fraction of a possibly
inflated harmonic can reject the real answer), fold into a frame x bit matrix, re-align
rows for slips (ranked-column reference, not a fixed threshold, since a truly constant
column's measured stability dilutes with however many rows are actually corrupted), and
label columns constant / counter-like / payload. The sync word is the leading constant
run; the whole stream is re-searched for it with a little bit-error tolerance.

**Known limitation, found while testing:** a repeating header has its own internal
autocorrelation. Since it recurs identically every frame, an arbitrarily-chosen header
can make the detector lock onto a shorter *internal* repeat instead of the intended
frame length — honestly, not a wrong-but-confident guess, just the wrong real
periodicity. Real protocols solve this by choosing a low-autocorrelation sync word
(e.g. a Barker code); this is not a bug to fix here, it is why that practice exists.

**Performance cap:** a huge capture can fold into an enormous row count if the detected
period is small, and every later stage costs at least one pass per row. `RasterOptions::
max_analysis_bits` (default 500,000) bounds this regardless of capture size, exactly like
the GUI's waterfall column cap (`app/README.md`) — found the same way, by measuring a
real large capture, not assumed.

## Feature 3 (SIH26147 extra feature): soft-decision decoding

Instead of forcing each received symbol to the nearest constellation point, `core/
include/soft_demod.hpp` reports a log-likelihood ratio (LLR) per bit -- how confident
the demodulator is, not just its guess (convention: LLR = log(P(bit=0)/P(bit=1));
positive means the bit is probably 0). One generic max-log formula, working from the
scheme's full constellation, covers every linear scheme (BPSK through QAM256/ASK/OOK);
noise variance is estimated decision-directed (hard-demodulate, re-modulate, measure
the error). De-interleaving (`core/include/interleave.hpp`'s float overloads -- every
interleaver here is a fixed gather/permutation, so the same code moves LLRs or bits)
and FEC decoding (`viterbi_decode_soft`, `ldpc_decode_soft`, a normalised min-sum
belief-propagation decoder on the same parity-check matrix as the hard bit-flipping
one) then consume the LLRs directly, ending in ordinary hard decoded bits -- soft vs
hard differ only in *how* that decode happened, not in the type it produces.

`PipelineOptions::soft_decoding` turns this on; it silently falls back to the hard path
when it does not apply (`PipelineResult::soft_decoding_used` says which one ran).
`reencode_agreement` (the feature spec's Section 2.3 scoring primitive: re-encode the
decoded message, compare against what was actually received) is computed in both
modes whenever a FEC scheme was used, so hard and soft are directly comparable.

**Verified, not assumed:** bit error rate is measured to be no worse with soft
decoding than hard, across an SNR sweep, for both Viterbi and LDPC (`test_soft_decode.
cpp`), and end to end through the whole pipeline and the real bridge (`test_pipeline.
cpp`, `tests/bridge/pipeline.test.mjs`).

**Known limitation, found while building this, not assumed away:** FSK has no soft
demodulator here. liquid's FSK modulator is phase-continuous across symbols (each
symbol's waveform depends on where the previous one left the phase), so a reference
tone built for one symbol in isolation is not what that symbol looks like inside a
real sequence -- confirmed by measurement (the true transmitted symbol's isolated-tone
distance was not even the smallest of the candidates on a test block). A correct
metric would need to re-synthesise the whole downstream waveform per hypothesis, which
is real additional work, not a small fix. Soft decoding therefore covers every linear
scheme, which is what Viterbi/LDPC are paired with in practice; FSK stays hard-decision
only, and the pipeline says so via `soft_decoding_used`.
