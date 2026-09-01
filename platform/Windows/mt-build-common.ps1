<#
.SYNOPSIS
    Shared helpers for the MTEngineSDL Windows build scripts.
.DESCRIPTION
    Dot-source it from a build script:

        . "$PSScriptRoot\mt-build-common.ps1"

    Two problems live here, both of them about ARCHITECTURE:

      Get-MTHostArch / Resolve-MTPlatform
          answer "what machine is this?" in a way that survives x64 emulation
          on Windows-on-ARM, where the environment lies.

      Reset-MTStaleCMakeCache
          make a CMake build directory that was configured for a DIFFERENT
          architecture (or generator, or toolset) reconfigure instead of
          aborting the build.

    This file is read by app builds, never written to -- it takes every output
    path as a parameter.
#>

function Get-MTHostArch {
    <#
    .SYNOPSIS
        The architecture of the MACHINE: 'ARM64' or 'x64'.
    #>
    [CmdletBinding()]
    param()

    # $env:PROCESSOR_ARCHITECTURE describes the PROCESS, not the machine, and a
    # child process inherits whatever its parent had. Git Bash on Windows-on-ARM
    # is an x64 build running under emulation, so it reports AMD64 -- and so
    # does every PowerShell it launches, and every script those launch in turn.
    # That is how ./build-windows.sh announced "Auto-detected platform: x64" on
    # an ARM64 box and configured the whole dependency tree for the wrong
    # architecture. .NET is no better under emulation: RuntimeInformation
    # reports X64 for OSArchitecture as well as ProcessArchitecture.
    #
    # The kernel writes the CPU identifier into the registry, and that value is
    # not redirected for emulated processes, so it answers for the MACHINE.
    # (Verified on an ARM64 box: an emulated x64 PowerShell reads back
    # "ARMv8 (64-bit) Family 8 Model 0 Revision 0" while $env:PROCESSOR_ARCHITECTURE
    # says AMD64.)
    $identifier = $null
    try {
        $identifier = (Get-ItemProperty -Path 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0' `
            -Name 'Identifier' -ErrorAction Stop).Identifier
    } catch { }

    if ($identifier) {
        if ($identifier -match '^ARM') { return 'ARM64' }
        return 'x64'
    }

    # Fallbacks. PROCESSOR_ARCHITEW6432 exists only inside a WOW64 process and
    # names the native architecture there; PROCESSOR_ARCHITECTURE is the last
    # resort and the one that lies under emulation.
    $arch = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
    if ($arch -eq 'ARM64') { return 'ARM64' }
    return 'x64'
}

function Resolve-MTPlatform {
    <#
    .SYNOPSIS
        Return $Platform if the caller was given one, else auto-detect it.
    #>
    [CmdletBinding()]
    param([string]$Platform)

    if ($Platform) { return $Platform }
    $detected = Get-MTHostArch
    Write-Host "Auto-detected platform: $detected"
    return $detected
}

function Get-MTVSGenerator {
    <#
    .SYNOPSIS
        The default Visual Studio CMake generator of the installed VS.
    .DESCRIPTION
        Handles 'Visual Studio 17 2022', 'Visual Studio 18 2026', ... without
        hardcoding a version.
    #>
    [CmdletBinding()]
    param()

    $genMatch = cmake --help | Select-String -Pattern '^\*\s+(Visual Studio \d+ \d+)'
    if (-not $genMatch) { throw "Could not detect the default Visual Studio CMake generator from 'cmake --help'." }
    return $genMatch.Matches[0].Groups[1].Value
}

