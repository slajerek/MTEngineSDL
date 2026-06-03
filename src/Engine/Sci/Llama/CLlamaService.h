#pragma once

#include "CLlamaTypes.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class CLlamaService {
public:
	CLlamaService();
	~CLlamaService();

	static bool IsCompiledIn();

	bool IsAvailable() const;
	std::string GetBackendName() const;

	bool TryLoadModel(const std::string &modelPath, const MT_LlamaLoadParams &params, std::string *errorOut);
	bool IsGpuOffloadSupported() const;
	void UnloadModel();
	bool HasModelLoaded() const;

	// Async model loading with progress callback.
	bool TryLoadModelAsync(const std::string &modelPath, const MT_LlamaLoadParams &params);
	bool IsLoadingModel() const;
	float GetLoadProgress() const;
	void CancelLoadModel();
	std::string GetLoadError() const;

	// Async text generation. Returns false if already generating or no model loaded.
	// tokenCb: called from background thread for each streamed token piece.
	// doneCb:  called from background thread when generation finishes.
	bool GenerateAsync(const std::string &prompt, const MT_LlamaGenerateParams &params,
	                   MT_LlamaTokenCallback tokenCb,
	                   std::function<void(MT_LlamaStopReason)> doneCb);

	// Higher-level async generation with structured segment callbacks.
	// segmentCb: called from background thread with each streamed text delta and its type
	//            (Thinking or Answer). May be null.
	// doneCb:    called from background thread when done, with the complete parse result.
	//            May be null.
	bool GenerateWithSegmentsAsync(const std::string &prompt,
	                               const MT_LlamaGenerateParams &params,
	                               MT_LlamaSegmentCallback segmentCb,
	                               std::function<void(MT_LlamaParseResult)> doneCb,
	                               bool startInThinkingMode = false);

	bool IsGenerating() const;

	// Stop generation and wait for the background thread to exit.
	void StopGeneration();

	// Clear the KV cache; resets incremental context. Do not call while generating.
	void ClearContext();

	// Format messages using the model's built-in chat template.
	// Returns formatted prompt, or empty string if no template available.
	std::string ApplyChatTemplate(const std::vector<std::pair<std::string,std::string>> &messages, bool addAssistantPrefix) const;

	// Returns the end-of-turn string for the model's chat template (e.g. "<|im_end|>").
	std::string GetChatStopSequence() const;

	// Returns true if the loaded model's template supports thinking mode.
	bool SupportsThinking() const;

	// Raw token log for diagnostics. Populated during generation.
	std::vector<std::pair<int32_t, std::string>> GetRawTokens();
	void ClearRawTokens();

	// System prompt — set by model loader, read by chat view.
	void SetSystemPrompt(const std::string &prompt) { systemPrompt = prompt; }
	const std::string &GetSystemPrompt() const { return systemPrompt; }

	// When false, the system prompt is session-only and not saved to config.
	void SetPersistSystemPrompt(bool persist) { persistSystemPrompt = persist; }
	bool GetPersistSystemPrompt() const { return persistSystemPrompt; }

	// Thinking mode — when true, models that support thinking will generate <think> blocks.
	void SetEnableThinking(bool enable) { enableThinking = enable; }
	bool GetEnableThinking() const { return enableThinking; }

private:
	class Impl;
	std::unique_ptr<Impl> impl;
	std::string systemPrompt;
	bool persistSystemPrompt = true;
	bool enableThinking = true;
};
