<#
.SYNOPSIS
    Build libjpeg-turbo for Windows as one static archive, MTJpeg.lib.
.DESCRIPTION
    libjpeg-turbo: the engine's ONE jpeg_* library (plan 2026-10-07-libjpeg-turbo).
    Core, not a codec: JPEGWriter needs it on EVERY configuration, so this runs
    before build-image_codecs.ps1, which builds libultrahdr and LibRaw against
    the prefix below and merges MTJpeg.lib into its bundle (stub included).

    Outputs (into this unit's STORE; Complete-MTStore copies them to the view):
      MTJpeg.lib       turbo's jpeg-static.lib + jpegtran's transupp
      MTJpeg.stamp     script hash, turbo version, Platform-Configuration and the
                       SIMD state configure actually reported
      jpeg\include\    jpeglib.h jconfig.h jmorecfg.h jerror.h transupp.h
    Work prefix (read by build-image_codecs.ps1, outside every checkout):
      <work>\jpeg\install-<Platform>-<Configuration>\{lib,include}
    PER CONFIGURATION, because Build-CMake sets the CRT per configuration and a
    Debug link against a /MT archive fails LNK2038.

    Environment:
      MT_PROD_BUILD=1     a final build (app-build-windows.ps1 -Prod). A build
                          without SIMD -- x64 without NASM, or either platform
                          whose requested SIMD turbo's configure turned off --
                          is then an ERROR instead of a warning: a shipped
                          build must not silently lose SIMD. (ARM64 needs no
                          NASM: its Neon code is C intrinsics.)

    BUILD TOOL (x64): NASM, on PATH or in MSYS2's usr\bin (pacman -S nasm in an
    MSYS2 shell; or https://www.nasm.us/). Set $env:MSYS2_ROOT if MSYS2 is not
    at C:\msys64.
      MT_JPEG_X86_SIMD=0  force the scalar x64 path (the test override).

    UNVERIFIED on a Windows machine as of 2026-10-09 (written on macOS beside the
    verified macOS twin); the Windows pass is Task 7 of the plan.
.PARAMETER OutLibDir
    The caps-keyed libs view. REQUIRED (build-deps.ps1 resolves it).
#>
param(
    [ValidateSet('x64','ARM64')]
    [string]$Platform,

    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release',

    [int]$Jobs = $env:NUMBER_OF_PROCESSORS,

    [string]$OutLibDir
)

$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\mt-build-common.ps1"

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# lib.exe, resolved the way build-image_codecs.ps1 resolves it (a plain shell
# has no VC tools on PATH).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$libExe = & $vswhere -latest -requires Microsoft.Component.MSBuild `
    -find "VC\Tools\MSVC\**\bin\HostX64\x64\lib.exe" 2>$null | Select-Object -First 1
if (-not $libExe) {
    Write-Error "lib.exe not found. Install Visual Studio 2022 with C++ workload."
    exit 1
}

$Platform = Resolve-MTPlatform $Platform

if (-not $OutLibDir) { throw "-OutLibDir is required. Run this through build-deps.ps1, which resolves it." }

$cacheDir = Get-MTCapsWorkDir 'jpeg'
$downloadDir = "$cacheDir\downloads"
$srcDir = "$cacheDir\src"
$buildDir = "$cacheDir\build-win-$Platform-$Configuration"
# = Join-Path (Get-MTCapsWorkDir 'jpeg') "install-$Platform-$Configuration", the
# path build-image_codecs.ps1 reads.
$prefixDir = "$cacheDir\install-$Platform-$Configuration"

# Per-unit store (L16). libjpeg-turbo reads no capability: one bucket per
# machine (per Platform/Configuration view). try/finally because a stamp hit
# leaves early and still owes the view a copy.
$outLibDir = Use-MTStore -Unit 'jpeg' -View $OutLibDir
try {
$outLib = "$outLibDir\MTJpeg.lib"
$stampFile = "$outLibDir\MTJpeg.stamp"
$stagedInc = Join-Path $outLibDir 'jpeg\include'

# Same pin as platform/MacOS/build-jpeg.sh (newest stable on 2026-10-09).
$libjpegTurboVersion = "3.2.0"
$libjpegTurboUrl  = "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$libjpegTurboVersion/libjpeg-turbo-$libjpegTurboVersion.tar.gz"
$libjpegTurboHash = "6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e"

$jpegHeaders = @('jpeglib.h', 'jconfig.h', 'jmorecfg.h', 'jerror.h', 'transupp.h')

New-Item -ItemType Directory -Force -Path $downloadDir, $srcDir, $outLibDir | Out-Null

$scriptHash = (Get-FileHash -Path $MyInvocation.MyCommand.Path -Algorithm SHA256).Hash

# x64 NASM: PATH, else MSYS2's usr\bin (build-video_codecs.ps1's lookup).
function Find-MTNasm {
    $cmd = Get-Command nasm -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $msys2Root = $env:MSYS2_ROOT
    if (-not $msys2Root) { $msys2Root = "C:\msys64" }
    $candidate = Join-Path $msys2Root "usr\bin\nasm.exe"
    if (Test-Path $candidate) { return $candidate }
    return $null
}

# STAMP RULE 1 -- the SIMD decision and the prod refusal come FIRST, before the
# stamp comparison (see the macOS twin): a development scalar stamp must never
# satisfy a final build.
$isProd = ($env:MT_PROD_BUILD -eq '1')
$nasmExe = $null
$simdRequested = 1
if ($Platform -eq 'x64') {
    if ($env:MT_JPEG_X86_SIMD -ne '0') { $nasmExe = Find-MTNasm }
    if (-not $nasmExe) {
        if ($isProd) { throw "error: libjpeg-turbo x64 needs nasm for SIMD in a final build (install it: pacman -S nasm in MSYS2, or https://www.nasm.us/)" }
        Write-Warning "libjpeg-turbo x64 built WITHOUT SIMD (nasm not found or MT_JPEG_X86_SIMD=0; install it: pacman -S nasm in MSYS2, or https://www.nasm.us/)"
        $simdRequested = 0
    }
}

function Get-StampValue([int]$simd) {
    return "$scriptHash|libjpeg-turbo-$libjpegTurboVersion|$Platform-$Configuration|simd=$simd"
}

# STAMP RULE 2 -- a hit requires every artefact the CONSUMERS read: the store's
# MTJpeg.lib and staged jpeglib.h AND the work prefix build-image_codecs.ps1
# reads. A store hit with a missing prefix is a miss.
function Test-StampHit {
    if (-not ((Test-Path $stampFile) -and (Test-Path $outLib) -and (Test-Path (Join-Path $stagedInc 'jpeglib.h')))) { return $false }
    $stamp = (Get-Content $stampFile -Raw).Trim()
    if ($Platform -eq 'x64') {
        if ($stamp -ne (Get-StampValue $simdRequested)) { return $false }
    } else {
        # ARM64: decided by turbo's configure; either recorded value matches.
        if (($stamp -ne (Get-StampValue 1)) -and ($stamp -ne (Get-StampValue 0))) { return $false }
    }
    # A final build never accepts a scalar build from the cache, on either platform.
    if ($isProd -and ($stamp -like '*simd=0*')) { return $false }
    foreach ($rel in @('lib\jpeg-static.lib', 'lib\jpeg_transupp.lib')) {
        if (-not (Test-Path (Join-Path $prefixDir $rel))) { return $false }
    }
    foreach ($h in $jpegHeaders) {
        if (-not (Test-Path (Join-Path $prefixDir "include\$h"))) { return $false }
    }
    return $true
}

if (Test-StampHit) {
    Write-Host "libjpeg-turbo is up to date: $outLib" -ForegroundColor Green
    exit 0
}

# ---- helpers (build-image_codecs.ps1's) ----

function Get-Sha256($filePath) {
    return (Get-FileHash -Path $filePath -Algorithm SHA256).Hash.ToLower()
}

function Download-Archive($name, $url, $expectedHash) {
    $archive = Join-Path $downloadDir "$name.tar.gz"
    $part = "$archive.part"
    if (Test-Path $archive) {
        if ((Get-Sha256 $archive) -eq $expectedHash) { return }
        Write-Warning "Cached $archive does not match the pinned SHA-256 (pin changed?) -- downloading again."
        Remove-Item -Force $archive
    }
    Remove-Item -Force $part -ErrorAction SilentlyContinue
    Write-Host "Downloading $name..."
    Invoke-WebRequest -Uri $url -OutFile $part
    $actualHash = Get-Sha256 $part
    if ($actualHash -ne $expectedHash) {
        Remove-Item -Force $part -ErrorAction SilentlyContinue
        Write-Error "Checksum mismatch for $name (downloaded from $url); the partial file was deleted`nExpected: $expectedHash`nActual:   $actualHash"
        exit 1
    }
    Move-Item -Force $part $archive
}

function Assert-Extracted($name, $dest, [string[]]$Expect) {
    foreach ($rel in $Expect) {
        $path = Join-Path $dest $rel
        if (-not (Test-Path $path)) {
            throw "$name is not fully extracted: '$path' is missing. Delete '$dest' and re-run to extract it again."
        }
    }
}

function Extract-Archive($name, $topDir, [string[]]$Expect) {
    $archive = Join-Path $downloadDir "$name.tar.gz"
    $dest = Join-Path $srcDir $topDir
    if (Test-Path $dest) {
        Assert-Extracted $name $dest $Expect
        return
    }
    Write-Host "Extracting $name..."
    Invoke-MTTarExtract -Archive $archive -TarFlag "-xzf" `
        -DestWindows $srcDir -DestPosix (ConvertTo-MTMsysPath $srcDir)
    Assert-Extracted $name $dest $Expect
}

# build-image_codecs.ps1's Build-CMake: CRT per configuration (CMP0091 NEW).
# Returns the configure output so the caller can read turbo's SIMD line.
function Build-CMake($srcDir, $buildDir, $prefixDir, [string[]]$extraArgs) {
    $crtLib = if ($Configuration -eq 'Debug') { 'MultiThreadedDebug' } else { 'MultiThreaded' }
    $cmakeArgs = @(
        "-S", $srcDir,
        "-B", $buildDir,
        "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DCMAKE_INSTALL_PREFIX=$prefixDir",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=$crtLib"
    )
    if ($Platform -eq "ARM64") { $cmakeArgs += "-A", "ARM64" } else { $cmakeArgs += "-A", "x64" }
    $cmakeArgs += $extraArgs

    # See build-image_codecs.ps1: a benign stderr line must not abort the script.
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        Write-Host "Configuring: $srcDir -> $buildDir"
        $configureOut = & cmake @cmakeArgs 2>&1 | ForEach-Object { "$_" }
        $configureOut | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $srcDir" }

        Write-Host "Building: $buildDir"
        # Out-Host: a function's output stream is its return value, and only
        # the configure text is meant to be returned.
        & cmake --build $buildDir --config $Configuration -j $Jobs | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $buildDir" }

        Write-Host "Installing to $prefixDir"
        & cmake --install $buildDir --config $Configuration | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "CMake install failed for $buildDir" }
    } finally {
        $ErrorActionPreference = $savedEap
    }
    return ($configureOut -join "`n")
}

# ---- download, extract, build ----

$turboName = "libjpeg-turbo-$libjpegTurboVersion"
Download-Archive $turboName $libjpegTurboUrl $libjpegTurboHash
Extract-Archive $turboName $turboName @("CMakeLists.txt", "src\transupp.c", "src\transupp.h")
$turboSrc = Join-Path $srcDir $turboName

Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $prefixDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $buildDir, $prefixDir | Out-Null

$turboBuild = Join-Path $buildDir "libjpeg-turbo"
$turboArgs = @(
    "-DENABLE_SHARED=OFF", "-DENABLE_STATIC=ON", "-DWITH_TURBOJPEG=OFF",
    "-DWITH_TOOLS=OFF", "-DWITH_TESTS=OFF",
    "-DWITH_CRT_DLL=OFF",
    "-DWITH_SIMD=$simdRequested", "-DREQUIRE_SIMD=OFF",
    "-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_INSTALL_INCLUDEDIR=include"
)
if ($nasmExe) { $turboArgs += "-DCMAKE_ASM_NASM_COMPILER=$($nasmExe -replace '\\','/')" }
$configureLog = Build-CMake $turboSrc $turboBuild $prefixDir $turboArgs

# What the build ACTUALLY got, recorded in the stamp -- from the INSTALLED
# jconfig.h (`#define WITH_SIMD 1`), not from configure's output: when requested
# SIMD fails, turbo's simd_fail prints only "WARNING: SIMD extensions disabled:
# ..." and turns WITH_SIMD off; "SIMD extensions: None" appears only when
# WITH_SIMD=0 was asked for.
$jconfig = Join-Path $prefixDir 'include\jconfig.h'
$simdActual = if ((Test-Path $jconfig) -and (Select-String -Path $jconfig -Pattern '^#define WITH_SIMD 1' -Quiet)) { 1 } else { 0 }
if ($simdRequested -eq 1 -and $simdActual -eq 0) {
    $why = ($configureLog -split "`n" | Where-Object { $_ -match 'SIMD extensions disabled' } | Select-Object -First 1)
    if (-not $why) { $why = 'jconfig.h has no WITH_SIMD' }
    if ($isProd) { throw "error: libjpeg-turbo $Platform built WITHOUT SIMD in a final build ($why)" }
    Write-Warning "libjpeg-turbo $Platform built WITHOUT SIMD ($why)"
}

# transupp.c is jpegtran's, not the library's: a generated two-line shim builds
# it against the turbo build tree's generated headers (jconfigint.h), with the
# same CRT through Build-CMake. turbo's jpegtran target adds no define for it.
$turboSrcFwd = ($turboSrc -replace '\\','/')
$turboBuildFwd = ($turboBuild -replace '\\','/')
$shimDir = Join-Path $buildDir "transupp-shim"
New-Item -ItemType Directory -Force -Path $shimDir | Out-Null
@"
cmake_minimum_required(VERSION 3.10)
project(jpegtransupp C)
add_library(jpeg_transupp STATIC "$turboSrcFwd/src/transupp.c")
target_include_directories(jpeg_transupp PRIVATE "$turboBuildFwd" "$turboSrcFwd/src")
install(TARGETS jpeg_transupp ARCHIVE DESTINATION lib)
"@ | Out-File -FilePath (Join-Path $shimDir "CMakeLists.txt") -Encoding UTF8
$null = Build-CMake $shimDir (Join-Path $buildDir "transupp") $prefixDir @()
Copy-Item -Force (Join-Path $turboSrc "src\transupp.h") (Join-Path $prefixDir "include\")

foreach ($rel in @('lib\jpeg-static.lib', 'lib\jpeg_transupp.lib')) {
    if (-not (Test-Path (Join-Path $prefixDir $rel))) { throw "libjpeg-turbo: expected $prefixDir\$rel" }
}
foreach ($h in $jpegHeaders) {
    if (-not (Test-Path (Join-Path $prefixDir "include\$h"))) { throw "libjpeg-turbo: expected $prefixDir\include\$h" }
}

Remove-Item -Force $outLib -ErrorAction SilentlyContinue
& $libExe /NOLOGO "/OUT:$outLib" (Join-Path $prefixDir 'lib\jpeg-static.lib') (Join-Path $prefixDir 'lib\jpeg_transupp.lib')
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed to create $outLib" }

if (Test-Path $stagedInc) { Remove-Item -Recurse -Force $stagedInc }
New-Item -ItemType Directory -Force -Path $stagedInc | Out-Null
foreach ($h in $jpegHeaders) {
    Copy-Item -Force (Join-Path $prefixDir "include\$h") $stagedInc
}

# The stamp LAST, recording what the build actually got.
(Get-StampValue $simdActual) | Out-File -FilePath $stampFile -Encoding ASCII -NoNewline
Write-Host "libjpeg-turbo $libjpegTurboVersion built: $outLib" -ForegroundColor Green

} finally {
    # Runs on `exit` and on a terminating error alike.
    Complete-MTStore
}
