#!/usr/bin/env python3
"""Experiment: quantize w600k_r50.onnx to INT8 and measure speed + accuracy.

Pipeline fidelity: reuses calibrate.py (bit-exact port of the C++ auth
pipeline) to build the real 112x112 chips that production embeds, so the
benchmark timing and the accuracy comparison share the production input
distribution.

Usage:
  python quantize_r50.py               # static QDQ INT8 (default; dynamic quantize
                                       # produces ConvInteger(10) which onnxruntime's
                                       # CPU EP does not implement -> load fails)
  python quantize_r50.py --dynamic     # try the (broken) dynamic path
  python quantize_r50.py --no-bench    # quantize only, skip timing

After quantization, compare accuracy with the calibration tool:
  python calibrate.py --images data/lfw_subset --recognizer <output>.onnx

Decision rule (same as the mbf rejection): the same-angle p50 must keep a
real margin below the 0.80 cutoff (r50 fp32 p50 = 0.665 on the me/ batch;
mbf p50 = 0.792 was rejected for hugging the threshold). Measured result of
the static QDQ model: same-angle p50 0.706 (+0.041, margin 0.094) — accepted.

NOTE: r50's opset is 11, so per-channel QDQ is unavailable (needs opset 13);
the current build is per-tensor. Per-channel + opset bump is a possible
follow-up if more accuracy margin is ever needed.
"""

from __future__ import annotations

import argparse
import statistics
import sys
import time
from pathlib import Path

import cv2
import numpy as np
import onnx
import onnxruntime as ort
from onnxruntime.quantization import CalibrationDataReader, QuantFormat, QuantType
from onnxruntime.quantization import quantize_dynamic, quantize_static

sys.path.insert(0, str(Path(__file__).parent))
import calibrate  # noqa: E402  — bit-exact pipeline port

DEFAULT_MODELS_DIR = calibrate.DEFAULT_MODELS_DIR
DEFAULT_IMAGES = Path(__file__).parent / "data" / "lfw_subset"

# Match production's ONNX intra-op thread count (OnnxThreadCount() cap = 8)
# so the speed ratio measured here transfers to the service.
INTRA_OP_THREADS = 8
BENCH_REPEATS = 15
BENCH_WARMUP = 3


def make_chips(images_root: Path, per_identity: int) -> list[np.ndarray]:
    """Build 112x112 BGR chips through the production detection+alignment.

    Uses the same SCRFD + Umeyama + warp path as calibrate.load_gallery, but
    stops at the chip (no embedding). One chip per image, first
    `per_identity` images per identity (deterministic, sorted).
    """
    detector = calibrate.ScrfdDetector(DEFAULT_MODELS_DIR / "det_10g_gnkps.onnx")
    chips: list[np.ndarray] = []
    exts = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
    for person_dir in sorted(p for p in images_root.iterdir() if p.is_dir()):
        n = 0
        for img_path in sorted(person_dir.iterdir()):
            if img_path.suffix.lower() not in exts:
                continue
            bgr = calibrate.load_image(img_path)
            if bgr is None:
                continue
            det = detector.detect_largest(bgr)
            if det is None:
                continue
            chip = calibrate.align_face(bgr, det.kps)
            if chip is None:
                continue
            chips.append(chip)
            n += 1
            if n >= per_identity:
                break
    return chips


def preprocess(chip_bgr: np.ndarray) -> np.ndarray:
    """calibrate.Recognizer.embed preprocessing, up to (but not including)
    the ONNX call — RGB planar (pixel / 127.5) - 1.0, NCHW float32."""
    rgb = chip_bgr[:, :, ::-1].astype(np.float32)
    tensor = (rgb / calibrate.RECOG_NORM_DIVISOR - 1.0).transpose(2, 0, 1)
    return tensor.reshape(1, 3, calibrate.RECOG_INPUT_SIZE, calibrate.RECOG_INPUT_SIZE)


class _CalibReader(CalibrationDataReader):
    """Feeds the static-quantization calibrator real production chips."""

    def __init__(self, tensors: list[np.ndarray], input_name: str) -> None:
        self._queue = [preprocess(t) for t in tensors]
        self._input_name = input_name

    def get_next(self) -> dict[str, np.ndarray] | None:
        if not self._queue:
            return None
        return {self._input_name: self._queue.pop(0)}


