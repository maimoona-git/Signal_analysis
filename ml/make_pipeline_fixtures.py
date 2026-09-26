"""Generates tests/data/pipeline/: inputs plus the outputs the ORIGINAL native
app produces for them, so the C++ port of the pipeline orchestration
(app/pipeline/) is validated by differential testing against the original, not
by inspection. Covers correlate.py, fec.py, interleave.py, detect.py and
pipeline.py (io.py and demod.py are exercised through the pipeline scenarios).

Also records the text the ORIGINAL GUI results panel shows for each successful
scenario (`panel_text_hex`, `status_hex`), by running the real PyQt6 ResultsPanel
offscreen, so the C++ text formatting (app/pipeline/src/result_text.cpp) is
checked byte for byte.

Dev-time only; needs the native project (its `app`, `ml` packages, built
`sigcore`), torch (the real classifier is run for the demo captures) and PyQt6,
so run it with the native project's own venv:

    ~/signal-project/signal/.venv/bin/python ml/make_pipeline_fixtures.py \
        --native-repo ~/signal-project/signal

Output is deterministic (seeded). Record format (read by
tests/pipeline_fixtures.hpp and tests/bridge/pipeline.test.mjs): records are
blocks of `key value` lines separated by a `---` line; bit arrays are strings
of 0/1 ("-" when empty); "none" marks None. Where the original raises, the
record has `error <ExceptionName>` and the C++ port must throw too.
"""

from __future__ import annotations

import argparse
import shutil
import struct
import sys
import wave
from dataclasses import dataclass
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent


# --- record writing ----------------------------------------------------------

def bstr(bits) -> str:
    bits = np.asarray(bits).astype(np.uint8)
    return "".join(str(int(b)) for b in bits) or "-"


def ostr(value) -> str:
    return "none" if value is None else str(value)


def params_str(params: dict | None) -> str:
    if not params:
        return "-"
    return ",".join(f"{k}={v}" for k, v in params.items())


def write_records(path: Path, records: list[dict]) -> None:
    lines: list[str] = []
    for rec in records:
        for key, value in rec.items():
            text = str(value)
            assert "\n" not in text, (key, text)
            lines.append(f"{key} {text}")
        lines.append("---")
    path.write_text("\n".join(lines) + "\n")


def framed_bits(num_frames=8, frame_len=32, header_len=16, seed=0):
    rng = np.random.default_rng(seed)
    header = rng.integers(0, 2, header_len).astype(np.uint8)
    frames = [np.concatenate([header, rng.integers(0, 2, frame_len - header_len).astype(np.uint8)])
              for _ in range(num_frames)]
    return np.concatenate(frames)


def flip(bits, positions):
    out = np.array(bits, dtype=np.uint8, copy=True)
    for p in positions:
        out[p % len(out)] ^= 1
    return out


# --- correlate.py -------------------------------------------------------------

