param(
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64')]
    [string]$Platform = 'x64',

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

if (-not $env:CUDA_PATH) {
    throw "CUDA_PATH is not set. Install NVIDIA CUDA Toolkit and reopen your shell." 
}

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$llamaSrc = Join-Path $repoRoot 'other\lib\llama.cpp'

# Keyed by architecture for the same reason as the CPU bundle: a build
# directory belongs to one configuration, and CMake aborts rather than
# reconfigure when a cache from another one is sitting in it.
$buildDir = Join-Path (Get-MTCapsWorkDir 'llama.cpp') "build-windows-cuda-$Platform"

$legacyBuildDir = Join-Path $llamaSrc 'build-windows-cuda'
if (Test-Path -LiteralPath $legacyBuildDir) {
    Remove-MTBuildDir -Dir $legacyBuildDir -Reason 'llama.cpp (CUDA) build directories are now keyed by architecture'
}

# REQUIRED, not defaulted. The caller decides where archives go; the only
# fallback in the system lives in build-deps.ps1, so the old
# platform\Windows\libs path cannot creep back in here -- that directory holds
# 25 TRACKED prebuilts and a build must not write into it.
if (-not $OutLibDir) { throw "-OutLibDir is required. Run this through build-deps.ps1, which resolves it." }
$outDir = $OutLibDir
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Self-skip (L11): keyed like the CPU leg; the marker lib for the check is
# ggml-cuda.lib, the leg's defining output.
$selfStamp = Join-Path $outDir 'llama_cpp_cuda.stamp'
$scriptSha = 'unknown'
try { $scriptSha = (Get-FileHash -Algorithm SHA256 -Path $PSCommandPath).Hash.ToLowerInvariant() } catch {}
$srcSha = 'unknown'
try { $srcSha = (git -C $llamaSrc rev-parse --short HEAD).Trim() } catch {}
$selfStampValue = "$srcSha`:$scriptSha`:$Configuration`:$Platform"
if ((Test-Path (Join-Path $outDir 'ggml-cuda.lib')) -and (Test-Path $selfStamp)) {
    if ((Get-Content $selfStamp -Raw).Trim() -eq $selfStampValue) {
        Write-Host "llama.cpp (CUDA) is up to date in $outDir"
        exit 0
    }
}

Write-Host "Configuring llama.cpp (CUDA) in $buildDir"
$generator = Get-MTVSGenerator
Write-Host "Using CMake generator: $generator"

# CUDA always builds with the default (MSVC) toolset, so no -T is passed and
# the cached toolset must be empty for this directory to be reusable.
$null = Reset-MTStaleCMakeCache -BuildDir $buildDir -Platform $Platform -Generator $generator `
    -Toolset '' -SourceDir $llamaSrc -Label 'llama.cpp (CUDA)'

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
    cmake -S $llamaSrc -B $buildDir -G $generator -A $Platform `
        -DBUILD_SHARED_LIBS=OFF `
        -DLLAMA_BUILD_COMMON=OFF `
        -DLLAMA_BUILD_TESTS=OFF `
        -DLLAMA_BUILD_TOOLS=OFF `
        -DLLAMA_BUILD_EXAMPLES=OFF `
        -DLLAMA_BUILD_SERVER=OFF `
        -DLLAMA_TOOLS_INSTALL=OFF `
        -DLLAMA_TESTS_INSTALL=OFF `
        -DGGML_CUDA=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for llama.cpp (CUDA)" }

    Write-Host "Building llama.cpp libs ($Configuration)"
    cmake --build $buildDir --config $Configuration --target llama ggml ggml-base ggml-cpu ggml-cuda
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for llama.cpp (CUDA)" }
} finally {
    $ErrorActionPreference = $savedEap
}

$copyList = @(
    (Join-Path $buildDir "src\$Configuration\llama.lib"),
    (Join-Path $buildDir "ggml\src\$Configuration\ggml.lib"),
    (Join-Path $buildDir "ggml\src\$Configuration\ggml-base.lib"),
    (Join-Path $buildDir "ggml\src\ggml-cpu\$Configuration\ggml-cpu.lib"),
    (Join-Path $buildDir "ggml\src\ggml-cuda\$Configuration\ggml-cuda.lib")
)

foreach ($p in $copyList) {
    if (-not (Test-Path $p)) {
        throw "Expected library not found: $p"
    }
    Copy-Item -Force $p $outDir
}

Set-Content -NoNewline -Path (Join-Path $outDir 'llama_cpp_cuda.stamp') -Value $selfStampValue
Write-Host "Done. Copied CUDA libs into: $outDir"

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
    # The SAME spelling Directory.Build.props gives MTOutRoot/MTBuildRoot under
    # MT_STANDALONE=1 -- see build-llama_cpp_cpu.ps1 for why the bare form was wrong.
    Join-Path (Get-MTBuildRoot) "_standalone\windows\$Platform\$Configuration"
}
$versionHeader = Join-Path $mtGenInclude 'include\Sci\Llama\llama_cpp_version.h'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $versionHeader) | Out-Null
@"
// Auto-generated by build-llama_cpp_cuda.ps1 — do not edit
#pragma once
#define MT_LLAMA_CPP_VERSION "$versionTag"
"@ | Set-Content -Path $versionHeader -Encoding UTF8
