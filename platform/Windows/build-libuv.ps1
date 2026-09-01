<#
.SYNOPSIS
    Build libuv as a static archive and stage it as libuv.lib.

.DESCRIPTION
    Replaces the TRACKED prebuilt at
    platform\Windows\libs\<Platform>\<Configuration>\libuv.lib.

    Build trees live OUTSIDE the engine checkout, alongside the archive this
    script stages (see build-sdl3.ps1 for the rationale).

.PARAMETER OutLibDir
    Where to stage libuv.lib. REQUIRED.
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

$srcDir = Join-Path $repoRoot 'other\lib\libuv'
if (-not (Test-Path (Join-Path $srcDir 'CMakeLists.txt'))) {
    throw "Missing libuv source tree: $srcDir (run: git submodule update --init --recursive)"
}

$buildDir = Join-Path (Get-MTCapsWorkDir 'libuv') "build-windows-$Platform-$Configuration"
$outLib   = Join-Path $outDir 'libuv.lib'
$stamp    = Join-Path $outDir 'libuv.stamp'

$scriptSha = 'unknown'
try { $scriptSha = (Get-FileHash -Algorithm SHA256 -Path $PSCommandPath).Hash.ToLowerInvariant() } catch {}

$srcSha = 'unknown'
# libuv is VENDORED (no .git of its own): `git -C $srcDir rev-parse HEAD`
# walks up to the engine repo and returns the ENGINE head, so the stamp
# changed on every engine commit and rebuilt libuv each time (Windows
# verification, HANDOVER item 12). The tree hash of the vendored directory
# changes only when the libuv sources themselves change.
try { $srcSha = (git -C $repoRoot rev-parse --short "HEAD:other/lib/libuv").Trim() } catch {}
if (-not $srcSha) {
    try { $srcSha = (Get-FileHash -Algorithm SHA256 -Path (Join-Path $srcDir 'CMakeLists.txt')).Hash.ToLowerInvariant() } catch {}
}

$stampValue = "$srcSha`:$scriptSha`:$Configuration`:$Platform`:$Compiler"
if ((Test-Path $outLib) -and (Test-Path $stamp)) {
    if ((Get-Content $stamp -Raw).Trim() -eq $stampValue) {
        Write-Host "libuv is up to date: $outLib"
        exit 0
    }
}

$cmakeToolsetName = if ($Compiler -eq 'Clang') { 'ClangCL' } else { '' }
$cmakeToolset = if ($cmakeToolsetName) { @('-T', $cmakeToolsetName) } else { @() }

$generator = Get-MTVSGenerator
Write-Host "Configuring libuv in $buildDir"
$null = Reset-MTStaleCMakeCache -BuildDir $buildDir -Platform $Platform -Generator $generator `
    -Toolset $cmakeToolsetName -SourceDir $srcDir -Label 'libuv'

$cmakeArgs = @(
    "-S", $srcDir,
    "-B", $buildDir,
    "-G", $generator,
    "-A", $Platform
) + $cmakeToolset + (Get-MTCrtCMakeArgs -Configuration $Configuration) + @(
    "-DBUILD_SHARED_LIBS=OFF",
    "-DLIBUV_BUILD_TESTS=OFF",
    "-DLIBUV_BUILD_BENCH=OFF",
    "-DBUILD_TESTING=OFF"
)

Invoke-MTNative -What "CMake configure for libuv" -Action { & cmake @cmakeArgs }

Write-Host "Building libuv ($Configuration)"
# uv_a is libuv's STATIC target; uv is the shared one. Naming the target
# explicitly also skips the test and benchmark executables.
Invoke-MTNative -What "CMake build for libuv" -Action {
    & cmake --build $buildDir --config $Configuration --target uv_a
}

# The static target is uv_a, but its OUTPUT_NAME is 'libuv', so the file on
# disk is libuv.lib -- already the name the engine and app link.
$built = Find-MTConfigLib -BuildDir $buildDir -Name 'libuv.lib' -Configuration $Configuration

if (Test-Path $outLib) { Remove-Item -Force $outLib }
Copy-Item -Force $built $outLib
Set-Content -NoNewline -Path $stamp -Value $stampValue
Write-Host "Done. Output: $outLib"
