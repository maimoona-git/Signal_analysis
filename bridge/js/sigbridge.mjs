// JS side of the bridge: a small, promise-based wrapper over the Embind module
// (bridge/src/bindings.cpp). The C++ pipeline and the Qt GUI use the same
// Embind functions directly; this wrapper is what a plain JS host (and the
// test suite) uses, and shows the intended calling conventions.

/** Normalises anything file-like into a Uint8Array (no copy for typed arrays). */
async function toBytes(source) {
  if (source instanceof ArrayBuffer) return new Uint8Array(source);
  if (ArrayBuffer.isView(source)) return source;
  if (source && typeof source.arrayBuffer === 'function') {
    // File / Blob, e.g. from <input type="file"> or a drop event.
    return new Uint8Array(await source.arrayBuffer());
  }
  throw new TypeError('expected a File, Blob, ArrayBuffer or typed array');
}

/** Anything numeric and iterable becomes a Float32Array (no copy if already one). */
function toFloat32(values) {
  if (values instanceof Float32Array) return values;
  // Strings are iterable but never a spectrogram.
  if (typeof values !== 'string' && values && typeof values[Symbol.iterator] === 'function') {
    return Float32Array.from(values);
  }
  throw new TypeError('expected a Float32Array or an array of numbers');
}

export class SigBridge {
  /** `factory` is the module factory emitted by the build (createSigBridge). */
  static async create(factory, moduleConfig = {}) {
    return new SigBridge(await factory(moduleConfig));
  }

  constructor(module) {
    this.module = module;
  }

  /**
   * Parses a capture in WASM. Accepts a File/Blob (its .name is used to pick the
   * format), an ArrayBuffer, or a typed array. Raw .IQ needs `sampleRate`; WAV
   * carries its own. Resolves to
   *   { sampleRate, format, numSamples, durationSec, samples: Float32Array (I,Q interleaved) }
   * and rejects with an Error if the file cannot be parsed.
   */
  async ingest(source, { filename, sampleRate = 0 } = {}) {
    const name = filename ?? source?.name ?? '';
    const bytes = await toBytes(source);
    return this.module.ingestFile(bytes, name, sampleRate);
  }

  /**
   * Registers the ML classifier the C++ pipeline calls out to:
   *   async (spectrogram: Float32Array, rows, cols) => ({ classes: string[], probabilities: number[] })
   * It may return the object directly or a Promise; it may throw or reject.
   */
  setClassifier(fn) {
    if (typeof fn !== 'function') throw new TypeError('classifier must be a function');
    this.module.sigMlClassifier = fn;
  }

  clearClassifier() {
    delete this.module.sigMlClassifier;
  }

  /**
   * Runs the C++ -> JS -> C++ round trip: hands `spectrogram` (row-major
   * rows x cols) to C++, which calls the registered classifier and validates
   * what comes back. Resolves to { label, confidence, topIndex, classes, probabilities }.
   */
  classify(spectrogram, rows, cols) {
    return new Promise((resolve, reject) => {
      try {
        this.module.classifySpectrogram(toFloat32(spectrogram), rows, cols, (err, result) =>
          err ? reject(err) : resolve(result));
      } catch (e) {
        reject(e);
      }
    });
  }

  /** Classifications started but not yet completed (should settle back to 0). */
  get pendingClassifications() {
    return this.module.mlPendingCount();
  }
}
