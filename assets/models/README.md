# Canonical runtime model assets

This directory is the sole source for the ONNX files shipped by FaceLogin.
`FaceLoginConsole` copies this exact set to
`installer/FaceLoginSetup/resources/models/` during its Release build; that
installer directory is generated output and must not become a second source of
truth.

| File | Role | Source-control policy |
|---|---|---|
| `det_10g_gnkps.onnx` | Kun-Hsiang Lin / DOCSAID SCRFD face detector and 5 keypoints | Revision/hash-pinned upstream download, locally normalized and INT8-quantized |
| `w600k_r50.onnx` | InsightFace 512-D recognizer | Revision/hash-pinned mirror download, locally INT8-quantized; product authorization pending |
| `MiniFASNetV2.onnx` | PAD model, 2.7x crop | Downloaded and hash-verified locally |
| `MiniFASNetV1SE.onnx` | PAD model, 4.0x crop | Downloaded and hash-verified locally |

Populate missing ignored models with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\download_models.ps1
```

The detector's publisher declares Apache-2.0 for the specific weights. Its
publisher, related training implementation, original hash and modification
record are documented in [model-licenses.md](../../docs/model-licenses.md).
The recognizer's mirror contains no separate weight-license grant. Its pinned
source, official research-only policy and unsent licensing inquiry are recorded
in [third_party/models/w600k-r50/](../../third_party/models/w600k-r50/authorization-request.md).
The downloader pins the upstream SCRFD 10g revision and raw SHA-256, normalizes
the export and verifies the final SHA-256 of all four files. Do not put the raw upstream detector in this
directory. Normalization requires Python with `onnx` and `numpy` installed
(`python -m pip install onnx numpy`).

For a locally running service, copy the canonical files to
`C:\ProgramData\FaceLogin\models\` (or the configured `DataPath`) after the
download. `MiniFASNetV2.onnx` and `MiniFASNetV1SE.onnx` are the two calibrated
production PAD models; both are required.
