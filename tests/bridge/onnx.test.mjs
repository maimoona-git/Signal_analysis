// Phase 5: the real exported model, running in onnxruntime-web, wired into the
// Embind bridge, checked against the ORIGINAL native Python results
// (tests/data/ml/expected.json, produced by ml/make_fixtures.py).
//
//   1. onnxruntime-web vs PyTorch, on the original preprocessed images
//   2. the full chain: raw .IQ bytes -> C++ ingest -> C++ preprocessing ->
//      JS ONNX classifier -> validated result, vs the original end to end

import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { after, before, describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';

import * as ort from 'onnxruntime-web/wasm';

import { SigBridge } from '../../bridge/js/sigbridge.mjs';
import { createOnnxClassifier, loadModelMeta } from '../../web/js/onnx-classifier.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const modulePath = process.env.SIGBRIDGE_JS ?? path.resolve(root, 'build-wasm/bridge/sigbridge.js');
const factory = createRequire(import.meta.url)(modulePath);

const MODEL = fs.readFileSync(path.join(root, 'ml/models/modulation_classifier.onnx'));
const META = JSON.parse(fs.readFileSync(path.join(root, 'ml/models/modulation_classifier.json'), 'utf8'));
const EXPECTED = JSON.parse(fs.readFileSync(path.join(root, 'tests/data/ml/expected.json'), 'utf8'));
const dataFile = (name) => fs.readFileSync(path.join(root, 'tests/data/ml', name));

const asFloat32 = (buf) => new Float32Array(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length));
const maxAbsDiff = (a, b) => Math.max(...a.map((v, i) => Math.abs(v - b[i])));

// Tolerances, set from the measurements the tests print (~70x headroom):
//   image -> probabilities: onnxruntime-web vs PyTorch, measured worst 1.5e-6
//   raw IQ -> probabilities: adds the WASM-vs-fftw preprocessing difference
//                            (<= 3.9e-5 on the image), measured worst 1.4e-6
const TOL_MODEL = 1e-4;
const TOL_CHAIN = 1e-4;

describe('onnxruntime-web reproduces the original PyTorch model', () => {
  let clf;
  before(async () => { clf = await createOnnxClassifier({ ort, model: MODEL, meta: META }); });
  after(async () => { await clf.dispose(); });

  it('matches PyTorch on every original preprocessed image', async () => {
    let worst = 0;
    for (const c of EXPECTED.cases) {
      const image = asFloat32(dataFile(c.image_file));
      assert.equal(image.length, 64 * 48);
      const { classes, probabilities } = await clf.classify(image, 64, 48);
      assert.deepEqual(classes, EXPECTED.classes);
      const diff = maxAbsDiff(probabilities, c.probabilities);
      worst = Math.max(worst, diff);
      assert.ok(diff < TOL_MODEL, `${c.name}: max|diff| ${diff}`);
      const top = probabilities.indexOf(Math.max(...probabilities));
      assert.equal(classes[top], c.label, `${c.name}: label`);
    }
    console.log(`         model parity, ${EXPECTED.cases.length} cases: worst max|diff| = ${worst.toExponential(2)}`);
  });

  it('returns valid probabilities (softmax in the graph)', async () => {
    const image = asFloat32(dataFile(EXPECTED.cases[0].image_file));
    const { probabilities } = await clf.classify(image, 64, 48);
    assert.equal(probabilities.length, META.classes.length);
    assert.ok(probabilities.every((p) => p >= 0 && p <= 1));
    assert.ok(Math.abs(probabilities.reduce((a, b) => a + b, 0) - 1) < 1e-5);
  });

  it('serialises overlapping calls, each getting its own answer', async () => {
    const cases = EXPECTED.cases.slice(0, 6);
    const results = await Promise.all(cases.map((c) => clf.classify(asFloat32(dataFile(c.image_file)), 64, 48)));
    results.forEach((r, i) => assert.ok(maxAbsDiff(r.probabilities, cases[i].probabilities) < TOL_MODEL, cases[i].name));
  });

  it('rejects the wrong geometry or input type with a clear message', async () => {
    await assert.rejects(clf.classify(new Float32Array(32 * 48), 32, 48), /expects a 64x48/);
    await assert.rejects(clf.classify(new Float32Array(10), 64, 48), /Float32Array of 3072/);
    await assert.rejects(clf.classify([1, 2, 3], 64, 48), /Float32Array/);
    // the queue survives failures
    assert.equal((await clf.classify(asFloat32(dataFile(EXPECTED.cases[0].image_file)), 64, 48)).classes.length, 6);
  });

  it('is fast enough to be interactive', async () => {
    const image = asFloat32(dataFile(EXPECTED.cases[0].image_file));
    await clf.classify(image, 64, 48);  // warm-up
    const t0 = performance.now();
    for (let i = 0; i < 20; i++) await clf.classify(image, 64, 48);
    const ms = (performance.now() - t0) / 20;
    console.log(`         inference: ${ms.toFixed(2)} ms per classification (wasm, 1 thread)`);
    assert.ok(ms < 250, `${ms} ms per inference`);
  });
});

