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

## Real-world signal support: carrier and timing recovery

Everything above -- and every demo/test capture in this repo -- is built with this
project's own synthetic convention: a linear scheme's samples land exactly one per
symbol, at zero carrier offset, because nothing generated them any other way. A real
recording (an actual RF capture, or a downloaded file that happens to carry raw or
WAV-wrapped IQ) is not built that way: it has real transmit pulse shaping, an unknown
fractional symbol-timing phase (possibly drifting), and a carrier frequency/phase
offset from the receiver not being perfectly tuned. Demodulating it the same way this
pipeline demodulates its own captures -- naive `[0, sps, 2*sps, ...]` decimation --
only happens to work when none of that is present.

`core/include/recover.hpp`'s `recover_linear` closes part of that gap, for linear
schemes (PSK/QAM/ASK/OOK), using liquid-dsp's own building blocks rather than
hand-rolled loops: an M-th-power coarse frequency estimate (via `dsp.hpp`'s existing
FFT), a timing recovery stage selected by `PipelineOptions::pulse_shape` (see below),
and a decision-directed Costas loop (`nco_crcf` steered by the real modem's own
phase-error estimate) for fine carrier tracking. `PipelineOptions::recover_timing`
turns the whole stage on; off by default, since it changes what gets decoded and
every existing golden-vector test assumes the old naive-decimation behaviour.
`PipelineResult::recovery_used` says whether it actually ran.

