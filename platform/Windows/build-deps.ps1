<#
.SYNOPSIS
    Build all MTEngineSDL dependencies for Windows.
.DESCRIPTION
    Orchestrates building llama.cpp (CPU), FTXUI, mbedTLS, and image codec (TIFF, WebP, AVIF, LibRaw) static libraries.
    Output: the caps-keyed bucket given by -OutLibDir (see -OutLibDir below).
.PARAMETER Platform
    Target architecture: x64 or ARM64. Default: auto-detect.
.PARAMETER Configuration
    Build configuration: Debug or Release. Default: Release.
.PARAMETER Compiler
    Compiler toolchain: Clang (ClangCL) or MSVC (v143). Default: Clang.
    CUDA builds always use MSVC regardless of this setting.
.PARAMETER SkipCuda
    Skip CUDA build for llama.cpp (always skipped on ARM64).
.EXAMPLE
    .\build-deps.ps1
    .\build-deps.ps1 -Platform ARM64 -Configuration Debug -Compiler MSVC
#>
param(
    [ValidateSet('x64','ARM64')]
    [string]$Platform,

    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release',

    [ValidateSet('Clang','MSVC')]
    [string]$Compiler = 'Clang',

    [switch]$SkipCuda,

    # Where the archives go. Supplied by an app's build-windows.ps1 from the
    # single mtcaps resolve it already does; see the fallback below for the
    # standalone case.
    [string]$OutLibDir,

    # The resolved capability fragment (MTEngineCaps.xcconfig), from the SAME
    # resolve that produced -OutLibDir. REQUIRED unless -Standalone: omitting it
    # used to warn-and-build-everything, and that fail-open default is how
    # the game app built a 541 MB codec bundle its manifest had switched off --
    # INTO the engine checkout (unification plan, E3/L3).
    [string]$CapsFile,

    # The explicit opt-in for a caps-less build: a bare engine clone with no
    # manifest. Absent-means-ON then applies and everything builds.
    [switch]$Standalone,

    [switch]$Help
)

if ($Help) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\mt-build-common.ps1"

# NOT $env:PROCESSOR_ARCHITECTURE, which describes the PROCESS and is inherited:
# under Git Bash (an emulated x64 build) it says AMD64 on an ARM64 machine.
$Platform = Resolve-MTPlatform $Platform

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# THE ONE FALLBACK IN THE SYSTEM. The six library scripts REQUIRE -OutLibDir and
# throw without it, deliberately: six scripts each carrying their own default is
# six places for the old platform\Windows\libs path to creep back in. That
# directory held 25 TRACKED prebuilts and is now deleted (item 9); writing
# outputs into a checkout dirties the working tree on every build, which is
# half of what moving the archives out was for.
#
# `clone the engine and build it` still has to work, so the fallback lives here,
# and it is OUTSIDE the checkout: a `standalone` prefix that cannot be mistaken
# for a keyed bucket, matching what mt_caps_lib_dir falls back to on macOS and
# Linux.
if (-not $OutLibDir) {
    $OutLibDir = Join-Path (Get-MTBuildRoot) "_deps\standalone\windows\$Platform\$Configuration\libs"
    Write-Host "No -OutLibDir given; standalone: $OutLibDir" -ForegroundColor Yellow
}
New-Item -ItemType Directory -Force -Path $OutLibDir | Out-Null

# WHY THIS IS HERE AND NOT IN EACH SCRIPT.
#
# The four library scripts gate themselves on $env:MT_ENABLE_<DEP>, matching
# their macOS counterparts. On macOS those variables arrive for free, because the
# dependency builds run as Xcode script phases and Xcode exports the resolved
# xcconfig into them. On Windows nothing did that, so the gates were dead: this
# script built llama.cpp, FTXUI, mbedTLS and the image codecs (TIFF, WebP, AVIF,
# LibRaw) on EVERY build regardless of the app's manifest.
#
# Publishing once here rather than resolving in each script keeps the guarantee
# that every dependency in one build sees one capability set.
if ($CapsFile) {
    Import-MTCapsEnvironment -CapsFile $CapsFile
} elseif ($Standalone) {
    Write-Host "-Standalone: building every dependency (absent means ON)" -ForegroundColor Yellow
} else {
    # FAIL CLOSED (Phase 2, decision E3). Building everything on a silent
    # default is a 10x cost regression and a licence hazard, not a convenience.
    throw ("build-deps.ps1 requires -CapsFile (the MTEngineCaps.xcconfig from " +
           "the same resolve that produced -OutLibDir). For a deliberate " +
           "caps-less engine build, pass -Standalone.")
}

# The four core deps that HAVE from-source scripts now (authored on the
# Windows box, integrated 2026-08-31 -- the Phase 5 follow-up): each stamps
# its output and self-skips, exactly like the capability-gated four above.
# These four are now the ONLY source of SDL3, FreeType, libuv and uSockets on
# Windows -- the prebuilt staging they used to race is gone (see below).
Write-Host "`n=== Building SDL3 ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-sdl3.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration -Compiler $Compiler
if ($LASTEXITCODE -ne 0) { Write-Error "SDL3 build failed"; exit 1 }

Write-Host "`n=== Building FreeType ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-freetype.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration -Compiler $Compiler
if ($LASTEXITCODE -ne 0) { Write-Error "FreeType build failed"; exit 1 }

Write-Host "`n=== Building libuv ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-libuv.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration -Compiler $Compiler
if ($LASTEXITCODE -ne 0) { Write-Error "libuv build failed"; exit 1 }

Write-Host "`n=== Building uSockets ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-usockets.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration -Compiler $Compiler
if ($LASTEXITCODE -ne 0) { Write-Error "uSockets build failed"; exit 1 }

# The prebuilt staging that used to sit here is GONE (HANDOVER item 9,
# 2026-09-02), together with the 13 tracked .lib files under
# platform\Windows\libs that it copied from. It was already a pure fallback;
# what it was waiting on was evidence that the four from-source scripts cover
# every arch/config this repo ships. That evidence now exists -- all four
# combinations built green from source on the Windows ARM64 box, each archive
# differing from the prebuilt it replaced in both size and hash:
#   ARM64 Release, ARM64 Debug, x64 Release, x64 Debug
# Nothing points a link at that directory any more either: every
# AdditionalLibraryDirectories in the engine and in all four apps resolves
# through $(MTCapsLibsDir), the caps-keyed bucket.

Write-Host "`n=== Building llama.cpp ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-llama-cpp.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration -Compiler $Compiler -SkipCuda:($SkipCuda -or ($Platform -eq 'ARM64'))

Write-Host "`n=== Building FTXUI ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-ftxui.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration -Compiler $Compiler

Write-Host "`n=== Building mbedTLS ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-mbedtls.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration -Compiler $Compiler

Write-Host "`n=== Building Image Codecs ($Platform $Configuration) ===" -ForegroundColor Cyan
& "$scriptDir\build-image_codecs.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration

# The FIFTH family, folded in 2026-08-31 (Phase 2): before this, every app
# wrapper called build-video_codecs.ps1 itself, each with its own gating --
# one orchestrator, one gate, like the four above. The script stubs itself
# at MT_ENABLE_WEBM_VPX=0 and reads MT_FFMPEG_BUILD_MODE for the decoder set.
Write-Host "`n=== Building Video Codecs ($Platform $Configuration) ===" -ForegroundColor Cyan
& "$scriptDir\build-video_codecs.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration

Write-Host "`n=== All dependencies built successfully ===" -ForegroundColor Green
