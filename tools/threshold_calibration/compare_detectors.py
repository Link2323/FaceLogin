"""Offline detector A/B: candidate SCRFD variants vs the deployed det_10g INT8.

Compares speed (production-matching session options, 512x512 input) and
detection quality (recall @0.5, box/keypoint agreement vs baseline, embedding
drift through the full align->w600k_r50 chain) on local corpora.

Decode semantics mirror face_service/onnx_models.cpp and calibrate.py exactly:
direct 512x512 stretch resize (no letterbox), BGR planar (p-127.5)/128,
stride-8/16/32 grouped outputs, 2 anchors per cell (i -> cell i//2), box/kps
* stride distance decoding, greedy NMS IoU>0.5, largest-score face selected.

Usage:
  python compare_detectors.py \
      --model base=../../../assets/models/det_10g_gnkps.onnx \
      --model f10g=../../../assets/models/det_10g_gnkps.fp32.onnx \
      --model g25=../../.det25_eval/scrfd_2.5g_kps_dyn.onnx \
      --bench --bench-threads 16,1 --bench-iters 200 \
      --corpus ../../../my_faces ../../../data/lfw_subset \
      --downscales 1.0,0.6,0.4,0.28 \
      --embed ../../../assets/models/w600k_r50.onnx
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
from calibrate import (  # noqa: E402  (production-faithful align + embed)
    Recognizer,
    align_face,
    imread_unicode,
)

INPUT_SIZE = 512
STRIDES = (8, 16, 32)
NUM_ANCHORS = 2
SCORE_THRESHOLD = 0.5
NMS_IOU = 0.5
NORM_MEAN = 127.5
NORM_DIVISOR = 128.0


# ---------------------------------------------------------------------------
# Detection (vectorized decode, semantics = onnx_models.cpp / calibrate.py)
# ---------------------------------------------------------------------------

@dataclass
class Det:
    score: float
    box: np.ndarray  # x1,y1,x2,y2 in source pixels
    kps: np.ndarray  # 5x2 in source pixels


class Detector:
    def __init__(self, key: str, path: Path, intra_threads: int = 8):
        self.key = key
        self.path = path
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = intra_threads
        opts.log_severity_level = 3
        self._sess = ort.InferenceSession(
            str(path), sess_options=opts, providers=["CPUExecutionProvider"])
        self._in = self._sess.get_inputs()[0].name
        # Grouped order score_8/16/32, box_8/16/32, kps_8/16/32 — matched by
        # position, which both the production normalized export and the raw
        # insightface export already follow.
        assert len(self._sess.get_outputs()) == 9, path

    def preprocess(self, bgr: np.ndarray) -> np.ndarray:
        resized = cv2.resize(bgr, (INPUT_SIZE, INPUT_SIZE),
                             interpolation=cv2.INTER_LINEAR)
        t = (resized.astype(np.float32).transpose(2, 0, 1) - NORM_MEAN) / NORM_DIVISOR
        return t.reshape(1, 3, INPUT_SIZE, INPUT_SIZE)

    def infer(self, tensor: np.ndarray) -> list[np.ndarray]:
        return self._sess.run(None, {self._in: tensor})

    def detect_largest(self, bgr: np.ndarray) -> Det | None:
        src_h, src_w = bgr.shape[:2]
        tensor = self.preprocess(bgr)
        outs = self.infer(tensor)
        sx, sy = src_w / INPUT_SIZE, src_h / INPUT_SIZE
        cands: list[Det] = []
        for s_idx, stride in enumerate(STRIDES):
            sc = outs[s_idx][0, :, 0]
            keep = np.nonzero(sc >= SCORE_THRESHOLD)[0]
            if keep.size == 0:
                continue
            grid = INPUT_SIZE // stride
            cells = keep // NUM_ANCHORS
            cx = (cells % grid * stride).astype(np.float32)
            cy = (cells // grid * stride).astype(np.float32)
            d = outs[3 + s_idx][0, keep, :].astype(np.float32) * stride
            x1, y1 = cx - d[:, 0], cy - d[:, 1]
            x2, y2 = cx + d[:, 2], cy + d[:, 3]
            kd = outs[6 + s_idx][0, keep, :].astype(np.float32) * stride
            kx = cx[:, None] + kd[:, 0::2]
            ky = cy[:, None] + kd[:, 1::2]
            for i in range(keep.size):
                kps_flat = np.stack([kx[i], ky[i]], axis=1).reshape(-1)
                kps_flat *= np.array([sx, sy] * 5, np.float32)
                cands.append(Det(
                    score=float(sc[keep[i]]),
                    box=np.array([x1[i], y1[i], x2[i], y2[i]], np.float32)
                        * np.array([sx, sy, sx, sy], np.float32),
                    kps=kps_flat,
                ))
        if not cands:
            return None
        return max(self._nms(cands), key=lambda d: d.score)

    @staticmethod
    def _nms(dets: list[Det]) -> list[Det]:
        dets = sorted(dets, key=lambda d: d.score, reverse=True)
        kept: list[Det] = []
        for d in dets:
            ok = True
            for k in kept:
                if iou(d.box, k.box) > NMS_IOU:
                    ok = False
                    break
            if ok:
                kept.append(d)
        return kept


def iou(a: np.ndarray, b: np.ndarray) -> float:
    x1, y1 = max(a[0], b[0]), max(a[1], b[1])
    x2, y2 = min(a[2], b[2]), min(a[3], b[3])
    inter = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    ua = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / ua if ua > 0 else 0.0


# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

def bench(models: dict[str, Detector], threads_list: list[int], iters: int,
          warmup_img: np.ndarray) -> None:
    print("\n== speed (inference only, 512x512, real-photo tensor) ==")
    print(f"{'model':<10}{'threads':>8}{'p50 ms':>9}{'p90 ms':>9}{'min ms':>9}")
    for threads in threads_list:
        for key, det in models.items():
            opts = ort.SessionOptions()
            opts.intra_op_num_threads = threads
            opts.log_severity_level = 3
            sess = ort.InferenceSession(str(det.path), sess_options=opts,
                                        providers=["CPUExecutionProvider"])
            tensor = det.preprocess(warmup_img)
            for _ in range(20):
                sess.run(None, {sess.get_inputs()[0].name: tensor})
            ts = []
            for _ in range(iters):
                t0 = time.perf_counter()
                sess.run(None, {sess.get_inputs()[0].name: tensor})
                ts.append((time.perf_counter() - t0) * 1000)
            ts.sort()
            p = lambda q: ts[min(int(q * len(ts)), len(ts) - 1)]
            print(f"{key:<10}{threads:>8}{p(0.5):>9.1f}{p(0.9):>9.1f}{ts[0]:>9.1f}")


# ---------------------------------------------------------------------------
# Corpus evaluation
# ---------------------------------------------------------------------------

def list_images(path: Path) -> list[Path]:
    if path.is_file():
        return [path]
    exts = {".jpg", ".jpeg", ".png", ".bmp"}
    out = [p for p in sorted(path.rglob("*")) if p.suffix.lower() in exts]
    return out


def eval_corpus(models: dict[str, Detector], base_key: str, imgs: list[Path],
                tag: str, recognizer: Recognizer | None, downscale: float = 1.0
                ) -> dict[str, dict]:
    stats = {k: dict(n=0, found=0, ious=[], kps_err=[], scores=[],
                     widths=[], drifts=[]) for k in models}
    for p in imgs:
        bgr = imread_unicode(p)
        if bgr is None:
            continue
        if downscale != 1.0:
            bgr = cv2.resize(bgr, None, fx=downscale, fy=downscale,
                             interpolation=cv2.INTER_AREA)
        results: dict[str, Det | None] = {}
        for key, det in models.items():
            results[key] = det.detect_largest(bgr)
        base = results[base_key]
        for key, r in results.items():
            st = stats[key]
            st["n"] += 1
            if r is None:
                continue
            st["found"] += 1
            st["scores"].append(r.score)
            st["widths"].append(float(r.box[2] - r.box[0]))
            if base is not None:
                st["ious"].append(iou(r.box, base.box))
                diag = float(np.hypot(base.box[2] - base.box[0],
                                      base.box[3] - base.box[1]))
                dk = (r.kps - base.kps).reshape(5, 2)
                kerr = np.hypot(dk[:, 0], dk[:, 1]).mean() / max(diag, 1.0)
                st["kps_err"].append(float(kerr))
    # embedding drift needs chips from BOTH detectors, do a second loop
    if recognizer is not None:
        for p in imgs:
            bgr = imread_unicode(p)
            if bgr is None:
                continue
            if downscale != 1.0:
                bgr = cv2.resize(bgr, None, fx=downscale, fy=downscale,
                                 interpolation=cv2.INTER_AREA)
            chips: dict[str, np.ndarray | None] = {}
            for key, det in models.items():
                if key == base_key:
                    continue
                r = det.detect_largest(bgr)
                chips[key] = align_face(bgr, r.kps) if r is not None else None
            rb = models[base_key].detect_largest(bgr)
            chip_b = align_face(bgr, rb.kps) if rb is not None else None
            if chip_b is None:
                continue
            eb = recognizer.embed(chip_b)
            if eb is None:
                continue
            for key, chip in chips.items():
                if chip is None:
                    continue
                ec = recognizer.embed(chip)
                if ec is not None:
                    stats[key]["drifts"].append(float(
                        np.linalg.norm(ec - eb)))

    print(f"\n== corpus: {tag} (downscale {downscale}) ==")
    hdr = (f"{'model':<10}{'n':>6}{'recall':>8}{'score p50':>10}"
           f"{'IoU(vs base)':>13}{'kps err':>9}{'face w':>8}{'emb drift':>10}")
    print(hdr)
    out = {}
    for key, st in stats.items():
        iou_m = np.mean(st["ious"]) if st["ious"] else float("nan")
        kerr_m = np.mean(st["kps_err"]) if st["kps_err"] else float("nan")
        w_m = np.mean(st["widths"]) if st["widths"] else float("nan")
        drift = (f"{np.mean(st['drifts']):.3f}" if st["drifts"] else "-")
        rec = st["found"] / st["n"] if st["n"] else 0.0
        sp50 = np.percentile(st["scores"], 50) if st["scores"] else float("nan")
        print(f"{key:<10}{st['n']:>6}{rec:>8.3f}{sp50:>10.3f}"
              f"{iou_m:>13.3f}{kerr_m:>9.3f}{w_m:>8.1f}{drift:>10}")
        out[key] = st
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--model", action="append", required=True,
                    help="key=path (first is the baseline)")
    ap.add_argument("--bench", action="store_true")
    ap.add_argument("--bench-threads", default="16,1")
    ap.add_argument("--bench-iters", type=int, default=200)
    ap.add_argument("--corpus", action="append", default=[],
                    help="dir or file of images to evaluate")
    ap.add_argument("--max-images", type=int, default=0)
    ap.add_argument("--downscales", default="",
                    help="comma list, e.g. 1.0,0.5,0.35,0.25")
    ap.add_argument("--embed", default="", help="w600k_r50.onnx for drift")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    specs = [s.split("=", 1) for s in args.model]
    models = {k: Detector(k, Path(v).resolve()) for k, v in specs}
    base_key = specs[0][0]

    if args.bench:
        first_corpus = list_images(Path(args.corpus[0]).resolve()) if args.corpus else []
        if first_corpus:
            img = imread_unicode(first_corpus[0])
        else:
            img = np.full((480, 640, 3), 110, np.uint8)
        bench(models, [int(t) for t in args.bench_threads.split(",")],
              args.bench_iters, img)

    if args.corpus:
        rng = np.random.default_rng(args.seed)
        recognizer = Recognizer(Path(args.embed).resolve()) if args.embed else None
        downscales = ([float(x) for x in args.downscales.split(",")]
                      if args.downscales else [1.0])
        for c in args.corpus:
            imgs = list_images(Path(c).resolve())
            if args.max_images and len(imgs) > args.max_images:
                idx = rng.choice(len(imgs), args.max_images, replace=False)
                imgs = [imgs[i] for i in sorted(idx)]
            for ds in downscales:
                eval_corpus(models, base_key, imgs, str(c), recognizer, ds)


if __name__ == "__main__":
    main()
