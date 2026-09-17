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

# TLS 1.2 for Invoke-WebRequest, same line build-image_codecs.ps1 carries.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Require-Command([string]$name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        throw "Missing required tool '$name'. Run from 'Developer PowerShell for VS 2022' and ensure CMake is installed."
    }
}

# REQUIRED, not defaulted. The caller decides where archives go; the only
# fallback in the system lives in build-deps.ps1.
if (-not $OutLibDir) { throw "-OutLibDir is required. Run this through build-deps.ps1, which resolves it." }

# NOT $env:PROCESSOR_ARCHITECTURE, which describes the PROCESS and is inherited.
$Platform = Resolve-MTPlatform $Platform

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$workDir  = Get-MTCapsWorkDir 'opencv'
$downloadDir = Join-Path $workDir 'downloads'
$srcDir      = Join-Path $workDir 'src'
$extraDir    = Join-Path $workDir 'extra-modules'
$buildDir    = Join-Path $workDir "build-windows-$Platform-$Configuration"
$installDir  = Join-Path $workDir "install-windows-$Platform-$Configuration"
$helperDir   = Join-Path $workDir "helper-windows-$Platform-$Configuration"

$openCvVersion  = '5.0.0'
$openCvUrl      = "https://github.com/opencv/opencv/archive/refs/tags/$openCvVersion.tar.gz"
$openCvSha256   = 'b0528f5a1d379d59d4701cb28c36e22214cc51cf64594e5b56f2d3e6c0233095'
$contribUrl     = "https://github.com/opencv/opencv_contrib/archive/refs/tags/$openCvVersion.tar.gz"
$contribSha256  = 'c58f6344170c39abf187c56f3843b59cab1fd3e89cf19ba2ce25dc061659b27f'

