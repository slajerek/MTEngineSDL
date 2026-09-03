<#
.SYNOPSIS
    Build static image codec libraries for Windows (TIFF, WebP, AVIF, LibRaw, lcms2).
.DESCRIPTION
    Downloads and compiles libtiff, libwebp, libavif (with embedded libgav1),
    LibRaw and lcms2 as static libraries, then combines them into a single
    MTImageCodecs.lib.
    Output: platform/Windows/libs/<Platform>/<Configuration>/MTImageCodecs.lib
.PARAMETER Platform
    Target architecture: x64 or ARM64. Default: auto-detect.
.PARAMETER Configuration
    Build configuration: Debug or Release. Default: Release.
.PARAMETER Jobs
    Number of parallel build jobs. Default: number of logical processors.
.EXAMPLE
    .\build-image_codecs.ps1
    .\build-image_codecs.ps1 -Platform ARM64 -Configuration Debug
#>
param(
    [ValidateSet('x64','ARM64')]
    [string]$Platform,

    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release',

    [int]$Jobs = $env:NUMBER_OF_PROCESSORS,

    [switch]$Help,

    # Where this script stages its archive. Supplied by build-deps.ps1 or by an
    # app wrapper, both from the single mtcaps resolve they already do.
    [string]$OutLibDir
)

if ($Help) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\mt-build-common.ps1"

# Ensure TLS 1.2 for Invoke-WebRequest downloads
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# lib.exe (the VC librarian used below to combine the built archives) lives
# under VC\Tools\MSVC\<ver>\bin\HostX64\x64\, which is on PATH only inside a
# "Developer" shell (vcvarsall.bat). This script is invoked from a plain
# PowerShell session by build-windows.ps1, so resolve it explicitly the same
# way that script resolves MSBuild.exe, rather than relying on ambient PATH.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$libExe = & $vswhere -latest -requires Microsoft.Component.MSBuild `
    -find "VC\Tools\MSVC\**\bin\HostX64\x64\lib.exe" 2>$null | Select-Object -First 1
if (-not $libExe) {
    Write-Error "lib.exe not found. Install Visual Studio 2022 with C++ workload."
    exit 1
}

