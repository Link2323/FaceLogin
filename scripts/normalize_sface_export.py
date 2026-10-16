"""Normalize pinned SFace with constant weights removed from graph inputs.

The pinned upstream weights/nodes are preserved. This enables ORT optimizations,
and has been compared with the original using production inference/matching.
Write to a new staging path; the downloader validates the derivative before
moving it into the canonical model directory. Direct production writes fail.
Usage: python normalize_sface_export.py input.onnx output.onnx
Reference: microsoft/onnxruntime tools/python/remove_initializer_from_input.py
"""
import argparse
import hashlib
import json
from pathlib import Path

import onnx


UPSTREAM_SHA256 = "0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79"
NORMALIZED_SHA256 = "ae6a6ac44d2bdc87924e75fb23d8212430dd24f037f5e035c21deff99afc8b61"


def normalize(source: Path, output: Path) -> dict:
    source = source.resolve()
    output = output.resolve()
    if source == output or output.exists():
        raise ValueError("Use a new output path; never overwrite a model")
    repo = Path(__file__).resolve().parents[1]
    forbidden = (repo / "assets", repo / "installer", Path("C:/Program Files/FaceLogin"))
    if any(root.resolve() == output or root.resolve() in output.parents for root in forbidden):
        raise ValueError("Staging output must stay outside production and installer model directories")
    raw = source.read_bytes()
    source_hash = hashlib.sha256(raw).hexdigest()
    if source_hash != UPSTREAM_SHA256:
        raise ValueError("Input is not the pinned upstream SFace 2021dec model")
    model = onnx.load_model_from_string(raw)
    if model.ir_version < 4:
        raise ValueError("IR < 4 requires initializers in graph inputs")
    onnx.checker.check_model(model)
    original = onnx.ModelProto()
    original.CopyFrom(model)
    initializers = {tensor.name for tensor in model.graph.initializer}
    keep = [item for item in model.graph.input if item.name not in initializers]
    removed = len(model.graph.input) - len(keep)
    if removed != 174 or len(keep) != 1:
        raise ValueError("Unexpected upstream SFace input/initializer layout")
    del model.graph.input[:]
    model.graph.input.extend(keep)
    onnx.checker.check_model(model)
    # Restoring graph.input must recover the entire protobuf exactly: this
    # verifies every weight, node, attribute and other field is unchanged.
    restored = onnx.ModelProto()
    restored.CopyFrom(model)
    del restored.graph.input[:]
    restored.graph.input.extend(original.graph.input)
    if restored.SerializeToString() != original.SerializeToString():
        raise ValueError("Transformation changed fields outside graph.input")
    output.parent.mkdir(parents=True, exist_ok=True)
    normalized = model.SerializeToString()
    normalized_hash = hashlib.sha256(normalized).hexdigest()
    if normalized_hash != NORMALIZED_SHA256:
        raise ValueError("Normalized model does not match the pinned derivative")
    with output.open("xb") as destination:
        destination.write(normalized)
    return {
        "source_sha256": source_hash,
        "candidate_sha256": normalized_hash,
        "source_bytes": len(raw), "candidate_bytes": len(normalized),
        "ir_version": model.ir_version,
        "removed_initializer_inputs": removed,
        "remaining_inputs": [item.name for item in keep],
        "weights_nodes_other_fields_unchanged": True,
        "deployment": "Pinned runtime derivative; caller must verify/stage the matching binaries",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(json.dumps(normalize(args.source, args.output), indent=2))


if __name__ == "__main__":
    main()
