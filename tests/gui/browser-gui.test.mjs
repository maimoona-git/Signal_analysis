// Phase 7: the browser app, end to end in real Chrome, driven like a user: mouse clicks
// on the Qt canvas, typing into its fields, and a REAL browser file chooser for
// "Open File...". The app is served from a plain static server (no COOP/COEP headers).
//
// What must come out matches the ORIGINAL native GUI: the results text is compared
// byte for byte with what the original PyQt6 panel showed for the same capture and
// settings (tests/data/pipeline/pipeline.rec), and the waterfall's pixels are checked
// against the viridis table.
//
//   npm install && scripts/build-wasm.sh && scripts/stage-web.sh && node --test tests/gui/browser-gui.test.mjs

import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { after, before, describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';

import puppeteer from 'puppeteer-core';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const dist = process.env.SIG_DIST ?? path.join(root, 'build-wasm/dist');
const dataDir = path.join(root, 'tests/data/pipeline');
const shotDir = path.join(root, 'build-wasm/gui-screenshots');
const CHROME = process.env.CHROME ?? [
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', '/usr/bin/google-chrome', '/usr/bin/chromium',
].find((p) => fs.existsSync(p));

// --- fixtures ---------------------------------------------------------------------

function readRecords(file) {
  const records = [];
  let cur = {};
  for (const line of fs.readFileSync(path.join(dataDir, file), 'utf8').split('\n')) {
    if (line === '---') { records.push(cur); cur = {}; } else if (line) { const i = line.indexOf(' '); cur[line.slice(0, i)] = line.slice(i + 1); }
  }
  return records;
}
const RECORDS = readRecords('pipeline.rec');
const record = (name) => RECORDS.find((r) => r.name === name);
const fromHex = (h) => Buffer.from(h, 'hex').toString('utf8');
// viridis table -> Set of packed colours, to check the waterfall is really drawn with it
const VIRIDIS = new Set(
  [...fs.readFileSync(path.join(root, 'app/gui/viridis_table.inc'), 'utf8').matchAll(/\{(\d+), (\d+), (\d+)\}/g)]
    .map((m) => (Number(m[1]) << 16) | (Number(m[2]) << 8) | Number(m[3])));

// --- a plain static file server (no special headers) --------------------------------

const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json', '.onnx': 'application/octet-stream' };
function serve(dir) {
  return new Promise((resolve) => {
    const server = http.createServer((req, res) => {
      const file = path.join(dir, decodeURIComponent(new URL(req.url, 'http://x').pathname));
      if (!file.startsWith(dir) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
        const index = path.join(file, 'index.html');
        if (file.startsWith(dir) && fs.existsSync(index)) { res.writeHead(200, { 'content-type': 'text/html' }); return res.end(fs.readFileSync(index)); }
        res.writeHead(404); return res.end('not found');
      }
      res.writeHead(200, { 'content-type': TYPES[path.extname(file)] ?? 'application/octet-stream' });
      res.end(fs.readFileSync(file));
    });
    server.listen(0, '127.0.0.1', () => resolve(server));
  });
}

// --- the test ---------------------------------------------------------------------------

describe('the Qt-WASM app in real Chrome', () => {
  let server, browser, page, origin;
  const consoleErrors = [];

  const state = () => page.evaluate(() => JSON.parse(JSON.stringify(window.sigApp.sigAppState())));
  const waitFor = async (what, predicate, timeout = 90000) => {
    const t0 = Date.now();
    for (;;) {
      const s = await state();
      if (predicate(s)) return s;
      if (Date.now() - t0 > timeout) assert.fail(`timed out waiting for ${what}; status was: ${s.status}`);
      await new Promise((r) => setTimeout(r, 100));
    }
  };
  const click = async (p) => { await page.mouse.click(p.x, p.y); await new Promise((r) => setTimeout(r, 50)); };
  const shot = async (name) => { fs.mkdirSync(shotDir, { recursive: true }); await page.screenshot({ path: path.join(shotDir, name + '.png') }); };
  const sample = (rect) => page.evaluate(({ x, y, width, height }) => {   // canvas pixels as packed RGB
    // Qt renders inside a shadow root
    const c = document.getElementById('qt-shadow-container').shadowRoot.querySelector('canvas');
    const data = c.getContext('2d').getImageData(Math.round(x), Math.round(y), Math.round(width), Math.round(height)).data;
    const out = new Uint32Array(data.length / 4);
    for (let i = 0; i < out.length; i++) out[i] = (data[4 * i] << 16) | (data[4 * i + 1] << 8) | data[4 * i + 2];
    return Array.from(out);
  }, rect);

  before(async () => {
    assert.ok(CHROME, 'Chrome not found (set CHROME=...)');
    assert.ok(fs.existsSync(path.join(dist, 'vendor/ort/ort.wasm.min.mjs')), `${dist} is not staged: run scripts/build-wasm.sh && scripts/stage-web.sh`);
    server = await serve(dist);
    origin = `http://127.0.0.1:${server.address().port}/`;
    browser = await puppeteer.launch({ executablePath: CHROME, headless: true, args: ['--no-sandbox', '--disable-gpu'], defaultViewport: { width: 1200, height: 800 } });
    page = await browser.newPage();
    page.on('console', (m) => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) consoleErrors.push(m.text()); });
    page.on('response', (r) => { if (r.status() >= 400 && !/favicon\.ico$/.test(r.url())) consoleErrors.push(`${r.status()} ${r.url()}`); });   // a missing asset, but not the browser's favicon probe
    page.on('pageerror', (e) => consoleErrors.push(String(e)));
    await page.goto(origin);
    await page.waitForFunction(() => window.sigApp && window.sigApp.sigAppState().window, { timeout: 90000 });
  });
  after(async () => { await browser?.close(); server?.close(); });

  it('starts as the native app does: empty plots, run disabled, prompt in the status bar', async () => {
    const s = await state();
    assert.equal(s.status, 'Open a .wav or .IQ file to begin.');
    assert.equal(s.canRun, false);
    assert.equal(s.resultText, '');
    assert.equal(s.waterfall.hasData, false);
    assert.equal(s.constellation.points, 0);
    assert.equal(s.samplesPerSymbol, 1);
    assert.equal(s.autoDetect, false);
    assert.equal(s.sampleRateText, '');
    assert.ok(s.window.width >= 1000 && s.window.height >= 600, 'the window fills the page');
    assert.equal(await page.evaluate(() => self.crossOriginIsolated), false, 'served without COOP/COEP (single-threaded build)');
    await shot('1-initial');
  });

  it('Open File... uses a real browser file chooser, then the analysis reproduces the original GUI exactly', async () => {
    let s = await state();
    const [chooser] = await Promise.all([page.waitForFileChooser({ timeout: 10000 }), click(s.controls.open)]);
    await chooser.accept([path.join(dataDir, 'bpsk_ldpc_block.iq')]);
    s = await waitFor('the file to load', (x) => x.filename === 'bpsk_ldpc_block.iq');
    assert.equal(s.status, "Loaded bpsk_ldpc_block.iq. Click 'Run Analysis' to proceed.");
    assert.equal(s.canRun, true);
    await shot('2-loaded');

    // the settings the original scenario used: sample rate 8000, 4 samples/symbol, auto-detect on
    await click(s.controls.sampleRate);
    await page.keyboard.type('8000');
    await click(s.controls.samplesPerSymbol);
    await page.keyboard.press('End');
    for (let i = 0; i < 3; i++) await page.keyboard.press('Backspace');
    await page.keyboard.type('4');
    await click(s.controls.autoDetect);
    s = await state();
    assert.equal(s.sampleRateText, '8000');
    assert.equal(s.samplesPerSymbol, 4);
    assert.equal(s.autoDetect, true);

    const t0 = Date.now();
    await click(s.controls.run);
    s = await waitFor('the analysis', (x) => x.status.startsWith('Done.') || x.status === 'Analysis failed.');
    console.log(`         analysis of a 512-sample capture with the real model: ${Date.now() - t0} ms (incl. first-run model load)`);

    const original = record('demo_ldpc_block_autodetect');
    assert.equal(s.status, fromHex(original.status_hex));                    // "Done. Predicted modulation: bpsk (100% confidence)."
    assert.equal(s.resultText, fromHex(original.panel_text_hex));           // byte for byte what the original GUI showed
    assert.equal(s.waterfall.hasData, true);
    assert.equal(s.waterfall.frames, Number(original.spec_frames));
    assert.equal(s.waterfall.nfft, Number(original.spec_nfft));
    assert.equal(s.constellation.points, 512);                              // all samples (< 4000)
    assert.equal(s.canRun, true);
    await shot('3-analysed-ldpc');
  });

  it('the waterfall is drawn with the viridis colour map and the constellation with blue dots', async () => {
    const s = await state();
    const wf = await sample(s.waterfall.plot);
    const coloured = wf.filter((c) => c !== 0xffffff);
    assert.ok(coloured.length > wf.length * 0.8, `waterfall mostly filled (${coloured.length}/${wf.length})`);
    const inTable = coloured.filter((c) => VIRIDIS.has(c)).length;
    assert.ok(inTable > coloured.length * 0.9, `${inTable}/${coloured.length} waterfall pixels are exact viridis colours`);

    const cs = await sample(s.constellation.plot);
    const blue = cs.filter((c) => { const r = c >> 16, g = (c >> 8) & 255, b = c & 255; return b > r + 40 && b > 150; });
    assert.ok(blue.length > 20, `constellation has ${blue.length} blue dot pixels`);
  });

  it('opening another file clears the previous results', async () => {
    let s = await state();
    await page.evaluate(async (b64) => {
      const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
      window.sigApp.sigAppOpenFile(bytes, 'fsk_plain.iq');
    }, fs.readFileSync(path.join(dataDir, 'fsk_plain.iq')).toString('base64'));
    s = await waitFor('the second file', (x) => x.filename === 'fsk_plain.iq');
    assert.equal(s.status, "Loaded fsk_plain.iq. Click 'Run Analysis' to proceed.");
    assert.equal(s.resultText, '');
    assert.equal(s.waterfall.hasData, false);
    assert.equal(s.constellation.points, 0);
  });

  it('a second analysis (FSK, real model, explicit settings) matches the original again', async () => {
    let s = await state();
    // auto-detect off, 1 sample/symbol, rate 8000: the original demo_fsk_plain_real scenario
    await click(s.controls.autoDetect);
    await click(s.controls.samplesPerSymbol);
    await page.keyboard.press('End');
    for (let i = 0; i < 3; i++) await page.keyboard.press('Backspace');
    await page.keyboard.type('1');
    s = await state();
    assert.equal(s.autoDetect, false);
    assert.equal(s.samplesPerSymbol, 1);
    assert.equal(s.sampleRateText, '8000');                              // the field kept its value

    await click(s.controls.run);
    s = await waitFor('the analysis', (x) => x.status.startsWith('Done.') || x.status === 'Analysis failed.');
    const original = record('demo_fsk_plain_real');
    assert.equal(s.resultText, fromHex(original.panel_text_hex));
    assert.equal(s.status, fromHex(original.status_hex));
    assert.match(s.resultText, /Decoded message:      'SIH2026!'/);
    await shot('4-analysed-fsk');
  });

  it('a file the pipeline rejects is reported in the panel, the status bar and a dialog', async () => {
    await page.evaluate(() => window.sigApp.sigAppOpenFile(new Uint8Array(64), 'mystery.bin'));
    let s = await waitFor('the file to load', (x) => x.filename === 'mystery.bin');
    await click(s.controls.run);
    s = await waitFor('the failure', (x) => x.status === 'Analysis failed.');
    assert.equal(s.resultText, "Error: unsupported file extension: '.bin' (expected .wav or .iq)");
    assert.equal(s.dialogOpen, true);
    assert.equal(s.canRun, true);          // and the user can carry on
    await shot('5-error');
    // the dialog is window-modal, like any message box: it must be dismissed before the window responds again
    await page.keyboard.press('Enter');
    s = await waitFor('the dialog to close', (x) => !x.dialogOpen, 10000);
    assert.equal(s.status, 'Analysis failed.');
  });

  it('handles a large capture: 500,000 samples through the whole pipeline', async () => {
    const t0 = Date.now();
    await page.evaluate(() => {
      const n = 500_000, f = new Float32Array(2 * n);
      let seed = 12345;
      const rnd = () => ((seed = (seed * 1664525 + 1013904223) >>> 0) / 4294967296) * 2 - 1;
      for (let i = 0; i < n; i++) { const sym = (i >> 3) & 1 ? 1 : -1; f[2 * i] = sym + 0.2 * rnd(); f[2 * i + 1] = 0.2 * rnd(); }
      window.sigApp.sigAppOpenFile(new Uint8Array(f.buffer), 'large.iq');
    });
    let s = await waitFor('the large file', (x) => x.filename === 'large.iq');
    await click(s.controls.run);
    s = await waitFor('the large analysis', (x) => x.status.startsWith('Done.') || x.status === 'Analysis failed.', 120000);
    const ms = Date.now() - t0;
    console.log(`         500,000-sample capture: ${ms} ms from load to Done`);
    assert.ok(s.status.startsWith('Done.'), s.status);
    assert.match(s.resultText, /Num samples:          500000/);
    assert.equal(s.constellation.points, 4000);                            // thinned for display
    assert.equal(s.waterfall.hasData, true);
    assert.ok(ms < 60000, `took ${ms} ms`);
    await shot('6-large');
  });

  it('logged no errors', async () => {
    assert.deepEqual(consoleErrors, []);
  });
});
