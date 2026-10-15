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

    The deploy stage is what ships the licence document, and it runs only
    with -Prod. A DEVELOPMENT build -- the default -- produces no package and
    copies no assets: it runs, and is tested, from the git root. A FINAL build
    passes -Prod and is tested from the package with tests\run_test.ps1
    -Package. "Always" therefore means "whenever a package is produced". The
    procedure is docs\testing.md.
.PARAMETER AppDir
    The app repo root (the stub passes $PSScriptRoot).
#>
param(
    [Parameter(Mandatory)][string]$AppDir,
    [ValidateSet('x64','ARM64')]
    [string]$Platform,
    [ValidateSet('Debug','Release')]
    [Alias('Config')]
    [string]$Configuration = 'Release',
    # THE FOUR SWITCHES, the same on every platform (maintainer, 2026-09-05).
    # -Logs on|off: MT_DEBUG_LOGS, on by default, OFF by default under -Prod.
    # -Symbols on|off: MT_RELEASE_SYMBOLS, on by default, OFF under -Prod.
    # -Tier dev|commercial: MT_COMMERCIAL_BUILD. An explicit switch beats what
    # -Prod implies; -Symbols on with -Tier commercial is refused.
    [ValidateSet('on','off')]
    [string]$Logs,
    [ValidateSet('on','off')]
    [string]$Symbols,
    [ValidateSet('dev','commercial')]
    [string]$Tier = 'dev',
    [ValidateSet('Clang','MSVC')]
    [string]$Compiler = 'Clang',
    [switch]$SkipCuda,
    [switch]$SkipDeps,
    [switch]$Prod,
    # The old default packaged on every build and -NoProd opted out. Kept as
    # a parameter so a caller still passing it fails with the new name
    # instead of silently building something else.
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

if ($NoProd) { throw '-NoProd is gone. A build makes no package unless you pass -Prod (see docs\testing.md).' }
# A final build tells the dependency scripts so: build-jpeg.ps1 then REFUSES a
# build without SIMD instead of warning. Set before the deps and compile
# stages; build-deps.ps1 and its children inherit the process env. The driver
# OWNS the variable: it is cleared when -Prod is not given, so a final build
# earlier in the same PowerShell session cannot leak into a development one.
if ($Prod) { $env:MT_PROD_BUILD = '1' } else { Remove-Item Env:MT_PROD_BUILD -ErrorAction SilentlyContinue }
$symbolsExplicit = [bool]$Symbols
if (-not $Logs)    { $Logs    = if ($Prod) { 'off' } else { 'on' } }
if (-not $Symbols) { $Symbols = if ($Prod) { 'off' } else { 'on' } }
if ($Tier -eq 'commercial' -and $symbolsExplicit -and $Symbols -eq 'on') {
    throw '-Symbols on cannot be combined with -Tier commercial: a store build never ships symbols.'
}
[string[]]$Set = @($Set | Where-Object { $_ })
$Set += "MT_DEBUG_LOGS=$(if ($Logs -eq 'on') { 1 } else { 0 })"
if ($Symbols -eq 'off') { $Set += 'MT_RELEASE_SYMBOLS=0' }
if ($Tier -eq 'commercial') { $Set += 'MT_COMMERCIAL_BUILD=1'; $Set += 'MT_PRIVATE_BUILD=0' }
Write-Host "Build: config=$($Configuration.ToLower()) logs=$Logs symbols=$Symbols tier=$Tier prod=$(if ($Prod) { 'yes' } else { 'no' })" -ForegroundColor Cyan
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
# EXPLICIT for BOTH branches, and forwarded to BOTH the engine .sln and the
# app .sln below (@toolsetArgs appears on both `& $msbuild` calls). An empty
# array here would not mean "no opinion" -- MSBuild then falls through to
# whatever PlatformToolset each .vcxproj hardcodes for itself, and the two
# disagree: the engine's is ClangCL, the app's is v143. That split reached CI
# for the first time on 2026-09-03 as 16 unresolved __std_* STL symbols at the
# app's final link -- object code the ClangCL engine build compiled against
# one STL/SDK vintage, linked by the app's v143 build against another's import
# libraries. A command-line /p: value is a GLOBAL property, which a .vcxproj's
# own unconditioned <PlatformToolset> assignment cannot override, so stating
# the toolset here -- for either $Compiler value -- settles it for both
# projects the same way regardless of what each .vcxproj says on its own.
# -Compiler stays the one command-line switch that decides it; the .vcxproj
# defaults are for an IDE build only, which never sees this script or its /p:
# arguments (see MTEngineApp.targets / Directory.Build.targets for that path).
[string[]]$toolsetArgs = if ($Compiler -eq 'MSVC') { @('/p:PlatformToolset=v143') } else { @('/p:PlatformToolset=ClangCL') }
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
$debugLogs = (($mtResolvedFlags | Where-Object { $_ -match '^MT_DEBUG_LOGS = ([01])$' } | Select-Object -First 1) -replace '^MT_DEBUG_LOGS = ', '')
if (-not $debugLogs) { $debugLogs = '1' }
Write-Host "Capabilities: $appName -> $mtOutRoot" -ForegroundColor Cyan
Write-Host "  deps: $mtLibsDir"
# tier and ffmpeg mode are different axes; see app-build-macos.sh.
Write-Host "  tier: $(if ($commercial -eq '1') { 'commercial' } else { 'non-commercial' }), ffmpeg=$ffmpegMode, symbols=$releaseSymbols, logs=$debugLogs" -ForegroundColor Cyan
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

# --- parallelism --------------------------------------------------------------
# Bare /m (every core) unless MT_BUILD_JOBS says otherwise.
#
# The default is unchanged: /m is what this script has always passed, and what
# it still passes when the variable is unset. The knob exists because /m scales
# with core count and not with available RAM -- on a many-core box a clang-cl
# build of the engine, or of OpenCV's dnn translation units, can exhaust memory
# and get the compiler killed. That surfaces as a build "failure" carrying no
# diagnostic at all, so capping jobs needs to be reachable without editing this
# file:
#
#   $env:MT_BUILD_JOBS = 4      # cap at 4 parallel jobs
#
# Set BEFORE the dependency stage so it covers that too: the two MSBuild calls
# below take $jobsArg, and the dep scripts' `cmake --build` reads the standard
# CMAKE_BUILD_PARALLEL_LEVEL, which is exported here for the same reason.
#
# BOTH LEVERS, because they multiply. /m caps how many PROJECTS build at once,
# but MTEngineSDL.vcxproj sets MultiProcessorCompilation, i.e. /MP, which spawns
# one cl per core INSIDE a single project -- and the engine is one project, so
# /m alone leaves it untouched. CL_MPCount is the property that caps /MP. Left
# unset, /MP keeps its default (one per core), which is today's behaviour.
$jobsArg = '/m'
$mpArgs  = @()
if ($env:MT_BUILD_JOBS) {
    $n = 0
    if (-not [int]::TryParse($env:MT_BUILD_JOBS, [ref]$n) -or $n -lt 1) {
        Write-Error "MT_BUILD_JOBS must be a positive integer, got '$env:MT_BUILD_JOBS'"; exit 1
    }
    $jobsArg = "/m:$n"
    $mpArgs  = @("/p:CL_MPCount=$n")
    $env:CMAKE_BUILD_PARALLEL_LEVEL = "$n"
    Write-Host "Build parallelism: $jobsArg /p:CL_MPCount=$n (MT_BUILD_JOBS)" -ForegroundColor Yellow
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
    @toolsetArgs @mtCapsArgs @modeArgs $jobsArg @mpArgs /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { Write-Error "MTEngineSDL build failed"; exit 1 }

Write-Host "`n=== Building $appName ($Platform $Configuration $Compiler) ===" -ForegroundColor Cyan
& $msbuild $appSln `
    /p:Configuration=$Configuration /p:Platform=$Platform `
    @toolsetArgs @mtCapsArgs @modeArgs $jobsArg @mpArgs /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { Write-Error "$appName build failed"; exit 1 }

$outDir = "$appDir\platform\Windows\bin\$Platform\$Configuration"
$exe    = Join-Path $outDir $conf['MT_WINDOWS_EXE']
if (-not (Test-Path $exe)) { Write-Error "Build completed but executable not found at $exe"; exit 1 }
Write-Host "`n=== Build successful ===" -ForegroundColor Green
Write-Host "Output: $outDir"

# --- licence gate -------------------------------------------------------------
# Keyed on the FLAG, not the capability -- see app-build-macos.sh. mtResolved
# is canonical_form and structurally cannot carry an MT_ENABLE_*, so the old
# test could not see an FFmpeg-less build and would demand a build-mode marker
# from a build that deliberately has none. UNVERIFIED: written on macOS.
# Use this resolve's generated flag, not an environment value that the
# dependency stage may never have populated (-SkipDeps), or left stale.
$ffmpegFlags = @($mtResolvedFlags | Where-Object { $_ -match '^\s*MT_ENABLE_FFMPEG\b' })
if ($ffmpegFlags.Count -ne 1 -or $ffmpegFlags[0] -notmatch '^MT_ENABLE_FFMPEG = ([01])$') {
    throw 'Expected exactly one MT_ENABLE_FFMPEG = 0 or 1 in MTEngineCaps.xcconfig'
}
$ffmpegOn = $Matches[1]
if ($ffmpegOn -eq '1') {
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

# The POSITIVE half -- proves a build that said it carries no FFmpeg carries
# none. Runs in BOTH directions on every build so it is exercised constantly
# rather than only in the rare configuration. UNVERIFIED on Windows.
& "$mtDir\tools\appbuild\scan-no-ffmpeg.ps1" -Binary $exe -Dir $outDir -Expect $(if ($ffmpegOn -eq '1') { 'present' } else { 'absent' })
if ($LASTEXITCODE -ne 0) { Write-Error "no-FFmpeg scan failed"; exit 1 }

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
if ($Prod) {
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
    # ---------------------------------------------------------------------
    # THE CUDA PLUGIN AND ITS RUNTIME -- BESIDE THE PACKAGE, NOT INSIDE IT.
    #
    # build-llama-cpp.ps1 stages mt_llama_cuda_backend.dll together with every
    # toolkit DLL its import table reaches into <libs>\cuda-runtime\. That set
    # is ~950 MB, of which cublasLt64_12.dll alone is 668 MB, because those
    # libraries carry kernels for every architecture NVIDIA supports. Bundling
    # it turns a 46 MB app into a ~1 GB download for EVERYONE, including the
    # majority with no NVIDIA GPU at all.
    #
    # So by default it ships as a separate add-on that a user unzips over the
    # app folder. This needs no engine change whatsoever: the plugin is already
    # runtime-loaded, LLamaBackend_WinCudaPlugin searches Data\lib among other
    # places, and a plugin that is not found is an ordinary supported state --
    # IsAvailable() returns false and the engine uses the CPU backend, which is
    # exactly what a machine without an NVIDIA GPU should do anyway.
    #
    # The add-on's layout mirrors the package (Data\lib\...) so unzipping it
    # over the app folder puts every DLL where the search already looks, and
    # Windows resolves the plugin's own imports from the directory it was
    # loaded from -- so co-locating the four files is sufficient and no PATH
    # change is needed.
    #
    # MT_CUDA_ADDON=bundled restores the old behaviour for a build that is
    # never distributed and where one folder is simply more convenient. It is
    # an environment variable rather than a parameter so that no app's stub has
    # to change to reach it -- the same reasoning as MT_BUILD_JOBS.
    #
    # A STALE ADD-ON CANNOT SILENTLY MISBEHAVE: the plugin API is versioned and
    # the loader refuses a plugin whose api_version does not match the engine's
    # (LLamaBackend_WinCudaPlugin.cpp), falling back to CPU. The manifest below
    # is therefore for a human reading a folder, not a safety mechanism.
    # ---------------------------------------------------------------------
    $cudaRuntimeSrc = Join-Path $mtLibsDir 'cuda-runtime'
    if (Test-Path $cudaRuntimeSrc) {
        $cudaMode = if ($env:MT_CUDA_ADDON) { $env:MT_CUDA_ADDON.ToLowerInvariant() } else { 'separate' }
        if ($cudaMode -notin @('separate', 'bundled')) {
            throw "MT_CUDA_ADDON must be 'separate' or 'bundled', got '$env:MT_CUDA_ADDON'"
        }

        $cudaRoot = if ($cudaMode -eq 'bundled') { $prodDir } else { "$appDir\platform\Windows\prod\$Platform-cuda" }
        if ($cudaMode -eq 'separate' -and (Test-Path $cudaRoot)) {
            Get-ChildItem $cudaRoot -Force | Remove-Item -Recurse -Force
        }
        $cudaLibDir = Join-Path $cudaRoot 'Data\lib'
        New-Item -ItemType Directory -Force -Path $cudaLibDir | Out-Null
        Get-ChildItem $cudaRuntimeSrc -Filter '*.dll' | Copy-Item -Destination $cudaLibDir -Force
        $cudaDlls = @(Get-ChildItem $cudaLibDir -Filter '*.dll')
        $cudaBytes = ($cudaDlls | Measure-Object -Property Length -Sum).Sum

        # THE NVIDIA LICENCE TEXT TRAVELS WITH THE DLLs, wherever they went.
        # It is assembled here, at DEPLOY time, and not from the capability
        # vocabulary -- the vocabulary is consumed by the caps RESOLVE, which
        # cannot know whether this deploy bundled or split, so an entry there
        # would name NVIDIA in the LICENSES.txt of every CUDA-capable build
        # including the ones that ship none of it. This stage knows.
        #
        # EVERY component's licence is included rather than a mapping from DLL
        # back to component: over-inclusive is harmless, and a mapping would
        # silently under-report the day the staged set changes. The text is
        # NVIDIA's own, copied by acquire-cuda-toolkit.ps1 out of each archive.
        $licDir = Join-Path (Get-MTCudaToolkitDir) 'licenses'
        if (-not (Test-Path $licDir)) {
            throw "No licence texts at $licDir -- CUDA binaries may not ship without them."
        }
        # WHOSE FILE IS WHOSE, stated exactly. mt_llama_cuda_backend.dll is
        # OURS -- it is the engine's plugin, built from this repository -- and
        # listing it under "these are NVIDIA's" would be a false statement in a
        # licence document. The NVIDIA files are the rest, identified by being
        # the ones the toolkit's own bin\ provided.
        $cudaOwnDll = 'mt_llama_cuda_backend.dll'
        $nvidiaDlls = @($cudaDlls | Where-Object { $_.Name -ne $cudaOwnDll })
        $licLines = @(
            "NVIDIA CUDA runtime components",
            "==============================",
            "",
            "The files listed under 'NVIDIA components' below are NVIDIA's,",
            "redistributed under the CUDA EULA's redistributable-components terms.",
            "They are not part of the application and are covered by no licence",
            "stated elsewhere in this package.",
            "",
            "NVIDIA components:"
        ) + @($nvidiaDlls | ForEach-Object { "  $($_.Name)" }) + @(
            "",
            "Also present, and NOT NVIDIA's -- it is part of this application and",
            "carries the application's own licence:",
            "  $cudaOwnDll",
            "",
            "nvcuda.dll is NOT redistributed: it is the display driver, installed on",
            "the user's own machine.",
            "",
            "The full text of every component licence from the CUDA toolkit follows.",
            "Some cover tools used only to BUILD these binaries and shipped nowhere;",
            "they are included in full rather than filtered, so that nothing which",
            "should appear is omitted by a mistaken mapping."
        )
        # ("-" * 70) PARENTHESISED, and it must be. Inside an array literal the
        # comma binds tighter than *, so `@("", "-" * 70, "x")` parses as
        # ("", "-") * (70, "x") -- array repetition with an ARRAY as the count,
        # which fails with "Cannot convert System.Object[] to System.UInt32"
        # and reads like it came from somewhere else entirely.
        $rule = '-' * 70
        foreach ($f in (Get-ChildItem $licDir -Filter '*.txt' | Sort-Object Name)) {
            $licLines += @("", $rule, "  $($f.BaseName)", $rule, "", (Get-Content -Raw $f.FullName))
        }

        if ($cudaMode -eq 'bundled') {
            # APPENDED to the package's own document. Bundling puts NVIDIA
            # binaries inside the package, so the package's LICENSES.txt has to
            # account for them; stage 7 already guarantees that file exists.
            $pkgLic = Join-Path $prodDir 'LICENSES.txt'
            if (-not (Test-Path $pkgLic)) { throw "bundled CUDA deploy but no LICENSES.txt in $prodDir" }
            Add-Content -Path $pkgLic -Encoding UTF8 -Value (@("", "") + $licLines)
            Write-Host "  NVIDIA CUDA licences appended to the package's LICENSES.txt"
        } else {
            # Its own file: the add-on is precisely the redistributed NVIDIA
            # material, and the package proper contains none of it -- the app
            # links llama_cpp.lib, the CPU bundle; only this plugin links the
            # CUDA archives.
            $licLines | Set-Content -Path (Join-Path $cudaRoot 'LICENSES-CUDA.txt') -Encoding UTF8

            @"
MTEngineSDL CUDA add-on
=======================

app         : $appName
platform    : windows $Platform
engine rev  : $(git -C $mtDir rev-parse --short HEAD 2>`$null)
cuda toolkit: $(Split-Path -Leaf (Get-MTCudaToolkitDir))
built       : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')

TO INSTALL: unzip this folder's contents over the application folder, so that
the DLLs land in <app>\Data\lib\. Start the app and check Help/AI settings --
the backend line reports whether the CUDA plugin was loaded.

An NVIDIA GPU and a current NVIDIA display driver are required. Without them
the application runs its CPU backend, which is the normal, supported state --
installing this add-on on a machine with no NVIDIA GPU changes nothing.

GPU architectures this build carries: see llama_cpp_cuda.archs in the engine's
dependency bucket; native code for Ampere/Ada/Blackwell, PTX (JIT-compiled by
the driver on first run) for Maxwell through Ampere.
"@ | Set-Content -Path (Join-Path $cudaRoot 'README-CUDA.txt') -Encoding UTF8
        }

        Write-Host ("CUDA {0}: {1} DLLs, {2:N0} MB -> {3}" -f $cudaMode, $cudaDlls.Count, ($cudaBytes / 1MB), $cudaRoot) -ForegroundColor Green
        if ($cudaMode -eq 'separate') {
            Write-Host "  the package itself stays CUDA-free; unzip the add-on over it to enable the GPU backend" -ForegroundColor Cyan
        }
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

# THE BINARY, BY ABSOLUTE PATH, ONCE, AT THE END. Same shape as the macOS and
# Linux drivers and as tests\run_test.ps1's "=== Using binary:" line, so a
# stale exe run from another location shows up as two different paths.
function Write-MtBinaryLine([string]$label, [string]$path) {
    $item = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
    if ($item) {
        Write-Host ("=== {0}: {1} ({2}, {3}) ===" -f $label, $item.FullName, $Configuration, $item.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')) -ForegroundColor Green
    } else {
        Write-Host ("=== {0}: {1} ({2}, missing) ===" -f $label, $path, $Configuration) -ForegroundColor Yellow
    }
}
Write-Host ""
Write-MtBinaryLine 'Built binary' $exe
if ($Prod) { Write-MtBinaryLine 'Deployed binary' (Join-Path $prodDir $prodExeName) }
