#!/usr/bin/env python3
"""Compare two recognizer ONNX models on the SAME chips, same-angle /
cross-angle / stranger distributions — the exact口径 of the mbf appendix in
docs/threshold-calibration.md.

Usage:
  python compare_models.py --a ../../assets/models/w600k_r50.onnx \
                           --b ../../assets/models/w600k_r50_static.onnx

Output is a markdown table mirroring the mbf-vs-r50 appendix:
  - same-angle pairs (front-front / left-left / right-right) — real auth
    scenario (angle template vs angle probe)
  - cross-angle pairs (0 vs ±30) — worst case
  - me vs 58 LFW strangers (min distance per me photo)
"""

from __future__ import annotations

import argparse
import math
import statistics
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import calibrate  # noqa: E402

DEFAULT_MODELS_DIR = calibrate.DEFAULT_MODELS_DIR
DEFAULT_IMAGES = Path(__file__).parent / "data" / "lfw_subset"
ME_DIR = "me"


def build_chips(images_root: Path) -> list[tuple[str, str, np.ndarray]]:
    """(identity, filename, 112x112 BGR chip) for every detect+alignable
    image — detector and alignment run ONCE; both models embed the same chips,
    so the only variable is the recognizer."""
    detector = calibrate.ScrfdDetector(DEFAULT_MODELS_DIR / "det_10g_gnkps.onnx")
    out: list[tuple[str, str, np.ndarray]] = []
    exts = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
    for person_dir in sorted(p for p in images_root.iterdir() if p.is_dir()):
        for img_path in sorted(person_dir.iterdir()):
            if img_path.suffix.lower() not in exts:
                continue
            bgr = calibrate.load_image(img_path)
            if bgr is None:
                continue
            det = detector.detect_largest(bgr)
            if det is None:
                print(f"  [warn] no face: {img_path}", file=sys.stderr)
                continue
            chip = calibrate.align_face(bgr, det.kps)
            if chip is None:
                continue
            out.append((person_dir.name, img_path.name, chip))
    return out


def embed_all(model_path: Path, chips: list[tuple[str, str, np.ndarray]]) -> dict[str, dict[str, np.ndarray]]:
    """{identity: {filename: embedding}} for one recognizer."""
    rec = calibrate.Recognizer(model_path)
    result: dict[str, dict[str, np.ndarray]] = {}
    for ident, fname, chip in chips:
        emb = rec.embed(chip)
        if emb is None:
            continue
        result.setdefault(ident, {})[fname] = emb
    return result


def euclidean(a: np.ndarray, b: np.ndarray) -> float:
    return float(math.sqrt(float(((a - b) ** 2).sum())))


def same_angle_pairs(me: dict[str, np.ndarray]) -> list[float]:
    """front-front / left-left / right-right pairs (unique)."""
    out: list[float] = []
    for angle in ("front", "left", "right"):
        embs = [e for f, e in me.items() if f.split("_")[0] == angle]
        for i in range(len(embs)):
            for j in range(i + 1, len(embs)):
                out.append(euclidean(embs[i], embs[j]))
    return out


def cross_angle_pairs(me: dict[str, np.ndarray]) -> list[float]:
    """All pairs across different angles."""
    out: list[float] = []
    by_angle: dict[str, list[np.ndarray]] = {}
    for f, e in me.items():
        by_angle.setdefault(f.split("_")[0], []).append(e)
    angles = list(by_angle)
    for i in range(len(angles)):
        for j in range(i + 1, len(angles)):
            for a in by_angle[angles[i]]:
                for b in by_angle[angles[j]]:
                    out.append(euclidean(a, b))
    return out


def me_vs_strangers(me: dict[str, np.ndarray], others: dict[str, dict[str, np.ndarray]]) -> list[float]:
    """Min distance from each me photo to ANY stranger embedding."""
    gallery = [e for embs in others.values() for e in embs.values()]
    if not gallery:
        return []
    return [min(euclidean(p, g) for g in gallery) for p in me.values()]


def stats(vals: list[float]) -> str:
    if not vals:
        return "—"
    s = sorted(vals)
    def pct(p: float) -> float:
        k = (len(s) - 1) * (p / 100.0)
        lo, hi = math.floor(k), math.ceil(k)
        if lo == hi:
            return float(s[lo])
        return float(s[lo] + (s[hi] - s[lo]) * (k - lo))
    return (f"min {s[0]:.3f} / p50 {pct(50):.3f} / p90 {pct(90):.3f} / "
            f"p99 {pct(99):.3f} (n={len(s)})")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", type=Path, default=DEFAULT_MODELS_DIR / "w600k_r50.onnx",
                    help="baseline recognizer")
    ap.add_argument("--b", type=Path, required=True,
                    help="candidate recognizer")
    ap.add_argument("--images", type=Path, default=DEFAULT_IMAGES)
    args = ap.parse_args()

    print(f"[1/3] Detecting + aligning {args.images} (once) ...", file=sys.stderr)
    chips = build_chips(args.images)
    print(f"      {len(chips)} chips", file=sys.stderr)

    print(f"[2/3] Embedding with {args.a.name} ...", file=sys.stderr)
    emb_a = embed_all(args.a, chips)
    print(f"[3/3] Embedding with {args.b.name} ...", file=sys.stderr)
    emb_b = embed_all(args.b, chips)

    me_a, me_b = emb_a.get(ME_DIR, {}), emb_b.get(ME_DIR, {})
    others_a = {k: v for k, v in emb_a.items() if k != ME_DIR}
    others_b = {k: v for k, v in emb_b.items() if k != ME_DIR}

    rows = [
        ("same-angle (auth real scenario)", stats(same_angle_pairs(me_a)), stats(same_angle_pairs(me_b))),
        ("cross-angle (worst case)", stats(cross_angle_pairs(me_a)), stats(cross_angle_pairs(me_b))),
        (f"me vs {len(others_a)} strangers (min)", stats(me_vs_strangers(me_a, others_a)), stats(me_vs_strangers(me_b, others_b))),
    ]
    print()
    print(f"| 分布 | {args.a.name} (baseline) | {args.b.name} (candidate) |")
    print("|---|---|---|")
    print(f"| me 嵌入成功 | {len(me_a)} | {len(me_b)} |")
    for name, a, b in rows:
        print(f"| {name} | {a} | {b} |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