def make_correlate(out: Path) -> int:
    from app import correlate as C

    recs: list[dict] = []
    rng = np.random.default_rng(11)

    def attempt(rec, fn):
        try:
            fn(rec)
        except Exception as e:  # noqa: BLE001  (recording the original's behaviour)
            rec["error"] = type(e).__name__
        recs.append(rec)

    streams = {
        "n2": rng.integers(0, 2, 2), "n3": rng.integers(0, 2, 3), "n9": rng.integers(0, 2, 9),
        "n33": rng.integers(0, 2, 33), "n128": rng.integers(0, 2, 128), "n300": rng.integers(0, 2, 300),
        "framed": framed_bits(10, 24, 8, 3), "framed_noisy": flip(framed_bits(10, 24, 8, 3), [5, 60, 130]),
        "zeros": np.zeros(64, np.uint8), "ones": np.ones(40, np.uint8), "alt": np.tile([0, 1], 40),
    }

    for name, bits in streams.items():
        bits = bits.astype(np.uint8)

        def autocorr(r, bits=bits):
            r["lags"] = ",".join(f"{v:.17g}" for v in C.autocorrelation(bits))
        attempt({"op": "autocorr", "name": name, "bits": bstr(bits)}, autocorr)

        for min_p, max_p in [(4, None), (2, None), (4, 20), (8, 9), (4, 4), (1, None), (3, 1000)]:
            def period(r, bits=bits, min_p=min_p, max_p=max_p):
                p, s = C.detect_frame_period(bits, min_period=min_p, max_period=max_p)
                r["period"], r["strength"] = p, f"{s:.17g}"
            attempt({"op": "frame_period", "name": name, "bits": bstr(bits), "min_period": min_p,
                     "max_period": ostr(max_p)}, period)

    fr = streams["framed"].astype(np.uint8)
    for name, pattern, errs in [("hdr0", fr[:8], 0), ("hdr1", fr[:8], 1), ("hdr2", fr[:8], 2), ("hdr3", fr[:8], 3),
                                ("long", fr[:100], 0), ("toolong", np.tile(fr, 2), 0), ("one", fr[:1], 0),
                                ("empty", np.array([], np.uint8), 0), ("allzero", np.zeros(4, np.uint8), 1)]:
        def find(r, pattern=pattern, errs=errs):
            r["positions"] = ",".join(map(str, C.find_pattern_occurrences(fr, pattern, max_bit_errors=errs))) or "-"
        attempt({"op": "find", "name": name, "bits": bstr(fr), "pattern": bstr(pattern), "max_errors": errs}, find)

    texts = [b"SIH2026!", b"line1\nline2\ttab\r", b"\x00\x01binary", b"caf\xc3\xa9", b"~ }{", b"A", b""]
    for i, raw in enumerate(texts):
        bits = np.unpackbits(np.frombuffer(raw, np.uint8)) if raw else np.array([], np.uint8)
        for extra in (0, 3):  # a ragged tail must be dropped
            b = np.concatenate([bits, np.ones(extra, np.uint8)])
            attempt({"op": "text", "name": f"text{i}_{extra}", "bits": bstr(b),
                     "expected_hex": C.bits_to_text(b).encode("utf-8").hex() or "-"}, lambda r: None)

    boundary_cases = []
    for k, (frames, flen, hlen, seed) in enumerate([(8, 32, 16, 0), (6, 20, 8, 1), (12, 17, 8, 2), (4, 64, 16, 3)]):
        b = framed_bits(frames, flen, hlen, seed)
        boundary_cases += [(f"framed{k}", b, None, 16, 1, 4), (f"framed{k}_exact", b, None, 16, 0, 4),
                           (f"framed{k}_given", b, b[:hlen], 16, 1, 4),
                           (f"framed{k}_noisy", flip(b, [3, 40, 41]), None, 16, 1, 4)]
    boundary_cases += [
        ("random", rng.integers(0, 2, 200).astype(np.uint8), None, 16, 1, 4),
        ("short12", rng.integers(0, 2, 12).astype(np.uint8), None, 16, 1, 4),
        ("short9", rng.integers(0, 2, 9).astype(np.uint8), None, 16, 1, 4),
        ("short8", rng.integers(0, 2, 8).astype(np.uint8), None, 16, 1, 4),
        ("short2", np.array([1, 0], np.uint8), None, 16, 1, 4),
        ("one", np.array([1], np.uint8), None, 16, 1, 4),
        ("empty", np.array([], np.uint8), None, 16, 1, 4),
        ("zeros", np.zeros(50, np.uint8), None, 16, 1, 4),
        ("alt_hdr8", np.tile([0, 1], 50).astype(np.uint8), None, 8, 0, 4),
        ("period_tie", np.tile(np.concatenate([[1, 1, 0, 1], np.zeros(4)]), 3).astype(np.uint8), None, 4, 0, 4),
        ("given_longer", np.array([1, 0, 1, 1], np.uint8), np.array([1, 0, 1, 1, 0, 0, 1, 1], np.uint8), 16, 1, 4),
        ("minp8", framed_bits(6, 20, 8, 1), None, 16, 1, 8),
    ]
    for name, bits, pattern, hlen, errs, minp in boundary_cases:
        def bounds(r, bits=bits, pattern=pattern, hlen=hlen, errs=errs, minp=minp):
            fb = C.find_header_payload_boundaries(bits, header_pattern=pattern, header_len=hlen,
                                                  max_bit_errors=errs, min_period=minp)
            r["out_pattern"] = bstr(fb.header_pattern)
            r["frame_period"] = ostr(fb.frame_period)
            r["positions"] = ",".join(map(str, fb.header_positions)) or "-"
            r["payloads"] = ",".join(bstr(p) for p in fb.payloads) or "-"
        attempt({"op": "boundaries", "name": name, "bits": bstr(bits), "pattern": ostr(None if pattern is None else bstr(pattern)),
                 "header_len": hlen, "max_errors": errs, "min_period": minp}, bounds)

    write_records(out / "correlate.rec", recs)
    return len(recs)


