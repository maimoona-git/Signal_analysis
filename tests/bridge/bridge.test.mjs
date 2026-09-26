// Phase 4: JS-driven tests of the Embind bridge, run against the compiled
// sigbridge module under node (node --test). The "browser" here is a fake JS
// caller: it hands over bytes exactly as a File read would, registers a fake ML
// classifier, and checks what comes back through the bridge.

import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { after, before, describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { SigBridge } from '../../bridge/js/sigbridge.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const modulePath = process.env.SIGBRIDGE_JS ?? path.resolve(here, '../../build-wasm/bridge/sigbridge.js');
const factory = createRequire(import.meta.url)(modulePath);

// --- byte builders ----------------------------------------------------------

function wavBytes({ channels = 1, rate = 8000, samples = [0, 16384, -16384, 32767] } = {}) {
  const data = new Int16Array(samples);
  const dataBytes = new Uint8Array(data.buffer);
  const out = new Uint8Array(44 + dataBytes.length);
  const dv = new DataView(out.buffer);
  const tag = (o, s) => [...s].forEach((c, i) => (out[o + i] = c.charCodeAt(0)));
  tag(0, 'RIFF'); dv.setUint32(4, 36 + dataBytes.length, true); tag(8, 'WAVE');
  tag(12, 'fmt '); dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, channels, true);
  dv.setUint32(24, rate, true); dv.setUint32(28, rate * channels * 2, true);
  dv.setUint16(32, channels * 2, true); dv.setUint16(34, 16, true);
  tag(36, 'data'); dv.setUint32(40, dataBytes.length, true);
  out.set(dataBytes, 44);
  return out;
}

const iqBytes = (floats) => new Uint8Array(new Float32Array(floats).buffer);

// --- tests ------------------------------------------------------------------

