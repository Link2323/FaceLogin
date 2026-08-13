# Restores the gitignored WebView2 loader static library.
#
# enrollment_app/webview2/lib/WebView2LoaderStatic.lib is ignored by git
# (*.lib) and therefore missing on a fresh clone; FaceLoginConsole then fails
# to link with LNK1181. This script is the one canonical restore path —
# docs/BUILD.md points here instead of repeating the curl/unzip commands.
#
# Usage (from the repository root):
#   powershell -ExecutionPolicy Bypass -File scripts\restore_webview2.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\restore_webview2.ps1 -Version 1.0.3124.44
#
# Idempotent: exits 0 immediately when the library is already present.
param(
    [string]$Version = "1.0.3124.44"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$libDir = Join-Path $root "enrollment_app\webview2\lib"
$target = Join-Path $libDir "WebView2LoaderStatic.lib"

if (Test-Path $target) {
    Write-Host "OK: $target already present ($((Get-Item $target).Length) bytes)"
    exit 0
}

$includeDir = Join-Path $root "enrollment_app\webview2\include"
if (-not (Test-Path (Join-Path $includeDir "WebView2.h"))) {
    Write-Warning "enrollment_app\webview2\include\WebView2.h is missing too; this script only restores the .lib, not the NuGet headers. Check the webview2 directory."
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("webview2-restore-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $url = "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$Version"
    Write-Host "Downloading $url ..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $nupkg = Join-Path $temp "webview2.nupkg"
    Invoke-WebRequest -Uri $url -OutFile $nupkg -UseBasicParsing

    $extract = Join-Path $temp "pkg"
    # Expand-Archive only accepts .zip extensions; the .nupkg is a zip.
    $zip = Join-Path $temp "webview2.zip"
    Copy-Item -Path $nupkg -Destination $zip
    Expand-Archive -Path $zip -DestinationPath $extract

    $source = Join-Path $extract "build\native\x64\WebView2LoaderStatic.lib"
    if (-not (Test-Path $source)) {
        throw "Package layout changed: $source not found in Microsoft.Web.WebView2 $Version"
    }
    Copy-Item -Path $source -Destination $target
    Write-Host "OK: restored $target ($((Get-Item $target).Length) bytes) from Microsoft.Web.WebView2 $Version"
} finally {
    Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
}