**Pulse shape is a real, separate axis, not a detail of timing recovery.**
`PipelineOptions::pulse_shape` (default `PulseShape::Rrc`) picks which transmit
pulse shape the timing stage assumes: `Rrc` uses an RRC-matched polyphase symbol
synchronizer (`symsync_crcf`), the right choice for a real transmitter that
pulse-shapes its output. `Rect` assumes no shaping at all (simple NRZ/on-off
pulses -- PySDR's own `bpsk_rect_20sps.sigmf-data` test file, in particular) and
uses an integrate-and-dump (boxcar) filter at whichever fixed sample phase gives
the largest average symbol magnitude instead. These are not interchangeable: an
RRC filter is not a matched filter for a rectangular pulse, and using it on one
measurably hurts the lock rather than just failing to help (see
`core/include/recover.hpp`'s own comment on `PulseShape`).

**Verified, not assumed:** `tests/core/test_recover.cpp` builds a signal impaired with
liquid's own TX-side RRC interpolator (the correct counterpart to the RX-side matched
filter here -- a cruder stand-in, tried first, turned out to prove nothing either way;
see that file's own comment), a carrier offset, noise, and an arbitrary start phase,
and confirms recovery locks onto it while naive decimation does not.
`tests/core/test_pipeline.cpp` goes further, end to end: the same kind of impaired
capture, through the exact same FEC-encode chain and file format the real pipeline
decodes, run through `run_pipeline` and Feature 1 (Parameter Search, since a real
capture's receiver does not know its exact framing any more than it knows its exact
timing) -- naive decimation cannot be rescued even by Parameter Search, `recover_timing`
decodes the message correctly. The same two files carry matching tests for
`PulseShape::Rect` against a genuinely unshaped (zero-order-hold) capture, confirming
the *reverse* is also true: `Rrc` (the default) is measurably worse than `Rect` on a
signal that was never RRC-shaped to begin with, not just unhelpfully neutral.

**Known limitations, found while building this, not assumed away:**
- **Not blind.** The caller still supplies the nominal samples-per-symbol (already a
  required input everywhere else in this pipeline); this recovers the timing *phase*
  and carrier, not the symbol *rate* itself.
- **`Rect`'s lock point can land mid-codeword, and there is no fix for that here.**
  `Rrc`'s matched filter always produces one output per input symbol from the very
  start (early ones are a filter-startup transient, but present); `Rect`'s
  integrate-and-dump instead locks onto the first *complete* symbol and simply omits
  any partial one before it, so a start offset that is not a multiple of the nominal
  samples-per-symbol can lose exactly one bit from the very front of the recovered
  stream. For a rate-1/2 convolutional code that desyncs bit-pairing for the entire
  decode, not just the first character -- unlike Parameter Search's ordinary offset
  scan (which only ever *skips forward* into already-available data and cannot
  recover a bit that was never there), this is exactly the class of problem the PS's
  own bitstream-correlation stage (Section v) exists to solve, so it is left there
  rather than patched into this stage.
- **FSK is out of scope here.** Its own frequency-discriminator demodulation is
  already far more tolerant of timing/carrier error than PSK/QAM's phase-sensitive
  slicing, and liquid's matched-filter symbol synchronizer used here is built for
  linear modulations' pulse shapes, not FSK's tone bursts.
- **Phase ambiguity.** PSK/QAM constellations are rotationally symmetric (BPSK:
  2-fold, everything else here: treated as 4-fold, an approximation above QPSK -- see
  `recover.hpp`'s own comment), and a decision-directed carrier loop can lock onto any
  of them equally validly: the recovered stream is only correct up to one of that many
  constant, unknown phase rotations. Resolving it needs a known sync word to test
  rotations against, or differential coding -- neither implemented here. In practice
  Parameter Search's bit-offset search plus its FEC re-encode-agreement scoring (see
  Feature 1 above) already resolves this for a message with FEC, as the test above
  demonstrates, by simply never ranking a garbled (wrong-rotation) decode highly; a
  capture with no FEC at all has no such signal to check against and may come back
  bit-inverted.

## Real-world signal support: blind symbol-rate estimation

`recover_timing` above still needs the caller to supply an approximate nominal
`samples_per_symbol` -- a real, required GUI/API input everywhere else in this
pipeline too. `core/include/symbol_rate.hpp`'s `estimate_symbol_rate` closes that
gap partway: a delay-and-multiply nonlinearity (`|x[n] - x[n-1]|^2`, which spikes at
symbol boundaries for any modulation and pulse shape, unlike a plain envelope, which
is flat for a constant-modulus scheme) followed by an FFT peak search finds a
symbol-rate estimate blind, with a confidence score.
`PipelineResult::symbol_rate_estimate` is always attempted (unset, not an error, for
a capture under 256 samples) and is purely advisory -- it never overrides
`PipelineOptions::samples_per_symbol`, which stays the value demodulation actually
uses; it exists so the GUI can show a suggested value next to that field.

**Verified, not assumed:** accurate to within 0.01% across pulse shapes, modulations
(constant- and non-constant-modulus) and rates 2-32 in `tests/core/test_symbol_rate.
cpp`, with a confidence metric that reads ~0 on pure noise (an earlier version read a
misleadingly high ~0.7-0.8, an extreme-value-statistics artifact of searching many
FFT bins, found and fixed by measuring against noise-only input directly) and
degrades to ~0 exactly where the estimate itself stops being trustworthy, not just
where it gets noisy.

## Real-world signal support: blind wideband channel finding

Everything above still assumes the capture itself is narrowband -- the signal of
interest occupies (most of) the sample rate. A real capture spanning more bandwidth
than one signal occupies (the PS's own stated case: data "recorded from different
sensors and different locations") is not narrowband at all.
`core/include/channel_finder.hpp` closes that gap in two steps: `find_channels`
blindly detects candidate occupied sub-bands via PSD peak/plateau detection against
a robust (median) noise floor, and `extract_channel` isolates one candidate to
baseband (mix down, lowpass filter, decimate) so it can be fed into everything above
exactly like a capture that was narrowband to begin with.

Unlike `recover_timing`/`pulse_shape` (which only change *how* demodulation runs),
picking a channel changes what the *rest of the result* reflects -- spectrogram,
`symbol_rate_estimate`, demodulated bits all come from the extracted channel, not the
original wideband capture -- so this is deliberately a two-step, explicit flow, the
same shape as Feature 1's Parameter Search: `PipelineOptions::find_channels_enabled`
alone reports `PipelineResult::channel_candidates` (sorted strongest first) without
changing anything else; `PipelineOptions::select_channel` (an index into that list)
then actually applies one, and `PipelineResult::channel_extracted` says whether it
did (false, not an error, for an out-of-range index).

**Verified, not assumed:** `tests/core/test_channel_finder.cpp` proves the actual
end-to-end claim -- a real BPSK+FEC message embedded away from DC in a wideband
capture, found, extracted, and decoded bit-perfectly through the exact same
`recover_linear` this project already uses for a real-world capture -- plus
multi-signal separation (two simultaneous channels correctly kept distinct) and
correct silence on pure noise. `tests/core/test_pipeline.cpp` repeats the same claim
through the full pipeline (`find` -> `extract` -> read `symbol_rate_estimate` for the
*new*, decimated rate, since the original nominal `samples_per_symbol` was only ever
true at the wideband rate -> `recover_timing`).

**Two margins, both found by measuring, not assumed:** `extract_channel`'s lowpass
filter cutoff needs headroom *past* the blindly-detected bandwidth (not sit exactly
at it) -- a detected bandwidth is necessarily a bit narrower than a real pulse-shaped
signal's true edge, and clipping that edge distorts the pulse shape before it ever
reaches `recover_linear`'s own matched filter, the same mismatched-filter lesson
`PulseShape::Rect`'s own comment describes, here from the wrong filter *width*
instead of the wrong filter *family*. Separately, the decimation ratio needs its own
headroom, because a caller only learns `samples_per_symbol` *after* extraction (via
`estimate_symbol_rate` on the result): a mismatched bandwidth detection can leave
that a non-integer ratio, and even a few percent of mismatch there was enough to
fully desync `recover_linear`'s own fixed-rate symbol synchronizer over a real
message's length, not just add noise. Both were found directly (a real embedded
message that decoded cleanly failed almost totally, ~43% bit errors, chance level,
with too-tight margins) and are documented in `channel_finder.cpp`'s own comment.

**Known limitation, found while building this, not assumed away:** searching Feature
1's Parameter Search over an *extracted* channel's demodulated bits inherits its own
pre-existing sensitivity to bit-offset *parity* for a rate-1/2 convolutional code
(one bit missing or misplaced desyncs the code's pairing entirely, the same
underlying reason `PulseShape::Rect`'s own limitation above exists) -- confirmed
directly while building this: the exact correct alignment for one real test capture
happened to land on an odd bit offset, which is fundamentally undecodable for
Viterbi regardless of how correct the recovery itself is (checked directly: the
recovered bits matched the true transmitted stream with zero errors at that exact
offset). This is a pre-existing fragility in Parameter Search's own Viterbi-candidate
generation, not something channel finding introduces, and is left there rather than
patched into this stage, for the same reason as `PulseShape::Rect`'s own note above.

## Link-layer verification, pulse search and the blind modulation check

Found by running real over-the-air recordings (the ten CPSat beacon captures, BPSK-G3RUH at 9,600 bit/s,
each with its published AX.25 frame) through the app. None of the FEC families applies to them, so
nothing could verify a decode, and the shipped classifier called all ten "fsk". Three additions came
out of that, all with auto-detect on:

- **`framing.hpp`.** NRZI, the G3RUH scrambler (x^17 + x^12 + 1), HDLC flags and bit stuffing, the
  CRC-16/X-25 frame check and the AX.25 address field, tried over four chains at both polarities. A frame
  whose CRC checks and whose addresses parse is very unlikely to be chance (random data produced none in
  300 streams of 20,000 bits), so it counts as verification (`decode_status` Verified, `frames`,
  `framing_chain`). A frame still open when the stream ends is accepted: filter delay routinely cuts the
  closing flag.
- **Pulse search ordered cheap-first.** `pipeline_finish` now tries the requested pulse, then every
  roll-off (0.25, 0.35, 0.5, 0.75, 1.0) and Rect, with *only* the framing verifier (milliseconds each)
  before any FEC search. Before that, the FEC searches used the whole time budget on the first try and
  the other roll-offs were never reached. With the default roll-off 0.35 four of the ten recordings were
  bit-perfect; with the search all ten are.
- **`modulation_blind.hpp`.** When the label is "fsk", the raw samples must fail three tests before the
  pipeline re-reads them as BPSK or QPSK: the blind FSK estimator doubts it, the envelope varies (std/mean
  >= 0.3 over the strongest 60% of samples) and the 2nd- (BPSK/ASK/OOK) or 4th-power (QPSK) spectrum has a
  line. Calibrated on 600 synthetic FSK signals (envelope variation never above 0.19: none flagged) and the
  ten real recordings (0.55-0.84: all flagged). **Honest limit:** the envelope test needs a *burst* (idle
  noise around the signal, as real captures have); a continuous, steady shaped-BPSK stream has an envelope
  as flat as noisy FSK, and the check stays silent on it by design.

Also found on the way: `fft_forward` took 17 s on a length with a large prime factor (a real recording's
97,919 samples cost 16 s in carrier recovery); such lengths now use Bluestein's algorithm (same answer,
~20 ms).

What this does **not** show: that the app reads arbitrary real protocols. AX.25 over G3RUH is one common
case; other link layers (pager BCH, other framings) are not implemented.

## CCSDS RS(255,223) + Viterbi (`ccsds_concat`)

`core/src/fec_ccsds.cpp` adds the CCSDS 131.0-B Reed-Solomon code (field polynomial 0x187, roots
(alpha^11)^112..143, conventional or Berlekamp dual basis) followed by the existing K=7 rate-1/2 Viterbi
code. The parity of a fixed 223-byte message, in both bases, matches the independent libfec reference
implementation byte for byte (`tests/core/test_fec_ccsds.cpp`); the decoder corrects 16 byte errors per
block and refuses 17+. It is searched by Parameter Search only (not by the original auto-detect).

The older `rs255` scheme is the common conventional-basis RS(255,223) over 0x11D, *not* the CCSDS code.
**Not implemented:** the standard's interleaving depth, pseudo-randomiser and sync marker, so a real
CCSDS downlink needs those undone first. No real CCSDS recording was available to try it on.

### Padded streams from high-order demodulators

8PSK and QAM64 carry 3 and 6 bits per symbol, so the demodulator pads the last symbol and the stream can be up
to 7 bits longer than the code word. Parameter Search now ignores up to 7 trailing bits when that is what makes
the length valid for a scheme (not when an interleaver is involved). Measured on 240 synthetic captures
(6 modulations x 5 SNRs x 8, Viterbi-coded, 1 sample/symbol): verified-and-correct decodes went from 57% to 66%
overall and 8PSK from 0% to 57%. The classifier's top guess was right in only 28% of them; trying every
modulation without it gave the same 66%, so the final answer comes from the decode check, not the label. One
of 240 captures verified under a wrong modulation in that try-everything mode.

### Dense constellations through timing and carrier recovery

Realistic captures (RRC pulse, 8 samples/symbol, a carrier offset, 12 trials per modulation at 15-30 dB
per-sample SNR) decoded as follows, before and after the changes below:

| | BPSK | QPSK | 8-PSK | QAM16 | QAM64 |
| --- | --- | --- | --- | --- | --- |
| before | 12 | 11 | 1 | 8 | 6 |
| after | 12 | 11 | 12 | 12 | 10 |

Causes, all in `core/src/recover.cpp` and `pipeline.cpp`: the 8-PSK carrier-offset estimate used the 4th
power (it needs the 8th); only 4 of 8 phase rotations were tried for 8-PSK; the default carrier-loop
bandwidth (0.02) slipped cycles on 8-PSK (0.001 holds lock); the symbol-timing loop spent the first few
hundred symbols settling, corrupting the start of a short capture (now run once over the first 256 symbols
and restarted); QAM is tracked with a feed-forward 4th-power phase estimate over a 64-symbol window instead
of the decision-directed loop, and normalised to unit power; and the four rotations each ran a full
Parameter Search of about 4 s against an 8 s budget, so the right one was often never reached (a cheap Viterbi
probe now picks the likely rotation first). Synthetic captures only, not real recordings.

Decode rate with the right modulation forced, on clean 1-sample/symbol captures, is set by noise as expected
for coded high-order modulation: QAM64 needs about 25 dB, QAM16 about 15 dB, 8PSK about 10 dB.

### Constellation check (modulation order from the symbols)

`sigcore::constellation_check` (`core/src/modulation_blind.cpp`) reads BPSK / QPSK / 8-PSK / QAM16 / QAM64
from the symbols themselves, which the spectrogram classifier cannot see (their spectra are alike). Phase-only
schemes are told apart by fourth-order cumulants (|C40|, C42 against 2/-2, 1/-1, 0/-1, scaled by the signal
share read from the fourth moment). A noisy QPSK and a clean QAM16 have the same cumulants of every order, so
one ring against the QAM ring structures is decided by the likelihood of the symbol magnitudes (Rician noise,
signal share fitted) instead. It needs symbol-rate samples with the carrier offset removed: raw input at 1
sample/symbol, or the output of timing recovery. At 1 sample/symbol it only runs when the spectrum is flat
(`spectral_peakiness` < 7.5): symbol-rate linear data measured 1.3-6.2, oversampled FSK 8.5-14, and without
that gate 27 of 40 FSK captures were read as QPSK.

A QAM reading needs a clear win over the single ring (at least 0.03 log-likelihood per symbol): the rings are a
flexible density, and on a real recovered BPSK beacon (non-circular noise) they edged out the single ring by
0.009 per symbol and the first version printed "qam64" for it -- caught by running the demo captures in the
real app. Ordinary noisy PSK never exceeds 0.02, QAM16 is 0.04 and up from 15 dB, QAM64 from 25 dB.

Measured on 200 synthetic non-FSK captures at symbol rate (5 modulations x 5 SNRs x 8, about 140-280 symbols
each): BPSK 100% and QPSK 100% at every SNR down to 3 dB, 8-PSK 95% (75% at 3 dB), QAM16 100% at 15 dB and
above and QAM64 100% at 25 dB, 71% overall, against 14% for the classifier's top guess on the same captures.
Below those SNRs a QAM is read as QPSK: noisy QAM and QPSK really are indistinguishable there, and the gate
chooses the safe reading over calling PSK "QAM". Silent on all 40 FSK captures. When the classifier's label does
not verify, the reading is tried first, ahead of its runner-up labels. It does not change how many captures
decode (66% either way: the retries already reach the right modulation). Where the reading supplied the label,
the panel says "(read from the constellation)" in place of a classifier confidence.

### Large captures: auto-detect on the first part, and files past 2 GiB

Auto-detect and its retries used to be skipped above 300,000 demodulated bits or 2M samples. They now run on
the first 2,000,000 samples, and of the bits those give, the first 60,000 (a search over 300,000 bits took
14 s; 60,000 is a fraction of a second), and the panel says "Auto-detect window: searched the first N of M
...; this result covers only that part" (JSON: `auto_window_total_samples`, `auto_window_total_bits`). A
convolutional code is a valid code word up to any cut, so a long Viterbi stream is found from its first part
(tested on 2.5M coded bits, and a 16M-sample window: found and verified in 6.5 s including loading). Block
codes are only found when the cut happens to land on a block boundary (up to 7 padding bits are ignored), and
soft decoding is off in a windowed run. The framing (AX.25) scan still sees the whole stream.

A 2.25 GiB file was stepped through in real Chrome, all 18 windows of 128 MiB, and the last one (past 2^31
bytes) analysed correctly (`SIG_BIG_FILE=1 node --test tests/gui/browser-gui.test.mjs`; it writes 2.25 GiB
of disk, so it only runs when asked). That test found a real bug: the wire format rejected a byte offset
above 2^31 as an "implausible length". Byte offsets are now plain 64-bit values. The limit that remains is
the 32-bit WASM `size_t` for sample counts (about 4 billion samples, 32 GiB of cf32).
