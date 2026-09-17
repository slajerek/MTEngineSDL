# Windows: llama.cpp CUDA Plugin

This repo supports a Windows-only CUDA backend via a runtime-loaded plugin DLL.
It is x64 only: Windows-on-ARM has no CUDA, and the driver forces `-SkipCuda`
for an ARM64 target.

See also: `docs/llama_cpp_windows.md` (CPU + CUDA end-to-end).

## What gets loaded

The engine looks for `mt_llama_cuda_backend.dll` in this order:

1. `MT_LLAMA_DLL_DIR`
2. `LH_LLAMA_DLL_DIR` (compat)
3. `Data\\lib` (relative to current working directory)
4. `lib` (relative)
5. current directory

If the DLL is missing or fails to load, the engine falls back to the CPU backend
(if compiled in). **One exe serves both audiences**: a machine without an NVIDIA
GPU runs the CPU backend, a machine with one loads the plugin. Nothing about
the exe differs.

## The toolkit: acquired, not installed

NVIDIA's Windows CUDA installer is x86_64-only and refuses an ARM64 host
outright -- *"CUDA toolkit must be installed on an amd64 computer"* -- and this
repository's Windows build machine is Windows 11 ARM64 under Parallels. But
NVIDIA also publishes the toolkit **component by component** at
`https://developer.download.nvidia.com/compute/cuda/redist/`: plain zips, a
per-version manifest with a sha256 for each, no installer and no OS check. An
x64 `nvcc.exe` runs under Windows' ARM64 emulation.

Measured on that machine, 2026-09-07: nvcc 12.9.86 drove `HostARM64\x64\cl.exe`
-- the same cross compiler every x64 build here already uses -- to PTX and to a
multi-arch object, and then configured and built the whole of ggml-cuda.
**Building needs no GPU**; nvcc compiles offline.

```powershell
.\platform\Windows\acquire-cuda-toolkit.ps1        # once per machine, ~2 GB
```

