"""Shared spectrogram-to-tensor preprocessing, used identically by dataset
generation (training) and inference, to avoid train/inference skew.

Reference implementation for the C++ port in core/src/ml_preprocess.cpp (the
browser has no Python); tests/data/ml/ fixtures come from this logic. Needs the
native `sigcore` module on PYTHONPATH (build/core after a native build)."""

from __future__ import annotations

import numpy as np

import sigcore

NFFT = 64
TARGET_FRAMES = 48

MODULATION_CLASSES = ["bpsk", "qpsk", "psk8", "qam16", "qam64", "fsk"]


def make_spectrogram_image(samples: np.ndarray, sample_rate: float) -> np.ndarray:
    """Computes a fixed-size (1, NFFT, TARGET_FRAMES) normalized spectrogram
    image suitable as CNN input, from a variable-length complex signal.
    """
    n = len(samples)
    hop = max(1, (n - NFFT) // max(1, TARGET_FRAMES - 1))
    data = sigcore.compute_spectrogram(samples.astype(np.complex64), NFFT, hop)

    image = data.T  # (NFFT freq bins, num_frames)
    num_frames = image.shape[1]

    if num_frames >= TARGET_FRAMES:
        image = image[:, :TARGET_FRAMES]
    else:
        pad = TARGET_FRAMES - num_frames
        image = np.pad(image, ((0, 0), (0, pad)), mode="edge")

    mean = image.mean()
    std = image.std()
    if std > 1e-6:
        image = (image - mean) / std
    else:
        image = image - mean

    return image[np.newaxis, :, :].astype(np.float32)
