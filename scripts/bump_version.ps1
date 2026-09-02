# Sync the version string across ALL 6 locations (docs/BUILD.md "版本号与版本资源"):
#   in git    : CMakeLists.txt project(VERSION), wails.json Info, main.go appVersion, README badge
#   disk-only : AGENTS.md overview, installer/FaceLoginSetup/build/windows/info.json (gitignored)
#
#   powershell -ExecutionPolicy Bypass -File scripts/bump_version.ps1 1.8.1-multi-angle
#   add -Dry to preview every replacement without writing.
#
# Numeric part (x.y.z) goes into exe/registry version resources; the -suffix
# only shows up in the README badge, AGENTS.md and the release tag. Every
# pattern is count-validated: a missed location fails loudly instead of
# silently drifting.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$Dry
)
$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

if ($Version -notmatch '^(?<num>\d+\.\d+\.\d+)(?:-(?<suf>[A-Za-z0-9.-]+))?$') {
    throw "bad version '$Version' - expected x.y.z or x.y.z-suffix (e.g. 1.8.1-multi-angle)"
}
$num = $Matches['num']
$display = if ($Matches['suf']) { "$num-$($Matches['suf'])" } else { $num }
$root = Split-Path $PSScriptRoot -Parent

# README badge text: shields.io escaping ('-' -> '--', ' ' -> '%20'), trailing fork note kept.
$badge = ($display + " (Link2323 fork)").Replace('-', '--').Replace(' ', '%20')

function Update-File {
    param([string]$Path, [object[]]$Rules)
    $full = Join-Path $root $Path
    if (-not (Test-Path $full)) { throw "not found: $full" }
    $bytes = [System.IO.File]::ReadAllBytes($full)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $text = [System.IO.File]::ReadAllText($full)
    foreach ($r in $Rules) {
        $count = [regex]::Matches($text, $r.Pattern).Count
        if ($count -ne $r.Expect) {
            throw "$Path : pattern '$($r.Pattern)' matched $count times, expected $($r.Expect)"
        }
        $old = [regex]::Match($text, $r.Pattern).Value
        $text = [regex]::Replace($text, $r.Pattern, $r.Replacement)
        Write-Host "  $Path"
        Write-Host "    - $old"
        Write-Host "    + $($r.Replacement)"
    }
    if (-not $Dry) {
        $enc = New-Object System.Text.UTF8Encoding($hasBom)
        [System.IO.File]::WriteAllText($full, $text, $enc)
    }
}

Write-Host "version: $Version  (numeric=$num, display=$display)$(if ($Dry) { '  [DRY RUN]' })"

Update-File "CMakeLists.txt" @(
    @{ Pattern = 'project\(FaceLogin VERSION \d+\.\d+\.\d+'; Replacement = "project(FaceLogin VERSION $num"; Expect = 1 }
)

Update-File "installer/FaceLoginSetup/wails.json" @(
    @{ Pattern = '"productVersion":\s*"[\d.]+"'; Replacement = "`"productVersion`": `"$num`""; Expect = 1 },
    @{ Pattern = '"fileVersion":\s*"[\d.]+"'; Replacement = "`"fileVersion`": `"$num`""; Expect = 1 }
)

Update-File "installer/FaceLoginSetup/main.go" @(
    @{ Pattern = 'const appVersion = "[^"]+"'; Replacement = "const appVersion = `"$num`""; Expect = 1 }
)

Update-File "README.md" @(
    @{ Pattern = 'version-[^" ]*?-green'; Replacement = "version-$badge-green"; Expect = 1 }
)

Update-File "AGENTS.md" @(
    @{ Pattern = '\*\*Windows 专属桌面应用\*\*\([0-9][^)]*\)'; Replacement = "**Windows 专属桌面应用**($display)"; Expect = 1 }
)

Update-File "installer/FaceLoginSetup/build/windows/info.json" @(
    @{ Pattern = '"file_version":\s*"[\d.]+"'; Replacement = "`"file_version`": `"$num`""; Expect = 1 },
    @{ Pattern = '"ProductVersion":\s*"[\d.]+"'; Replacement = "`"ProductVersion`": `"$num`""; Expect = 1 },
    @{ Pattern = '"FileVersion":\s*"[\d.]+"'; Replacement = "`"FileVersion`": `"$num`""; Expect = 1 }
)

Write-Host ""
Write-Host "notes:"
Write-Host "  - AGENTS.md and info.json are gitignored (disk-only); the other 4 changes are committable."
Write-Host "  - run scripts/build_installer.ps1 to bake the new version into the binaries/installer."
Write-Host "  - release tag: v$display"
