# scan-capability-symbols.ps1 -Binary <exe> -MTOut <MT_OUT>
#
# The Windows half of tools/appbuild/scan-capability-symbols.sh. Read that
# script's header first: the two-directions rule is the whole design, and it is
# stated there rather than repeated here.
#
#   forbidden_when_off  a HARD GATE -- the capability is off, so its symbols
#                       must not be in the binary.
#   required_when_on    a REPORT -- absence means "linked, never called", which
#                       is correct for a static archive nothing references.
#
# The patterns in vocabulary.json carry BOTH manglings where the two differ
# (`_ZN2cv|@cv@@`), because one vocabulary serves both platforms. dumpbin's
# symbol dump is matched against them unchanged.
#
# NEVER RUN ON THE MAC. Written on macOS from the sh version and marked as
# an untested file in the Windows handover notes; the first Windows run is
# where it is verified.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Binary,
    [Parameter(Mandatory)][string]$MTOut
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Binary)) {
    Write-Error "scan-capability-symbols: no such binary: $Binary"
    exit 2
}

$fragment = Join-Path $MTOut 'MTEngineCapabilities.cmake'
if (-not (Test-Path $fragment)) {
    Write-Error "scan-capability-symbols: no capability fragment at $fragment"
    exit 2
}

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$vocabPath = Join-Path $repoRoot 'tools\mtcaps\vocabulary.json'
if (-not (Test-Path $vocabPath)) {
    Write-Error "scan-capability-symbols: no vocabulary at $vocabPath"
    exit 2
}

# dumpbin lives beside lib.exe, on PATH only inside a Developer shell. Resolved
# explicitly, the same way build-image_codecs.ps1 resolves lib.exe.
$dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
if (-not $dumpbin) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $found = & $vswhere -latest -requires Microsoft.Component.MSBuild `
        -find "VC\Tools\MSVC\**\bin\HostX64\x64\dumpbin.exe" 2>$null | Select-Object -First 1
    if (-not $found) {
        Write-Error "scan-capability-symbols: dumpbin.exe not found. Run from 'Developer PowerShell for VS 2022'."
        exit 2
    }
    $dumpbin = $found
}

$symbols = & $dumpbin /symbols $Binary 2>$null | Out-String
if ([string]::IsNullOrWhiteSpace($symbols)) {
    # A release .exe carries no COFF symbol table; the import/export dumps are
    # what remains. Neither is as complete as nm on a Mach-O binary, so a clean
    # result here is weaker evidence than a clean result there -- say so rather
    # than let it read as a pass.
    $symbols = (& $dumpbin /imports $Binary 2>$null | Out-String) +
               (& $dumpbin /exports $Binary 2>$null | Out-String)
    if ([string]::IsNullOrWhiteSpace($symbols)) {
        Write-Error "scan-capability-symbols: dumpbin produced nothing for $Binary"
        exit 2
    }
    Write-Host "note: no COFF symbol table; matched against imports and exports only"
}

$vocab = (Get-Content $vocabPath -Raw | ConvertFrom-Json).capabilities

$values = @{}
foreach ($line in (Get-Content $fragment)) {
    if ($line -match '^\s*set\((MT_CAP_[A-Z0-9_]+)\s+([01])\)') {
        $values[$Matches[1]] = $Matches[2]
    }
}

$failed = 0
foreach ($key in ($vocab.PSObject.Properties.Name | Sort-Object)) {
    $cap = $vocab.$key
    $value = $values[$key]
    if (-not $value) { continue }

    $forbidden = $cap.symbols.forbidden_when_off
    $required  = $cap.symbols.required_when_on

    if ($value -eq '0' -and $forbidden) {
        if ($symbols -match $forbidden) {
            Write-Host "FAIL  $key=0 but the binary defines symbols matching /$forbidden/"
            $failed = 1
        } else {
            Write-Host "ok    $key=0, nothing matches /$forbidden/"
        }
    } elseif ($value -eq '1' -and $required) {
        if ($symbols -match $required) {
            Write-Host "ok    $key=1, /$required/ present"
        } else {
            Write-Host "unused $key=1 but nothing matches /$required/ -- linked, never called"
        }
    }
}

if ($failed -ne 0) {
    Write-Error "scan-capability-symbols: FAILED for $Binary"
}
exit $failed
