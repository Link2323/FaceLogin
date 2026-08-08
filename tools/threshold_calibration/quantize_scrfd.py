#!/usr/bin/env python3
"""Quantize the SCRFD detector (det_10g_gnkps.onnx) to INT8 and validate.

Pipeline fidelity: calibration inputs are real photos resized with the
production direct-stretch 640x640 path (calibrate.py / onnx_models.cpp:238-252,
BGR planar, (p - 127.5) / 128).  The model is opset 11, so per-tensor QDQ
static quantization applies (same constraint as r50).

Validation: the same batch of photos is run through FP32 vs INT8 and compared
per photo: largest-face box IoU, score delta, and detection agreement.  SCRFD
is not a security gate (it only locates faces) but a missed face blocks auth,
so detection agreement must be 100% on the validation set and box IoU high.

Usage:
  python quantize_scrfd.py --model assets/models/det_10g_gnkps.onnx
"""

from __future__ import annotations

import argparse
import statistics
import sys
import time
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort
from onnxruntime.quantization import CalibrationDataReader, QuantFormat, QuantType
from onnxruntime.quantization import quantize_static

sys.path.insert(0, str(Path(__file__).parent))
import calibrate  # noqa: E402

DEFAULT_MODELS_DIR = calibrate.DEFAULT_MODELS_DIR
DEFAULT_IMAGES = Path(__file__).parent / "data" / "lfw_subset"
SCRFD_INPUT_SIZE = calibrate.SCRFD_INPUT_SIZE  # 640

INTRA_OP_THREADS = 8  # production OnnxThreadCount() cap
BENCH_REPEATS = 20
BENCH_WARMUP = 5


def build_input(bgr_image: np.ndarray) -> np.ndarray:
    """Production SCRFD input tensor: direct-stretch 640x640, BGR planar,
    (p - 127.5) / 128 (calibrate.py:238-252)."""
    resized = cv2.resize(bgr_image, (SCRFD_INPUT_SIZE, SCRFD_INPUT_SIZE),
                         interpolation=cv2.INTER_LINEAR)
    tensor = (resized.astype(np.float32).transpose(2, 0, 1) -
              calibrate.SCRFD_NORM_MEAN) / calibrate.SCRFD_NORM_DIVISOR
    return tensor.reshape(1, 3, SCRFD_INPUT_SIZE, SCRFD_INPUT_SIZE)


def load_images(images_root: Path, limit: int) -> list[np.ndarray]:
    imgs: list[np.ndarray] = []
    exts = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
    for person_dir in sorted(p for p in images_root.iterdir() if p.is_dir()):
        for img_path in sorted(person_dir.iterdir()):
            if img_path.suffix.lower() not in exts:
                continue
            bgr = calibrate.load_image(img_path)
            if bgr is None:
                continue
            imgs.append(bgr)
            if len(imgs) >= limit:
                return imgs
    return imgs


class _CalibReader(CalibrationDataReader):
    def __init__(self, tensors: list[np.ndarray], input_name: str) -> None:
        self._queue = list(tensors)
        self._input_name = input_name

    def get_next(self) -> dict[str, np.ndarray] | None:
        if not self._queue:
            return None
        return {self._input_name: self._queue.pop(0)}