function Add-MTVCToolsToPath {
    <#
    .SYNOPSIS
        Put this build's VC tools (cl.exe, lib.exe) on PATH. Returns the
        directory added, or $null if none was found.
    .DESCRIPTION
        The dependency scripts shell out to lib.exe to combine archives and to
        cl.exe for the mbedTLS stub. Those live in
        VC\Tools\MSVC\<ver>\bin\Host<host>\<target> and are on PATH only inside
        a "Developer PowerShell for VS 2022" -- so a build started from an
        ordinary shell, or through build-windows.sh, has to resolve them itself
        or die with "Missing required tool 'lib'".

        The host half is where the compiler RUNS, the target half is what it
        BUILDS. On an ARM64 machine without the host-native tools installed, the
        x64-hosted cross compiler runs under emulation, so it is the fallback.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][ValidateSet('x64', 'ARM64')][string]$Platform,
        [string]$HostArch
    )

    if (-not $HostArch) { $HostArch = Get-MTHostArch }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $vsInstall = & $vswhere -latest -property installationPath 2>$null | Select-Object -First 1
    if (-not $vsInstall) { return $null }

    $vcToolsDir = Join-Path $vsInstall 'VC\Tools\MSVC'
    if (-not (Test-Path $vcToolsDir)) { return $null }
    $latestVc = Get-ChildItem $vcToolsDir -Directory | Sort-Object Name | Select-Object -Last 1
    if (-not $latestVc) { return $null }

    foreach ($candidate in @(
        (Join-Path $latestVc.FullName "bin\Host$HostArch\$Platform"),
        (Join-Path $latestVc.FullName "bin\Hostx64\$Platform")
    )) {
        if (Test-Path $candidate) {
            if (($env:PATH -split ';') -notcontains $candidate) {
                $env:PATH = "$candidate;$env:PATH"
            }
            Write-Host "Added VC tools: $candidate"
            return $candidate
        }
    }
    return $null
}

function Remove-MTBuildDir {
    <#
    .SYNOPSIS
        Delete a build directory, saying why, and fail loudly if it cannot.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Dir,
        [Parameter(Mandatory = $true)][string]$Reason
    )

    if (-not (Test-Path -LiteralPath $Dir)) { return }
    Write-Host "Removing stale build directory: $Dir" -ForegroundColor Yellow
    Write-Host "  reason: $Reason" -ForegroundColor Yellow
    try {
        Remove-Item -LiteralPath $Dir -Recurse -Force -ErrorAction Stop
    } catch {
        throw ("Could not remove stale build directory '$Dir': $($_.Exception.Message). " +
               "Close whatever is holding files open there (Visual Studio, a running build, " +
               "an antivirus scan) and re-run.")
    }
}

function Get-MTCMakeCacheValue {
    <#
    .SYNOPSIS
        Read one entry out of the lines of a CMakeCache.txt ('' when absent).
    #>
    [CmdletBinding()]
    param(
        [string[]]$CacheLines,
        [Parameter(Mandatory = $true)][string]$Key
    )

    if (-not $CacheLines) { return '' }
    $pattern = "^\s*$([regex]::Escape($Key)):[A-Za-z]+="
    $line = $CacheLines | Where-Object { $_ -match $pattern } | Select-Object -First 1
    if (-not $line) { return '' }
    return ($line -replace $pattern, '')
}

function Reset-MTStaleCMakeCache {
    <#
    .SYNOPSIS
        Wipe a CMake build directory whose cache disagrees with this build.
    .DESCRIPTION
        CMake refuses to reconfigure a build directory when the generator
        platform, generator or toolset changed, and the message it prints
        ("Does not match the platform used previously: ARM64 ... Either remove
        the CMakeCache.txt file and CMakeFiles directory") is a dead end in an
        automated build. Every caller here owns its build directory outright,
        so the right answer is simply to remove it and configure afresh.

        Only the parameters the caller actually passes are compared, so a
        script that does not care about the toolset can leave it out.

        Returns $true when the directory was removed. Callers that do not want
        that on their pipeline should assign it to $null.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [string]$Platform,
        [string]$Generator,
        [string]$Toolset,
        [string]$SourceDir,
        [string]$Label
    )

    if (-not $Label) { $Label = Split-Path -Leaf $BuildDir }
    if (-not (Test-Path -LiteralPath $BuildDir)) { return $false }

    $cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cacheFile)) {
        # CMakeFiles without a cache is an interrupted or half-deleted
        # configure; CMake can trip over it, and nothing in there is worth
        # keeping.
        if (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeFiles')) {
            Remove-MTBuildDir -Dir $BuildDir -Reason "$Label has a CMakeFiles directory but no CMakeCache.txt (interrupted configure)"
            return $true
        }
        return $false
    }

    $lines = @(Get-Content -LiteralPath $cacheFile -ErrorAction SilentlyContinue)
    if ($lines.Count -eq 0) {
        Remove-MTBuildDir -Dir $BuildDir -Reason "$Label has an empty or unreadable CMakeCache.txt"
        return $true
    }

    $reasons = @()

    if ($PSBoundParameters.ContainsKey('Platform')) {
        $have = Get-MTCMakeCacheValue -CacheLines $lines -Key 'CMAKE_GENERATOR_PLATFORM'
        if ($have -ne $Platform) {
            $reasons += "platform '$(Format-MTCacheValue $have)' != '$(Format-MTCacheValue $Platform)'"
        }
    }
    if ($PSBoundParameters.ContainsKey('Generator')) {
        $have = Get-MTCMakeCacheValue -CacheLines $lines -Key 'CMAKE_GENERATOR'
        if ($have -ne $Generator) {
            $reasons += "generator '$(Format-MTCacheValue $have)' != '$(Format-MTCacheValue $Generator)'"
        }
    }
    if ($PSBoundParameters.ContainsKey('Toolset')) {
        $have = Get-MTCMakeCacheValue -CacheLines $lines -Key 'CMAKE_GENERATOR_TOOLSET'
        if ($have -ne $Toolset) {
            $reasons += "toolset '$(Format-MTCacheValue $have)' != '$(Format-MTCacheValue $Toolset)'"
        }
    }
    if ($PSBoundParameters.ContainsKey('SourceDir')) {
        $have = Get-MTCMakeCacheValue -CacheLines $lines -Key 'CMAKE_HOME_DIRECTORY'
        if ((ConvertTo-MTComparablePath $have) -ne (ConvertTo-MTComparablePath $SourceDir)) {
            $reasons += "source directory '$(Format-MTCacheValue $have)' != '$(Format-MTCacheValue $SourceDir)'"
        }
    }

    if ($reasons.Count -eq 0) { return $false }

    Remove-MTBuildDir -Dir $BuildDir -Reason "$Label was configured with $($reasons -join ', ')"
    return $true
}

