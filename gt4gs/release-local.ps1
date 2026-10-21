# Builds, packages and publishes a gt4-gs release from this PC (owner decision 2026-10-16; GitHub Actions is a manual
# fallback only, see .github/workflows/gt4gs-windows.yml). Anyone can rebuild a release from its tag with BUILD.md.
#
# Usage (PowerShell, repository root, the release tag checked out, clean tree):
#   powershell -File gt4gs\release-local.ps1 -Tag gt4gs-v0.3.1 -Build <build dir> -Deps <deps dir> -OutDir <dir>
#     [-Release] [-Draft] [-NoPublish]
#
# Steps: refuse a dirty tree or a HEAD that is not exactly -Tag; configure (if needed) and build pcsx2-gt4gs Release with
# the local recipe of BUILD.md (VS Build Tools vcvars64, Ninja, no LTO, GT4GS_ONLY=ON (JIT-free, since 0.4.0), the
# dependencies in -Deps); package the CI zip layout (gt4-gs.exe, DLLs, resources, COPYING.GPLv3, NOTICE, README.md,
# SOURCE.txt with tag, commit and tool versions); refuse the package unless gt4gs/check-nojit.ps1 passes on it;
# make the source archive with `git archive` of the tag (GPLv3 section 6(d)); write SHA256SUMS; publish with
# `gh release create` (a prerelease unless -Release) with the three assets and notes. -Draft creates the release as a
# draft (not public; the owner publishes it after the in-game test; the launcher ignores drafts). -NoPublish stops before
# gh.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [Parameter(Mandatory = $true)][string]$Build,
    [Parameter(Mandatory = $true)][string]$Deps,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [switch]$Release,
    [switch]$Draft,
    [switch]$NoPublish
)
$ErrorActionPreference = 'Stop'
$repo = (git rev-parse --show-toplevel).Trim()
Set-Location $repo

# 1. Clean tree, tag checked out.
$dirty = git status --porcelain --untracked-files=no
if ($dirty) { throw "The working tree is not clean:`n$dirty" }
$headTag = (git describe --exact-match --tags HEAD 2>$null)
if ($LASTEXITCODE -ne 0 -or ($headTag -split "`n") -notcontains $Tag) { throw "HEAD is not the tag $Tag (check it out first)" }
if (-not $Tag.StartsWith('gt4gs-v')) { throw "The tag must be gt4gs-v<version>" }
$version = $Tag.Substring('gt4gs-v'.Length)
$commit = (git rev-parse HEAD).Trim()

