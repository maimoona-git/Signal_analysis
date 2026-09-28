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

## Feature 1 (SIH26147 extra feature, descoped): decode-driven parameter search

`param_search.hpp`/`param_search.cpp` try every (bit offset x interleaver x FEC)
combination against the raw demodulated bits and rank ALL of them by how much a
decode looks *correct* -- not by how often a header happens to repeat, which is what
`detect.hpp`'s existing heuristic does (and does well, for a message that repeats:
that module is left completely unchanged here, still matching the original app
decision for decision). "Correct" reuses Feature 3's own re-encode-and-compare metric
(`reencode_agreement`): decode, re-encode, and see how much of what was actually
received comes back.

**The bug this exists to fix, found during this port's own testing:** a real report
during testing -- a single LDPC-encoded message, no repeats at all -- showed the
results panel a hex-encoded blob instead of the real text. Tracing it back: with
nothing to repeat, `detect.hpp`'s header-repeat score has nothing reliable to rank on,
and picked "no FEC" over a genuinely-decodable LDPC hypothesis on exactly that kind of
one-shot capture. This search's ranking is built to not have that gap: a real decode
match is the primary signal, and a header-repeat count is used only as a fallback for
candidates with no FEC hypothesis to check at all, deliberately capped well below any
score a real decode could report (see `test_param_search.cpp`'s test of that exact
scenario).

**Every FEC decoder needs its own noise baseline, found by measuring, not assumed:**
Viterbi and LDPC both always produce SOME output for any input at all (a
nearest-codeword / best-effort search), so decoding pure noise then re-encoding it
still agrees with that noise far more than a naive "agreement > 50%" floor would
expect -- measured (on nonlinear-mixed bits, not testutil's own linear xorshift Rng;
see test_param_search.cpp's own note on why that specific choice matters here) at
~87% for Viterbi and ~59-72% for LDPC, purely from random input. Reed-Solomon and
concatenated mostly just THROW on pure noise instead (self-filtering: RS(15,13) can
only correct one symbol error per block, and random data essentially never satisfies
that). Confidence is normalised against each scheme's own measured baseline
(`noise_baseline` in param_search.cpp), not a flat threshold, exactly the same lesson
Feature 2 learned from GF(2) rank's own non-zero random baseline.

**Descoped, like the rest of this port's extra features:** the feature spec's fuller
version implies a blind receiver front end (searching modulation and sample rate too).
This pipeline has no timing recovery or pulse shaping to search over -- modulation and
sample rate are given here exactly as everywhere else -- so this searches interleaver,
FEC, and a small bit-offset ("phase") window instead, which is the part that is
actually missing from `detect.hpp`'s own search.

**Cost, and why it is opt-in, not automatic like Features 2 and 4:** every candidate
costs a real interleave + FEC decode + re-encode attempt, and there are a lot of them
(measured: ~3.4s natively at the default 200,000-bit cap, offset search included makes
it worse). Unlike the always-on raster/rank-analysis views, this only runs when the
GUI's "Search Parameters" button is pressed, on the last analysis' raw demodulated
bits -- exactly the situation it exists for (auto-detect's single guess turned out
wrong), not every run.

## Feature 2 (SIH26147 extra feature): rank-analysis view

`rank_analysis.hpp`/`rank_analysis.cpp` find a linear block code's length BLIND, with no
knowledge of the FEC scheme and no decode attempt: fold the bitstream into an R x W
matrix for a range of candidate widths W and compute its rank over GF(2). A width that
is not a real code boundary folds essentially independent rows; a width that IS the
codeword length folds every row into the same k-dimensional code subspace, so the rank
saturates at k long before W rows have been seen -- an excess rank *deficit* that stands
out. Where it appears is the code length; the rank there estimates the dimension, so
rank/width estimates the code rate. Runs on `PipelineResult::demodulated_bits`, before
de-interleaving or decoding either one, since decoding removes exactly the redundancy
this looks for.

**The random baseline is a real floor, not zero — found while building this, not
assumed:** a random GF(2) matrix is surprisingly often *not* full rank (corank 0 happens
only ~29% of the time for a square one); the expected corank converges quickly to a
known constant, ~0.7869, independent of matrix size once the smaller dimension isn't
tiny. Comparing against a flat zero floor would flag nearly every width as "deficient"
from this alone; every width's excess here is measured against
`baseline_deficiency(rows, width) = max(0, width - rows) + 0.7869` instead, and that
excess's own tail decays extremely fast (unlike Feature 4's autocorrelation, this does
not need to scale with capture size), so a small absolute threshold (3 bits, by default)
is already a safe one.

**Known limitation, found while measuring this, not assumed:** bit errors are far more
damaging here than to the frame raster. A single flipped bit in one row can, on its own,
add one whole new independent dimension to the observed rank. Measured on a real
LDPC(16,8) codeword stream (8 redundant bits — real headroom, not a toy code): a per-bit
error rate of as little as 0.5%, applied independently across a few thousand rows in one
big matrix, already erases the spike; by 2% it is gone. `RankAnalysisOptions::
window_rows` splits the rows into many small windows and takes the *median* rank across
them instead of one big elimination -- a window untouched by an error still reports the
true rank exactly, and the median survives a minority of corrupted windows outvoting the
rest, but this is a real, bounded mitigation, not a fix: it needs a majority of windows
to stay clean, and does nothing for a capture where errors are spread evenly through
every window (`test_rank_analysis.cpp`'s "honest limit" test measures, rather than
asserts, that the unwindowed default is worse on exactly the same corrupted stream).

**No offset search:** folding always starts at bit 0, the same assumption the rest of
the pipeline already makes (`find_header_payload_boundaries` looks for its header from
the start of the stream too). A capture whose first codeword does not begin at bit 0
will not show a clean spike; phase synchronization is a possible extension, not
implemented here.

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
