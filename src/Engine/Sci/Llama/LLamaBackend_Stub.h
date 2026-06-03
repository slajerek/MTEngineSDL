#pragma once

#include "CLlamaTypes.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

class LLamaBackend_Stub {
public:
	bool IsAvailable() const { return false; }
	std::string GetBackendName() const { return "llama.cpp (disabled)"; }
	bool TryLoadModel(const std::string &, const MT_LlamaLoadParams &, std::string *errorOut);
	void UnloadModel() {}
	bool HasModelLoaded() const { return false; }

	bool IsGpuOffloadSupported() const { return false; }
	bool GenerateAsync(const std::string &prompt, const MT_LlamaGenerateParams &params,
	                   MT_LlamaTokenCallback tokenCb,
	                   std::function<void(MT_LlamaStopReason)> doneCb) { return false; }
	bool IsGenerating() const { return false; }
	void StopGeneration() {}
	void ClearContext() {}

	bool TryLoadModelAsync(const std::string &, const MT_LlamaLoadParams &) { return false; }
	bool IsLoadingModel() const { return false; }
	float GetLoadProgress() const { return 0.0f; }
	void CancelLoadModel() {}
	std::string GetLoadError() const { return ""; }

	std::string ApplyChatTemplate(const std::vector<std::pair<std::string,std::string>> &, bool) const { return ""; }
	std::string GetChatStopSequence() const { return ""; }
};
