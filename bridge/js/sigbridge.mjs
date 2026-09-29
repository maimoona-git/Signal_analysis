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

  /**
   * The whole chain in one call: bytes -> samples -> spectrogram image (C++) ->
   * the registered classifier -> validated result. Same input/result shapes as
   * ingest() and classify(); rejects if the file cannot be parsed, is shorter
   * than one 64-sample FFT window, or the classifier fails.
   */
  async classifyCapture(source, { filename, sampleRate = 0 } = {}) {
    const name = filename ?? source?.name ?? '';
    const bytes = await toBytes(source);
    return new Promise((resolve, reject) => {
      try {
        this.module.classifyCapture(bytes, name, sampleRate, (err, result) => (err ? reject(err) : resolve(result)));
      } catch (e) {
        reject(e);
      }
    });
  }

  /**
   * The full analysis pipeline: ingest -> spectrogram -> ML classification ->
   * demod -> de-interleave -> FEC decode -> correlate. Options mirror the native
   * app's run_pipeline (unknown options are rejected, so typos are caught):
   *
   *   filename          decides the format by extension (.wav / .iq / .sigmf-data), as the
   *                     native app does -- a SigMF .sigmf-data file needs no renaming, since
   *                     (for the default cf32 case) its bytes already are a raw .iq file
   *   sampleRate        raw .IQ/.sigmf-data only (default 1 MHz); WAV carries its own
   *   iqFormat          'cf32' (default) | 'ci16' | 'cu8': how raw .IQ/.sigmf-data samples are
   *                     stored on disk -- cf32 is this project's own convention and needs no
   *                     conversion; ci16 (full-scale +-32768, e.g. USRP) and cu8 (unsigned,
   *                     centred on 127.5, e.g. RTL-SDR) are real formats real SDR hardware
   *                     actually produces, converted to the same float samples either way.
   *                     Ignored for WAV, which carries its own PCM format in its header.
   *   samplesPerSymbol  decimation for linear schemes (not FSK), default 1
   *   demod             { bitsPerSymbol, samplesPerSymbol, bandwidth }, FSK only
   *   interleaver       { kind: 'block'|'diagonal'|'pseudo_random'|'convolutional',
   *                       rows, cols | seed | branches, delayIncrement }
   *   fecScheme         'viterbi' | 'ldpc' | 'rs' | 'concatenated', with fecMsgLen (bits for
   *                     viterbi/ldpc, bytes for rs/concatenated)
   *   headerPattern     '0110...' or an array of bits (default: the stream's first 16 bits)
   *   autoDetect        with no interleaver/fec given, search for them blindly
   *   softDecoding      Feature 3 (extra feature): decode from soft LLRs instead of hard bits,
   *                     when the modulation/FEC support it (falls back to hard silently otherwise;
   *                     result.softDecodingUsed says which one actually ran)
   *   recoverTiming     Real-world signal support: for a linear scheme (not FSK) with
   *                     samplesPerSymbol > 1, run matched-filter timing recovery plus carrier
   *                     tracking instead of naive decimation, for a capture with real (unknown)
   *                     timing offset and carrier error -- this project's own synthetic captures
   *                     have neither, so this only matters for something recorded off the air.
   *                     result.recoveryUsed says whether it actually ran; when it did,
   *                     result.recoveryPhaseAmbiguity carries the count of equally-valid constant
   *                     phase rotations the decode could be off by (e.g. 2 for BPSK means the
   *                     message may come out with every bit flipped) -- see core/include/recover.hpp.
   *
   * Resolves to the result object documented in bridge/README.md.
   */
  async runPipeline(source, { filename, ...options } = {}) {
    const name = filename ?? source?.name ?? '';
    const bytes = await toBytes(source);
    return new Promise((resolve, reject) => {
      try {
        this.module.runPipeline(bytes, name, options, (err, result) => (err ? reject(err) : resolve(result)));
      } catch (e) {
        reject(e);
      }
    });
  }

  /** Classifications started but not yet completed (should settle back to 0). */
  get pendingClassifications() {
    return this.module.mlPendingCount();
  }

  /**
   * Feature 1 (SIH26147 extra feature, descoped): decode-driven parameter search.
   * Tries every (bit offset x interleaver x FEC) combination against `bits` (typically
   * a result's `demodulatedBits`, BEFORE any de-interleave/decode: decoding a code
   * removes exactly the redundancy this looks for) and ranks all of them by how much a
   * decode looks correct (the same re-encode-and-compare metric as `reencodeAgreement`
   * elsewhere), not by how often a header happens to repeat -- see
   * app/pipeline/param_search.hpp's own comment for why that matters. Synchronous (no
   * ML call-out involved).
   *
   *   options.maxBitOffset     try offsets [0, maxBitOffset] too (default 0: offset 0 only)
   *   options.confidenceFloor  a confidence at or above this counts as `detected` (default 0.5)
   *
   * Resolves to { detected, bestIndex, candidates: [{ bitOffset, interleaver, fec,
   * fecMsgLen, reencodeAgreement, headerRepeatScore, confidence, decodedBits }, ...] }
   * (candidates ranked by confidence descending; decodedBits is a bit COUNT, not the
   * bits themselves -- see decodeParamCandidate for those). Throws for empty input.
   */
  searchParams(bits, options = {}) {
    return this.module.searchParams(bits, options);
  }

  /**
   * Re-applies one candidate from searchParams' list (or any object with the same
   * { bitOffset, interleaver, fec, fecMsgLen } shape) to `bits` and returns
   * { bits: Uint8Array, text: string } -- the actual decoded message for that specific
   * candidate, computed on demand so a user can inspect any entry in the ranked list,
   * not just the top one.
   */
  decodeParamCandidate(bits, candidate) {
    return this.module.decodeParamCandidate(bits, candidate);
  }
}
