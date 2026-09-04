#!/usr/bin/env python3
"""Full-LFW imposter calibration for the FaceLogin 512-D pipeline.

Extends the 58-identity subset evidence in docs/threshold-calibration.md to
the complete LFW roster (~5,749 people / 13,233 images). The numerically
faithful embedding pipeline (SCRFD gnkps -> 5-pt Umeyama -> w600k_r50 ->
L2-unit embedding) is imported from calibrate.py so every distance here is
comparable to the production matcher bit-for-bit.

Only the pairwise stage is reimplemented: calibrate.other_person_distances()
is a pure-Python triple loop (fine for 58 identities, hours for 5,749). Here
the per-identity-pair min distance — the same "best across faces" rule the
credential_store matcher applies (calibrate.py:554-570) — is computed with
chunked matrix products:

    ||a - b||^2 = 2 - 2*a.b        (both inputs L2-unit)

Usage:
  python full_lfw_imposter.py --images data/lfw_full
  python full_lfw_imposter.py --tgz data/lfw-funneled.tgz   # auto-extract

Embeddings are cached to data/_lfw_full_cache.npz; re-runs load the cache
and skip straight to the analysis.
"""

from __future__ import annotations

import argparse
import math
import sys
import tarfile
import tempfile
import time
from pathlib import Path

import numpy as np

import calibrate

DEFAULT_MODELS_DIR = calibrate.DEFAULT_MODELS_DIR
HERE = Path(__file__).resolve().parent
CACHE = HERE / "data" / "_lfw_full_cache.npz"
EXTRACT_ROOT = HERE / "data" / "lfw_full"
IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}

# Sentinel / gate thresholds the progressive-learning revision cites.
REPORT_BINS = (1.00, 1.05, 1.10, 1.15, 1.20, 1.25, 1.30)


def extract_tgz(tgz: Path) -> Path:
    """Extract a LFW tarball into per-identity folders under EXTRACT_ROOT."""
    if EXTRACT_ROOT.is_dir() and any(EXTRACT_ROOT.iterdir()):
        print(f"[cache] {EXTRACT_ROOT} already populated", file=sys.stderr)
        return EXTRACT_ROOT
    EXTRACT_ROOT.mkdir(parents=True, exist_ok=True)
    n = 0
    with tarfile.open(tgz, "r:gz") as tf:
        for member in tf:
            if not member.isfile() or Path(member.name).suffix.lower() not in IMAGE_EXTS:
                continue
            # LFW tarballs nest as lfw*/<Identity>/<img>.jpg — flatten one level.
            parts = Path(member.name).parts
            if len(parts) < 2:
                continue
            identity, fname = parts[-2], parts[-1]
            dest = EXTRACT_ROOT / identity / fname
            dest.parent.mkdir(parents=True, exist_ok=True)
            with tf.extractfile(member) as src, dest.open("wb") as out:
                out.write(src.read())
            n += 1
    print(f"[extract] {n} images -> {EXTRACT_ROOT}", file=sys.stderr)
    return EXTRACT_ROOT


def embed_all(images_root: Path, detector, recognizer) -> tuple[np.ndarray, np.ndarray, list[str]]:
    """Embed every gallery image. Returns (embeddings, person_index, names).

    person_index[i] is the row in `names` that embeddings[i] belongs to.
    """
    idents = sorted(p for p in images_root.iterdir() if p.is_dir())
    names: list[str] = []
    embs: list[np.ndarray] = []
    owner: list[int] = []
    failed = 0
    total = 0
    t0 = time.time()
    for pi, ident_dir in enumerate(idents):
        names.append(ident_dir.name)
        for img_path in sorted(ident_dir.iterdir()):
            if img_path.suffix.lower() not in IMAGE_EXTS:
                continue
            total += 1
            bgr = calibrate.load_image(img_path, 0)
            if bgr is None:
                failed += 1
                continue
            det = detector.detect_largest(bgr)
            if det is None:
                failed += 1
                continue
            chip = calibrate.align_face(bgr, det.kps)
            if chip is None:
                failed += 1
                continue
            emb = recognizer.embed(chip)
            if emb is None:
                failed += 1
                continue
            embs.append(np.asarray(emb, dtype=np.float32))
            owner.append(pi)
        if (pi + 1) % 200 == 0:
            dt = time.time() - t0
            print(f"  [embed] {pi + 1}/{len(idents)} identities, "
                  f"{len(embs)} embeddings, {failed} failed, {dt:.0f}s",
                  file=sys.stderr, flush=True)
    E = np.stack(embs) if embs else np.zeros((0, 512), np.float32)
    return E, np.asarray(owner, dtype=np.int64), names


def same_person_pairs(E: np.ndarray, owner: np.ndarray) -> np.ndarray:
    """All unique within-identity pairwise distances (sanity check)."""
    out = []
    for p in np.unique(owner):
        idx = np.flatnonzero(owner == p)
        if len(idx) < 2:
            continue
        Ep = E[idx]
        G = Ep @ Ep.T
        d2 = np.clip(2.0 - 2.0 * G, 0.0, None)
        iu = np.triu_indices(len(idx), 1)
        out.append(np.sqrt(d2[iu]))
    return np.concatenate(out) if out else np.zeros(0)


