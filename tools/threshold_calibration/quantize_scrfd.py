#!/usr/bin/env python3
"""Quantize the SCRFD detector (det_10g_gnkps.onnx) to INT8 and validate.

Pipeline fidelity: calibration inputs are real photos resized with the
production direct-stretch 512x512 path (mirrors onnx_models.cpp:241-252,
BGR planar, (p - 127.5) / 128).  The model is opset 11, so per-tensor QDQ
static quantization applies.

The SCRFD decode below is a faithful port of vision/onnx_models.cpp:186-428;
it moved here from calibrate.py when that (retired-recognizer) calibration
tool was removed.

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
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort
from onnxruntime.quantization import CalibrationDataReader, QuantFormat, QuantType
from onnxruntime.quantization import quantize_static

DEFAULT_MODELS_DIR = Path(__file__).resolve().parents[2] / "assets" / "models"
DEFAULT_IMAGES = Path(__file__).parent / "data" / "lfw_subset"

INTRA_OP_THREADS = 8  # production OnnxThreadCount() cap
BENCH_REPEATS = 20
BENCH_WARMUP = 5

# ---------------------------------------------------------------------------
# SCRFD constants + decode — faithful port of vision/onnx_models.cpp:186-428.
# ---------------------------------------------------------------------------

# vision/onnx_models.cpp:244 — SCRFD input is a fixed square (512 since 2026-08).
SCRFD_INPUT_SIZE = 512  # production detect input (was 640; 512 validated
# 2026-08: 60/60 detections, box IoU 0.96, detect time -34% on weak CPUs)
# vision/onnx_models.cpp:311, 379.
SCRFD_SCORE_THRESHOLD = 0.5
SCRFD_NMS_IOU = 0.5
# Three strides, two anchors per cell. onnx_models.cpp:330-344.
SCRFD_STRIDES = (8, 16, 32)
SCRFD_NUM_ANCHORS = 2

# vision/onnx_models.cpp:241-252. NOTE the divisor is 128.0, not 127.5.
SCRFD_NORM_MEAN = 127.5
SCRFD_NORM_DIVISOR = 128.0


def imread_unicode(path: Path) -> np.ndarray | None:
    """cv2.imread that survives non-ASCII (Chinese) paths on Windows.

    cv2.imread on Windows silently returns None for paths containing
    characters outside the system code page. np.fromfile + cv2.imdecode
    bypasses the libc FILE* layer that causes this.
    """
    try:
        data = np.fromfile(str(path), dtype=np.uint8)
    except OSError:
        return None
    if data.size == 0:
        return None
    return cv2.imdecode(data, cv2.IMREAD_COLOR)


def load_image(path: Path, rotate: int = 0) -> np.ndarray | None:
    """Load an image with optional 90-degree rotation, Unicode-path safe.

    `rotate` is clockwise degrees, one of {0, 90, 180, 270}. Useful when the
    capture device stored frames sideways (e.g. portrait phone video saved as
    landscape) — SCRFD is trained on upright faces and produces garbage
    keypoints/bboxes on rotated faces, which would corrupt the embedding.
    """
    bgr = imread_unicode(path)
    if bgr is None:
        return None
    if rotate == 90:
        return cv2.rotate(bgr, cv2.ROTATE_90_CLOCKWISE)
    if rotate == 180:
        return cv2.rotate(bgr, cv2.ROTATE_180)
    if rotate == 270:
        return cv2.rotate(bgr, cv2.ROTATE_90_COUNTERCLOCKWISE)
    return bgr


@dataclass(frozen=True)
class Detection:
    """One face: bbox (x1,y1,x2,y2) + score + 5 keypoints (x,y interleaved)."""

    score: float
    x1: float
    y1: float
    x2: float
    y2: float
    kps: np.ndarray  # shape (10,), float32


class ScrfdDetector:
    """SCRFD 10g gnkps detector, consuming the normalized export.

    The model MUST be the output of scripts/normalize_scrfd_export.py: that
    script strips the trailing Mul(stride) nodes and reorders outputs to
    grouped [score_8/16/32, box_8/16/32, lmk5pt_8/16/32]. The C++ decoder
    (and this port) assumes that layout.
    """

    # Names in the grouped order produced by normalize_scrfd_export.py.
    # Verified against the production det_10g_gnkps.onnx in assets/models/:
    #   score_{8,16,32}, box_{8,16,32}, lmk5pt_{8,16,32}.
    _OUTPUT_NAMES = [
        "score_8", "score_16", "score_32",
        "box_8", "box_16", "box_32",
        "lmk5pt_8", "lmk5pt_16", "lmk5pt_32",
    ]

    def __init__(self, model_path: Path) -> None:
        if not model_path.is_file():
            raise FileNotFoundError(f"SCRFD model not found: {model_path}")
        self._session = ort.InferenceSession(
            str(model_path),
            providers=["CPUExecutionProvider"],
        )
        self._input_name = self._session.get_inputs()[0].name
        # Sort the model's outputs into the grouped order the decoder expects.
        # The graph stores them under their normalized names; match by name so
        # we are robust to onnxruntime's insertion order.
        avail = {o.name: i for i, o in enumerate(self._session.get_outputs())}
        missing = [n for n in self._OUTPUT_NAMES if n not in avail]
        if missing:
            raise ValueError(
                f"SCRFD model is missing grouped outputs {missing}. "
                f"Run scripts/normalize_scrfd_export.py on it first. "
                f"Available outputs: {list(avail)}"
            )
        self._output_indices = [avail[n] for n in self._OUTPUT_NAMES]

    def detect_largest(self, bgr_image: np.ndarray) -> Detection | None:
        """Return the highest-score detection, or None if no face found.

        Mirrors FaceService's DetectLargestFace usage: take the max-score box
        after NMS rather than letting callers re-sort.
        """
        dets = self._detect_all(bgr_image)
        if not dets:
            return None
        return max(dets, key=lambda d: d.score)

    def _detect_all(self, bgr_image: np.ndarray) -> list[Detection]:
        src_h, src_w = bgr_image.shape[:2]
        # onnx_models.cpp:238-239 — direct stretch resize, no letterbox.
        resized = cv2.resize(
            bgr_image, (SCRFD_INPUT_SIZE, SCRFD_INPUT_SIZE),
            interpolation=cv2.INTER_LINEAR,
        )
        # onnx_models.cpp:241-252 — BGR planar, (p - 127.5) / 128.
        # resized is HWC BGR uint8; transpose to CHW, keep BGR order.
        tensor = (
            resized.astype(np.float32).transpose(2, 0, 1)
            - SCRFD_NORM_MEAN
        ) / SCRFD_NORM_DIVISOR
        tensor = tensor.reshape(1, 3, SCRFD_INPUT_SIZE, SCRFD_INPUT_SIZE)

        outputs = self._session.run(
            None, {self._input_name: tensor.astype(np.float32)}
        )
        # Reorder to grouped [score_8/16/32, box_8/16/32, kps_8/16/32].
        grouped = [outputs[i] for i in self._output_indices]

        # onnx_models.cpp:235-236 — independent X/Y scale factors (different
        # for non-square source frames).
        scale_x = src_w / SCRFD_INPUT_SIZE
        scale_y = src_h / SCRFD_INPUT_SIZE

        candidates: list[Detection] = []
        for s_idx, stride in enumerate(SCRFD_STRIDES):
            score_blob = grouped[0 + s_idx]   # shape (N, 1)
            box_blob = grouped[3 + s_idx]     # shape (N, 4)
            kps_blob = grouped[6 + s_idx]     # shape (N, 10)
            n = score_blob.shape[1]
            grid = SCRFD_INPUT_SIZE // stride  # 64 / 32 / 16

            # onnx_models.cpp:330-344 — anchor centers, NO +0.5 offset.
            centers = np.empty((grid * grid, 2), dtype=np.float32)
            for r in range(grid):
                for c in range(grid):
                    centers[r * grid + c, 0] = c * stride
                    centers[r * grid + c, 1] = r * stride

            for i in range(n):
                score = float(score_blob[0, i, 0])
                if score < SCRFD_SCORE_THRESHOLD:
                    continue
                # onnx_models.cpp:336-337 — i -> cell i//2; both anchors share
                # the same center.
                cx, cy = centers[i // SCRFD_NUM_ANCHORS]

                # onnx_models.cpp:357-364 — box * stride, then distance-to-bbox.
                dist = box_blob[0, i, :4] * stride
                x1 = cx - dist[0]
                y1 = cy - dist[1]
                x2 = cx + dist[2]
                y2 = cy + dist[3]

                # onnx_models.cpp:366-369 — kps * stride, then distance-to-kps.
                kd = kps_blob[0, i, :10] * stride
                kps = np.empty(10, dtype=np.float32)
                for k in range(5):
                    kps[k * 2] = cx + kd[k * 2]
                    kps[k * 2 + 1] = cy + kd[k * 2 + 1]

                candidates.append(
                    Detection(
                        score=score,
                        x1=x1 * scale_x,
                        y1=y1 * scale_y,
                        x2=x2 * scale_x,
                        y2=y2 * scale_y,
                        kps=kps * np.array(
                            [scale_x, scale_y] * 5, dtype=np.float32
                        ),
                    )
                )

        return self._nms(candidates)

    @staticmethod
    def _nms(dets: list[Detection]) -> list[Detection]:
        # onnx_models.cpp:375-398 — greedy, score-descending, IoU > 0.5
        # (strictly greater) suppresses the lower-score box.
        if not dets:
            return []
        order = sorted(range(len(dets)), key=lambda i: dets[i].score, reverse=True)
        kept: list[bool] = [True] * len(dets)
        result: list[Detection] = []
        for a_pos, a in enumerate(order):
            if not kept[a_pos]:
                continue
            result.append(dets[a])
            da = dets[a]
            a_area = (da.x2 - da.x1) * (da.y2 - da.y1) + 1e-5
            for b_pos in range(a_pos + 1, len(order)):
                if not kept[b_pos]:
                    continue
                db = dets[order[b_pos]]
                ix = min(da.x2, db.x2) - max(da.x1, db.x1)
                if ix <= 0:
                    continue
                iy = min(da.y2, db.y2) - max(da.y1, db.y1)
                if iy <= 0:
                    continue
                b_area = (db.x2 - db.x1) * (db.y2 - db.y1) + 1e-5
                inter = ix * iy
                iou = inter / (a_area + b_area - inter + 1e-5)
                if iou > SCRFD_NMS_IOU:
                    kept[b_pos] = False
        return result


# ---------------------------------------------------------------------------
# Quantization.
# ---------------------------------------------------------------------------


def build_input(bgr_image: np.ndarray) -> np.ndarray:
    """Production SCRFD input tensor: direct-stretch 512x512, BGR planar,
    (p - 127.5) / 128 (mirrors onnx_models.cpp:241-252)."""
    resized = cv2.resize(bgr_image, (SCRFD_INPUT_SIZE, SCRFD_INPUT_SIZE),
                         interpolation=cv2.INTER_LINEAR)
    tensor = (resized.astype(np.float32).transpose(2, 0, 1) -
              SCRFD_NORM_MEAN) / SCRFD_NORM_DIVISOR
    return tensor.reshape(1, 3, SCRFD_INPUT_SIZE, SCRFD_INPUT_SIZE)


def load_images(images_root: Path, limit: int) -> list[np.ndarray]:
    imgs: list[np.ndarray] = []
    exts = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
    for person_dir in sorted(p for p in images_root.iterdir() if p.is_dir()):
        for img_path in sorted(person_dir.iterdir()):
            if img_path.suffix.lower() not in exts:
                continue
            bgr = load_image(img_path)
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
    fp32_det = ScrfdDetector(args.model)
    fp32_det._session = fp32
    int8_det = ScrfdDetector(args.model)
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
