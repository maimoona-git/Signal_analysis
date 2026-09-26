# app/: the Qt Widgets GUI (and the pipeline it drives)

`app/pipeline/` is the Qt-free C++ port of the native app's orchestration (see its
README). `app/gui/` is the GUI, a port of the native PyQt6 + pyqtgraph window to Qt
Widgets in C++, compiled to WebAssembly and rendered into the browser.

| Native (PyQt6 / pyqtgraph) | Here (`app/gui/`) |
| --- | --- |
| `main_window.py` | `main_window`: toolbar, splitters, status bar, run/error handling |
| `results_panel.py` | `results_panel` + `sigapp::format_result_text` (byte-identical text) |
| `waterfall_widget.py` (pyqtgraph ImageItem, viridis) | `waterfall_widget` (QPainter, pyqtgraph's viridis table) |
| `constellation_widget.py` (pyqtgraph scatter) | `constellation_widget` (QPainter) |
| (pyqtgraph axes) | `plot_axes`: nice-number ticks, axis titles, grid |
| direct call to `run_pipeline` | `AnalysisBackend`: `WasmBackend` in the browser, a stub in tests |

Browser-specific files: `wasm_backend` (runs the pipeline in this module, ML call-out to
onnxruntime-web through the bridge), `wasm_file_picker` (the `<input type="file">`),
`wasm_bindings` (Embind hooks for the page: `sigAppOpenFile`, `sigAppState`).

## Where it deliberately differs from the native app

- **Files arrive as bytes.** There is no path, so "Open File..." runs a real
  `<input type="file">` and hands the bytes to the app through the bridge
  (`Module.sigAppOpenFile`, the same entry point drag-and-drop uses). Qt's own
  `QFileDialog::getOpenFileContent` was not used: on Chromium it opens the File System
  Access picker, which behaves differently from other browsers and cannot be automated.
- **Analysis is asynchronous** (the ML model runs in JS), so "Run Analysis" is disabled
  while one is in flight, and a result that arrives after a *different* file was loaded
  is dropped.
- **The error dialog is non-modal at the C++ level** (`open()`, not `exec()`, which cannot
  block in a browser); it is still window-modal on screen, so it has to be dismissed.
- **The constellation thins long captures deterministically** (4000 evenly spaced samples);
  the native app draws a random subset, which cannot be reproduced without numpy's RNG.
- **The results panel uses a fixed-width font** so its aligned columns line up, and the
  splitter starts at the intended 2:1 (Qt's own initial sizing gives ~4:1 and wraps the lines).
- The toolbar button reads "Open File" (Qt drops a trailing "..." from toolbar text, in
  the native app too).

## Testing

| What | How | Where it runs |
| --- | --- | --- |
| Results text vs the ORIGINAL panel, byte for byte (72 scenarios) | `test_result_text` | native + WASM (node) |
| Widgets: pixel colours at computed positions, orientation, equal axis scale, thinning, window behaviour with a stub backend, panel text vs the original GUI's for the same run | `test_gui` (21 tests) | native desktop Qt, offscreen platform |
| The real app: mouse clicks, typing, a real file chooser, real ONNX model, panel text vs the original, waterfall pixels are exact viridis colours, error path, a 500,000-sample capture | `tests/gui/browser-gui.test.mjs` | headless Chrome (`scripts/browser-gui-test.sh`, also a ctest) |

## Performance

In headless Chrome (single-threaded WASM): a 512-sample capture analyses in ~70 ms,
including the ML inference; a 500,000-sample (4 MB) capture takes under 0.2 s from load
to the finished display. The pipeline runs on the UI thread; at these sizes that does not
block noticeably. If much larger captures matter, the plan from `CLAUDE.md` applies (a
Web Worker running a second WASM instance), not in-module threads.
