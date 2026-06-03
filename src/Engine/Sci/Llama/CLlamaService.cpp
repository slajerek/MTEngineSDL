#include "CLlamaService.h"

#include "CLlamaStreamParser.h"
#include "LLamaBackend_Stub.h"

#if defined(_WIN32)
#include "LLamaBackend_WinCudaPlugin.h"
#endif

#if !defined(MT_ENABLE_LLAMA_CPP) || (MT_ENABLE_LLAMA_CPP)
#include "LLamaBackend_LlamaCpp.h"
#endif

class CLlamaService::Impl {
public:
	Impl()
	{
	}

	LLamaBackend_Stub stub;

#if defined(_WIN32)
	LLamaBackend_WinCudaPlugin cudaPlugin;
#endif

#if !defined(MT_ENABLE_LLAMA_CPP) || (MT_ENABLE_LLAMA_CPP)
	LLamaBackend_LlamaCpp llama;
#endif
};

CLlamaService::CLlamaService()
{
	impl = std::make_unique<Impl>();
}

CLlamaService::~CLlamaService() = default;

bool CLlamaService::IsCompiledIn()
{
	#if !defined(MT_ENABLE_LLAMA_CPP) || (MT_ENABLE_LLAMA_CPP)
	return true;
	#else
	return false;
	#endif
}

bool CLlamaService::IsAvailable() const
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.IsAvailable();
#else
	#if defined(_WIN32)
	if (impl->cudaPlugin.IsAvailable())
		return true;
	#endif
	return impl->llama.IsAvailable();
#endif
}

std::string CLlamaService::GetBackendName() const
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.GetBackendName();
#else
	#if defined(_WIN32)
	if (impl->cudaPlugin.IsAvailable())
		return impl->cudaPlugin.GetBackendName();
	#endif
	return impl->llama.GetBackendName();
#endif
}

bool CLlamaService::IsGpuOffloadSupported() const
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return false;
#else
	return impl->llama.IsGpuOffloadSupported();
#endif
}

bool CLlamaService::TryLoadModel(const std::string &modelPath, const MT_LlamaLoadParams &params, std::string *errorOut)
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.TryLoadModel(modelPath, params, errorOut);
#else
	#if defined(_WIN32)
	if (impl->cudaPlugin.IsAvailable())
		return impl->cudaPlugin.TryLoadModel(modelPath, params, errorOut);
	#endif
	return impl->llama.TryLoadModel(modelPath, params, errorOut);
#endif
}

void CLlamaService::UnloadModel()
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	impl->stub.UnloadModel();
#else
	#if defined(_WIN32)
	if (impl->cudaPlugin.IsAvailable())
		impl->cudaPlugin.UnloadModel();
	else
	#endif
	impl->llama.UnloadModel();
#endif
}

bool CLlamaService::HasModelLoaded() const
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.HasModelLoaded();
#else
	#if defined(_WIN32)
	if (impl->cudaPlugin.HasModelLoaded())
		return true;
	#endif
	return impl->llama.HasModelLoaded();
#endif
}

bool CLlamaService::TryLoadModelAsync(const std::string &modelPath, const MT_LlamaLoadParams &params)
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.TryLoadModelAsync(modelPath, params);
#else
	return impl->llama.TryLoadModelAsync(modelPath, params);
#endif
}

bool CLlamaService::IsLoadingModel() const
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.IsLoadingModel();
#else
	return impl->llama.IsLoadingModel();
#endif
}

float CLlamaService::GetLoadProgress() const
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.GetLoadProgress();
#else
	return impl->llama.GetLoadProgress();
#endif
}

void CLlamaService::CancelLoadModel()
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	impl->stub.CancelLoadModel();
#else
	impl->llama.CancelLoadModel();
#endif
}

std::string CLlamaService::GetLoadError() const
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.GetLoadError();
#else
	return impl->llama.GetLoadError();
#endif
}

bool CLlamaService::GenerateAsync(const std::string &prompt, const MT_LlamaGenerateParams &params,
                                   MT_LlamaTokenCallback tokenCb, std::function<void(MT_LlamaStopReason)> doneCb)
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.GenerateAsync(prompt, params, tokenCb, doneCb);
#else
	return impl->llama.GenerateAsync(prompt, params, tokenCb, doneCb);
#endif
}

bool CLlamaService::IsGenerating() const
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.IsGenerating();
#else
	return impl->llama.IsGenerating();
#endif
}

void CLlamaService::StopGeneration()
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	impl->stub.StopGeneration();
#else
	impl->llama.StopGeneration();
#endif
}

void CLlamaService::ClearContext()
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	impl->stub.ClearContext();
#else
	impl->llama.ClearContext();
#endif
}

std::string CLlamaService::ApplyChatTemplate(const std::vector<std::pair<std::string,std::string>> &messages, bool addAssistantPrefix) const
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.ApplyChatTemplate(messages, addAssistantPrefix);
#else
	return impl->llama.ApplyChatTemplate(messages, addAssistantPrefix);
#endif
}

std::string CLlamaService::GetChatStopSequence() const
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return impl->stub.GetChatStopSequence();
#else
	return impl->llama.GetChatStopSequence();
#endif
}

bool CLlamaService::SupportsThinking() const
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return false;
#else
	return impl->llama.SupportsThinking();
#endif
}

std::vector<std::pair<int32_t, std::string>> CLlamaService::GetRawTokens()
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
	return {};
#else
	return impl->llama.GetRawTokens();
#endif
}

void CLlamaService::ClearRawTokens()
{
#if defined(MT_ENABLE_LLAMA_CPP) && !(MT_ENABLE_LLAMA_CPP)
#else
	impl->llama.ClearRawTokens();
#endif
}

bool CLlamaService::GenerateWithSegmentsAsync(const std::string &prompt,
                                               const MT_LlamaGenerateParams &params,
                                               MT_LlamaSegmentCallback segmentCb,
                                               std::function<void(MT_LlamaParseResult)> doneCb,
                                               bool startInThinkingMode)
{
	auto parser = std::make_shared<CLlamaStreamParser>();
	parser->SetStartInThinkingMode(startInThinkingMode);
	return GenerateAsync(
		prompt, params,
		[parser, segmentCb](const std::string &token) {
			parser->Feed(token, segmentCb);
		},
		[parser, segmentCb, doneCb](MT_LlamaStopReason reason) {
			MT_LlamaParseResult result = parser->Finish(segmentCb);
			result.stopReason = reason;
			if (doneCb) doneCb(std::move(result));
		}
	);
}
