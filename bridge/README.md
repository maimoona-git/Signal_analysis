# Bridge: JS <-> C++ (Embind)

Two directions, both tested under node against the compiled module
(`tests/bridge/bridge.test.mjs`, run by `ctest`).

## File bytes in

```js
const sig = await bridge.ingest(file, { sampleRate });   // File | Blob | ArrayBuffer | typed array
// { sampleRate, format, numSamples, durationSec, samples: Float32Array /* I,Q interleaved */ }
```

`Module.ingestFile(bytes, filename, sampleRate)` copies the bytes into WASM
memory (one bulk `TypedArray.set`, honouring `byteOffset`) and calls
`sigcore::ingest_bytes`, which picks WAV vs raw `.IQ`: a RIFF/WAVE header wins,
then a `.wav` extension, otherwise raw complex64 `.IQ` (which needs
`sampleRate`). Files are untrusted: chunk sizes are bounds-checked, truncated
WAVs are clamped, and every failure is thrown to JS as a real `Error` with the
C++ message.

## ML call-out (C++ -> JS -> C++)

Register a classifier on the module (`bridge.setClassifier(fn)` or
`Module.sigMlClassifier = fn`):

```js
async (spectrogram /* Float32Array, private copy */, rows, cols) =>
  ({ classes: ['BPSK', ...], probabilities: [0.1, ...] })   // object or Promise
```

C++ calls it with `sigbridge::classify_async(spec, rows, cols, done)`
(`bridge/include/ml_bridge.hpp`); `done(ok, result, error)` fires later from a
JS microtask, never re-entrantly. `result` is validated in C++
(`make_ml_result`): matching lengths, non-empty names, every probability a
finite number in [0, 1]; the top class is picked in C++. A throwing or rejecting
classifier, a malformed result, or no classifier at all all arrive as
`ok == false` with a message.

**Why callbacks and not Asyncify:** a synchronous C++ pipeline cannot block on a
Promise without Asyncify, which would instrument every function in the Qt app
(bigger, slower). Phase 6's pipeline is therefore split at the ML step and
resumes in `done`.

## The whole chain

`bridge.classifyCapture(file, { sampleRate })` (Embind `classifyCapture`) runs
bytes -> `ingest_bytes` -> `make_spectrogram_image` (C++) -> `classify_async` ->
the registered classifier -> validated result. With `web/js/onnx-classifier.mjs`
as the classifier this is the browser ML path:

```js
import * as ort from './vendor/ort/ort.wasm.min.mjs';          // scripts/vendor-ort.sh
const clf = await createOnnxClassifier({ ort, model: 'models/modulation_classifier.onnx',
                                         meta: 'models/modulation_classifier.json', wasmPaths: 'vendor/ort/' });
bridge.setClassifier(clf.classify);
const { label, confidence, probabilities } = await bridge.classifyCapture(file, { sampleRate });
```

## The pipeline

`bridge.runPipeline(file, options)` (Embind `runPipeline`) runs everything:
`ingest -> spectrogram -> ML classification -> demod -> de-interleave -> FEC decode ->
correlate`, i.e. `app/pipeline` with the asynchronous ML step in the middle
(`sigbridge::run_pipeline_async`). Options (unknown ones are rejected, so typos
fail loudly; the option names are documented on `SigBridge.runPipeline`):

```js
const r = await bridge.runPipeline(file, {
  sampleRate: 8000, samplesPerSymbol: 4,
  interleaver: { kind: 'block', rows: 8, cols: 16 },
  fecScheme: 'ldpc', fecMsgLen: 64,        // or autoDetect: true to search for both
});
```

Resolves to (all arrays are copies, safe to keep):

```
{ filePath, sampleRate, numSamples, fileFormat,
  samples: Float32Array (I,Q interleaved),
  spectrogram: { data: Float32Array (frames x nfft, dB), frames, nfft, freqs: Float64Array, times: Float64Array },
  predictedModulation, predictedConfidence,
  demodulatedBits: Uint8Array, deinterleavedBits: Uint8Array|null, decodedBits: Uint8Array|null,
  boundaries: { headerPattern: Uint8Array, framePeriod: number|null, headerPositions: number[], payloads: Uint8Array[] },
  detectedInterleaver: { kind, rows?, cols?, seed?, branches?, delayIncrement? }|null,
  detectedFec: string|null, detectionScore: number|null }
```

Rejects with an `Error` (never throws synchronously through the wrapper; an out-of-memory failure reads
"out of memory: this capture is too large...", not `std::bad_alloc`) for a bad
file or option, a capture shorter than the ML model's 64-sample FFT window, a
classifier failure, or any pipeline-stage error. The file format follows the
native app's rule: `.wav` / `.iq` by extension.

## Targets

- `sigbridge_portable`: `ml_result` validation, plain C++ (native + WASM tests)
- `sigbridge_objs`: Embind + call-outs (`ml_bridge`, `pipeline_bridge`, `bindings`) (OBJECT library so the registrations are
  never dropped by the linker; link it into the Qt app in Phase 7)
- `sigbridge_module`: standalone `sigbridge.js`/`.wasm` (`createSigBridge()`)
  for the JS tests and any non-Qt host
