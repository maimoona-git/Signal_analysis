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

## Targets

- `sigbridge_portable`: `ml_result` validation, plain C++ (native + WASM tests)
- `sigbridge_objs`: Embind + call-out (OBJECT library so the registrations are
  never dropped by the linker; link it into the Qt app in Phase 7)
- `sigbridge_module`: standalone `sigbridge.js`/`.wasm` (`createSigBridge()`)
  for the JS tests and any non-Qt host
