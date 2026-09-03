<#
.SYNOPSIS
    Build uSockets as a static archive and stage it as uSockets.lib.

.DESCRIPTION
    Replaces the TRACKED prebuilt at
    platform\Windows\libs\<Platform>\<Configuration>\uSockets.lib (and the
    equally tracked copies under other\lib\uSockets.vs\).

    This compiles other\lib\uSockets\src directly with cl/lib rather than
    driving other\lib\uSockets.vs\uSockets.sln, and that is deliberate. That
    solution is not usable as a build step:

      * it hardcodes absolute paths -- 'C:\develop\MTEngineSDL\other\lib\...'
        and, in the Win32/x64 configurations, 'C:\develop\libuv\include', which
        does not exist in this layout at all;
      * it sets no <RuntimeLibrary>, so it inherits CMake-free MSBuild defaults
        of /MDd and /MD while the engine and app build /MTd and /MT;
      * its outputs land in other\lib\uSockets.vs\<Platform>\<Configuration>,
        inside the engine checkout.

    Compiling the sources directly avoids all three. The source list and the
    preprocessor defines match what that project compiled, so the archive is
    equivalent to the prebuilt it replaces. uSockets guards each eventing
    backend and its TLS layer internally (libusockets.h:343-345 auto-selects
    LIBUS_USE_LIBUV when no backend is named, and LIBUS_NO_SSL empties the
    crypto files), so the non-Windows sources compile to nothing.

.PARAMETER OutLibDir
    Where to stage uSockets.lib. REQUIRED.
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
# cl and lib need the Windows SDK on INCLUDE/LIB, not just on PATH.
$null = Add-MTVCToolsToPath -Platform $Platform
$null = Import-MTVCEnvironment -Platform $Platform

foreach ($tool in @('cl', 'lib')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Missing required tool '$tool'. Run from 'Developer PowerShell for VS 2022'."
    }
}

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
# Per-unit store (L16). The unit builds in a directory keyed only by the
# capabilities IT reads, then its outputs are copied into the shared view. The
# body is wrapped in try/finally because a stamp hit and a capability-off stub
# both leave early and both still owe the view a copy.
$outDir = Use-MTStore -Unit 'usockets' -View $OutLibDir
try {
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$srcRoot = Join-Path $repoRoot 'other\lib\uSockets\src'
if (-not (Test-Path $srcRoot)) {
    throw "Missing uSockets source tree: $srcRoot (run: git submodule update --init --recursive)"
}
$uvInclude = Join-Path $repoRoot 'other\lib\libuv\include'
if (-not (Test-Path $uvInclude)) {
    throw "Missing libuv headers: $uvInclude (uSockets uses the libuv eventing backend on Windows)"
}

# Same set the retired uSockets.vcxproj compiled.
$sources = @(
    'bsd.c',
    'context.c',
    'crypto\openssl.c',
    'crypto\sni_tree.cpp',
    'eventing\asio.cpp',
    'eventing\epoll_kqueue.c',
    'eventing\gcd.c',
    'eventing\libuv.c',
    'io_uring\io_context.c',
    'io_uring\io_loop.c',
    'io_uring\io_socket.c',
    'loop.c',
    'quic.c',
    'socket.c',
    'udp.c'
)

$outLib = Join-Path $outDir 'uSockets.lib'
$stamp  = Join-Path $outDir 'uSockets.stamp'

$scriptSha = 'unknown'
try { $scriptSha = (Get-FileHash -Algorithm SHA256 -Path $PSCommandPath).Hash.ToLowerInvariant() } catch {}

# Hash every source, so an edit anywhere in the tree invalidates the archive.
$srcSha = 'unknown'
try {
    $joined = ($sources | ForEach-Object {
        $f = Join-Path $srcRoot $_
        if (Test-Path $f) { (Get-FileHash -Algorithm SHA256 -Path $f).Hash } else { 'missing' }
    }) -join '|'
    $bytes = [Text.Encoding]::UTF8.GetBytes($joined)
    $sha = [Security.Cryptography.SHA256]::Create()
    $srcSha = ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
} catch {}

$stampValue = "$srcSha`:$scriptSha`:$Configuration`:$Platform"
if ((Test-Path $outLib) -and (Test-Path $stamp)) {
    if ((Get-Content $stamp -Raw).Trim() -eq $stampValue) {
        Write-Host "uSockets is up to date: $outLib"
        exit 0
    }
}

# Object files land outside the engine checkout, next to the staged archive.
$objDir = Join-Path (Get-MTCapsWorkDir 'uSockets') "build-windows-$Platform-$Configuration"
if (Test-Path $objDir) { Remove-Item -Recurse -Force $objDir }
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$isDebug = ($Configuration -eq 'Debug')
# /MT and /MTd, matching the engine and app. The retired project used the
# MSBuild default (/MD, /MDd), which put a second CRT in the image.
$crtFlag = if ($isDebug) { '/MTd' } else { '/MT' }
$optFlag = if ($isDebug) { '/Od' } else { '/O2' }
$ndebug  = if ($isDebug) { '_DEBUG' } else { 'NDEBUG' }

$defines = @('LIBUS_NO_SSL', $ndebug, '_LIB', '_CRT_SECURE_NO_WARNINGS') |
    ForEach-Object { "/D$_" }

Write-Host "Compiling uSockets ($Platform $Configuration) into $objDir"

$objs = @()
foreach ($rel in $sources) {
    $src = Join-Path $srcRoot $rel
    if (-not (Test-Path $src)) {
        Write-Host "  skipping absent source: $rel"
        continue
    }
    # Flatten the object name so crypto\ and eventing\ cannot collide.
    $objName = ($rel -replace '[\\/]', '_') -replace '\.(c|cpp)$', '.obj'
    $obj = Join-Path $objDir $objName

    $clArgs = @('/nologo', '/c', $crtFlag, $optFlag) + $defines + @(
        "/I$srcRoot",
        "/I$uvInclude",
        $src,
        "/Fo$obj"
    )
    Invoke-MTNative -What "cl for $rel" -Action { & cl @clArgs | Out-Null }
    $objs += $obj
}

if ($objs.Count -eq 0) { throw "uSockets produced no object files" }

if (Test-Path $outLib) { Remove-Item -Force $outLib }
Write-Host "Packing into $outLib"
Invoke-MTNative -What "lib for uSockets" -Action { & lib /nologo "/OUT:$outLib" @objs | Out-Null }

Set-Content -NoNewline -Path $stamp -Value $stampValue
Write-Host "Done. Output: $outLib"

} finally {
    # Runs on `exit` and on a terminating error alike.
    Complete-MTStore
}
