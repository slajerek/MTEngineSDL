<#
.SYNOPSIS
    Tests for platform\Windows\mt-build-common.ps1.
.DESCRIPTION
    Two things are asserted here, both of which broke a real build:

      1. Host-architecture detection ignores $env:PROCESSOR_ARCHITECTURE.
         That variable belongs to the PROCESS and is inherited, so Git Bash --
         an emulated x64 build on Windows-on-ARM -- handed "AMD64" to every
         build script below it and an ARM64 machine built x64.

      2. A CMake build directory configured for another platform, generator or
         toolset is REMOVED rather than reused. CMake will not reconfigure such
         a directory; it aborts with "Does not match the platform used
         previously", which is a dead end inside an automated build.

    Run:  pwsh -File tests\test-windows-build-arch.ps1
#>

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'platform\Windows\mt-build-common.ps1')

$failures = 0
function Check([string]$what, [scriptblock]$assertion) {
    try {
        $ok = & $assertion
        if ($ok) { Write-Host "  ok   $what" -ForegroundColor Green }
        else { Write-Host "  FAIL $what" -ForegroundColor Red; $script:failures++ }
    } catch {
        Write-Host "  FAIL $what -- $($_.Exception.Message)" -ForegroundColor Red
        $script:failures++
    }
}

function New-FakeCMakeBuildDir {
    param([string]$Platform, [string]$Generator, [string]$Toolset, [string]$SourceDir)
    $dir = Join-Path ([System.IO.Path]::GetTempPath()) ("mt-arch-test-" + [System.Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'CMakeFiles') | Out-Null
    @(
        "CMAKE_GENERATOR:INTERNAL=$Generator",
        "CMAKE_GENERATOR_PLATFORM:INTERNAL=$Platform",
        "CMAKE_GENERATOR_TOOLSET:INTERNAL=$Toolset",
        "CMAKE_HOME_DIRECTORY:INTERNAL=$($SourceDir -replace '\\','/')"
    ) | Set-Content -Path (Join-Path $dir 'CMakeCache.txt') -Encoding UTF8
    return $dir
}

$src = 'C:\develop\MTEngineSDL\other\lib\llama.cpp'
$gen = 'Visual Studio 17 2022'

Write-Host "Get-MTHostArch"

Check 'reports a supported architecture' {
    (Get-MTHostArch) -in @('x64', 'ARM64')
}

Check 'ignores a spoofed $env:PROCESSOR_ARCHITECTURE' {
    $real = Get-MTHostArch
    $saved = $env:PROCESSOR_ARCHITECTURE
    $savedWow = $env:PROCESSOR_ARCHITEW6432
    try {
        # Both directions: whichever this machine really is, claiming the other
        # in the environment must not change the answer.
        $env:PROCESSOR_ARCHITECTURE = 'AMD64'
        $env:PROCESSOR_ARCHITEW6432 = ''
        $spoofedX64 = Get-MTHostArch
        $env:PROCESSOR_ARCHITECTURE = 'ARM64'
        $spoofedArm = Get-MTHostArch
        ($spoofedX64 -eq $real) -and ($spoofedArm -eq $real)
    } finally {
        $env:PROCESSOR_ARCHITECTURE = $saved
        $env:PROCESSOR_ARCHITEW6432 = $savedWow
    }
}

Check 'Resolve-MTPlatform honours an explicit platform' {
    (Resolve-MTPlatform 'ARM64') -eq 'ARM64' -and (Resolve-MTPlatform 'x64') -eq 'x64'
}

Check 'Resolve-MTPlatform falls back to the host' {
    (Resolve-MTPlatform '') -eq (Get-MTHostArch)
}

Write-Host "Add-MTVCToolsToPath"

# Skipped rather than failed where Visual Studio is absent (a Linux/macOS
# checkout, a bare CI image): the function returns $null there by design.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Host "  skip Visual Studio not installed" -ForegroundColor Yellow
} else {
    Check 'puts a lib.exe for the target on PATH' {
        $saved = $env:PATH
        try {
            $dir = Add-MTVCToolsToPath -Platform (Get-MTHostArch)
            $dir -and (Test-Path (Join-Path $dir 'lib.exe'))
        } finally { $env:PATH = $saved }
    }

    Check 'resolves both targets' {
        $saved = $env:PATH
        try {
            $x64 = Add-MTVCToolsToPath -Platform 'x64'
            $arm = Add-MTVCToolsToPath -Platform 'ARM64'
            # An install may lack one of the two cross toolchains; whatever it
            # returns must at least end in the target it was asked for.
            (-not $x64 -or $x64.EndsWith('\x64')) -and (-not $arm -or $arm.EndsWith('\ARM64'))
        } finally { $env:PATH = $saved }
    }

    Check 'does not stack duplicate PATH entries' {
        $saved = $env:PATH
        try {
            $dir = Add-MTVCToolsToPath -Platform (Get-MTHostArch)
            $null = Add-MTVCToolsToPath -Platform (Get-MTHostArch)
            (($env:PATH -split ';') | Where-Object { $_ -eq $dir }).Count -eq 1
        } finally { $env:PATH = $saved }
    }
}