def bench(model_path: Path, chips: list[np.ndarray]) -> tuple[float, float, float]:
    """Median wall-clock per embedding, FP32 baseline vs this model."""
    opts = ort.SessionOptions()
    opts.intra_op_num_threads = INTRA_OP_THREADS
    sess = ort.InferenceSession(str(model_path), sess_options=opts,
                                providers=["CPUExecutionProvider"])
    inp = sess.get_inputs()[0].name
    tensor = preprocess(chips[0])

    times: list[float] = []
    for i in range(BENCH_WARMUP + BENCH_REPEATS):
        t0 = time.perf_counter()
        out = sess.run(None, {inp: tensor})[0]
        times.append(time.perf_counter() - t0)
        if i == 0 and not np.all(np.isfinite(out)):
            raise RuntimeError(f"{model_path.name}: embedding contains NaN/Inf")
    steady = times[BENCH_WARMUP:]
    return statistics.median(steady), float(np.asarray(out[0]).std()), float(times[0])


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Quantize w600k_r50 to INT8 and measure the trade-off.",
    )
    parser.add_argument(
        "--recognizer", type=Path,
        default=DEFAULT_MODELS_DIR / "w600k_r50.onnx",
        help="FP32 r50 model to quantize",
    )
    parser.add_argument(
        "--output", type=Path, default=None,
        help="Output path (default: <recognizer>_int8.onnx next to input)",
    )
    parser.add_argument(
        "--dynamic", action="store_true",
        help="Use the dynamic-quantize path (generates ConvInteger; CPU EP "
             "cannot execute it, kept for reference)",
    )
    parser.add_argument(
        "--images", type=Path, default=DEFAULT_IMAGES,
        help="Gallery root for calibration chips (static mode) and bench input",
    )
    parser.add_argument(
        "--no-bench", action="store_true",
        help="Quantize only, skip the timing benchmark",
    )
    args = parser.parse_args()

    if not args.recognizer.is_file():
        parser.error(f"recognizer model not found: {args.recognizer}")
    out = args.output or args.recognizer.with_name(
        f"{args.recognizer.stem}_int8{args.recognizer.suffix}"
    )

    print(f"[1/4] Building real chips from {args.images} ...", file=sys.stderr)
    chips = make_chips(args.images, per_identity=2)
    print(f"      {len(chips)} chips", file=sys.stderr)
    if not chips:
        parser.error(f"no chips could be built from {args.images}")

    if not args.no_bench:
        fp32_ms, _, _ = bench(args.recognizer, chips)
        print(f"[2/4] FP32 baseline: {fp32_ms * 1000:.1f} ms/embed "
              f"({INTRA_OP_THREADS} threads)", file=sys.stderr)

    print(f"[3/4] Quantizing -> {out} ...", file=sys.stderr)
    if args.dynamic:
        quantize_dynamic(
            str(args.recognizer), str(out),
            weight_type=QuantType.QInt8,
        )
    else:
        sess = ort.InferenceSession(str(args.recognizer))
        input_name = sess.get_inputs()[0].name
        calib = _CalibReader(chips, input_name)
        # per-channel needs opset 13; r50 is opset 11 -> per-tensor.
        quantize_static(
            str(args.recognizer), str(out), calib,
            quant_format=QuantFormat.QDQ,
            per_channel=False,
        )

    mb_in = args.recognizer.stat().st_size / 1e6
    mb_out = out.stat().st_size / 1e6
    print(f"      {mb_in:.0f} MB -> {mb_out:.0f} MB", file=sys.stderr)

    if not args.no_bench:
        int8_ms, _, _ = bench(out, chips)
        print(f"[4/4] INT8: {int8_ms * 1000:.1f} ms/embed "
              f"(speedup {fp32_ms / int8_ms:.2f}x)", file=sys.stderr)
        print(f"      (dynamic-mode check above is on {out.name})", file=sys.stderr)

    print(f"\nNext: python calibrate.py --images {args.images} "
          f"--recognizer {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
