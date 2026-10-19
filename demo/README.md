# Demo captures

Five synthetic captures for showing the app to someone without needing a
purpose-built test fixture on hand, or reaching for a random downloaded file
(which will not decode -- see the note at the bottom, and the "why" in
`bridge/include/error_text.hpp`'s own comment).

Each one is a real modulation scheme (FSK, throughout -- see "Why they are all
FSK" below), most with a real interleaver and/or FEC code, encoded with the
same functions the app itself decodes with (built by
`scripts/generate_demo_captures.cpp`, not hand-crafted), plus a proven-safe
amount of channel noise so none of them is a suspiciously clean, too-perfect
signal. Every claim below (sample rate, whether the real ML classifier and
auto-detect find it, whether Parameter Search is needed) was measured by
actually running the file through the real pipeline -- and, for two of them,
through the real GUI in real headless Chrome -- not assumed.

## How to open one

Launch the app, "Open File...", pick one of these, type in the **Sample rate**
shown below (every file here except `intercept_alpha.wav` is raw `.IQ`, so the
field is required -- a `.wav` carries its own rate and the field is ignored),
leave **Samples/symbol** at its default (1), and click **Run Analysis**.

| File | Sample rate | Auto-detect | Result |
| --- | --- | --- | --- |
| `intercept_alpha.wav` | *(carried in the file)* | off | Decodes immediately: **"OPERATION NIGHTFALL CONFIRMED"** |
| `intercept_bravo.iq` | 8000 | off | Decodes immediately: **"COORDINATES RECEIVED HOLD POSITION"** |
| `intercept_charlie.iq` | 8000 | **on**, but still shows undecoded bits | Tick auto-detect and run: the panel still won't show the real message (a documented, inherited limitation -- see below). Open the **Parameter Search** tab and click **Search Parameters**: the top result is a real LDPC decode (confidence 1.000), and its panel shows **"SIGNAL INTERCEPT VERIFIED DELTA STATION"** -- verified in real headless Chrome, not just the C++ harness |
| `intercept_delta.iq` | 8000 | **on**, but still shows undecoded bits | Same as Charlie (Viterbi + a block interleaver this time): auto-detect misses it, Parameter Search finds it -- **"HOLD POSITION AT POINT ZULU"** |
| `intercept_echo.iq` | 8000 | **on**, but still shows undecoded bits | Same again (Reed-Solomon): auto-detect misses it, Parameter Search finds it -- **"MAINTAIN RADIO SILENCE NOW"** |

**Charlie, Delta and Echo are deliberate**, not a bug: they demonstrate
exactly the gap Feature 1 (Parameter Search) exists to close. `detect.hpp`'s
auto-detect heuristic ranks candidates mainly by how often a header repeats;
a single, non-repeating message (real intercepted traffic is not going to
repeat itself for the detector's benefit) gives it nothing reliable to go on,
so it reports "no FEC" -- the same class of miss a real user hit and reported
during this port's own testing (see `app/pipeline/README.md`'s "Feature 1"
section). Parameter Search instead ranks every candidate by whether a decode
actually looks correct, which does not depend on anything repeating.

## Why they are all FSK

Found by measurement while building this set, not assumed: the shipped ML
classifier (trained only on this project's own synthetic captures, a known,
documented limitation) is strongly biased toward predicting **FSK** for
structured, low-entropy bit content like real English text -- plausibly
because plain uppercase ASCII text shares a constant zero top bit in every
byte, an artificially periodic pattern the model was never trained against.
Building these files as BPSK, QPSK, PSK8 and QAM16 (one real English message
each) and running them through the *real* classifier confirmed it: every one
of them was called "fsk" anyway, with 0.5-0.95 confidence, regardless of the
scheme they actually were. A demo file has to survive the real classifier
call, not a test harness that supplies the correct label directly (that
mistake is exactly what let an earlier draft of this set ship broken -- see
the generator's own history), so every file here is genuinely FSK-modulated,
which the real model confirms with 1.000 confidence, every time, measured.

## Regenerating

```
c++ -std=c++17 -O2 -Iapp/pipeline/include -Icore/include \
  scripts/generate_demo_captures.cpp \
  build/app/pipeline/libsigpipeline.a build/core/libsigcore_core.a \
  <path to libliquid> \
  -o /tmp/generate_demo_captures
/tmp/generate_demo_captures
```

Prints a verification line per file (known-params / gui-auto-detect /
search-params, each measured against the real message, using the real
`run_pipeline` and `search_params` -- not the real ML classifier, which needs
`onnxruntime-web` in JS, so check that separately if you change what a file's
content looks like; see the note above about why that matters). Read the
output before trusting a change to this set.

## Why a real downloaded file still won't decode

These are built to match this pipeline's actual assumptions (a signal already
demodulated with one of its known schemes, already perfectly sample-aligned).
An arbitrary real-world recording -- audio or an actual RF capture -- almost
never satisfies that on its own, and the ML classifier has only ever been
trained on this project's own synthetic captures, so it will confidently
misclassify one. That is a real, documented scope boundary of this tool, not
something these demo files paper over.

**Partially closed, for linear schemes:** the "Recover timing/carrier" toggle
(`PipelineOptions::recover_timing`, see `app/pipeline/README.md`'s own section)
now does real matched-filter timing recovery and carrier tracking for a PSK/
QAM/ASK/OOK capture that actually has an unknown timing offset and carrier
error -- something these demo files never had a reason to need, since they are
generated already sample-aligned. It does not blindly find the symbol rate
(still a required input), does not help FSK (every file in this set), and does
not fix the classifier's own bias documented below -- so it does not make
these particular five files decode any differently, and does not turn this
into a tool that decodes an arbitrary downloaded file end to end. It closes
one specific, real, measured gap (see the pipeline README's own test
description), not the whole scope boundary above.
