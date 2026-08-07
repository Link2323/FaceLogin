#!/usr/bin/env python3
"""Download a small LFW subset for threshold calibration.

LFW (Labeled Faces in the Wild) gives the other-person distance distribution
that docs/todo.md ui1 was missing: hundreds of different people, organized as
one folder per identity. This script fetches the dataset, keeps the first N
identities with at most M images each, and writes them to a folder layout
that calibrate.py consumes directly:

    <out>/<identity_name>/<image>.jpg

Two sources are tried in order, because no single mirror is universally
reachable:

  1. HuggingFace `bitmind/lfw` parquet (via hf-mirror.com). ~180 MB download,
     then stream-extracted per-identity so the full parquet never needs to
     stay resident. Requires pyarrow (auto-installed if missing, with
     confirmation).
  2. UMass `lfw-funneled.tgz`. The canonical ~173 MB tarball. Only reachable
     if vis-www.cs.umass.edu resolves on your network.

If both fail (DNS, CDN reset, etc.), the script prints a clear message and
exits non-zero so you can drop in any per-identity image folder manually —
calibrate.py does not care where the images come from.

Usage:
  python download_dataset.py [--num-identities 30] [--max-per-identity 8]
                              [--out data/lfw_subset]
"""

from __future__ import annotations

import argparse
import hashlib
import io
import shutil
import subprocess
import sys
import tarfile
import urllib.error
import urllib.request
from pathlib import Path
from urllib.request import HTTPRedirectHandler

# HuggingFace parquet source (hf-mirror.com is the CN-friendly mirror; the
# upstream huggingface.co resolves to the same CDN). This dataset stores one
# row per image with columns including the identity label and the raw image
# bytes. Confirmed schema via pyarrow inspection: `label` (string) + `image`
# (struct with `bytes` field containing the JPEG).
HF_LFW_PARQUET_URL = (
    "https://hf-mirror.com/datasets/bitmind/lfw/resolve/main/"
    "data/train-00000-of-00001.parquet"
)

# UMass canonical tarball. Extracts to lfw_funneled/<Identity>/*.jpg.
UMASS_LFW_TGZ_URL = "http://vis-www.cs.umass.edu/lfw/lfw-funneled.tgz"

DEFAULT_OUT = Path(__file__).resolve().parent / "data" / "lfw_subset"
DEFAULT_CACHE = Path(__file__).resolve().parent / "data" / "_cache"


class _Redirect308(HTTPRedirectHandler):
    """urllib's default handler ignores 308 Permanent Redirect. Add explicit
    handlers for 307/308 so they follow like 301."""

    def http_error_307(self, req, fp, code, msg, headers):  # type: ignore[override]
        newurl = headers.get("location")
        if newurl:
            return self.redirect_request(req, fp, 301, msg, headers, newurl)
        return None

    def http_error_308(self, req, fp, code, msg, headers):  # type: ignore[override]
        newurl = headers.get("location")
        if newurl:
            return self.redirect_request(req, fp, 301, msg, headers, newurl)
        return None


