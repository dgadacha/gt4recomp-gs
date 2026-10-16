# Static no-JIT check for a gt4-gs package (TASK-042). Scans gt4-gs.exe and every DLL next to it.
#
# Usage (Windows PowerShell 5 or PowerShell 7):
#   powershell -File gt4gs\check-nojit.ps1 -Path <package dir or gt4-gs.exe> [-Report <file.json>]
#
# Checks, per module:
#   1. Markers of code generators in the file bytes: xbyak RTTI names, the GS software JIT classes and messages, the
#      microVU / EE / IOP / VIF recompiler classes and messages, the x86 emitter, and source paths of pcsx2\x86,
#      common\emitter and 3rdparty\xbyak (assert file names). Any marker is a failure.
#   2. dumpbin /headers: no section may be both writable and executable (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE);
#      gt4-gs.exe must be NX compatible.
#   3. dumpbin /imports: memory APIs that can create executable memory (VirtualAlloc*, VirtualProtect*,
#      MapViewOfFile*, CreateFileMapping*, FlushInstructionCache) are listed for the record. They are not failures by
#      themselves: the runtime check (Arbitrary Code Guard, gt4-gs.exe log) covers what they are called with.
#   4. gt4-gs.exe must import SetProcessMitigationPolicy and contain the "GT4GS no-JIT" log text (the build enables
#      Arbitrary Code Guard).
# Exit code 0 when every check passes, 1 otherwise. dumpbin comes from Visual Studio (found with vswhere).
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$Report = ''
)
$ErrorActionPreference = 'Stop'

# --- inputs -------------------------------------------------------------------------------------------------------
if (Test-Path $Path -PathType Leaf) { $dir = Split-Path -Parent (Resolve-Path $Path) } else { $dir = (Resolve-Path $Path).Path }
$exe = Join-Path $dir 'gt4-gs.exe'
if (-not (Test-Path $exe)) { throw "gt4-gs.exe not found in $dir" }
$modules = @(Get-Item $exe) + @(Get-ChildItem -Path $dir -Filter '*.dll' -File | Sort-Object Name)

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
$msvcRoot = Join-Path $vs 'VC\Tools\MSVC'
$msvc = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
if (-not (Test-Path $dumpbin)) { throw "dumpbin.exe not found under $msvcRoot" }

# --- markers ------------------------------------------------------------------------------------------------------
# Byte strings that only a code generator or a recompiler brings into a binary (checked against gt4-gs 0.3.2, which has
# all of them, and against the JIT-free 0.4.0, which has none).
$markers = @(
    '@Xbyak@@',                        # RTTI of Xbyak::CodeGenerator / CodeArray / Allocator
    'Xbyak',
    'GSDrawScanlineCodeGenerator',     # GS software JIT
    'GSSetupPrimCodeGenerator',
    'GSNewCodeGenerator',
    'GSCodeGeneratorFunctionMap',
    'SW JIT generated',
    'GS Software JIT cache overflow',
    '?AVrecMicroVU0@@',                # microVU (VU0/VU1 recompilers)
    '?AVrecMicroVU1@@',
    'microVU0 Recompiler',
    'microVU1 Recompiler',
    'microVU Error',
    'Recompiler Cache',                # code area of the recompilers (Memory.cpp)
    'EE/iR5900 Recompiler Reset',      # EE recompiler
    'recCancelInstruction',
    'iR5900.cpp',
    'iR3000A.cpp',                     # IOP recompiler
    'VifUnpackSSE_',                   # VIF unpack recompilers
    'x86Emitter::',                    # x86 emitter of the recompilers
    '\pcsx2\x86\',
    '\common\emitter\',
    '\3rdparty\xbyak\'
)
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$execApis = @('VirtualAlloc', 'VirtualAlloc2', 'VirtualAllocEx', 'VirtualAllocExNuma', 'VirtualAlloc2FromApp',
    'VirtualAllocFromApp', 'VirtualProtect', 'VirtualProtectEx', 'VirtualProtectFromApp', 'MapViewOfFile',
    'MapViewOfFileEx', 'MapViewOfFile3', 'MapViewOfFile3FromApp', 'MapViewOfFileFromApp', 'CreateFileMappingW',
    'CreateFileMappingA', 'CreateFileMapping2', 'FlushInstructionCache')

