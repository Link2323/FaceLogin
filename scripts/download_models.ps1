# FaceLogin canonical-model downloader.
#
# Fetches the four production ONNX files into assets\models by default.  The
# SCRFD 10g upstream export is normalized before it is accepted: the C++
# decoder requires raw stride units and grouped outputs, while the upstream
# graph uses pre-scaled/interleaved outputs.  Every final artifact is pinned by
# exact byte size and SHA-256 so a partial, stale, or raw detector export never
# becomes a release input.

param(
    [string]$ModelsDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'assets\models'),
    [string]$PythonExe = 'python',
    [string]$CalibImages = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\threshold_calibration\data\lfw_subset')
)

$ErrorActionPreference = 'Stop'

# The release detector is a locally-quantized INT8 build (static QDQ,
# per-tensor) of the normalized FP32 artifact — see
# tools/threshold_calibration/quantize_scrfd.py for the experiment that
# validated it (60/60 detection agreement, box IoU 0.99, 1.23x detect
# speedup, 15.5 -> 4.3 MB).  This script downloads the raw upstream export,
# normalizes it, then quantizes it in place.
$detectorRaw = @{
    Name = 'det_10g_gnkps.onnx.raw-download'
    Url = 'https://hf-mirror.com/kunkunlin1221/face-detection_scrfd-10g-gnkps/resolve/main/scrfd_10g_gnkps_fp32.onnx'
    RawSize = 16273449
}
$detectorFp32 = @{
    Name = 'det_10g_gnkps.fp32.onnx'
    Size = 16272909
    Sha256 = 'C940F97765FDC4B872B4A1EA041248D3E3D550202B7639F9488BE558A6C0ACB0'
}
$detectorInt8 = @{
    Name = 'det_10g_gnkps.onnx'
    Size = 4257451
    Sha256 = '07B62718EB454EE1881465C12D0D0546F2E916E3BB549F142DC221729BF7F4DC'
}
# The release recognizer is a locally-quantized INT8 build (static QDQ,
# per-tensor) of the pinned FP32 artifact — see
# tools/threshold_calibration/quantize_r50.py for the experiment + calibration
# that validated it (same-angle p50 0.665 -> 0.694, 1.68x on the dev CPU).
# This script downloads the pinned FP32 model, then quantizes it in place.
$recognizerFp32 = @{
    Name = 'w600k_r50.fp32.onnx'
    Url = 'https://hf-mirror.com/richarrrddd/w600k_r50_v1/resolve/main/w600k_r50.onnx'
    Size = 174383860
    Sha256 = '4C06341C33C2CA1F86781DAB0E829F88AD5B64BE9FBA56E56BC9EBDEFC619E43'
}
$recognizerInt8 = @{
    Name = 'w600k_r50.onnx'
    Size = 43805153
    Sha256 = 'B9B2EA32AFAA88DFD226255F354EA241C3A744ABF75B3DBDC00C95F7F00E185'
}

function Test-PinnedFile {
    param(
        [string]$Path,
        [Int64]$ExpectedSize,
        [string]$ExpectedSha256
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }

    if ((Get-Item -LiteralPath $Path).Length -ne $ExpectedSize) {
        return $false
    }

    $actualSha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    return $actualSha256 -ieq $ExpectedSha256
}