Write-Host "Reset-MTStaleCMakeCache"

Check 'keeps a matching cache' {
    $dir = New-FakeCMakeBuildDir -Platform 'x64' -Generator $gen -Toolset 'ClangCL' -SourceDir $src
    try {
        $removed = Reset-MTStaleCMakeCache -BuildDir $dir -Platform 'x64' -Generator $gen -Toolset 'ClangCL' -SourceDir $src
        (-not $removed) -and (Test-Path $dir)
    } finally { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}

Check 'removes a cache configured for another architecture' {
    # The reported failure verbatim: an ARM64 cache, an x64 build.
    $dir = New-FakeCMakeBuildDir -Platform 'ARM64' -Generator $gen -Toolset 'ClangCL' -SourceDir $src
    try {
        $removed = Reset-MTStaleCMakeCache -BuildDir $dir -Platform 'x64' -Generator $gen -Toolset 'ClangCL' -SourceDir $src
        $removed -and (-not (Test-Path $dir))
    } finally { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}

Check 'removes a cache configured with another toolset' {
    $dir = New-FakeCMakeBuildDir -Platform 'x64' -Generator $gen -Toolset 'ClangCL' -SourceDir $src
    try {
        $removed = Reset-MTStaleCMakeCache -BuildDir $dir -Platform 'x64' -Generator $gen -Toolset '' -SourceDir $src
        $removed -and (-not (Test-Path $dir))
    } finally { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}

Check 'removes a cache configured with another generator' {
    $dir = New-FakeCMakeBuildDir -Platform 'x64' -Generator 'Visual Studio 16 2019' -Toolset '' -SourceDir $src
    try {
        $removed = Reset-MTStaleCMakeCache -BuildDir $dir -Platform 'x64' -Generator $gen -Toolset '' -SourceDir $src
        $removed -and (-not (Test-Path $dir))
    } finally { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}

Check 'removes a cache pointing at another source tree' {
    $dir = New-FakeCMakeBuildDir -Platform 'x64' -Generator $gen -Toolset '' -SourceDir 'C:\elsewhere\llama.cpp'
    try {
        $removed = Reset-MTStaleCMakeCache -BuildDir $dir -Platform 'x64' -Generator $gen -Toolset '' -SourceDir $src
        $removed -and (-not (Test-Path $dir))
    } finally { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}

Check 'accepts the source path however it is spelled' {
    # CMake writes forward slashes; the build scripts pass backslashes.
    $dir = New-FakeCMakeBuildDir -Platform 'x64' -Generator $gen -Toolset '' -SourceDir $src
    try {
        $removed = Reset-MTStaleCMakeCache -BuildDir $dir -Platform 'x64' -Generator $gen -Toolset '' -SourceDir ($src.ToUpper() + '\')
        (-not $removed) -and (Test-Path $dir)
    } finally { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}

Check 'removes an interrupted configure (CMakeFiles, no cache)' {
    $dir = New-FakeCMakeBuildDir -Platform 'x64' -Generator $gen -Toolset '' -SourceDir $src
    Remove-Item -Force (Join-Path $dir 'CMakeCache.txt')
    try {
        $removed = Reset-MTStaleCMakeCache -BuildDir $dir -Platform 'x64' -Generator $gen -Toolset '' -SourceDir $src
        $removed -and (-not (Test-Path $dir))
    } finally { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}

Check 'ignores a directory that was never configured' {
    $dir = Join-Path ([System.IO.Path]::GetTempPath()) ("mt-arch-test-" + [System.Guid]::NewGuid().ToString('N'))
    (-not (Reset-MTStaleCMakeCache -BuildDir $dir -Platform 'x64' -Generator $gen))
}

Check 'compares only what the caller passes' {
    $dir = New-FakeCMakeBuildDir -Platform 'x64' -Generator 'Visual Studio 16 2019' -Toolset 'ClangCL' -SourceDir $src
    try {
        $removed = Reset-MTStaleCMakeCache -BuildDir $dir -Platform 'x64'
        (-not $removed) -and (Test-Path $dir)
    } finally { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}

Write-Host ""
if ($failures -gt 0) {
    Write-Host "$failures check(s) FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "All checks passed" -ForegroundColor Green
