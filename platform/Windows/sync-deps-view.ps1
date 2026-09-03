<#
.SYNOPSIS
    Populate a caps-keyed dependency VIEW from the per-unit stores. Copies only;
    never builds.
.DESCRIPTION
    The Visual Studio IDE path has never run acquisition -- MTCapsResolveForIDE
    resolves the view's path and nothing fills it. Before L16 there was nothing
    cheap to do about that: filling the view meant BUILDING the dependencies,
    and a forty-minute FFmpeg compile starting because somebody pressed F7 in
    Visual Studio is worse than the failure it prevents.

    L16 changed the arithmetic. A unit's outputs now live in a store keyed only
    by what that unit reads, and a view is a COPY of the stores. So the IDE can
    have its view for the price of a file copy -- measured at 3 s for the whole
    317-file DummyApp view -- with no compiler involved and no chance of a long
    build appearing out of nowhere.

    THE FAILURE THIS REPLACES is not the Link error the handover predicted. The
    view carries the dependency HEADERS too (ffmpeg\include, image-codecs\...),
    so an empty view stops the build at ClCompile:

        CImageDataTIFF.cpp(5,10): fatal error : 'tiffio.h' file not found

    WHAT THIS SCRIPT WILL NOT DO. It will not build a missing store, and it will
    not fail the build for one. A unit with no store has never been built for
    this capability set, and the only cure is a driver run; this says so, by
    name, and lets the build proceed to whatever it was going to do anyway. An
    Error here could only turn a working build into a broken one.
.PARAMETER CapsFile
    The resolved MTEngineCaps.xcconfig, which carries MT_STORE_<UNIT> for every
    unit on this platform.
.PARAMETER OutLibDir
    The view -- the `libs` directory every consumer links against.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CapsFile,
    [Parameter(Mandatory)][string]$OutLibDir
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\mt-build-common.ps1"

if (-not (Test-Path $CapsFile)) { throw "no caps fragment at $CapsFile" }
Import-MTCapsEnvironment -CapsFile $CapsFile | Out-Null

$stores = @(Get-ChildItem env: | Where-Object { $_.Name -like 'MT_STORE_*' } | Sort-Object Name)
if ($stores.Count -eq 0) {
    Write-Host "sync-deps-view: the resolve emitted no MT_STORE_* -- nothing to sync (engine predates L16?)"
    exit 0
}

New-Item -ItemType Directory -Force -Path $OutLibDir | Out-Null
$copied = 0
$missing = @()
foreach ($s in $stores) {
    $unit = $s.Name.Substring('MT_STORE_'.Length).ToLowerInvariant()
    if (Test-Path $s.Value) {
        Sync-MTStoreToView -Store $s.Value -View $OutLibDir
        $copied++
    } else {
        $missing += $unit
    }
}
Write-Host "sync-deps-view: $copied of $($stores.Count) stores copied into $OutLibDir"
if ($missing.Count -gt 0) {
    Write-Warning ("sync-deps-view: no store yet for: " + ($missing -join ', ') +
                   ". The IDE never acquires dependencies; run build-windows.ps1 once for this " +
                   "capability set, then this view fills itself from the stores on every later build.")
}
exit 0
