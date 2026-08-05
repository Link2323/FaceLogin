# FaceLogin Model Download Script
# Downloads the model files required by the v1.5 pipeline:
#   - det_10g_gnkps.onnx   (SCRFD 10g group-norm keypoints detector, ~15.5 MB)
#   - w600k_r50.onnx       (InsightFace ResNet50 recognizer, ~174 MB)
#
# NOTE on det_10g_gnkps.onnx: the source repo (kunkunlin1221/face-detection_scrfd-10g-gnkps)
# exports with two convention differences vs the C++ decoder: outputs interleaved
# per stride (box_8/score_8/lmk5pt_8, ...) AND box/lmk pre-scaled by stride
# (Mul(8/16/32) inside the graph — decoding would place keypoints stride-times
# too far out). scripts/normalize_scrfd_export.py strips the Muls and reorders
# the outputs; run it after downloading. (The 34g detector it replaced was
# already in the decoder's convention — see git history.)
#
# The OULU anti-spoof model (OULU_Protocol_2_model_0_0.onnx) is small and
# bundled with the installer, so it is not fetched here.
#
# Sources are HuggingFace mirrors reachable from mainland China
# (huggingface.co direct is blocked there; hf-mirror.com works).
# The dlib 68-point shape predictor is no longer used (v1.5 replaced it with
# SCRFD's own 5 keypoints — see docs/side-face-plan-v2.md).

param(
    [string]$ModelsDir = "$env:ProgramData\FaceLogin\models"
)

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  FaceLogin - Model Download Script" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Models will be downloaded to: $ModelsDir"
Write-Host ""

# Create directory
New-Item -ItemType Directory -Force -Path $ModelsDir | Out-Null

# Model URLs (verified 2026-08-05; byte sizes are exact)
$models = @(
    @{
        Name = "det_10g_gnkps.onnx"
        Url  = "https://hf-mirror.com/kunkunlin1221/face-detection_scrfd-10g-gnkps/resolve/main/scrfd_10g_gnkps_fp32.onnx"
        Size = 16273449
    },
    @{
        Name = "w600k_r50.onnx"
        Url  = "https://hf-mirror.com/richarrrddd/w600k_r50_v1/resolve/main/w600k_r50.onnx"
        Size = 174383860
    }
)

foreach ($model in $models) {
    $file = Join-Path $ModelsDir $model.Name

    if (Test-Path $file) {
        $fileInfo = Get-Item $file
        if ($fileInfo.Length -eq $model.Size) {
            Write-Host "[SKIP] $($model.Name) already exists (size OK)." -ForegroundColor Green
            continue
        }
        Write-Host "[RESUME] $($model.Name) exists but size mismatch ($($fileInfo.Length) vs $($model.Size)). Redownloading." -ForegroundColor Yellow
    }

    Write-Host "[DOWNLOAD] $($model.Name)" -ForegroundColor Yellow
    Write-Host "  URL: $($model.Url)"
    Write-Host "  Expected: $([math]::Round($model.Size / 1MB, 1)) MB"

    try {
        Invoke-WebRequest -Uri $model.Url -OutFile $file -ErrorAction Stop
        $fileInfo = Get-Item $file
        if ($fileInfo.Length -ne $model.Size) {
            Write-Host "  WARNING: size $($fileInfo.Length) != expected $($model.Size) — file may be corrupt." -ForegroundColor Red
        } else {
            Write-Host "  Download complete. Size verified." -ForegroundColor Green
        }
    }
    catch {
        Write-Host "  ERROR: Download failed: $_" -ForegroundColor Red
        Write-Host "  Please download manually from: $($model.Url)"
    }
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "Model download complete." -ForegroundColor Cyan
Write-Host ""
Write-Host "Models location: $ModelsDir"
Write-Host ""
Write-Host "Required files:"
Write-Host "  1. det_10g_gnkps.onnx (~15.5 MB; normalize with scripts/normalize_scrfd_export.py)"
Write-Host "  2. w600k_r50.onnx (~174 MB)"
Write-Host "  3. OULU_Protocol_2_model_0_0.onnx (bundled with the installer)"
Write-Host ""
Write-Host "Next step: Run the FaceLoginSetup.exe installer, or place the models in the install dir."
Write-Host "============================================" -ForegroundColor Cyan
