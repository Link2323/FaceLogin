# Normalizes a SCRFD ONNX export to the convention the C++ decoder expects
# (validated against the RuteNL 34g_gnkps.onnx export).
#
# The kunkunlin1221/face-detection_scrfd-10g-gnkps export differs in TWO ways:
#   1. box_*/lmk5pt_* outputs are pre-scaled by the stride (Mul nodes with
#      constants 8/16/32 inside the graph), so they are pixel distances from
#      the anchor center. The C++ decoder multiplies by stride itself, which
#      would place the keypoints stride-times too far out. This script strips
#      those Mul nodes so outputs are raw stride-units again.
#   2. The 9 outputs are interleaved per stride (box_8, score_8, lmk5pt_8, ...)
#      instead of grouped (score_8.., box_8.., lmk5pt_8..). This script
#      reorders them to the grouped layout.
#
# Both operations are exact graph metadata edits (no weights touched); the
# result is verified with onnx.checker and numerically with onnxruntime when
# available. Idempotent: safe to run on an already-normalized file.
#
# Usage: python normalize_scrfd_export.py <input.onnx> <output.onnx>

import sys
import onnx
import numpy as np

try:
    import onnxruntime as ort
except ImportError:
    ort = None

GROUPED = ['score_8', 'score_16', 'score_32',
           'box_8', 'box_16', 'box_32',
           'lmk5pt_8', 'lmk5pt_16', 'lmk5pt_32']
STRIDES = {8: ['box_8', 'lmk5pt_8'],
           16: ['box_16', 'lmk5pt_16'],
           32: ['box_32', 'lmk5pt_32']}


def constant_value(m, name):
    """Scalar value of a constant (initializer or Constant node), or None."""
    for init in m.graph.initializer:
        if init.name == name:
            return float(np.asarray(onnx.numpy_helper.to_array(init)).item())
    for node in m.graph.node:
        if node.op_type == 'Constant' and node.output[0] == name:
            return float(np.asarray(
                onnx.numpy_helper.to_array(node.attribute[0].t)).item())
    return None


def strip_stride_muls(m):
    """Remove Mul(stride) nodes on the box_/lmk5pt_ output branches."""
    removed = 0
    for stride, outs in STRIDES.items():
        for out_name in outs:
            # output <- Reshape <- Transpose <- Mul? <- conv
            reshape = next(n for n in m.graph.node
                           if n.op_type == 'Reshape' and out_name in n.output)
            transpose = next(n for n in m.graph.node
                             if n.op_type == 'Transpose' and reshape.input[0] in n.output)
            producer = next((n for n in m.graph.node
                             if transpose.input[0] in n.output), None)
            if producer is None or producer.op_type != 'Mul':
                continue  # already normalized (or unusual graph — leave alone)
            val = constant_value(m, producer.input[1])
            if val != float(stride):
                raise SystemExit(
                    f'Mul feeding {out_name} has constant {val} (expected {stride}) — '
                    f'unexpected convention, refusing to modify')
            transpose.input[0] = producer.input[0]
            m.graph.node.remove(producer)
            removed += 1
    return removed


def main():
    src, dst = sys.argv[1], sys.argv[2]
    m = onnx.load(src, load_external_data=False)

    names = [o.name for o in m.graph.output]
    assert sorted(names) == sorted(GROUPED), f'unexpected outputs: {names}'

    stripped = strip_stride_muls(m)
    print(f'stripped stride-Mul nodes: {stripped}')

    ordered = sorted(m.graph.output, key=lambda o: GROUPED.index(o.name))
    if [o.name for o in ordered] != names:
        del m.graph.output[:]
        m.graph.output.extend(ordered)
        print(f'reordered outputs: {names} -> {[o.name for o in m.graph.output]}')
    else:
        print('outputs already in grouped order')

    onnx.checker.check_model(m)
    onnx.save(m, dst)
    print(f'saved: {dst}')

    if ort is not None:
        sess_old = ort.InferenceSession(src, providers=['CPUExecutionProvider'])
        sess_new = ort.InferenceSession(dst, providers=['CPUExecutionProvider'])
        in_name = sess_old.get_inputs()[0].name
        rng = np.random.default_rng(42)
        blob = rng.uniform(-1, 1, (1, 3, 320, 320)).astype(np.float32)
        old = {o.name: sess_old.run(None, {in_name: blob})[i]
               for i, o in enumerate(sess_old.get_outputs())}
        new = {o.name: sess_new.run(None, {in_name: blob})[i]
               for i, o in enumerate(sess_new.get_outputs())}
        # The only expected difference: box/lmk outputs of the new model are
        # the OLD model's outputs divided by the stride (Mul stripped).
        for stride, outs in STRIDES.items():
            for name in outs:
                assert np.allclose(new[name], old[name] / stride, atol=1e-4), \
                    f'{name} mismatch after Mul strip'
        for name in ['score_8', 'score_16', 'score_32']:
            assert np.array_equal(old[name], new[name]), f'{name} changed unexpectedly'
        print('numerical check passed: Mul strip exactly divides by stride')
    else:
        print('onnxruntime not available — numerical check skipped')


if __name__ == '__main__':
    main()
