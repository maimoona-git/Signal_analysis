// Phase 6: the whole pipeline through the Embind bridge, in JS: file (as a
// browser File) -> C++ ingest + spectrogram -> ML call-out (the real ONNX model,
// or a stub) -> C++ demod / de-interleave / FEC decode / correlate -> result.
// Every one of the 84 scenarios recorded from the ORIGINAL native pipeline
// (tests/data/pipeline/pipeline.rec) must come out the same here.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { after, before, describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';

import * as ort from 'onnxruntime-web/wasm';

import { SigBridge } from '../../bridge/js/sigbridge.mjs';
import { createOnnxClassifier } from '../../web/js/onnx-classifier.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const modulePath = process.env.SIGBRIDGE_JS ?? path.resolve(root, 'build-wasm/bridge/sigbridge.js');
const factory = createRequire(import.meta.url)(modulePath);
const dataDir = path.join(root, 'tests/data/pipeline');

// --- fixture records ------------------------------------------------------------

function readRecords(file) {
  const records = [];
  let current = {};
  for (const line of fs.readFileSync(path.join(dataDir, file), 'utf8').split('\n')) {
    if (line === '---') { records.push(current); current = {}; continue; }
    if (!line) continue;
    const space = line.indexOf(' ');
    current[line.slice(0, space)] = line.slice(space + 1);
  }
  return records;
}
const RECORDS = readRecords('pipeline.rec');

const bitsStr = (u8) => (u8 && u8.length ? Array.from(u8, (b) => (b ? '1' : '0')).join('') : '-');
const optStr = (v) => (v === null || v === undefined ? 'none' : String(v));
const camel = { rows: 'rows', cols: 'cols', seed: 'seed', branches: 'branches', delay_increment: 'delayIncrement' };
const parseParams = (s) => (s === '-' ? {} : Object.fromEntries(s.split(',').map((kv) => kv.split('='))));

function optionsOf(r) {
  const o = { samplesPerSymbol: Number(r.samples_per_symbol), autoDetect: r.auto_detect === '1' };
  if (r.sample_rate !== 'none') o.sampleRate = Number(r.sample_rate);
  if (r.demod !== '-') {
    const p = parseParams(r.demod);
    o.demod = {};
    if (p.bits_per_symbol) o.demod.bitsPerSymbol = Number(p.bits_per_symbol);
    if (p.samples_per_symbol) o.demod.samplesPerSymbol = Number(p.samples_per_symbol);
    if (p.bandwidth) o.demod.bandwidth = Number(p.bandwidth);
  }
  if (r.interleaver !== 'none') {
    o.interleaver = { kind: r.interleaver };
    for (const [k, v] of Object.entries(parseParams(r.iparams))) o.interleaver[camel[k]] = Number(v);
  }
  if (r.fec !== 'none') o.fecScheme = r.fec;
  if (r.msg_len !== 'none') o.fecMsgLen = Number(r.msg_len);
  if (r.header !== 'none') o.headerPattern = r.header;
  return o;
}

const fileBytes = (name) => fs.readFileSync(path.join(dataDir, name));
const asFile = (name) => new File([fileBytes(name)], name);

// --- the model + a stub classifier ------------------------------------------------

const MODEL = fs.readFileSync(path.join(root, 'ml/models/modulation_classifier.onnx'));
const META = JSON.parse(fs.readFileSync(path.join(root, 'ml/models/modulation_classifier.json'), 'utf8'));

