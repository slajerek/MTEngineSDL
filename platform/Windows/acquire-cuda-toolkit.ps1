<#
.SYNOPSIS
    Acquire the pinned CUDA toolkit from NVIDIA's redistributable archives and
    assemble it into a CUDA_PATH-shaped tree, outside every checkout.
.DESCRIPTION
    WHY NOT THE INSTALLER. NVIDIA's Windows CUDA installer is x86_64-only and
    refuses an ARM64 host outright:

        "CUDA toolkit must be installed on an amd64 computer"

    which is what this repository's Windows build machine is (Windows 11 ARM64
    under Parallels). The toolkit is ALSO published component by component at
    https://developer.download.nvidia.com/compute/cuda/redist/ as plain zips
    with a per-version manifest carrying a sha256 for each -- no installer, no
    OS check, intended for exactly this kind of packaging. An x64 nvcc.exe runs
    under ARM64 emulation; measured 2026-09-07, it drove HostARM64\x64 cl.exe
    to PTX and to a multi-arch object here. Building needs no GPU.

    WHAT IS FETCHED, AND WHY EACH. The eight components below are the closed
    set the llama.cpp CUDA backend and its plugin need:
      cuda_nvcc          the compiler, plus crt/ and nvvm/ (not separate on Windows)
      cuda_cudart        runtime headers, cudart.lib, and cudart64_12.dll TO SHIP
      libcublas          cublas/cublasLt .lib and the two .dll TO SHIP
      cuda_cccl          thrust/cub headers ggml-cuda includes
      cuda_nvrtc         runtime compilation, which cublasLt 12 links against
      libnvjitlink       likewise, a cublas 12 dependency
      cuda_profiler_api  cuda_profiler_api.h, included by ggml
      cuda_cuobjdump     for build-llama_cpp_cuda.ps1 to PROVE which GPU
                         architectures made it into the binary
    Every archive is pinned by the sha256 from redistrib_<version>.json. Two
    were cross-checked against files downloaded independently before the table
    was trusted.

    WHERE IT GOES. Get-MTCudaToolkitDir, i.e. <cache root>\toolchains\cuda-
    <version>\ -- the version is pinned in mt-build-common.ps1 so the directory
    the build reads and the archives fetched here cannot disagree. Downloads
    are kept beside the other dependency work under _deps\work\cuda-toolkit\.
    Nothing touches a checkout.

    IDEMPOTENT. A stamp records the version, every sha256 and this script's own
    hash; a matching stamp with bin\nvcc.exe present is a no-op. Anything else
    reassembles from scratch -- a half-merged tree is never patched in place.

    Run it once. Resolve-MTCudaPath then finds the result for every later
    build, and a developer with a real installation (CUDA_PATH set) is left
    alone: their toolkit wins.
.PARAMETER Clean
    Remove the assembled toolkit and the cached downloads, then stop.
#>
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\mt-build-common.ps1"

# TLS 1.2 for Invoke-WebRequest, the same line build-opencv.ps1 carries.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$version     = $script:MT_CUDA_TOOLKIT_VERSION
$toolkitDir  = Get-MTCudaToolkitDir
$workDir     = Join-Path (Get-MTBuildRoot) '_deps\work\cuda-toolkit'
$downloadDir = Join-Path $workDir 'downloads'
$extractDir  = Join-Path $workDir 'extract'
$stampFile   = Join-Path $toolkitDir '.mt-cuda-toolkit.stamp'
$baseUrl     = 'https://developer.download.nvidia.com/compute/cuda/redist'

if ($Clean) {
    foreach ($d in @($toolkitDir, $workDir)) {
        if (Test-Path $d) { Write-Host "Removing $d"; Remove-Item -Recurse -Force $d }
    }
    Write-Host "Clean: done."
    exit 0
}

