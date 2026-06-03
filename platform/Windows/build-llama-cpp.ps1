param(
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
    [string]$Config = 'Release',
    [ValidateSet('x64','ARM64')]
    [string]$Platform = 'x64',
    [ValidateSet('Clang','MSVC')]
    [string]$Compiler = 'Clang',
    [switch]$SkipCuda
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

Require-Command cmake
Require-Command msbuild

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$llamaDir = Join-Path $repoRoot 'other\lib\llama.cpp'
Require-Path $llamaDir 'llama.cpp submodule (run: git submodule update --init --recursive)'

Write-Host "=== Building llama.cpp CPU bundle ==="
& (Join-Path $repoRoot 'platform\Windows\build-llama_cpp_cpu.ps1') -Config $Config -Platform $Platform -Compiler $Compiler

if (-not $SkipCuda) {
    if (-not $env:CUDA_PATH) {
        Write-Host "CUDA_PATH not set - skipping CUDA build (install NVIDIA CUDA Toolkit to enable)" -ForegroundColor Yellow
        $SkipCuda = $true
    }
}

if (-not $SkipCuda) {

    Write-Host "=== Building llama.cpp CUDA libs ==="
    & (Join-Path $repoRoot 'platform\Windows\build-llama_cpp_cuda.ps1') -Config $Config -Platform $Platform

    Write-Host "=== Building CUDA plugin DLL (mt_llama_cuda_backend) ==="
    $sln = Join-Path $repoRoot 'platform\Windows\MTEngineSDL.sln'
    Require-Path $sln 'solution file'

    msbuild $sln /m /t:mt_llama_cuda_backend /p:Configuration=$Config /p:Platform=$Platform /p:PlatformToolset=v143

    $pluginOutDir = Join-Path $repoRoot "platform\Windows\bin\$Platform\$Config"
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
Write-Host "CPU engine lib: $(Join-Path $repoRoot \"platform\\Windows\\libs\\$Platform\\$Config\\llama_cpp.lib\")"
if (-not $SkipCuda) {
    Write-Host "CUDA libs: $(Join-Path $repoRoot \"platform\\Windows\\libs\\$Platform\\$Config\")"
    Write-Host "Plugin output: $(Join-Path $repoRoot \"platform\\Windows\\bin\\$Platform\\$Config\\mt_llama_cuda_backend.dll\")"
}
