<#
.SYNOPSIS
    Scan built FFmpeg DLLs for the patent-encumbered decoders a restricted
    build must not carry (unification plan Phase 2, 2026-08-31).
.DESCRIPTION
    The Windows leg of tools/appbuild/scan-forbidden-symbols.sh. A linked PE
    carries no COFF symbol table and FFCodec entries are never DLL exports, so
    this leg scans the DLL's EMBEDDED codec-name strings (the same surface the
    codec build's own verification uses) -- a heuristic, stated as such: a hit
    is definitive, a miss is strong but not symbol-grade evidence. The
    symbol-grade probe on Windows is the retained PDB (decision 0.5).

    The decoder list comes from the vocabulary; no app names appear here.
.EXAMPLE
    .\scan-forbidden-symbols.ps1 -Mode commercial -Libs $mtLibsDir
#>
param(
    [Parameter(Mandatory)][ValidateSet('full', 'commercial')][string]$Mode,
    [Parameter(Mandatory)][string]$Libs
)
$ErrorActionPreference = 'Stop'

if ($Mode -eq 'full') {
    Write-Host "scan-forbidden-symbols: mode=full (private build) -- scan not applicable"
    exit 0
}
if (-not (Test-Path $Libs)) { Write-Error "no libs dir at $Libs"; exit 2 }

$vocab = Join-Path $PSScriptRoot '..\mtcaps\vocabulary.json'
$data = Get-Content $vocab -Raw | ConvertFrom-Json
$decoders = @()
foreach ($cap in $data.capabilities.PSObject.Properties.Value) {
    # The FFmpeg policy block is where the withheld decoder list lives since
    # 2026-09-02 (L4). commercial.forbidden_decoders_commercial stays readable
    # because other capabilities still use it -- photo codecs withhold 'heif',
    # which is not an FFmpeg decoder and is harmless noise in this grep.
    if ($cap.ffmpeg -and $cap.ffmpeg.decoders_withheld) {
        $decoders += $cap.ffmpeg.decoders_withheld
    }
    if ($cap.commercial -and $cap.commercial.forbidden_decoders_commercial) {
        $decoders += $cap.commercial.forbidden_decoders_commercial
    }
}
$decoders = $decoders | Sort-Object -Unique

$dlls = @(Get-ChildItem -Path $Libs -Recurse -Filter 'avcodec*.dll' -ErrorAction SilentlyContinue)
if ($dlls.Count -eq 0) {
    Write-Error "no avcodec DLL found under $Libs -- nothing was scanned, which proves nothing"
    exit 2
}

# THE PROBE (respecified 2026-08-31): the embedded configure string. Bare
# codec-name strings false-positive on parsers (commercial keeps the
# hevc/aac PARSERS); the engine's codec builds always pass explicit
# --enable-decoder= lists, and that line is embedded in the image.
$found = $false
foreach ($dll in $dlls) {
    $bytes = [System.IO.File]::ReadAllBytes($dll.FullName)
    $ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
    $enabled = @()
    foreach ($m in [regex]::Matches($ascii, "--enable-decoder=['`"]?([a-z0-9_,]+)")) {
        $enabled += $m.Groups[1].Value -split ','
    }
    if ($enabled.Count -eq 0) {
        Write-Error ("no --enable-decoder clause found in $($dll.FullName) -- not one of " +
                     "the engine's own codec builds, so this probe cannot prove anything about it.")
        exit 2
    }
    $bad = @($enabled | Sort-Object -Unique | Where-Object { $decoders -contains $_ })
    if ($bad.Count -gt 0) {
        Write-Host "FORBIDDEN: decoder(s) [$($bad -join ' ')] enabled in $($dll.FullName)" -ForegroundColor Red
        $found = $true
    }
}

if ($found) {
    Write-Error ("a '$Mode' build carries patent-encumbered decoders. The FFmpeg " +
                 "prefix is stale for this mode; rebuild the codecs and re-run.")
    exit 1
}
Write-Host "scan-forbidden-symbols: $($dlls.Count) avcodec DLL(s) clean for mode=$Mode ($($decoders.Count) decoders checked)"
exit 0