def _download_streaming(url: str, dest: Path, expected_min_bytes: int) -> bool:
    """Download `url` to `dest` with redirect following; verify size. Returns
    True on success, False on network/size failure. Uses requests (handles
    308/SSL/streaming cleanly) with a urllib fallback."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and dest.stat().st_size >= expected_min_bytes:
        print(f"  cached: {dest}", file=sys.stderr)
        return True
    print(f"  downloading {url}", file=sys.stderr)
    print(f"          -> {dest}", file=sys.stderr)
    total = 0
    try:
        try:
            import requests
            with requests.get(url, stream=True, timeout=120,
                              headers={"User-Agent": "Mozilla/5.0"}) as r:
                r.raise_for_status()
                with dest.open("wb") as f:
                    for chunk in r.iter_content(chunk_size=1 << 20):
                        if not chunk:
                            continue
                        f.write(chunk)
                        total += len(chunk)
                        if total % (8 << 20) == 0:
                            print(f"  ...{total // (1024 * 1024)} MB",
                                  end="\r", file=sys.stderr, flush=True)
        except ImportError:
            # urllib fallback (rare; requests is near-universal).
            opener = urllib.request.build_opener(_Redirect308)
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            with opener.open(req, timeout=120) as r, dest.open("wb") as f:
                while True:
                    chunk = r.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
                    total += len(chunk)
                    if total % (8 << 20) == 0:
                        print(f"  ...{total // (1024 * 1024)} MB",
                              end="\r", file=sys.stderr, flush=True)
    except Exception as e:
        print(f"  network error: {type(e).__name__}: {e}", file=sys.stderr)
        if dest.exists():
            dest.unlink()
        return False
    print(file=sys.stderr)
    if total < expected_min_bytes:
        print(
            f"  incomplete: got {total} bytes, expected at least "
            f"{expected_min_bytes}. CDN may have reset the connection.",
            file=sys.stderr,
        )
        if dest.exists():
            dest.unlink()
        return False
    print(f"  done ({total // (1024 * 1024)} MB)", file=sys.stderr)
    return True


def _ensure_pyarrow() -> bool:
    """Return True if pyarrow importable; offer to pip install if not."""
    try:
        import pyarrow  # noqa: F401
        return True
    except ImportError:
        pass
    print(
        "[info] pyarrow is required to read the HF parquet source. "
        "Install it now with pip? [y/N] ",
        end="", file=sys.stderr, flush=True,
    )
    try:
        answer = input().strip().lower()
    except EOFError:
        answer = ""
    if answer not in ("y", "yes"):
        return False
    try:
        subprocess.check_call([sys.executable, "-m", "pip", "install", "pyarrow"])
    except subprocess.CalledProcessError as e:
        print(f"  pip install failed: {e}", file=sys.stderr)
        return False
    return True


def from_hf_parquet(out: Path, num_identities: int, max_per_identity: int) -> bool:
    """Source 1: HuggingFace bitmind/lfw parquet. Stream-read row groups, write
    JPGs per identity until we have enough identities."""
    if not _ensure_pyarrow():
        return False
    import pyarrow.parquet as pq

    cache = DEFAULT_CACHE / "lfw.parquet"
    # Parquet footer is at the end, so we need the WHOLE file — no partial read.
    # ~180 MB; cache it so re-runs are free.
    if not _download_streaming(HF_LFW_PARQUET_URL, cache, expected_min_bytes=100_000_000):
        return False

    print(f"  reading parquet, extracting up to {num_identities} identities...",
          file=sys.stderr)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    try:
        pf = pq.ParquetFile(str(cache))
    except Exception as e:
        print(f"  parquet read failed: {e}", file=sys.stderr)
        return False

    # Inspect schema to find the label + image columns robustly.
    schema = pf.schema_arrow
    col_names = [f.name for f in schema]
    # Identity can be a dedicated label column, or embedded in a filename/path
    # column (LFW convention: "<Identity>/<img>.jpg").
    label_col = next(
        (c for c in col_names if c.lower() in ("label", "name", "identity")),
        None,
    )
    path_col = next(
        (c for c in col_names if c.lower() in ("filename", "path", "file")),
        None,
    )
    if label_col is None and path_col is None:
        print(f"  cannot find identity column; available: {col_names}",
              file=sys.stderr)
        return False
    # Image column is typically `image` (struct with `bytes`) per HF convention.
    img_col = next((c for c in col_names if "image" in c.lower() or "img" in c.lower()), None)
    if img_col is None:
        print(f"  cannot find image column; available: {col_names}",
              file=sys.stderr)
        return False

    # Stream row groups; collect identities until we hit the target.
    cols_to_read = [img_col]
    if label_col:
        cols_to_read.append(label_col)
    if path_col:
        cols_to_read.append(path_col)
    seen: dict[str, int] = {}
    total = 0
    for rg_idx in range(pf.num_row_groups):
        table = pf.read_row_group(rg_idx, columns=cols_to_read)
        images = table.column(img_col).to_pylist()
        if label_col:
            labels = table.column(label_col).to_pylist()
        else:
            # Derive identity from the filename. LFW filenames are either
            # "<Identity>/<img>.jpg" (path form) or "<Identity>_NNNN.jpg"
            # (flat form). Handle both.
            import re
            paths = table.column(path_col).to_pylist()
            labels = []
            for p in paths:
                if not p:
                    labels.append("")
                    continue
                stem = Path(p.replace("\\", "/")).parts[-1]  # last segment
                # Strip trailing _NNNN(.jpg) identity index.
                m = re.match(r"^(.+?)_\d+(\.\w+)?$", stem)
                labels.append(m.group(1) if m else Path(stem).stem)
        for label, image in zip(labels, images):
            if not label:
                continue
            if label in seen:
                if seen[label] >= max_per_identity:
                    continue
            else:
                if len(seen) >= num_identities:
                    continue
                seen[label] = 0
                (out / label).mkdir(exist_ok=True)
            # image is a dict {"bytes": ..., "path": ...} per HF Image type.
            blob = image["bytes"] if isinstance(image, dict) else image
            if not blob:
                continue
            count = seen.get(label, 0)
            (out / label / f"{count:03d}.jpg").write_bytes(blob)
            seen[label] = count + 1
            total += 1
        if len(seen) >= num_identities and all(
            v >= max_per_identity for v in list(seen.values())[:num_identities]
        ):
            break

    # Trim to exactly num_identities (drop overflow).
    for extra in sorted(seen.keys())[num_identities:]:
        shutil.rmtree(out / extra, ignore_errors=True)

    print(
        f"  extracted {total} images across "
        f"{min(len(seen), num_identities)} identities -> {out}",
        file=sys.stderr,
    )
    return True


def from_umass_tgz(out: Path, num_identities: int, max_per_identity: int) -> bool:
    """Source 2: UMass lfw-funneled.tgz. Canonical per-identity tarball."""
    cache = DEFAULT_CACHE / "lfw-funneled.tgz"
    if not _download_streaming(UMASS_LFW_TGZ_URL, cache, expected_min_bytes=100_000_000):
        return False

    print(f"  extracting up to {num_identities} identities...", file=sys.stderr)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    exts = {".jpg", ".jpeg", ".png"}
    total = 0
    identities_done = 0
    with tarfile.open(cache, "r:gz") as tar:
        # Iteration order inside the tarball is alphabetical by identity.
        current_identity: str | None = None
        per_id_count = 0
        for member in tar:
            if not member.isfile():
                continue
            parts = Path(member.name).parts
            # Expect lfw_funneled/<Identity>/<img>.jpg
            if len(parts) < 3:
                continue
            identity = parts[-2]
            if identity != current_identity:
                if identities_done >= num_identities:
                    break
                current_identity = identity
                per_id_count = 0
                (out / identity).mkdir(exist_ok=True)
                identities_done += 1
            if per_id_count >= max_per_identity:
                continue
            if Path(member.name).suffix.lower() not in exts:
                continue
            f = tar.extractfile(member)
            if f is None:
                continue
            (out / identity / Path(member.name).name).write_bytes(f.read())
            per_id_count += 1
            total += 1
    print(
        f"  extracted {total} images across {identities_done} identities -> {out}",
        file=sys.stderr,
    )
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Download and trim an LFW subset for threshold calibration.",
    )
    parser.add_argument(
        "--num-identities", type=int, default=30,
        help="How many identities (people) to keep (default 30)",
    )
    parser.add_argument(
        "--max-per-identity", type=int, default=8,
        help="Max images per identity (default 8)",
    )
    parser.add_argument(
        "--out", type=Path, default=DEFAULT_OUT,
        help="Output subset directory (default data/lfw_subset)",
    )
    parser.add_argument(
        "--source", choices=["auto", "hf", "umass"], default="auto",
        help="Dataset source: hf (HuggingFace parquet), umass (tgz), or auto (try both)",
    )
    args = parser.parse_args()

    sources: list[tuple[str, callable]] = []
    if args.source in ("auto", "hf"):
        sources.append(("HuggingFace parquet (hf-mirror)", from_hf_parquet))
    if args.source in ("auto", "umass"):
        sources.append(("UMass tgz", from_umass_tgz))

    for name, fn in sources:
        print(f"[try] {name}", file=sys.stderr)
        try:
            ok = fn(args.out, args.num_identities, args.max_per_identity)
        except Exception as e:
            print(f"  source failed with exception: {e}", file=sys.stderr)
            ok = False
        if ok and args.out.is_dir() and any(args.out.iterdir()):
            print(
                f"\nDone. Dataset ready at {args.out}\n"
                f"Next: python scripts/threshold_calibration/calibrate.py "
                f"--images {args.out}",
                file=sys.stderr,
            )
            return 0

    # All sources failed.
    print(
        "\n[fail] Could not download LFW from any configured source.\n"
        "       This is usually a network/CDN issue, not a code bug.\n"
        "       Options:\n"
        "         1. Re-run later (CDN resets are often transient).\n"
        "         2. Use a VPN/proxy and retry.\n"
        "         3. Download any per-identity face image folder yourself\n"
        "            (LFW, CelebA, or photos of friends/family) and point\n"
        "            calibrate.py at it with --images <folder>. The script\n"
        "            only needs <folder>/<identity>/*.jpg — source does not\n"
        "            matter.\n"
        "         4. For small tests, copy photos of a few different people\n"
        "            into my_faces/<person_name>/ and rerun calibrate.py.",
        file=sys.stderr,
    )
    return 3


if __name__ == "__main__":
    raise SystemExit(main())
