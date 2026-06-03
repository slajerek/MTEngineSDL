#pragma once

// Simple C ABI for runtime-loaded CUDA backend on Windows.
// Kept intentionally minimal: current engine tests only verify availability and basic load/unload.

#include <stdbool.h>
#include <stdint.h>

#define MT_LLAMA_CUDA_PLUGIN_API_VERSION 1u

#if defined(_WIN32)
#define MT_LLAMA_CUDA_PLUGIN_EXPORT __declspec(dllexport)
#else
#define MT_LLAMA_CUDA_PLUGIN_EXPORT
#endif

typedef struct MT_LlamaCudaPluginApi_v1 {
	uint32_t api_version;

	const char *(*GetBackendName)(void);
	bool (*IsAvailable)(void);

	bool (*TryLoadModel)(const char *model_path, int32_t n_ctx, int32_t n_threads, char *error_out, int32_t error_out_size);
	void (*UnloadModel)(void);
	bool (*HasModelLoaded)(void);
} MT_LlamaCudaPluginApi_v1;

// Returns true on success and fills out_api.
typedef bool (*MT_LlamaCudaPluginGetApiFn)(uint32_t requested_api_version, MT_LlamaCudaPluginApi_v1 *out_api);