function Download-File {
    param(
        [string]$Url,
        [string]$Destination
    )

    $temporary = "$Destination.download"
    Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    try {
        Invoke-WebRequest -Uri $Url -OutFile $temporary
        Move-Item -LiteralPath $temporary -Destination $Destination -Force
    }
    finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

Write-Host '============================================' -ForegroundColor Cyan
Write-Host '  FaceLogin - Canonical Model Downloader' -ForegroundColor Cyan
Write-Host '============================================' -ForegroundColor Cyan
Write-Host "Models directory: $ModelsDir"

New-Item -ItemType Directory -Force -Path $ModelsDir | Out-Null

$detectorFp32Path = Join-Path $ModelsDir $detectorFp32.Name
$detectorPath = Join-Path $ModelsDir $detectorInt8.Name
if (Test-PinnedFile -Path $detectorPath -ExpectedSize $detectorInt8.Size -ExpectedSha256 $detectorInt8.Sha256) {
    Write-Host "[SKIP] $($detectorInt8.Name) is already the verified INT8 artifact." -ForegroundColor Green
}
else {
    # 1. Fetch + normalize + pin the canonical FP32 detector.
    if (-not (Test-PinnedFile -Path $detectorFp32Path -ExpectedSize $detectorFp32.Size -ExpectedSha256 $detectorFp32.Sha256)) {
        $rawDetectorPath = "$detectorFp32Path.raw-download"
        Write-Host "[DOWNLOAD] raw SCRFD export" -ForegroundColor Yellow
        try {
            Download-File -Url $detectorRaw.Url -Destination $rawDetectorPath
            if ((Get-Item -LiteralPath $rawDetectorPath).Length -ne $detectorRaw.RawSize) {
                throw "Raw detector size is unexpected: $rawDetectorPath"
            }

            $normalizer = Join-Path $PSScriptRoot 'normalize_scrfd_export.py'
            Write-Host '[NORMALIZE] converting SCRFD export to the decoder convention' -ForegroundColor Yellow
            & $PythonExe $normalizer $rawDetectorPath $detectorFp32Path
            if ($LASTEXITCODE -ne 0) {
                throw "SCRFD normalization failed with exit code $LASTEXITCODE."
            }
        }
        finally {
            Remove-Item -LiteralPath $rawDetectorPath -Force -ErrorAction SilentlyContinue
        }

        if (-not (Test-PinnedFile -Path $detectorFp32Path -ExpectedSize $detectorFp32.Size -ExpectedSha256 $detectorFp32.Sha256)) {
            throw "Normalized detector did not match the pinned artifact: $detectorFp32Path"
        }
        Write-Host "[OK] $($detectorFp32.Name) normalized and verified." -ForegroundColor Green
    }
    else {
        Write-Host "[SKIP] $($detectorFp32.Name) is already verified." -ForegroundColor Green
    }

    # 2. Quantize in place to the release INT8 artifact.
    Write-Host "[QUANTIZE] $($detectorFp32.Name) -> $($detectorInt8.Name)" -ForegroundColor Yellow
    $quantizer = Join-Path $PSScriptRoot '..\tools\threshold_calibration\quantize_scrfd.py'
    & $PythonExe $quantizer --model $detectorFp32Path --output $detectorPath
    if ($LASTEXITCODE -ne 0) {
        throw "SCRFD quantization failed with exit code $LASTEXITCODE. Pass -CalibImages to point at a folder of real face photos (one subdir per person)."
    }
    if (-not (Test-PinnedFile -Path $detectorPath -ExpectedSize $detectorInt8.Size -ExpectedSha256 $detectorInt8.Sha256)) {
        throw "Quantized detector did not match the pinned artifact: $detectorPath"
    }
    Write-Host "[OK] $($detectorInt8.Name) quantized and verified." -ForegroundColor Green
}

$recognizerFp32Path = Join-Path $ModelsDir $recognizerFp32.Name
$recognizerPath = Join-Path $ModelsDir $recognizerInt8.Name
if (Test-PinnedFile -Path $recognizerPath -ExpectedSize $recognizerInt8.Size -ExpectedSha256 $recognizerInt8.Sha256) {
    Write-Host "[SKIP] $($recognizerInt8.Name) is already the verified INT8 artifact." -ForegroundColor Green
}
else {
    # 1. Fetch + pin the canonical FP32 model.
    if (Test-PinnedFile -Path $recognizerFp32Path -ExpectedSize $recognizerFp32.Size -ExpectedSha256 $recognizerFp32.Sha256) {
        Write-Host "[SKIP] $($recognizerFp32.Name) is already verified." -ForegroundColor Green
    }
    else {
        Write-Host "[DOWNLOAD] $($recognizerFp32.Name)" -ForegroundColor Yellow
        Download-File -Url $recognizerFp32.Url -Destination $recognizerFp32Path
        if (-not (Test-PinnedFile -Path $recognizerFp32Path -ExpectedSize $recognizerFp32.Size -ExpectedSha256 $recognizerFp32.Sha256)) {
            throw "Recognizer did not match the pinned artifact: $recognizerFp32Path"
        }
        Write-Host "[OK] $($recognizerFp32.Name) verified." -ForegroundColor Green
    }

    # 2. Quantize in place to the release INT8 artifact.
    Write-Host "[QUANTIZE] $($recognizerFp32.Name) -> $($recognizerInt8.Name)" -ForegroundColor Yellow
    $quantizer = Join-Path $PSScriptRoot '..\tools\threshold_calibration\quantize_r50.py'
    & $PythonExe $quantizer --recognizer $recognizerFp32Path --output $recognizerPath --images $CalibImages --no-bench
    if ($LASTEXITCODE -ne 0) {
        throw "r50 quantization failed with exit code $LASTEXITCODE. Pass -CalibImages to point at a folder of real face photos (one subdir per person)."
    }
    if (-not (Test-PinnedFile -Path $recognizerPath -ExpectedSize $recognizerInt8.Size -ExpectedSha256 $recognizerInt8.Sha256)) {
        throw "Quantized recognizer did not match the pinned artifact: $recognizerPath"
    }
    Write-Host "[OK] $($recognizerInt8.Name) quantized and verified." -ForegroundColor Green
}

# The shared downloader pins and verifies both calibrated MiniFAS models.
$miniFasDownloader = Join-Path $PSScriptRoot 'download_minifas_models.ps1'
& $miniFasDownloader -Destination $ModelsDir

Write-Host ''
Write-Host 'All four canonical runtime models are verified.' -ForegroundColor Green
Write-Host 'For standalone/service debugging, copy assets\models\*.onnx to the configured DataPath\models directory.'
