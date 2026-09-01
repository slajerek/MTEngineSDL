# Windows builds: which architecture, and which build directory

Two failures on Windows-on-ARM share one cause — nobody agreed on what
architecture was being built. This is what the build scripts do about it.

Everything here lives in `platform/Windows/mt-build-common.ps1`, dot-sourced by
every Windows build script in this repo and by the app builds that call them
(`c64d/build-windows.ps1`, `tools/make-release/make-release-windows.ps1`).

## 1. Detecting the host architecture

**`$env:PROCESSOR_ARCHITECTURE` describes the PROCESS, not the machine, and
children inherit it.** Git Bash for Windows is an x64 build; on an ARM64 machine
it runs under emulation and reports `AMD64`, and so does every PowerShell it
launches, and every script those launch in turn. That is why

```
$ ./build-windows.sh
Auto-detected platform: x64
=== Building llama.cpp (x64 Release Clang) ===
```

on an ARM64 box. .NET is no better: under emulation
`[RuntimeInformation]::OSArchitecture` returns `X64` as well.

`Get-MTHostArch` reads the CPU identifier the kernel writes to
`HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor\0`. That value is not
redirected for emulated processes, so it answers for the **machine**:

| source                             | native ARM64 shell | emulated x64 shell on the same box |
| ---------------------------------- | ------------------ | ---------------------------------- |
| `$env:PROCESSOR_ARCHITECTURE`      | `ARM64`            | `AMD64`  ← lies                    |
| `RuntimeInformation::OSArchitecture` | `Arm64`          | `X64`    ← lies                    |
| registry CPU `Identifier`          | `ARMv8 (64-bit)…`  | `ARMv8 (64-bit)…`                  |

Use `Resolve-MTPlatform $Platform` in a script that takes a `-Platform`
parameter: it returns the explicit value when given one and auto-detects
otherwise. Use `Get-MTHostArch` when you specifically mean the host.

Passing `-Platform x64` on an ARM64 machine remains a supported cross build;
`c64d/build-windows.ps1` prints a line saying so rather than silently obeying.

## 1a. Finding the VC tools

`Add-MTVCToolsToPath -Platform <target>` puts `lib.exe` and `cl.exe` on `PATH`
and returns the directory it added. The dependency scripts shell out to `lib.exe`
to combine archives and to `cl.exe` for the mbedTLS stub, and those are on `PATH`
only inside a "Developer PowerShell for VS 2022" — so a build from an ordinary
shell, or through a `build-windows.sh` wrapper, has to resolve them itself or die
in the first dependency script with `Missing required tool 'lib'`.

The layout is `VC\Tools\MSVC\<ver>\bin\Host<host>\<target>`: the host half is
where the compiler RUNS, the target half is what it BUILDS. The function takes the
host from `Get-MTHostArch` and falls back to `Hostx64\<target>`, which every
install has and which runs under emulation on an ARM64 host. Calling it twice is
harmless — it will not stack duplicate entries on `PATH`.

App scripts that build dependencies call it right after they put MSBuild on
`PATH`: c64d, the photo app and MTEngineSDLDummyApp all do. the game app does not
build dependencies, so it does not call it.

## 2. Build directories are keyed by architecture

A CMake build directory belongs to exactly one configuration. Point a second one
at it and CMake does not reconfigure, it aborts:

```
CMake Error: Error: generator platform: x64
Does not match the platform used previously: ARM64
Either remove the CMakeCache.txt file and CMakeFiles directory or choose a
different binary directory.
```

So the names say which configuration they hold:

| dependency        | build directory                                    |
| ----------------- | -------------------------------------------------- |
| llama.cpp (CPU)   | `other/lib/llama.cpp/build-windows-cpu-<Platform>`   |
| llama.cpp (CUDA)  | `other/lib/llama.cpp/build-windows-cuda-<Platform>`  |
| FTXUI             | `other/lib/ftxui/build-windows-<Platform>`           |
| mbedTLS           | `other/lib/mbedtls.windows-<Platform>`               |
| image codecs      | `other/lib/image-codecs/build-win-<Platform>`        |
| video codecs      | `other/lib/video-codecs/build-win-<Platform>`        |

The llama.cpp pair used to be unkeyed (`build-windows-cpu`,
`build-windows-cuda`), which is what produced the error above the first time a
machine built the other architecture. Both scripts delete the unkeyed directory
if they find one.

## 3. `Reset-MTStaleCMakeCache`

Keying the directory by architecture does not cover everything that invalidates
a cache: a Visual Studio upgrade changes the generator, and `-Compiler
Clang|MSVC` changes the toolset. Each of those makes CMake abort exactly as
above.

Before configuring, every dependency script calls

```powershell
$null = Reset-MTStaleCMakeCache -BuildDir $buildDir -Platform $Platform -Generator $generator `
    -Toolset $cmakeToolsetName -SourceDir $srcDir -Label 'llama.cpp (CPU)'
```

It reads `CMakeCache.txt`, compares only the parameters the caller passed
(`CMAKE_GENERATOR_PLATFORM`, `CMAKE_GENERATOR`, `CMAKE_GENERATOR_TOOLSET`,
`CMAKE_HOME_DIRECTORY`), and on any mismatch removes the whole directory and
says why. It also removes a `CMakeFiles` directory left without a cache by an
interrupted configure. The scripts own these directories outright, so a wipe is
always the correct answer — the alternative is a build that cannot proceed.

`$cmakeToolsetName` is `'ClangCL'` for `-Compiler Clang` and `''` for MSVC,
which is also what CMake stores, so a Clang → MSVC switch wipes and reconfigures
rather than failing.

Note the `$null =`: the function returns whether it removed anything, and an
uncaptured `True` would land on the script's output stream.

## 4. Cleaning by hand

`c64d/build-windows.ps1 -Clean` removes the directories above for the
`$Platform` it was given, plus the legacy unkeyed llama.cpp and mbedTLS names.
It does not touch the image/video codec caches, which hold downloaded sources.

## Tests

`tests/test-windows-build-arch.ps1` covers both halves: that detection ignores a
spoofed `PROCESSOR_ARCHITECTURE` in either direction, and that a cache from
another platform, generator, toolset or source tree is removed while a matching
one is kept. It also checks that `Add-MTVCToolsToPath` resolves a real
`lib.exe` for both targets and does not duplicate `PATH` entries.

```powershell
pwsh -File tests\test-windows-build-arch.ps1
```
