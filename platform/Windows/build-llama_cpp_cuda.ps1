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

# ---------------------------------------------------------------------------
# THE TOOLKIT: a real installation if CUDA_PATH names one, else the acquired
# tree from acquire-cuda-toolkit.ps1. Set back into the environment so that
# MSBuild -- which reads environment variables as properties -- and the plugin
# project's $(CUDA_PATH)\lib\x64 see the same toolkit CMake configures with.
# ---------------------------------------------------------------------------
$cudaPath = Resolve-MTCudaPath
if (-not $cudaPath) {
    throw "No CUDA toolkit. Either install NVIDIA CUDA Toolkit (sets CUDA_PATH) or run platform\Windows\acquire-cuda-toolkit.ps1 -- the installer refuses ARM64 hosts; the acquisition does not."
}
$env:CUDA_PATH = $cudaPath
$nvcc = Join-Path $cudaPath 'bin\nvcc.exe'
$cuobjdump = Join-Path $cudaPath 'bin\cuobjdump.exe'

# ---------------------------------------------------------------------------
# NINJA, NOT THE VISUAL STUDIO GENERATOR. CMake's CUDA support under the VS
# generator emits projects that build .cu files through the "CUDA 12.x.targets"
# customization, which NVIDIA's installer drops into MSBuild and nothing else
# provides -- the redistributable archives do not carry it, and on this
# repository's ARM64 build machine the installer will not run. Ninja drives
# nvcc directly and needs none of that. Visual Studio bundles a ninja.exe.
#
# Ninja is single-config: CMAKE_BUILD_TYPE at configure time, no --config, and
# no per-configuration subdirectory in the output tree -- the copy step below
# locates each archive by name rather than assuming the VS layout.
# ---------------------------------------------------------------------------
$ninja = Get-MTNinja
if (-not $ninja) { throw "ninja.exe not found: neither on PATH nor bundled with Visual Studio (Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja)." }

