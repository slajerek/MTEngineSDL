<#
.SYNOPSIS
    Build FreeType as a static archive and stage it as freetype.lib.

.DESCRIPTION
    Replaces the TRACKED prebuilt at
    platform\Windows\libs\<Platform>\<Configuration>\freetype.lib.

    Build trees live OUTSIDE the engine checkout, alongside the archive this
    script stages (see build-sdl3.ps1 for the rationale).

.PARAMETER OutLibDir
    Where to stage freetype.lib. REQUIRED.
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
$outDir = $OutLibDir
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Newest vendored FreeType tree, so a version bump needs no edit here.
$srcDir = (Get-ChildItem -Path (Join-Path $repoRoot 'other\lib') -Directory -Filter 'freetype-*' -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1)
if (-not $srcDir) { throw "No other\lib\freetype-* source tree found" }
$srcDir = $srcDir.FullName

$buildDir = Join-Path (Get-MTCapsWorkDir 'freetype') "build-windows-$Platform-$Configuration"
$outLib   = Join-Path $outDir 'freetype.lib'
$stamp    = Join-Path $outDir 'freetype.stamp'

$scriptSha = 'unknown'
try { $scriptSha = (Get-FileHash -Algorithm SHA256 -Path $PSCommandPath).Hash.ToLowerInvariant() } catch {}

$srcSha = 'unknown'
try { $srcSha = (Get-FileHash -Algorithm SHA256 -Path (Join-Path $srcDir 'CMakeLists.txt')).Hash.ToLowerInvariant() } catch {}

$stampValue = "$(Split-Path $srcDir -Leaf)`:$srcSha`:$scriptSha`:$Configuration`:$Platform`:$Compiler"
if ((Test-Path $outLib) -and (Test-Path $stamp)) {
    if ((Get-Content $stamp -Raw).Trim() -eq $stampValue) {
        Write-Host "FreeType is up to date: $outLib"
        exit 0
    }
}

$cmakeToolsetName = if ($Compiler -eq 'Clang') { 'ClangCL' } else { '' }
$cmakeToolset = if ($cmakeToolsetName) { @('-T', $cmakeToolsetName) } else { @() }

$generator = Get-MTVSGenerator
Write-Host "Configuring FreeType in $buildDir"
$null = Reset-MTStaleCMakeCache -BuildDir $buildDir -Platform $Platform -Generator $generator `
    -Toolset $cmakeToolsetName -SourceDir $srcDir -Label 'FreeType'

# The engine uses FreeType only for glyph rasterisation, and none of the
# optional dependencies are vendored for Windows. FreeType 2.10 gates these
# through CMAKE_DISABLE_FIND_PACKAGE_* -- the FT_DISABLE_* spellings only
# arrived in 2.11, so pin the ones this tree actually reads. An unused -D is a
# CMake warning, not an error, so a later version bump still configures.
$cmakeArgs = @(
    "-S", $srcDir,
    "-B", $buildDir,
    "-G", $generator,
    "-A", $Platform
) + $cmakeToolset + (Get-MTCrtCMakeArgs -Configuration $Configuration) + @(
    "-DBUILD_SHARED_LIBS=OFF",
    "-DCMAKE_DISABLE_FIND_PACKAGE_HarfBuzz=TRUE",
    "-DCMAKE_DISABLE_FIND_PACKAGE_PNG=TRUE",
    "-DCMAKE_DISABLE_FIND_PACKAGE_BZip2=TRUE",
    "-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE",
    "-DCMAKE_DISABLE_FIND_PACKAGE_BrotliDec=TRUE"
)

Invoke-MTNative -What "CMake configure for FreeType" -Action { & cmake @cmakeArgs }

Write-Host "Building FreeType ($Configuration)"
Invoke-MTNative -What "CMake build for FreeType" -Action {
    & cmake --build $buildDir --config $Configuration --target freetype
}

# FreeType sets CMAKE_DEBUG_POSTFIX to "d", so a Debug build emits
# freetyped.lib. The engine links plain freetype.lib in every configuration.
$built = $null
foreach ($candidate in @('freetype.lib', 'freetyped.lib')) {
    try {
        $built = Find-MTConfigLib -BuildDir $buildDir -Name $candidate -Configuration $Configuration
        break
    } catch {
        $built = $null
    }
}
if (-not $built) { throw "No freetype.lib or freetyped.lib built for '$Configuration' under $buildDir" }

if (Test-Path $outLib) { Remove-Item -Force $outLib }
Copy-Item -Force $built $outLib
Set-Content -NoNewline -Path $stamp -Value $stampValue
Write-Host "Done. Output: $outLib"
