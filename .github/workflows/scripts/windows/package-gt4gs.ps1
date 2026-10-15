# Packages gt4-gs.exe for a GitHub release (see gt4gs/BUILD.md).
# Usage: package-gt4gs.ps1 -Version <ver> -Build <build dir> -Deps <deps dir> -OutDir <dir>
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Build,
    [Parameter(Mandatory = $true)][string]$Deps,
    [Parameter(Mandatory = $true)][string]$OutDir
)
$ErrorActionPreference = 'Stop'
$repo = (Get-Location).Path
$name = "gt4-gs-$Version-windows-x64"

$exe = Get-ChildItem -Path $Build -Recurse -Filter 'pcsx2-gsrunner.exe' | Select-Object -First 1
if (-not $exe) { throw "pcsx2-gsrunner.exe not found under $Build" }

$stage = Join-Path $OutDir $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Renamed for the package; the binary itself is the stock GSRunner (task 033).
Copy-Item $exe.FullName (Join-Path $stage 'gt4-gs.exe')

# Runtime DLLs: those next to the built exe and the dependency bin folder.
$dllNames = @()
foreach ($dir in @($exe.DirectoryName, (Join-Path $Deps 'bin'))) {
    if (Test-Path $dir) {
        Get-ChildItem -Path $dir -Filter '*.dll' -File | ForEach-Object {
            if (-not (Test-Path (Join-Path $stage $_.Name))) {
                Copy-Item $_.FullName $stage
                $dllNames += $_.Name
            }
        }
    }
}

# Resources (shaders, GameIndex.yaml, fonts). These are PCSX2 resources, not game data.
Copy-Item -Recurse (Join-Path $repo 'bin/resources') (Join-Path $stage 'resources')

Copy-Item (Join-Path $repo 'COPYING.GPLv3') $stage
Copy-Item (Join-Path $repo 'gt4gs/NOTICE') $stage
Copy-Item (Join-Path $repo 'gt4gs/README.md') $stage

$commit = (git rev-parse HEAD).Trim()
$ref = if ($env:GITHUB_REF_NAME) { $env:GITHUB_REF_NAME } else { 'local' }
$source = @(
    "GT4Recomp GS process (gt4-gs.exe) $Version",
    "Source repository: https://github.com/moesuito/gt4recomp-gs",
    "Source ref: $ref",
    "Source commit: $commit",
    "Based on: PCSX2 tag v2.8.2, commit fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3",
    "Binary: pcsx2-gsrunner renamed to gt4-gs.exe (no GT4 code)",
    "Bundled DLLs: " + ($dllNames -join ', ')
)
Set-Content -Path (Join-Path $stage 'SOURCE.txt') -Value $source -Encoding ASCII

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip

$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
Set-Content -Path (Join-Path $OutDir 'SHA256SUMS') -Value "$hash  $(Split-Path -Leaf $zip)" -Encoding ASCII
Remove-Item -Recurse -Force $stage
Write-Host "Packaged $zip ($((Get-Item $zip).Length) bytes) sha256 $hash"
