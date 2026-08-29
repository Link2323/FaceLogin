# Packs installer/FaceLoginSetup/resources into payload.zip for
# //go:embed payload.zip.
#
# Entries are created MANUALLY (not via ZipFile.CreateFromDirectory): the .NET
# helper writes Windows backslash separators in entry names, but the Go side
# reads the zip through io/fs, whose spec requires forward slashes — a
# backslash zip breaks fs.ReadDir/Open silently. Explicit directory entries
# are also required: fs.FS can only enumerate directories that exist as
# entries. Run from the repository root before every full wails build.
param(
    [string]$ResourcesDir = "installer/FaceLoginSetup/resources",
    [string]$Out = "installer/FaceLoginSetup/payload.zip"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not (Test-Path $ResourcesDir)) {
    Write-Error "resources dir not found: $ResourcesDir"
}
$dir = (Resolve-Path $ResourcesDir).Path
$outPath = (Join-Path (Resolve-Path (Split-Path $Out -Parent)).Path (Split-Path $Out -Leaf))
if (Test-Path $outPath) { Remove-Item $outPath }

$before = (Get-ChildItem -Recurse -File $dir | Measure-Object Length -Sum).Sum
$zip = [System.IO.Compression.ZipFile]::Open($outPath, 'Create')
try {
    $dirs = @("")
    foreach ($file in Get-ChildItem -Recurse -File $dir | Sort-Object FullName) {
        # PS 5.1 / .NET Framework has no Path.GetRelativePath — strip the
        # known "$dir\" prefix instead.
        $rel = $file.FullName.Substring($dir.Length + 1) -replace '\\', '/'
        # Create every ancestor as an explicit directory entry (with trailing /).
        $parts = $rel -split '/'
        for ($i = 0; $i -lt $parts.Count - 1; $i++) {
            $d = ($parts[0..$i] -join '/') + '/'
            if ($dirs -notcontains $d) {
                [void]$zip.CreateEntry("resources/$d")
                $dirs += $d
            }
        }
        $entry = $zip.CreateEntry("resources/$rel", [System.IO.Compression.CompressionLevel]::Optimal)
        $inStream = $file.OpenRead()
        try {
            $outStream = $entry.Open()
            try { $inStream.CopyTo($outStream) } finally { $outStream.Dispose() }
        } finally { $inStream.Dispose() }
    }
} finally {
    $zip.Dispose()
}

$after = (Get-Item $outPath).Length
Write-Host ("payload.zip: {0:N1} MB -> {1:N1} MB" -f ($before / 1MB), ($after / 1MB))