$failures = New-Object System.Collections.Generic.List[string]
$results = @()
foreach ($m in $modules) {
    $name = $m.Name
    $text = $latin1.GetString([System.IO.File]::ReadAllBytes($m.FullName))
    $found = @($markers | Where-Object { $text.IndexOf($_, [System.StringComparison]::Ordinal) -ge 0 })
    foreach ($f in $found) { $failures.Add("${name}: marker '$f'") }

    # Sections: flags are printed as "<hex> flags".
    $headers = & $dumpbin /nologo /headers $m.FullName
    $sections = @()
    $current = $null
    foreach ($line in $headers) {
        if ($line -match '^SECTION HEADER #\d+') { $current = @{ name = ''; flags = 0 }; $sections += $current; continue }
        if ($current -and $current.name -eq '' -and $line -match '^\s+(\S+) name$') { $current.name = $Matches[1]; continue }
        if ($current -and $line -match '^\s+([0-9A-Fa-f]{8}) flags$') { $current.flags = [Convert]::ToUInt32($Matches[1], 16) }
    }
    $rwx = @($sections | Where-Object { ($_.flags -band 0x20000000) -and ($_.flags -band 0x80000000) } | ForEach-Object { $_.name })
    foreach ($s in $rwx) { $failures.Add("${name}: section $s is writable and executable") }
    $nx = [bool]($headers | Select-String -SimpleMatch 'NX compatible')
    if ($name -eq 'gt4-gs.exe' -and -not $nx) { $failures.Add("${name}: not NX compatible") }

    $imports = & $dumpbin /nologo /imports $m.FullName
    $importNames = @($imports | ForEach-Object { if ($_ -match '^\s+[0-9A-Fa-f]+\s+([A-Za-z0-9_]+)\s*$') { $Matches[1] } })
    $memApis = @($execApis | Where-Object { $importNames -contains $_ })

    if ($name -eq 'gt4-gs.exe') {
        if (-not ($importNames -contains 'SetProcessMitigationPolicy')) { $failures.Add("${name}: does not import SetProcessMitigationPolicy (no Arbitrary Code Guard)") }
        if ($text.IndexOf('GT4GS no-JIT', [System.StringComparison]::Ordinal) -lt 0) { $failures.Add("${name}: no 'GT4GS no-JIT' text (not a GT4GS_ONLY build)") }
    }
    $results += [ordered]@{
        module = $name; size = $m.Length
        sha256 = (Get-FileHash -Algorithm SHA256 $m.FullName).Hash.ToLower()
        markers = $found; sections = @($sections | ForEach-Object { '{0}:{1:x8}' -f $_.name, $_.flags })
        writable_executable_sections = $rwx; nx_compatible = $nx; memory_api_imports = $memApis
    }
    $status = if ($found.Count -or $rwx.Count) { 'FAIL' } else { 'ok' }
    Write-Host ('{0,-24} {1,-4} markers={2} rwx_sections={3} nx={4} memory_apis={5}' -f $name, $status, $found.Count, $rwx.Count, $nx, ($memApis -join ','))
}

$summary = [ordered]@{
    package = $dir; dumpbin = $dumpbin; modules = $results; failures = @($failures)
    passed = ($failures.Count -eq 0)
}
if ($Report) { $summary | ConvertTo-Json -Depth 6 | Set-Content -Path $Report -Encoding UTF8 }
if ($failures.Count) {
    Write-Host "FAILED ($($failures.Count)):"
    $failures | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "PASSED: $($modules.Count) modules, no code generator markers, no writable+executable sections"
exit 0