def other_person_min_pairs(E: np.ndarray, owner: np.ndarray) -> np.ndarray:
    """Min distance per identity pair — the matcher's best-across-faces rule.

    Chunked over probe images; per chunk, distances to every image are
    reduced to per-identity minima with np.minimum.reduceat over the
    person-sorted column layout, then folded into a running per-pair min.
    """
    order = np.argsort(owner, kind="stable")
    Es = E[order]
    os_ = owner[order]
    starts = np.flatnonzero(np.r_[True, os_[1:] != os_[:-1]])
    counts = np.diff(np.r_[starts, len(os_)])
    n_person = len(starts)

    pair_min = np.full((n_person, n_person), np.inf, dtype=np.float32)
    CH = 1024
    for c0 in range(0, len(Es), CH):
        chunk = Es[c0:c0 + CH]
        G = chunk @ Es.T                       # (ch, N)
        d = np.sqrt(np.clip(2.0 - 2.0 * G, 0.0, None))
        # Reduce columns to per-person min: (ch, n_person)
        pers_min = np.minimum.reduceat(d, starts, axis=1)
        # Fold rows of the chunk into their owner rows.
        row_owner = os_[c0:c0 + CH]
        for r in range(chunk.shape[0]):
            pair_min[row_owner[r]] = np.minimum(pair_min[row_owner[r]], pers_min[r])
        if (c0 // CH) % 4 == 0:
            print(f"  [pairs] {c0 + chunk.shape[0]}/{len(Es)} rows",
                  file=sys.stderr, flush=True)
    iu = np.triu_indices(n_person, 1)
    return pair_min[iu]


def top_pairs(pair_min: np.ndarray, names: list[str], k: int = 10) -> list[tuple[float, str, str]]:
    iu0, iu1 = np.triu_indices(len(names), 1)
    order = np.argsort(pair_min)[:k]
    return [(float(pair_min[i]), names[iu0[i]], names[iu1[i]]) for i in order]


def quantiles(v: np.ndarray, qs=(0.001, 0.01, 0.05, 0.5, 0.95, 0.99)) -> dict[str, float]:
    if len(v) == 0:
        return {}
    keys = [f"p{q * 100:g}" for q in qs]
    return dict(zip(keys, np.quantile(v, qs).tolist()))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--images", type=Path, default=None,
                    help="Gallery root: <images>/<identity>/*.{jpg,png} "
                         "(default: extracted tgz location)")
    ap.add_argument("--tgz", type=Path, default=None,
                    help="LFW tarball to extract (lfw.tgz / lfw-funneled.tgz)")
    ap.add_argument("--detector", type=Path,
                    default=DEFAULT_MODELS_DIR / "det_10g_gnkps.onnx")
    ap.add_argument("--recognizer", type=Path,
                    default=DEFAULT_MODELS_DIR / "w600k_r50.onnx")
    ap.add_argument("--top", type=int, default=10,
                    help="Report the N closest identity pairs")
    args = ap.parse_args()

    if args.images is None and args.tgz is None:
        if CACHE.exists():
            args.images = EXTRACT_ROOT
        else:
            ap.error("need --images or --tgz (or a previous cache)")

    if args.images is None:
        args.images = extract_tgz(args.tgz)
    if not args.images.is_dir():
        ap.error(f"gallery not found: {args.images}")

    if CACHE.exists():
        z = np.load(CACHE, allow_pickle=False)
        E, owner = z["E"], z["owner"]
        names = [str(x) for x in z["names"]]
        print(f"[cache] {E.shape[0]} embeddings / {len(names)} identities",
              file=sys.stderr)
    else:
        detector = calibrate.ScrfdDetector(args.detector)
        recognizer = calibrate.Recognizer(args.recognizer)
        E, owner, names = embed_all(args.images, detector, recognizer)
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        np.savez_compressed(CACHE, E=E, owner=owner,
                            names=np.asarray(names))
        print(f"[cache] saved {CACHE}", file=sys.stderr)

    print(f"\n== LFW full imposter calibration ==")
    print(f"identities: {len(names)}   embeddings: {E.shape[0]}")

    same = same_person_pairs(E, owner)
    if len(same):
        qs = quantiles(same)
        print(f"same-person (within-identity, n={len(same)}): "
              f"min={same.min():.4f} "
              + " ".join(f"{k}={v:.4f}" for k, v in qs.items())
              + f" max={same.max():.4f}")

    pair_min = other_person_min_pairs(E, owner)
    qs = quantiles(pair_min)
    print(f"\nother-person per-pair min (n={len(pair_min)} pairs):")
    print(f"  min = {pair_min.min():.4f}")
    for k, v in qs.items():
        print(f"  {k:>6} = {v:.4f}")
    print(f"  max = {pair_min.max():.4f}")
    print("\nbelow-threshold pair counts:")
    for t in REPORT_BINS:
        n = int((pair_min < t).sum())
        print(f"  < {t:.2f}: {n} ({n / len(pair_min) * 100:.4f}%)")
    print(f"\ntop-{args.top} closest identity pairs:")
    for d, a, b in top_pairs(pair_min, names, args.top):
        print(f"  {d:.4f}  {a}  vs  {b}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