function Format-MTCacheValue {
    [CmdletBinding()]
    param([string]$Value)
    if ([string]::IsNullOrEmpty($Value)) { return '<none>' }
    return $Value
}

function ConvertTo-MTComparablePath {
    <#
    .SYNOPSIS
        Normalize a path for comparison. CMake writes forward slashes into the
        cache; the scripts hand it backslashes.
    #>
    [CmdletBinding()]
    param([string]$Path)

    if ([string]::IsNullOrEmpty($Path)) { return '' }
    return ($Path -replace '/', '\').TrimEnd('\').ToLowerInvariant()
}

function Get-MTLlamaBackendOption {
    <#
    .SYNOPSIS
        The --engine-option arguments that tell mtcaps which llama.cpp backend
        this build selects.
    .DESCRIPTION
        -SkipCuda picks between build_llama_cpp_cpu.ps1 and
        build_llama_cpp_cuda.ps1: two materially different archives under an
        IDENTICAL capability set. That is precisely what resolve.backend_hash
        exists for, and the backend it returns is a component of both $MT_OUT and
        the dependency-archive directory.

        Without this, both a CPU and a CUDA build resolved backend=default and
        shared one output root -- they already did before the dependency
        directory existed; this makes the sharing visible rather than creating
        it.

        ARM64 never gets CUDA, which is the same rule build-deps.ps1 already
        writes as -SkipCuda:($SkipCuda -or ($Platform -eq 'ARM64')). Every
        Windows resolve must pass the SAME value -- this function, and the
        MTCapsCuda property in the engine's and each app's
        Directory.Build.targets. Two resolves that disagree put the fragments
        under one backend and the archives under another, with no diagnostic.
    #>
    param(
        [Parameter(Mandatory)][string]$Platform,
        [bool]$SkipCuda
    )
    $cuda = if ($SkipCuda -or $Platform -eq 'ARM64') { 'OFF' } else { 'ON' }
    return @('--engine-option', "MT_LLAMA_CUDA=$cuda")
}

function Get-MTBuildRoot {
    <#
    .SYNOPSIS
        THE build root, and the only place PowerShell spells its default.

    .DESCRIPTION
        The PowerShell mirror of tools\mtcaps\resolve.py default_build_root().
        Keep the two in step: when they disagree, the resolve puts artifacts
        under one root and the dependency scripts put theirs under another,
        with no diagnostic -- which is exactly what happened between
        2026-09-01 and 2026-09-02, when the LOCALAPPDATA -> .cache decision
        was applied to resolve.py and to the props but NOT to the four
        PowerShell fallbacks. The dependency WORK trees kept building under
        %LOCALAPPDATA%\mtengine, so the orphan cache that item 16(b) deleted
        came straight back, and mtengine-gc.py -- which only ever walks the
        CURRENT root -- could not see it.

        %USERPROFILE%\.cache\mtengine and NOT %LOCALAPPDATA%: MSBuild's
        FileTracker drops read/write tracking under LOCALAPPDATA, which
        disables incremental builds outright. That applies to the dependency
        build trees this root also holds, not just to the engine's objects.
    #>
    if ($env:MTENGINE_BUILD_ROOT) { return $env:MTENGINE_BUILD_ROOT }
    $home_ = if ($env:USERPROFILE) { $env:USERPROFILE } else { [Environment]::GetFolderPath('UserProfile') }
    return (Join-Path $home_ '.cache\mtengine')
}

function Get-MTCapsWorkDir {
    <#
    .SYNOPSIS
        The dependency WORK root (downloads/sources/build trees/installs) for
        one dependency family -- outside every checkout. Phase 2 moved the
        codec caches here from other\lib\{image,video}-codecs.
    #>
    param([Parameter(Mandatory)][string]$Name)
    $dir = Join-Path (Get-MTBuildRoot) "_deps\work\$Name"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    return $dir
}

function Import-MTCapsEnvironment {
    <#
    .SYNOPSIS
        Publish a resolved capability set into the environment as MT_ENABLE_*.

    .DESCRIPTION
        THE PIECE WINDOWS WAS MISSING.

        The dependency scripts gate themselves on $env:MT_ENABLE_<DEP>, which is
        how macOS has always worked -- but on macOS nothing has to publish those
        variables, because the dependency builds are Xcode SCRIPT PHASES and
        Xcode exports every build setting from the resolved xcconfig into the
        phase's environment for free.

        Windows has no equivalent. build-deps.ps1 and the six library scripts are
        plain PowerShell, MSBuild properties are not environment variables, and
        nothing converted the resolved fragment into either. So the one gate that
        did exist -- build-mbedtls.ps1's -- could never fire, and every app built
        mbedTLS, FTXUI, llama.cpp and the image codecs whatever its manifest
        said. This function is what makes a gate on Windows mean anything.

        Reads MTEngineCaps.xcconfig, the same fragment mtcaps resolve writes for
        Xcode, whose MT_ENABLE_* lines are already exactly `NAME = 0|1`.

        ABSENT MEANS ON. A name missing from the fragment is left alone rather
        than defaulted to 0, matching `${MT_ENABLE_FTXUI:-1}` in the macOS
        scripts: a bare engine build resolves no manifest and must still build
        everything.
    #>
    param(
        [Parameter(Mandatory)][string]$CapsFile
    )

    if (-not (Test-Path $CapsFile)) {
        throw "No capability fragment at $CapsFile. It is written by 'mtcaps resolve'; pass the out_dir that resolve printed."
    }

    $count = 0
    foreach ($line in (Get-Content $CapsFile)) {
        if ($line -match '^\s*(MT_ENABLE_[A-Z0-9_]+)\s*=\s*([01])\s*$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
            $count++
        }
        # The resolved FFmpeg mode (Phase 2, 2026-08-31): a string, not 0/1,
        # so it needs its own line. build-video_codecs.ps1 prefers it over the
        # legacy COMMERCIAL env channel.
        elseif ($line -match '^\s*MT_FFMPEG_BUILD_MODE\s*=\s*(full|commercial)\s*$') {
            $env:MT_FFMPEG_BUILD_MODE = $Matches[1]
        }
    }

    if ($count -eq 0) {
        throw "Read $CapsFile but found no MT_ENABLE_* lines. A resolve that emits none is a resolve that went wrong -- building on it would silently build everything."
    }

    Write-Host "Capabilities: published $count MT_ENABLE_* flags from $CapsFile"
}

function New-MTCapsStubArchive {
    <#
    .SYNOPSIS
        Write a stub .lib for a capability that is switched OFF, and stamp it.

    .DESCRIPTION
        A disabled dependency still has to leave an archive behind: the engine
        and app projects name these libraries on the link line unconditionally,
        and an absent file is a link error rather than a saving. The stub carries
        one dummy symbol and no library symbols, so anything that actually calls
        into the dependency fails at link time -- which is the intended outcome,
        because the calling code should have been compiled out by the matching
        MT_ENABLE_* define.

        Returns nothing; the caller exits after this. The stamp records that the
        archive is a STUB and which script produced it, so flipping the
        capability back on rebuilds for real instead of finding a stale stub and
        declaring itself up to date. That is why $StampValue carries the script
        hash and the disabled marker together.
    #>
    param(
        [Parameter(Mandatory)][string]$OutLib,
        [Parameter(Mandatory)][string]$Stamp,
        [Parameter(Mandatory)][string]$Symbol,
        [Parameter(Mandatory)][string]$StampValue
    )

    if ((Test-Path $OutLib) -and (Test-Path $Stamp)) {
        if ((Get-Content $Stamp -Raw).Trim() -eq $StampValue) { return }
    }

    $outDir = Split-Path -Parent $OutLib
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    $tmpDir = Join-Path $outDir "$Symbol`_stub"
    if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir }
    New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

    $stubC   = Join-Path $tmpDir "$Symbol`_stub.c"
    $stubObj = Join-Path $tmpDir "$Symbol`_stub.obj"
    Set-Content -NoNewline -Path $stubC -Value "int $Symbol`_disabled_stub = 0;`r`n"

    & cl /nologo /c $stubC "/Fo$stubObj" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cl failed compiling the $Symbol stub" }
    if (Test-Path $OutLib) { Remove-Item -Force $OutLib }
    & lib /nologo /OUT:$OutLib $stubObj | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "lib failed packing the $Symbol stub" }

    Set-Content -NoNewline -Path $Stamp -Value $StampValue
    if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir }

    Write-Host "$Symbol is disabled by the capability set; wrote stub $OutLib"
}

