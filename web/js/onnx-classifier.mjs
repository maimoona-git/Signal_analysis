// Browser-side ML: loads the exported ONNX model with onnxruntime-web and
// exposes it in the shape the C++ bridge calls out to (see bridge/README.md):
//
//   const clf = await createOnnxClassifier({ ort, model: 'models/x.onnx', meta: 'models/x.json' });
//   bridge.setClassifier(clf.classify);
//
// `ort` is passed in rather than imported so the same module runs in the
// browser (a vendored ort.wasm.min.mjs), under node (the tests) and in a
// worker, and so the app decides where the .wasm runtime is served from.
// Inference is single-threaded (numThreads = 1): the app deploys to plain
// static hosts without the COOP/COEP headers WASM threads would need.

/**
 * onnxruntime-web loads its runtime with a dynamic import() of
 * `wasmPaths + file`, and a browser treats a relative specifier like
 * "vendor/ort/x.mjs" as a bare module name and refuses it. So a relative
 * `wasmPaths` is turned into an absolute URL against the page here, and
 * callers can pass the natural "vendor/ort/". (No page, e.g. under node: unchanged.)
 */
function resolveAgainstPage(wasmPaths) {
  if (typeof wasmPaths !== 'string') return wasmPaths;
  const base = globalThis.document?.baseURI ?? globalThis.location?.href;
  return base ? new URL(wasmPaths, base).href : wasmPaths;
}

/** Loads and validates the model's sidecar description (ml/models/*.json). */
export async function loadModelMeta(source) {
  let meta = source;
  if (typeof source === 'string') {
    const response = await fetch(source);
    if (!response.ok) throw new Error(`could not load model metadata ${source}: HTTP ${response.status}`);
    meta = await response.json();
  }
  const classes = meta?.classes;
  if (!Array.isArray(classes) || classes.length === 0 || classes.some((c) => typeof c !== 'string' || !c)) {
    throw new Error('model metadata: "classes" must be a non-empty array of names');
  }
  const input = meta.input ?? {};
  for (const key of ['channels', 'height', 'width']) {
    if (!Number.isInteger(input[key]) || input[key] <= 0) {
      throw new Error(`model metadata: input.${key} must be a positive integer`);
    }
  }
  if (typeof input.name !== 'string' || typeof meta.output?.name !== 'string') {
    throw new Error('model metadata: input.name and output.name are required');
  }
  return meta;
}

/**
 * @param {object} options
 * @param {object} options.ort     the onnxruntime-web namespace
 * @param {string|ArrayBuffer|Uint8Array} options.model   URL or bytes of the .onnx file
 * @param {string|object} options.meta   URL or parsed contents of the sidecar .json
 * @param {string} [options.wasmPaths]   directory ort loads its .wasm runtime from (relative to the page is fine)
 */
export async function createOnnxClassifier({ ort, model, meta, wasmPaths }) {
  if (!ort?.InferenceSession) throw new TypeError('createOnnxClassifier: `ort` must be the onnxruntime-web namespace');
  const description = await loadModelMeta(meta);
  const { classes } = description;
  const { name: inputName, channels, height, width } = description.input;
  const outputName = description.output.name;

  ort.env.wasm.numThreads = 1;
  if (wasmPaths) ort.env.wasm.wasmPaths = resolveAgainstPage(wasmPaths);
  const session = await ort.InferenceSession.create(model, { executionProviders: ['wasm'] });
  if (!session.inputNames.includes(inputName) || !session.outputNames.includes(outputName)) {
    throw new Error(`model does not match its metadata: expected input "${inputName}" and output ` +
      `"${outputName}", found [${session.inputNames}] -> [${session.outputNames}]`);
  }

  // One inference at a time per session: callers may fire several overlapping
  // classifications and each must still get its own result.
  let queue = Promise.resolve();

  /** The function to register with the bridge: (Float32Array, rows, cols) -> {classes, probabilities}. */
  async function classify(spectrogram, rows, cols) {
    if (rows !== height || cols !== width) {
      throw new Error(`classifier expects a ${height}x${width} (freq x time) image, got ${rows}x${cols}`);
    }
    if (!(spectrogram instanceof Float32Array) || spectrogram.length !== rows * cols) {
      throw new Error(`classifier expects a Float32Array of ${rows * cols} values`);
    }
    const run = queue.then(async () => {
      const input = new ort.Tensor('float32', spectrogram, [1, channels, rows, cols]);
      const outputs = await session.run({ [inputName]: input });
      const data = outputs[outputName]?.data;
      if (!data || data.length !== classes.length) {
        throw new Error(`model returned ${data?.length ?? 0} values for ${classes.length} classes`);
      }
      return { classes, probabilities: Array.from(data) };
    });
    queue = run.catch(() => {});  // keep the queue alive after a failure
    return run;
  }

  return {
    classify,
    meta: description,
    session,
    async dispose() { await session.release(); },
  };
}
