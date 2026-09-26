# /ml: dev-time model tooling (Python)

Python exists here only, and only while developing: nothing in `/ml` ships in
the browser app. At runtime the app runs the exported ONNX model with
`onnxruntime-web`.

| File | Role |
| --- | --- |
| `model.py` | `ModulationClassifierCNN` (verbatim from the native app) |
| `preprocessing.py` | reference for the spectrogram image; the C++ port is `core/src/ml_preprocess.cpp` |
| `export_onnx.py` | checkpoint -> `models/modulation_classifier.{onnx,json}`, verified against PyTorch |
| `make_fixtures.py` | writes `tests/data/ml/` from the **original native app** (expected images + probabilities) |
| `models/` | the exported model (360 KB, committed) and its metadata sidecar |
| `checkpoints/` | trained weights (`.pt`), gitignored |

Training itself (`dataset.py`, `train.py`) still lives in the native project: it
depends on that project's Python modulator (`app.demod`), which is not part of
this port.

## Re-exporting

```bash
python3 -m venv ~/.ml-venv && ~/.ml-venv/bin/pip install -r ml/requirements-dev.txt
cmake -B build && cmake --build build          # native build gives preprocessing its `sigcore`
PYTHONPATH=build/core ~/.ml-venv/bin/python -m ml.export_onnx
```

The export is deterministic (byte-identical for an unchanged checkpoint) and
fails unless onnxruntime agrees with PyTorch to 1e-5 at batch sizes 1, 3 and 16.
The graph ends in a softmax so the bridge receives probabilities in [0, 1]; the
`.json` sidecar carries the class names and the `[1, 64, 48]` NCHW input
contract that `web/js/onnx-classifier.mjs` checks.

## Regenerating the test fixtures

Only needed if the model or preprocessing changes; needs the native project:

```bash
~/.ml-venv/bin/python ml/make_fixtures.py --native-repo ~/signal-project/signal
```

## Known limitation of the shipped model

The bundled weights come from the native prototype's small synthetic-data run
(150 examples per class). On the fixtures it classifies bpsk and fsk correctly
but calls qpsk, psk8, qam16 and qam64 signals psk8/qpsk/psk8/psk8 (confidence
0.3 to 0.4). The port reproduces this **exactly**: that is what the parity tests
check. Better weights need retraining, then a re-export.
