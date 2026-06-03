#include "LLamaBackend_WinCudaPlugin.h"

#if defined(_WIN32)

#include "mt_llama_cuda_plugin_api.h"

#include "DBG_Log.h"
#include "SYS_FileSystem.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

static const char *kCudaPluginDllName = "mt_llama_cuda_backend.dll";

class LLamaBackend_WinCudaPlugin::Impl {
public:
	HMODULE dll = NULL;
	MT_LlamaCudaPluginApi_v1 api = {};
	bool apiLoaded = false;

	std::string dllPath;
};

static std::string NormalizeDir(std::string s)
{
	if (s.empty())
		return s;
	std::replace(s.begin(), s.end(), '/', '\\');
	while (!s.empty() && (s.back() == '\\'))
		s.pop_back();
	return s;
}

static std::string JoinPath(const std::string &dir, const std::string &leaf)
{
	if (dir.empty())
		return leaf;
	return dir + "\\" + leaf;
}

static std::vector<std::string> GetCandidateDllDirs()
{
	std::vector<std::string> dirs;

	// 1) Preferred env var
	if (const char *env = getenv("MT_LLAMA_DLL_DIR"))
		dirs.push_back(NormalizeDir(env));
	// 2) Compatibility env var
	if (const char *env = getenv("LH_LLAMA_DLL_DIR"))
		dirs.push_back(NormalizeDir(env));

	// 3) Data/lib, then lib, then exe dir (current directory in this engine)
	if (gCPathToCurrentDirectory)
	{
		std::string base = NormalizeDir(gCPathToCurrentDirectory);
		dirs.push_back(JoinPath(base, "Data\\lib"));
		dirs.push_back(JoinPath(base, "lib"));
		dirs.push_back(base);
	}

	// remove empties
	dirs.erase(std::remove_if(dirs.begin(), dirs.end(), [](const std::string &s) { return s.empty(); }), dirs.end());
	return dirs;
}

static HMODULE TryLoadDllFromSearch(const std::string &dllName, std::string *outPath)
{
	for (const std::string &dir : GetCandidateDllDirs())
	{
		std::string full = JoinPath(dir, dllName);
		if (SYS_FileExists(full.c_str()))
		{
			HMODULE h = LoadLibraryA(full.c_str());
			if (h)
			{
				if (outPath)
					*outPath = full;
				return h;
			}
		}
	}

	// Final attempt: let Windows search PATH / default locations.
	HMODULE h = LoadLibraryA(dllName.c_str());
	if (h && outPath)
		*outPath = dllName;
	return h;
}

LLamaBackend_WinCudaPlugin::LLamaBackend_WinCudaPlugin()
{
	impl = new Impl();

	std::string dllPath;
	impl->dll = TryLoadDllFromSearch(kCudaPluginDllName, &dllPath);
	impl->dllPath = dllPath;
	if (!impl->dll)
		return;

	auto getApiFn = (MT_LlamaCudaPluginGetApiFn)GetProcAddress(impl->dll, "MT_LlamaCudaPlugin_GetApi");
	if (!getApiFn)
		return;

	MT_LlamaCudaPluginApi_v1 api = {};
	if (!getApiFn(MT_LLAMA_CUDA_PLUGIN_API_VERSION, &api))
		return;
	if (api.api_version != MT_LLAMA_CUDA_PLUGIN_API_VERSION)
		return;
	if (!api.GetBackendName || !api.IsAvailable || !api.TryLoadModel || !api.UnloadModel || !api.HasModelLoaded)
		return;

	impl->api = api;
	impl->apiLoaded = true;
}

LLamaBackend_WinCudaPlugin::~LLamaBackend_WinCudaPlugin()
{
	UnloadModel();
	if (impl)
	{
		if (impl->dll)
			FreeLibrary(impl->dll);
		delete impl;
		impl = nullptr;
	}
}

bool LLamaBackend_WinCudaPlugin::IsAvailable() const
{
	if (!impl || !impl->apiLoaded)
		return false;
	return impl->api.IsAvailable();
}

std::string LLamaBackend_WinCudaPlugin::GetBackendName() const
{
	if (!impl || !impl->apiLoaded)
		return "llama.cpp (cuda plugin not loaded)";
	const char *n = impl->api.GetBackendName();
	return n ? std::string(n) : std::string("llama.cpp (cuda)");
}

bool LLamaBackend_WinCudaPlugin::TryLoadModel(const std::string &modelPath, const MT_LlamaLoadParams &params, std::string *errorOut)
{
	if (!impl || !impl->apiLoaded)
	{
		if (errorOut)
			*errorOut = "CUDA plugin not loaded";
		return false;
	}

	char errBuf[1024];
	errBuf[0] = 0;
	int32_t n_ctx = params.n_ctx;
	int32_t n_threads = params.n_threads;

	bool ok = impl->api.TryLoadModel(modelPath.c_str(), n_ctx, n_threads, errBuf, (int32_t)sizeof(errBuf));
	if (!ok && errorOut)
		*errorOut = errBuf[0] ? std::string(errBuf) : std::string("Failed to load model via CUDA plugin");
	return ok;
}

void LLamaBackend_WinCudaPlugin::UnloadModel()
{
	if (impl && impl->apiLoaded)
		impl->api.UnloadModel();
}

bool LLamaBackend_WinCudaPlugin::HasModelLoaded() const
{
	if (!impl || !impl->apiLoaded)
		return false;
	return impl->api.HasModelLoaded();
}

#else

LLamaBackend_WinCudaPlugin::LLamaBackend_WinCudaPlugin() { impl = nullptr; }
LLamaBackend_WinCudaPlugin::~LLamaBackend_WinCudaPlugin() {}
bool LLamaBackend_WinCudaPlugin::IsAvailable() const { return false; }
std::string LLamaBackend_WinCudaPlugin::GetBackendName() const { return "llama.cpp (cuda plugin unsupported)"; }
bool LLamaBackend_WinCudaPlugin::TryLoadModel(const std::string &, const MT_LlamaLoadParams &, std::string *errorOut)
{
	if (errorOut) *errorOut = "Unsupported platform";
	return false;
}
void LLamaBackend_WinCudaPlugin::UnloadModel() {}
bool LLamaBackend_WinCudaPlugin::HasModelLoaded() const { return false; }

#endif