# ============================================================================
# Shared helpers for the dependency scripts that build a single static archive
# from a vendored source tree (SDL3, libuv, freetype, uSockets).
# ============================================================================

function Invoke-MTNative {
    <#
    .SYNOPSIS
        Run a native command and fail only on a non-zero exit code.

    .DESCRIPTION
        With the script-global $ErrorActionPreference = 'Stop', PowerShell 5.1
        turns ANY stderr line written by a native command into a terminating
        NativeCommandError -- regardless of the exit code, and with no explicit
        2>&1 redirection. A benign "CMake Deprecation Warning" from a
        third-party CMakeLists.txt on an otherwise-successful configure was
        enough to abort build-mbedtls.ps1 and build-video_codecs.ps1 outright,
        which is why both carry the same relaxation inline. $LASTEXITCODE is
        the real, sufficient success/failure signal.
    #>
    param(
        [Parameter(Mandatory = $true)] [scriptblock] $Action,
        [Parameter(Mandatory = $true)] [string] $What
    )

    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Action
        if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
    } finally {
        $ErrorActionPreference = $savedEap
    }
}

function Find-MTConfigLib {
    <#
    .SYNOPSIS
        Locate the archive built for a specific configuration.

    .DESCRIPTION
        The Visual Studio generator is multi-config: one build tree holds both
        Debug\ and Release\ outputs. A bare `Get-ChildItem -Recurse | Select
        -First 1` returns whichever the enumeration reaches first, and "Debug"
        sorts before "Release" -- so a Release bundle silently gets packed from
        the DEBUG libraries. That is not hypothetical: it shipped debug mbedTLS,
        and with it msvcrtd (the non-redistributable debug CRT), inside a
        Release build, while the stamp still recorded ":Release:".

        Falls back to a configuration-neutral hit for single-config generators,
        but never to a DIFFERENT configuration's output.
    #>
    param(
        [Parameter(Mandatory = $true)] [string] $BuildDir,
        [Parameter(Mandatory = $true)] [string] $Name,
        [Parameter(Mandatory = $true)] [string] $Configuration
    )

    $knownConfigs = @('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')

    $all = @(Get-ChildItem -Path $BuildDir -Recurse -File -Filter $Name -ErrorAction SilentlyContinue)
    if ($all.Count -eq 0) { throw "Expected library not found under ${BuildDir}: $Name" }

    $sep = [System.IO.Path]::DirectorySeparatorChar

    $inConfig = @($all | Where-Object { $_.DirectoryName.Split($sep) -contains $Configuration })
    if ($inConfig.Count -gt 0) { return $inConfig[0].FullName }

    $otherConfigs = @($knownConfigs | Where-Object { $_ -ne $Configuration })
    $neutral = @($all | Where-Object {
        $parts = $_.DirectoryName.Split($sep)
        @($parts | Where-Object { $otherConfigs -contains $_ }).Count -eq 0
    })
    if ($neutral.Count -gt 0) { return $neutral[0].FullName }

    throw "No '$Name' built for configuration '$Configuration' (found: $($all.FullName -join ', '))"
}

