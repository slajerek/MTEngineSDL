# Windows: llama.cpp (CPU + CUDA Plugin)

This repo supports:

- CPU backend linked directly into the engine (`llama_cpp.lib`)
- optional CUDA backend as a runtime-loaded plugin DLL (`mt_llama_cuda_backend.dll`)

If the CUDA plugin DLL is missing or fails to load, the engine falls back to CPU.

## Prerequisites (download/install)

Required:

- Visual Studio 2022 (Desktop development with C++)
- Git (for submodules)
- CMake (install from cmake.org, or via Visual Studio)

For CUDA backend:

- NVIDIA driver compatible with your GPU
- NVIDIA CUDA Toolkit (newest stable; ensure `CUDA_PATH` is set)

## One-time setup

```powershell
git submodule update --init --recursive
```

## Build: CPU libraries from source

1. Open **Developer PowerShell for VS 2022**.
2. From repo root, run:

```powershell
./platform/Windows/build-llama_cpp_cpu.ps1 -Config Release -Platform x64
```

This creates:

- `platform/Windows/libs/x64/Release/llama_cpp.lib`

Then you can build `platform/Windows/MTEngineSDL.sln` normally.

## Build: CPU + CUDA (one command)

From **Developer PowerShell for VS 2022**:

```powershell
./platform/Windows/build-llama-cpp.ps1 -Config Release -Platform x64
```

If you only want CPU (no CUDA toolkit installed):

```powershell
./platform/Windows/build-llama-cpp.ps1 -Config Release -Platform x64 -SkipCuda
```

## Build: CUDA plugin from source

1. Build upstream llama.cpp CUDA libs (also from **Developer PowerShell for VS 2022**):

```powershell
./platform/Windows/build-llama_cpp_cuda.ps1 -Config Release -Platform x64
```

This copies these libs into `platform/Windows/libs/x64/Release/`:

- `llama.lib`, `ggml.lib`, `ggml-base.lib`, `ggml-cpu.lib`, `ggml-cuda.lib`

2. Open `platform/Windows/MTEngineSDL.sln` and build the `mt_llama_cuda_backend` project (x64).

## Runtime DLL search paths

The engine looks for `mt_llama_cuda_backend.dll` in this order:

1. `MT_LLAMA_DLL_DIR`
2. `LH_LLAMA_DLL_DIR` (compat)
3. `Data\\lib` (relative to current working directory)
4. `lib`
5. current directory

Recommended packaging location:

- `Data\\lib\\mt_llama_cuda_backend.dll`

Plus any CUDA runtime DLL dependencies required by your build.
