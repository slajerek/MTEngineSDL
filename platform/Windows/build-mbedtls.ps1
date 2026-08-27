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

function Require-Path([string]$path, [string]$what) {
    if (-not (Test-Path $path)) {
        throw "Missing ${what}: $path"
    }
}

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
# REQUIRED, not defaulted. The caller decides where archives go; the only
# fallback in the system lives in build-deps.ps1, so the old
# platform\Windows\libs path cannot creep back in here -- that directory holds
# 25 TRACKED prebuilts and a build must not write into it.
if (-not $OutLibDir) { throw "-OutLibDir is required. Run this through build-deps.ps1, which resolves it." }
$outDir = $OutLibDir
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$outLib   = Join-Path $outDir 'mbedtls_bundle.lib'
$stamp    = Join-Path $outDir 'mbedtls_bundle.stamp'

$scriptSha = 'unknown'
try {
    $scriptSha = (Get-FileHash -Algorithm SHA256 -Path $PSCommandPath).Hash.ToLowerInvariant()
} catch {}

if ($env:MT_ENABLE_MBEDTLS -eq '0') {
    # Create a stub library so projects that always link this lib still build.
    Require-Command lib
    Require-Command cl

    $stampValue = "disabled`:$scriptSha`:$Configuration`:$Platform"
    if ((Test-Path $outLib) -and (Test-Path $stamp)) {
        if ((Get-Content $stamp -Raw).Trim() -eq $stampValue) {
            exit 0
        }
    }

    $tmpDir = Join-Path $outDir 'mbedtls_stub'
    if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir }
    New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

    $stubC = Join-Path $tmpDir 'mbedtls_stub.c'
    $stubObj = Join-Path $tmpDir 'mbedtls_stub.obj'
    Set-Content -NoNewline -Path $stubC -Value "int mbedtls_bundle_disabled_stub = 0;`r`n"

    & cl /nologo /c $stubC /Fo$stubObj | Out-Null
    if (Test-Path $outLib) { Remove-Item -Force $outLib }
    & lib /nologo /OUT:$outLib $stubObj | Out-Null

    Set-Content -NoNewline -Path $stamp -Value $stampValue
    if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir }
    exit 0
}

Require-Command cmake
Require-Command lib

$mbedSrc  = Join-Path $repoRoot 'other\lib\mbedtls'
Require-Path $mbedSrc 'mbedtls submodule (run: git submodule update --init --recursive)'

$buildDir = Join-Path $repoRoot "other\lib\mbedtls.windows-$Platform"

$mbedSha = 'unknown'
try {
    $mbedSha = (git -C $mbedSrc rev-parse --short HEAD).Trim()
} catch {}

$stampValue = "$mbedSha`:$scriptSha`:$Configuration`:$Platform"
if ((Test-Path $outLib) -and (Test-Path $stamp)) {
    if ((Get-Content $stamp -Raw).Trim() -eq $stampValue) {
        exit 0
    }
}

$cmakeToolsetName = if ($Compiler -eq 'Clang') { 'ClangCL' } else { '' }
$cmakeToolset = if ($cmakeToolsetName) { @('-T', $cmakeToolsetName) } else { @() }

Write-Host "Configuring mbedTLS in $buildDir"
$generator = Get-MTVSGenerator
Write-Host "Using CMake generator: $generator"

# The directory is already keyed by architecture, but a Visual Studio upgrade
# or a -Compiler switch leaves a cache CMake will refuse to reconfigure.
$null = Reset-MTStaleCMakeCache -BuildDir $buildDir -Platform $Platform -Generator $generator `
    -Toolset $cmakeToolsetName -SourceDir $mbedSrc -Label 'mbedTLS'

# RELAX $ErrorActionPreference AROUND THESE TWO NATIVE CALLS, same issue and
# same fix as build-llama_cpp_cpu.ps1's Build-CMake / build-image_codecs.ps1:
# PowerShell 5.1 wraps ANY stderr line from a native command into a
# terminating NativeCommandError under 'Stop', regardless of exit code -- so a
# benign "CMake Deprecation Warning: cmake_minimum_required" from mbedTLS's own
# (third-party, unmaintained-by-us) CMakeLists.txt on an otherwise-successful
# configure aborted this script outright. $LASTEXITCODE below is the real,
# sufficient success/failure signal.
$savedEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    cmake -S $mbedSrc -B $buildDir -G $generator -A $Platform @cmakeToolset `
        -DBUILD_SHARED_LIBS=OFF `
        -DENABLE_TESTING=OFF `
        -DENABLE_PROGRAMS=OFF `
        -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for mbedTLS" }

    Write-Host "Building mbedTLS libs ($Configuration)"
    cmake --build $buildDir --config $Configuration --target mbedcrypto mbedx509 mbedtls
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for mbedTLS" }
} finally {
    $ErrorActionPreference = $savedEap
}

function Find-Lib([string]$name) {
    $hits = Get-ChildItem -Path $buildDir -Recurse -File -Filter $name | Select-Object -First 1
    if (-not $hits) { throw "Expected library not found: $name" }
    return $hits.FullName
}

$crypto = Find-Lib 'mbedcrypto.lib'
$x509   = Find-Lib 'mbedx509.lib'
$tls    = Find-Lib 'mbedtls.lib'

if (Test-Path $outLib) { Remove-Item -Force $outLib }
Write-Host "Packing into $outLib"
lib /nologo /OUT:$outLib $crypto $x509 $tls

Set-Content -NoNewline -Path $stamp -Value $stampValue
Write-Host "Done. Output: $outLib"
