// The analysis Web Worker (gap-list Phase 4). A SECOND WASM instance -- the bridge module
// (sigbridge.js/.wasm: the whole pipeline, no Qt) -- plus the ONNX classifier, running off the
// page's main thread, so a long search no longer freezes the Qt UI. This is the plan CLAUDE.md
// settled on for a single-threaded build: a worker with its own module instance, not in-module
// threads.
//
// Protocol (all messages are plain objects):
//   page -> worker  { type: 'init', base }                    base = the page's base URL
//   worker -> page  { type: 'ready' } | { type: 'init-failed', error }
//   page -> worker  { type: 'run', id, bytes, filename, options }   bytes/options: Uint8Array
//   worker -> page  { type: 'done', id, blob }                blob: Uint8Array (pipeline_io result)
//                 | { type: 'done', id, error }
// `options` and `blob` are the binary formats of app/pipeline/pipeline_io.hpp; both buffers are
// transferred, not copied.
//
// A classic (not module) worker, because Emscripten's MODULARIZE output is a classic script
// loaded with importScripts; the ES-module pieces (ORT, the classifier wrapper) come in through
// dynamic import(), which classic workers support.

let bridge = null;

async function init(base) {
  const abs = (path) => new URL(path, base).href;
  importScripts(abs('sigbridge.js'));
  const ort = await import(abs('vendor/ort/ort.wasm.min.mjs'));
  const { createOnnxClassifier } = await import(abs('js/onnx-classifier.mjs'));
  const classifier = createOnnxClassifier({
    ort,
    model: abs('models/modulation_classifier.onnx'),
    meta: abs('models/modulation_classifier.json'),
    wasmPaths: abs('vendor/ort/'),
  });
  classifier.catch((e) => console.error('ML model failed to load in the worker:', e));  // reported on first use
  bridge = await createSigBridge({
    locateFile: (file) => abs(file),
    // Read by the C++ pipeline (bridge/src/ml_bridge.cpp), exactly as in the page's own module.
    sigMlClassifier: async (spectrogram, rows, cols) => (await classifier).classify(spectrogram, rows, cols),
  });
}

function run({ id, bytes, filename, options }) {
  const reply = (message, transfer) => postMessage({ type: 'done', id, ...message }, transfer || []);
  try {
    bridge.runPipelineBinary(bytes, filename, options, (error, blob) => {
      if (error) reply({ error: String(error.message ?? error) });
      else reply({ blob }, [blob.buffer]);
    });
  } catch (e) {
    // a synchronous failure (an unreadable file, one too short to analyse)
    reply({ error: String(e?.message ?? e) });
  }
}

onmessage = async (event) => {
  const message = event.data;
  if (message.type === 'init') {
    try {
      await init(message.base);
      postMessage({ type: 'ready' });
    } catch (e) {
      postMessage({ type: 'init-failed', error: String(e?.message ?? e) });
    }
  } else if (message.type === 'run') {
    if (!bridge) postMessage({ type: 'done', id: message.id, error: 'the analysis worker is not ready' });
    else run(message);
  }
};