# NOT $env:PROCESSOR_ARCHITECTURE, which describes the PROCESS and is inherited:
# under Git Bash (an emulated x64 build) it says AMD64 on an ARM64 machine.
$Platform = Resolve-MTPlatform $Platform

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Resolve-Path "$scriptDir\..\.."
$cacheDir = Get-MTCapsWorkDir 'image-codecs'
$patchesDir = "$rootDir\other\lib\image-codecs\patches"
$downloadDir = "$cacheDir\downloads"
$srcDir = "$cacheDir\src"
$buildDir = "$cacheDir\build-win-$Platform"
$prefixDirLabel = "install-win-$Platform"
$prefixDir = "$cacheDir\$prefixDirLabel"
# REQUIRED, not defaulted. The caller decides where archives go; the only
# fallback in the system lives in build-deps.ps1, so the old
# platform\Windows\libs path cannot creep back in here -- that directory holds
# 25 TRACKED prebuilts and a build must not write into it.
if (-not $OutLibDir) { throw "-OutLibDir is required. Run this through build-deps.ps1, which resolves it." }
# Per-unit store (L16). The unit builds in a directory keyed only by the
# capabilities IT reads, then its outputs are copied into the shared view. The
# body is wrapped in try/finally because a stamp hit and a capability-off stub
# both leave early and both still owe the view a copy.
$outLibDir = Use-MTStore -Unit 'image_codecs' -View $OutLibDir
try {
$outLib = "$outLibDir\MTImageCodecs.lib"
$stampFile = "$outLibDir\MTImageCodecs.stamp"

$tiffVersion    = "4.7.1"
$webpVersion    = "1.6.0"
$avifVersion    = "1.4.2"
$libgav1Version = "0.20.0"
$librawVersion  = "0.22.1"
# libjxl decodes JPEG XL DNGs (Compression 52546), which current Adobe DNG
# Converter writes from its plain "lossy compression" option. Pinned by
# REVISION, with each submodule revision asserted -- see the macOS script and
# the photo app specs/superpowers/specs/2026-08-19-jpegxl-dng-*.md.
$libjxlVersion  = "0.11.2"
$libjxlGitUrl   = "https://github.com/libjxl/libjxl"
$libjxlGitRev   = "332feb17d17311c748445f7ee75c4fb55cc38530"
$libjxlHighwayRev = "457c891775a7397bdb0376bb1031e6e027af1c48"
$libjxlBrotliRev  = "36533a866ed1ca4b75cf049f4521e4ec5fe24727"
$libjxlSkcmsRev   = "b2e692629c1fb19342517d7fb61f1cf83d075492"
# lcms2 is the colour-management backend selectable alongside ICM 2.0/WCS 1.0
# on Windows (CmsEngineVariant::CMS_VARIANT_LCMS2). 2.19 is the first release
# with a root CMakeLists.txt -- matches the version pinned by the Linux build.
$lcms2Version   = "2.19.1"

$tiffUrl    = "https://download.osgeo.org/libtiff/tiff-$tiffVersion.tar.gz"
$webpUrl    = "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$webpVersion.tar.gz"
$avifUrl    = "https://github.com/AOMediaCodec/libavif/archive/refs/tags/v$avifVersion.tar.gz"
$librawUrl  = "https://www.libraw.org/data/LibRaw-$librawVersion.tar.gz"
$lcms2Url   = "https://github.com/mm2/Little-CMS/archive/refs/tags/lcms$lcms2Version.tar.gz"

$tiffHash    = "f698d94f3103da8ca7438d84e0344e453fe0ba3b7486e04c5bf7a9a3fabe9b69"
$webpHash    = "e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564"
$avifHash    = "2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d"
$librawHash  = "a789dc4e2409e2901d93793a4e0b80c7b49d0d97cf6ad71c850eb7616acfd786"
# Computed from the same GitHub tag tarball the Linux build script downloads
# -- cross-verified with both sha256sum and Get-FileHash against an identical
# download, so this is a real pin, not a guess. The Linux script's own
# LCMS2_SHA256 was left empty when this was written; it was pinned to this
# same value on 2026-08-18 by the first real Linux run, which independently
# downloaded and hashed the tarball and got a byte-for-byte match.
$lcms2Hash   = "267705e278e2f7c2fb886c259dadcbaeb2be52748bcbc71c79f08aacacb7a709"
$libgav1GitUrl = "https://chromium.googlesource.com/codecs/libgav1"
$libgav1GitRev = "c05bf9be660cf170d7c26bd06bb42b3322180e58"

New-Item -ItemType Directory -Force -Path $downloadDir, $srcDir, $outLibDir | Out-Null

# Stale stamp check -- if versions changed, force rebuild
$stampValue = "$tiffVersion|$webpVersion|$avifVersion|$libgav1Version|$librawVersion|$libjxlVersion|$lcms2Version|$Platform|$Configuration"
$scriptHash = (Get-FileHash -Path $MyInvocation.MyCommand.Path -Algorithm SHA256).Hash
$stampValue = "$scriptHash|$stampValue"

# THE CAPABILITY GATE.
#
# Unlike FTXUI, llama.cpp and mbedTLS, this bundle had no gate on EITHER
# platform: the macOS script builds it unconditionally too, which is why a C64
# debugger with MT_CAP_RAW=0 and MT_CAP_PHOTO_CODECS=0 still downloaded and
# compiled LibRaw, libtiff, libwebp, libavif, libgav1 and libjxl.
#
# The bundle is one archive serving six flags, so it is skipped only when EVERY
# one of them is off. Any single codec still wanted means building the bundle --
# there is no per-codec archive to stub.
#
# libjxl has no capability of its own: it exists to decode JPEG XL DNGs, which is
# a RAW concern and therefore travels with MT_ENABLE_LIBRAW.
$codecFlags = @('MT_ENABLE_LIBTIFF','MT_ENABLE_LIBWEBP','MT_ENABLE_LIBAVIF',
                'MT_ENABLE_LIBHEIF','MT_ENABLE_LIBRAW','MT_ENABLE_LCMS2')
$anyCodecWanted = $false
foreach ($flag in $codecFlags) {
    # Absent means ON, matching the standalone default in Import-MTCapsEnvironment.
    if ((Get-Item -Path "env:$flag" -ErrorAction SilentlyContinue).Value -ne '0') {
        $anyCodecWanted = $true
        break
    }
}

if (-not $anyCodecWanted) {
    if (-not (Get-Command cl -ErrorAction SilentlyContinue) -or
        -not (Get-Command lib -ErrorAction SilentlyContinue)) {
        throw "Image codecs are disabled by the capability set, but cl/lib are needed to write the stub archive. Run from 'Developer PowerShell for VS 2022'."
    }
    New-MTCapsStubArchive -OutLib $outLib -Stamp $stampFile -Symbol 'mt_image_codecs' `
                          -StampValue "disabled|$scriptHash|$Platform|$Configuration"
    exit 0
}

if ((Test-Path $outLib) -and (Test-Path $stampFile)) {
    $existingStamp = Get-Content $stampFile -Raw
    if ($existingStamp.Trim() -eq $stampValue) {
        # Stamp hit: backfill the Phase 2 header staging into this keyed bucket.
        $stagedInc = Join-Path $outLibDir 'image-codecs\include'
        $prefixInc = Join-Path $cacheDir "$prefixDirLabel\include"
        if (-not (Test-Path $stagedInc) -and (Test-Path $prefixInc)) {
            New-Item -ItemType Directory -Force -Path (Split-Path $stagedInc) | Out-Null
            Copy-Item -Recurse -Force $prefixInc $stagedInc
        }
        Write-Host "Image codec bundle is up to date: $outLib" -ForegroundColor Green
        exit 0
    }
}

# ---- helpers ----

function Get-Sha256($filePath) {
    return (Get-FileHash -Path $filePath -Algorithm SHA256).Hash.ToLower()
}

function Download-Archive($name, $url, $expectedHash) {
    $archive = Join-Path $downloadDir "$name.tar.gz"
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
# raising an error", not "the tree is complete". Same function, same reasoning,
# as build-video_codecs.ps1's copy -- read the longer note there.
function Assert-Extracted($name, $dest, [string[]]$Expect) {
    foreach ($rel in $Expect) {
        $path = Join-Path $dest $rel
        if (-not (Test-Path $path)) {
            throw "$name is not fully extracted: '$path' is missing. Delete '$dest' and re-run to extract it again. (The extraction step reported success; this check exists because that report is not sufficient.)"
        }
    }
}

function Extract-Archive($name, $topDir, [string[]]$Expect) {
    $archive = Join-Path $downloadDir "$name.tar.gz"
    $dest = Join-Path $srcDir $topDir
    # THE EARLY-OUT, SAID OUT LOUD. The destination directory existing is the
    # ONLY thing that makes extraction a once-per-cache event, so a HALF
    # extracted tree is permanent -- every later run returns here and fails
    # later, somewhere that says nothing about extraction. $Expect runs on this
    # cached path as well as the freshly extracted one, so a cache poisoned by
    # an earlier failed run is reported HERE.
    if (Test-Path $dest) {
        Assert-Extracted $name $dest $Expect
        return
    }
    Write-Host "Extracting $name..."
    # Extract into $srcDir so the tarball's top-level dir creates $dest naturally
    # (avoids double nesting like src/tiff-4.7.1/tiff-4.7.1/)
    # --force-local prevents Cygwin/MSYS2 tar from interpreting C:\ as host:path
    & tar --force-local -xzf "$archive" -C "$srcDir" 2>&1
    if ($LASTEXITCODE -ne 0) {
        # Fallback: BSD tar (Windows built-in) doesn't support --force-local
        & tar -xzf "$archive" -C "$srcDir"
        if ($LASTEXITCODE -ne 0) { throw "Failed to extract $archive" }
    }
    Assert-Extracted $name $dest $Expect
}

# git writes routine progress ("Cloning into...", "Submodule '...' registered
# for path...", detached-HEAD notes) to stderr. With script-global
# $ErrorActionPreference = 'Stop', PowerShell treats ANY stderr line from a
# native command as a terminating NativeCommandError regardless of exit code
# -- same pathology already worked around for the three cmake calls in
# Build-CMake (see its comment), just not yet for git. Relax to 'Continue' for
# the duration of the call so routine stderr chatter can't masquerade as
# failure; $LASTEXITCODE remains the real, sufficient success/failure signal.
function Invoke-Git {
    param([Parameter(Mandatory)][string[]]$GitArgs, [string]$Context)
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & git @GitArgs
        if ($LASTEXITCODE -ne 0) {
            $ctx = if ($Context) { " for $Context" } else { "" }
            throw "git $($GitArgs -join ' ') failed$ctx"
        }
    } finally {
        $ErrorActionPreference = $savedEap
    }
}

function Clone-GitTag($name, $url, $tag, $expectedRev) {
    $dest = Join-Path $srcDir $name
    if (Test-Path (Join-Path $dest ".git")) {
        $actualRev = Invoke-Git @('-C', $dest, 'rev-parse', 'HEAD')
        if ($actualRev.Trim() -eq $expectedRev) { return }
    }
    Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
    Write-Host "Cloning $name..."
    Invoke-Git @('clone', '--depth', '1', '--branch', $tag, $url, $dest) $name
    $actualRev = Invoke-Git @('-C', $dest, 'rev-parse', 'HEAD')
    if ($actualRev.Trim() -ne $expectedRev) {
        Write-Error "Unexpected git rev for $name`nExpected: $expectedRev`nActual:   $actualRev"
        exit 1
    }
}

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
    # -- regardless of exit code, even with no explicit 2>&1 redirection
    # (confirmed on first ARM64 run: cmake's own non-fatal "CMake Warning:"
    # after a successful "-- Generating done" aborted the script outright;
    # same issue and same fix as build-video_codecs.ps1's Build-CMake).
    # Relax to 'Continue' for just these three native calls so a benign
    # stderr write can't masquerade as failure; $LASTEXITCODE below remains
    # the real, sufficient success/failure signal.
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

# ---- download & extract ----

Download-Archive "tiff-$tiffVersion"    $tiffUrl    $tiffHash
Download-Archive "libwebp-$webpVersion" $webpUrl    $webpHash
Download-Archive "libavif-$avifVersion" $avifUrl    $avifHash
Download-Archive "LibRaw-$librawVersion" $librawUrl $librawHash
Download-Archive "lcms$lcms2Version"    $lcms2Url   $lcms2Hash

Extract-Archive "tiff-$tiffVersion"    "tiff-$tiffVersion" @("CMakeLists.txt")
Extract-Archive "libwebp-$webpVersion" "libwebp-$webpVersion" @("CMakeLists.txt")
Extract-Archive "libavif-$avifVersion" "libavif-$avifVersion" @("CMakeLists.txt")
Clone-GitTag "libgav1-v$libgav1Version" $libgav1GitUrl "v$libgav1Version" $libgav1GitRev
Extract-Archive "LibRaw-$librawVersion" "LibRaw-$librawVersion" @("CMakeLists.txt")
Extract-Archive "lcms$lcms2Version"    "Little-CMS-lcms$lcms2Version" @("CMakeLists.txt")

# Link libgav1 into avif ext dir
$libgav1Src = Join-Path $srcDir "libgav1-v$libgav1Version"
$avifExtLibGav1 = Join-Path $srcDir "libavif-$avifVersion\ext\libgav1"
Remove-Item -Recurse -Force $avifExtLibGav1 -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $srcDir "libavif-$avifVersion\ext") | Out-Null
Copy-Item -Recurse $libgav1Src $avifExtLibGav1

# Clean previous builds
Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $prefixDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $buildDir, $prefixDir | Out-Null

# ---- build each library ----

$cmakeGenerator = "Visual Studio 17 2022"

# TIFF
Build-CMake `
    (Join-Path $srcDir "tiff-$tiffVersion") `
    (Join-Path $buildDir "tiff") `
    $prefixDir `
    @(
        "-DBUILD_SHARED_LIBS=OFF",
        "-Dtiff-tools=OFF",
        "-Dtiff-tests=OFF",
        "-Dtiff-contrib=OFF",
        "-Dtiff-docs=OFF",
        "-Dtiff-cxx=OFF",
        "-Dzlib=OFF",
        "-Dlibdeflate=OFF",
        "-Djpeg=OFF",
        "-Dold-jpeg=OFF",
        "-Djpeg12=OFF",
        "-Djbig=OFF",
        "-Dlerc=OFF",
        "-Dlzma=OFF",
        "-Dzstd=OFF",
        "-Dwebp=OFF"
    )

# WebP
Build-CMake `
    (Join-Path $srcDir "libwebp-$webpVersion") `
    (Join-Path $buildDir "webp") `
    $prefixDir `
    @(
        "-DWEBP_BUILD_ANIM_UTILS=OFF",
        "-DWEBP_BUILD_CWEBP=OFF",
        "-DWEBP_BUILD_DWEBP=OFF",
        "-DWEBP_BUILD_GIF2WEBP=OFF",
        "-DWEBP_BUILD_IMG2WEBP=OFF",
        "-DWEBP_BUILD_VWEBP=OFF",
        "-DWEBP_BUILD_WEBPINFO=OFF",
        "-DWEBP_BUILD_LIBWEBPMUX=OFF",
        "-DWEBP_BUILD_WEBPMUX=OFF",
        "-DWEBP_BUILD_EXTRAS=OFF",
        "-DWEBP_BUILD_WEBP_JS=OFF",
        "-DWEBP_BUILD_FUZZTEST=OFF",
        "-DWEBP_LINK_STATIC=ON"
    )

# AVIF with embedded libgav1
Build-CMake `
    (Join-Path $srcDir "libavif-$avifVersion") `
    (Join-Path $buildDir "avif") `
    $prefixDir `
    @(
        "-DAVIF_BUILD_APPS=OFF",
        "-DAVIF_BUILD_TESTS=OFF",
        "-DAVIF_BUILD_EXAMPLES=OFF",
        "-DAVIF_BUILD_MAN_PAGES=OFF",
        "-DAVIF_CODEC_AOM=OFF",
        "-DAVIF_CODEC_DAV1D=OFF",
        "-DAVIF_CODEC_LIBGAV1=LOCAL",
        "-DAVIF_CODEC_RAV1E=OFF",
        "-DAVIF_CODEC_SVT=OFF",
        "-DAVIF_ZLIBPNG=OFF",
        "-DAVIF_JPEG=OFF",
        "-DAVIF_LIBYUV=OFF",
        "-DAVIF_LIBSHARPYUV=OFF",
        "-DAVIF_LIBXML2=OFF"
    )

# RD-A: build the engine's own vendored jpeg-9a and zlib as static libs so
# LibRaw's full decode gains USE_JPEG/USE_ZLIB from the one in-repo copy of
# each library (roadmap #2.11). CMake shims live in the build cache and point
# at the engine tree; the archives are merged into MTImageCodecs.lib below so
# the bundle stays self-contained (RD-A design #4.2.1 route (a)).
$engineJpegSrc = ("$rootDir\src\Engine\Libs\jpeg\jpeg-9a" -replace '\\','/')
$engineZlibSrc = ("$rootDir\src\Engine\Libs\zlib" -replace '\\','/')
# The engine's zlib carries a deliberate unconditional '#include <unistd.h>'
# in gzlib/gzread/gzwrite (POSIX prototypes); MSVC resolves it via the
# engine's shim header, which must therefore be on THIS shim's include path
# too or the standalone build dies at C1083 before ever reaching LibRaw.
$engineWinShim = ("$rootDir\platform\Windows\src.Windows" -replace '\\','/')

$engineJpegShim = Join-Path $srcDir "engine-jpeg"
New-Item -ItemType Directory -Force -Path $engineJpegShim | Out-Null
@"
cmake_minimum_required(VERSION 3.10)
project(enginejpeg C)
file(GLOB JPEG_SOURCES "$engineJpegSrc/*.c")
add_library(jpeg STATIC `${JPEG_SOURCES})
target_include_directories(jpeg PRIVATE "$engineJpegSrc")
install(TARGETS jpeg ARCHIVE DESTINATION lib)
install(FILES "$engineJpegSrc/jpeglib.h" "$engineJpegSrc/jconfig.h"
              "$engineJpegSrc/jmorecfg.h" "$engineJpegSrc/jerror.h"
        DESTINATION include)
"@ | Out-File -FilePath (Join-Path $engineJpegShim "CMakeLists.txt") -Encoding UTF8

$engineZlibShim = Join-Path $srcDir "engine-zlib"
New-Item -ItemType Directory -Force -Path $engineZlibShim | Out-Null
@"
cmake_minimum_required(VERSION 3.10)
project(enginezlib C)
file(GLOB ZLIB_SOURCES "$engineZlibSrc/*.c")
add_library(zlib STATIC `${ZLIB_SOURCES})
target_include_directories(zlib PRIVATE "$engineZlibSrc" "$engineWinShim")
install(TARGETS zlib ARCHIVE DESTINATION lib)
install(FILES "$engineZlibSrc/zlib.h" "$engineZlibSrc/zconf.h" DESTINATION include)
"@ | Out-File -FilePath (Join-Path $engineZlibShim "CMakeLists.txt") -Encoding UTF8

Build-CMake $engineJpegShim (Join-Path $buildDir "engine-jpeg") $prefixDir @()
Build-CMake $engineZlibShim (Join-Path $buildDir "engine-zlib") $prefixDir @()

# --- libjxl -----------------------------------------------------------------
# Cloned rather than downloaded: the three submodules a decoder-only build
# links are not in the release tarball.
$libjxlSrc = Join-Path $srcDir "libjxl-$libjxlVersion"
$needClone = $true
if (Test-Path (Join-Path $libjxlSrc ".git")) {
    $rev = (Invoke-Git @('-C', $libjxlSrc, 'rev-parse', 'HEAD')).Trim()
    if ($rev -eq $libjxlGitRev) { $needClone = $false }
}
if ($needClone) {
    Remove-Item -Recurse -Force $libjxlSrc -ErrorAction SilentlyContinue
    Write-Host "Checking out libjxl $libjxlVersion..."
    Invoke-Git @('clone', '--quiet', $libjxlGitUrl, $libjxlSrc) 'libjxl'
    Invoke-Git @('-C', $libjxlSrc, 'checkout', '--quiet', $libjxlGitRev) 'libjxl'
    Invoke-Git @('-C', $libjxlSrc, 'submodule', 'update', '--init', '--depth', '1', 'third_party/highway', 'third_party/brotli', 'third_party/skcms') 'libjxl'
}
# Assert the submodule revisions: a moved pointer changes both the shipped code
# and the licence set with no other signal.
foreach ($sm in @(@("highway", $libjxlHighwayRev), @("brotli", $libjxlBrotliRev), @("skcms", $libjxlSkcmsRev))) {
    $smPath = Join-Path $libjxlSrc "third_party\$($sm[0])"
    $smRev = (Invoke-Git @('-C', $smPath, 'rev-parse', 'HEAD')).Trim()
    if ($smRev -ne $sm[1]) {
        throw "libjxl submodule $($sm[0]) is at $smRev, expected $($sm[1])"
    }
}

# DECODER ONLY -- every tool, test, plugin and encoder-side extra is off.
Build-CMake `
    $libjxlSrc `
    (Join-Path $buildDir "libjxl") `
    $prefixDir `
    @(
        "-DBUILD_TESTING=OFF",
        "-DJPEGXL_STATIC=OFF",
        "-DJPEGXL_ENABLE_TOOLS=OFF",
        "-DJPEGXL_ENABLE_VIEWERS=OFF",
        "-DJPEGXL_ENABLE_DEVTOOLS=OFF",
        "-DJPEGXL_ENABLE_EXAMPLES=OFF",
        "-DJPEGXL_ENABLE_BENCHMARK=OFF",
        "-DJPEGXL_ENABLE_FUZZERS=OFF",
        "-DJPEGXL_ENABLE_MANPAGES=OFF",
        "-DJPEGXL_ENABLE_DOXYGEN=OFF",
        "-DJPEGXL_ENABLE_JNI=OFF",
        "-DJPEGXL_ENABLE_PLUGINS=OFF",
        "-DJPEGXL_ENABLE_OPENEXR=OFF",
        "-DJPEGXL_ENABLE_SJPEG=OFF",
        "-DJPEGXL_ENABLE_JPEGLI=OFF",
        "-DJPEGXL_ENABLE_JPEGLI_LIBJPEG=OFF",
        "-DJPEGXL_ENABLE_TRANSCODE_JPEG=OFF",
        "-DJPEGXL_ENABLE_SKCMS=ON",
        "-DJPEGXL_BUNDLE_SKCMS=ON",
        "-DJPEGXL_ENABLE_TCMALLOC=OFF",
        "-DJPEGXL_FORCE_SYSTEM_BROTLI=OFF",
        "-DJPEGXL_FORCE_SYSTEM_HWY=OFF",
        "-DJPEGXL_BUNDLE_LIBPNG=OFF",
        "-DJPEGXL_ENABLE_COVERAGE=OFF",
        "-DJPEGXL_WARNINGS_AS_ERRORS=OFF"
    )

# --- patch LibRaw for JPEG XL ------------------------------------------------
# One patcher shared by all three platforms, so the decoder cannot drift
# between them. It is idempotent and fails loudly if a LibRaw bump moves its
# anchors. NEEDS PYTHON 3 -- the only Python dependency in the Windows build;
# reimplementing it in PowerShell would mean two copies of the same edit and
# exactly the drift this avoids.
$pythonExe = $null
# A bare PATH hit isn't enough: when no Python is installed via the Microsoft
# Store, Windows still puts a "python3"/"python" App Execution Alias stub on
# PATH under WindowsApps that Get-Command happily finds but that does nothing
# but print an install nag to stderr and exit non-zero -- and with
# $ErrorActionPreference = 'Stop' script-wide, that stderr line alone throws a
# terminating NativeCommandError, same pathology as Invoke-Git above, so this
# probe needs the same relaxation. Only trust a candidate that actually runs.
$savedEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    foreach ($cand in @("python3", "python", "py")) {
        if (-not (Get-Command $cand -ErrorAction SilentlyContinue)) { continue }
        & $cand --version 2>$null 1>$null
        if ($LASTEXITCODE -eq 0) { $pythonExe = $cand; break }
    }
} finally {
    $ErrorActionPreference = $savedEap
}
if (-not $pythonExe) {
    throw "Python 3 is required to patch LibRaw for JPEG XL DNG support. Install it from python.org or the Microsoft Store, or run: winget install Python.Python.3"
}
$patcher = Join-Path $patchesDir "apply_libraw_jxl.py"
Write-Host "Patching LibRaw for JPEG XL..."
$savedEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    & $pythonExe $patcher (Join-Path $srcDir "LibRaw-$librawVersion")
    if ($LASTEXITCODE -ne 0) { throw "apply_libraw_jxl.py failed" }
} finally {
    $ErrorActionPreference = $savedEap
}

# LibRaw does not ship CMakeLists.txt (removed since 2014). Generate one.
$librawSrc = Join-Path $srcDir "LibRaw-$librawVersion"
$librawCMake = Join-Path $librawSrc "CMakeLists.txt"
Write-Host "Generating CMakeLists.txt for LibRaw..."
    @"
cmake_minimum_required(VERSION 3.10)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded`$<`$<CONFIG:Debug>:Debug>")
project(LibRaw CXX)
set(CMAKE_CXX_STANDARD 11)
file(GLOB_RECURSE LIBRAW_SOURCES "src/*.cpp")
# Exclude samples/, test files, and *_ph.cpp alternates (duplicate symbols)
list(FILTER LIBRAW_SOURCES EXCLUDE REGEX "samples/")
list(FILTER LIBRAW_SOURCES EXCLUDE REGEX "_ph\\.cpp$")
include_directories(
    `${CMAKE_CURRENT_SOURCE_DIR}
    `${CMAKE_CURRENT_SOURCE_DIR}/internal
    `${CMAKE_CURRENT_SOURCE_DIR}/src
    "$engineJpegSrc"
    "$engineZlibSrc"
    "$($prefixDir -replace '\\','/')/include"
)
# RD-A (#2.11): -DNODEPS replaced by USE_JPEG + USE_ZLIB (dropping NODEPS
# alone enables NOTHING -- defines.h:23-24 defaults NO_JPEG unless USE_JPEG
# is set). Do NOT add -DNO_LCMS: libraw_types.h:84-90 already defines it
# (empty) whenever no USE_LCMS* is set, and a command-line =1 against the
# header's empty definition is a C4005 macro-redefinition warning in every TU.
# JXL_STATIC_DEFINE: libjxl's CMake-generated jxl_export.h decorates every
# Jxl* symbol __declspec(dllimport) for consumers unless this is defined --
# harmless on Linux/macOS (generate_export_header emits visibility attributes
# there, not import/export), but on Windows it left dng.obj expecting an
# import library for JxlDecoderCreate et al. that a static .lib does not
# provide (LNK2001 unresolved __imp_Jxl* at the photo app link time).
add_definitions(-DLIBRAW_NODLL -DLIBRAW_BUILDLIB -DUSE_JPEG -DUSE_ZLIB -DUSE_JXL -DJXL_STATIC_DEFINE)
add_library(raw STATIC `${LIBRAW_SOURCES})
install(TARGETS raw ARCHIVE DESTINATION lib)
install(DIRECTORY `${CMAKE_CURRENT_SOURCE_DIR}/libraw/
    DESTINATION include/libraw
    FILES_MATCHING PATTERN "*.h")
"@ | Out-File -FilePath $librawCMake -Encoding UTF8
Build-CMake `
    $librawSrc `
    (Join-Path $buildDir "libraw") `
    $prefixDir `
    @()

# lcms2 (colour management; MIT-licensed, store-safe). LCMS2_BUILD_SHARED and
# LCMS2_BUILD_STATIC are independent CMake options, NOT tied to the standard
# BUILD_SHARED_LIBS this script's Build-CMake always passes (confirmed by
# reading cmake/Lcms2Options.cmake) -- both default ON, and on Windows,
# building both produces a static archive literally named "lcms2_static.lib"
# (to avoid colliding with the shared DLL's "lcms2.lib" import library), which
# the combine step below would miss entirely. Disabling the shared target
# collapses the static target back to the plain "lcms2" name. Also skip
# tools/tests (jpgicc/tificc/tifdiff need JPEG/TIFF as separate finds; the
# testbed needs none of this bundle) -- this project only ever calls the
# library API.
Build-CMake `
    (Join-Path $srcDir "Little-CMS-lcms$lcms2Version") `
    (Join-Path $buildDir "lcms2") `
    $prefixDir `
    @(
        "-DLCMS2_BUILD_SHARED=OFF",
        "-DLCMS2_BUILD_STATIC=ON",
        "-DLCMS2_BUILD_TOOLS=OFF",
        "-DLCMS2_BUILD_TESTS=OFF"
    )

# ---- find libraries to combine ----

$libDir = Join-Path $prefixDir "lib"

# Search recursively for static libs (CMake install may put them in config subdirs)
$allFoundLibs = Get-ChildItem -Path $libDir -Recurse -Filter "*.lib" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }

