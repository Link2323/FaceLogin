# Canonical runtime model assets

This directory is the sole source for the ONNX files shipped by FaceLogin.
`FaceLoginConsole` copies this exact set to
`installer/FaceLoginSetup/resources/models/` during its Release build; that
installer directory is generated output and must not become a second source of
truth.

| File | Role | Source-control policy |
|---|---|---|
| `det_10g_gnkps.onnx` | SCRFD face detector and 5 keypoints | Downloaded and normalized locally |
| `w600k_r50.onnx` | InsightFace 512-D recognizer | Downloaded locally |
| `MiniFASNetV2.onnx` | PAD model, 2.7x crop | Downloaded and hash-verified locally |
| `MiniFASNetV1SE.onnx` | PAD model, 4.0x crop | Downloaded and hash-verified locally |

Populate missing ignored models with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\download_models.ps1
```

The downloader normalizes the upstream SCRFD 10g export and verifies the final
SHA-256 of all four files. Do not put the raw upstream detector in this
directory. Normalization requires Python with `onnx` and `numpy` installed
(`python -m pip install onnx numpy`).

For a locally running service, copy the canonical files to
`C:\ProgramData\FaceLogin\models\` (or the configured `DataPath`) after the
download. `MiniFASNetV2.onnx` and `MiniFASNetV1SE.onnx` are the two calibrated
production PAD models; both are required.