describe('createOnnxClassifier / loadModelMeta validate their inputs', () => {
  it('rejects a bad ort namespace, bad metadata and a mismatched model', async () => {
    await assert.rejects(createOnnxClassifier({ ort: {}, model: MODEL, meta: META }), TypeError);
    await assert.rejects(loadModelMeta({ classes: [] }), /non-empty array/);
    await assert.rejects(loadModelMeta({ classes: ['a', ''], input: {} }), /non-empty array/);
    await assert.rejects(loadModelMeta({ classes: ['a'], input: { channels: 1, height: 0, width: 4, name: 'x' }, output: { name: 'y' } }), /input\.height/);
    await assert.rejects(loadModelMeta({ classes: ['a'], input: { channels: 1, height: 4, width: 4 }, output: {} }), /input\.name/);
    const wrongNames = structuredClone(META);
    wrongNames.input.name = 'not_the_input';
    await assert.rejects(createOnnxClassifier({ ort, model: MODEL, meta: wrongNames }), /does not match its metadata/);
    await assert.rejects(createOnnxClassifier({ ort, model: new Uint8Array([1, 2, 3, 4]), meta: META }));
  });
});

describe('full chain through the bridge: bytes -> C++ -> ONNX -> C++', () => {
  let bridge, clf;
  before(async () => {
    bridge = await SigBridge.create(factory);
    clf = await createOnnxClassifier({ ort, model: MODEL, meta: META });
    bridge.setClassifier(clf.classify);
  });
  after(async () => { bridge.clearClassifier(); await clf.dispose(); });

  it('reproduces the original end-to-end result for every fixture capture', async () => {
    let worst = 0;
    for (const c of EXPECTED.cases) {
      const result = await bridge.classifyCapture(dataFile(c.iq_file), { filename: c.iq_file, sampleRate: c.sample_rate });
      assert.deepEqual(result.classes, EXPECTED.classes);
      const diff = maxAbsDiff(result.probabilities, c.probabilities);
      worst = Math.max(worst, diff);
      assert.ok(diff < TOL_CHAIN, `${c.name}: max|diff| ${diff}`);
      assert.equal(result.label, c.label, `${c.name}: label`);
      assert.ok(Math.abs(result.confidence - c.confidence) < TOL_CHAIN, `${c.name}: confidence`);
    }
    console.log(`         end-to-end parity, ${EXPECTED.cases.length} captures: worst max|diff| = ${worst.toExponential(2)}`);
  });

  it('accepts a File, like the browser file input', async () => {
    const c = EXPECTED.cases.find((x) => x.name === 'fsk');
    const file = new File([dataFile(c.iq_file)], c.iq_file);
    const result = await bridge.classifyCapture(file, { sampleRate: c.sample_rate });
    assert.equal(result.label, 'fsk');
    assert.ok(result.confidence > 0.99);
  });

  it('classify_signal path agrees with classifySpectrogram on the same image', async () => {
    const c = EXPECTED.cases.find((x) => x.name === 'qam16');
    const viaCapture = await bridge.classifyCapture(dataFile(c.iq_file), { filename: c.iq_file, sampleRate: c.sample_rate });
    const viaImage = await bridge.classify(asFloat32(dataFile(c.image_file)), 64, 48);
    assert.ok(maxAbsDiff(viaCapture.probabilities, viaImage.probabilities) < TOL_CHAIN);
  });

  it('runs overlapping captures concurrently without mixing results', async () => {
    const cases = EXPECTED.cases.slice(0, 6);
    const results = await Promise.all(cases.map((c) =>
      bridge.classifyCapture(dataFile(c.iq_file), { filename: c.iq_file, sampleRate: c.sample_rate })));
    results.forEach((r, i) => assert.equal(r.label, cases[i].label, cases[i].name));
    assert.equal(bridge.pendingClassifications, 0);
  });

  it('rejects captures the pipeline cannot use, with the C++ reason', async () => {
    await assert.rejects(bridge.classifyCapture(new Float32Array(2 * 63), { filename: 'short.iq', sampleRate: 8000 }),
      /at least 64 samples, got 63/);
    await assert.rejects(bridge.classifyCapture(new Float32Array(200), { filename: 'nosr.iq' }), /sample rate is required/);
    await assert.rejects(bridge.classifyCapture(new Uint8Array(64), { filename: 'bad.wav' }), /RIFF\/WAVE/);
    assert.equal(bridge.pendingClassifications, 0);
  });

  it('surfaces a model failure through the bridge as an Error', async () => {
    bridge.setClassifier(async () => { throw new Error('onnx runtime exploded'); });
    const c = EXPECTED.cases[0];
    await assert.rejects(bridge.classifyCapture(dataFile(c.iq_file), { filename: c.iq_file, sampleRate: c.sample_rate }),
      /onnx runtime exploded/);
    bridge.setClassifier(clf.classify);
    assert.equal((await bridge.classifyCapture(dataFile(c.iq_file), { filename: c.iq_file, sampleRate: c.sample_rate })).label, c.label);
  });
});