# Lib names to look for (in priority order, duplicates get one definition)
# WebP installs with "lib" prefix (libwebp.lib etc.); gav1 is embedded in avif so may be absent.
# tiff's CMake build applies MSVC's standard Debug-suffix convention (tiffd.lib in Debug,
# tiff.lib in Release) that the other libs here don't (confirmed empirically: none of the
# rest gain a "d" suffix in Debug) -- match both forms for tiff specifically so Debug
# doesn't silently drop it from the combined archive (confirmed: it always had, since this
# script's first Debug run on any machine -- LNK2019 on every TIFF symbol at the photo app
# link time, with no warning from this script because the "not found" check only fires
# for names ALREADY in $inputLibs, never for a name the regex never matched at all).
$wantedLibs = @("tiff", "libwebp", "libwebpdemux", "libsharpyuv", "avif", "raw", "gav1", "lcms2", "jpeg", "zlib",
                "jxl", "jxl_cms", "hwy", "brotlidec", "brotlicommon")
$inputLibs = @()
foreach ($name in $wantedLibs) {
    # lcms2 applies MSVC's DEBUG_POSTFIX "d" per-target (cmake/Lcms2Library.cmake),
    # the same convention tiff's own CMake build uses -- match both forms here too.
    $suffix = if ($name -eq "tiff" -or $name -eq "lcms2") { "d?" } else { "" }
    $found = $allFoundLibs | Where-Object { $_ -match "[\\/]${name}${suffix}\.lib$" } | Select-Object -First 1
    if ($found) {
        $inputLibs += $found
    }
}

