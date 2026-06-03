#include "mt_llama_cuda_plugin_api.h"

#include <cstring>
#include <mutex>

#include "llama.h"

static std::once_flag s_once;
static llama_model *s_model = nullptr;
static llama_context *s_ctx = nullptr;

static void UnloadModel();

static void set_err(char *out, int32_t out_size, const char *msg)
{
	if (!out || out_size <= 0)
		return;
	out[0] = 0;
	if (!msg)
		return;
	strncpy(out, msg, (size_t)out_size - 1);
	out[out_size - 1] = 0;
}

static const char *BackendName()
{
	return "llama.cpp (cuda plugin)";
}

static bool IsAvailable()
{
	// If DLL loaded, assume CUDA backend is usable. Actual failure is reported in TryLoadModel.
	return true;
}

static bool TryLoadModel(const char *model_path, int32_t n_ctx, int32_t n_threads, char *error_out, int32_t error_out_size)
{
	UnloadModel();

	std::call_once(s_once, []() {
		llama_backend_init();
	});

	llama_model_params mparams = llama_model_default_params();
	llama_context_params cparams = llama_context_default_params();

	if (n_ctx > 0)
		cparams.n_ctx = (uint32_t)n_ctx;
	if (n_threads > 0)
		cparams.n_threads = (uint32_t)n_threads;

	s_model = llama_model_load_from_file(model_path, mparams);
	if (!s_model)
	{
		set_err(error_out, error_out_size, "Failed to load model");
		return false;
	}

	s_ctx = llama_init_from_model(s_model, cparams);
	if (!s_ctx)
	{
		llama_model_free(s_model);
		s_model = nullptr;
		set_err(error_out, error_out_size, "Failed to init context from model");
		return false;
	}

	return true;
}

static void UnloadModel()
{
	if (s_ctx)
	{
		llama_free(s_ctx);
		s_ctx = nullptr;
	}
	if (s_model)
	{
		llama_model_free(s_model);
		s_model = nullptr;
	}
}

static bool HasModelLoaded()
{
	return s_model != nullptr && s_ctx != nullptr;
}

extern "C" MT_LLAMA_CUDA_PLUGIN_EXPORT bool MT_LlamaCudaPlugin_GetApi(uint32_t requested_api_version, MT_LlamaCudaPluginApi_v1 *out_api)
{
	if (!out_api)
		return false;
	if (requested_api_version != MT_LLAMA_CUDA_PLUGIN_API_VERSION)
		return false;

	out_api->api_version = MT_LLAMA_CUDA_PLUGIN_API_VERSION;
	out_api->GetBackendName = &BackendName;
	out_api->IsAvailable = &IsAvailable;
	out_api->TryLoadModel = &TryLoadModel;
	out_api->UnloadModel = &UnloadModel;
	out_api->HasModelLoaded = &HasModelLoaded;
	return true;
}
