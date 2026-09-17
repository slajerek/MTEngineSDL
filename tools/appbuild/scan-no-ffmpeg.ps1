# Positive licence gate, Windows half. See scan-no-ffmpeg.sh for the full
# reasoning -- this is a direct translation and the comments there are the
# authority.
#
# NEVER RUN. Written on macOS, where PowerShell is not exercised. The two method
# choices carried over deliberately:
#   * the IMPORT TABLE is the primary signal, not defined symbols -- a
#     dynamically linked FFmpeg appears as an import, and a defined-symbol scan
#     reports a bundle full of FFmpeg as clean;
#   * the OUTPUT DIRECTORY is scanned too, not only the .exe -- a build can stop
#     linking FFmpeg while a copy step still stages the DLLs beside it, and
#     shipping those is shipping LGPL whether or not anything calls them.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Binary,
    [string]$Dir = "",
    [ValidateSet('absent','present')][string]$Expect = 'absent'
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Binary)) { Write-Error "scan-no-ffmpeg: no binary at '$Binary'"; exit 2 }

# dumpbin ships with MSVC; link.exe /dump is the same tool under another name.
$dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
if (-not $dumpbin) {
    Write-Error "scan-no-ffmpeg: dumpbin not on PATH -- run from a Developer prompt. A scan that cannot run proves nothing."
    exit 2
}

$imports = & dumpbin /IMPORTS $Binary 2>$null
if (-not $imports) {
    Write-Error "scan-no-ffmpeg: read no import table from '$Binary'; this proves nothing."
    exit 2
}

$hits = @()
$hits += ($imports | Select-String -Pattern 'av(codec|format|util)-\d+\.dll|sw(scale|resample)-\d+\.dll' -AllMatches |
          ForEach-Object { $_.Matches.Value })
if ($Dir -and (Test-Path $Dir)) {
    $hits += (Get-ChildItem -Path $Dir -Recurse -File -Include 'av*.dll','sw*.dll' -ErrorAction SilentlyContinue |
              ForEach-Object { $_.Name })
}
$hits = $hits | Sort-Object -Unique

if ($Expect -eq 'absent') {
    if ($hits.Count -gt 0) {
        Write-Host "FAIL: this build declared MT_ENABLE_FFMPEG=0 but carries FFmpeg:" -ForegroundColor Red
        $hits | ForEach-Object { Write-Host "    $_" }
        Write-Host "  A distributed build shipping these is shipping LGPL-2.1 code the"
        Write-Host "  store's own terms make it impossible to comply with."
        exit 1
    }
    Write-Host "ok: no FFmpeg in $(Split-Path $Binary -Leaf) -- import table clean, nothing staged"
} else {
    if ($hits.Count -eq 0) {
        Write-Host "FAIL: expected FFmpeg to be present and found none." -ForegroundColor Red
        Write-Host "  Either this build lost FFmpeg unintentionally, or the scanner is"
        Write-Host "  looking at the wrong file. '$Binary'"
        exit 1
    }
    Write-Host "ok: FFmpeg present as expected ($($hits.Count) distinct matches)"
}
exit 0