foreach ($lib in $inputLibs) {
    if (-not (Test-Path $lib)) {
        Write-Warning "Library not found: $lib -- skipping"
    } else {
        Write-Host "Found: $lib"
    }
}

# Use lib.exe to merge all input libraries directly
Write-Host "Combining into $outLib..."
Remove-Item -Force $outLib -ErrorAction SilentlyContinue

if ($inputLibs.Count -eq 0) {
    Write-Error "No input libraries found to combine"
    exit 1
}

$libArgs = @("/NOLOGO", "/OUT:$outLib") + $inputLibs
& $libExe @libArgs
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed to create $outLib" }

Write-Host "Image codec bundle built: $outLib" -ForegroundColor Green
# Assert the JPEG XL decoder actually made it into the shipped archive. Both
# halves can fail silently: libjxl can be present while LibRaw was never
# patched, and LibRaw can be patched while USE_JXL never reached the compiler.
$dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
if ($dumpbin) {
    $syms = & dumpbin /SYMBOLS $outLib 2>$null | Out-String
    foreach ($want in @("JxlDecoderCreate", "jxl_dng_load_raw")) {
        if ($syms -notmatch [regex]::Escape($want)) {
            throw "expected symbol not found in ${outLib}: $want -- JPEG XL DNGs will not decode"
        }
    }
    Write-Host "Verified JPEG XL symbols in $outLib"
} else {
    Write-Warning "dumpbin not on PATH -- skipped the JPEG XL symbol check"
}

# Headers travel WITH the archive (Phase 2).
$stagedInc = Join-Path $outLibDir 'image-codecs\include'
if (Test-Path $stagedInc) { Remove-Item -Recurse -Force $stagedInc }
New-Item -ItemType Directory -Force -Path (Split-Path $stagedInc) | Out-Null
Copy-Item -Recurse -Force (Join-Path $cacheDir "$prefixDirLabel\include") $stagedInc
$stampValue | Out-File -FilePath $stampFile -Encoding ASCII -NoNewline

} finally {
    # Runs on `exit` and on a terminating error alike.
    Complete-MTStore
}
