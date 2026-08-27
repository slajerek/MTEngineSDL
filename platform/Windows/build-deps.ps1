<#
.SYNOPSIS
    Build all MTEngineSDL dependencies for Windows.
.DESCRIPTION
    Orchestrates building llama.cpp (CPU), FTXUI, mbedTLS, and image codec (TIFF, WebP, AVIF, LibRaw) static libraries.
    Output: platform/Windows/libs/<Platform>/<Configuration>/
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
    # resolve that produced -OutLibDir. Without it every dependency below builds,
    # which is the correct standalone default and was previously the only
    # behaviour on Windows -- see Import-MTCapsEnvironment.
    [string]$CapsFile,

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
# six places for the old platform\Windows\libs path to creep back in, and that
# directory holds 25 TRACKED prebuilts -- writing outputs into it dirties the
# working tree on every build, which is half of what moving the archives out was
# for.
#
# `clone the engine and build it` still has to work, so the fallback lives here,
# and it is OUTSIDE the checkout: a `standalone` prefix that cannot be mistaken
# for a keyed bucket, matching what mt_caps_lib_dir falls back to on macOS and
# Linux.
if (-not $OutLibDir) {
    $mtRoot = if ($env:MTENGINE_BUILD_ROOT) { $env:MTENGINE_BUILD_ROOT }
              else { Join-Path $env:LOCALAPPDATA 'mtengine' }
    $OutLibDir = Join-Path $mtRoot "_deps\standalone\windows\$Platform\$Configuration\libs"
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
} else {
    Write-Host "No -CapsFile given; building every dependency (standalone default)" -ForegroundColor Yellow
}

Write-Host "`n=== Building llama.cpp ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-llama-cpp.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration -Compiler $Compiler -SkipCuda:($SkipCuda -or ($Platform -eq 'ARM64'))

Write-Host "`n=== Building FTXUI ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-ftxui.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration -Compiler $Compiler

Write-Host "`n=== Building mbedTLS ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-mbedtls.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration -Compiler $Compiler

Write-Host "`n=== Building Image Codecs ($Platform $Configuration) ===" -ForegroundColor Cyan
& "$scriptDir\build-image_codecs.ps1" -OutLibDir $OutLibDir -Platform $Platform -Configuration $Configuration

Write-Host "`n=== All dependencies built successfully ===" -ForegroundColor Green
