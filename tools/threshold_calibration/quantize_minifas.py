#!/usr/bin/env python3
"""Quantize a MiniFAS PAD model (V2 2.7x / V1SE 4.0x) to INT8.

Pipeline fidelity: crops are built through the same SCRFD detection plus a
faithful port of CropImage._get_new_box (see onnx_models.cpp:539-576) —
bbox expansion by the model's crop scale, resize to 80x80, RGB planar,
raw 0-255 float32 input — matching the production PAD input distribution.
Both models are opset 17, so per-tensor QDQ static quantization applies.

Validation: the same batch of real crops is scored with FP32 vs INT8 and the
score drift is reported.  The 0.281 threshold was calibrated on the FP32
fused score; drift must be small enough that the threshold stays valid.
Final sign-off additionally requires a live/screen recalibration with
PadCalibration (tools/pad_calibration).

Usage:
  python quantize_minifas.py --model assets/models/MiniFASNetV2.onnx --crop-scale 2.7
  python quantize_minifas.py --model assets/models/MiniFASNetV1SE.onnx --crop-scale 4.0
"""

from __future__ import annotations

import argparse
import math
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
INPUT_SIZE = 80  # MiniFASNet input is 80x80 (onnx_models.cpp m_inputHeight)

# Production MiniFAS sessions run single-threaded (onnx_models.cpp:474-477).
INTRA_OP_THREADS = 1
BENCH_REPEATS = 30
BENCH_WARMUP = 10


def crop_box(h: float, w: float, box: tuple[float, float, float, float],
             crop_scale: float) -> tuple[int, int, int, int]:
    """Faithful port of Silent-Face-Anti-Spoofing's _get_new_box +
    onnx_models.cpp:539-569.  box = (x1, y1, x2, y2) float SCRFD output;
    C++ integerizes the box first (dlib rectangle from long coords), so we
    mirror that before computing width/height.
    """
    x1, y1, x2, y2 = (int(v) for v in box)
    box_w = x2 - x1 + 1.0
    box_h = y2 - y1 + 1.0
    if box_w <= 1.0 or box_h <= 1.0:
        raise ValueError("degenerate box")
    scale = min((h - 1.0) / box_h, (w - 1.0) / box_w, float(crop_scale))
    new_w = box_w * scale
    new_h = box_h * scale
    cx = x1 + box_w / 2.0
    cy = y1 + box_h / 2.0
    left = cx - new_w / 2.0
    top = cy - new_h / 2.0
    right = cx + new_w / 2.0
    bottom = cy + new_h / 2.0
    if left < 0.0:
        right -= left
        left = 0.0
    if top < 0.0:
        bottom -= top
        top = 0.0
    if right > w - 1.0:
        left -= right - w + 1.0
        right = w - 1.0
    if bottom > h - 1.0:
        top -= bottom - h + 1.0
        bottom = h - 1.0
    return (max(0, int(left)), max(0, int(top)),
            min(int(w) - 1, int(right)), min(int(h) - 1, int(bottom)))


def make_crops(images_root: Path, crop_scale: float, per_identity: int) -> list[np.ndarray]:
    """Build 80x80 RGB-planar float32 tensors (raw 0-255 pixels) through the
    production detection + expanded-crop path.  One crop per image."""
    detector = calibrate.ScrfdDetector(DEFAULT_MODELS_DIR / "det_10g_gnkps.onnx")
    crops: list[np.ndarray] = []
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
            h, w = bgr.shape[:2]
            try:
                x1, y1, x2, y2 = crop_box(h, w, (det.x1, det.y1, det.x2, det.y2),
                                          crop_scale)
            except ValueError:
                continue
            crop = bgr[y1:y2 + 1, x1:x2 + 1]
            if crop.size == 0:
                continue
            resized = cv2.resize(crop, (INPUT_SIZE, INPUT_SIZE),
                                 interpolation=cv2.INTER_LINEAR)
            # RGB planar, raw 0-255 float — onnx_models.cpp:578-591.
            rgb = resized[:, :, ::-1].astype(np.float32)
            tensor = np.stack([rgb[:, :, 0], rgb[:, :, 1], rgb[:, :, 2]])
            crops.append(tensor.reshape(1, 3, INPUT_SIZE, INPUT_SIZE))
            n += 1
            if n >= per_identity:
                break
    return crops


