<#
.SYNOPSIS
    The Windows APP-BUILD DRIVER (unification plan, Phase 3).
.DESCRIPTION
    The engine owns the build flow; an app owns parameters. An app repo keeps
    a thin build-windows.ps1 stub whose only jobs are the chicken-and-egg
    ones -- verify MTENGINE_REF against the engine checkout, then call THIS
    script -- plus a mtengine-app.conf naming the app. Ported from the
    MTEngineSDLDummyApp wrapper, which was the reference implementation.

    Stages: conf -> tools -> resolve ONCE (mtcaps direct) -> selective
    submodules (the only init path, decision 0.6a) -> build-deps.ps1
    -CapsFile (fail-closed, all five families) -> engine sln -> app sln ->
    licence gate (keyed marker + forbidden-decoder scan for restricted
    modes) -> symbols contract (PDB to $MT_OUT\symbols; never shipped at
    MT_RELEASE_SYMBOLS=0) -> prod deploy with LICENSES.txt.

    The deploy stage is what ships the licence document, and -NoProd skips
    the stage. "Always" therefore means "whenever a package is produced" --
    a dev loop that skips the package skips the document with it, and a
    release does not skip the package.
.PARAMETER AppDir
    The app repo root (the stub passes $PSScriptRoot).
#>
param(
    [Parameter(Mandatory)][string]$AppDir,
    [ValidateSet('x64','ARM64')]
    [string]$Platform,
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('Clang','MSVC')]
    [string]$Compiler = 'Clang',
    [switch]$SkipCuda,
    [switch]$SkipDeps,
    [switch]$NoProd,
    [switch]$Clean,
    # Capability overrides forwarded to mtcaps (rung 1, persisted to
    # overrides.caps). How a store build is made -- -Set MT_COMMERCIAL_BUILD=1
    # -Set MT_PRIVATE_BUILD=0 -- WITHOUT editing the app's tracked licence
    # manifest. The override changes the caps hash, so such a build gets its
    # own out root and deps bucket and cannot be confused with a dev one.
    [string[]]$Set,
    [switch]$Gc,
    [Parameter(ValueFromRemainingArguments)][string[]]$GcArgs
)

$ErrorActionPreference = 'Stop'

$mtDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$appDir = (Resolve-Path $AppDir).Path

# --- GC shortcut: everything after -Gc goes to mtengine-gc.py verbatim -------
# (-Gc = report, -Gc --prune, -Gc --prune --dry-run, ...)
if ($GcArgs -and -not $Gc) { throw "unrecognized arguments: $GcArgs (GC args need -Gc first)" }
if ($Gc) {
    $gcScript = Join-Path $mtDir 'tools\appbuild\mtengine-gc.py'
    if (Get-Command py -ErrorAction SilentlyContinue) { & py -3 $gcScript @GcArgs }
    else { & python3 $gcScript @GcArgs }
    exit $LASTEXITCODE
}

# --- the app's parameters: one file, three platforms --------------------------
$confPath = Join-Path $appDir 'mtengine-app.conf'
if (-not (Test-Path $confPath)) { throw "no mtengine-app.conf in $appDir (see the DummyApp's)" }
$conf = @{}
foreach ($line in (Get-Content $confPath)) {
    $t = $line.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    $k, $v = $t -split '=', 2
    $v = $v.Trim()
    # Values with spaces are double-quoted so bash `source` accepts them too;
    # strip the quotes here for symmetry.
    if ($v.StartsWith('"') -and $v.EndsWith('"') -and $v.Length -ge 2) {
        $v = $v.Substring(1, $v.Length - 2)
    }
    $conf[$k.Trim()] = $v
}
foreach ($key in @('MT_APP_NAME', 'MT_WINDOWS_SLN', 'MT_WINDOWS_EXE')) {
    if (-not $conf[$key]) { throw "mtengine-app.conf does not set $key" }
}
$appName = $conf['MT_APP_NAME']
$appSln  = Join-Path $appDir $conf['MT_WINDOWS_SLN']
$manifest = Join-Path $appDir 'mtengine.caps'
if (-not (Test-Path $manifest)) { throw "mtengine.caps not found at $manifest" }

. (Join-Path $mtDir 'platform\Windows\mt-build-common.ps1')