function Get-MTCrtCMakeArgs {
    <#
    .SYNOPSIS
        CMake arguments that pin a dependency to the engine's CRT.

    .DESCRIPTION
        Every dependency staged into the caps libs directory is linked into the
        same image as the engine and the app, which build /MT (MultiThreaded)
        in Release and /MTd in Debug. CMP0091 must be NEW or
        CMAKE_MSVC_RUNTIME_LIBRARY is ignored outright and CMake falls back to
        its /MD default -- putting two CRTs, and so two heaps, in one binary
        and raising LNK4098.

        Note that a project can still overwrite CMAKE_MSVC_RUNTIME_LIBRARY from
        its own option after this is passed: opus does exactly that from
        OPUS_STATIC_RUNTIME. Where that happens the project's own option has to
        be set too.
    #>
    param([Parameter(Mandatory = $true)] [string] $Configuration)

    $crtLib = if ($Configuration -eq 'Debug') { 'MultiThreadedDebug' } else { 'MultiThreaded' }
    return @(
        "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=$crtLib"
    )
}

function Import-MTVCEnvironment {
    <#
    .SYNOPSIS
        Import a full VS developer environment (INCLUDE, LIB, LIBPATH, PATH)
        for the given target architecture. Returns $true if the environment is
        usable afterwards.

    .DESCRIPTION
        Add-MTVCToolsToPath puts cl.exe and lib.exe on PATH, which is enough to
        LAUNCH the compiler but not to COMPILE with it: the Windows SDK headers
        and import libraries are found through the INCLUDE and LIB environment
        variables, which only a "Developer PowerShell for VS 2022" or
        vcvarsall.bat sets. A build started from an ordinary shell, or through
        build-windows.sh, otherwise dies on the first #include with

            fatal error C1083: Cannot open include file: 'winsock2.h'

        The CMake-driven dependency scripts do not need this -- CMake locates
        the SDK itself -- so only the scripts that shell out to cl/lib directly
        do (build-usockets.ps1, and build-mbedtls.ps1's disabled-capability
        stub).

        No-ops when INCLUDE already names a Windows Kits directory, so running
        inside a real Developer prompt costs nothing and cannot be clobbered.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][ValidateSet('x64', 'ARM64')][string]$Platform,
        [string]$HostArch
    )

    if ($env:INCLUDE -and ($env:INCLUDE -match 'Windows Kits')) { return $true }

    if (-not $HostArch) { $HostArch = Get-MTHostArch }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $false }
    $vsInstall = & $vswhere -latest -property installationPath 2>$null | Select-Object -First 1
    if (-not $vsInstall) { return $false }

    $vcvarsall = Join-Path $vsInstall 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path $vcvarsall)) { return $false }

    # vcvarsall takes <host>_<target>, collapsing to a single token when they
    # match. Same host/target reasoning as Add-MTVCToolsToPath.
    $hostTok = $HostArch.ToLowerInvariant()
    $targetTok = $Platform.ToLowerInvariant()
    $archArg = if ($hostTok -eq $targetTok) { $targetTok } else { "${hostTok}_${targetTok}" }

    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $lines = & cmd.exe /c "call `"$vcvarsall`" $archArg >nul 2>&1 && set" 2>$null
        if ($LASTEXITCODE -ne 0 -and $hostTok -ne 'x64') {
            # An ARM64 host without native tools falls back to the emulated
            # x64-hosted cross compiler, exactly as Add-MTVCToolsToPath does.
            $archArg = "x64_$targetTok"
            $lines = & cmd.exe /c "call `"$vcvarsall`" $archArg >nul 2>&1 && set" 2>$null
        }
    } finally {
        $ErrorActionPreference = $savedEap
    }

    if (-not $lines) { return $false }

    foreach ($line in $lines) {
        if ($line -match '^(INCLUDE|LIB|LIBPATH|PATH)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
        }
    }

    Write-Host "Imported VC environment: vcvarsall $archArg"
    return ($env:INCLUDE -and ($env:INCLUDE -match 'Windows Kits'))
}
