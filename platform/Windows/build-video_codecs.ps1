<#
.SYNOPSIS
    Build Windows video codec dependencies: libvpx + opus (static), FFmpeg (shared, LGPL).
.DESCRIPTION
    Downloads and compiles libvpx and opus as static libraries (combined into
    MTVideoCodecs.lib, matching the per-Configuration /MT vs /MTd split used by
    build-image_codecs.ps1's Build-CMake), and FFmpeg as a shared (LGPL,
    decode-only) DLL bundle. This is the Windows port of
    platform/MacOS/build-video_codecs.sh -- read that script first; the
    licensing-load-bearing FFmpeg configure line here is a byte-for-byte copy
    of the macOS one except for the documented platform tail
    (--toolchain=msvc --arch=... --target-os=win64 --enable-d3d11va
    --enable-dxva2 instead of --enable-videotoolbox / darwin flags).

    Output:
      <OutLibDir>/ffmpeg/   (was other/lib/video-codecs/install-win-<Platform>/)
          bin/*.dll      (avcodec-61, avformat-61, avutil-59, swscale-8, swresample-5)
          lib/*.lib      (import libs for the above)
          include/       (ffmpeg + vpx + opus headers, single include root)
      platform/Windows/libs/<Platform>/<Configuration>/MTVideoCodecs.lib
          (libvpx 1.15.2 + opus 1.5.2, static, per-Configuration CRT)

    FFmpeg is built ONCE per Platform (its DLLs are /MD and config-independent
    across Debug/Release); libvpx and opus are built once per
    Platform+Configuration, matching MTImageCodecs.lib's convention.

.PARAMETER Platform
    Target architecture: x64 or ARM64. Default: auto-detect.
.PARAMETER Configuration
    Build configuration: Debug or Release. Default: Release. Only affects the
    libvpx/opus static-lib CRT selection; FFmpeg is always built /MD.
.PARAMETER Jobs
    Number of parallel build jobs. Default: number of logical processors.
.PARAMETER Clean
    Remove this Platform+Configuration's vpx/opus build/install artifacts and
    MTVideoCodecs.lib/stamp, then exit. The shared per-Platform FFmpeg
    build/install/stamp is deliberately KEPT (it is Configuration-independent
    and expensive; cleaning Debug must not force an FFmpeg rebuild for
    Release). Reference note: build-image_codecs.ps1 has no clean mode at all,
    so these semantics are defined here: -Clean is per-Configuration scoped,
    -CleanAll widens to the whole Platform. Cached downloads under
    other/lib/video-codecs/downloads are always kept.
.PARAMETER CleanAll
    Like -Clean, but also removes the shared per-Platform FFmpeg
    build/install/stamp tree (forces a full FFmpeg rebuild on the next run
    for this Platform, for BOTH Configurations). Implies -Clean.
.EXAMPLE
    .\build-video_codecs.ps1
    .\build-video_codecs.ps1 -Platform ARM64 -Configuration Debug

.NOTES
    Prerequisites (document, do not auto-install):
      - Visual Studio 2022 with the "Desktop development with C++" workload,
        including the ARM64 build tools component (Microsoft.VisualStudio.Component.
        VC.Tools.ARM64) if building -Platform ARM64.
      - MSYS2 (https://www.msys2.org/), default install path C:\msys64.
        Override with $env:MSYS2_ROOT if installed elsewhere.
        Required packages (from an MSYS2 shell):
            pacman -S --needed make diffutils pkgconf
        nasm is required for -Platform x64 only (x86 SIMD assembly in both
        libvpx and FFmpeg); it is not needed for -Platform ARM64.
      - git (for locating vswhere.exe indirectly is not required, but git must
        be on PATH if a future revision adds submodule-style fetches).

    Why MSYS2 + a loaded VS environment: FFmpeg's ./configure and libvpx's
    ./configure are POSIX shell scripts that must run under a real shell, but
    the actual compiler/linker/assembler they invoke (cl.exe / link.exe /
    lib.exe, and msbuild.exe for libvpx's VS-project output) are MSVC tools
    that only appear on PATH after vcvarsall.bat runs. This script loads the
    VS environment into the *current PowerShell process* (Import-VcVars, by
    capturing `vcvarsall.bat <arch> && set` and re-applying every resulting
    variable, then splicing MSYS2's usr\bin into PATH right after the VC
    dirs), then launches MSYS2's bash.exe --noprofile --norc as a child of
    that same process so it inherits this exact PATH verbatim (Task 9 found
    that a login shell's /etc/profile -- even with MSYS2_PATH_TYPE=inherit --
    unreliably discards the vcvarsall-added directories on this install, so
    the build deliberately never sources it). Splicing usr\bin in after the
    VC dirs, rather than relying on the plain original PATH, matters because
    MSYS2 ships its own link.exe (a hardlink coreutil) that would otherwise
    shadow MSVC's linker if usr\bin ever ended up ahead of the VC dirs.
#>
param(
    [ValidateSet('x64','ARM64')]
    [string]$Platform,

    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release',

    [int]$Jobs = $env:NUMBER_OF_PROCESSORS,

    [switch]$Clean,

    [switch]$CleanAll,

    [switch]$Help,

    # Where this script stages its archive. Supplied by build-deps.ps1 or by an
    # app wrapper, both from the single mtcaps resolve they already do.
    [string]$OutLibDir,

    # Where the FFmpeg import libraries, DLLs and headers install. Defaults to
    # <OutLibDir>\ffmpeg and must stay inside it -- see the paths section.
    [string]$OutPrefixDir
)

if ($Help) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\mt-build-common.ps1"

# Ensure TLS 1.2 for Invoke-WebRequest downloads
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# NOT $env:PROCESSOR_ARCHITECTURE, which describes the PROCESS and is inherited:
# under Git Bash (an emulated x64 build) it says AMD64 on an ARM64 machine.
$Platform = Resolve-MTPlatform $Platform

# ---- paths ----

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Resolve-Path "$scriptDir\..\.."
$cacheDir = Get-MTCapsWorkDir 'video-codecs'
$downloadDir = "$cacheDir\downloads"
$srcDir = "$cacheDir\src"

# REQUIRED, not defaulted. The caller decides where archives go; the only
# fallback in the system lives in build-deps.ps1, so the old
# platform\Windows\libs path cannot creep back in here -- that directory holds
# 25 TRACKED prebuilts and a build must not write into it.
#
# It comes FIRST now, because the FFmpeg prefix below is derived from it.
if (-not $OutLibDir) { throw "-OutLibDir is required. Run this through build-deps.ps1, which resolves it." }
# Per-unit store (L16). The unit builds in a directory keyed only by the
# capabilities IT reads, then its outputs are copied into the shared view. The
# body is wrapped in try/finally because a stamp hit and a capability-off stub
# both leave early and both still owe the view a copy.
$outLibDir = Use-MTStore -Unit 'video_codecs' -View $OutLibDir
try {
$outLib = "$outLibDir\MTVideoCodecs.lib"
$stampFile = "$outLibDir\MTVideoCodecs.stamp"

# FFmpeg: built once per Platform (config-independent /MD DLLs).
#
# THE PREFIX LIVES WITH THE ARCHIVE, keyed identically, and that is a
# CORRECTNESS requirement rather than tidiness. It used to be
# other\lib\video-codecs\install-win-<Platform> inside the checkout: one shared
# directory that the photo app both links out of and copies runtime DLLs from. The
# skip check below tests for lib\, bin\ and the stamp in that prefix, so a keyed
# stamp over a SHARED prefix lets app A (full) fill it, app B (commercial)
# overwrite it, and app A then skip on its own valid stamp and link the
# store-safe decoder set. No error, wrong binary. Keying one without the other
# is strictly worse than keying neither. Same reasoning, and the same fix, as
# platform/Linux/build-video_codecs.sh.
$ffmpegBuildDir = "$cacheDir\build-win-$Platform\ffmpeg"
$ffmpegPrefix = if ($OutPrefixDir) { $OutPrefixDir } else { Join-Path $outLibDir 'ffmpeg' }
$ffmpegStampFile = "$ffmpegPrefix\ffmpeg.stamp"

if (-not $ffmpegPrefix.StartsWith($outLibDir, [StringComparison]::OrdinalIgnoreCase)) {
    throw "FFmpeg prefix $ffmpegPrefix is not inside the keyed dependency directory $outLibDir"
}

# libvpx/opus: built once per Platform+Configuration (CRT-dependent static libs).
# These are INTERMEDIATES -- nothing links or loads them directly, the archive
# absorbs them -- so they stay beside the sources until chunk 0.11 moves the
# build trees wholesale.
$vpxBuildDir = "$cacheDir\build-win-$Platform-$Configuration\vpx"
$opusBuildDir = "$cacheDir\build-win-$Platform-$Configuration\opus"
$vpxOpusPrefixDir = "$cacheDir\install-vpxopus-win-$Platform-$Configuration"

New-Item -ItemType Directory -Force -Path $downloadDir, $srcDir, $outLibDir | Out-Null

# ---- build mode (COMMERCIAL=1 -> store-safe FFmpeg: no WMV/VC-1, no WMA,
#      no HEVC/AAC/EAC3 software decoders; COMMERCIAL=0 "full" -> adds
#      WMV/WMA software decode plus the HEVC/AAC/EAC3 software fallbacks
#      (2026-07-19 codec-superset spec) for non-store internal builds).
#      COMMERCIAL is a PARAMETER now, not a file read. The engine used to track
#      platform\BUILD_MODE_DEFAULT and the photo app tracked a root-level file of
#      the same name -- two files, two owners, one name, neither reconciled with
#      MT_COMMERCIAL_BUILD. All three are retired: the licence mode lives in the
#      app's mtengine.caps and reaches acquisition, compilation and LICENSES.txt
#      through one channel. Identical semantics to the macOS/Linux scripts. ----

# The RESOLVED mode wins when present (unification plan Phase 2, 2026-08-31):
# mtcaps derives `full` only for MT_PRIVATE_BUILD=1 -- the withheld decoders
# are patent-encumbered, and patents attach to DISTRIBUTION, so public/free
# gets the restricted set like the store tier. COMMERCIAL stays as the legacy
# channel for direct/standalone runs.
if ($env:MT_FFMPEG_BUILD_MODE) {
    if ($env:MT_FFMPEG_BUILD_MODE -notin @('full', 'commercial')) {
        throw "MT_FFMPEG_BUILD_MODE must be full or commercial (got '$($env:MT_FFMPEG_BUILD_MODE)')"
    }
    $ffmpegBuildMode = $env:MT_FFMPEG_BUILD_MODE
    Write-Host "FFmpeg build mode: $ffmpegBuildMode (from resolved MT_FFMPEG_BUILD_MODE)"
} else {
    $commercial = $env:COMMERCIAL
    if ([string]::IsNullOrEmpty($commercial)) {
        # A direct run defaults to non-commercial, the safer default: it only
        # ever includes more, never less than a licence permits.
        $commercial = "0"
    }
    if ($commercial -ne "0" -and $commercial -ne "1") {
        throw "COMMERCIAL must be 0 or 1 (got '$commercial')"
    }
    $ffmpegBuildMode = if ($commercial -eq "1") { "commercial" } else { "full" }
    Write-Host "FFmpeg build mode: $ffmpegBuildMode (COMMERCIAL=$commercial)"
}

# ---- pinned versions / URLs / hashes -- copied verbatim from
#      platform/MacOS/build-video_codecs.sh; never re-derive from memory ----

$vpxVersion    = "1.15.2"
$opusVersion   = "1.5.2"
$ffmpegVersion = "7.1.2"

$vpxUrl    = "https://github.com/webmproject/libvpx/archive/refs/tags/v$vpxVersion.tar.gz"
$vpxHash   = "26fcd3db88045dee380e581862a6ef106f49b74b6396ee95c2993a260b4636aa"
$opusUrl   = "https://downloads.xiph.org/releases/opus/opus-$opusVersion.tar.gz"
$opusHash  = "65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"
# ffmpeg.org does not publish a .sha256 sidecar for release tarballs (only a
# detached .asc PGP signature). This hash was computed locally from a tarball
# downloaded over TLS from https://ffmpeg.org/releases/ and independently
# verified against the "FFmpeg release signing key <ffmpeg-devel@ffmpeg.org>"
# (fingerprint FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8) via the
# accompanying ffmpeg-7.1.2.tar.xz.asc before being pinned here (same value as
# the macOS script -- it is the same upstream tarball on both platforms).
$ffmpegUrl  = "https://ffmpeg.org/releases/ffmpeg-$ffmpegVersion.tar.xz"
$ffmpegHash = "089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304"

# The exact component sets expected in the built FFmpeg libraries, per build
# mode: the 19 decoders explicitly requested via --enable-decoder in the
# configure invocation below, plus h263, which FFmpeg's configure
# force-enables as a select-dependency of mpeg4
# (mpeg4_decoder_select="h263_decoder"); full mode adds the 7 WMV/WMA
# software decoders and the vc1 parser (2026-07-18 WMV spec). The parser set
# also carries a force-enabled h263 (select-dependency of the mpeg4video
# parser). Anything outside these lists appearing enabled fails the build --
# this is the licensing guard for the LGPL decode-only bundle, and the
# AUTHORITATIVE one (config_components.h reflects what was compiled).
# Identical lists to the macOS/Linux scripts; do not edit without updating
# all three.
# THE POLICY COMES FROM THE VOCABULARY. mt-build-common.ps1 reads
# MT_FFMPEG_DECODERS / _PARSERS / _DEMUXERS / _DECODERS_WITHHELD out of the
# caps fragment; this script used to carry its own copy of all four, as did
# the two sh scripts, so a name added in one platform was invisible to the
# other two and to the licence scanner.
#
# Interpolated HERE, in PowerShell, because the configure line is generated
# into a bash heredoc that MSYS2 runs -- there is no reading an environment
# variable "later" on that side.
function Policy-Csv([string]$list) { ($list -split '\s+' | Where-Object { $_ }) -join ',' }
function Policy-Sorted([string]$list) { (($list -split '\s+' | Where-Object { $_ }) | Sort-Object -Unique) -join ' ' }

$ffmpegDecoders = $env:MT_FFMPEG_DECODERS
$ffmpegParsers = $env:MT_FFMPEG_PARSERS
$ffmpegDemuxers = $env:MT_FFMPEG_DEMUXERS
$ffmpegWithheld = $env:MT_FFMPEG_DECODERS_WITHHELD
foreach ($pair in @(@('MT_FFMPEG_DECODERS', $ffmpegDecoders), @('MT_FFMPEG_PARSERS', $ffmpegParsers),
                    @('MT_FFMPEG_DEMUXERS', $ffmpegDemuxers), @('MT_FFMPEG_DECODERS_WITHHELD', $ffmpegWithheld))) {
    if ([string]::IsNullOrWhiteSpace($pair[1])) {
        throw "$($pair[0]) is empty -- the caps fragment carries the decoder policy since 2026-09-02; run through the driver, or pass -CapsFile."
    }
}

# What FFmpeg force-enables on top of the requested sets is a fact about
# ffmpeg-$ffmpegVersion, not policy, and lives beside that pin:
#   h263 decoder <- mpeg4_decoder_select; h263 parser <- mpeg4video parser
$ffmpegImplicitDecoders = "h263"
$ffmpegImplicitParsers = "h263"

$ffmpegExpectedDecoders = Policy-Sorted "$ffmpegDecoders $ffmpegImplicitDecoders"
$ffmpegExpectedParsers = Policy-Sorted "$ffmpegParsers $ffmpegImplicitParsers"
$ffmpegExpectedDemuxers = Policy-Sorted $ffmpegDemuxers
# Upper-cased for the CONFIG_<NAME>_DECODER symbols; 13 names now, where the
# hand-written copy had 10 (it was missing msmpeg4v1/v2/v3).
$ffmpegForbiddenDecodersCommercial = ($ffmpegWithheld -split '\s+' | Where-Object { $_ }) | ForEach-Object { $_.ToUpperInvariant() }

# ---- Platform -> toolchain arg mappings ----

$vcArch       = @{ 'ARM64' = 'arm64';           'x64' = 'x64'     }[$Platform]  # vcvarsall.bat arg
$ffmpegArch   = @{ 'ARM64' = 'arm64';           'x64' = 'x86_64'  }[$Platform]  # ffmpeg --arch=
$vpxTarget    = @{ 'ARM64' = 'arm64-win64-vs17'; 'x64' = 'x86_64-win64-vs17' }[$Platform]
$msbuildPlat  = @{ 'ARM64' = 'ARM64';            'x64' = 'x64'    }[$Platform]  # msbuild /p:Platform

# ---- clean ----

if ($Clean -or $CleanAll) {
    # -Clean is scoped to this Platform+Configuration's vpx/opus artifacts.
    # The shared per-Platform FFmpeg tree is config-independent (/MD DLLs) and
    # expensive to rebuild, so it is only removed under the explicit -CleanAll.
    Write-Host "Cleaning vpx/opus build artifacts for $Platform $Configuration..."
    Remove-Item -Recurse -Force $vpxBuildDir, $opusBuildDir, $vpxOpusPrefixDir, `
        $outLib, $stampFile -ErrorAction SilentlyContinue
    if ($CleanAll) {
        Write-Host "Cleaning shared FFmpeg build/install for $Platform (affects both Configurations)..."
        Remove-Item -Recurse -Force $ffmpegBuildDir, $ffmpegPrefix, $ffmpegStampFile -ErrorAction SilentlyContinue
    }
    Write-Host "Done. Source downloads remain cached in $downloadDir."
    exit 0
}

# ---- stamp check (full skip if nothing changed) ----

$scriptHash = (Get-FileHash -Path $MyInvocation.MyCommand.Path -Algorithm SHA256).Hash
# THE CAPABILITY GATE. build-video_codecs had none on any platform, so
# MT_CAP_VIDEO_PLAYBACK=0 still downloaded and built FFmpeg, libvpx and opus.
#
# Nothing in the automatic Windows build calls this script today -- build-deps.ps1
# does not -- so this gate is for parity and for anyone running it by hand with a
# capability set published. Absent means ON.
# WEBM_VPX is the archive's own flag; FFMPEG kept as fallback for a fragment
# older than 2026-08-31.
$webmVpx = if ($null -ne $env:MT_ENABLE_WEBM_VPX) { $env:MT_ENABLE_WEBM_VPX }
           else { $env:MT_ENABLE_FFMPEG }
if ($webmVpx -eq '0') {
    if (-not (Get-Command cl -ErrorAction SilentlyContinue) -or
        -not (Get-Command lib -ErrorAction SilentlyContinue)) {
        throw "Video codecs are disabled by the capability set, but cl/lib are needed to write the stub archive. Run from 'Developer PowerShell for VS 2022'."
    }
    New-MTCapsStubArchive -OutLib $outLib -Stamp $stampFile -Symbol 'mt_video_codecs' `
                          -StampValue "disabled|$scriptHash|$Platform|$Configuration"
    exit 0
}

$stampValue = "$scriptHash|libvpx-$vpxVersion|opus-$opusVersion|ffmpeg-$ffmpegVersion|$Platform|$Configuration|mode-$ffmpegBuildMode"
$ffmpegStampValue = "$scriptHash|ffmpeg-$ffmpegVersion|$Platform|mode-$ffmpegBuildMode"

$ffmpegUpToDate = $false
if ((Test-Path (Join-Path $ffmpegPrefix "lib")) -and (Test-Path (Join-Path $ffmpegPrefix "bin")) -and (Test-Path $ffmpegStampFile)) {
    $existing = (Get-Content $ffmpegStampFile -Raw).Trim()
    if ($existing -eq $ffmpegStampValue) { $ffmpegUpToDate = $true }
}

if ((Test-Path $outLib) -and (Test-Path $stampFile) -and $ffmpegUpToDate) {
    $existingStamp = (Get-Content $stampFile -Raw).Trim()
    if ($existingStamp -eq $stampValue) {
        Write-Host "Video codec bundle is up to date: $outLib" -ForegroundColor Green
        exit 0
    }
}

# ---- generic helpers (download/extract; same conventions as build-image_codecs.ps1) ----

function Get-Sha256($filePath) {
    return (Get-FileHash -Path $filePath -Algorithm SHA256).Hash.ToLower()
}

function Download-Archive($name, $url, $expectedHash, $ext) {
    $archive = Join-Path $downloadDir "$name.$ext"
    if (-not (Test-Path $archive)) {
        Write-Host "Downloading $name..."
        Invoke-WebRequest -Uri $url -OutFile $archive
    }
    $actualHash = Get-Sha256 $archive
    if ($actualHash -ne $expectedHash) {
        Write-Error "Checksum mismatch for $archive`nExpected: $expectedHash`nActual:   $actualHash"
        exit 1
    }
}

# Fails if any of $Expect (paths relative to $dest) is missing. Separate from
# the extractor's exit code ON PURPOSE: bsdtar's 0 says "I stopped without
# raising an error", not "the tree is complete", and there is no manifest check
# anywhere else in this script.
function Assert-Extracted($name, $dest, [string[]]$Expect) {
    foreach ($rel in $Expect) {
        $path = Join-Path $dest $rel
        if (-not (Test-Path $path)) {
            throw "$name is not fully extracted: '$path' is missing. Delete '$dest' and re-run to extract it again. (The extraction step reported success; this check exists because that report is not sufficient.)"
        }
    }
}

function Extract-Archive($name, $topDir, $ext, $tarFlag, [string[]]$Expect) {
    $archive = Join-Path $downloadDir "$name.$ext"
    $dest = Join-Path $srcDir $topDir
    # THE EARLY-OUT, SAID OUT LOUD. The existence of the destination directory
    # is the ONLY thing that makes extraction a once-per-cache event -- there is
    # no stamp, no manifest, no size check. So a HALF-extracted tree is
    # permanent: every later run reaches this line, sees the directory, returns,
    # and then fails somewhere much further along with a message that says
    # nothing about extraction at all. $Expect is the guard, and it runs on this
    # cached path as well as on the freshly extracted one -- a cache poisoned by
    # an earlier failed run is reported HERE, where the cause is still legible,
    # and the fix is one rm -rf away.
    if (Test-Path $dest) {
        Assert-Extracted $name $dest $Expect
        return
    }
    Write-Host "Extracting $name..."
    # WHICH tar decides how the destination must be spelled -- GNU tar needs
    # --force-local and the /c/... form, BSD tar rejects the flag and cannot
    # chdir to /c/... at all. The shared helper asks once, instead of making an
    # attempt that is EXPECTED to fail print a red multi-line usage message
    # before every archive. See Invoke-MTTarExtract.
    Invoke-MTTarExtract -Archive $archive -TarFlag $tarFlag `
        -DestWindows $srcDir -DestPosix (ConvertTo-MsysPath $srcDir)
    Assert-Extracted $name $dest $Expect
}

function Require-File($path) {
    if (-not (Test-Path $path)) { throw "Expected file not found: $path" }
}

# ---- VS + MSYS2 bridging helpers ----

function Find-VcVarsAll {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if ($vsPath) {
            $candidate = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    # Fallback: common install paths (vswhere absent, e.g. minimal CI images)
    foreach ($edition in @('Enterprise','Professional','Community','BuildTools')) {
        $candidate = "${env:ProgramFiles}\Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvarsall.bat"
        if (Test-Path $candidate) { return $candidate }
    }
    throw "Could not locate vcvarsall.bat. Install Visual Studio 2022 with the 'Desktop development with C++' workload (and the ARM64 build tools component for -Platform ARM64)."
}

function Find-Msys2Bash {
    $msys2Root = $env:MSYS2_ROOT
    if (-not $msys2Root) { $msys2Root = "C:\msys64" }
    $bash = Join-Path $msys2Root "usr\bin\bash.exe"
    if (-not (Test-Path $bash)) {
        throw "MSYS2 bash.exe not found at $bash. Install MSYS2 from https://www.msys2.org/ (default path C:\msys64) or set `$env:MSYS2_ROOT to your install location."
    }
    return $bash
}

# THE MAKE THE GENERATED SCRIPTS MUST USE, and why it is spelled absolutely.
#
# FFmpeg is configured OUT OF TREE, and its configure then writes a one-line
# Makefile into the build directory:
#
#     include /c/Users/.../video-codecs/src/ffmpeg-7.1.2/Makefile
#
# an ABSOLUTE path in MSYS2's /c/... spelling, because that is what `pwd`
# returns inside the shell configure ran in. Only an MSYS2 make can read it. A
# NATIVE Windows GNU make -- the "Built for Windows32" build a Chocolatey,
# Strawberry Perl or CMake-adjacent install can put on PATH -- reads /c/Users
# as C:\c\Users and stops with
#
#     Makefile:1: /c/Users/.../Makefile: No such file or directory
#     make: *** No rule to make target '/c/Users/.../Makefile'.  Stop.
#
# which is exactly how GitHub's windows runner failed on 2026-09-03 (run
# 33742482983), and exactly what a native make reproduces here on demand. The
# failure names the SOURCE tree, so it reads like a broken extraction; it is
# not. Note that libvpx sails through the same shell untouched: ITS generated
# Makefile says `include config.mk`, relative, which any make resolves -- so
# "vpx built, FFmpeg did not" is the signature of this defect, not evidence
# against it.
#
# Import-VcVars splices MSYS2's usr\bin into PATH ahead of the pre-existing
# entries, which is enough WHEN MSYS2 HAS make INSTALLED -- it is not part of
# a base MSYS2 install, it is the documented `pacman -S --needed make`
# prerequisite. When it is absent, PATH search does not fail; it falls through
# to whatever other make the machine has, and the build gets a make that
# cannot read its own Makefile. Naming /usr/bin/make removes the search
# entirely: /usr/bin inside the bash we launch is always <MSYS2_ROOT>\usr\bin,
# whatever $env:MSYS2_ROOT is set to.
function Require-Msys2Make {
    $msys2Root = $env:MSYS2_ROOT
    if (-not $msys2Root) { $msys2Root = "C:\msys64" }
    $make = Join-Path $msys2Root "usr\bin\make.exe"
    if (-not (Test-Path $make)) {
        throw "MSYS2's make is not installed: $make is missing. It is a documented prerequisite of this script -- from an MSYS2 shell run: pacman -S --needed make diffutils pkgconf. There is deliberately no fallback to another make on PATH: FFmpeg's out-of-tree Makefile includes its source Makefile by absolute /c/... path, which only an MSYS2 make can resolve."
    }
    return '/usr/bin/make'
}

# Imports the VS developer environment (cl.exe, link.exe, lib.exe, msbuild.exe,
# dumpbin.exe, nasm if on PATH already) into the CURRENT PowerShell process by
# shelling out to `vcvarsall.bat <arch> && set` and re-applying every resulting
# environment variable. Any child process launched afterwards (including
# MSYS2's bash.exe, and msbuild/dumpbin invoked directly) inherits this PATH.
function Import-VcVars([string]$Arch) {
    $vcvarsall = Find-VcVarsAll
    Write-Host "Loading VS environment ($Arch) via $vcvarsall"
    # vcvarsall.bat's ARM64 cross-toolset resolution shells out to
    # vswhere.exe by bare name; it is not on PATH by default (only its
    # fixed install location under "Program Files (x86)\...\Installer\" is
    # known - see Find-VcVarsAll above), so vcvarsall.bat fails with
    # "'vswhere.exe' is not recognized" unless that directory is on PATH for
    # this child cmd.exe.
    $vswhereDir = Split-Path -Parent "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $beforePath = $env:PATH
    $cmdLine = "set `"PATH=$vswhereDir;%PATH%`" && call `"$vcvarsall`" $Arch >nul && set"
    $lines = & cmd.exe /c $cmdLine
    if ($LASTEXITCODE -ne 0) {
        throw "vcvarsall.bat failed for arch '$Arch' (exit $LASTEXITCODE). Is the ARM64 build tools component installed?"
    }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
    # Insert MSYS2's usr\bin right after the VC dirs vcvarsall.bat just
    # prepended, and before everything else already on PATH (Chocolatey,
    # Git-for-Windows, etc.). Order matters: MSYS2's usr\bin ships its own
    # link.exe (a hardlink coreutil) and make.exe that must NOT shadow
    # cl.exe/link.exe/nmake from VC dirs, but SHOULD win over whatever
    # differently-behaved make/tar/sed a Chocolatey or Git install might
    # place earlier on the pre-existing PATH. vcvarsall.bat's own edit is
    # exactly "PATH=<new dirs>;%PATH%", so the prefix it added is recovered
    # by trimming the pre-import PATH (captured above) off the end of the
    # now-current one.
    $msys2Root = $env:MSYS2_ROOT
    if (-not $msys2Root) { $msys2Root = "C:\msys64" }
    $afterPath = $env:PATH
    if ($afterPath.EndsWith($beforePath)) {
        $vcAddedPrefix = $afterPath.Substring(0, $afterPath.Length - $beforePath.Length)
        $env:PATH = "$vcAddedPrefix$msys2Root\usr\bin;$beforePath"
    } else {
        $env:PATH = "$msys2Root\usr\bin;$afterPath"
    }
}

# Converts a Windows path (C:\foo\bar) to the /c/foo/bar form MSYS2 bash and
# the autoconf configure scripts expect (this is the form FFmpeg's own
# Windows/MSVC build docs use for --prefix, and what MSYS2's path-mangling
# understands unambiguously).
function ConvertTo-MsysPath([string]$WindowsPath) {
    $p = $WindowsPath -replace '\\', '/'
    if ($p -match '^([A-Za-z]):(.*)$') {
        return "/$($Matches[1].ToLower())$($Matches[2])"
    }
    return $p
}

# Runs a bash script file inside MSYS2, with the VS environment (already
# imported into this process via Import-VcVars) inherited verbatim.
#
# DELIBERATELY NOT a login shell (-l): MSYS2_PATH_TYPE=inherit +
# CHERE_INVOKING=1 are the documented way to make an MSYS2 LOGIN shell's
# /etc/profile keep the inherited Windows-style PATH instead of rebuilding
# a pure-MSYS one, but on this install (verified empirically on first run --
# see the Task-9 report) /etc/profile's PATH_TYPE=inherit branch still
# ultimately loses the vcvarsall-added cl.exe/link.exe directories, so
# FFmpeg's configure fails with "cl.exe: command not found" even though the
# Windows env var itself carries the right value into bash's process. A
# non-login shell (--noprofile --norc) never touches PATH at all -- it uses
# exactly the PATH Import-VcVars + the usr\bin insertion above already built
# for this PowerShell process, verbatim, which is the actually-reliable
# mechanism here.
function Invoke-Msys2Script([string]$ScriptPath, [string]$LogPath) {
    $bash = Find-Msys2Bash
    $posixPath = ConvertTo-MsysPath $ScriptPath
    Write-Host "msys2> bash '$posixPath'"
    & $bash --noprofile --norc -c "bash '$posixPath'" 2>&1 | Tee-Object -FilePath $LogPath
    if ($LASTEXITCODE -ne 0) {
        throw "MSYS2 build script failed: $ScriptPath (see $LogPath)"
    }
}

# Writes $Content to $Path with LF line endings (bash scripts with CRLF can
# misbehave on `case`/heredoc constructs; PowerShell's Set-Content defaults to
# CRLF on Windows). Encoding is UTF-8 WITHOUT a BOM: a BOM would be fed to
# bash as garbage bytes before `set -euo pipefail`, and plain ASCII would
# mangle any non-ASCII characters in interpolated repo paths.
function Write-BashScript([string]$Path, [string]$Content) {
    $lf = $Content -replace "`r`n", "`n"
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $lf, $utf8NoBom)
}

# CMake build helper -- identical convention to build-image_codecs.ps1's
# Build-CMake (same CRT-selection logic, same generator/platform args).
function Build-CMake($srcDir, $buildDir, $prefixDir, [string[]]$extraArgs) {
    $crtLib = if ($Configuration -eq 'Debug') { 'MultiThreadedDebug' } else { 'MultiThreaded' }
    $cmakeArgs = @(
        "-S", $srcDir,
        "-B", $buildDir,
        "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DCMAKE_INSTALL_PREFIX=$prefixDir",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=$crtLib"
    )
    if ($Platform -eq "ARM64") {
        $cmakeArgs += "-A", "ARM64"
    } else {
        $cmakeArgs += "-A", "x64"
    }
    $cmakeArgs += $extraArgs

    # $ErrorActionPreference = 'Stop' (script-global) makes PowerShell treat
    # ANY stderr line from a native command as a terminating NativeCommandError
    # -- regardless of exit code, and even with no explicit 2>&1 redirection
    # (confirmed on first Windows run: cmake's own non-fatal "CMake Warning:"
    # after a successful "-- Generating done" aborted the script outright).
    # Relax to 'Continue' for just these three native calls so a benign
    # stderr write can't masquerade as failure; $LASTEXITCODE below remains
    # the real, sufficient success/failure signal (unaffected by this).
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        Write-Host "Configuring: $srcDir -> $buildDir"
        & cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $srcDir" }

        Write-Host "Building: $buildDir"
        & cmake --build $buildDir --config $Configuration -j $Jobs
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $buildDir" }

        Write-Host "Installing to $prefixDir"
        & cmake --install $buildDir --config $Configuration
        if ($LASTEXITCODE -ne 0) { throw "CMake install failed for $buildDir" }
    } finally {
        $ErrorActionPreference = $savedEap
    }
}

# ---- decoder-guard: parses config_components.h (generated by FFmpeg's
#      ./configure, before `make` even runs) directly as text. This is the
#      SAME mechanism the macOS script uses (it also greps config_components.h,
#      not `nm`) so the guard logic ports essentially unchanged -- no MSYS2
#      shell-out is needed for this part at all, which makes it robust even
#      if the MSYS2/VS bridging above needs iteration on the first real
#      Windows run. ----

function Get-EnabledComponents([string]$configComponentsPath, [string]$kind) {
    Require-File $configComponentsPath
    $content = Get-Content -Path $configComponentsPath -Raw
    $matches = [regex]::Matches($content, "(?m)^#define CONFIG_([A-Z0-9_]+)_${kind} 1$")
    $names = foreach ($m in $matches) { $m.Groups[1].Value.ToLowerInvariant() }
    return ($names | Sort-Object)
}

# Kind is the config_components.h suffix: DECODER, DEMUXER, or PARSER.
function Require-FfmpegExactSet([string]$buildDir, [string]$kind, [string]$expectedList) {
    $configComponentsPath = Join-Path $buildDir "config_components.h"
    $actual = Get-EnabledComponents $configComponentsPath $kind
    $expected = ($expectedList -split ' ') | Sort-Object
    $actualStr = $actual -join "`n"
    $expectedStr = $expected -join "`n"
    if ($actualStr -ne $expectedStr) {
        Write-Host "ERROR: ffmpeg enabled-$kind set mismatch in $configComponentsPath (mode: $ffmpegBuildMode)" -ForegroundColor Red
        Write-Host "Expected:`n$expectedStr"
        Write-Host "Actual:`n$actualStr"
        throw "FFmpeg $kind set mismatch (licensing guard) in $configComponentsPath"
    }
}

function Require-FfmpegDecoderDisabled([string]$buildDir, [string]$decoder) {
    $configComponentsPath = Join-Path $buildDir "config_components.h"
    Require-File $configComponentsPath
    $pattern = "define\s+CONFIG_${decoder}_DECODER\s+0"
    if (-not (Select-String -Path $configComponentsPath -Pattern $pattern -Quiet)) {
        throw "Forbidden decoder '$decoder' is not confirmed disabled in $configComponentsPath"
    }
}

# ---- symbol check: nm's Windows/MSVC equivalent is dumpbin /linkermember:1
#      (NOT /symbols -- confirmed on first Windows run: /symbols is only
#      meaningful for a single .obj/PE image, and returns essentially nothing
#      for a multi-member .lib archive like these combined static libs;
#      /linkermember:1 dumps the archive's actual public-symbol export table,
#      which is what this check needs). Unlike macOS Mach-O (which prefixes C
#      symbols with `_`), MSVC's x64/ARM64 COFF output does NOT add an
#      underscore prefix to C functions, so the expected names below are bare
#      (vpx_codec_..., not _vpx_codec_...). dumpbin requires the VS
#      environment (Import-VcVars) to already be loaded in this process. ----

function Require-Symbol([string]$libPath, [string]$symbol) {
    $output = & dumpbin /nologo /linkermember:1 $libPath 2>&1
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $libPath" }
    if (-not ($output | Select-String -SimpleMatch $symbol -Quiet)) {
        throw "Expected symbol not found in ${libPath}: $symbol"
    }
}

# ==================== download & extract ====================

Download-Archive "libvpx-$vpxVersion" $vpxUrl $vpxHash "tar.gz"
Download-Archive "opus-$opusVersion" $opusUrl $opusHash "tar.gz"
Download-Archive "ffmpeg-$ffmpegVersion" $ffmpegUrl $ffmpegHash "tar.xz"

# The -Expect lists are the files the build IMMEDIATELY needs and would
# otherwise miss late and confusingly: each tree's configure, and -- for FFmpeg,
# whose out-of-tree build includes it by absolute path from a generated
# one-liner -- the top-level Makefile too.
Extract-Archive "libvpx-$vpxVersion" "libvpx-$vpxVersion" "tar.gz" "-xzf" @("configure")
Extract-Archive "opus-$opusVersion" "opus-$opusVersion" "tar.gz" "-xzf" @("configure")
Extract-Archive "ffmpeg-$ffmpegVersion" "ffmpeg-$ffmpegVersion" "tar.xz" "-xJf" @("configure", "Makefile")

# ==================== load VS environment for this Platform ====================

Import-VcVars $vcArch

# Checked HERE, before a single archive is configured, rather than at the first
# `make` an hour in: a missing prerequisite should cost seconds. See
# Require-Msys2Make for why the answer is an absolute path and not PATH.
$msysMake = Require-Msys2Make

if ($Platform -eq 'x64') {
    # The documented install path (`pacman -S nasm` inside an MSYS2 shell)
    # lands nasm.exe in MSYS2's own usr\bin, which is on the MSYS2 SHELL's
    # PATH but NOT on this PowerShell process's PATH -- a Get-Command-only
    # check here is a false negative for anyone who followed the documented
    # instructions verbatim. Probe both: PowerShell's PATH (nasm installed
    # some other way, e.g. https://www.nasm.us/ with its installer adding
    # itself to the system PATH) and MSYS2's usr\bin directly (same
    # MSYS2_ROOT / C:\msys64 default resolution as Find-Msys2Bash above).
    $nasmOnPath = [bool](Get-Command nasm -ErrorAction SilentlyContinue)
    $msys2RootForNasm = $env:MSYS2_ROOT
    if (-not $msys2RootForNasm) { $msys2RootForNasm = "C:\msys64" }
    $nasmInMsys2 = Test-Path (Join-Path $msys2RootForNasm "usr\bin\nasm.exe")
    if (-not $nasmOnPath -and -not $nasmInMsys2) {
        throw "nasm not found on PATH or at $msys2RootForNasm\usr\bin\nasm.exe. nasm is a required prerequisite for -Platform x64 (x86 SIMD assembly in libvpx and FFmpeg). Install it (e.g. via MSYS2: pacman -S nasm, or https://www.nasm.us/); if MSYS2 is installed somewhere other than C:\msys64, set `$env:MSYS2_ROOT first."
    }
}

# FFmpeg's ARM64+MSVC toolchain assembles its NEON asm via gas-preprocessor.pl
# (translating GNU-style asm for clang, since MSVC has no compatible
# assembler for it) -- not a standard MSYS2 package, and not documented as a
# prerequisite anywhere for this plan. Same situation as x64's optional nasm
# above and mirrors platform/MacOS/build-video_codecs.sh's own fallback for
# a missing x86 assembler (--disable-x86asm there): when the ARM assembler
# toolchain isn't present, disable asm rather than fail the whole build --
# every enabled decoder still has a pure-C fallback path, only SIMD
# performance is affected (same tradeoff, same warning-not-error posture).
$ffmpegExtraConfigureArgs = ""
if ($Platform -eq 'ARM64') {
    $gasPreprocessorOnPath = [bool](Get-Command gas-preprocessor.pl -ErrorAction SilentlyContinue)
    if (-not $gasPreprocessorOnPath) {
        Write-Warning "gas-preprocessor.pl not found; disabling ARM assembly for the ARM64 ffmpeg build (--disable-asm). Decode performance will be reduced (pure-C decoders only)."
        $ffmpegExtraConfigureArgs = "--disable-asm "
    }
}

# WMV/ASF (2026-07-18 spec): the asf demuxer is enabled in BOTH modes (above,
# in the main demuxer list) -- demuxing ASF is licensing-safe (Microsoft Open
# Specification Promise) and the commercial Windows build needs it to feed
# WMV packets to Media Foundation (CVideoDecoderWMVMF). The WMV/WMA software
# DECODERS (patent-encumbered: VC-1 pool, WMA) are full-mode only. On
# Windows, WMV normally decodes through MF in both modes; the software
# decoders are the parity fallback for N editions without the Media Feature
# Pack.
# (msmpeg4v1/v2/v3: early-era .wmv/.avi coverage, patents expired -- same
# rationale as the macOS script.)
# hevc/aac/eac3 (2026-07-19 codec-superset spec): software fallbacks for
# internal builds only -- used when no HEVC decoder MFT resolves (N edition /
# pre-HEVC GPU) or the native AAC path is unavailable. Native MF decoders
# always win where present. eac3's ac3-core configure dependency is already
# satisfied (ac3 in the base enabled-decoder list, both modes).
# THE MODE, not the tier. $ffmpegBuildMode is derived above from resolved
# MT_FFMPEG_BUILD_MODE, which is `full` only for MT_PRIVATE_BUILD=1. The tier
# ($commercial) answers a different question: the public/free tier is
# COMMERCIAL=0 with mode `commercial`, because patents attach to distribution
# rather than to payment. Reading the tier here handed that tier the withheld
# decoders -- and on this platform $commercial is not even assigned unless the
# legacy fallback branch runs, so a standalone -CapsFile invocation read $null
# and took NEITHER side.
# No mode branch: $ffmpegDecoders and $ffmpegParsers already carry the
# withheld names when the resolved mode is `full`, decided by mtcaps from
# MT_PRIVATE_BUILD. The branch this replaces read the licence TIER.
$ffmpegModeConfigureArgs = ""

# ==================== FFmpeg (built once per Platform) ====================

if (-not $ffmpegUpToDate) {
    Write-Host "`n=== Building FFmpeg ($Platform) ===" -ForegroundColor Cyan

    Remove-Item -Recurse -Force $ffmpegBuildDir, $ffmpegPrefix -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $ffmpegBuildDir, $ffmpegPrefix | Out-Null

    $ffmpegSrc = ConvertTo-MsysPath (Join-Path $srcDir "ffmpeg-$ffmpegVersion")
    $ffmpegBuildPosix = ConvertTo-MsysPath $ffmpegBuildDir
    $ffmpegPrefixPosix = ConvertTo-MsysPath $ffmpegPrefix

    # LGPL-load-bearing configure block: copied verbatim from the macOS
    # script's build_ffmpeg() except for the documented platform tail
    # (--toolchain=msvc --arch=<arch> --target-os=win64, --enable-d3d11va
    # --enable-dxva2 instead of --enable-videotoolbox, no darwin/universal
    # flags). See task-3-report.md for a line-by-line diff proof.
    #
    # NOTE (mpeg4/h263): FFmpeg's mpeg4 decoder select-depends on the h263
    # decoder core (configure: mpeg4_decoder_select="h263_decoder"); h263 is
    # therefore force-enabled. Passing --disable-decoder=h263 disables mpeg4
    # along with it, so it cannot be removed while mpeg4 stays enabled.
    # Approved deviation (H.263 patents expired). The exact resulting decoder
    # set is asserted after configure via Require-FfmpegExactDecoderSet.
    #
    # NOTE (ac3/eac3 dead-code link, owner-approved fix): ac3dec_float.c/
    # ac3dec_fixed.c unconditionally `#include "eac3dec.c"` (upstream FFmpeg
    # composition pattern, not something this script touches) --
    # ff_eac3_apply_spectral_extension() et al. land in the SAME translation
    # unit as the AC3 decoder regardless of CONFIG_EAC3_DECODER, guarded only
    # by a runtime `if (CONFIG_EAC3_DECODER)` (a compile-time-constant 0) --
    # a preprocessor `#if` would be dead-code-eliminated at parse time, but
    # this is a real `if`, evaluated by the OPTIMIZER. GCC/Clang prove it
    # dead and drop the call (and thus never need eac3_data.o's tables, which
    # the Makefile only compiles when CONFIG_EAC3_DECODER=1); MSVC's cl.exe
    # does not (confirmed empirically: a minimal repro still fails identically
    # under /O2, /Gy+link/OPT:REF, and /GL+link/LTCG -- MSVC's linker resolves
    # every external symbol before it ever considers dropping unreferenced
    # code, so no flag combination avoids needing SOME definition for these
    # symbols; this is not a flag-tuning problem).
    #
    # Fix (owner-approved -- see specs/superpowers/plans, Task 9 report):
    # compile eac3_data.c -- 1136 lines of `const` lookup tables, ZERO decoder
    # logic, no AVCodec registration -- as an extra object spliced into just
    # the avcodec DLL link, entirely outside FFmpeg's own Makefile object
    # selection. CONFIG_EAC3_DECODER stays 0 in config_components.h
    # (Require-FfmpegDecoderDisabled/Require-FfmpegExactDecoderSet below still
    # assert this unmodified), no eac3 AVCodec is ever registered or
    # reachable via the public API, and FFmpeg source stays byte-for-byte
    # unmodified upstream -- this is build-script orchestration, not a patch.
    # The exact link command is recovered via `make -n` (a dry run) rather
    # than hand-duplicated here, so it can never drift from whatever
    # LDFLAGS/SHFLAGS the configure line above actually produces.
    $ffmpegConfigureScript = @"
set -euo pipefail
mkdir -p '$ffmpegBuildPosix'
cd '$ffmpegBuildPosix'
'$ffmpegSrc/configure' --prefix='$ffmpegPrefixPosix' \
  --toolchain=msvc --arch=$ffmpegArch --target-os=win64 \
  --enable-shared --disable-static --disable-programs --disable-doc \
  --disable-network --disable-everything \
  --enable-protocol=file \
  --enable-demuxer=$(Policy-Csv $ffmpegDemuxers) \
  --enable-decoder=$(Policy-Csv $ffmpegDecoders) \
  --enable-parser=$(Policy-Csv $ffmpegParsers) \
  --enable-d3d11va --enable-dxva2 \
  --disable-encoders --disable-muxers --disable-filters --disable-bsfs \
  --enable-bsf=hevc_mp4toannexb,h264_mp4toannexb,aac_adtstoasc \
  --disable-devices --disable-autodetect \
  $ffmpegModeConfigureArgs$ffmpegExtraConfigureArgs\
  2>&1 | tee configure.log

set +e
$msysMake -j$Jobs 2>&1 | tee make.log
MAKE_STATUS=`$?
set -e

# NOTE (2026-07-19 codec-superset spec, Task 1 Step 4): in FULL mode the eac3
# decoder is genuinely compiled, ff_eac3_bits_vs_hebap resolves normally, and
# this recovery is expected to never fire (the error-signature grep below
# simply won't match). If the Windows pass ever shows full mode tripping it
# (double-linked eac3_data.o), guard this block on the commercial MODE -- the
# splice is only ever needed for the commercial dead-code link gap.
if [ `$MAKE_STATUS -ne 0 ]; then
  if grep -q 'unresolved external symbol ff_eac3_bits_vs_hebap' make.log; then
    echo '=== Known MSVC/eac3 dead-code link gap (owner-approved fix): linking eac3_data.o (pure tables, no decoder) into avcodec only ==='
    $msysMake libavcodec/eac3_data.o
    LINKCMD=`$($msysMake -n libavcodec/avcodec-61.dll | grep -F '/compat/windows/mslink ')
    if [ -z "`$LINKCMD" ]; then
      echo 'ERROR: could not recover the avcodec-61.dll link command from a make dry-run' >&2
      exit 1
    fi
    LINKCMD=`${LINKCMD/libavcodec\/avcodec.o /libavcodec\/avcodec.o libavcodec\/eac3_data.o }
    eval "`$LINKCMD"
    $msysMake -j$Jobs 2>&1 | tee make-continued.log
  else
    echo 'FFmpeg build failed for a reason other than the known eac3 dead-code link gap -- not auto-recovering.' >&2
    exit `$MAKE_STATUS
  fi
fi

$msysMake install 2>&1 | tee install.log
"@

    $ffmpegScriptPath = Join-Path $ffmpegBuildDir "build.sh"
    New-Item -ItemType Directory -Force -Path $ffmpegBuildDir | Out-Null
    Write-BashScript $ffmpegScriptPath $ffmpegConfigureScript
    Invoke-Msys2Script $ffmpegScriptPath (Join-Path $ffmpegBuildDir "msys2-driver.log")

    # The guard picks its expected set on the same value the configure line
    # used, or it asserts one mode's library against the other's set.
    # ONE expected set per kind, whatever the mode: the policy lists already
    # carry the withheld names in full mode, so the guard no longer picks
    # between two hand-written sets -- which is what it got wrong by picking
    # on the licence tier. Only the absence check is mode-specific.
    Require-FfmpegExactSet $ffmpegBuildDir "DECODER" $ffmpegExpectedDecoders
    Require-FfmpegExactSet $ffmpegBuildDir "PARSER" $ffmpegExpectedParsers
    if ($ffmpegBuildMode -eq "commercial") {
        foreach ($dec in $ffmpegForbiddenDecodersCommercial) {
            Require-FfmpegDecoderDisabled $ffmpegBuildDir $dec
        }
    }
    Require-FfmpegExactSet $ffmpegBuildDir "DEMUXER" $ffmpegExpectedDemuxers

    # The object-file trace scan that stood here is GONE, as L4 (engine
    # 2e35fb25) said it would be and this file alone did not get. Three of its
    # seven names could never have matched -- hevcdec.o and aacdec.o are built
    # under libavcodec/hevc/ and libavcodec/aac/ while it looked in
    # libavcodec/ flat, and eac3dec.o never exists because eac3dec.c is
    # #included by ac3dec_float.c -- and the remaining four were a hand-kept
    # subset of a policy that now has thirteen names and one home. The
    # exact-set and absence checks above read config_components.h, which is
    # what configure actually decided, and scan-forbidden-symbols.ps1 reads
    # the built DLL. A stale seven-name list adds nothing to either.

    # Mode marker: consumed by app build scripts (guard #3 in the 2026-07-18
    # WMV spec). A commercial app build against a "full" install is a fatal
    # error there; this file is how it knows what it is linking.
    $ffmpegBuildMode | Out-File -FilePath (Join-Path $ffmpegPrefix ".ffmpeg-build-mode") -Encoding ASCII -NoNewline

    $ffmpegStampValue | Out-File -FilePath $ffmpegStampFile -Encoding ASCII -NoNewline
    Write-Host "FFmpeg built: $ffmpegPrefix" -ForegroundColor Green
} else {
    Write-Host "FFmpeg is up to date for $Platform, skipping rebuild: $ffmpegPrefix" -ForegroundColor Green
}

# ==================== libvpx (per Platform+Configuration) ====================

Write-Host "`n=== Building libvpx ($Platform $Configuration) ===" -ForegroundColor Cyan

# Wipe build dirs AND the vpx/opus install prefix so stale artifacts from a
# previous run (e.g. an old opus.lib built with different flags) can never
# leak into the combined MTVideoCodecs.lib below.
Remove-Item -Recurse -Force $vpxBuildDir, $opusBuildDir, $vpxOpusPrefixDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $vpxBuildDir | Out-Null

$vpxSrc = ConvertTo-MsysPath (Join-Path $srcDir "libvpx-$vpxVersion")
$vpxBuildPosix = ConvertTo-MsysPath $vpxBuildDir

# libvpx's *-vs17 configure targets are invoked via a POSIX shell (configure
# is itself a bash script). Unlike the Unix targets, `make` here does NOT
# compile anything itself -- per solution.mk / build/make/gen_msvs_*.sh in
# the libvpx tree, the default `make` target's job is to *generate*
# vpx.sln + per-module .vcxproj files (by shelling out to
# gen_msvs_proj.sh/gen_msvs_sln.sh), which are then compiled with msbuild.exe
# directly (below) -- this matches libvpx's documented Windows/MSVC build
# flow (configure && make && msbuild vpx.sln). `make` therefore needs to run
# inside the same MSYS2 shell as configure (hence the prereq: pacman -S make
# diffutils pkgconf). Configure flags mirror the macOS build_libvpx() exactly,
# minus --enable-pic (Mach-O/ELF-only concept, meaningless for PE/COFF) and
# --prefix (the vs17 generator has no install step; outputs are copied
# manually below).
$vpxConfigureScript = @"
set -euo pipefail
mkdir -p '$vpxBuildPosix'
cd '$vpxBuildPosix'
'$vpxSrc/configure' \
  --target=$vpxTarget \
  --disable-examples \
  --disable-tools \
  --disable-docs \
  --disable-unit-tests \
  --disable-install-bins \
  --enable-vp9 \
  --enable-vp9-highbitdepth \
  --disable-vp8-encoder \
  --disable-vp8-decoder \
  --disable-vp9-encoder \
  2>&1 | tee configure.log

$msysMake -j$Jobs 2>&1 | tee make.log
"@

$vpxScriptPath = Join-Path $vpxBuildDir "configure.sh"
Write-BashScript $vpxScriptPath $vpxConfigureScript
Invoke-Msys2Script $vpxScriptPath (Join-Path $vpxBuildDir "msys2-driver.log")

# NOTE (make with NO GOAL, not `make vpx.sln`): the generated top-level Makefile
# only defines a real "vpx.sln" target once $(target) is set (it conditionally
# includes "$(target)-$(TOOLCHAIN).mk", see its own comment "we invoke make
# recursively for multiple targets"); with $(target) empty -- our case, since
# we never pass target=... -- "vpx.sln" matches no rule at all and GNU Make's
# .DEFAULT catch-all does NOT fire for an arbitrary named goal like this,
# so `make vpx.sln` fails with "No rule to make target 'vpx.sln'" (confirmed
# on first Windows run). Bare `make` (no goal) hits the Makefile's own
# ifeq($(target),)/.DEFAULT block instead, which loops
# `$(MAKE) target=$$t` over $(ALL_TARGETS) ("libs solution" for this
# configure) -- solution.mk's real `vpx.sln: $(wildcard *.vcxproj)` rule only
# gets included via that recursion. Side effect (also confirmed on first
# run): the "libs" sub-target's own default goal ALSO invokes msbuild itself,
# producing an incidental ARM64EC|Debug build regardless of -Platform/
# -Configuration -- harmless (~20s, not fatal) but its .lib must not be
# mistaken for the one OUR msbuild call below produces, hence the
# Configuration/Platform-qualified search first.
$vpxSln = Get-ChildItem -Path $vpxBuildDir -Filter "*.sln" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $vpxSln) { throw "libvpx 'make' did not produce a .sln in $vpxBuildDir" }

Write-Host "Building $($vpxSln.FullName) ($Configuration|$msbuildPlat)"
& msbuild $vpxSln.FullName "/p:Configuration=$Configuration" "/p:Platform=$msbuildPlat" "/m:$Jobs" "/nologo"
if ($LASTEXITCODE -ne 0) { throw "msbuild failed for $($vpxSln.FullName)" }

# Prefer a .lib under a path naming OUR OWN Configuration+Platform (what the
# msbuild invocation just above produces) so the incidental ARM64EC/Debug
# build the bare `make` step triggers (see NOTE above) can never be picked up
# by accident; only fall back to the loose match if that's somehow absent
# (deliberately tolerant otherwise, same technique build-image_codecs.ps1
# uses for LibRaw/gav1 -- exact output layout not something to hardcode
# further than necessary).
$vpxLibFound = Get-ChildItem -Path $vpxBuildDir -Recurse -Filter "*.lib" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match "vpx" -and $_.FullName -match [regex]::Escape("\$msbuildPlat\$Configuration\") } |
    Select-Object -First 1
if (-not $vpxLibFound) {
    $vpxLibFound = Get-ChildItem -Path $vpxBuildDir -Recurse -Filter "*.lib" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match "vpx" } | Select-Object -First 1
}
if (-not $vpxLibFound) { throw "No vpx*.lib produced under $vpxBuildDir" }

New-Item -ItemType Directory -Force -Path "$vpxOpusPrefixDir\lib", "$vpxOpusPrefixDir\include\vpx" | Out-Null
Copy-Item $vpxLibFound.FullName "$vpxOpusPrefixDir\lib\vpx.lib" -Force

# Headers: libvpx's normal `make install` would copy vpx/*.h (API headers)
# plus the configure-generated vpx_config.h / vpx_version.h. The vs17
# generator skips that install step, so replicate it manually.
Copy-Item (Join-Path $srcDir "libvpx-$vpxVersion\vpx\*.h") "$vpxOpusPrefixDir\include\vpx\" -Force
foreach ($generated in @("vpx_config.h", "vpx_version.h")) {
    $genPath = Get-ChildItem -Path $vpxBuildDir -Recurse -Filter $generated -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($genPath) {
        Copy-Item $genPath.FullName "$vpxOpusPrefixDir\include\vpx\$generated" -Force
    } else {
        Write-Warning "$generated not found under $vpxBuildDir -- verify on first Windows run."
    }
}

# ==================== opus (per Platform+Configuration, CMake+MSVC) ====================

Write-Host "`n=== Building opus ($Platform $Configuration) ===" -ForegroundColor Cyan

Build-CMake `
    (Join-Path $srcDir "opus-$opusVersion") `
    $opusBuildDir `
    $vpxOpusPrefixDir `
    @(
        "-DOPUS_BUILD_SHARED_LIBRARY=OFF",
        "-DOPUS_BUILD_TESTING=OFF",
        "-DOPUS_BUILD_PROGRAMS=OFF",
        # opus-1.5.2/CMakeLists.txt:274-279 OVERWRITES CMAKE_MSVC_RUNTIME_LIBRARY
        # from its own OPUS_STATIC_RUNTIME option, which defaults to OFF -- so
        # Build-CMake's -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded was silently
        # discarded and opus.lib came out /MD while the engine and every app
        # link /MT. That mismatch put two CRTs (two heaps) in one image and
        # raised LNK4098 "LIBCMT conflicts with use of other libs". Setting the
        # option opus actually reads is the only way to make the request stick.
        "-DOPUS_STATIC_RUNTIME=ON"
    )

# ==================== combine vpx + opus into MTVideoCodecs.lib ====================

Write-Host "`n=== Combining MTVideoCodecs.lib ($Platform $Configuration) ===" -ForegroundColor Cyan

$vpxOpusLibDir = Join-Path $vpxOpusPrefixDir "lib"
$allFoundLibs = Get-ChildItem -Path $vpxOpusLibDir -Recurse -Filter "*.lib" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }

$wantedLibs = @("vpx", "opus")
$inputLibs = @()
foreach ($name in $wantedLibs) {
    $found = $allFoundLibs | Where-Object { $_ -match "[\\/]${name}\.lib$" } | Select-Object -First 1
    if ($found) {
        $inputLibs += $found
        Write-Host "Found: $found"
    } else {
        Write-Warning "Library not found: $name.lib -- skipping"
    }
}

if ($inputLibs.Count -eq 0) {
    Write-Error "No input libraries found to combine"
    exit 1
}

Remove-Item -Force $outLib -ErrorAction SilentlyContinue
$libArgs = @("/NOLOGO", "/OUT:$outLib") + $inputLibs
& lib @libArgs
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed to create $outLib" }

Require-Symbol $outLib "vpx_codec_dec_init_ver"
Require-Symbol $outLib "vpx_codec_decode"
Require-Symbol $outLib "vpx_codec_destroy"
Require-Symbol $outLib "vpx_codec_err_to_string"
Require-Symbol $outLib "vpx_codec_get_frame"
Require-Symbol $outLib "vpx_codec_vp9_dx"
Require-Symbol $outLib "opus_decoder_create"
Require-Symbol $outLib "opus_decode_float"
Require-Symbol $outLib "opus_decoder_ctl"
Require-Symbol $outLib "opus_decoder_destroy"
Require-Symbol $outLib "opus_strerror"

# ==================== merge vpx/opus headers into the FFmpeg include root ====================

# Single include root for consumers, mirroring merge_ffmpeg_headers() on
# macOS (there it merges per-arch ffmpeg headers; here ffmpeg headers are
# already single-arch, so this step only needs to fold in vpx/opus).
#
# Idempotency: Copy-Item <srcDir> <dstDir> -Recurse nests the source INSIDE
# the destination when the destination directory already exists (producing
# include\vpx\vpx\*.h on a second run for the same Platform -- which is a
# normal workflow, since the other -Configuration reuses this FFmpeg
# install). Remove the destination subtree first so re-runs converge.
$ffmpegIncludeDir = Join-Path $ffmpegPrefix "include"
New-Item -ItemType Directory -Force -Path $ffmpegIncludeDir | Out-Null

$vpxIncludeDst = Join-Path $ffmpegIncludeDir "vpx"
Remove-Item -Recurse -Force $vpxIncludeDst -ErrorAction SilentlyContinue
Copy-Item (Join-Path $vpxOpusPrefixDir "include\vpx") $vpxIncludeDst -Recurse -Force

$opusIncludeSrc = Join-Path $vpxOpusPrefixDir "include\opus"
$opusIncludeDst = Join-Path $ffmpegIncludeDir "opus"
if (Test-Path $opusIncludeSrc) {
    Remove-Item -Recurse -Force $opusIncludeDst -ErrorAction SilentlyContinue
    Copy-Item $opusIncludeSrc $opusIncludeDst -Recurse -Force
} else {
    Write-Warning "$opusIncludeSrc not found -- opus CMake install layout may differ; verify on first Windows run."
}

# ==================== stamp ====================

$stampValue | Out-File -FilePath $stampFile -Encoding ASCII -NoNewline

Write-Host "`nVideo codec bundle built: $outLib" -ForegroundColor Green
Write-Host "FFmpeg LGPL decode DLLs: $ffmpegPrefix\bin (avcodec/avformat/avutil/swscale/swresample)" -ForegroundColor Green

} finally {
    # Runs on `exit` and on a terminating error alike.
    Complete-MTStore
}
