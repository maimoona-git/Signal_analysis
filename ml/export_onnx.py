"""Exports the trained modulation classifier (PyTorch checkpoint) to ONNX for
in-browser inference with onnxruntime-web. Dev-time only: Python never runs in
the shipped app.

    python -m ml.export_onnx [--checkpoint ml/checkpoints/modulation_classifier.pt]
                             [--out-dir ml/models]

Writes, next to each other:
    modulation_classifier.onnx   softmax-terminated graph (the bridge wants
                                 probabilities in [0, 1], not logits)
    modulation_classifier.json   class names + input/output contract, read by
                                 the JS side (web/js/onnx-classifier.mjs)

The graph takes NCHW float32 `spectrogram` [batch, 1, 64, 48] (the image from
ml.preprocessing.make_spectrogram_image / core/src/ml_preprocess.cpp) and
returns `probabilities` [batch, num_classes]. The export is verified against
PyTorch with onnxruntime before it is declared good; output is deterministic
(no timestamps) so re-exporting an unchanged checkpoint is a no-op diff.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import torch
from torch import nn

from ml.model import ModulationClassifierCNN
from ml.preprocessing import NFFT, TARGET_FRAMES

ML_DIR = Path(__file__).parent
DEFAULT_CHECKPOINT = ML_DIR / "checkpoints" / "modulation_classifier.pt"
DEFAULT_OUT_DIR = ML_DIR / "models"
MODEL_NAME = "modulation_classifier"
OPSET = 17
INPUT_NAME = "spectrogram"
OUTPUT_NAME = "probabilities"
# float32 CNN: PyTorch and onnxruntime agree to ~1e-7 in practice; 1e-5 leaves
# headroom for different conv kernels without ever hiding a real export bug.
PARITY_TOLERANCE = 1e-5


class _WithSoftmax(nn.Module):
    """The classifier plus softmax, so the exported graph yields probabilities."""

    def __init__(self, model: nn.Module):
        super().__init__()
        self.model = model

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return torch.softmax(self.model(x), dim=1)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_checkpoint(path: Path) -> tuple[ModulationClassifierCNN, list[str]]:
    if not path.exists():
        raise FileNotFoundError(f"no checkpoint at {path}")
    checkpoint = torch.load(path, map_location="cpu", weights_only=False)
    classes: list[str] = list(checkpoint["classes"])
    model = ModulationClassifierCNN(num_classes=len(classes), in_height=NFFT, in_width=TARGET_FRAMES)
    model.load_state_dict(checkpoint["state_dict"])
    model.eval()
    return model, classes


def export(checkpoint: Path, out_dir: Path) -> tuple[Path, Path]:
    model, classes = load_checkpoint(checkpoint)
    wrapped = _WithSoftmax(model).eval()

    out_dir.mkdir(parents=True, exist_ok=True)
    onnx_path = out_dir / f"{MODEL_NAME}.onnx"
    meta_path = out_dir / f"{MODEL_NAME}.json"

    dummy = torch.zeros(1, 1, NFFT, TARGET_FRAMES, dtype=torch.float32)
    torch.onnx.export(
        wrapped,
        dummy,
        str(onnx_path),
        input_names=[INPUT_NAME],
        output_names=[OUTPUT_NAME],
        dynamic_axes={INPUT_NAME: {0: "batch"}, OUTPUT_NAME: {0: "batch"}},
        opset_version=OPSET,
        do_constant_folding=True,
    )

    import onnx

    onnx.checker.check_model(onnx.load(str(onnx_path)), full_check=True)

    meta = {
        "name": MODEL_NAME,
        "classes": classes,
        "input": {"name": INPUT_NAME, "layout": "NCHW", "dtype": "float32",
                  "channels": 1, "height": NFFT, "width": TARGET_FRAMES},
        "output": {"name": OUTPUT_NAME, "kind": "softmax"},
        "opset": OPSET,
        "source_checkpoint_sha256": _sha256(checkpoint),
        "onnx_sha256": _sha256(onnx_path),
    }
    meta_path.write_text(json.dumps(meta, indent=2) + "\n")
    return onnx_path, meta_path


def verify(checkpoint: Path, onnx_path: Path, seed: int = 0) -> float:
    """Max abs difference between PyTorch and onnxruntime on random images,
    at several batch sizes (proving the batch axis really is dynamic)."""
    import onnxruntime as ort

    model, classes = load_checkpoint(checkpoint)
    session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    rng = np.random.default_rng(seed)
    worst = 0.0
    for batch in (1, 3, 16):
        x = rng.standard_normal((batch, 1, NFFT, TARGET_FRAMES)).astype(np.float32)
        with torch.no_grad():
            expected = torch.softmax(model(torch.from_numpy(x)), dim=1).numpy()
        (actual,) = session.run([OUTPUT_NAME], {INPUT_NAME: x})
        if actual.shape != (batch, len(classes)):
            raise AssertionError(f"batch {batch}: output shape {actual.shape}")
        if not np.allclose(actual.sum(axis=1), 1.0, atol=1e-5):
            raise AssertionError(f"batch {batch}: probabilities do not sum to 1")
        worst = max(worst, float(np.abs(actual - expected).max()))
    if worst > PARITY_TOLERANCE:
        raise AssertionError(f"ONNX vs PyTorch max abs diff {worst:.3e} > {PARITY_TOLERANCE:.0e}")
    return worst


def main() -> int:
    parser = argparse.ArgumentParser(description="Export the modulation classifier to ONNX.")
    parser.add_argument("--checkpoint", type=Path, default=DEFAULT_CHECKPOINT)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument("--no-verify", action="store_true", help="skip the onnxruntime parity check")
    args = parser.parse_args()

    onnx_path, meta_path = export(args.checkpoint, args.out_dir)
    print(f"wrote {onnx_path} ({onnx_path.stat().st_size} bytes)")
    print(f"wrote {meta_path}")
    if not args.no_verify:
        worst = verify(args.checkpoint, onnx_path)
        print(f"verified against PyTorch: max abs diff {worst:.2e} (tolerance {PARITY_TOLERANCE:.0e})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