# The manifest, transcribed. name | relative path under $baseUrl | sha256 | bytes.
$components = @(
    @{ name='cuda_nvcc';         path='cuda_nvcc/windows-x86_64/cuda_nvcc-windows-x86_64-12.9.86-archive.zip';                 sha256='227b109663b5e57d2718bcabb24a4ba0d9d4e52d958e327dc476f7c28691be85'; size=126917884 },
    @{ name='cuda_cudart';       path='cuda_cudart/windows-x86_64/cuda_cudart-windows-x86_64-12.9.79-archive.zip';             sha256='179e9c43b0735ffe67207b3da556eb5a0c50f3047961882b7657d3b822d34ef8'; size=3521238 },
    @{ name='cuda_cccl';         path='cuda_cccl/windows-x86_64/cuda_cccl-windows-x86_64-12.9.27-archive.zip';                 sha256='17aaa7c6b8f94a417d8f3261780b7e34b9cbdfab7513bce86768623b06aa28b5'; size=3127658 },
    @{ name='libcublas';         path='libcublas/windows-x86_64/libcublas-windows-x86_64-12.9.2.10-archive.zip';               sha256='62782c354e1536118caab8fe5fd295ac5fd9076e4814aba0bdaf032fe2b833ae'; size=549731131 },
    @{ name='cuda_nvrtc';        path='cuda_nvrtc/windows-x86_64/cuda_nvrtc-windows-x86_64-12.9.86-archive.zip';               sha256='1aa0644fa53c8ca34cdc73db17bcc73530557bdd3f582c7bfdbd7916c8b48f65'; size=314608748 },
    @{ name='libnvjitlink';      path='libnvjitlink/windows-x86_64/libnvjitlink-windows-x86_64-12.9.86-archive.zip';           sha256='ee7175da9628d47ccc92dce6d28d57ca77633e79079a2aee90e2a645edcd1384'; size=265666466 },
    @{ name='cuda_profiler_api'; path='cuda_profiler_api/windows-x86_64/cuda_profiler_api-windows-x86_64-12.9.79-archive.zip'; sha256='b05519f8ce4f02167bfc859358f62eb771d89ac2af3a6952a82317f2af4bc5bd'; size=20352 },
    @{ name='cuda_cuobjdump';    path='cuda_cuobjdump/windows-x86_64/cuda_cuobjdump-windows-x86_64-12.9.82-archive.zip';       sha256='1eda43a76a2eac25fce5bdb4b68673b5bda737d54cca5513148c36362ab7c811'; size=6219088 }
)

# What a finished tree must contain. Checked after the merge, and it is what
# lets the stamp mean "usable" rather than "the copy loop ran".
$requiredFiles = @(
    'bin\nvcc.exe', 'bin\ptxas.exe', 'bin\fatbinary.exe', 'bin\cuobjdump.exe',
    'include\cuda_runtime.h', 'include\cublas_v2.h', 'include\cub\cub.cuh',
    'lib\x64\cudart.lib', 'lib\x64\cublas.lib', 'lib\x64\cublasLt.lib', 'lib\x64\cuda.lib',
    'bin\cudart64_12.dll', 'bin\cublas64_12.dll', 'bin\cublasLt64_12.dll'
)

$scriptSha = 'unknown'
try { $scriptSha = (Get-FileHash -Algorithm SHA256 -Path $PSCommandPath).Hash.ToLowerInvariant() } catch {}
$stampValue = "cuda-toolkit`:$version`:" + (($components | ForEach-Object { $_.sha256.Substring(0, 12) }) -join ':') + "`:$scriptSha"

if ((Test-Path $stampFile) -and (Test-Path (Join-Path $toolkitDir 'bin\nvcc.exe'))) {
    if ((Get-Content $stampFile -Raw).Trim() -eq $stampValue) {
        Write-Host "CUDA toolkit $version is up to date: $toolkitDir"
        Write-Host "CUDA_PATH=$toolkitDir"
        exit 0
    }
}

New-Item -ItemType Directory -Force -Path $downloadDir, $extractDir | Out-Null

# ---------------------------------------------------------------------------
# DOWNLOAD TO .part, RENAME ONLY ONCE THE HASH VERIFIES -- the same discipline
# build-opencv.ps1 gained on 2026-09-07 after a truncated opencv_contrib
# archive wedged every later build behind a checksum mismatch that looked like
# tampering. A stump can never occupy the real name; an existing file with the
# wrong hash is deleted and fetched once more; only a SECOND mismatch throws.
#
# curl.exe over Invoke-WebRequest for the transfer: it ships with Windows 10+,
# streams a 550 MB archive without buffering it, and its progress display does
# not cost a PowerShell 5.1 download the order of magnitude that
# Write-Progress does. Invoke-WebRequest remains as the fallback.
# ---------------------------------------------------------------------------
function Get-MTRedistArchive([hashtable]$c) {
    $archive = Join-Path $downloadDir (Split-Path $c.path -Leaf)
    $url = "$baseUrl/$($c.path)"
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        if (Test-Path $archive) {
            $actual = (Get-FileHash -Path $archive -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -eq $c.sha256) { return $archive }
            if ($attempt -ge 2) {
                throw "Checksum mismatch for $archive`nExpected: $($c.sha256)`nActual:   $actual"
            }
            Write-Warning "$($c.name) is corrupt or stale (sha256 $actual) -- deleting and re-downloading"
            Remove-Item -Force $archive
        }
        $mb = [math]::Round($c.size / 1MB, 1)
        Write-Host "Downloading $($c.name) ($mb MB)..."
        $part = "$archive.part"
        if (Test-Path $part) { Remove-Item -Force $part }
        $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
        if ($curl) {
            & $curl.Source -fsSL --retry 3 -o $part $url
            if ($LASTEXITCODE -ne 0) { throw "curl failed ($LASTEXITCODE) fetching $url" }
        } else {
            $savedPP = $ProgressPreference
            $ProgressPreference = 'SilentlyContinue'
            try { Invoke-WebRequest -Uri $url -OutFile $part -UseBasicParsing } finally { $ProgressPreference = $savedPP }
        }
        Move-Item -Force $part $archive
    }
    return $archive
}

