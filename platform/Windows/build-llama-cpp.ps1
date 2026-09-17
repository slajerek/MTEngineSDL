param(
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64','ARM64')]
    [string]$Platform = 'x64',
    [ValidateSet('Clang','MSVC')]
    [string]$Compiler = 'Clang',
    [switch]$SkipCuda,

    # The APP's out root -- where mtcaps wrote MTEngineCaps.props. Only the CUDA
    # plugin needs it: it is the one dependency MSBuild builds, so it goes
    # through Directory.Build.props, which imports those props and errors when a
    # non-empty MTOutRoot has none beside it. build-deps.ps1 passes the
    # directory its own -CapsFile came from. Empty is legitimate and means a
    # standalone run: the plugin then builds without capability defines, which
    # is why a standalone run of this script cannot produce a shippable plugin.
    [string]$MTOutRoot,

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
Require-Path $llamaDir 'llama.cpp submodule (run: git submodule update --init --recursive)'

# NOT `Require-Command msbuild`. MSBuild is on PATH only inside a Developer
# prompt or after vcvarsall, so demanding it there failed this script outright
# from an ordinary shell -- and unconditionally, before the CPU leg, which
# needs no MSBuild at all. Get-MTMSBuild prefers PATH and falls back to the
# vswhere query the driver itself uses. Resolved here rather than at the point
# of use so the failure, when there is one, arrives before any building.
$msbuildExe = Get-MTMSBuild
if (-not $msbuildExe -and -not $SkipCuda) {
    throw "MSBuild not found (needed for the CUDA plugin). Install Visual Studio 2022 with the C++ workload, or pass -SkipCuda."
}

Write-Host "=== Building llama.cpp CPU bundle ==="
& (Join-Path $repoRoot 'platform\Windows\build-llama_cpp_cpu.ps1') -OutLibDir $OutLibDir -Configuration $Configuration -Platform $Platform -Compiler $Compiler

if (-not $SkipCuda) {
    # A real installation if CUDA_PATH names one, else the toolkit that
    # acquire-cuda-toolkit.ps1 assembled from NVIDIA's redistributable archives
    # -- the route this repository's ARM64 build machine has to take, since the
    # installer refuses it. Set back into the environment: MSBuild reads
    # environment variables as properties, and the plugin project links
    # $(CUDA_PATH)\lib\x64.
    $cudaPath = Resolve-MTCudaPath
    if (-not $cudaPath) {
        Write-Host "No CUDA toolkit - skipping CUDA build (run platform\Windows\acquire-cuda-toolkit.ps1, or install NVIDIA CUDA Toolkit)" -ForegroundColor Yellow
        $SkipCuda = $true
    } else {
        $env:CUDA_PATH = $cudaPath
    }
}

if (-not $SkipCuda) {

    Write-Host "=== Building llama.cpp CUDA libs ==="
    & (Join-Path $repoRoot 'platform\Windows\build-llama_cpp_cuda.ps1') -OutLibDir $OutLibDir -Configuration $Configuration -Platform $Platform

    Write-Host "=== Building CUDA plugin DLL (mt_llama_cuda_backend) ==="
    $sln = Join-Path $repoRoot 'platform\Windows\MTEngineSDL.sln'
    Require-Path $sln 'solution file'

    # OUTPUT UNDER THE LLAMA WORK DIR, NAMED HERE. R0.3 routed the plugin
    # project's OutDir under $(MTBuildRoot) -- and this script went on looking
    # for the DLL in the engine's own platform\Windows\bin, where nothing had
    # put it since. Passing MTBuildRoot explicitly means the DLL lands where
    # THIS script says, not where a Directory.Build.props fallback would put
    # it. MTCapsLibsDir is the view the cuda leg has just synced its archives
    # into, and what the project links; MTOutRoot is whatever the driver
    # exported, or the standalone spelling when nothing is driving.
    $pluginRoot = Join-Path (Get-MTCapsWorkDir 'llama.cpp') "plugin-windows-$Platform-$Configuration\"
    # MTOutRoot is passed ONLY when we have a real one. Directory.Build.props
    # fires MTCapsGuard on a non-empty MTOutRoot whose MTEngineCaps.props is
    # missing, and deliberately does NOT fire on an empty one -- so an unknown
    # root must be left unstated rather than guessed at. Guessing the
    # _standalone spelling is exactly what failed here: that directory has no
    # props, and the build stopped with "nothing was imported and this build
    # has no capability defines".
    $msbuildArgs = @($sln, '/m', '/t:mt_llama_cuda_backend',
        "/p:Configuration=$Configuration", "/p:Platform=$Platform", '/p:PlatformToolset=v143',
        "/p:MTBuildRoot=$pluginRoot", "/p:MTCapsLibsDir=$OutLibDir",
        '/v:minimal', '/nologo')
    # THE PLUGIN NEEDS AN APP'S CAPABILITY SET, and there is no way to conjure
    # one here. It is the only dependency MSBuild builds, so it goes through
    # Directory.Build.props, which imports MTEngineCaps.props from
    # $(MTOutRoot); the guard on that import runs BeforeTargets=PrepareForBuild,
    # EARLIER than the target that could resolve caps, so an out root without
    # props already beside it fails and cannot bootstrap itself. MT_STANDALONE=1
    # does not rescue it: that only redirects MTOutRoot to the _standalone
    # spelling, which has no props either unless a bare engine build put them
    # there first. Both measured, 2026-09-07.
    #
    # So this is stated as a requirement rather than worked around. A driven
    # build always satisfies it -- build-deps.ps1 derives the root from the
    # -CapsFile the driver already hands it -- and that is also the only way to
    # get a plugin whose capability defines match the app it ships with.
    if (-not $MTOutRoot) {
        throw ("The CUDA plugin needs an app's capability set: pass -MTOutRoot, or build through " +
               "build-deps.ps1 -CapsFile <app out root>\MTEngineCaps.xcconfig (which the app's " +
               "build-windows.ps1 does). Pass -SkipCuda to build the CPU bundle alone.")
    }
    if (-not (Test-Path (Join-Path $MTOutRoot 'MTEngineCaps.props'))) {
        throw "MTOutRoot '$MTOutRoot' has no MTEngineCaps.props; the caps resolve did not write there."
    }
    $msbuildArgs += "/p:MTOutRoot=$MTOutRoot"
    if ($env:MT_BUILD_JOBS) { $msbuildArgs += "/p:CL_MPCount=$env:MT_BUILD_JOBS" }
    & $msbuildExe @msbuildArgs
    if ($LASTEXITCODE -ne 0) { throw "msbuild failed for mt_llama_cuda_backend" }

    $pluginDll = Join-Path $pluginRoot "bin\$Platform\$Configuration\mt_llama_cuda_backend.dll"
    Require-Path $pluginDll 'CUDA plugin DLL'

    # -----------------------------------------------------------------------
    # WHAT SHIPS BESIDE IT -- read from the binary, not guessed. The plugin
    # links cudart, cublas and cublasLt as import libraries, so on a user's
    # machine it needs their DLLs beside it; cublas in turn pulls cublasLt,
    # and CUDA 12's cublasLt reaches for nvrtc and nvJitLink. A hand-written
    # list would be wrong at the next toolkit bump and nobody here can run
    # the plugin on a GPU to notice. So: walk the import tables with dumpbin,
    # transitively, keeping only what exists in the toolkit's bin\. That
    # excludes nvcuda.dll (the driver -- theirs, never shipped) and the
    # system DLLs by the same rule, and prints what it decided.
    # -----------------------------------------------------------------------
    if (-not (Import-MTVCEnvironment -Platform $Platform)) { throw "Could not import a Visual Studio environment for $Platform" }
    $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $dumpbin) { throw "dumpbin.exe is not on PATH after importing the VC environment" }
    $cudaBin = Join-Path $cudaPath 'bin'
    $runtime = New-Object System.Collections.Generic.List[string]
    $seen = @{}
    $queue = New-Object System.Collections.Generic.Queue[string]
    $queue.Enqueue($pluginDll)
    while ($queue.Count -gt 0) {
        $f = $queue.Dequeue()
        foreach ($line in (& $dumpbin.Source /nologo /dependents $f 2>&1)) {
            if ($line -match '^\s+([^\s\\/]+\.dll)\s*$') {
                $name = $Matches[1]
                $key = $name.ToLowerInvariant()
                $cand = Join-Path $cudaBin $name
                if ((Test-Path $cand) -and -not $seen[$key]) {
                    $seen[$key] = $true
                    $runtime.Add($cand)
                    $queue.Enqueue($cand)
                }
            }
        }
    }
    if ($runtime.Count -eq 0) {
        # The plugin links cudart at minimum; finding nothing means the walk
        # failed to read the import table, not that there is nothing to ship.
        throw "dumpbin found no CUDA runtime dependencies for $pluginDll -- the import-table walk failed"
    }

    # One directory the driver copies wholesale into Data\lib: the plugin and
    # everything it needs from the toolkit, and nothing else.
    $stage = Join-Path $OutLibDir 'cuda-runtime'
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item -Force $pluginDll $stage
    foreach ($r in $runtime) { Copy-Item -Force $r $stage }

    Write-Host "CUDA plugin built: $pluginDll"
    Write-Host "Staged for Data\lib ($stage):"
    Get-ChildItem $stage -File | ForEach-Object { Write-Host ("  {0,-32} {1,12:N0} bytes" -f $_.Name, $_.Length) }
}

# The summary names the VIEW, where every consumer links from. The block this
# replaces pointed at platform\Windows\libs -- the 25 tracked prebuilts a build
# must never write to, and a place nothing has written since L16 -- and did so
# with backslash escapes PowerShell does not have, which is where the
# "platform\\Windows\\libs\\x64\\Release\\llama_cpp.lib\" lines in every build
# log came from.
Write-Host "=== Done ==="
Write-Host "CPU engine lib: $(Join-Path $OutLibDir 'llama_cpp.lib')"
if (-not $SkipCuda) {
    Write-Host "CUDA archives : $OutLibDir (llama, ggml, ggml-base, ggml-cpu, ggml-cuda)"
    Write-Host "CUDA plugin   : $(Join-Path $OutLibDir 'cuda-runtime') (staged for Data\lib, with its runtime DLLs)"
}
