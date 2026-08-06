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
    [string]$PythonExe = 'python'
)

$ErrorActionPreference = 'Stop'

$detector = @{
    Name = 'det_10g_gnkps.onnx'
    Url = 'https://hf-mirror.com/kunkunlin1221/face-detection_scrfd-10g-gnkps/resolve/main/scrfd_10g_gnkps_fp32.onnx'
    RawSize = 16273449
    Size = 16272909
    Sha256 = 'C940F97765FDC4B872B4A1EA041248D3E3D550202B7639F9488BE558A6C0ACB0'
}
$recognizer = @{
    Name = 'w600k_r50.onnx'
    Url = 'https://hf-mirror.com/richarrrddd/w600k_r50_v1/resolve/main/w600k_r50.onnx'
    Size = 174383860
    Sha256 = '4C06341C33C2CA1F86781DAB0E829F88AD5B64BE9FBA56E56BC9EBDEFC619E43'
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

$detectorPath = Join-Path $ModelsDir $detector.Name
if (Test-PinnedFile -Path $detectorPath -ExpectedSize $detector.Size -ExpectedSha256 $detector.Sha256) {
    Write-Host "[SKIP] $($detector.Name) is already normalized and verified." -ForegroundColor Green
}
else {
    $rawDetectorPath = "$detectorPath.raw-download"
    Write-Host "[DOWNLOAD] raw $($detector.Name)" -ForegroundColor Yellow
    try {
        Download-File -Url $detector.Url -Destination $rawDetectorPath
        if ((Get-Item -LiteralPath $rawDetectorPath).Length -ne $detector.RawSize) {
            throw "Raw detector size is unexpected: $rawDetectorPath"
        }

        $normalizer = Join-Path $PSScriptRoot 'normalize_scrfd_export.py'
        Write-Host '[NORMALIZE] converting SCRFD export to the decoder convention' -ForegroundColor Yellow
        & $PythonExe $normalizer $rawDetectorPath $detectorPath
        if ($LASTEXITCODE -ne 0) {
            throw "SCRFD normalization failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        Remove-Item -LiteralPath $rawDetectorPath -Force -ErrorAction SilentlyContinue
    }

    if (-not (Test-PinnedFile -Path $detectorPath -ExpectedSize $detector.Size -ExpectedSha256 $detector.Sha256)) {
        throw "Normalized detector did not match the pinned artifact: $detectorPath"
    }
    Write-Host "[OK] $($detector.Name) normalized and verified." -ForegroundColor Green
}

$recognizerPath = Join-Path $ModelsDir $recognizer.Name
if (Test-PinnedFile -Path $recognizerPath -ExpectedSize $recognizer.Size -ExpectedSha256 $recognizer.Sha256) {
    Write-Host "[SKIP] $($recognizer.Name) is already verified." -ForegroundColor Green
}
else {
    Write-Host "[DOWNLOAD] $($recognizer.Name)" -ForegroundColor Yellow
    Download-File -Url $recognizer.Url -Destination $recognizerPath
    if (-not (Test-PinnedFile -Path $recognizerPath -ExpectedSize $recognizer.Size -ExpectedSha256 $recognizer.Sha256)) {
        throw "Recognizer did not match the pinned artifact: $recognizerPath"
    }
    Write-Host "[OK] $($recognizer.Name) verified." -ForegroundColor Green
}

# The shared downloader pins and verifies both calibrated MiniFAS models.
$miniFasDownloader = Join-Path $PSScriptRoot 'download_minifas_models.ps1'
& $miniFasDownloader -Destination $ModelsDir

Write-Host ''
Write-Host 'All four canonical runtime models are verified.' -ForegroundColor Green
Write-Host 'For standalone/service debugging, copy assets\models\*.onnx to the configured DataPath\models directory.'