# A Ninja build has no vcvars of its own: cl.exe, INCLUDE and LIB must already
# be in this process. Import-MTVCEnvironment is a no-op inside a real Developer
# prompt and does the vcvarsall dance otherwise, choosing HostARM64\x64 on an
# ARM64 machine -- the cross compiler nvcc was measured driving to PTX here.
if (-not (Import-MTVCEnvironment -Platform $Platform)) {
    throw "Could not import a Visual Studio environment for $Platform (vswhere/vcvarsall)."
}
$cl = (Get-Command cl.exe -ErrorAction SilentlyContinue)
if (-not $cl) { throw "cl.exe is not on PATH after importing the VC environment." }
$clPath = $cl.Source

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
# Per-unit store (L16). The unit builds in a directory keyed only by the
# capabilities IT reads, then its outputs are copied into the shared view. The
# body is wrapped in try/finally because a stamp hit and a capability-off stub
# both leave early and both still owe the view a copy.
$outDir = Use-MTStore -Unit 'llama_cpp' -View $OutLibDir
try {
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Self-skip (L11): keyed like the CPU leg; the marker lib for the check is
# ggml-cuda.lib, the leg's defining output. The toolkit is in the key too: a
# different CUDA version or location must rebuild, since the archives it
# produced embed that toolkit's PTX and link against its runtime.
$selfStamp = Join-Path $outDir 'llama_cpp_cuda.stamp'
$scriptSha = 'unknown'
try { $scriptSha = (Get-FileHash -Algorithm SHA256 -Path $PSCommandPath).Hash.ToLowerInvariant() } catch {}
$srcSha = 'unknown'
try { $srcSha = (git -C $llamaSrc rev-parse --short HEAD).Trim() } catch {}
$cudaVersionLine = 'unknown'
try { $cudaVersionLine = ((& $nvcc --version 2>$null) | Where-Object { $_ -match '^Cuda compilation tools' } | Select-Object -First 1).Trim() } catch {}
$selfStampValue = "$srcSha`:$scriptSha`:$Configuration`:$Platform`:ninja`:$cudaVersionLine`:$cudaPath"
if ((Test-Path (Join-Path $outDir 'ggml-cuda.lib')) -and (Test-Path $selfStamp)) {
    if ((Get-Content $selfStamp -Raw).Trim() -eq $selfStampValue) {
        Write-Host "llama.cpp (CUDA) is up to date in $outDir"
        exit 0
    }
}

Write-Host "Configuring llama.cpp (CUDA) in $buildDir"
Write-Host "  toolkit : $cudaPath"
Write-Host "  ninja   : $ninja"
Write-Host "  host cl : $clPath"

# No -Platform: Ninja has no generator platform, and the helper compares only
# what it is given. A cache left by the old VS-generator leg differs in
# generator and is wiped for that reason.
$null = Reset-MTStaleCMakeCache -BuildDir $buildDir -Generator 'Ninja' `
    -Toolset '' -SourceDir $llamaSrc -Label 'llama.cpp (CUDA)'

# FORWARD SLASHES FOR EVERY PATH HANDED TO CMAKE -- the same lesson
# build-opencv.ps1 learned the hard way with '\U' in C:\Users.
$cm = @{
    ninja  = ($ninja -replace '\\','/')
    cl     = ($clPath -replace '\\','/')
    nvcc   = ($nvcc -replace '\\','/')
    cuda   = ($cudaPath -replace '\\','/')
}

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
    # GGML_OPENMP=OFF MATCHES THE CPU LEG, and it is a shipping requirement
    # rather than a preference. With OpenMP on, the plugin DLL imports
    # VCOMP140.DLL -- a Visual C++ redistributable, not a CUDA one, so the
    # runtime staging below (which ships what it finds in the toolkit's bin\)
    # correctly excludes it and the plugin then fails to load on any machine
    # without the VC++ redistributable installed. It fails by falling back to
    # the CPU backend, silently, which is the exact failure that staging exists
    # to prevent. Everything else here links /MT precisely so the shipped
    # artifacts need no VC runtime; one OpenMP import would undo that.
    # build-llama_cpp_cpu.ps1 has passed this since it was written.
    #
    # GGML_NATIVE=OFF IS THE POINT. With it on, ggml resolves
    # CMAKE_CUDA_ARCHITECTURES to "native" -- the GPUs present at build time --
    # and this machine has none. Off, ggml applies its maintained portable
    # list: 50/61/70/75/80 as PTX (JIT-compiled on first run, so cards newer
    # than the toolkit still work) and 86/89/120a/121a as native code for the
    # common ones. Not pinned here on purpose: the list tracks upstream when
    # the submodule bumps, and the verification below reads back whatever was
    # chosen rather than trusting a copy of it.
    cmake -S $llamaSrc -B $buildDir -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$($cm.ninja)" `
        "-DCMAKE_BUILD_TYPE=$Configuration" `
        "-DCMAKE_C_COMPILER=$($cm.cl)" `
        "-DCMAKE_CXX_COMPILER=$($cm.cl)" `
        "-DCMAKE_CUDA_COMPILER=$($cm.nvcc)" `
        "-DCMAKE_CUDA_HOST_COMPILER=$($cm.cl)" `
        "-DCUDAToolkit_ROOT=$($cm.cuda)" `
        (Get-MTCrtCMakeArgs $Configuration) `
        -DBUILD_SHARED_LIBS=OFF `
        -DLLAMA_BUILD_COMMON=OFF `
        -DLLAMA_BUILD_TESTS=OFF `
        -DLLAMA_BUILD_TOOLS=OFF `
        -DLLAMA_BUILD_EXAMPLES=OFF `
        -DLLAMA_BUILD_SERVER=OFF `
        -DLLAMA_TOOLS_INSTALL=OFF `
        -DLLAMA_TESTS_INSTALL=OFF `
        -DGGML_NATIVE=OFF `
        -DGGML_OPENMP=OFF `
        -DGGML_CUDA=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for llama.cpp (CUDA)" }

    # EXPECT THIS TO BE LONG. ggml-cuda is on the order of a hundred .cu files,
    # each compiled for every architecture above, by an nvcc that is itself
    # running under emulation on this machine. MT_BUILD_JOBS is honoured
    # (nvcc is memory-hungry and the box has 8 GB); unset means Ninja's own
    # default.
    $buildArgs = @('--build', $buildDir, '--target', 'llama', 'ggml', 'ggml-base', 'ggml-cpu', 'ggml-cuda')
    if ($env:MT_BUILD_JOBS) { $buildArgs += @('--parallel', $env:MT_BUILD_JOBS) }
    Write-Host "Building llama.cpp libs ($Configuration, Ninja)"
    cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for llama.cpp (CUDA)" }
} finally {
    $ErrorActionPreference = $savedEap
}

# Locate each archive by NAME. Ninja has no <Configuration>\ subdirectory, and
# llama.cpp has moved these between target directories before; a search that
# demands exactly one hit is robust to both and loud about anything else.
#
# NAME THE DESTINATION FILE, do not hand Copy-Item the directory. `Copy-Item
# -Force <src> <dir>` copies INTO the directory only when it already exists;
# when it does not, it silently creates a FILE at that path and each further
# copy overwrites it -- five archives in, the "directory" is one 223 MB file
# called `libs` and the failure surfaces later as a missing lib. Measured
# 2026-09-07. An explicit destination path fails loudly instead.
if (-not (Test-Path -LiteralPath $outDir -PathType Container)) {
    throw "Output directory is not a directory: $outDir"
}
foreach ($lib in @('llama.lib', 'ggml.lib', 'ggml-base.lib', 'ggml-cpu.lib', 'ggml-cuda.lib')) {
    $hits = @(Get-ChildItem -Path $buildDir -Recurse -Filter $lib -File -ErrorAction SilentlyContinue)
    if ($hits.Count -ne 1) {
        throw "Expected exactly one $lib under $buildDir, found $($hits.Count)" + $(if ($hits.Count) { ":`n  " + (($hits | ForEach-Object { $_.FullName }) -join "`n  ") } else { '' })
    }
    Copy-Item -LiteralPath $hits[0].FullName -Destination (Join-Path $outDir $lib) -Force
}

# ---------------------------------------------------------------------------
# PROVE THE ARCHITECTURES, do not assume them. Nobody can run this plugin on a
# GPU here, so "it built" is the only evidence there would otherwise be about
# which cards it runs on. cuobjdump lists what is actually inside the archive:
# one ELF (cubin) per -real architecture and one PTX per -virtual one. The
# expected set is read back from the CMake cache -- whatever ggml chose -- so
# this check cannot drift from the build it is checking.
#
# THE EXPECTED LIST COMES FROM build.ninja, NOT FROM CMakeCache.txt. ggml sets
# CMAKE_CUDA_ARCHITECTURES as an ordinary variable and puts the result on the
# target, so it never reaches the cache -- reading it there found nothing and
# failed a build whose artifact was perfectly good (measured 2026-09-07, after
# all 302 objects had compiled). build.ninja carries the flags nvcc was
# actually given, which is the better source anyway: it is the command line,
# not an intention.
#
# Spellings: CMake "86-real" becomes --generate-code=arch=compute_86,code=[sm_86]
# and shows as sm_86 in --list-elf; "75-virtual" becomes code=[compute_75] and
# shows as sm_75 in --list-ptx. An architecture suffix ("120a") rides along.
# ---------------------------------------------------------------------------
$ninjaFile = Join-Path $buildDir 'build.ninja'
if (-not (Test-Path $ninjaFile)) { throw "build.ninja not found in $buildDir" }
$wantElf = @{}; $wantPtx = @{}
foreach ($m in [regex]::Matches((Get-Content -Raw -LiteralPath $ninjaFile), 'arch=compute_([0-9]+[a-z]?),code=\[?(compute|sm)_([0-9]+[a-z]?)\]?')) {
    if ($m.Groups[2].Value -eq 'sm') { $wantElf["sm_$($m.Groups[3].Value)"] = $true }
    else                             { $wantPtx["sm_$($m.Groups[3].Value)"] = $true }
}
if ($wantElf.Count + $wantPtx.Count -eq 0) {
    throw "No --generate-code flags in build.ninja; the CUDA architecture list could not be determined"
}

$cudaLib = Join-Path $outDir 'ggml-cuda.lib'
$haveElf = @{}; $havePtx = @{}
foreach ($l in @(& $cuobjdump --list-elf $cudaLib 2>&1)) { if ($l -match '\.(sm_[0-9]+[a-z]?)\.cubin') { $haveElf[$Matches[1]] = $true } }
foreach ($l in @(& $cuobjdump --list-ptx $cudaLib 2>&1)) { if ($l -match '\.(sm_[0-9]+[a-z]?)\.ptx')   { $havePtx[$Matches[1]] = $true } }

$archMissing = @()
foreach ($sm in $wantElf.Keys) { if (-not $haveElf[$sm]) { $archMissing += "$sm (native code)" } }
foreach ($sm in $wantPtx.Keys) { if (-not $havePtx[$sm]) { $archMissing += "$sm (PTX)" } }
$archSpec = "elf=" + (($haveElf.Keys | Sort-Object) -join ',') + " ptx=" + (($havePtx.Keys | Sort-Object) -join ',')
Write-Host "CUDA architectures in ggml-cuda.lib: $archSpec"
if ($archMissing.Count -gt 0) {
    throw "ggml-cuda.lib lacks code for: $($archMissing -join ', ') -- nvcc was asked for it but cuobjdump does not find it. Do not ship this."
}
Set-Content -NoNewline -Path (Join-Path $outDir 'llama_cpp_cuda.archs') -Value $archSpec

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

} finally {
    # Runs on `exit` and on a terminating error alike.
    Complete-MTStore
}
