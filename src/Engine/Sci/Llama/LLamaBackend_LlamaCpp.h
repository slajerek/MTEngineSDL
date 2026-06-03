#pragma once

#include "CLlamaTypes.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct llama_model;
struct llama_context;
struct llama_sampler;

class LLamaBackend_LlamaCpp {
public:
	LLamaBackend_LlamaCpp();
	~LLamaBackend_LlamaCpp();

	bool IsAvailable() const;
	std::string GetBackendName() const;

	bool TryLoadModel(const std::string &modelPath, const MT_LlamaLoadParams &params, std::string *errorOut);
	bool IsGpuOffloadSupported() const;
	void UnloadModel();
	bool HasModelLoaded() const;

	// Async model loading — loads in background thread with progress.
	bool TryLoadModelAsync(const std::string &modelPath, const MT_LlamaLoadParams &params);
	bool IsLoadingModel() const;
	float GetLoadProgress() const;
	void CancelLoadModel();
	std::string GetLoadError() const;

	// Async token generation. Returns false if already generating or no model loaded.
	// tokenCb is called from the background thread for each generated token piece.
	// doneCb is called when generation is complete (also from background thread).
	bool GenerateAsync(const std::string &prompt, const MT_LlamaGenerateParams &params,
	                   MT_LlamaTokenCallback tokenCb,
	                   std::function<void(MT_LlamaStopReason)> doneCb);

	bool IsGenerating() const;

	// Signal stop and block until the inference thread exits.
	void StopGeneration();

	// Clear the KV cache and reset n_past. Must not be called while generating.
	void ClearContext();

	// Format messages using the model's built-in chat template.
	// Each pair is {role, content}. Returns formatted prompt, or empty on failure.
	std::string ApplyChatTemplate(const std::vector<std::pair<std::string,std::string>> &messages, bool addAssistantPrefix) const;

	// Returns the end-of-turn string for the model's chat template (e.g. "<|im_end|>").
	// Derived by formatting a dummy assistant message. Returns empty if no template.
	std::string GetChatStopSequence() const;

	// Returns true if the model's chat template contains thinking-related tags
	// (e.g. <think>, enable_thinking), suggesting the model supports thinking mode.
	bool SupportsThinking() const;

	// Convert a token ID to its string representation.
	// If special=true, special tokens (e.g. <|im_end|>) are rendered as text.
	std::string TokenToString(int32_t tokenId, bool special) const;

	// Raw token log for diagnostics. Populated during generation.
	// Returns {tokenId, piece_with_special_tokens_visible} for each generated token.
	std::vector<std::pair<int32_t, std::string>> GetRawTokens();
	void ClearRawTokens();

private:
	void InferenceThread(std::string prompt, MT_LlamaGenerateParams params,
	                     MT_LlamaTokenCallback tokenCb, std::function<void(MT_LlamaStopReason)> doneCb);
	void LoadModelThread(std::string modelPath, MT_LlamaLoadParams params);
	static bool LlamaProgressCallback(float progress, void *userData);
	std::vector<int32_t> Tokenize(const std::string &text, bool addBos, bool parseSpecial = false);

	llama_model *model = nullptr;
	llama_context *ctx = nullptr;
	std::string activeBackendName = "llama.cpp (cpu)";

	std::thread inferenceThread;
	std::atomic<bool> generating{false};
	std::atomic<bool> stopRequested{false};
	int n_past = 0;
	std::mutex ctxMutex;  // Protects ctx and n_past from concurrent access

	// Raw token log for diagnostics (populated during generation)
	std::mutex rawTokensMutex;
	std::vector<std::pair<int32_t, std::string>> rawTokens; // {tokenId, piece with special=true}

	std::thread loadThread;
	std::atomic<bool> loadingModel{false};
	std::atomic<bool> cancelLoadRequested{false};
	std::atomic<float> loadProgress{0.0f};
	std::mutex loadErrorMutex;
	std::string loadError;
};
