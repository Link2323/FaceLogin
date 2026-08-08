param(
    [string]$Destination = (Join-Path (Split-Path -Parent $PSScriptRoot) 'assets\models')
)

$ErrorActionPreference = 'Stop'

# Canonical production PAD models. FaceLogin uses both models sequentially and
# fuses their real-face probabilities 50/50; never substitute one model alone.
# Source: https://github.com/yakhyo/face-anti-spoofing/releases/tag/weights
$models = @(
    @{
        Name = 'MiniFASNetV2.onnx'
        Url = 'https://github.com/yakhyo/face-anti-spoofing/releases/download/weights/MiniFASNetV2.onnx'
        Size = 1743581
        Sha256 = 'b32929adc2d9c34b9486f8c4c7bc97c1b69bc0ea9befefc380e4faae4e463907'
    },
    @{
        Name = 'MiniFASNetV1SE.onnx'
        Url = 'https://github.com/yakhyo/face-anti-spoofing/releases/download/weights/MiniFASNetV1SE.onnx'
        Size = 1742335
        Sha256 = 'ebab7f90c7833fbccd46d3a555410e78d969db5438e169b6524be444862b3676'
    }
)

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

foreach ($model in $models) {
    $target = Join-Path $Destination $model.Name
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $item = Get-Item -LiteralPath $target
        $hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($item.Length -eq $model.Size -and $hash -eq $model.Sha256) {
            Write-Host "[SKIP] Verified $($model.Name): $hash" -ForegroundColor Green
            continue
        }
        Write-Host "[REPLACE] $($model.Name) did not match the pinned release." -ForegroundColor Yellow
    }

    $temporary = "$target.download"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $model.Url -OutFile $temporary
        $item = Get-Item -LiteralPath $temporary
        $hash = (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($item.Length -ne $model.Size) {
            throw "$($model.Name) size is $($item.Length), expected $($model.Size)"
        }
        if ($hash -ne $model.Sha256) {
            throw "$($model.Name) SHA-256 is $hash, expected $($model.Sha256)"
        }
        Move-Item -LiteralPath $temporary -Destination $target -Force
        Write-Host "[READY] Verified $($model.Name): $hash" -ForegroundColor Green
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

Write-Host "Dual MiniFAS production models are ready in $Destination"