# Per-unit store (L16). The body is wrapped in try/finally because a stamp hit
# and a capability-off stub both leave early and both still owe the view a copy.
$outDir = Use-MTStore -Unit 'opencv' -View $OutLibDir
try {

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$outLib = Join-Path $outDir 'mt_opencv.lib'
$stamp  = Join-Path $outDir 'mt_opencv.stamp'

$scriptSha = 'unknown'
try { $scriptSha = (Get-FileHash -Algorithm SHA256 -Path $PSCommandPath).Hash.ToLowerInvariant() } catch {}

# ---------------------------------------------------------------------------
# THE CAPABILITY GATE, and it is the only one in the tree that defaults OFF.
#
# Every other script here treats an absent flag as ON, matching
# Import-MTCapsEnvironment and the ${MT_ENABLE_X:-1} of the sh side, so that a
# bare engine clone builds everything. OpenCV is the exception the vocabulary
# records: turning it on downloads 154 MB and builds a second computer-vision
# library, and three of the five apps never call it. Hence `-ne '1'` rather
# than `-eq '0'`: absent means OFF.
#
# It comes BEFORE the download and before Require-Command cmake, both on
# purpose. Nothing below is needed to write a stub: only cl and lib.
# ---------------------------------------------------------------------------
if ($env:MT_ENABLE_OPENCV -ne '1') {
    Require-Command cl
    Require-Command lib
    New-MTCapsStubArchive -OutLib $outLib -Stamp $stamp -Symbol 'mt_opencv' `
                          -StampValue "disabled`:$scriptSha`:$Configuration`:$Platform"
    # The headers go too. Leaving a previous build's opencv2\ behind would let a
    # translation unit include <opencv2/core.hpp> in a build that cannot link
    # it, and fail at the link rather than at the #include.
    $headerDir = Join-Path $outDir 'opencv'
    if (Test-Path $headerDir) { Remove-Item -Recurse -Force $headerDir }
    exit 0
}

$nonfree = if ($env:MT_ENABLE_OPENCV_NONFREE -eq '1') { '1' } else { '0' }

# THE DNN MODE (MT_CAP_YOLO). Object detection is OpenCV's own dnn module, so
# this is the same configure with one more module plus the protobuf that ONNX
# parsing needs. WITH_ONNXRUNTIME stays OFF: it is the one dnn option that
# would make the configure download something.
#
# NEVER RUN ON WINDOWS. Written on a Mac; the macOS build of this same mode
# needed two workarounds (see below and the MLAS block), and a third that is
# macOS-only. If an ARM64 build on an x64 host tries to assemble aarch64 MLAS
# kernels, that is the same class of problem the macOS script solves with
# CMAKE_SYSTEM_NAME/CMAKE_SYSTEM_PROCESSOR -- record it rather than guessing.
$yolo = if ($env:MT_ENABLE_YOLO -eq '1') { '1' } else { '0' }
$dnnArgs = if ($yolo -eq '1') {
    @('-DWITH_PROTOBUF=ON', '-DBUILD_PROTOBUF=ON', '-DWITH_ONNXRUNTIME=OFF')
} else {
    @('-DWITH_PROTOBUF=OFF', '-DBUILD_PROTOBUF=OFF')
}

# The nonfree flag is IN the stamp: the two capabilities produce different
# archives from the same script and the same sources, so a stamp without it
# would call an OpenCV-only archive up to date for a build that asked for SURF.
$stampValue = "enabled`:$($openCvSha256.Substring(0,12))`:$($contribSha256.Substring(0,12))`:nonfree=$nonfree`:yolo=$yolo`:$scriptSha`:$Configuration`:$Platform`:$Compiler"
if ((Test-Path $outLib) -and (Test-Path $stamp)) {
    if ((Get-Content $stamp -Raw).Trim() -eq $stampValue) {
        Write-Host "OpenCV is up to date: $outLib"
        exit 0
    }
}

Require-Command cmake
Require-Command cl
Require-Command lib

New-Item -ItemType Directory -Force -Path $downloadDir, $srcDir | Out-Null

function Get-MTOpenCVArchive([string]$name, [string]$url, [string]$sha256) {
    $archive = Join-Path $downloadDir "$name.tar.gz"

    # DOWNLOAD TO .part, RENAME ONLY ONCE THE HASH VERIFIES.
    #
    # Invoke-WebRequest writes straight into -OutFile, so an interrupted
    # download -- Ctrl-C, a dropped connection, the OOM killer this machine is
    # prone to -- leaves a TRUNCATED file sitting at the real name. The guard
    # here used to be `if (-not (Test-Path $archive))`, which then treated that
    # stump as a finished download for ever after: every later build failed the
    # checksum and none of them retried. Measured on 2026-09-07, opencv_contrib
    # cut off at 20 MB of 60, reported as
    #     Checksum mismatch ... Actual: 2849b945f0ee...
    # which is indistinguishable, at a glance, from a tampered archive or a
    # moved upstream tag. It was neither, and no amount of re-running fixed it.
    #
    # So: a hash that verifies is kept, a hash that does not is DELETED and
    # fetched once more, and only a second failure throws -- by then the file
    # is freshly downloaded and a mismatch means what it says.
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        if (Test-Path $archive) {
            $actual = (Get-FileHash -Path $archive -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -eq $sha256) { break }
            if ($attempt -ge 2) {
                throw "Checksum mismatch for $archive`nExpected: $sha256`nActual:   $actual"
            }
            Write-Warning "$name is corrupt or stale (sha256 $actual) -- deleting and re-downloading"
            Remove-Item -Force $archive
        }
        Write-Host "Downloading $name..."
        $part = "$archive.part"
        if (Test-Path $part) { Remove-Item -Force $part }
        Invoke-WebRequest -Uri $url -OutFile $part -UseBasicParsing
        Move-Item -Force $part $archive
    }
    $dest = Join-Path $srcDir $name
    if (-not (Test-Path $dest)) {
        Write-Host "Extracting $name..."
        $posix = ($srcDir -replace '\\','/')
        Invoke-MTTarExtract -Archive $archive -TarFlag '-xzf' -DestWindows $srcDir -DestPosix $posix
    }
    return $dest
}

$openCvSrc = Get-MTOpenCVArchive "opencv-$openCvVersion" $openCvUrl $openCvSha256

