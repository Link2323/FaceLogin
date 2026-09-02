# One-command full installer rebuild. Chains docs/BUILD.md "Full Installer
# Rebuild" end to end and absorbs the manual chain's known traps:
#
#   powershell -ExecutionPolicy Bypass -File scripts/build_installer.ps1
#
#   1. incremental CMake build
#   2. stage product binaries + the 5 runtime DLLs + slim uninstaller
#   3. prune resources/ against the payload allow-list (dev leftovers such as
#      PadCalibration.exe or build-tree noise DLLs cannot ship silently)
#   4. payload.zip via make_payload.ps1 (absolute paths, as it requires)
#   5. go test ./... payload validation
#   6. wails build (full); a failed -clean attempt (transient Defender lock
#      that deletes the old exe and produces nothing) is retried once without
#      -clean, and success requires the exe to be NEWER than script start,
#      which guards the no-op wails build trap
param()
$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

$root = Split-Path $PSScriptRoot -Parent
$inst = Join-Path $root "installer/FaceLoginSetup"
$res = Join-Path $inst "resources"
$scriptStart = Get-Date

function Find-Tool([string]$Name, [string]$Fallback) {
    $c = Get-Command $Name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    if ($Fallback -and (Test-Path $Fallback)) { return $Fallback }
    throw "$Name not found on PATH (fallback $Fallback missing)"
}

$cmake = Find-Tool "cmake" "C:/vcpkg/downloads/tools/cmake-4.4.0-windows/cmake-4.4.0-windows-x64/bin/cmake.exe"
$wails = Find-Tool "wails" (Join-Path $env:USERPROFILE "go/bin/wails.exe")
$go = Find-Tool "go" "C:/Program Files/Go/bin/go.exe"

# Exactly the payload allow-list from docs/BUILD.md — anything else in
# resources/ ends up inside payload.zip and ships.
$rootAllow = @(
    "FaceLoginService.exe", "FaceLoginCredentialProvider.dll", "FaceLoginConsole.exe",
    "uninstall.exe", "onnxruntime.dll", "libprotobuf.dll", "libprotobuf-lite.dll",
    "re2.dll", "abseil_dll.dll"
)
$modelAllow = @("det_10g_gnkps.onnx", "w600k_r50.onnx", "MiniFASNetV2.onnx", "MiniFASNetV1SE.onnx")

$total = [System.Diagnostics.Stopwatch]::StartNew()
function Step([string]$Title, [scriptblock]$Body) {
    Write-Host ""
    Write-Host "==> $Title" -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $Body
    Write-Host ("    done in {0:N1}s" -f $sw.Elapsed.TotalSeconds)
}

Step "1/6 C++ build (incremental)" {
    & $cmake --build (Join-Path $root "build") --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed (exit $LASTEXITCODE)" }
}

$faceRel = Join-Path $root "build/face_service/Release"
$cpRel = Join-Path $root "build/credential_provider/Release"
Step "2/6 stage binaries + slim uninstaller" {
    # Provider DLL builds to credential_provider/Release; everything else to
    # face_service/Release — do NOT glob *.dll (that dir also holds build-tree
    # noise outside the runtime closure).
    $copyPlan = @(
        @{ Src = Join-Path $faceRel "FaceLoginService.exe"; Dst = "FaceLoginService.exe" },
        @{ Src = Join-Path $cpRel "FaceLoginCredentialProvider.dll"; Dst = "FaceLoginCredentialProvider.dll" },
        @{ Src = Join-Path $faceRel "onnxruntime.dll"; Dst = "onnxruntime.dll" },
        @{ Src = Join-Path $faceRel "libprotobuf.dll"; Dst = "libprotobuf.dll" },
        @{ Src = Join-Path $faceRel "libprotobuf-lite.dll"; Dst = "libprotobuf-lite.dll" },
        @{ Src = Join-Path $faceRel "re2.dll"; Dst = "re2.dll" },
        @{ Src = Join-Path $faceRel "abseil_dll.dll"; Dst = "abseil_dll.dll" }
    )
    foreach ($c in $copyPlan) {
        if (-not (Test-Path $c.Src)) { throw "missing build output: $($c.Src)" }
        Copy-Item $c.Src (Join-Path $res $c.Dst) -Force
    }
    # FaceLoginConsole.exe outputs directly into resources/ via its CMake
    # RUNTIME_OUTPUT_DIRECTORY — never copied, only checked.
    if (-not (Test-Path (Join-Path $res "FaceLoginConsole.exe"))) {
        throw "FaceLoginConsole.exe missing from resources/ (CMake output dir)"
    }
    Push-Location $inst
    try {
        & $wails build -tags slim -platform windows/amd64 -ldflags "-s -w" 2>&1 |
            ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0) { throw "slim wails build failed (exit $LASTEXITCODE)" }
    } finally { Pop-Location }
    Copy-Item (Join-Path $inst "build/bin/FaceLoginSetup.exe") (Join-Path $res "uninstall.exe") -Force
}