describe('runPipeline reproduces the original native pipeline', () => {
  let bridge, onnx;
  before(async () => {
    bridge = await SigBridge.create(factory);
    onnx = await createOnnxClassifier({ ort, model: MODEL, meta: META });
  });
  after(async () => { bridge.clearClassifier(); await onnx.dispose(); });

  // Fake-classifier scenarios: the recorded label, as a one-class result.
  const stub = (label, confidence) => () => ({ classes: [label], probabilities: [confidence] });

  it('replays all 84 recorded scenarios', async () => {
    assert.equal(RECORDS.length, 84);
    const failures = [];
    let compared = 0, errors = 0, tooShort = 0, decodes = 0, detections = 0;

    for (const r of RECORDS) {
      const fail = (what, got, want) => failures.push(`${r.name}: ${what} (got ${String(got).slice(0, 60)}, expected ${String(want).slice(0, 60)})`);

      if (r.classifier === 'real') bridge.setClassifier(onnx.classify);
      else bridge.setClassifier(stub(r.label ?? 'bpsk', r.confidence ? Number(r.confidence) : 1));

      const bytes = fileBytes(r.file);
      const numSamples = r.num_samples ? Number(r.num_samples) : null;
      let result, thrown;
      try {
        result = await bridge.runPipeline(asFile(r.file), optionsOf(r));
      } catch (e) {
        thrown = e;
      }

      // The browser build always runs the ML model, which needs >= 64 samples
      // (one FFT window of its preprocessing); the original's *fake* classifier
      // never looks at the samples, so a very short capture is the one place a
      // recorded success legitimately becomes an error here.
      if (!r.error && numSamples !== null && numSamples < 64) {
        if (!(thrown instanceof Error) || !/at least 64 samples/.test(thrown.message)) fail('short capture should be rejected by ML preprocessing', thrown?.message, 'at least 64 samples');
        tooShort++;
        continue;
      }

      if (r.error) {
        errors++;
        if (!(thrown instanceof Error)) fail(`original raised ${r.error}`, 'no error', 'an Error');
        continue;
      }
      if (thrown) { fail('threw unexpectedly', thrown.message, 'a result'); continue; }
      compared++;

      const eq = (what, got, want) => { if (got !== want) fail(what, got, want); };
      eq('file_path', result.filePath, r.file);
      eq('label', result.predictedModulation, r.label);
      eq('file_format', result.fileFormat, r.file_format);
      eq('num_samples', result.numSamples, Number(r.num_samples));
      eq('sample_rate', result.sampleRate, Number(r.out_sample_rate));
      if (r.classifier === 'real' && Math.abs(result.predictedConfidence - Number(r.confidence)) > 1e-4) fail('confidence', result.predictedConfidence, r.confidence);
      if (r.classifier !== 'real') eq('confidence', result.predictedConfidence, Number(r.confidence));

      const sp = result.spectrogram;
      eq('spectrogram frames', sp.frames, Number(r.spec_frames));
      eq('spectrogram nfft', sp.nfft, Number(r.spec_nfft));
      eq('freq_first', sp.freqs[0], Number(r.freq_first));
      eq('freq_last', sp.freqs[sp.freqs.length - 1], Number(r.freq_last));
      eq('time_last', sp.times[sp.times.length - 1], Number(r.time_last));
      eq('spectrogram data length', sp.data.length, sp.frames * sp.nfft);
      const mean = sp.data.reduce((a, b) => a + b, 0) / sp.data.length;
      if (Math.abs(mean - Number(r.spec_mean)) > 1e-3) fail('spectrogram mean', mean, r.spec_mean);

      if (r.file.endsWith('.iq')) {  // the samples are exactly the file's float32 pairs
        const raw = new Float32Array(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.length));
        if (result.samples.length !== raw.length || raw.some((v, i) => v !== result.samples[i])) fail('samples differ from the file', result.samples.length, raw.length);
      }

      eq('demodulated_bits', bitsStr(result.demodulatedBits), r.demodulated);
      eq('deinterleaved_bits', result.deinterleavedBits ? bitsStr(result.deinterleavedBits) : 'none', r.deinterleaved);
      eq('decoded_bits', result.decodedBits ? bitsStr(result.decodedBits) : 'none', r.decoded);
      eq('detected_interleaver', result.detectedInterleaver ? result.detectedInterleaver.kind : 'none', r.detected_interleaver);
      eq('detected_fec', optStr(result.detectedFec), r.detected_fec);
      eq('detection_score', optStr(result.detectionScore), r.detection_score);

      const b = result.boundaries;
      eq('boundaries.header_pattern', bitsStr(b.headerPattern), r.b_pattern);
      eq('boundaries.frame_period', optStr(b.framePeriod), r.b_period);
      eq('boundaries.header_positions', b.headerPositions.length ? b.headerPositions.join(',') : '-', r.b_positions);
      eq('boundaries.payloads', b.payloads.length ? b.payloads.map(bitsStr).join(',') : '-', r.b_payloads);

      if (result.detectionScore !== null) detections++;
      if (result.decodedBits) decodes++;
    }

    assert.equal(failures.length, 0, `${failures.length} mismatches:\n  ${failures.slice(0, 8).join('\n  ')}`);
    assert.equal(compared + errors + tooShort, 84);
    assert.equal(errors, 12);      // the cases where the original itself raises
    assert.equal(tooShort, 5);     // captures of 8, 9, 10, 32, 48 samples: fine for its stub classifier, too short for the ML model
    assert.ok(compared >= 60 && detections >= 20 && decodes >= 30, `coverage: ${compared} compared, ${detections} detections, ${decodes} decodes`);
    console.log(`         ${compared} scenarios matched the original field by field; ${errors} original errors rejected; ${tooShort} short captures rejected by the ML step; ${detections} with blind detection, ${decodes} FEC-decoded`);
  });

  it('recovers the demo message end to end with the REAL model in the loop', async () => {
    bridge.setClassifier(onnx.classify);
    const r = RECORDS.find((x) => x.name === 'demo_ldpc_block_real');
    const result = await bridge.runPipeline(asFile(r.file), optionsOf(r));
    assert.equal(result.predictedModulation, r.label);          // what the original's model said
    assert.equal(result.predictedModulation, 'bpsk');           // and what the native test_demo_captures asserts
    assert.ok(result.decodedBits instanceof Uint8Array);
    const text = (bits) => { const b = new Uint8Array(bits.length / 8); bits.forEach((bit, i) => { b[i >> 3] |= bit << (7 - (i & 7)); }); return new TextDecoder().decode(b); };
    assert.equal(text(result.decodedBits), 'SIH2026!');         // the message survives LDPC + block interleaving + BPSK + the real model
    const fsk = RECORDS.find((x) => x.name === 'demo_fsk_plain_real');
    const fskResult = await bridge.runPipeline(asFile(fsk.file), optionsOf(fsk));
    assert.equal(fskResult.predictedModulation, 'fsk');
    const bytes = new Uint8Array(fskResult.demodulatedBits.length / 8);
    fskResult.demodulatedBits.forEach((bit, i) => { bytes[i >> 3] |= bit << (7 - (i & 7)); });
    assert.equal(new TextDecoder().decode(bytes), 'SIH2026!');
  });

  it('runs overlapping pipelines without mixing their results', async () => {
    const chosen = RECORDS.filter((r) => !r.error && r.classifier === 'fake' && Number(r.num_samples) >= 64).slice(0, 12);
    // one classifier for all: it answers per call from the label the *caller* encoded in the input
    bridge.setClassifier(async (spec) => ({ classes: ['bpsk', 'qpsk', 'fsk'], probabilities: [1, 0, 0] }));
    const results = await Promise.all(chosen.map((r) => bridge.runPipeline(asFile(r.file), { ...optionsOf(r), sampleRate: Number(r.out_sample_rate) })));
    results.forEach((res, i) => assert.equal(res.filePath, chosen[i].file));
    assert.equal(bridge.pendingClassifications, 0);
  });

  it('results stay valid after the WASM heap grows (they are copies)', async () => {
    bridge.setClassifier(stub('fsk', 1));
    const r = RECORDS.find((x) => x.name === 'demo_fsk_fake');
    const result = await bridge.runPipeline(asFile(r.file), optionsOf(r));
    const before = bitsStr(result.demodulatedBits);
    const samples = result.samples.slice();
    await bridge.ingest(new Float32Array(6_000_000), { filename: 'grow.iq', sampleRate: 1 });   // forces growth
    assert.equal(bitsStr(result.demodulatedBits), before);
    assert.deepEqual(result.samples, samples);
  });
});

