param(
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
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

Require-Command cmake
Require-Command lib

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$llamaSrc = Join-Path $repoRoot 'other\lib\llama.cpp'

# KEYED BY ARCHITECTURE. It used to be a single 'build-windows-cpu' shared by
# x64 and ARM64, so the second architecture to build walked into the first
# one's CMakeCache.txt and CMake refused outright:
#     "generator platform: x64 Does not match the platform used previously: ARM64"
# A build directory is per-configuration; the name now says so.
$buildDir = Join-Path $llamaSrc "build-windows-cpu-$Platform"

# The unkeyed directory this replaced holds whichever architecture happened to
# configure it last. Nothing reads it any more, and it is not small.
$legacyBuildDir = Join-Path $llamaSrc 'build-windows-cpu'
if (Test-Path -LiteralPath $legacyBuildDir) {
    Remove-MTBuildDir -Dir $legacyBuildDir -Reason 'llama.cpp (CPU) build directories are now keyed by architecture'
}

# REQUIRED, not defaulted. The caller decides where archives go; the only
# fallback in the system lives in build-deps.ps1, so the old
# platform\Windows\libs path cannot creep back in here -- that directory holds
# 25 TRACKED prebuilts and a build must not write into it.
if (-not $OutLibDir) { throw "-OutLibDir is required. Run this through build-deps.ps1, which resolves it." }
$outDir = $OutLibDir
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$cmakeToolsetName = if ($Compiler -eq 'Clang') { 'ClangCL' } else { '' }
$cmakeToolset = if ($cmakeToolsetName) { @('-T', $cmakeToolsetName) } else { @() }

Write-Host "Configuring llama.cpp (CPU) in $buildDir"
$generator = Get-MTVSGenerator
Write-Host "Using CMake generator: $generator"

# A cache left by a different architecture, a different Visual Studio or a
# switch between -Compiler Clang and MSVC makes CMake abort rather than
# reconfigure. This build directory is ours, so throw it away and start over.
$null = Reset-MTStaleCMakeCache -BuildDir $buildDir -Platform $Platform -Generator $generator `
    -Toolset $cmakeToolsetName -SourceDir $llamaSrc -Label 'llama.cpp (CPU)'

# RELAX $ErrorActionPreference AROUND THESE TWO NATIVE CALLS, same issue and
# same fix as build-image_codecs.ps1's Build-CMake / build-video_codecs.ps1:
# PowerShell 5.1 wraps ANY stderr line from a native command into a
# terminating NativeCommandError under 'Stop', regardless of exit code -- so a
# benign "CMake Warning: Manually-specified variables were not used by the
# project" on an otherwise-successful configure aborted this script outright
# (confirmed on this VM: x64 configure with a pre-existing ARM64 CMakeCache
# failed loudly with the real error, but a fresh reconfigure then failed just
# as hard on nothing but a warning). $LASTEXITCODE below is the real,
# sufficient success/failure signal.
$savedEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    cmake -S $llamaSrc -B $buildDir -G $generator -A $Platform @cmakeToolset `
        -DBUILD_SHARED_LIBS=OFF `
        -DLLAMA_BUILD_COMMON=OFF `
        -DLLAMA_BUILD_TESTS=OFF `
        -DLLAMA_BUILD_TOOLS=OFF `
        -DLLAMA_BUILD_EXAMPLES=OFF `
        -DLLAMA_BUILD_SERVER=OFF `
        -DLLAMA_TOOLS_INSTALL=OFF `
        -DLLAMA_TESTS_INSTALL=OFF `
        -DGGML_CUDA=OFF `
        -DGGML_VULKAN=OFF `
        -DGGML_OPENMP=OFF `
        -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for llama.cpp" }

    Write-Host "Building llama.cpp libs ($Configuration)"
    cmake --build $buildDir --config $Configuration --target llama ggml ggml-base ggml-cpu
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for llama.cpp" }
} finally {
    $ErrorActionPreference = $savedEap
}

# VS generator places libs under buildDir\src and buildDir\ggml\src
$libs = @(
    (Join-Path $buildDir "src\$Configuration\llama.lib"),
    (Join-Path $buildDir "ggml\src\$Configuration\ggml.lib"),
    (Join-Path $buildDir "ggml\src\$Configuration\ggml-base.lib"),
    (Join-Path $buildDir "ggml\src\$Configuration\ggml-cpu.lib")
)

foreach ($p in $libs) {
    if (-not (Test-Path $p)) {
        throw "Expected library not found: $p"
    }
}

$outLib = Join-Path $outDir 'llama_cpp.lib'
Write-Host "Packing into $outLib"
if (Test-Path $outLib) { Remove-Item -Force $outLib }
lib /nologo /OUT:$outLib $libs

# Retire any stub stamp left by a capability-off build. build-llama-cpp.ps1
# writes llama_cpp.stamp when MT_CAP_LLM=0; if that stamp survived beside this
# real archive, the next off build would match it, skip, and keep the real
# llama_cpp.lib while believing it had written a stub.
$stamp = Join-Path $outDir 'llama_cpp.stamp'
if (Test-Path $stamp) { Remove-Item -Force $stamp }

Write-Host "Done. Output: $outLib"

# Generate version header from git tag
$versionTag = "unknown"
try {
    $tag = git -C $llamaSrc describe --tags --abbrev=0 2>$null
    if ($tag) { $versionTag = $tag.Trim() }
} catch {}

# NOT src\Engine\Sci\Llama\llama_cpp_version.h, which is TRACKED. An app build
# writing a tracked file inside the engine checkout is the strongest form of the
# invariant violation, and it also fed back into the output-root key: engine_rev()
# appends -dirty-<hash>, so a changed version header sent every subsequent resolve
# to a different $MT_OUT and threw the keyed cache away.
#
# It goes under the generated include dir, already on every build system's include
# path. mtcaps writes a placeholder there first, so the header exists even when
# this script never runs.
$mtGenInclude = if ($env:MTCapsOut) { $env:MTCapsOut } elseif ($env:MTOutRoot) { $env:MTOutRoot } else {
    Join-Path $env:LOCALAPPDATA 'mtengine\_standalone'
}
$versionHeader = Join-Path $mtGenInclude 'include\Sci\Llama\llama_cpp_version.h'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $versionHeader) | Out-Null
@"
// Auto-generated by build-llama_cpp_cpu.ps1 — do not edit
#pragma once
#define MT_LLAMA_CPP_VERSION "$versionTag"
"@ | Set-Content -Path $versionHeader -Encoding UTF8
