param(
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64','ARM64')]
    [string]$Platform = 'x64',
    [ValidateSet('Clang','MSVC')]
    [string]$Compiler = 'Clang',
    [switch]$SkipCuda,

    # Forwarded to whichever backend script this dispatches to. A CPU and a CUDA
    # build produce materially different archives, and they must land in the
    # directory their own resolve keyed -- see Get-MTLlamaBackendOption.
    [string]$OutLibDir
)

if ($Platform -eq 'ARM64') { $SkipCuda = $true }

$ErrorActionPreference = 'Stop'

function Require-Command([string]$name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        throw "Missing required tool '$name'. Run from 'Developer PowerShell for VS 2022' and ensure CMake is installed." 
    }
}

function Require-Path([string]$path, [string]$what) {
    if (-not (Test-Path $path)) {
        throw "Missing ${what}: $path"
    }
}

. "$PSScriptRoot\mt-build-common.ps1"

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$llamaDir = Join-Path $repoRoot 'other\lib\llama.cpp'

# THE CAPABILITY GATE -- build-llama_cpp.sh on macOS has had one since the
# capability programme started; this dispatcher never did, so MT_CAP_LLM=0 still
# built llama.cpp on Windows.
#
# It gates HERE rather than in the two backend scripts because this is the only
# place that knows a llama build was asked for at all, and because the submodule
# check below must not run first: with the capability off an app's
# build-windows.ps1 never fetches other/lib/llama.cpp, so Require-Path would turn
# the saving into a failure on a clean clone.
#
# Only the CPU archive gets a stub. The CUDA half is a separate plugin DLL that
# nothing links unconditionally, so its absence is not a link error.
if ($env:MT_ENABLE_LLAMA_CPP -eq '0') {
    if (-not $OutLibDir) { throw "-OutLibDir is required. Run this through build-deps.ps1, which resolves it." }
    Require-Command cl
    Require-Command lib

    $scriptSha = 'unknown'
    try {
        $scriptSha = (Get-FileHash -Algorithm SHA256 -Path $PSCommandPath).Hash.ToLowerInvariant()
    } catch {}

    New-MTCapsStubArchive -OutLib (Join-Path $OutLibDir 'llama_cpp.lib') `
                          -Stamp  (Join-Path $OutLibDir 'llama_cpp.stamp') `
                          -Symbol 'llama_cpp' `
                          -StampValue "disabled`:$scriptSha`:$Configuration`:$Platform"
    exit 0
}

Require-Command cmake
Require-Command msbuild
Require-Path $llamaDir 'llama.cpp submodule (run: git submodule update --init --recursive)'

Write-Host "=== Building llama.cpp CPU bundle ==="
& (Join-Path $repoRoot 'platform\Windows\build-llama_cpp_cpu.ps1') -OutLibDir $OutLibDir -Configuration $Configuration -Platform $Platform -Compiler $Compiler

if (-not $SkipCuda) {
    if (-not $env:CUDA_PATH) {
        Write-Host "CUDA_PATH not set - skipping CUDA build (install NVIDIA CUDA Toolkit to enable)" -ForegroundColor Yellow
        $SkipCuda = $true
    }
}

if (-not $SkipCuda) {

    Write-Host "=== Building llama.cpp CUDA libs ==="
    & (Join-Path $repoRoot 'platform\Windows\build-llama_cpp_cuda.ps1') -OutLibDir $OutLibDir -Configuration $Configuration -Platform $Platform

    Write-Host "=== Building CUDA plugin DLL (mt_llama_cuda_backend) ==="
    $sln = Join-Path $repoRoot 'platform\Windows\MTEngineSDL.sln'
    Require-Path $sln 'solution file'

    msbuild $sln /m /t:mt_llama_cuda_backend /p:Configuration=$Configuration /p:Platform=$Platform /p:PlatformToolset=v143

    $pluginOutDir = Join-Path $repoRoot "platform\Windows\bin\$Platform\$Configuration"
    $pluginDll = Join-Path $pluginOutDir 'mt_llama_cuda_backend.dll'
    Require-Path $pluginDll 'CUDA plugin DLL'

    # Put it into a default runtime location (relative to current working directory)
    $dataLibDir = Join-Path $pluginOutDir 'Data\lib'
    New-Item -ItemType Directory -Force -Path $dataLibDir | Out-Null
    Copy-Item -Force $pluginDll $dataLibDir

    Write-Host "CUDA plugin built: $pluginDll"
    Write-Host "CUDA plugin copied to: $(Join-Path $dataLibDir 'mt_llama_cuda_backend.dll')"
}

Write-Host "=== Done ==="
Write-Host "CPU engine lib: $(Join-Path $repoRoot \"platform\\Windows\\libs\\$Platform\\$Configuration\\llama_cpp.lib\")"
if (-not $SkipCuda) {
    Write-Host "CUDA libs: $(Join-Path $repoRoot \"platform\\Windows\\libs\\$Platform\\$Configuration\")"
    Write-Host "Plugin output: $(Join-Path $repoRoot \"platform\\Windows\\bin\\$Platform\\$Configuration\\mt_llama_cuda_backend.dll\")"
}
