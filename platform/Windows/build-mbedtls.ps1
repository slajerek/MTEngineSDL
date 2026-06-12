param(
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
    [string]$Config = 'Release',
    [ValidateSet('x64','ARM64')]
    [string]$Platform = 'x64',
    [ValidateSet('Clang','MSVC')]
    [string]$Compiler = 'Clang'
)

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

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$outDir   = Join-Path $repoRoot "platform\Windows\libs\$Platform\$Config"
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

    $stampValue = "disabled`:$scriptSha`:$Config`:$Platform"
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

$stampValue = "$mbedSha`:$scriptSha`:$Config`:$Platform"
if ((Test-Path $outLib) -and (Test-Path $stamp)) {
    if ((Get-Content $stamp -Raw).Trim() -eq $stampValue) {
        exit 0
    }
}

$cmakeToolset = if ($Compiler -eq 'Clang') { @('-T', 'ClangCL') } else { @() }

Write-Host "Configuring mbedTLS in $buildDir"
# Auto-detect the default Visual Studio CMake generator for the installed VS
# (handles 'Visual Studio 17 2022', 'Visual Studio 18 2026', ... without hardcoding).
$genMatch = cmake --help | Select-String -Pattern '^\*\s+(Visual Studio \d+ \d+)'
if (-not $genMatch) { throw "Could not detect the default Visual Studio CMake generator from 'cmake --help'." }
$generator = $genMatch.Matches[0].Groups[1].Value
Write-Host "Using CMake generator: $generator"

cmake -S $mbedSrc -B $buildDir -G $generator -A $Platform @cmakeToolset `
    -DBUILD_SHARED_LIBS=OFF `
    -DENABLE_TESTING=OFF `
    -DENABLE_PROGRAMS=OFF `
    -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>"

Write-Host "Building mbedTLS libs ($Config)"
cmake --build $buildDir --config $Config --target mbedcrypto mbedx509 mbedtls

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