describe('bridge: file bytes in', () => {
  let bridge;
  before(async () => { bridge = await SigBridge.create(factory); });

  it('parses WAV bytes handed over as a Uint8Array', async () => {
    const sig = await bridge.ingest(wavBytes({ rate: 44100 }), { filename: 'tone.wav' });
    assert.equal(sig.sampleRate, 44100);
    assert.equal(sig.format, 'wav_mono_real');
    assert.equal(sig.numSamples, 4);
    assert.ok(sig.samples instanceof Float32Array);
    assert.equal(sig.samples.length, 8); // interleaved I,Q
    assert.deepEqual([...sig.samples], [0, 0, 0.5, 0, -0.5, 0, 32767 / 32768, 0]);
    assert.ok(Math.abs(sig.durationSec - 4 / 44100) < 1e-9);
  });

  it('parses stereo WAV as I/Q', async () => {
    const sig = await bridge.ingest(wavBytes({ channels: 2, samples: [16384, -16384, 8192, 0] }), { filename: 'iq.wav' });
    assert.equal(sig.format, 'wav_stereo_iq');
    assert.deepEqual([...sig.samples], [0.5, -0.5, 0.25, 0]);
  });

  it('parses raw .IQ with the supplied sample rate', async () => {
    const sig = await bridge.ingest(iqBytes([1, 2, -3.5, 0.25]), { filename: 'cap.iq', sampleRate: 2.4e6 });
    assert.equal(sig.format, 'iq_cf32');
    assert.equal(sig.sampleRate, 2.4e6);
    assert.deepEqual([...sig.samples], [1, 2, -3.5, 0.25]);
  });

  it('accepts a File (uses its name and reads it asynchronously)', async () => {
    const file = new File([wavBytes()], 'from_input_element.wav');
    const sig = await bridge.ingest(file);
    assert.equal(sig.numSamples, 4);
    const iqFile = new File([iqBytes([1, 1, 2, 2])], 'capture.IQ');
    assert.equal((await bridge.ingest(iqFile, { sampleRate: 1000 })).numSamples, 2);
  });

  it('accepts a Blob and a bare ArrayBuffer', async () => {
    assert.equal((await bridge.ingest(new Blob([wavBytes()]), { filename: 'b.wav' })).numSamples, 4);
    const ab = wavBytes().buffer;
    assert.equal((await bridge.ingest(ab, { filename: 'ab.wav' })).numSamples, 4);
  });

  it('respects byteOffset/byteLength of a subarray view', async () => {
    const wav = wavBytes();
    const padded = new Uint8Array(wav.length + 100);
    padded.fill(0xAB);
    padded.set(wav, 37);
    const view = padded.subarray(37, 37 + wav.length);
    assert.equal(view.byteOffset, 37);
    const sig = await bridge.ingest(view, { filename: 'view.wav' });
    assert.deepEqual([...sig.samples], [0, 0, 0.5, 0, -0.5, 0, 32767 / 32768, 0]);
  });

  it('sniffs WAV by content regardless of the filename', async () => {
    assert.equal((await bridge.ingest(wavBytes(), { filename: 'no_extension' })).format, 'wav_mono_real');
  });

  it('handles an empty capture and a large one (memory growth)', async () => {
    assert.equal((await bridge.ingest(new Uint8Array(0), { filename: 'e.iq', sampleRate: 1 })).numSamples, 0);
    const n = 1_500_000; // 12 MB of IQ, well past the initial heap
    const big = new Float32Array(2 * n);
    for (let i = 0; i < big.length; i++) big[i] = (i % 1000) / 1000;
    const sig = await bridge.ingest(big, { filename: 'big.iq', sampleRate: 1e6 });
    assert.equal(sig.numSamples, n);
    assert.equal(sig.samples.length, 2 * n);
    assert.equal(sig.samples[1999], big[1999]);  // float32 in, float32 out, bit-exact
    assert.equal(sig.samples[2 * n - 1], big[2 * n - 1]);
    // and the module is still healthy afterwards
    assert.equal((await bridge.ingest(wavBytes(), { filename: 'after.wav' })).numSamples, 4);
  });

  it('rejects bad input with a real Error carrying the C++ message', async () => {
    await assert.rejects(bridge.ingest(iqBytes([1, 2]), { filename: 'x.iq' }), (e) =>
      e instanceof Error && /sample rate is required/.test(e.message));
    await assert.rejects(bridge.ingest(new Uint8Array([1, 2, 3]), { filename: 'odd.iq', sampleRate: 1 }), (e) =>
      e instanceof Error && /not aligned/.test(e.message));
    await assert.rejects(bridge.ingest(new Uint8Array(64), { filename: 'bad.wav' }), (e) =>
      e instanceof Error && /RIFF\/WAVE/.test(e.message));
    const truncated = wavBytes().subarray(0, 30);
    await assert.rejects(bridge.ingest(truncated, { filename: 'cut.wav' }), Error);
  });

  it('rejects non-byte input', async () => {
    await assert.rejects(bridge.ingest('not bytes'), TypeError);
    await assert.rejects(bridge.ingest(null), TypeError);
    // straight at the Embind layer, bypassing the wrapper's own check
    assert.throws(() => bridge.module.ingestFile({ length: 3 }, 'x.iq', 1), (e) =>
      e instanceof Error && /ArrayBuffer or typed array/.test(e.message));
  });

  it('survives corrupted headers without crashing the module', async () => {
    const good = wavBytes();
    for (let i = 0; i < 200; i++) {
      const bad = good.slice();
      bad[(i * 7) % bad.length] ^= 0xFF;
      bad[(i * 13 + 5) % bad.length] = (i * 31) & 0xFF;
      try { await bridge.ingest(bad, { filename: 'fuzz.wav' }); } catch (e) { assert.ok(e instanceof Error); }
    }
    assert.equal((await bridge.ingest(good, { filename: 'ok.wav' })).numSamples, 4);
  });
});

