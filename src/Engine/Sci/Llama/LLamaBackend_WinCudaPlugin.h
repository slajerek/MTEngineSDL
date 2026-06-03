#pragma once

#include "CLlamaTypes.h"

#include <string>

// Windows-only runtime-loaded CUDA backend.
// Loads a plugin DLL and forwards llama operations. Safe fallback when DLL is missing.

class LLamaBackend_WinCudaPlugin {
public:
	LLamaBackend_WinCudaPlugin();
	~LLamaBackend_WinCudaPlugin();

	bool IsAvailable() const;
	std::string GetBackendName() const;

	bool TryLoadModel(const std::string &modelPath, const MT_LlamaLoadParams &params, std::string *errorOut);
	void UnloadModel();
	bool HasModelLoaded() const;



	bool TryLoadModelAsync(const std::string &, const MT_LlamaLoadParams &) { return false; }
	bool IsLoadingModel() const { return false; }
	float GetLoadProgress() const { return 0.0f; }
	void CancelLoadModel() {}
	std::string GetLoadError() const { return ""; }

private:
	class Impl;
	Impl *impl;
};