# --- fec.py / interleave.py ---------------------------------------------------

def make_fec(out: Path) -> int:
    from app import fec as F

    recs: list[dict] = []
    rng = np.random.default_rng(21)

    def attempt(rec, fn):
        try:
            fn(rec)
        except Exception as e:  # noqa: BLE001
            rec["error"] = type(e).__name__
        recs.append(rec)

    for scheme in ["viterbi", "ldpc", "rs", "concatenated", "VITERBI", "Rs"]:
        for n in [1, 7, 8, 13, 16, 40, 64, 100, 104, 200]:
            bits = rng.integers(0, 2, n).astype(np.uint8)

            def enc(r, scheme=scheme, bits=bits):
                r["out"] = bstr(F.encode(bits, scheme))
            attempt({"op": "encode", "scheme": scheme, "bits": bstr(bits)}, enc)

            try:
                coded = F.encode(bits, scheme)
            except Exception:  # noqa: BLE001
                continue
            unit = 8 if scheme.lower() in ("rs", "concatenated") else 1
            msg_len = (n + 7) // 8 if unit == 8 else n
            for label, received in [("clean", coded), ("1err", flip(coded, [len(coded) // 3])),
                                    ("3err", flip(coded, [2, len(coded) // 2, len(coded) - 3]))]:
                def dec(r, scheme=scheme, received=received, msg_len=msg_len):
                    r["out"] = bstr(F.decode(received, scheme, msg_len))
                attempt({"op": "decode", "scheme": scheme, "bits": bstr(received), "msg_len": msg_len,
                         "case": label}, dec)

    # invalid inputs
    for scheme, n, msg_len in [("viterbi", 15, 5), ("viterbi", 0, 1), ("ldpc", 15, 8), ("rs", 100, 13),
                               ("rs", 120, 500), ("concatenated", 30, 2), ("ldpc", 32, 100), ("viterbi", 40, 500)]:
        bits = rng.integers(0, 2, n).astype(np.uint8)
        attempt({"op": "decode", "scheme": scheme, "bits": bstr(bits), "msg_len": msg_len, "case": "invalid"},
                lambda r, scheme=scheme, bits=bits, msg_len=msg_len: r.__setitem__("out", bstr(F.decode(bits, scheme, msg_len))))
    attempt({"op": "encode", "scheme": "turbo", "bits": "1010"}, lambda r: r.__setitem__("out", bstr(F.encode(np.array([1, 0, 1, 0]), "turbo"))))
    attempt({"op": "decode", "scheme": "turbo", "bits": "1010", "msg_len": 4, "case": "unknown"},
            lambda r: r.__setitem__("out", bstr(F.decode(np.array([1, 0, 1, 0]), "turbo", 4))))
    attempt({"op": "encode", "scheme": "ldpc", "bits": "-"}, lambda r: r.__setitem__("out", bstr(F.encode(np.array([], np.uint8), "ldpc"))))

    write_records(out / "fec.rec", recs)
    return len(recs)


def make_interleave(out: Path) -> int:
    from app import interleave as I

    recs: list[dict] = []
    rng = np.random.default_rng(31)

    def attempt(rec, fn):
        try:
            fn(rec)
        except Exception as e:  # noqa: BLE001
            rec["error"] = type(e).__name__
        recs.append(rec)

    configs = [("block", {"rows": 4, "cols": 6}), ("block", {"rows": 8, "cols": 16}), ("block", {"rows": 1, "cols": 9}),
               ("diagonal", {"rows": 5, "cols": 6}), ("diagonal", {"rows": 8, "cols": 16}), ("Diagonal", {"rows": 3, "cols": 3}),
               ("pseudo_random", {"seed": 0}), ("pseudo_random", {"seed": 1234}), ("pseudo_random", {"seed": 4294967295}),
               ("convolutional", {"branches": 4, "delay_increment": 2}), ("convolutional", {"branches": 3, "delay_increment": 1}),
               ("convolutional", {"branches": 1, "delay_increment": 5}), ("BLOCK", {"rows": 2, "cols": 2})]
    for kind, params in configs:
        n = params["rows"] * params["cols"] if "rows" in params else 96
        bits = rng.integers(0, 2, n).astype(np.uint8)
        for op, fn in (("interleave", I.interleave), ("deinterleave", I.deinterleave)):
            attempt({"op": op, "kind": kind, "params": params_str(params), "bits": bstr(bits)},
                    lambda r, fn=fn, kind=kind, params=params, bits=bits: r.__setitem__("out", bstr(fn(bits, kind, **params))))
    bad = [("block", {"rows": 4, "cols": 4}, 10), ("block", {"rows": 0, "cols": 4}, 0), ("block", {"cols": 4}, 8),
           ("diagonal", {"rows": 4}, 8), ("pseudo_random", {}, 8), ("convolutional", {"branches": 3}, 12),
           ("convolutional", {"branches": 0, "delay_increment": 1}, 12), ("zigzag", {"rows": 2, "cols": 2}, 4),
           ("pseudo_random", {"seed": 3}, 0)]
    for kind, params, n in bad:
        bits = rng.integers(0, 2, n).astype(np.uint8)
        for op, fn in (("interleave", I.interleave), ("deinterleave", I.deinterleave)):
            attempt({"op": op, "kind": kind, "params": params_str(params), "bits": bstr(bits)},
                    lambda r, fn=fn, kind=kind, params=params, bits=bits: r.__setitem__("out", bstr(fn(bits, kind, **params))))
    write_records(out / "interleave.rec", recs)
    return len(recs)


# --- detect.py ----------------------------------------------------------------

def make_detect(out: Path) -> int:
    from app import detect as D
    from app.fec import encode as fec_encode
    from app.interleave import interleave

    recs: list[dict] = []
    rng = np.random.default_rng(41)

    def record(name, bits, header_len=None, score_floor=3, seed_range=32):
        rec = {"name": name, "bits": bstr(bits), "header_len": ostr(header_len), "score_floor": score_floor,
               "seed_range": seed_range}
        try:
            r = D.detect_params(bits, header_len=header_len, score_floor=score_floor, pseudo_random_seed_range=seed_range)
            rec["interleaver"] = ostr(r.interleaver)
            rec["iparams"] = params_str(r.interleaver_params)
            rec["fec"] = ostr(r.fec)
            rec["msg_len"] = ostr(r.fec_msg_len)
            rec["score"] = r.score
            rec["deinterleaved"] = "none" if r.deinterleaved_bits is None else bstr(r.deinterleaved_bits)
            rec["decoded"] = "none" if r.decoded_bits is None else bstr(r.decoded_bits)
            rec["working"] = bstr(r.working_bits)
        except Exception as e:  # noqa: BLE001
            rec["error"] = type(e).__name__
        recs.append(rec)

    def pick_pair(n, which):
        pairs = D._divisor_pairs(n)
        return pairs[min(len(pairs) - 1, which)] if pairs else None

    payload = framed_bits(num_frames=6, frame_len=64, header_len=16, seed=5)

    # the full transmit-chain grid: every FEC x every interleaver kind
    for fec in [None, "viterbi", "ldpc", "rs", "concatenated"]:
        coded = payload if fec is None else fec_encode(payload, fec)
        n = len(coded)
        chains = [("plain", None, {})]
        pair = pick_pair(n, len(D._divisor_pairs(n)) // 2)
        if pair:
            chains += [("block", "block", {"rows": pair[0], "cols": pair[1]}),
                       ("diagonal", "diagonal", {"rows": pair[1], "cols": pair[0]})]
        chains += [("conv32", "convolutional", {"branches": 3, "delay_increment": 2}),
                   ("conv41", "convolutional", {"branches": 4, "delay_increment": 1}),
                   ("pr7", "pseudo_random", {"seed": 7}), ("pr31", "pseudo_random", {"seed": 31}),
                   ("pr40_outside_range", "pseudo_random", {"seed": 40})]
        for cname, kind, params in chains:
            tx = coded if kind is None else interleave(coded, kind, **params)
            record(f"grid_{fec}_{cname}", tx)

    # the native app's own test scenarios
    for kind, params in [(None, {}), ("block", {"rows": 8, "cols": 32}), ("diagonal", {"rows": 32, "cols": 8}),
                         ("convolutional", {"branches": 3, "delay_increment": 2})]:
        b = framed_bits(8, 32)
        record(f"native_nofec_{kind}", b if kind is None else interleave(b, kind, **params))
    for fec in ["viterbi", "ldpc", "rs", "concatenated"]:
        record(f"native_fec_{fec}", fec_encode(framed_bits(6, 64), fec))
    record("native_block_ldpc", interleave(fec_encode(framed_bits(6, 64), "ldpc"), "block", rows=16, cols=48))
    record("native_noise256", np.random.default_rng(42).integers(0, 2, 256).astype(np.uint8))
    record("native_empty", np.array([], np.uint8))

    # noisy channels: a few bit errors after transmission
    for fec in ["viterbi", "concatenated", "ldpc", "rs"]:
        tx = fec_encode(payload, fec)
        for nerr in (1, 3):
            record(f"noisy_{fec}_{nerr}err", flip(tx, [11, len(tx) // 2, len(tx) - 7][:nerr]))
    record("noisy_plain_2err", flip(payload, [100, 200]))

    # size / degenerate edge cases
    for n in [1, 2, 3, 8, 12, 16, 20, 47, 64, 97, 251]:
        record(f"size_{n}", rng.integers(0, 2, n).astype(np.uint8))
    record("zeros_128", np.zeros(128, np.uint8))
    record("ones_128", np.ones(128, np.uint8))
    record("alternating_128", np.tile([0, 1], 64).astype(np.uint8))
    record("period4_128", np.tile([1, 1, 0, 1], 32).astype(np.uint8))

    # parameters
    b = framed_bits(8, 32)
    for hl in (4, 8, 16, 64, 1000):
        record(f"header_len_{hl}", b, header_len=hl)
    for floor in (0, 1, 8, 100):
        record(f"floor_{floor}", b, score_floor=floor)
    for sr in (0, 1, 8, 64):
        record(f"seed_range_{sr}", interleave(framed_bits(6, 64), "pseudo_random", seed=5), seed_range=sr)
    record("other_frame_shapes", framed_bits(12, 20, 8, 9))
    record("other_frame_shapes2", interleave(framed_bits(10, 40, 12, 4), "block", rows=20, cols=20))

    write_records(out / "detect.rec", recs)
    return len(recs)


# --- pipeline.py --------------------------------------------------------------

@dataclass
class _Cls:
    label: str
    confidence: float
    probabilities: dict


class _Fake:
    def __init__(self, label, confidence=1.0):
        self.label, self.confidence = label, confidence

    def classify(self, samples, sample_rate):
        return _Cls(self.label, self.confidence, {self.label: self.confidence})


def write_iq(path: Path, samples) -> None:
    samples = np.asarray(samples, np.complex64)
    inter = np.empty(2 * len(samples), np.float32)
    inter[0::2], inter[1::2] = samples.real, samples.imag
    path.write_bytes(inter.astype("<f4").tobytes())


def add_awgn(sig, snr_db, seed):
    rng = np.random.default_rng(seed)
    p = np.mean(np.abs(sig) ** 2)
    std = np.sqrt(p / (10 ** (snr_db / 10)) / 2)
    return (sig + std * (rng.standard_normal(len(sig)) + 1j * rng.standard_normal(len(sig)))).astype(np.complex64)


def make_pipeline(out: Path, native: Path) -> int:
    import os
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PyQt6.QtWidgets import QApplication
    from app.gui.results_panel import ResultsPanel
    qt_app = QApplication.instance() or QApplication([])
    panel = ResultsPanel()
    from app.classify import ModulationClassifier
    from app.demod import modulate
    from app.fec import encode as fec_encode
    from app.interleave import interleave
    from app.pipeline import run_pipeline

    real = ModulationClassifier(native / "ml" / "checkpoints" / "modulation_classifier.pt")
    recs: list[dict] = []
    rng = np.random.default_rng(51)

    # Demo captures shipped with the native app (real files, real classifier).
    for name in ("bpsk_ldpc_block.iq", "fsk_plain.iq"):
        shutil.copy(native / "data" / "fixtures" / "demo" / name, out / name)

    def scenario(name, file, classifier, label_kind, sample_rate=None, sps=1, demod=None, interleaver=None,
                 iparams=None, fec=None, msg_len=None, header=None, auto=False):
        path = out / file
        rec = {"name": name, "file": file, "classifier": label_kind,
               "sample_rate": ostr(sample_rate), "samples_per_symbol": sps,
               "demod": params_str(demod), "interleaver": ostr(interleaver), "iparams": params_str(iparams),
               "fec": ostr(fec), "msg_len": ostr(msg_len), "header": ostr(None if header is None else bstr(header)),
               "auto_detect": int(auto)}
        try:
            r = run_pipeline(str(path), sample_rate=sample_rate, samples_per_symbol=sps, demod_params=demod,
                             interleaver=interleaver, interleaver_params=iparams, fec_scheme=fec, fec_msg_len=msg_len,
                             header_pattern=header, classifier=classifier, auto_detect=auto)
            rec["label"] = r.predicted_modulation
            rec["confidence"] = f"{r.predicted_confidence:.17g}"
            rec["out_sample_rate"] = f"{r.sample_rate:.17g}"
            rec["num_samples"] = r.num_samples
            rec["file_format"] = r.file_format
            sp = r.spectrogram
            rec["spec_frames"], rec["spec_nfft"] = sp.data.shape
            rec["spec_mean"] = f"{float(np.mean(sp.data)):.9g}"
            rec["freq_first"], rec["freq_last"] = f"{sp.freqs[0]:.17g}", f"{sp.freqs[-1]:.17g}"
            rec["time_last"] = f"{sp.times[-1]:.17g}"
            rec["demodulated"] = bstr(r.demodulated_bits)
            rec["deinterleaved"] = "none" if r.deinterleaved_bits is None else bstr(r.deinterleaved_bits)
            rec["decoded"] = "none" if r.decoded_bits is None else bstr(r.decoded_bits)
            rec["detected_interleaver"] = ostr(r.detected_interleaver)
            rec["detected_fec"] = ostr(r.detected_fec)
            rec["detection_score"] = ostr(r.detection_score)
            fb = r.boundaries
            rec["b_pattern"] = bstr(fb.header_pattern)
            rec["b_period"] = ostr(fb.frame_period)
            rec["b_positions"] = ",".join(map(str, fb.header_positions)) or "-"
            rec["b_payloads"] = ",".join(bstr(p) for p in fb.payloads) or "-"
            # what the original GUI shows (the browser has a filename, not a path)
            panel.show_result(r)
            panel_text = panel.toPlainText().replace(str(path), file)
            status = f"Done. Predicted modulation: {r.predicted_modulation} ({r.predicted_confidence:.0%} confidence)."
            rec["panel_text_hex"] = panel_text.encode("utf-8").hex()
            rec["status_hex"] = status.encode("utf-8").hex()
        except Exception as e:  # noqa: BLE001
            rec["error"] = type(e).__name__
            if label_kind != "real":
                rec["label"] = classifier.label
        recs.append(rec)

    # --- the two demo captures, with the real classifier and known parameters
    scenario("demo_ldpc_block_real", "bpsk_ldpc_block.iq", real, "real", 8000.0, 4, None, "block",
             {"rows": 8, "cols": 16}, "ldpc", 64)
    scenario("demo_fsk_plain_real", "fsk_plain.iq", real, "real", 8000.0)
    scenario("demo_ldpc_block_bpsk", "bpsk_ldpc_block.iq", _Fake("bpsk"), "fake", 8000.0, 4, None, "block",
             {"rows": 8, "cols": 16}, "ldpc", 64)
    scenario("demo_ldpc_block_autodetect", "bpsk_ldpc_block.iq", _Fake("bpsk"), "fake", 8000.0, 4, auto=True)
    scenario("demo_fsk_fake", "fsk_plain.iq", _Fake("fsk"), "fake", 8000.0)
    scenario("demo_default_sample_rate", "fsk_plain.iq", _Fake("fsk"), "fake")  # -> 1 MHz default
    scenario("demo_ldpc_wrong_label", "bpsk_ldpc_block.iq", _Fake("qpsk"), "fake", 8000.0, 4, None, "block",
             {"rows": 8, "cols": 16}, "ldpc", 64)

    # --- every modulation the classifier can output, no FEC/interleaving
    msg = rng.integers(0, 2, 96).astype(np.uint8)
    for scheme in ["bpsk", "qpsk", "psk8", "qam16", "qam64"]:
        write_iq(out / f"mod_{scheme}.iq", np.repeat(modulate(msg, scheme), 2))
        scenario(f"mod_{scheme}", f"mod_{scheme}.iq", _Fake(scheme), "fake", 4000.0, 2)
    write_iq(out / "mod_fsk2.iq", modulate(msg, "fsk", bits_per_symbol=2, samples_per_symbol=8, bandwidth=0.3))
    scenario("mod_fsk_bps2", "mod_fsk2.iq", _Fake("fsk"), "fake", 8000.0, 1, {"bits_per_symbol": 2, "samples_per_symbol": 8, "bandwidth": 0.3})
    scenario("mod_fsk_wrong_params", "mod_fsk2.iq", _Fake("fsk"), "fake", 8000.0, 1, {"bits_per_symbol": 1, "samples_per_symbol": 8, "bandwidth": 0.25})
    write_iq(out / "mod_bpsk_noisy.iq", add_awgn(np.repeat(modulate(msg, "bpsk"), 4), 6.0, 7))
    scenario("mod_bpsk_noisy", "mod_bpsk_noisy.iq", _Fake("bpsk"), "fake", 8000.0, 4)
    scenario("mod_bpsk_sps_ignored_for_fsk", "mod_fsk2.iq", _Fake("fsk"), "fake", 8000.0, 4, {"bits_per_symbol": 2, "samples_per_symbol": 8, "bandwidth": 0.3})

    # --- WAV input (sample rate comes from the header, the argument is ignored)
    bits = np.repeat(2.0 * rng.integers(0, 2, 200) - 1, 8) * 0.5
    with wave.open(str(out / "real_bpsk.wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(11025)
        w.writeframes((bits * 32767).astype("<i2").tobytes())
    scenario("wav_mono", "real_bpsk.wav", _Fake("bpsk"), "fake", None, 8)
    scenario("wav_ignores_sample_rate_arg", "real_bpsk.wav", _Fake("bpsk"), "fake", 999.0, 8)

    # --- full chains with parameters supplied
    payload = framed_bits(6, 64, 16, 8)
    def chain(name, fec, kind, params, label="bpsk", auto=False, given=True, header=None, noise=None):
        coded = payload if fec is None else fec_encode(payload, fec)
        tx = coded if kind is None else interleave(coded, kind, **params)
        sig = np.repeat(modulate(tx, label), 2).astype(np.complex64) if label != "fsk" else modulate(tx, "fsk")
        if noise:
            sig = add_awgn(sig, *noise)
        write_iq(out / f"{name}.iq", sig)
        msg_len = None
        if fec in ("rs", "concatenated"):
            msg_len = (len(payload) + 7) // 8
        elif fec in ("viterbi", "ldpc"):
            msg_len = len(payload)
        scenario(name, f"{name}.iq", _Fake(label), "fake", 8000.0, 2 if label != "fsk" else 1,
                 None, kind if given and not auto else None, params if given and not auto else None,
                 fec if given and not auto else None, msg_len if given and not auto else None, header, auto)

    for fec in ["viterbi", "ldpc", "rs", "concatenated", None]:
        for kind, params in [("block", {"rows": 16, "cols": 24}), ("diagonal", {"rows": 24, "cols": 16}),
                             ("pseudo_random", {"seed": 5}), ("convolutional", {"branches": 3, "delay_increment": 2}), (None, None)]:
            coded_len = len(payload) if fec is None else len(fec_encode(payload, fec))
            if kind in ("block", "diagonal") and params["rows"] * params["cols"] != coded_len:
                pair = [p for p in _pairs(coded_len)]
                if not pair:
                    continue
                params = {"rows": pair[0][0], "cols": pair[0][1]}
            tag = f"chain_{fec}_{kind}"
            chain(tag, fec, kind, params)
            chain(tag + "_auto", fec, kind, params, auto=True)

    chain("chain_qpsk_ldpc_block", "ldpc", "block", {"rows": 24, "cols": 32}, label="qpsk")
    chain("chain_noisy_ldpc", "ldpc", "block", {"rows": 24, "cols": 32}, noise=(9.0, 3))
    chain("chain_noisy_viterbi_auto", "viterbi", None, None, noise=(8.0, 4), auto=True)
    chain("chain_given_header", None, None, None, header=payload[:16])

    # --- streams too short to have a frame period (the original prints `None`):
    # with 8 bits there is one header occurrence and not enough length for autocorrelation
    # (needs > 8); with 9 bits autocorrelation is attempted but its period range is empty
    for nbits in (8, 9):
        write_iq(out / f"few_bits_{nbits}.iq", modulate(rng.integers(0, 2, nbits).astype(np.uint8), "bpsk"))
        scenario(f"few_bits_{nbits}", f"few_bits_{nbits}.iq", _Fake("bpsk"), "fake", 1000.0)

    # --- errors the original raises
    scenario("err_fec_without_msg_len", "chain_ldpc_None.iq", _Fake("bpsk"), "fake", 8000.0, 2, None, None, None, "ldpc", None)
    scenario("err_unknown_interleaver", "chain_ldpc_None.iq", _Fake("bpsk"), "fake", 8000.0, 2, None, "zigzag", {}, None, None)
    scenario("err_wrong_interleave_dims", "chain_ldpc_None.iq", _Fake("bpsk"), "fake", 8000.0, 2, None, "block", {"rows": 3, "cols": 5})
    scenario("err_unknown_label", "mod_bpsk.iq", _Fake("morse"), "fake", 4000.0, 2)
    scenario("err_uncorrectable_rs", "mod_bpsk.iq", _Fake("bpsk"), "fake", 4000.0, 2, None, None, None, "rs", 5)
    (out / "bad_extension.bin").write_bytes(b"\x00" * 64)
    scenario("err_unsupported_extension", "bad_extension.bin", _Fake("bpsk"), "fake", 8000.0)
    write_iq(out / "tiny.iq", np.ones(10, np.complex64))
    scenario("short_capture_10_samples", "tiny.iq", _Fake("bpsk"), "fake", 8000.0)
    (out / "odd_size.iq").write_bytes(b"\x00" * 13)
    scenario("err_misaligned_iq", "odd_size.iq", _Fake("bpsk"), "fake", 8000.0)
    scenario("err_negative_sample_rate", "mod_bpsk.iq", _Fake("bpsk"), "fake", -5.0, 2)
    (out / "empty.iq").write_bytes(b"")
    scenario("err_empty_capture", "empty.iq", _Fake("bpsk"), "fake", 8000.0)

    write_records(out / "pipeline.rec", recs)
    return len(recs)


def _pairs(n):
    from app import detect as D
    pairs = D._divisor_pairs(n)
    return [pairs[len(pairs) // 2]] if pairs else []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native-repo", type=Path, default=Path.home() / "signal-project" / "signal")
    parser.add_argument("--out", type=Path, default=REPO / "tests" / "data" / "pipeline")
    args = parser.parse_args()
    sys.path.insert(0, str(args.native_repo))
    args.out.mkdir(parents=True, exist_ok=True)

    print("correlate  ", make_correlate(args.out), "records")
    print("fec        ", make_fec(args.out), "records")
    print("interleave ", make_interleave(args.out), "records")
    print("detect     ", make_detect(args.out), "records")
    print("pipeline   ", make_pipeline(args.out, args.native_repo), "records")
    return 0


if __name__ == "__main__":
    sys.exit(main())