describe('runPipeline option validation and failure modes', () => {
  let bridge;
  const iq = () => new File([fileBytes('mod_bpsk.iq')], 'mod_bpsk.iq');
  before(async () => {
    bridge = await SigBridge.create(factory);
    bridge.setClassifier(() => ({ classes: ['bpsk'], probabilities: [1] }));
  });
  after(() => bridge.clearClassifier());

  it('accepts no options at all, or null', async () => {
    assert.equal((await bridge.runPipeline(iq())).sampleRate, 1e6);   // raw .IQ default rate
    assert.equal((await bridge.runPipeline(iq(), { sampleRate: null })).sampleRate, 1e6);
  });

  it('rejects unknown options (typos) at every level', async () => {
    await assert.rejects(bridge.runPipeline(iq(), { fecMsglen: 8 }), /unknown pipeline option 'fecMsglen'/);
    await assert.rejects(bridge.runPipeline(iq(), { demod: { bps: 1 } }), /unknown demod option 'bps'/);
    await assert.rejects(bridge.runPipeline(iq(), { interleaver: { kind: 'block', rows: 2, cols: 2, cols2: 1 } }), /unknown interleaver option 'cols2'/);
  });

  it('rejects wrongly typed or out-of-range options with the option name', async () => {
    const bad = [
      [{ sampleRate: '8000' }, /sampleRate must be a finite number/],
      [{ sampleRate: NaN }, /sampleRate must be a finite number/],
      [{ samplesPerSymbol: -1 }, /samplesPerSymbol must be a non-negative integer/],
      [{ samplesPerSymbol: 1.5 }, /samplesPerSymbol must be a non-negative integer/],
      [{ fecMsgLen: 2 ** 32 }, /fecMsgLen must be a non-negative integer/],
      [{ fecScheme: 5 }, /fecScheme must be a string/],
      [{ autoDetect: 1 }, /autoDetect must be a boolean/],
      [{ headerPattern: '01x' }, /only 0 and 1/],
      [{ headerPattern: [0, 2] }, /only 0 and 1/],
      [{ headerPattern: 7 }, /string of 0\/1 or an array/],
      [{ interleaver: { rows: 2 } }, /interleaver\.kind must be a string/],
      [{ interleaver: { kind: 'block', rows: -3 } }, /interleaver\.rows must be a non-negative integer/],
      [{ demod: { bandwidth: 'wide' } }, /demod\.bandwidth must be a finite number/],
    ];
    for (const [options, pattern] of bad) {
      await assert.rejects(bridge.runPipeline(iq(), options), pattern, JSON.stringify(options));
    }
    // straight at the Embind layer (bypassing the wrapper): a synchronous, real Error
    assert.throws(() => bridge.module.runPipeline(new Uint8Array(8), 'x.iq', 'not an object', () => {}), (e) =>
      e instanceof Error && /must be an object/.test(e.message));
  });

  it('accepts a header pattern as an array or typed array of bits', async () => {
    const a = await bridge.runPipeline(iq(), { sampleRate: 4000, samplesPerSymbol: 2, headerPattern: [1, 0, 1, 1] });
    assert.deepEqual([...a.boundaries.headerPattern], [1, 0, 1, 1]);
    const b = await bridge.runPipeline(iq(), { sampleRate: 4000, samplesPerSymbol: 2, headerPattern: Uint8Array.of(1, 0, 1, 1) });
    assert.deepEqual([...b.boundaries.headerPattern], [1, 0, 1, 1]);
  });

  it('rejects, never throws synchronously, for bad files and pipeline errors', async () => {
    await assert.rejects(bridge.runPipeline(new Uint8Array(64), { filename: 'mystery.bin' }), /unsupported file extension: '.bin'/);
    await assert.rejects(bridge.runPipeline(new Uint8Array(13), { filename: 'odd.iq' }), /not aligned/);
    await assert.rejects(bridge.runPipeline(iq(), { fecScheme: 'ldpc' }), /fec_msg_len is required/);
    await assert.rejects(bridge.runPipeline(iq(), { interleaver: { kind: 'zigzag' } }), /unknown interleaver: 'zigzag'/);
    await assert.rejects(bridge.runPipeline(iq(), { interleaver: { kind: 'block', rows: 2 } }), /requires parameter 'cols'/);
    await assert.rejects(bridge.runPipeline('not bytes'), TypeError);
    assert.equal(bridge.pendingClassifications, 0);
  });

  it('surfaces classifier failures and a missing classifier as rejections', async () => {
    bridge.setClassifier(() => { throw new Error('model unavailable'); });
    await assert.rejects(bridge.runPipeline(iq()), /model unavailable/);
    bridge.setClassifier(() => ({ classes: ['morse'], probabilities: [1] }));
    await assert.rejects(bridge.runPipeline(iq()), /unknown modulation scheme: 'morse'/);   // pipeline stage, after the ML call
    bridge.clearClassifier();
    await assert.rejects(bridge.runPipeline(iq()), /no ML classifier registered/);
    assert.equal(bridge.pendingClassifications, 0);
    bridge.setClassifier(() => ({ classes: ['bpsk'], probabilities: [1] }));
  });

  it('result carries the documented shape', async () => {
    const r = await bridge.runPipeline(iq(), { sampleRate: 4000, samplesPerSymbol: 2 });
    assert.deepEqual(Object.keys(r).sort(), ['boundaries', 'decodedBits', 'deinterleavedBits', 'demodulatedBits', 'detectedFec',
      'detectedInterleaver', 'detectionScore', 'fileFormat', 'filePath', 'numSamples', 'predictedConfidence', 'predictedModulation',
      'sampleRate', 'samples', 'spectrogram'].sort());
    assert.equal(r.deinterleavedBits, null);
    assert.equal(r.decodedBits, null);
    assert.equal(r.detectedInterleaver, null);
    assert.ok(r.samples instanceof Float32Array && r.demodulatedBits instanceof Uint8Array);
    assert.ok(r.spectrogram.freqs instanceof Float64Array && r.spectrogram.data instanceof Float32Array);
    assert.deepEqual(Object.keys(r.boundaries).sort(), ['framePeriod', 'headerPattern', 'headerPositions', 'payloads']);
  });
});