# 2. Build (Visual Studio Build Tools found with vswhere, like CI).
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$configure = ''
if (-not (Test-Path (Join-Path $Build 'build.ninja'))) {
    $configure = "cmake -S `"$repo`" -B `"$Build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl " +
        "-DENABLE_QT_UI=OFF -DENABLE_GSRUNNER=OFF -DENABLE_GT4GS=ON -DGT4GS_ONLY=ON -DENABLE_TESTS=OFF -DDISABLE_ADVANCE_SIMD=ON " +
        "`"-DCMAKE_PREFIX_PATH=$Deps`" && "
}
cmd /c "call `"$vcvars`" >nul && $configure cmake --build `"$Build`" --target pcsx2-gt4gs"
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
# An existing build directory must be a JIT-free configuration too.
if (-not (Select-String -Quiet -SimpleMatch 'GT4GS_ONLY:BOOL=ON' (Join-Path $Build 'CMakeCache.txt'))) { throw "$Build is not configured with GT4GS_ONLY=ON" }

# Tool versions and the dependency script hash, for SOURCE.txt and the notes.
# Version numbers only (the cl banner is localized). vcvars may print vswhere noise on stderr: discarded.
$clBanner = (cmd /c "call `"$vcvars`" >nul 2>nul && cl 2>&1" | Select-Object -First 1)
$compiler = if ($clBanner -match '(\d+\.\d+\.\d+)') { "MSVC cl $($Matches[1]) x64 ($((Split-Path -Leaf $vs)))" } else { 'MSVC cl (unknown version)' }
$cmakeVersion = ((cmd /c "call `"$vcvars`" >nul 2>nul && cmake --version" | Select-Object -First 1) -replace 'cmake version ', '').Trim()
$ninjaVersion = (cmd /c "call `"$vcvars`" >nul 2>nul && ninja --version" | Select-Object -First 1).Trim()
$depsScript = '.github/workflows/scripts/windows/build-dependencies-gt4gs.bat'
$depsHash = (Get-FileHash -Algorithm SHA256 (Join-Path $repo $depsScript)).Hash.ToLower()

# 3. Package (same layout as package-gt4gs.ps1 / CI).
$name = "gt4-gs-$version-windows-x64"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stage = Join-Path $OutDir $name
if (Test-Path $stage) { throw "$stage exists; use an empty -OutDir" }
New-Item -ItemType Directory -Path $stage | Out-Null
$exe = Get-ChildItem -Path $Build -Recurse -Filter 'gt4-gs.exe' | Select-Object -First 1
if (-not $exe) { throw "gt4-gs.exe not found under $Build" }
Copy-Item $exe.FullName (Join-Path $stage 'gt4-gs.exe')
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
Copy-Item -Recurse (Join-Path $repo 'bin/resources') (Join-Path $stage 'resources')
Copy-Item (Join-Path $repo 'COPYING.GPLv3') $stage
Copy-Item (Join-Path $repo 'gt4gs/NOTICE') $stage
Copy-Item (Join-Path $repo 'gt4gs/README.md') $stage
$toolLines = @(
    "Built on: owner PC with gt4gs/release-local.ps1 (local release flow; CI is a manual fallback)",
    "Compiler: $compiler",
    "CMake: $cmakeVersion",
    "Ninja: $ninjaVersion",
    "Build options: Release, Ninja, ENABLE_QT_UI=OFF, ENABLE_GT4GS=ON, GT4GS_ONLY=ON (JIT-free), DISABLE_ADVANCE_SIMD=ON, no LTO",
    "Dependency script: $depsScript sha256 $depsHash"
)
$source = @(
    "GT4Recomp GS process (gt4-gs.exe) $version",
    "Source repository: https://github.com/moesuito/gt4recomp-gs",
    "Source ref: $Tag",
    "Source commit: $commit",
    "Based on: PCSX2 tag v2.8.2, commit fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3",
    "Binary: gt4-gs.exe from pcsx2-gt4gs (GSRunner frontend plus the GS stream consumer; no GT4 game code)"
) + $toolLines + @("Bundled DLLs: " + ($dllNames -join ', '))
Set-Content -Path (Join-Path $stage 'SOURCE.txt') -Value $source -Encoding ASCII
# No-JIT check of the staged package (markers, section flags, Arbitrary Code Guard import); the report is kept next to
# the zip.
& powershell -NoProfile -File (Join-Path $repo 'gt4gs/check-nojit.ps1') -Path $stage -Report (Join-Path $OutDir 'check-nojit.json')
if ($LASTEXITCODE -ne 0) { throw 'check-nojit.ps1 failed on the package' }
$zip = Join-Path $OutDir "$name.zip"
Compress-Archive -Path $stage -DestinationPath $zip
Remove-Item -Recurse -Force $stage

# 4. Source archive of the tag (GPLv3 6(d): the corresponding source next to the binary).
$sourceZip = Join-Path $OutDir "gt4recomp-gs-$version-source.zip"
git archive --format=zip --prefix="gt4recomp-gs-$version/" -o $sourceZip $Tag
if ($LASTEXITCODE -ne 0) { throw 'git archive failed' }

# 5. SHA256SUMS (sha256sum format, LF line endings).
$sums = foreach ($file in @($zip, $sourceZip)) {
    "$((Get-FileHash -Algorithm SHA256 $file).Hash.ToLower())  $(Split-Path -Leaf $file)"
}
$sumsPath = Join-Path $OutDir 'SHA256SUMS'
[System.IO.File]::WriteAllText($sumsPath, (($sums -join "`n") + "`n"), [System.Text.Encoding]::ASCII)
Get-Content $sumsPath

# 6. Publish.
$notes = @(
    "GT4Recomp GS process (gt4-gs.exe) $version, built from PCSX2 v2.8.2 (GPL-3.0+) at commit $commit.",
    "Built and published from the owner's PC with gt4gs/release-local.ps1; GitHub Actions is a manual fallback only.",
    "Anyone can rebuild it from this tag with gt4gs/BUILD.md; the source archive is attached (GPLv3 section 6(d)).",
    "Verify the downloads with SHA256SUMS.",
    ""
) + $toolLines
$notesPath = Join-Path $OutDir 'NOTES.md'
Set-Content -Path $notesPath -Value $notes -Encoding UTF8
if ($NoPublish) { Write-Host "Not published (-NoPublish). Assets in $OutDir"; exit 0 }
$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if (-not $gh) { $gh = 'C:\Program Files\GitHub CLI\gh.exe' }
# The repository of the remote "origin" (this checkout also has the PCSX2 upstream, which gh would pick by default).
$origin = (git remote get-url origin).Trim()
if ($origin -notmatch 'github\.com[:/](.+?)(\.git)?$') { throw "origin is not a GitHub repository: $origin" }
$ghRepo = $Matches[1]
$ghArgs = @('release', 'create', '--repo', $ghRepo, $Tag, $zip, $sourceZip, $sumsPath, '--title', "gt4-gs $version", '--notes-file', $notesPath)
if (-not $Release) { $ghArgs += '--prerelease' }
if ($Draft) { $ghArgs += '--draft' }
& $gh @ghArgs
if ($LASTEXITCODE -ne 0) { throw 'gh release create failed' }
if ($Draft) { Write-Host "Created draft release $Tag (not published)" } else { Write-Host "Published $Tag" }
