"""Download pinned offline evaluation weights; never stage installer resources."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

HERE = Path(__file__).resolve().parent
DEST = HERE / "data" / "recognizer_ab" / "models"


def verified(path: Path, spec: dict) -> bool:
    if not path.is_file() or path.stat().st_size != spec["bytes"]:
        return False
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest() == spec["sha256"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=HERE / "recognizer_candidates.json")
    args = parser.parse_args()
    specs = json.loads(args.manifest.read_text("utf-8"))
    DEST.mkdir(parents=True, exist_ok=True)
    for name, spec in specs.items():
        target = DEST / spec["filename"]
        if verified(target, spec):
            print(f"{name}: cached and verified", flush=True)
            continue
        partial = target.with_suffix(".part")
        urls = [spec["url"]]
        if "huggingface.co/" in spec["url"]:
            urls.append(spec["url"].replace("huggingface.co/", "hf-mirror.com/"))
        for url in urls:
            try:
                print(f"{name}: downloading {spec['bytes']} bytes", flush=True)
                request = urllib.request.Request(url, headers={"User-Agent": "FaceLogin-offline-evaluation"})
                with urllib.request.urlopen(request, timeout=45) as response, partial.open("wb") as dest:
                    for block in iter(lambda: response.read(1024 * 1024), b""):
                        dest.write(block)
                if not verified(partial, spec):
                    raise ValueError(f"{name}: size or SHA-256 mismatch (received {partial.stat().st_size} bytes)")
                partial.replace(target)
                print(f"{name}: SHA-256 verified", flush=True)
                break
            except Exception as exc:
                partial.unlink(missing_ok=True)
                print(f"{name}: {type(exc).__name__}: {exc}", flush=True)
        else:
            raise RuntimeError(f"Could not download verified {name} weights")


if __name__ == "__main__":
    main()