# Windows' own tar.exe -- bsdtar, in System32 since Windows 10 1803 -- reads
# zip natively, is markedly faster than Expand-Archive on a 550 MB archive and
# has no path-length trouble. BY ABSOLUTE PATH, not Get-Command: a PowerShell
# started from Git Bash inherits an MSYS PATH on which `tar.exe` resolves to
# GNU tar, which reads "C:\..." as a remote host ("Cannot connect to C:
# resolve failed", exit 128). Measured on the first run of this script; the
# fallback caught it, slowly. Expand-Archive remains for a machine without it.
function Expand-MTRedistArchive([string]$archive, [string]$dest) {
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    $bsdtar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (Test-Path $bsdtar) {
        & $bsdtar -xf $archive -C $dest
        if ($LASTEXITCODE -eq 0) { return }
        Write-Warning "$bsdtar failed ($LASTEXITCODE) on $archive; falling back to Expand-Archive"
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $dest -Force
}

# The archives are fetched and verified BEFORE anything is assembled, so a
# failure half-way leaves no partial toolkit behind.
$archives = @{}
foreach ($c in $components) { $archives[$c.name] = Get-MTRedistArchive $c }

# A fresh tree every time this runs to completion: merging into a leftover
# from another version is how a toolkit ends up with two cudart.libs.
if (Test-Path $toolkitDir) {
    Write-Host "Removing previous toolkit tree: $toolkitDir"
    Remove-Item -Recurse -Force $toolkitDir
}
New-Item -ItemType Directory -Force -Path $toolkitDir, (Join-Path $toolkitDir 'licenses') | Out-Null

# ---------------------------------------------------------------------------
# MERGE. Each archive unpacks to a single directory,
#     <name>-windows-x86_64-<ver>-archive\{bin,include,lib,nvvm,...,LICENSE}
# and those children are laid over one another to make the tree nvcc expects:
# bin\ beside include\ beside lib\x64\, with nvvm\ under the root (nvcc.profile
# locates everything relative to its own parent). An explicit file walk rather
# than Copy-Item -Recurse on the containers: PowerShell's rule for copying a
# directory into an existing directory of the same name is exactly the kind of
# thing that puts cudart.lib under lib\x64\x64\ on one machine and not another.
# ---------------------------------------------------------------------------
foreach ($c in $components) {
    $dest = Join-Path $extractDir $c.name
    Write-Host "Extracting $($c.name)..."
    Expand-MTRedistArchive $archives[$c.name] $dest

    $roots = @(Get-ChildItem -LiteralPath $dest -Directory)
    if ($roots.Count -ne 1) { throw "$($c.name): expected one top-level directory in the archive, found $($roots.Count)" }
    $root = $roots[0].FullName

    $licence = Join-Path $root 'LICENSE'
    if (Test-Path $licence) { Copy-Item -Force $licence (Join-Path $toolkitDir "licenses\$($c.name)-LICENSE.txt") }

    $count = 0
    foreach ($f in (Get-ChildItem -LiteralPath $root -Recurse -File)) {
        $rel = $f.FullName.Substring($root.Length).TrimStart('\')
        if ($rel -eq 'LICENSE') { continue }
        $target = Join-Path $toolkitDir $rel
        $targetDir = Split-Path -Parent $target
        if (-not (Test-Path -LiteralPath $targetDir)) { New-Item -ItemType Directory -Force -Path $targetDir | Out-Null }
        Copy-Item -LiteralPath $f.FullName -Destination $target -Force
        $count++
    }
    Write-Host "  merged $count files"
}

# The extracted trees have served their purpose; the verified archives stay,
# so a re-run costs no download.
Remove-Item -Recurse -Force $extractDir

$missing = @($requiredFiles | Where-Object { -not (Test-Path (Join-Path $toolkitDir $_)) })
if ($missing.Count -gt 0) {
    throw "Toolkit assembled but incomplete; missing:`n  " + ($missing -join "`n  ")
}

# THE SMOKE TEST, and the reason the stamp is written last: it proves the
# compiler executes on THIS host, which on an ARM64 machine is the whole
# question. The output is shown, not swallowed.
Write-Host "Smoke test: nvcc --version"
& (Join-Path $toolkitDir 'bin\nvcc.exe') --version
if ($LASTEXITCODE -ne 0) { throw "nvcc.exe does not run on this host (exit $LASTEXITCODE)" }

Set-Content -NoNewline -Path $stampFile -Value $stampValue
Write-Host ''
Write-Host "CUDA toolkit $version assembled: $toolkitDir" -ForegroundColor Green
Write-Host "CUDA_PATH=$toolkitDir"
