<#
.SYNOPSIS
    Build SDL3 as a static archive and stage it as SDL3.lib.

.DESCRIPTION
    The Windows counterpart to platform/MacOS/build-sdl3.sh and
    platform/Linux/build-sdl3.sh. Until this existed, SDL3.lib was a TRACKED
    prebuilt under platform\Windows\libs\<Platform>\<Configuration> -- roughly
    50 MB of binary in git that nothing could regenerate on Windows.

    Build trees live OUTSIDE the engine checkout, alongside the archive this
    script stages. The older dependency scripts still use other\lib\<dep>\build*
    inside the checkout (modularity item 0.11, still open); new scripts do not
    add to that debt.

.PARAMETER OutLibDir
    Where to stage SDL3.lib. REQUIRED -- the caller decides, and
    platform\Windows\libs must never be written to.
#>
param(
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64','ARM64')]
    [string]$Platform = 'x64',
    [ValidateSet('Clang','MSVC')]
    [string]$Compiler = 'Clang',
    [string]$OutLibDir
)

$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\mt-build-common.ps1"

if (-not $OutLibDir) { throw "-OutLibDir is required. Run this through build-deps.ps1, which resolves it." }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "Missing required tool 'cmake'. Run from 'Developer PowerShell for VS 2022' and ensure CMake is installed."
}

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
# Per-unit store (L16). The unit builds in a directory keyed only by the
# capabilities IT reads, then its outputs are copied into the shared view. The
# body is wrapped in try/finally because a stamp hit and a capability-off stub
# both leave early and both still owe the view a copy.
$outDir = Use-MTStore -Unit 'sdl3' -View $OutLibDir
try {
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Newest vendored SDL3 static source tree, so a version bump needs no edit here.
$srcDir = (Get-ChildItem -Path (Join-Path $repoRoot 'other\lib') -Directory -Filter 'SDL-release-3.*-static' -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1)
if (-not $srcDir) { throw "No other\lib\SDL-release-3.*-static source tree found" }
$srcDir = $srcDir.FullName

# Phase 5 convention: ONE work root serves every caps-hash bucket -- building
# under the bucket's parent would rebuild an identical archive per hash.
$buildDir = Join-Path (Get-MTCapsWorkDir 'sdl3') "build-windows-$Platform-$Configuration"
$outLib   = Join-Path $outDir 'SDL3.lib'
$stamp    = Join-Path $outDir 'SDL3.stamp'

$scriptSha = 'unknown'
try { $scriptSha = (Get-FileHash -Algorithm SHA256 -Path $PSCommandPath).Hash.ToLowerInvariant() } catch {}

$srcSha = 'unknown'
try { $srcSha = (Get-FileHash -Algorithm SHA256 -Path (Join-Path $srcDir 'include\SDL3\SDL_version.h')).Hash.ToLowerInvariant() } catch {}

$stampValue = "$srcSha`:$scriptSha`:$Configuration`:$Platform`:$Compiler"
if ((Test-Path $outLib) -and (Test-Path $stamp)) {
    if ((Get-Content $stamp -Raw).Trim() -eq $stampValue) {
        Write-Host "SDL3 is up to date: $outLib"
        exit 0
    }
}

$cmakeToolsetName = if ($Compiler -eq 'Clang') { 'ClangCL' } else { '' }
$cmakeToolset = if ($cmakeToolsetName) { @('-T', $cmakeToolsetName) } else { @() }

$generator = Get-MTVSGenerator
Write-Host "Configuring SDL3 in $buildDir"
$null = Reset-MTStaleCMakeCache -BuildDir $buildDir -Platform $Platform -Generator $generator `
    -Toolset $cmakeToolsetName -SourceDir $srcDir -Label 'SDL3'

# SDL_STATIC=ON / SDL_SHARED=OFF: when both defaults are left alone SDL's
# CMakeLists sets SDL_STATIC_DEFAULT OFF ("Default to just building the shared
# library"), so a plain configure produces no static archive at all -- only a
# DLL that would then have to be shipped. See build-sdl3.sh:32-37.
#
# SDL_CAMERA=OFF is deliberate and is a PRODUCT decision, not a build tidy-up
# (build-sdl3.sh:17). Keep the three platforms in agreement.
$cmakeArgs = @(
    "-S", $srcDir,
    "-B", $buildDir,
    "-G", $generator,
    "-A", $Platform
) + $cmakeToolset + (Get-MTCrtCMakeArgs -Configuration $Configuration) + @(
    "-DSDL_STATIC=ON",
    "-DSDL_SHARED=OFF",
    "-DSDL_TEST_LIBRARY=OFF",
    "-DBUILD_SHARED_LIBS=OFF",
    "-DSDL_CAMERA=OFF"
)

Invoke-MTNative -What "CMake configure for SDL3" -Action { & cmake @cmakeArgs }

Write-Host "Building SDL3 ($Configuration)"
Invoke-MTNative -What "CMake build for SDL3" -Action {
    & cmake --build $buildDir --config $Configuration --target SDL3-static
}

# CMake names the static target's output SDL3-static.lib; the engine and app
# link plain SDL3.lib, which is what the tracked prebuilt was called.
$built = Find-MTConfigLib -BuildDir $buildDir -Name 'SDL3-static.lib' -Configuration $Configuration

if (Test-Path $outLib) { Remove-Item -Force $outLib }
Copy-Item -Force $built $outLib
Set-Content -NoNewline -Path $stamp -Value $stampValue
Write-Host "Done. Output: $outLib"

} finally {
    # Runs on `exit` and on a terminating error alike.
    Complete-MTStore
}
