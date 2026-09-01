param(
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64','ARM64')]
    [string]$Platform = 'x64',
    [ValidateSet('Clang','MSVC')]
    [string]$Compiler = 'Clang',

    # Where this script stages its archive. Supplied by build-deps.ps1 or by an
    # app wrapper, both from the single mtcaps resolve they already do.
    [string]$OutLibDir
)

$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\mt-build-common.ps1"

function Require-Command([string]$name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        throw "Missing required tool '$name'. Run from 'Developer PowerShell for VS 2022' and ensure CMake is installed."
    }
}

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$ftxuiSrc = Join-Path $repoRoot 'other\lib\ftxui'
$buildDir = Join-Path (Get-MTCapsWorkDir 'ftxui') "build-windows-$Platform"

# REQUIRED, not defaulted. The caller decides where archives go; the only
# fallback in the system lives in build-deps.ps1, so the old
# platform\Windows\libs path cannot creep back in here -- that directory holds
# 25 TRACKED prebuilts and a build must not write into it.
if (-not $OutLibDir) { throw "-OutLibDir is required. Run this through build-deps.ps1, which resolves it." }
$outDir = $OutLibDir
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$outLib = Join-Path $outDir 'ftxui.lib'
$stamp  = Join-Path $outDir 'ftxui.stamp'

$scriptSha = 'unknown'
try {
    $scriptSha = (Get-FileHash -Algorithm SHA256 -Path $PSCommandPath).Hash.ToLowerInvariant()
} catch {}

# THE CAPABILITY GATE -- the macOS counterpart of this script has had one since
# the capability programme started (build-ftxui.sh), and this one never did.
#
# It comes BEFORE the submodule check and before Require-Command cmake, both on
# purpose. With MT_CAP_FTXUI=0 an app's build-windows.ps1 does not fetch the
# ftxui submodule at all -- that is the point of selective acquisition -- so
# checking for it first would turn the saving into a build failure on any clean
# clone. Nothing below is needed to write a stub: only cl and lib.
if ($env:MT_ENABLE_FTXUI -eq '0') {
    Require-Command cl
    Require-Command lib
    New-MTCapsStubArchive -OutLib $outLib -Stamp $stamp -Symbol 'ftxui' `
                          -StampValue "disabled`:$scriptSha`:$Configuration`:$Platform"
    exit 0
}

# Self-skip (L11): same contract as build-libuv.ps1 -- source rev + script
# hash + configuration; a real git checkout here (submodule), so rev-parse
# inside it is correct, unlike vendored libuv.
$srcSha = 'unknown'
try { $srcSha = (git -C $ftxuiSrc rev-parse --short HEAD).Trim() } catch {}
$stampValue = "enabled`:$srcSha`:$scriptSha`:$Configuration`:$Platform`:$Compiler"
if ((Test-Path $outLib) -and (Test-Path $stamp)) {
    if ((Get-Content $stamp -Raw).Trim() -eq $stampValue) {
        Write-Host "ftxui is up to date: $outLib"
        exit 0
    }
}

Require-Command cmake
Require-Command lib

if (-not (Test-Path $ftxuiSrc)) {
    throw "FTXUI submodule not found at $ftxuiSrc. Run: git submodule update --init --recursive"
}

$cmakeToolsetName = if ($Compiler -eq 'Clang') { 'ClangCL' } else { '' }
$cmakeToolset = if ($cmakeToolsetName) { @('-T', $cmakeToolsetName) } else { @() }

Write-Host "Configuring FTXUI ($Platform, $Configuration) in $buildDir"
$generator = Get-MTVSGenerator
Write-Host "Using CMake generator: $generator"

# The directory is already keyed by architecture, but a Visual Studio upgrade
# or a -Compiler switch leaves a cache CMake will refuse to reconfigure.
$null = Reset-MTStaleCMakeCache -BuildDir $buildDir -Platform $Platform -Generator $generator `
    -Toolset $cmakeToolsetName -SourceDir $ftxuiSrc -Label 'FTXUI'

# RELAX $ErrorActionPreference AROUND THESE TWO NATIVE CALLS, same issue and
# same fix as build-llama_cpp_cpu.ps1's Build-CMake / build-image_codecs.ps1:
# PowerShell 5.1 wraps ANY stderr line from a native command into a
# terminating NativeCommandError under 'Stop', regardless of exit code -- so a
# benign CMake deprecation warning on an otherwise-successful configure would
# abort this script outright. $LASTEXITCODE below is the real, sufficient
# success/failure signal.
$savedEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    # THE STATIC CRT, per configuration -- the same line build-llama_cpp_cpu.ps1,
    # build-mbedtls.ps1, build-image_codecs.ps1 and build-video_codecs.ps1 all
    # carry. This script was the only one that did not, so CMake used its own
    # default (MultiThreadedDLL) and ftxui.lib came out /MDd while the engine and
    # every app compile /MTd. Nothing linked ftxui.lib on Windows until
    # the game app did, and then the link failed with LNK2038 mismatch detected
    # for 'RuntimeLibrary'.
    #
    # CMP0091 HAS TO COME WITH IT. FTXUI declares cmake_minimum_required(VERSION
    # 3.12) and that policy arrived in 3.15, so it defaults to OLD and
    # CMAKE_MSVC_RUNTIME_LIBRARY is ignored -- SILENTLY. The variable lands in
    # CMakeCache.txt either way, so the cache is not evidence that it took
    # effect; the link error is. The other four scripts build projects that
    # already require >= 3.15, which is why none of them needs this line.
    cmake -S $ftxuiSrc -B $buildDir -G $generator -A $Platform @cmakeToolset `
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW `
        -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>" `
        -DBUILD_SHARED_LIBS=OFF `
        -DFTXUI_BUILD_DOCS=OFF `
        -DFTXUI_BUILD_EXAMPLES=OFF `
        -DFTXUI_BUILD_MODULES=OFF `
        -DFTXUI_BUILD_TESTS=OFF `
        -DFTXUI_BUILD_TESTS_FUZZER=OFF `
        -DFTXUI_ENABLE_INSTALL=OFF `
        -DFTXUI_QUIET=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for FTXUI" }

    Write-Host "Building FTXUI libs ($Configuration)"
    cmake --build $buildDir --config $Configuration --target screen dom component
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for FTXUI" }
} finally {
    $ErrorActionPreference = $savedEap
}

# VS generator places libs under buildDir\{Configuration}\
$libs = @(
    (Join-Path $buildDir "$Configuration\ftxui-screen.lib"),
    (Join-Path $buildDir "$Configuration\ftxui-dom.lib"),
    (Join-Path $buildDir "$Configuration\ftxui-component.lib")
)

foreach ($p in $libs) {
    if (-not (Test-Path $p)) {
        throw "Expected library not found: $p"
    }
}

Write-Host "Packing into $outLib"
if (Test-Path $outLib) { Remove-Item -Force $outLib }
lib /nologo /OUT:$outLib $libs

# Stamp the ENABLED archive too, not just the stub. Without this the stub stamp
# from a previous capability-off build survives beside the real ftxui.lib, and
# the next off build finds stamp and archive both present, matches, and keeps the
# REAL archive while believing it wrote a stub.
Set-Content -NoNewline -Path $stamp -Value $stampValue

Write-Host "Done. Output: $outLib"