def score(session: ort.InferenceSession, tensor: np.ndarray) -> float:
    """softmax(logits)[1] — the production real-face probability."""
    out = session.run(None, {session.get_inputs()[0].name: tensor})[0]
    logits = np.asarray(out).reshape(-1)
    if logits.shape[0] != 3:
        raise RuntimeError(f"unexpected output size {logits.shape}")
    m = logits.max()
    exps = np.exp(logits - m)
    return float(exps[1] / exps.sum())


def bench(session: ort.InferenceSession, tensor: np.ndarray) -> float:
    """Median wall-clock per inference, single thread (production config)."""
    times: list[float] = []
    for i in range(BENCH_WARMUP + BENCH_REPEATS):
        t0 = time.perf_counter()
        session.run(None, {session.get_inputs()[0].name: tensor})
        times.append(time.perf_counter() - t0)
    return statistics.median(times[BENCH_WARMUP:])


class _CalibReader(CalibrationDataReader):
    def __init__(self, tensors: list[np.ndarray], input_name: str) -> None:
        self._queue = list(tensors)
        self._input_name = input_name

    def get_next(self) -> dict[str, np.ndarray] | None:
        if not self._queue:
            return None
        return {self._input_name: self._queue.pop(0)}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Quantize a MiniFAS PAD model to INT8 and validate.",
    )
    parser.add_argument("--model", type=Path, required=True,
                        help="FP32 MiniFAS model (.onnx)")
    parser.add_argument("--crop-scale", type=float, required=True,
                        help="2.7 for MiniFASNetV2, 4.0 for MiniFASNetV1SE")
    parser.add_argument("--output", type=Path, default=None,
                        help="Output path (default: <model>_int8.onnx next to input)")
    parser.add_argument("--images", type=Path, default=DEFAULT_IMAGES,
                        help="Photo root for calibration crops (one subdir per person)")
    args = parser.parse_args()

    if not args.model.is_file():
        parser.error(f"model not found: {args.model}")
    out = args.output or args.model.with_name(
        f"{args.model.stem}_int8{args.model.suffix}")

    print(f"[1/5] Building crops (crop_scale={args.crop_scale}) from {args.images} ...",
          file=sys.stderr)
    crops = make_crops(args.images, args.crop_scale, per_identity=2)
    print(f"      {len(crops)} crops", file=sys.stderr)
    if not crops:
        parser.error(f"no crops could be built from {args.images}")

    opts = ort.SessionOptions()
    opts.intra_op_num_threads = INTRA_OP_THREADS
    fp32 = ort.InferenceSession(str(args.model), sess_options=opts,
                                providers=["CPUExecutionProvider"])
    fp32_ms = bench(fp32, crops[0])
    fp32_scores = [score(fp32, t) for t in crops]
    print(f"[2/5] FP32: {fp32_ms * 1000:.1f} ms/infer, "
          f"score mean={statistics.mean(fp32_scores):.4f}", file=sys.stderr)

    print(f"[3/5] Quantizing -> {out} ...", file=sys.stderr)
    input_name = fp32.get_inputs()[0].name
    calib = _CalibReader(crops, input_name)
    quantize_static(
        str(args.model), str(out), calib,
        quant_format=QuantFormat.QDQ,
        per_channel=False,
        weight_type=QuantType.QInt8,
    )
    print(f"      {args.model.stat().st_size / 1e6:.1f} MB -> "
          f"{out.stat().st_size / 1e6:.1f} MB", file=sys.stderr)

    print(f"[4/5] INT8 scoring drift over {len(crops)} crops ...", file=sys.stderr)
    int8 = ort.InferenceSession(str(out), sess_options=opts,
                                providers=["CPUExecutionProvider"])
    int8_ms = bench(int8, crops[0])
    int8_scores = [score(int8, t) for t in crops]
    drifts = [b - a for a, b in zip(fp32_scores, int8_scores)]
    print(f"      score drift: max={max(drifts):+.4f} mean={statistics.mean(drifts):+.4f}",
          file=sys.stderr)
    print(f"[5/5] INT8: {int8_ms * 1000:.1f} ms/infer "
          f"(speedup {fp32_ms / int8_ms:.2f}x)", file=sys.stderr)

    print(f"\nNext: replace the model, update SHA-256 in FaceService.cpp + "
          f"extract.go, then rerun PadCalibration live/screen to confirm the "
          f"0.281 threshold still holds.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
