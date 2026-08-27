# Windows: llama.cpp CUDA Plugin

This repo supports a Windows-only CUDA backend via a runtime-loaded plugin DLL.

See also: `docs/llama_cpp_windows.md` (CPU + CUDA end-to-end).

## What gets loaded

The engine looks for `mt_llama_cuda_backend.dll` in this order:

1. `MT_LLAMA_DLL_DIR`
2. `LH_LLAMA_DLL_DIR` (compat)
3. `Data\\lib` (relative to current working directory)
4. `lib` (relative)
5. current directory

If the DLL is missing or fails to load, the engine falls back to the CPU backend (if compiled in).

## Building

Prereqs:

- Visual Studio 2022 (Desktop development with C++)
- CMake
- NVIDIA CUDA Toolkit (`CUDA_PATH` must be set)

Build steps:

1) Initialize submodules:

```powershell
git submodule update --init --recursive
```

2) Build upstream llama.cpp CUDA libs (from **Developer PowerShell for VS 2022**):

```powershell
./platform/Windows/build-llama_cpp_cuda.ps1 -Config Release -Platform x64
```

3) Open `platform/Windows/MTEngineSDL.sln` and build the `mt_llama_cuda_backend` project (x64).

Notes:

- The plugin project links against `llama.lib`, `ggml.lib`, `ggml-base.lib`, `ggml-cuda.lib` and CUDA libs.
- `build-llama_cpp_cuda.ps1` copies those libs into: `platform/Windows/libs/x64/<Config>/`

## Runtime packaging

Ship the following next to the app (recommended location):

- `Data\\lib\\mt_llama_cuda_backend.dll`
- any CUDA runtime DLL dependencies required by your build (CUDA, cuBLAS, etc.)