# --- engine-ref verification + stub drift check (stub-shrink, 2026-08-31) ----
# Both moved here FROM the stubs: verification is read-only, so the driver
# owns it; the stub keeps only clone-when-absent.
# --- MTEngineSDL revision: track, or pin ---------------------------------------
# MTENGINE_REF names the engine revision this repo is built against, and the
# default is the BRANCH `origin/master` -- these apps track the engine's head and
# adapt to it. The file's own comments carry the why; the two behaviours are:
#
#   branch   TRACK. A checkout that is BEHIND prints a note, and nothing is ever
#            fatal -- ahead, diverged or dirty is ordinary engine work.
#   SHA/tag  PIN. A mismatch warns, and MTENGINE_PIN_STRICT=1 makes it an error.
#
# An EXISTING checkout is never moved either way: a build must not discard a
# developer's engine work in progress.
$refFile = Join-Path $appDir 'MTENGINE_REF'
if (-not (Test-Path $refFile)) { throw "MTENGINE_REF not found at $refFile" }
$mtRef = (Get-Content $refFile | Where-Object { $_ -notmatch '^\s*#' -and $_.Trim() -ne '' } | Select-Object -First 1).Trim()
if (-not $mtRef) { throw "MTENGINE_REF names no revision" }
Push-Location $mtDir
$mtHave = (git rev-parse HEAD).Trim()
# Branch or not: that one question is the whole difference between track and pin.
git show-ref --verify --quiet "refs/remotes/$mtRef"
$mtIsBranch = ($LASTEXITCODE -eq 0)
if (-not $mtIsBranch) {
    git show-ref --verify --quiet "refs/heads/$mtRef"
    $mtIsBranch = ($LASTEXITCODE -eq 0)
}
# A remote-tracking ref is only as fresh as the last fetch, so comparing against
# one without fetching compares against nothing. Offline, keep building.
if ($mtIsBranch) { git fetch --quiet origin 2>$null }
$mtWant = (git rev-parse --verify --quiet "$mtRef^{commit}")
if (-not $mtWant -and -not $mtIsBranch) {
    git fetch --quiet origin 2>$null
    $mtWant = (git rev-parse --verify --quiet "$mtRef^{commit}")
}
# BEHIND is the only state worth a word on a branch ref.
$mtBehind = $false
$mtCount = ''
if ($mtIsBranch -and $mtWant -and $mtHave -ne $mtWant.Trim()) {
    git merge-base --is-ancestor $mtHave $mtWant.Trim()
    $mtBehind = ($LASTEXITCODE -eq 0)
    if ($mtBehind) { $mtCount = (git rev-list --count "$mtHave..$($mtWant.Trim())").Trim() }
}
Pop-Location
if ($mtIsBranch) {
    if ($mtBehind) {
        Write-Host "NOTE: MTEngineSDL is $mtCount commit(s) behind $mtRef -- git -C `"$mtDir`" pull" -ForegroundColor Yellow
    }
} elseif ($mtWant -and $mtHave -ne $mtWant.Trim()) {
    $msg = "MTEngineSDL is at $($mtHave.Substring(0,12)) but MTENGINE_REF pins $($mtWant.Trim().Substring(0,12))"
    if ($env:MTENGINE_PIN_STRICT -eq '1') { throw "$msg -- run: git -C `"$mtDir`" checkout --detach $mtRef" }
    Write-Host "WARNING: $msg -- building what is checked out" -ForegroundColor Yellow
}

# The app-side files the engine owns a template for: the build stub, (L13) the
# MSBuild targets stub that imports the engine's IDE channel, and the props stub
# beside it.
#
# The props template carries __MT_APP_NAME__ where the others carry nothing app
# specific, because the app's identity is the one thing that cannot move into
# the engine: $(MTCapsApp) is what the engine's file is parameterized ON. So the
# comparison substitutes it first, and a stub that hardcoded some OTHER app's
# name -- the classic copy-paste between these four repos -- still shows up as
# drift.
foreach ($pair in @(
        @{ Template = 'tools\appbuild\stubs\build-windows.ps1';        App = 'build-windows.ps1' },
        @{ Template = 'tools\appbuild\stubs\Directory.Build.targets';  App = 'platform\Windows\Directory.Build.targets' },
        @{ Template = 'tools\appbuild\stubs\Directory.Build.props';    App = 'platform\Windows\Directory.Build.props' })) {
    $stubTemplate = Join-Path $mtDir $pair.Template
    $appStub = Join-Path $appDir $pair.App
    if ((Test-Path $stubTemplate) -and (Test-Path $appStub)) {
        $a = (Get-Content $stubTemplate -Raw).Replace('__MT_APP_NAME__', $appName)
        $b = Get-Content $appStub -Raw
        if ($a -ne $b) {
            Write-Host "NOTE: $($pair.App) differs from the engine's canonical stub template." -ForegroundColor Yellow
            if ($pair.App -like '*Directory.Build.props') {
                Write-Host "      Refresh it: (Get-Content `"$stubTemplate`" -Raw).Replace('__MT_APP_NAME__','$appName') | Set-Content `"$appStub`" -NoNewline" -ForegroundColor Yellow
            } else {
                Write-Host "      Refresh it: Copy-Item `"$stubTemplate`" `"$appStub`"" -ForegroundColor Yellow
            }
        }
    }
}

$Platform = Resolve-MTPlatform $Platform

foreach ($tool in @('cmake', 'git')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Error "$tool not found in PATH"; exit 1
    }
}
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -requires Microsoft.Component.MSBuild `
    -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
if (-not $msbuild) { Write-Error "MSBuild not found. Install Visual Studio 2022 with C++ workload."; exit 1 }
Write-Host "Using MSBuild: $msbuild"
[string[]]$toolsetArgs = if ($Compiler -eq 'MSVC') { @('/p:PlatformToolset=v143') } else { @() }
Write-Host "Compiler: $Compiler" -ForegroundColor Cyan
$env:PATH = (Split-Path $msbuild) + ";$env:PATH"
$null = Add-MTVCToolsToPath -Platform $Platform

# --- resolve ONCE (mtcaps direct; the backend is an engine option) ------------
$py = (Get-Command py -ErrorAction SilentlyContinue)
if ($py) { $pythonExe = @('py','-3') } else {
    $p3 = (Get-Command python3 -ErrorAction SilentlyContinue)
    if (-not $p3) { throw "python3 not found, and MTEngineSDL/tools/mtcaps needs it." }
    $pythonExe = @($p3.Source)
}
$mtcapsArgs = @(
    '-B', "$mtDir\tools\mtcaps\mtcaps.py", 'resolve',
    '--manifest', $manifest, '--app', $appName,
    '--platform', 'windows', '--arch', $Platform, '--config', $Configuration,
    '--engine-dir', $mtDir
) + (Get-MTLlamaBackendOption -Platform $Platform -SkipCuda:$SkipCuda) `
  + @($Set | Where-Object { $_ } | ForEach-Object { '--set'; $_ })