describe('bridge: C++ -> JS ML call-out -> C++', () => {
  let bridge;
  before(async () => { bridge = await SigBridge.create(factory); });
  after(() => bridge.clearClassifier());

  const CLASSES = ['BPSK', 'QPSK', 'FSK'];

  it('calls the JS classifier with the spectrogram and returns the validated result', async () => {
    const spec = Float32Array.from({ length: 12 }, (_, i) => i * 0.5 - 1);
    let seen;
    bridge.setClassifier((input, rows, cols) => {
      seen = { input, rows, cols };
      return { classes: CLASSES, probabilities: [0.1, 0.75, 0.15] };
    });
    const result = await bridge.classify(spec, 3, 4);

    assert.ok(seen.input instanceof Float32Array);
    assert.deepEqual([...seen.input], [...spec]);
    assert.equal(seen.rows, 3);
    assert.equal(seen.cols, 4);
    assert.equal(result.label, 'QPSK');
    assert.equal(result.topIndex, 1);
    assert.ok(Math.abs(result.confidence - 0.75) < 1e-6);
    assert.deepEqual(result.classes, CLASSES);
    assert.equal(result.probabilities.length, 3);
    assert.ok(Math.abs(result.probabilities[2] - 0.15) < 1e-6);
  });

  it('awaits an async classifier (real Promise, real delay)', async () => {
    bridge.setClassifier(async () => {
      await new Promise((r) => setTimeout(r, 25));
      return { classes: CLASSES, probabilities: [0.9, 0.05, 0.05] };
    });
    assert.equal((await bridge.classify(new Float32Array(4), 2, 2)).label, 'BPSK');
  });

  it('accepts typed arrays and any iterable in the classifier result', async () => {
    bridge.setClassifier(() => ({ classes: new Set(CLASSES), probabilities: new Float32Array([0.2, 0.3, 0.5]) }));
    assert.equal((await bridge.classify([1, 2, 3, 4], 2, 2)).label, 'FSK');
  });

  it('gives the classifier a private copy of the input', async () => {
    const spec = new Float32Array([1, 2, 3, 4]);
    let held;
    bridge.setClassifier((input) => { held = input; return { classes: ['a'], probabilities: [1] }; });
    await bridge.classify(spec, 2, 2);
    spec.fill(99);
    assert.deepEqual([...held], [1, 2, 3, 4]);
    // and it stays readable after the WASM heap grows
    await bridge.ingest(new Float32Array(4_000_000), { filename: 'grow.iq', sampleRate: 1 });
    assert.deepEqual([...held], [1, 2, 3, 4]);
  });

  it('turns every classifier failure into a rejected Error', async () => {
    const cases = [
      ['throws synchronously', () => { throw new Error('model exploded'); }, /model exploded/],
      ['throws a non-Error', () => { throw 'plain string'; }, /plain string/],
      ['rejects', async () => { throw new Error('async boom'); }, /async boom/],
      ['returns undefined', () => undefined, /must return an object/],
      ['returns a number', () => 42, /must return an object/],
      ['omits classes', () => ({ probabilities: [1] }), /result\.classes/],
      ['omits probabilities', () => ({ classes: ['a'] }), /result\.probabilities/],
      ['non-iterable classes', () => ({ classes: 5, probabilities: [1] }), /result\.classes/],
      ['length mismatch', () => ({ classes: ['a', 'b'], probabilities: [1] }), /2 classes but 1 probabilities/],
      ['NaN probability', () => ({ classes: ['a'], probabilities: [NaN] }), /finite number/],
      ['non-numeric probability', () => ({ classes: ['a'], probabilities: ['x'] }), /finite number/],
      ['probability > 1 (raw logits)', () => ({ classes: ['a', 'b'], probabilities: [2.5, -1] }), /finite number/],
      ['empty class list', () => ({ classes: [], probabilities: [] }), /no classes/],
    ];
    for (const [name, fn, pattern] of cases) {
      bridge.setClassifier(fn);
      await assert.rejects(bridge.classify(new Float32Array(4), 2, 2), (e) => {
        assert.ok(e instanceof Error, `${name}: not an Error`);
        assert.match(e.message, pattern, name);
        return true;
      });
    }
    // still usable after all that
    bridge.setClassifier(() => ({ classes: ['ok'], probabilities: [1] }));
    assert.equal((await bridge.classify(new Float32Array(4), 2, 2)).label, 'ok');
  });

  it('rejects (never throws re-entrantly) when no classifier is registered', async () => {
    bridge.clearClassifier();
    let sync = true, order = [];
    const p = bridge.classify(new Float32Array(4), 2, 2).catch((e) => { order.push(sync ? 'sync' : 'async'); throw e; });
    sync = false;
    await assert.rejects(p, /no ML classifier registered/);
    assert.deepEqual(order, ['async']);
  });

  it('validates the call arguments', async () => {
    bridge.setClassifier(() => ({ classes: ['a'], probabilities: [1] }));
    await assert.rejects(bridge.classify(new Float32Array(5), 2, 2), (e) =>
      e instanceof Error && /rows \* cols/.test(e.message));
    await assert.rejects(bridge.classify('nope', 1, 1), TypeError);
    assert.throws(() => bridge.module.classifySpectrogram([1, 2, 3, 4], 2, 2, () => {}), (e) =>
      e instanceof Error && /Float32Array/.test(e.message));
  });

  it('keeps concurrent classifications separate and releases every callback', async () => {
    bridge.setClassifier(async (input) => {
      const tag = input[0];
      await new Promise((r) => setTimeout(r, tag === 1 ? 40 : tag === 2 ? 5 : 20));
      return { classes: ['first', 'second', 'third'], probabilities: [tag === 1 ? 1 : 0, tag === 2 ? 1 : 0, tag === 3 ? 1 : 0] };
    });
    const before = bridge.pendingClassifications;
    const pending = [1, 2, 3].map((t) => bridge.classify(new Float32Array([t, 0, 0, 0]), 2, 2));
    assert.equal(bridge.pendingClassifications, before + 3);
    const results = await Promise.all(pending);
    assert.deepEqual(results.map((r) => r.label), ['first', 'second', 'third']); // 2 finished first, order preserved per call
    assert.equal(bridge.pendingClassifications, before);
  });

  it('a failing classifier does not leak pending callbacks', async () => {
    bridge.setClassifier(() => { throw new Error('nope'); });
    for (let i = 0; i < 20; i++) await assert.rejects(bridge.classify(new Float32Array(4), 2, 2));
    assert.equal(bridge.pendingClassifications, 0);
  });

  it('a throwing completion callback cannot wedge the bridge', async () => {
    bridge.setClassifier(() => ({ classes: ['a'], probabilities: [1] }));
    await new Promise((resolve) => {
      bridge.module.classifySpectrogram(new Float32Array(4), 2, 2, () => resolve());
    });
    assert.equal(bridge.pendingClassifications, 0);
    assert.equal((await bridge.classify(new Float32Array(4), 2, 2)).label, 'a');
  });

  it('full path: bytes in, then a spectrogram of them out through the ML bridge', async () => {
    const sig = await bridge.ingest(wavBytes({ samples: [100, 200, 300, 400] }), { filename: 'chain.wav' });
    // a stand-in "spectrogram": the real part of each sample, as a 2x2 image
    const spec = Float32Array.from({ length: 4 }, (_, i) => sig.samples[2 * i]);
    let received;
    bridge.setClassifier((input) => { received = [...input]; return { classes: ['tone', 'noise'], probabilities: [0.8, 0.2] }; });
    const result = await bridge.classify(spec, 2, 2);
    assert.deepEqual(received, [...spec]);
    assert.equal(result.label, 'tone');
  });
});
