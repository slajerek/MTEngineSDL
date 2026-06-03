<#
.SYNOPSIS
    Build all MTEngineSDL dependencies for Windows.
.DESCRIPTION
    Orchestrates building llama.cpp (CPU), FTXUI, and mbedTLS static libraries.
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
    [switch]$Help
)

if ($Help) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

$ErrorActionPreference = 'Stop'

if (-not $Platform) {
    $arch = $env:PROCESSOR_ARCHITECTURE
    $Platform = if ($arch -eq 'ARM64') { 'ARM64' } else { 'x64' }
    Write-Host "Auto-detected platform: $Platform"
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "`n=== Building llama.cpp ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-llama-cpp.ps1" -Platform $Platform -Configuration $Configuration -Compiler $Compiler -SkipCuda:($SkipCuda -or ($Platform -eq 'ARM64'))

Write-Host "`n=== Building FTXUI ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-ftxui.ps1" -Platform $Platform -Configuration $Configuration -Compiler $Compiler

Write-Host "`n=== Building mbedTLS ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$scriptDir\build-mbedtls.ps1" -Platform $Platform -Configuration $Configuration -Compiler $Compiler

Write-Host "`n=== All dependencies built successfully ===" -ForegroundColor Green
