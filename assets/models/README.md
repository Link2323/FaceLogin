# Canonical runtime model assets

This directory is the sole source for the ONNX files shipped by FaceLogin.
`FaceLoginConsole` copies this exact set to
`installer/FaceLoginSetup/resources/models/` during its Release build; that
installer directory is generated output and must not become a second source of
truth.

| File | Role | Source-control policy |
|---|---|---|
| `det_10g_gnkps.onnx` | Kun-Hsiang Lin / DOCSAID SCRFD face detector and 5 keypoints | Revision/hash-pinned upstream download, locally normalized and INT8-quantized |
| `face_recognition_sface_2021dec.onnx` | OpenCV SFace 128-D recognizer | Unmodified revision/hash-pinned FP32; Apache-2.0 model-directory declaration |
| `MiniFASNetV2.onnx` | PAD model, 2.7x crop | Downloaded and hash-verified locally |
| `MiniFASNetV1SE.onnx` | PAD model, 4.0x crop | Downloaded and hash-verified locally |

Populate missing ignored models with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\download_models.ps1
```

The detector's publisher declares Apache-2.0 for the specific weights. Its
publisher, related training implementation, original hash and modification
record are documented in [model-licenses.md](../../docs/model-licenses.md).
The SFace model-directory Apache-2.0 declaration and weight provenance are archived in [third_party/models/sface/](../../third_party/models/sface/provenance.json). Historical R50 weights may remain locally for offline comparison but are not runtime or installer inputs.
The downloader pins the upstream SCRFD 10g revision and raw SHA-256, normalizes
the export and verifies the final SHA-256 of all four files. Do not put the raw upstream detector in this
directory. Normalization requires Python with `onnx` and `numpy` installed
(`python -m pip install onnx numpy`).

For a locally running service, copy the canonical files to
`C:\ProgramData\FaceLogin\models\` (or the configured `DataPath`) after the
download. `MiniFASNetV2.onnx` and `MiniFASNetV1SE.onnx` are the two calibrated
production PAD models; both are required.
