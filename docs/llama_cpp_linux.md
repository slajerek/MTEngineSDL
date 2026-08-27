# Linux: llama.cpp (CPU, CUDA, Vulkan)

MTEngineSDL integrates llama.cpp as a submodule under `other/lib/llama.cpp`.

## CMake configure options

- `-DMT_ENABLE_LLAMA_CPP=ON|OFF` (default ON)
- `-DMT_LLAMA_CUDA=ON` to enable CUDA backend (requires CUDA toolkit)
- `-DMT_LLAMA_VULKAN=ON` to enable Vulkan backend (requires Vulkan SDK / headers)

Example (CPU only):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMT_ENABLE_LLAMA_CPP=ON
cmake --build build -j
```

Example (CUDA):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMT_ENABLE_LLAMA_CPP=ON -DMT_LLAMA_CUDA=ON
cmake --build build -j
```

Example (Vulkan):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMT_ENABLE_LLAMA_CPP=ON -DMT_LLAMA_VULKAN=ON
cmake --build build -j
```

## Dependencies

CPU-only build depends only on standard build tools.

Recommended packages (Ubuntu/Debian names):

- `build-essential`
- `cmake` (>= 3.14)
- `pkg-config`

CUDA build:

- NVIDIA CUDA Toolkit (new enough for your driver)
- CMake must be able to find CUDA (via `CUDAToolkit` / `CUDA_PATH` / default install)

Vulkan build:

- Vulkan loader + headers (e.g. `libvulkan` + `vulkan-headers`)
- Shader compiler tools may be required by ggml-vulkan (e.g. `glslc` / `glslangValidator`)

On Ubuntu/Debian this often means:

- `libvulkan-dev`
- `vulkan-tools`
- `glslang-tools`

## Build artifact

On Linux we build llama.cpp targets and then pack them into a single archive: `libllama_cpp.a` in the build directory.
