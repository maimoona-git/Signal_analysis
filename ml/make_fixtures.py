"""Generates tests/data/ml/: signals plus the *expected* preprocessing output
and classifier probabilities, produced by the ORIGINAL native app code, so the
C++ port (core/src/ml_preprocess.cpp) and the ONNX model running in the browser
are checked against the original implementation, not against a re-derivation.

Dev-time only; needs the native project checked out (its `app`, `ml` packages
and built `sigcore`) plus torch:

    ~/.ml-venv/bin/python ml/make_fixtures.py --native-repo ~/signal-project/signal

Committed output is small (~150 KB) and deterministic (seeded).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native-repo", type=Path, default=Path.home() / "signal-project" / "signal")
    parser.add_argument("--out", type=Path, default=REPO / "tests" / "data" / "ml")
    parser.add_argument("--checkpoint", type=Path, default=None,
                        help="defaults to the native repo's ml/checkpoints/modulation_classifier.pt")
    args = parser.parse_args()

    # Put the native repo first so `app`, `ml` and `sigcore` are the originals
    # (this repo's own `ml` package would otherwise shadow the native one).
    sys.path.insert(0, str(args.native_repo))
    from app.classify import ModulationClassifier  # noqa: E402  (original inference wrapper)
    from ml.dataset import generate_example  # noqa: E402
    from ml.preprocessing import MODULATION_CLASSES, NFFT, TARGET_FRAMES, make_spectrogram_image  # noqa: E402

    checkpoint = args.checkpoint or (args.native_repo / "ml" / "checkpoints" / "modulation_classifier.pt")
    classifier = ModulationClassifier(checkpoint)
    args.out.mkdir(parents=True, exist_ok=True)

    cases: list[tuple[str, str | None, np.ndarray, float]] = []

    rng = np.random.default_rng(2026)
    for scheme in MODULATION_CLASSES:
        signal, rate = generate_example(scheme, 200, rng)
        cases.append((scheme, scheme, signal.astype(np.complex64), rate))

    # Branch coverage for the fixed-size image: pad (few frames), crop (many
    # frames), and the zero-variance branch that skips the z-score division.
    noise = (rng.standard_normal(4096) + 1j * rng.standard_normal(4096)).astype(np.complex64)
    cases += [
        ("edge_min_len_64", None, noise[:64], 8000.0),       # exactly one frame -> heavy edge padding
        ("edge_pad_100", None, noise[:100], 8000.0),          # 37 frames -> padded to 48
        ("edge_crop_157", None, noise[:157], 8000.0),         # 94 frames -> cropped to 48
        ("edge_long_20000", None, np.tile(noise, 5)[:20000], 8000.0),
        ("edge_zeros_512", None, np.zeros(512, dtype=np.complex64), 8000.0),  # all bins -240 dB: std == 0
    ]

    manifest = {"version": 1, "classes": list(classifier.classes), "nfft": NFFT, "frames": TARGET_FRAMES, "cases": []}
    for name, true_class, signal, rate in cases:
        image = make_spectrogram_image(signal, rate)             # (1, 64, 48) float32, original code
        assert image.shape == (1, NFFT, TARGET_FRAMES) and image.dtype == np.float32
        result = classifier.classify(signal, rate)                # original torch model on that image

        (args.out / f"{name}.iq").write_bytes(signal.astype("<c8").tobytes())
        (args.out / f"{name}.image.f32").write_bytes(image.astype("<f4").tobytes())
        manifest["cases"].append({
            "name": name,
            "true_class": true_class,
            "iq_file": f"{name}.iq",
            "image_file": f"{name}.image.f32",
            "sample_rate": rate,
            "num_samples": int(len(signal)),
            "label": result.label,
            "confidence": result.confidence,
            "probabilities": [result.probabilities[c] for c in classifier.classes],
        })
        print(f"{name:20s} n={len(signal):6d}  -> {result.label:6s} {result.confidence:.3f}")

    (args.out / "expected.json").write_text(json.dumps(manifest, indent=2) + "\n")
    # Same cases as plain text for the C++ tests (no JSON parser needed there):
    #   name iq_file image_file sample_rate num_samples
    (args.out / "cases.txt").write_text("".join(
        f"{c['name']} {c['iq_file']} {c['image_file']} {c['sample_rate']:.1f} {c['num_samples']}\n"
        for c in manifest["cases"]))
    print(f"wrote {len(cases)} cases to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