# ---------------------------------------------------------------------------
# THE ONE PATCH THIS SCRIPT APPLIES TO UPSTREAM, and it is the dnn mode only.
#
# OPENCV 5.0.0's VENDORED MLAS DOES NOT BUILD ON WINDOWS/x86. This is the
# "NEVER RUN ON WINDOWS" note at the top of this file, now run. Two upstream
# bugs, both from the same root -- the mlas CMakeLists assumes a GCC driver:
#
#   1. It force-includes <cstring> in the GCC driver's two-token spelling,
#          "$<$<COMPILE_LANGUAGE:CXX>:-include>"
#          "$<$<COMPILE_LANGUAGE:CXX>:cstring>"
#      behind `CMAKE_CXX_COMPILER_ID MATCHES "Clang"`. clang-cl REPORTS Clang
#      but drives like MSVC, so it takes that branch and cannot read it:
#      measured, `clang-cl -include cstring` gives "unknown argument ignored in
#      clang-cl: '-include'" and leaves `cstring` as a positional INPUT --
#      "no such file or directory: 'cstring'". (/FIcstring is the spelling that
#      works, if this ever needs reviving.) Upstream guards its OTHER
#      force-include, platform.cpp/unistd.h, with `AND NOT WIN32`; not this one.
#
#   2. Past that, the x86_64 SGEMM kernels it compiles are GNU-syntax .S files
#      carrying GCC flags (-msse2/-mavx). The MSVC driver cannot assemble them
#      and the vendored tree ships NO MASM equivalent for x86 or x86_64 -- only
#      arm64 and arm64ec have .asm. MSBuild produced no object and the failure
#      surfaced far downstream, at opencv_dnn's llvm-lib step:
#      "SgemmKernelSse2.obj: no such file or directory".
#
# (2) has no fix short of writing the MASM kernels, so this takes the path
# UPSTREAM ITSELF PROVIDES for that case: the `_MLAS_REQUIRES_ASM` branch a few
# lines below already skips MLAS with a recorded reason when no assembler is
# available, and the DNN module falls back to its built-in SGEMM. The patch
# routes MSVC/x86 into that same branch. dnn keeps working; it loses only
# MLAS's GEMM acceleration, which on this platform was never actually built.
#
# The rewrite is textual and idempotent, the version is pinned by SHA256 above,
# and $scriptSha is in the stamp, so editing this block re-runs the build.
#
# The sources are re-extracted only when missing, so patching HERE (not in
# Get-MTOpenCVArchive) is what makes it hold for an already-extracted tree.
# ---------------------------------------------------------------------------
if ($yolo -eq '1') {
    $mlasCMake = Join-Path $openCvSrc '3rdparty\mlas\CMakeLists.txt'
    if (-not (Test-Path $mlasCMake)) { throw "OpenCV $openCvVersion has no 3rdparty\mlas\CMakeLists.txt to patch" }

    $mlasText = [System.IO.File]::ReadAllText($mlasCMake)
    $marker  = 'MTEngineSDL: MSVC driver cannot build the x86 MLAS kernels'
    $anchor  = "check_language(ASM)`nif(CMAKE_ASM_COMPILER)"
    $patched = @"
check_language(ASM)
# $marker -- the x86/x86_64
# kernels are GNU-syntax .S with GCC flags and the tree ships no MASM
# equivalent, so route MSVC/x86 into the no-ASM skip below and let DNN use its
# built-in SGEMM.
if(MSVC AND (MLAS_X86_64 OR MLAS_X86))
  set(OPENCV_DNN_MLAS_SKIP_REASON
    "GNU-syntax .S kernels unsupported by the MSVC driver on `${CMAKE_SYSTEM_PROCESSOR}"
    CACHE INTERNAL "" FORCE)
  message(STATUS "MLAS: disabled on MSVC/`${CMAKE_SYSTEM_PROCESSOR} (DNN will use its built-in SGEMM)")
  return()
endif()
if(CMAKE_ASM_COMPILER)
"@ -replace "`r`n", "`n"

    if ($mlasText.Contains($marker)) {
        Write-Host "MLAS already patched out for the MSVC driver"
    } elseif ($mlasText.Contains($anchor)) {
        [System.IO.File]::WriteAllText($mlasCMake, $mlasText.Replace($anchor, $patched))
        Write-Host "Patched MLAS to skip on MSVC/x86 (DNN falls back to its built-in SGEMM)"
    } else {
        # Anchor absent: upstream changed the block this patch targets. Failing
        # here beats building a tree the patch silently missed.
        throw "MLAS ASM-probe block not found in $mlasCMake -- upstream changed; re-check the patch"
    }
}

$modules = 'core,imgproc,geometry,features'
if ($yolo -eq '1') { $modules = "$modules,dnn" }
$extraArgs = @()
if ($nonfree -eq '1') {
    $contribSrc = Get-MTOpenCVArchive "opencv_contrib-$openCvVersion" $contribUrl $contribSha256

    # ONLY xfeatures2d, copied into a directory of its own.
    #
    # CMake scans EVERY directory under OPENCV_EXTRA_MODULES_PATH and runs each
    # module's own dependency detection, including for modules that BUILD_LIST
    # excludes. Measured on macOS: pointing this at the whole contrib modules
    # tree made the configure download ade/archive/v0.1.2e.zip (gapi wants it)
    # and detect system freetype, harfbuzz, HDF5, Eigen, OGRE and VTK.
    if (Test-Path $extraDir) { Remove-Item -Recurse -Force $extraDir }
    New-Item -ItemType Directory -Force -Path $extraDir | Out-Null
    Copy-Item -Recurse -Force (Join-Path $contribSrc 'modules\xfeatures2d') $extraDir

    $modules = "$modules,xfeatures2d"
    $extraArgs = @(
        # Slashed for the same reason as the paths at the configure below.
        "-DOPENCV_EXTRA_MODULES_PATH=$($extraDir -replace '\\','/')"
        '-DOPENCV_ENABLE_NONFREE=ON'
        # xfeatures2d otherwise fetches the BoostDesc and VGG descriptor data
        # at configure time. Without them those two descriptors report "not
        # available"; SURF, FREAK, BRIEF and DAISY are unaffected.
        '-DOPENCV_SKIP_FEATURES2D_DOWNLOADING=ON'
    )
}

# ---------------------------------------------------------------------------
# The engine's OWN vendored zlib.
#
# OpenCV core needs zlib. It must not bring a second one: BUILD_ZLIB=OFF plus
# these two paths point its configure at the same sources the engine compiles
# into MTEngineSDL.lib. The helper IS merged into the bundle below, which looks
# like duplication and is not -- see the macOS script's comment: the archives
# are consumed in a fixed order and a linker pulls a member only for a symbol
# that is still undefined.
#
# /MT or /MTd to match Get-MTCrtCMakeArgs, or the objects carry a different CRT
# from everything else in the image.
# ---------------------------------------------------------------------------
# HOISTED out of the zlib-helper block below, which only runs when the helper
# is missing: the MLAS stub further down needs the same flag on every run, and
# it must match Get-MTCrtCMakeArgs or the object carries a different CRT from
# the rest of the image.
$crtFlag = if ($Configuration -eq 'Debug') { '/MTd' } else { '/MT' }

$zlibSrc = Join-Path $repoRoot 'src\Engine\Libs\zlib'
# The engine's zlib carries a deliberate unconditional '#include <unistd.h>' in
# gzlib/gzread/gzwrite (POSIX prototypes); MSVC resolves it via the engine's shim
# header, which must therefore be on the include path here too or the helper dies
# at C1083 on gzlib.c. build-image_codecs.ps1 compiles these same sources and
# carries the same pairing -- keep the two in step.
$zlibWinShim = Join-Path $repoRoot 'platform\Windows\src.Windows'
$zlibLib = Join-Path $helperDir 'lib\zlib.lib'
if (-not (Test-Path $zlibLib)) {
    $zobj = Join-Path $workDir "zobj-$Platform-$Configuration"
    New-Item -ItemType Directory -Force -Path $zobj, (Join-Path $helperDir 'lib'), (Join-Path $helperDir 'include') | Out-Null

    $objs = @()
    foreach ($c in (Get-ChildItem -Path $zlibSrc -Filter '*.c')) {
        $obj = Join-Path $zobj ($c.BaseName + '.obj')
        # Captured rather than discarded: cl reports C1083 and friends on stdout,
        # and swallowing it left the throw below as the only evidence.
        $clOut = & cl /nologo /c /O2 $crtFlag "/I$zlibSrc" "/I$zlibWinShim" $c.FullName "/Fo$obj" 2>&1
        if ($LASTEXITCODE -ne 0) {
            $clOut | ForEach-Object { Write-Host $_ }
            throw "cl failed compiling $($c.Name) for the OpenCV zlib helper"
        }
        $objs += $obj
    }
    & lib /nologo "/OUT:$zlibLib" @objs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "lib failed packing the OpenCV zlib helper" }
    Copy-Item -Force (Join-Path $zlibSrc 'zlib.h'), (Join-Path $zlibSrc 'zconf.h') (Join-Path $helperDir 'include')
}

# FORWARD SLASHES FOR EVERY PATH HANDED TO CMAKE. A backslash is an escape
# character in CMake code, and OpenCV re-parses ZLIB_LIBRARY as code: it reaches
# target_link_libraries by way of ocv_target_link_libraries, where the '\U' of
# '...\Users\...' is rejected as an invalid character escape and the configure
# dies in modules/core. The other three survive as plain cache strings, but they
# are the same class of value and CMake takes '/' on Windows everywhere.
# build-image_codecs.ps1 slashes its paths for this reason -- keep the two in step.
$zlibLibCM     = $zlibLib -replace '\\','/'
$zlibIncCM     = (Join-Path $helperDir 'include') -replace '\\','/'
$installDirCM  = $installDir -replace '\\','/'
$downloadCmCM  = (Join-Path $downloadDir 'cmake') -replace '\\','/'

$cmakeToolsetName = if ($Compiler -eq 'Clang') { 'ClangCL' } else { '' }
$cmakeToolset = if ($cmakeToolsetName) { @('-T', $cmakeToolsetName) } else { @() }
$generator = Get-MTVSGenerator

# ---------------------------------------------------------------------------
# TELL OPENCV THE TARGET ARCHITECTURE. It cannot work it out here, and when it
# guesses wrong the build links only by accident.
#
# OpenCVDetectCXXCompiler.cmake keys X86_64/AARCH64 off CMAKE_SYSTEM_PROCESSOR.
# With a Visual Studio generator CMake does NOT derive that from `-A $Platform`;
# it copies the HOST processor. And the host value is the architecture of the
# CMAKE BINARY, not of the machine: measured on this ARM64 box, the x64 CMake in
# C:\Program Files\CMake runs emulated and reports CMAKE_HOST_SYSTEM_PROCESSOR
# = AMD64. So an `-A ARM64` configure produced X86_64=1, CPU_BASELINE=SSE3 and a
# CPU_DISPATCH of SSE4_1..AVX512_SKX, and NEON was in neither -- while clang-cl,
# actually targeting ARM64, defines __ARM_NEON, so OpenCV's own CV_NEON is 1 and
# modules/dnn/.../convolution.cpp emits calls to cv::dnn::opt_NEON::convBlock_F32
# and convBlockMR1_F32. conv_block.neon.cpp was generated and never compiled: two
# undefined symbols at the app's link, from mt_opencv.lib(convolution.obj).
#
# The mismatch cuts both ways -- on this same host a `-A x64` build is right only
# because the emulated CMake happens to say AMD64, and would be wrong with a
# native ARM64 CMake. So do not depend on the host at all: OPENCV_SKIP_SYSTEM_
# PROCESSOR_DETECTION is upstream's documented hand-off for exactly this, and the
# arch variable then comes from $Platform, which is the truth.
#
# Not fixed here, deliberately: 3rdparty/mlas keys off CMAKE_SYSTEM_PROCESSOR
# too, so on ARM64 it still sets MLAS_X86_64 and the skip patch above still
# fires. That leaves MLAS off on ARM64 as it already was -- a missed
# optimisation, not a link error, and turning it on would build the arm64 .asm
# kernels for the first time. See the Windows handover notes.
# ---------------------------------------------------------------------------
$archArgs = switch ($Platform) {
    'ARM64' { @('-DOPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION=ON', '-DAARCH64=ON') }
    'x64'   { @('-DOPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION=ON', '-DX86_64=ON') }
    'Win32' { @('-DOPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION=ON', '-DX86=ON') }
    default { throw "build-opencv.ps1: no OpenCV architecture mapping for platform '$Platform'" }
}

# CPU_BASELINE and CPU_DISPATCH are `CACHE STRING` defaults: OpenCV sets them
# only when they are not already cached, so a tree configured under the wrong
# arch keeps its SSE3 baseline forever and the flag above changes nothing. The
# generator, platform and toolset all still match, so Reset-MTStaleCMakeCache
# has no reason to fire either. Key the reset on the baseline itself.
$archMarker = if ($Platform -eq 'ARM64') { 'NEON' } else { 'SSE' }
$cacheFile  = Join-Path $buildDir 'CMakeCache.txt'
if (Test-Path $cacheFile) {
    $cachedBaseline = (Select-String -Path $cacheFile -Pattern '^CPU_BASELINE:STRING=(.*)$' |
                       Select-Object -First 1).Matches.Groups[1].Value
    if ($cachedBaseline -and $cachedBaseline -notmatch $archMarker) {
        Write-Host "OpenCV cache has CPU_BASELINE=$cachedBaseline, wrong for $Platform -- reconfiguring from scratch"
        Remove-Item -Recurse -Force $buildDir
    }
}

$null = Reset-MTStaleCMakeCache -BuildDir $buildDir -Platform $Platform -Generator $generator `
    -Toolset $cmakeToolsetName -SourceDir $openCvSrc -Label 'OpenCV'

# RELAX $ErrorActionPreference AROUND THE NATIVE CALLS: PowerShell 5.1 wraps any
# stderr line from a native command into a terminating NativeCommandError under
# 'Stop', regardless of exit code, and CMake writes deprecation notices to
# stderr on a successful configure. $LASTEXITCODE is the real signal.
$savedEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    Write-Host "Configuring OpenCV $openCvVersion ($Platform, $Configuration, nonfree=$nonfree)"
    cmake -S $openCvSrc -B $buildDir -G $generator -A $Platform @cmakeToolset `
        (Get-MTCrtCMakeArgs $Configuration) `
        "-DCMAKE_INSTALL_PREFIX=$installDirCM" `
        -DBUILD_SHARED_LIBS=OFF `
        "-DBUILD_LIST=$modules" `
        @extraArgs `
        -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF `
        -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_OBJC=OFF `
        -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF -DBUILD_opencv_js=OFF `
        -DOPENCV_GENERATE_PKGCONFIG=OFF -DINSTALL_C_EXAMPLES=OFF `
        -DWITH_FFMPEG=OFF -DWITH_AVFOUNDATION=OFF -DWITH_MSMF=OFF -DWITH_DSHOW=OFF `
        -DWITH_V4L=OFF -DWITH_GTK=OFF -DWITH_QT=OFF -DWITH_1394=OFF -DWITH_GSTREAMER=OFF `
        -DWITH_OPENCL=OFF -DWITH_IPP=OFF -DWITH_TBB=OFF -DWITH_OPENMP=OFF `
        -DWITH_EIGEN=OFF -DWITH_LAPACK=OFF `
        @dnnArgs `
        -DWITH_ITT=OFF -DWITH_CUDA=OFF -DWITH_VA=OFF -DWITH_OPENEXR=OFF `
        -DWITH_JPEG=OFF -DWITH_PNG=OFF -DWITH_TIFF=OFF -DWITH_WEBP=OFF `
        -DWITH_OPENJPEG=OFF -DWITH_JASPER=OFF -DWITH_IMGCODEC_HDR=OFF `
        -DWITH_IMGCODEC_SUNRASTER=OFF -DWITH_IMGCODEC_PXM=OFF -DWITH_IMGCODEC_PFM=OFF `
        -DWITH_KLEIDICV=OFF -DWITH_CAROTENE=OFF -DWITH_FLATBUFFERS=OFF `
        -DWITH_UNIFONT=OFF `
        -DBUILD_ZLIB=OFF `
        "-DZLIB_INCLUDE_DIR=$zlibIncCM" `
        "-DZLIB_LIBRARY=$zlibLibCM" `
        "-DOPENCV_DOWNLOAD_PATH=$downloadCmCM" `
        @archArgs `
        -DCV_ENABLE_INTRINSICS=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for OpenCV" }

    Write-Host "Building OpenCV ($Configuration)"
    cmake --build $buildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for OpenCV" }

    # WIPE THE INSTALL TREE FIRST: it is not keyed by the capability set, and
    # `cmake --install` adds rather than prunes, so a previous run with dnn on
    # would leave opencv2\dnn.hpp here for a build that cannot link it.
    if (Test-Path $installDir) { Remove-Item -Recurse -Force $installDir }
    cmake --install $buildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "CMake install failed for OpenCV" }
} finally {
    $ErrorActionPreference = $savedEap
}

