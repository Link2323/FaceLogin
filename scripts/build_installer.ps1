# One-command product installer build. Development tests and diagnostics
# are built/run separately; they are never part of this packaging pipeline.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_installer.ps1
#
#   1. incremental CMake build of the three product targets (serial)
#   2. stage product binaries + the 5 runtime DLLs + slim uninstaller
#   3. prune resources/ against the payload allow-list (dev leftovers such as
#      PadCalibration.exe or build-tree noise DLLs cannot ship silently)
#   4. payload.zip via make_payload.ps1 (absolute paths, as it requires)
#   5. wails build (full), without deleting the preceding build output;
#      success requires an executable newer than this script's start time
param()
$ErrorActionPreference = "Stop"

# Windows permits malformed inherited environment blocks with case variants
# such as PATH/Path. .NET Framework/MSBuild treats those names as identical
# when creating a child process and throws before launching CL.exe.
# Collapse duplicate names in this process only. Preserve all PATH entries.
$buildEnvironment = [Environment]::GetEnvironmentVariables("Process")
$environmentGroups = @{}
foreach ($entry in $buildEnvironment.GetEnumerator()) {
    if (-not $environmentGroups.ContainsKey($entry.Key)) {
        $environmentGroups[$entry.Key] = @()
    }
    $environmentGroups[$entry.Key] += $entry
}
foreach ($group in $environmentGroups.Values) {
    if ($group.Count -lt 2) { continue }
    $name = $group[0].Key
    $value = [Environment]::GetEnvironmentVariable($name, "Process")
    if ($name -ieq "PATH") {
        $pathEntries = @()
        foreach ($candidate in (@($value) + @($group | ForEach-Object { $_.Value }))) {
            foreach ($pathEntry in ($candidate -split ';')) {
                if ($pathEntry -and $pathEntries -notcontains $pathEntry) { $pathEntries += $pathEntry }
            }
        }
        $value = $pathEntries -join ';'
    }
    foreach ($entry in $group) {
        [Environment]::SetEnvironmentVariable($entry.Key, $null, "Process")
    }
    [Environment]::SetEnvironmentVariable($name, $value, "Process")
}

# Windows PowerShell 5.1 cannot autoload its standard modules from an
# inherited PowerShell 7 module path (notably Get-FileHash at completion).
if ($PSVersionTable.PSEdition -eq "Desktop") {
    $env:PSModulePath = @(
        ([IO.Path]::Combine([Environment]::GetFolderPath("MyDocuments"), "WindowsPowerShell/Modules")),
        ([IO.Path]::Combine($env:ProgramFiles, "WindowsPowerShell/Modules")),
        ([IO.Path]::Combine($PSHOME, "Modules"))
    ) -join ';'
}
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

$cmake = Find-Tool "cmake" "C:/vcpkg/downloads/tools/cmake-4.4.0-windows/cmake-4.4.0-windows-x86_64/bin/cmake.exe"
$wails = Find-Tool "wails" (Join-Path $env:USERPROFILE "go/bin/wails.exe")
$npm = Find-Tool "npm.cmd" ""
# Keep npm's mutable cache with build outputs rather than requiring writes
# to a protected user profile. Honor an explicitly configured cache.
if (-not $env:NPM_CONFIG_CACHE) { $env:NPM_CONFIG_CACHE = Join-Path $root "build/npm-cache" }

# Exactly the payload allow-list from docs/BUILD.md — anything else in
# resources/ ends up inside payload.zip and ships.
$rootAllow = @(
    "FaceLoginService.exe", "FaceLoginCredentialProvider.dll", "FaceLoginConsole.exe",
    "uninstall.exe", "onnxruntime.dll", "libprotobuf.dll", "libprotobuf-lite.dll",
    "re2.dll", "abseil_dll.dll",
    "LICENSE.txt", "THIRD_PARTY_NOTICES.txt", "MODEL_LICENSES.txt"
)
$modelAllow = @("det_10g_gnkps.onnx", "face_recognition_sface_2021dec.onnx", "MiniFASNetV2.onnx", "MiniFASNetV1SE.onnx")

$total = [System.Diagnostics.Stopwatch]::StartNew()
function Step([string]$Title, [scriptblock]$Body) {
    Write-Host ""
    Write-Host "==> $Title" -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $Body
    Write-Host ("    done in {0:N1}s" -f $sw.Elapsed.TotalSeconds)
}