It fetches eight pinned components (nvcc, cudart, cccl, cublas, nvrtc,
nvjitlink, profiler_api, cuobjdump), verifies each against the manifest's
sha256 -- two were cross-checked against independently downloaded files before
the table was trusted -- and assembles them into one `CUDA_PATH`-shaped tree at
`<cache root>\toolchains\cuda-<version>\`, outside every checkout. A stamp
records the version, every hash and the script's own hash; a matching stamp is
a no-op. The `nvcc --version` smoke test runs before the stamp is written, so
the stamp means "executes on this host", not "the copy loop ran".

**The version is pinned once**, `MT_CUDA_TOOLKIT_VERSION` in
`mt-build-common.ps1`, so the directory the build reads and the archives the
acquisition fetches cannot disagree. It is **12.9.2, deliberately not 13.x**:
CUDA 13 dropped Maxwell, Pascal and Volta, which is why ggml gates
`50/61/70-virtual` on `CUDA < 13`. 12.9 keeps them and has Blackwell. Widest
coverage for a plugin loaded on other people's cards.

**How the build finds it.** `Resolve-MTCudaPath` returns `CUDA_PATH` if that
names a tree with `bin\nvcc.exe` -- a developer with a real installation is
left alone and their toolkit wins -- else the acquired tree if its stamp is
present, else nothing, in which case the driver skips CUDA with a note. The
answer is written back to `$env:CUDA_PATH`, because MSBuild reads environment
variables as properties and the plugin project links `$(CUDA_PATH)\lib\x64`.

## Building

Prereqs: Visual Studio 2022 (Desktop C++), CMake, the toolkit above. Nothing
else; in particular **not** NVIDIA's Visual Studio integration.

Through the driver it is automatic for an x64 build with `MT_CAP_LLM=1`:

```powershell
.\build-windows.ps1 -Platform x64                  # any app; CUDA when a toolkit resolves
```

Standalone, from `platform\Windows\`:

```powershell
.\build-llama_cpp_cuda.ps1 -Configuration Release -Platform x64 -OutLibDir <view>
```

What `build-llama_cpp_cuda.ps1` does, and why each part is the way it is:

- **Ninja, not the Visual Studio generator.** CMake's CUDA support under the
  VS generator emits projects that build `.cu` through the `CUDA 12.x.targets`
  build customization, which NVIDIA's installer drops into MSBuild and nothing
  else provides -- the redistributables do not carry it, and on the ARM64 box
  the installer will not run. Ninja drives nvcc directly. Visual Studio bundles
  a `ninja.exe`; `Get-MTNinja` finds it. Ninja is single-config, so
  `CMAKE_BUILD_TYPE` at configure time and no `<Configuration>\` subdirectory
  in the output tree.
- **`Import-MTVCEnvironment`** first: a Ninja build has no vcvars of its own,
  and CMake needs `cl.exe`, `INCLUDE` and `LIB` already in the process. On an
  ARM64 host that selects `HostARM64\x64`.
- **`GGML_NATIVE=OFF` is the point.** With it on, ggml resolves
  `CMAKE_CUDA_ARCHITECTURES` to `native` -- the GPUs present at build time --
  and the build machine has none; measured, that yielded
  `CMAKE_CUDA_ARCHITECTURES_NATIVE=No CUDA devices found.-real`. Off, ggml
  applies its maintained portable list, which for 12.9 is

      50-virtual;61-virtual;70-virtual;75-virtual;80-virtual;86-real;89-real;120a-real;121a-real

  `-virtual` is PTX, JIT-compiled by the driver on first run, so cards newer
  than the toolkit still work; `-real` is native code for the common ones.
  The list is **not pinned in the script on purpose**: it tracks upstream when
  the submodule bumps, and the verification reads back whatever was chosen.
- **The architectures are proved, not assumed.** Nobody here can run the
  plugin on a GPU, so "it built" would otherwise be the only evidence of which
  cards it runs on. After the build, `cuobjdump --list-elf` / `--list-ptx` on
  `ggml-cuda.lib` must show one cubin per `-real` entry and one PTX per
  `-virtual` entry of the list read back from `CMakeCache.txt`. A missing one
  fails the build with *"Do not ship this."* The list is also written to
  `llama_cpp_cuda.archs` beside the archives.
- **Expect it to be long.** ggml-cuda is ~300 Ninja steps, each `.cu` compiled
  for every architecture above, by an nvcc that is itself running under
  emulation. `MT_BUILD_JOBS` is honoured (nvcc is memory-hungry; the box has
  8 GB) and is worth setting to 2 there.
- The CRT is pinned to the engine's `/MT` via `Get-MTCrtCMakeArgs`, and every
  path handed to CMake is forward-slashed -- the `\U` lesson from
  `build-opencv.ps1`.

`build-llama-cpp.ps1` then builds `mt_llama_cuda_backend.vcxproj` -- a plain
C++ project that links the archives and `$(CUDA_PATH)\lib\x64`; it compiles no
`.cu`, which is why the redistributables suffice for it. It is built with
`MTBuildRoot` named explicitly, under the llama work dir, so the DLL lands
where the script says: R0.3 had routed the project's `OutDir` under
`$(MTBuildRoot)` while the script went on looking in the engine's own
`platform\Windows\bin`, where nothing had put it since.

## Runtime packaging

The plugin links `cudart`, `cublas` and `cublasLt` as **import libraries**, so
on a user's machine it needs those DLLs beside it -- and cublas pulls
cublasLt, and CUDA 12's cublasLt reaches for nvrtc and nvJitLink. Ship only
the plugin and a user with a real GPU gets a load failure and a *silent*
fall-back to CPU, which is what the deploy did until 2026-09-07.

**What ships is read from the binary, not written by hand.** After the plugin
links, `build-llama-cpp.ps1` walks its import table with `dumpbin /dependents`,
transitively, keeping every DLL that exists in the toolkit's `bin\`. That rule
admits exactly the runtime and excludes `nvcuda.dll` -- the driver, which is
the user's and is never shipped -- and the system DLLs. The result is staged
as one directory, `<libs>\cuda-runtime\`, and printed in the build log; the
driver's `-Prod` deploy copies it wholesale into the package's `Data\lib\`.
A hand-written list would be wrong at the next toolkit bump and nobody here
could run the plugin to notice.

**Licensing.** The NVIDIA CUDA EULA permits redistribution of the runtime
libraries named in its redistributable-components attachment (cudart, cublas,
cublasLt, nvrtc, nvJitLink are among them). Each component's licence text is
kept at `<toolkit>\licenses\<component>-LICENSE.txt`.
**OPEN:** the capability vocabulary does not yet list the CUDA runtime as a
dependency of `MT_CAP_LLM`, so a package that ships the plugin has a
`LICENSES.txt` that does not mention NVIDIA. That needs a vocabulary entry
before such a package is distributed.

## Verifying

`tests/run_test.sh --package` from the package proves the exe and the CPU
path. It cannot prove the plugin: the build machine has no NVIDIA GPU, so
`IsAvailable()` is false there and the engine falls back to CPU exactly as
designed. What *can* be verified without a GPU, and is: the architectures
baked into the binary (`cuobjdump`, above) and the completeness of the runtime
beside it (`dumpbin`, above). The first load on a real card is still an
untested claim until someone does it, and the AI Settings panel's
`Backend:`/`Available:` line is where they will see the answer.