$pyArgs = @($pythonExe | Select-Object -Skip 1)
$mtcapsOut = & $pythonExe[0] ($pyArgs + $mtcapsArgs)
if ($LASTEXITCODE -ne 0) { throw "mtcaps resolve failed for $manifest" }

$mtOutRoot = ($mtcapsOut | Where-Object { $_ -like 'out_dir=*' }) -replace '^out_dir=', ''
if (-not $mtOutRoot) { throw "mtcaps resolve produced no out_dir line" }
$mtLibsDir = ($mtcapsOut | Where-Object { $_ -like 'deps_dir=*' }) -replace '^deps_dir=', ''
if (-not $mtLibsDir) { throw "mtcaps resolve produced no deps_dir line" }
New-Item -ItemType Directory -Force -Path $mtLibsDir | Out-Null
if (-not $mtOutRoot.EndsWith('\')) { $mtOutRoot += '\' }
$mtResolved = ($mtcapsOut | Where-Object { $_ -like 'resolved=*' }) -replace '^resolved=', ''
$ffmpegMode = ($mtcapsOut | Where-Object { $_ -like 'ffmpeg_mode=*' }) -replace '^ffmpeg_mode=', ''
$mtBuildRoot = ($mtcapsOut | Where-Object { $_ -like 'build_dir=*' }) -replace '^build_dir=', ''
if (-not $mtBuildRoot) { $mtBuildRoot = $mtOutRoot }
if (-not $mtBuildRoot.EndsWith('\')) { $mtBuildRoot += '\' }
if ($mtResolved -match 'MT_COMMERCIAL_BUILD=1') { $env:COMMERCIAL = '1' } else { $env:COMMERCIAL = '0' }
$commercial = $env:COMMERCIAL
[string[]]$modeArgs = if ($commercial -eq '1') { @('/p:MTExtraDefines=MT_COMMERCIAL_BUILD=1') } else { @() }
$mtResolvedFlags = Get-Content (Join-Path $mtOutRoot 'MTEngineCaps.xcconfig')
$releaseSymbols = (($mtResolvedFlags | Where-Object { $_ -match '^MT_RELEASE_SYMBOLS = ([01])$' } | Select-Object -First 1) -replace '^MT_RELEASE_SYMBOLS = ', '')
if (-not $releaseSymbols) { $releaseSymbols = '1' }
Write-Host "Capabilities: $appName -> $mtOutRoot" -ForegroundColor Cyan
Write-Host "  deps: $mtLibsDir"
# tier and ffmpeg mode are different axes; see app-build-macos.sh.
Write-Host "  tier: $(if ($commercial -eq '1') { 'commercial' } else { 'non-commercial' }), ffmpeg=$ffmpegMode, symbols=$releaseSymbols" -ForegroundColor Cyan
[string[]]$mtCapsArgs = @("/p:MTOutRoot=$mtOutRoot", "/p:MTCapsApp=$appName",
                          "/p:MTCapsLibsDir=$mtLibsDir",
                          "/p:MTBuildRoot=$mtBuildRoot")

# --- selective submodules: the ONLY init path (decision 0.6a) -----------------
Write-Host "`n=== Initializing MTEngineSDL submodules ===" -ForegroundColor Cyan
Push-Location $mtDir
$gated = @{
    'other/lib/mbedtls'   = 'MT_ENABLE_MBEDTLS'
    'other/lib/llama.cpp' = 'MT_ENABLE_LLAMA_CPP'
    'other/lib/ftxui'     = 'MT_ENABLE_FTXUI'
}
$needed = @()
foreach ($sm in $gated.Keys) {
    $off = ($mtResolvedFlags -match "^$($gated[$sm]) = 0$")
    if (-not $off -and -not (Test-Path (Join-Path $mtDir "$sm\CMakeLists.txt"))) { $needed += $sm }
}
if ($needed.Count -gt 0) {
    Write-Host "Fetching $($needed.Count) of 3 gated submodules: $($needed -join ', ')"
    git submodule update --init --recursive @needed | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "git submodule update failed" }
} else {
    Write-Host "No gated submodule needs fetching for this capability set"
}
Pop-Location

if ($Clean) {
    Write-Host "`n=== Cleaning ===" -ForegroundColor Yellow
    & $msbuild "$mtDir\platform\Windows\MTEngineSDL.sln" `
        /t:Clean /p:MT_STANDALONE=1 /p:Configuration=$Configuration /p:Platform=$Platform `
        @toolsetArgs @mtCapsArgs /v:minimal /nologo 2>$null
    & $msbuild $appSln `
        /t:Clean /p:MT_STANDALONE=1 /p:Configuration=$Configuration /p:Platform=$Platform `
        @toolsetArgs @mtCapsArgs /v:minimal /nologo 2>$null
    Write-Host "Clean complete." -ForegroundColor Green
    exit 0
}

# --- dependencies: fail-closed, all five families -----------------------------
if ($SkipDeps) {
    Write-Host "`n=== Skipping dependency acquisition (-SkipDeps) ===" -ForegroundColor Yellow
} else {
Write-Host "`n=== Building MTEngineSDL dependencies ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& "$mtDir\platform\Windows\build-deps.ps1" `
    -OutLibDir $mtLibsDir -CapsFile (Join-Path $mtOutRoot 'MTEngineCaps.xcconfig') `
    -Platform $Platform -Configuration $Configuration -Compiler $Compiler `
    -SkipCuda:($SkipCuda -or ($Platform -eq 'ARM64'))
if ($LASTEXITCODE -ne 0) { Write-Error "MTEngineSDL dependency build failed"; exit 1 }
}

Write-Host "`n=== Building MTEngineSDL ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& $msbuild "$mtDir\platform\Windows\MTEngineSDL.sln" `
    /t:MTEngineSDL `
    /p:Configuration=$Configuration /p:Platform=$Platform `
    @toolsetArgs @mtCapsArgs @modeArgs /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { Write-Error "MTEngineSDL build failed"; exit 1 }

Write-Host "`n=== Building $appName ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& $msbuild $appSln `
    /p:Configuration=$Configuration /p:Platform=$Platform `
    @toolsetArgs @mtCapsArgs @modeArgs /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { Write-Error "$appName build failed"; exit 1 }

$outDir = "$appDir\platform\Windows\bin\$Platform\$Configuration"
$exe    = Join-Path $outDir $conf['MT_WINDOWS_EXE']
if (-not (Test-Path $exe)) { Write-Error "Build completed but executable not found at $exe"; exit 1 }
Write-Host "`n=== Build successful ===" -ForegroundColor Green
Write-Host "Output: $outDir"

# --- licence gate -------------------------------------------------------------
if ($mtResolved -match 'MT_CAP_VIDEO_PLAYBACK=1') {
    $marker = "$mtLibsDir\ffmpeg\.ffmpeg-build-mode"
    $markerMode = if (Test-Path $marker) { (Get-Content $marker -Raw).Trim() } else { 'missing' }
    if ($commercial -eq '1' -and $markerMode -ne 'commercial') {
        Write-Error "commercial app build against a '$markerMode' FFmpeg install ($marker)"; exit 1
    } elseif ($ffmpegMode -and $markerMode -ne $ffmpegMode) {
        Write-Warning "FFmpeg install is '$markerMode' but this resolve expects '$ffmpegMode' -- stale prefix."
    }
    if ($ffmpegMode -and $ffmpegMode -ne 'full') {
        & "$mtDir\tools\appbuild\scan-forbidden-symbols.ps1" -Mode $ffmpegMode -Libs $mtLibsDir
        if ($LASTEXITCODE -ne 0) { Write-Error "forbidden-decoder scan failed"; exit 1 }
    }
}

# --- app post-build hook (optional) -------------------------------------------
if ($conf['MT_HOOK_POST_BUILD']) {
    $hook = Join-Path $appDir $conf['MT_HOOK_POST_BUILD']
    if (-not (Test-Path $hook)) { Write-Error "MT_HOOK_POST_BUILD names a missing file: $hook"; exit 1 }
    Write-Host "Running post-build hook: $($conf['MT_HOOK_POST_BUILD'])"
    $env:APP_DIR = $appDir; $env:APP_BINARY = $exe; $env:MT_OUT = $mtOutRoot
    if ($hook.EndsWith('.sh')) {
        # A shared sh hook (the common case -- one hook, three platforms).
        # Git Bash is already a requirement of build-windows.sh.
        $bash = Get-Command bash -ErrorAction SilentlyContinue
        if (-not $bash) { Write-Error "the post-build hook is a .sh and bash is not on PATH"; exit 1 }
        & $bash.Source $hook
    } else {
        & $hook
    }
    if ($LASTEXITCODE -ne 0) { Write-Error "post-build hook failed"; exit 1 }
}

# --- symbols contract (decision 0.5) ------------------------------------------
# The PDB always lands in $MT_OUT\symbols (store-crash symbolication and the
# stripped-build probe surface); it is never shipped -- prod copies only the
# exe/DLLs -- and MT_RELEASE_SYMBOLS=0 records that this artifact must not
# carry symbols (on Windows the split is inherent: the PE never embeds them).
$symbolsDir = Join-Path ($mtOutRoot.TrimEnd('\')) 'symbols'
New-Item -ItemType Directory -Force -Path $symbolsDir | Out-Null
$pdb = [System.IO.Path]::ChangeExtension($exe, '.pdb')
if (Test-Path $pdb) {
    Copy-Item $pdb $symbolsDir -Force
    Write-Host "Symbols: PDB retained at $symbolsDir (MT_RELEASE_SYMBOLS=$releaseSymbols)"
} else {
    Write-Warning "no PDB next to the exe -- the /DEBUG plumbing has not produced one (Phase 3 Windows leg is UNVERIFIED)"
}

# Cache-growth hint (cheap dir count; the GC owns cleanup).
$mtRootDir = if ($env:MTENGINE_BUILD_ROOT) { $env:MTENGINE_BUILD_ROOT } else { Join-Path $env:USERPROFILE '.cache\mtengine' }
$appCache = Join-Path $mtRootDir $appName
if (Test-Path $appCache) {
    $revCount = @(Get-ChildItem $appCache -Directory | Where-Object { $_.Name -ne '_build' }).Count
    if ($revCount -gt 12) {
        Write-Host "NOTE: $revCount rev-keyed build dirs under $appCache -- consider: py -3 `"$mtDir\tools\appbuild\mtengine-gc.py`" --prune" -ForegroundColor Yellow
    }
}

# --- prod deploy --------------------------------------------------------------
if (-not $NoProd) {
    $exeBase = [System.IO.Path]::GetFileNameWithoutExtension($conf['MT_WINDOWS_EXE'])
    $prodExeName = if ($env:COMMERCIAL -eq '1') { "$exeBase.exe" } else { "$exeBase-nc.exe" }
    $prodDir = "$appDir\platform\Windows\prod\$Platform"
    Write-Host "`n=== Deploying release package ($Platform $Configuration) ===" -ForegroundColor Cyan
    if (Test-Path $prodDir) { Get-ChildItem $prodDir -Force | Remove-Item -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $prodDir | Out-Null
    Copy-Item $exe (Join-Path $prodDir $prodExeName)
    Get-ChildItem "$outDir\*.dll" -ErrorAction SilentlyContinue | Copy-Item -Destination $prodDir
    Copy-Item (Join-Path $mtOutRoot 'LICENSES.txt') $prodDir
    $assetsName = if ($conf['MT_WINDOWS_ASSETS']) { $conf['MT_WINDOWS_ASSETS'] } else { 'assets' }
    $assetsDir = Join-Path $appDir $assetsName
    if (Test-Path $assetsDir) { Copy-Item $assetsDir (Join-Path $prodDir 'assets') -Recurse -Force }
    # MT_APP_PAYLOAD -- further directories the app needs AT RUNTIME, copied in
    # under their own names. One `assets` directory was not enough: the package
    # is the working directory a test or a user runs from, so anything the app
    # opens by a relative path has to be in it. One host application reads a
    # config file under data/ during init and SYS_FatalExit's without it, which
    # is exactly what the first run of its package did (2026-09-02).
    # Space separated, repo-relative, same key on all three platforms.
    if ($conf['MT_APP_PAYLOAD']) {
        foreach ($rel in ($conf['MT_APP_PAYLOAD'] -split '\s+' | Where-Object { $_ })) {
            $src = Join-Path $appDir $rel
            if (Test-Path $src) {
                Copy-Item $src (Join-Path $prodDir (Split-Path $rel -Leaf)) -Recurse -Force
            } else {
                Write-Warning "MT_APP_PAYLOAD names '$rel', which does not exist in $appDir"
            }
        }
    }
    $cudaPluginSrc = Join-Path $outDir 'Data\lib\mt_llama_cuda_backend.dll'
    if (Test-Path $cudaPluginSrc) {
        $cudaPluginDestDir = Join-Path $prodDir 'Data\lib'
        New-Item -ItemType Directory -Force -Path $cudaPluginDestDir | Out-Null
        Copy-Item $cudaPluginSrc $cudaPluginDestDir
    }
    # Belt for the symbols contract: a PDB must never ride a prod deploy.
    Get-ChildItem $prodDir -Recurse -Filter '*.pdb' -ErrorAction SilentlyContinue | Remove-Item -Force
    # The licence check. Stage 7 says the document ships ALWAYS, and until the
    # sh drivers grew this stage that was true on one platform in three. It is
    # checked here rather than assumed, because a copy that silently did not
    # happen is exactly what "always" is supposed to rule out.
    if (-not (Test-Path (Join-Path $prodDir 'LICENSES.txt'))) {
        throw "release package has no LICENSES.txt: $prodDir"
    }
    Write-Host "Deployed: $prodDir\$prodExeName" -ForegroundColor Green
}