Step "1/5 C++ product build (incremental, serial)" {
    & $cmake --build (Join-Path $root "build") --config Release `
        --target FaceLoginService FaceLoginCredentialProvider FaceLoginConsole --parallel 1
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed (exit $LASTEXITCODE)" }
}

$faceRel = Join-Path $root "build/face_service/Release"
$cpRel = Join-Path $root "build/credential_provider/Release"
Step "2/5 stage binaries + slim uninstaller" {
    # Restore the locked frontend packages before collecting their license
    # texts, so stale node_modules cannot label the new bundle with old terms.
    $frontend = Join-Path $inst "frontend"
    Push-Location $frontend
    try {
        & $npm ci --no-audit --no-fund
        if ($LASTEXITCODE -ne 0) {
            throw "npm ci failed (exit $LASTEXITCODE). The build host must allow npm/Node child processes and cache writes; a sandbox spawn EPERM requires host execution permission."
        }
    } finally { Pop-Location }
    & (Join-Path $PSScriptRoot "update_third_party_notices.ps1")
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
    # Model staging must work even when an up-to-date Console does not run
    # its POST_BUILD command. Reuse the canonical files; never download here.
    $modelDir = Join-Path $res "models"
    New-Item -ItemType Directory -Path $modelDir -Force | Out-Null
    foreach ($model in $modelAllow) {
        Copy-Item -LiteralPath (Join-Path $root "assets/models/$model") `
            -Destination (Join-Path $modelDir $model) -Force
    }
    Push-Location $inst
    try {
        # wails writes progress (incl. a benign "Elevation failed" line from
        # bindings generation, 2026-09-10) to stderr; under
        # $ErrorActionPreference=Stop a raw 2>&1 turns the first stderr line
        # into a terminating NativeCommandError. Merge inside cmd instead so
        # PowerShell only sees stdout.
        cmd /c "`"$wails`" build -tags slim -platform windows/amd64 -ldflags `"-s -w`" 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "slim wails build failed (exit $LASTEXITCODE)" }
    } finally { Pop-Location }
    Copy-Item (Join-Path $inst "build/bin/FaceLoginSetup.exe") (Join-Path $res "uninstall.exe") -Force
}

Step "3/5 prune resources/ to payload allow-list" {
    $resourceRoot = [IO.Path]::GetFullPath($res).TrimEnd('\') + '\'
    foreach ($f in Get-ChildItem -LiteralPath $res -File -Recurse) {
        if (-not $f.FullName.StartsWith($resourceRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw "resource path is outside packaging directory: $($f.FullName)"
        }
        $relative = $f.FullName.Substring($resourceRoot.Length).Replace('\', '/')
        if ($rootAllow -notcontains $relative -and
            @($modelAllow | ForEach-Object { "models/$_" }) -notcontains $relative) {
            Write-Host "    pruning: $relative"
            Remove-Item -LiteralPath $f.FullName
        }
    }
    $missing = @()
    foreach ($f in $rootAllow) { if (-not (Test-Path (Join-Path $res $f))) { $missing += $f } }
    foreach ($m in $modelAllow) { if (-not (Test-Path (Join-Path $res "models/$m"))) { $missing += "models/$m" } }
    if ($missing.Count -gt 0) { throw "resources/ incomplete after sync: $($missing -join ', ')" }
}

Step "4/5 pack payload.zip" {
    & (Join-Path $PSScriptRoot "make_payload.ps1") -ResourcesDir $res -Out (Join-Path $inst "payload.zip")
}

$setupExe = Join-Path $inst "build/bin/FaceLoginSetup.exe"
Step "5/5 wails build (full)" {
    Push-Location $inst
    try {
        cmd /c "`"$wails`" build -platform windows/amd64 -ldflags `"-s -w`" 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "full wails build failed (exit $LASTEXITCODE)" }
    } finally { Pop-Location }
    if (-not (Test-Path -LiteralPath $setupExe) -or (Get-Item $setupExe).LastWriteTime -lt $scriptStart) {
        throw "FaceLoginSetup.exe is older than this script's start time - wails produced a stale no-op build"
    }
    # Preserve accompanying legal material when publishing the build directory.
    foreach ($name in @("LICENSE.txt", "THIRD_PARTY_NOTICES.txt", "MODEL_LICENSES.txt")) {
        Copy-Item -LiteralPath (Join-Path $res $name) -Destination (Join-Path $inst "build/bin/$name") -Force
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