Step "3/6 prune resources/ to payload allow-list" {
    foreach ($f in Get-ChildItem -File $res) {
        if ($rootAllow -notcontains $f.Name) {
            Write-Host "    pruning: $($f.Name)"
            Remove-Item $f.FullName
        }
    }
    foreach ($f in Get-ChildItem -File (Join-Path $res "models")) {
        if ($modelAllow -notcontains $f.Name) {
            Write-Host "    pruning models/: $($f.Name)"
            Remove-Item $f.FullName
        }
    }
    $missing = @()
    foreach ($f in $rootAllow) { if (-not (Test-Path (Join-Path $res $f))) { $missing += $f } }
    foreach ($m in $modelAllow) { if (-not (Test-Path (Join-Path $res "models/$m"))) { $missing += "models/$m" } }
    if ($missing.Count -gt 0) { throw "resources/ incomplete after sync: $($missing -join ', ')" }
}

Step "4/6 pack payload.zip" {
    & (Join-Path $PSScriptRoot "make_payload.ps1") -ResourcesDir $res -Out (Join-Path $inst "payload.zip")
}

Step "5/6 go test (payload validation)" {
    Push-Location $inst
    try {
        & $go test ./...
        if ($LASTEXITCODE -ne 0) { throw "go test failed (exit $LASTEXITCODE)" }
    } finally { Pop-Location }
}

$setupExe = Join-Path $inst "build/bin/FaceLoginSetup.exe"
Step "6/6 wails build (full)" {
    Push-Location $inst
    try {
        for ($attempt = 1; ; $attempt++) {
            Write-Host "    attempt $attempt"
            if ($attempt -eq 1) {
                & $wails build -clean -platform windows/amd64 -ldflags "-s -w" 2>&1 |
                    ForEach-Object { "$_" } | Tee-Object -Variable wailsOut
            } else {
                & $wails build -platform windows/amd64 -ldflags "-s -w" 2>&1 |
                    ForEach-Object { "$_" } | Tee-Object -Variable wailsOut
            }
            if ($LASTEXITCODE -eq 0 -and (Test-Path $setupExe)) { break }
            if ($attempt -ge 2) {
                $tail = ($wailsOut | Select-Object -Last 15) -join "`n"
                throw "wails build failed twice (exit $LASTEXITCODE). Last output:`n$tail"
            }
            Write-Host "    failed (likely the transient -clean file lock); retrying without -clean..." -ForegroundColor Yellow
            Start-Sleep -Seconds 2
        }
    } finally { Pop-Location }
    if ((Get-Item $setupExe).LastWriteTime -lt $scriptStart) {
        throw "FaceLoginSetup.exe is older than this script's start time - wails produced a stale no-op build"
    }
}

$size = (Get-Item $setupExe).Length / 1MB
$hash = (Get-FileHash $setupExe -Algorithm SHA256).Hash
$ver = (Get-Item $setupExe).VersionInfo.ProductVersion
Write-Host ""
Write-Host "==== installer ready ====" -ForegroundColor Green
Write-Host "  path    : $setupExe"
Write-Host ("  size    : {0:N1} MB" -f $size)
Write-Host "  sha256  : $hash"
if ($ver) { Write-Host "  version : $ver" }
else { Write-Host "  version : <empty ProductVersion - check build/windows/info.json lang key is 0409>" -ForegroundColor Yellow }
Write-Host ("  total   : {0:N1}s" -f $total.Elapsed.TotalSeconds)