def bench(session: ort.InferenceSession, tensor: np.ndarray) -> float:
    times: list[float] = []
    for i in range(BENCH_WARMUP + BENCH_REPEATS):
        t0 = time.perf_counter()
        session.run(None, {session.get_inputs()[0].name: tensor})
        times.append(time.perf_counter() - t0)
    return statistics.median(times[BENCH_WARMUP:])


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Quantize the SCRFD detector to INT8 and validate.",
    )
    parser.add_argument("--model", type=Path,
                        default=DEFAULT_MODELS_DIR / "det_10g_gnkps.onnx")
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--images", type=Path, default=DEFAULT_IMAGES)
    parser.add_argument("--limit", type=int, default=60,
                        help="max validation photos")
    args = parser.parse_args()

    if not args.model.is_file():
        parser.error(f"model not found: {args.model}")
    out = args.output or args.model.with_name(
        f"{args.model.stem}_int8{args.model.suffix}")

    print(f"[1/6] Loading photos from {args.images} ...", file=sys.stderr)
    imgs = load_images(args.images, args.limit)
    print(f"      {len(imgs)} photos", file=sys.stderr)
    if not imgs:
        parser.error("no photos loaded")

    opts = ort.SessionOptions()
    opts.intra_op_num_threads = INTRA_OP_THREADS
    fp32 = ort.InferenceSession(str(args.model), sess_options=opts,
                                providers=["CPUExecutionProvider"])
    input_name = fp32.get_inputs()[0].name
    tensors = [build_input(i) for i in imgs]

    fp32_ms = bench(fp32, tensors[0])
    print(f"[2/6] FP32 detect: {fp32_ms * 1000:.1f} ms "
          f"({INTRA_OP_THREADS} threads)", file=sys.stderr)

    print(f"[3/6] Quantizing -> {out} ...", file=sys.stderr)
    calib = _CalibReader(tensors, input_name)
    quantize_static(
        str(args.model), str(out), calib,
        quant_format=QuantFormat.QDQ,
        per_channel=False,
        weight_type=QuantType.QInt8,
    )
    print(f"      {args.model.stat().st_size / 1e6:.1f} MB -> "
          f"{out.stat().st_size / 1e6:.1f} MB", file=sys.stderr)

    print(f"[4/6] Detection agreement FP32 vs INT8 over {len(imgs)} photos ...",
          file=sys.stderr)
    int8 = ort.InferenceSession(str(out), sess_options=opts,
                                providers=["CPUExecutionProvider"])
    fp32_det = calibrate.ScrfdDetector(args.model)
    fp32_det._session = fp32
    int8_det = calibrate.ScrfdDetector(args.model)
    int8_det._session = int8

    agree, missed_fp32, missed_int8 = 0, 0, 0
    iou_scores: list[float] = []
    score_deltas: list[float] = []
    for img in imgs:
        a = fp32_det.detect_largest(img)
        b = int8_det.detect_largest(img)
        if a is None and b is None:
            agree += 1
        elif a is None or b is None:
            if a is None:
                missed_int8 += 1  # INT8 detected, FP32 missed
            else:
                missed_fp32 += 1  # FP32 detected, INT8 missed
        else:
            inter = max(0, min(a.x2, b.x2) - max(a.x1, b.x1)) * \
                    max(0, min(a.y2, b.y2) - max(a.y1, b.y1))
            uni = (a.x2 - a.x1) * (a.y2 - a.y1) + \
                  (b.x2 - b.x1) * (b.y2 - b.y1) - inter
            iou = inter / uni if uni > 0 else 0.0
            iou_scores.append(iou)
            score_deltas.append(b.score - a.score)
            agree += 1 if iou >= 0.5 else 0

    print(f"      agreement: {agree}/{len(imgs)}  "
          f"(FP32-only={missed_fp32}, INT8-only={missed_int8})", file=sys.stderr)
    if iou_scores:
        print(f"      box IoU: min={min(iou_scores):.3f} "
              f"mean={statistics.mean(iou_scores):.3f}", file=sys.stderr)
    if score_deltas:
        print(f"      score delta: max={max(score_deltas):+.3f} "
              f"mean={statistics.mean(score_deltas):+.3f}", file=sys.stderr)

    int8_ms = bench(int8, tensors[0])
    print(f"[5/6] INT8 detect: {int8_ms * 1000:.1f} ms "
          f"(speedup {fp32_ms / int8_ms:.2f}x)", file=sys.stderr)
    print(f"[6/6] INT8 size: {out.stat().st_size / 1e6:.1f} MB", file=sys.stderr)

    print(f"\nNext: replace the model, update SHA-256 in FaceService.cpp + "
          f"extract.go, then verify on real auths.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
