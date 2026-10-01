# Stage the canonical legal documents for installation and for both Wails builds.
# Called by npm prebuild, so the license viewer works before installation and
# inside the slim uninstaller without depending on the deployment payload.
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$setup = Join-Path $root 'installer/FaceLoginSetup'
$copies = @(
    @{ source = 'LICENSE'; name = 'LICENSE.txt' },
    @{ source = 'THIRD_PARTY_NOTICES.txt'; name = 'THIRD_PARTY_NOTICES.txt' },
    @{ source = 'third_party/MODEL_LICENSES.txt'; name = 'MODEL_LICENSES.txt' }
)
foreach ($copy in $copies) {
    $source = Join-Path $root $copy.source
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing legal document: $source" }
    foreach ($subdir in @('resources', 'frontend/public')) {
        $dir = Join-Path $setup $subdir
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Copy-Item -LiteralPath $source -Destination (Join-Path $dir $copy.name) -Force
    }
}
