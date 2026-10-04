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
    # Publisher's Apache-2.0 declaration and provenance: docs/model-licenses.md.
    # Pin the source revision as well as the original bytes; never infer weight
    # terms from the SCRFD architecture name or from InsightFace's code license.
    Url = 'https://hf-mirror.com/kunkunlin1221/face-detection_scrfd-10g-gnkps/resolve/eb0e349519cd951b2a9423dac45b39e8ce5a71b8/scrfd_10g_gnkps_fp32.onnx'
    RawSize = 16273449
    RawSha256 = '2112D066C1DCE6CC648670E69CF90561B9287BB1945153F3B461A487131255B9'
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
# Pinned OpenCV SFace FP32 export; remove initializer graph inputs only.
# Pixel normalization remains inside ONNX; weights/nodes are unchanged.
$recognizer = @{
    Name = 'face_recognition_sface_2021dec.onnx'
    Url = 'https://media.githubusercontent.com/media/opencv/opencv_zoo/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_recognition_sface/face_recognition_sface_2021dec.onnx'
    RawSize = 38696353
    RawSha256 = '0BA9FBFA01B5270C96627C4EF784DA859931E02F04419C829E83484087C34E79'
    Size = 38688787
    Sha256 = 'AE6A6AC44D2BDC87924E75FB23D8212430DD24F037F5E035C21DEFF99AFC8B61'
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
            if (-not (Test-PinnedFile -Path $rawDetectorPath -ExpectedSize $detectorRaw.RawSize -ExpectedSha256 $detectorRaw.RawSha256)) {
                throw "Raw detector did not match the pinned publisher artifact: $rawDetectorPath"
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

$recognizerPath = Join-Path $ModelsDir $recognizer.Name
if (Test-PinnedFile -Path $recognizerPath -ExpectedSize $recognizer.Size -ExpectedSha256 $recognizer.Sha256) {
    Write-Host "[SKIP] $($recognizer.Name) is already verified." -ForegroundColor Green
} else {
    # Stage away from production directories; install only the fully checked
    # derivative. Reuse a verified original already on disk without download.
    $recognizerStage = Join-Path ([System.IO.Path]::GetTempPath()) ("FaceLogin-SFace-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $recognizerStage | Out-Null
    $rawRecognizerPath = Join-Path $recognizerStage 'upstream.onnx'
    $normalizedRecognizerPath = Join-Path $recognizerStage 'normalized.onnx'
    try {
        if (Test-PinnedFile -Path $recognizerPath -ExpectedSize $recognizer.RawSize -ExpectedSha256 $recognizer.RawSha256) {
            Copy-Item -LiteralPath $recognizerPath -Destination $rawRecognizerPath
        } else {
            Download-File -Url $recognizer.Url -Destination $rawRecognizerPath
        }
        if (-not (Test-PinnedFile -Path $rawRecognizerPath -ExpectedSize $recognizer.RawSize -ExpectedSha256 $recognizer.RawSha256)) {
            throw 'Raw SFace recognizer did not match the pinned OpenCV artifact.'
        }
        $normalizer = Join-Path $PSScriptRoot 'normalize_sface_export.py'
        & $PythonExe $normalizer $rawRecognizerPath $normalizedRecognizerPath
        if ($LASTEXITCODE -ne 0) { throw "SFace normalization failed with exit code $LASTEXITCODE." }
        if (-not (Test-PinnedFile -Path $normalizedRecognizerPath -ExpectedSize $recognizer.Size -ExpectedSha256 $recognizer.Sha256)) {
            throw 'Normalized SFace recognizer did not match the pinned runtime artifact.'
        }
        Move-Item -LiteralPath $normalizedRecognizerPath -Destination $recognizerPath -Force
    } finally {
        Remove-Item -LiteralPath $rawRecognizerPath,$normalizedRecognizerPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $recognizerStage -Force -ErrorAction SilentlyContinue
    }
}

# The shared downloader pins and verifies both calibrated MiniFAS models.
$miniFasDownloader = Join-Path $PSScriptRoot 'download_minifas_models.ps1'
& $miniFasDownloader -Destination $ModelsDir

Write-Host ''
Write-Host 'All four canonical runtime models are verified.' -ForegroundColor Green
Write-Host 'For standalone/service debugging, copy assets\models\*.onnx to the configured DataPath\models directory.'
