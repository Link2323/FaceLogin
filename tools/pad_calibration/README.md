# PAD threshold calibration

`PadCalibration` records the same production PAD score used by FaceLogin:
MiniFASNetV2 (2.7x crop) and MiniFASNetV1SE (4.0x crop), evaluated sequentially
and fused with a 50/50 arithmetic mean. It also records each model's score for
diagnostics. The tool does not make an authentication decision; it appends raw
observations to CSV for offline analysis.

## Build

Build the `PadCalibration` CMake target in Release mode. Close FaceLoginConsole
and other applications that hold the camera before capture.

To benchmark PAD inference on the current CPU without camera or detector time:

```powershell
build\tools\pad_calibration\Release\PadCalibration.exe `
  --models assets\models `
  --validate-models-only --benchmark-iterations 200
```

Each model is warmed up 30 times. The timing includes face-region crop, resize,
tensor preparation, ONNX Runtime execution, and score postprocessing. It
excludes model initialization and the shared SCRFD detector.

Download the pinned production models and verify their GitHub Release hashes:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\download_minifas_models.ps1
```

The files come from the Apache-2.0 licensed
[`yakhyo/face-anti-spoofing`](https://github.com/yakhyo/face-anti-spoofing)
ONNX exports of MiniVision's Silent-Face-Anti-Spoofing models. The downloader
and calibration executable both verify the pinned size and SHA-256. The same
canonical files are copied into installer resources and verified again there.

## Pilot capture

Use pseudonyms rather than real names. Each `session` must be unique. Do not
mix live and attack frames in one session.

```powershell
$tool = 'build\tools\pad_calibration\Release\PadCalibration.exe'
$csv = 'pad-calibration\pilot.csv'

& $tool --label live --split tune --subject p01 `
  --session p01_d1_live_normal --condition normal_front `
  --output $csv --count 50 --models assets\models

# Show a photo of the same subject on a phone/tablet to the camera.
& $tool --label screen --split tune --subject p01 `
  --session p01_d1_screen_phone --condition phone_brightness_100_front `
  --output $csv --count 50 --models assets\models

python scripts\analyze_pad_calibration.py $csv --split tune

# Analyze the same rows with each individual model score.
python scripts\analyze_pad_calibration.py $csv --score-column minifas_v2_score
python scripts\analyze_pad_calibration.py $csv --score-column minifas_v1se_score
python scripts\analyze_pad_calibration.py $csv --score-column minifas_ensemble_score
```

The analyzer rejects a threshold unless both frame-level and complete
5/5-window results satisfy the APCER target (default 0%) and BPCER ceiling
(default 5%). Among those operating points it reports the strictest threshold.
Threshold search covers `[0,1]` by default. The deployed operating point is
`0.281`, and production requires all 5 of 5 frames to meet it.

Five-frame groups cut from one continuous capture are correlated decision
windows, not independent presentations, so their Wilson intervals must not be
treated as a formal PAD certification result.

New CSV files use `production_score` as the primary 50/50 fusion score. For a
CSV captured before this tool was promoted to production, analyze the identical
historical fusion column explicitly:

```powershell
python scripts\analyze_pad_calibration.py $csv `
  --score-column minifas_ensemble_score --fixed-threshold 0.281
```

By default the tool reads `DataPath` from `HKLM\SOFTWARE\FaceLogin`, loads the
active `config.json`, and reuses its camera, rotation, and low-light settings.
Use `--data-dir`, `--models`, `--camera-device`, `--rotation`, or
`--low-light-enhance` only for an intentional controlled comparison.

Collect separate sessions for printed photos and screens, normal/low/back
lighting, near/far distance, glasses, and the supported head poses. Formal
validation must use new sessions (ideally another day and additional people)
with `--split validation`; frames from a tuning session are correlated and are
not a valid holdout set.

Media Foundation capture matches enrollment/standalone operation. Before any
production threshold is adopted, repeat the validation through the lock-screen
service's DirectShow camera path by adding `--backend ds`. Use a separate CSV
for DS validation so backend-specific distributions cannot be mixed silently.

The CSV contains biometric-derived scores and metadata. Keep it local, restrict
access, and delete it when calibration is complete.