# flann is in this list although nothing asked for it: modules/features declares
# `OPTIONAL opencv_flann`, and BUILD_LIST resolution enables an optional
# dependency that is present.
#
# The installed name carries the version and, in Debug, a 'd': opencv_core500.lib
# / opencv_core500d.lib. Find-MTConfigLib exists for exactly this, but the
# staticlib subdirectory is OpenCV's own layout, so the search is explicit.
$libSuffix = if ($Configuration -eq 'Debug') { '500d' } else { '500' }
$moduleNames = @('core','imgproc','geometry','flann','features')
if ($nonfree -eq '1') { $moduleNames += 'xfeatures2d' }
if ($yolo -eq '1') { $moduleNames += 'dnn' }

$libs = @()
foreach ($m in $moduleNames) {
    $found = Get-ChildItem -Path $installDir -Recurse -Filter "opencv_$m$libSuffix.lib" -ErrorAction SilentlyContinue |
             Select-Object -First 1
    if (-not $found) {
        throw "Expected opencv_$m$libSuffix.lib under $installDir after the OpenCV build"
    }
    $libs += $found.FullName
}
if ($yolo -eq '1') {
    # protobuf installs under the 3rdparty prefix, which on Windows is
    # `staticlib`, NOT lib/opencv5/3rdparty as on macOS, and the name carries
    # OPENCV_DEBUG_POSTFIX in Debug. Searched rather than spelled, the same way
    # the module libs above are found.
    $pb = Get-ChildItem -Path $installDir -Recurse -Filter 'libprotobuf*.lib' -ErrorAction SilentlyContinue |
          Select-Object -First 1
    if (-not $pb) { throw "Expected libprotobuf*.lib under $installDir after the OpenCV dnn build" }
    $libs += $pb.FullName

    # MLAS objects, IF they are not already members of opencv_dnn*.lib.
    $mlas = Get-ChildItem -Path (Join-Path $buildDir '3rdparty\mlas') -Recurse -Filter '*.obj' -ErrorAction SilentlyContinue
    if ($mlas) { $libs += $mlas.FullName }

    # ONE SYMBOL OPENCV 5.0.0 DECLARES, CALLS AND NEVER DEFINES -- see the
    # macOS script for the evidence. The CRT flag is $crtFlag, NOT a hardcoded
    # /MT: it must match Get-MTCrtCMakeArgs or the object carries a different
    # runtime from everything else in the image.
    $stubSrc = Join-Path $workDir 'mlas_hgemm_stub.cpp'
    Set-Content -Path $stubSrc -Value @'
enum CBLAS_TRANSPOSE { CblasNoTrans = 111, CblasTrans = 112, CblasConjTrans = 113 };
bool MlasHGemmSupported(CBLAS_TRANSPOSE, CBLAS_TRANSPOSE);
bool MlasHGemmSupported(CBLAS_TRANSPOSE, CBLAS_TRANSPOSE) { return false; }
'@
    $stubObj = Join-Path $workDir 'mlas_hgemm_stub.obj'
    & cl /nologo /c /O2 $crtFlag $stubSrc "/Fo$stubObj" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cl failed compiling the MlasHGemmSupported stub" }
    $libs += $stubObj
}

$libs += $zlibLib

Write-Host "Packing into $outLib"
if (Test-Path $outLib) { Remove-Item -Force $outLib }
& lib /nologo "/OUT:$outLib" @libs
if ($LASTEXITCODE -ne 0) { throw "lib failed packing $outLib" }

# OpenCV 5 installs headers under include\opencv5\, not the include\opencv4\
# that every OpenCV 4 recipe on the internet names.
$headerOut = Join-Path $outDir 'opencv\include'
if (Test-Path $headerOut) { Remove-Item -Recurse -Force $headerOut }
New-Item -ItemType Directory -Force -Path $headerOut | Out-Null
$headerSrc = Join-Path $installDir 'include\opencv5\opencv2'
if (-not (Test-Path $headerSrc)) {
    # Some generators install straight into include\opencv2 on Windows.
    $headerSrc = Join-Path $installDir 'include\opencv2'
}
if (-not (Test-Path $headerSrc)) { throw "No installed opencv2 headers under $installDir" }
Copy-Item -Recurse -Force $headerSrc $headerOut

# Stamp the ENABLED archive too, not just the stub. Without this the stub stamp
# from a previous capability-off build survives beside the real mt_opencv.lib,
# and the next off build finds stamp and archive both present, matches, and
# keeps the REAL archive while believing it wrote a stub.
Set-Content -NoNewline -Path $stamp -Value $stampValue

Write-Host "Done. Output: $outLib (nonfree=$nonfree)"

} finally {
    # Runs on `exit` and on a terminating error alike.
    Complete-MTStore
}
