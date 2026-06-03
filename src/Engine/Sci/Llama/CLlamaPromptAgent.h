#pragma once

#include "CLlamaTypes.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class CLlamaService;

// ─────────────────────────────────────────────────────────────────────────────
// CLlamaPromptAgent
//
// Generic base class for Llama-based AI agents.
//
// Usage:
//   1. Derive from this class and override the virtual On* methods.
//   2. Construct with a non-owning pointer to a loaded CLlamaService.
//   3. Call SendPrompt() to start async generation.
//   4. OnThinkingToken / OnAnswerToken fire on the background thread
//      (token by token). OnStatusChanged fires on status transitions.
//      OnComplete fires once generation finishes — result ready to analyse.
//
// Thread safety:
//   On* callbacks arrive on CLlamaService's background thread.
//   GetStatus() / GetLastResult() are safe to call from any thread.
// ─────────────────────────────────────────────────────────────────────────────
class CLlamaPromptAgent {
public:
	enum class Status {
		Idle,       // no generation in progress
		Thinking,   // model is producing <think> tokens
		Answering,  // model is producing the actual answer
		Done,       // generation finished successfully — OnComplete was called
		Stopped,    // user called Stop() — OnComplete is NOT called
		Error       // generation failed or was cancelled before producing output
	};

	// llama: non-owning pointer — caller must ensure it outlives this agent.
	explicit CLlamaPromptAgent(CLlamaService *llama);
	virtual ~CLlamaPromptAgent();

	// Send a user prompt. Maintains conversation history across calls so the
	// model retains context (the KV cache stays warm inside CLlamaService).
	//
	// rememberHistory: if true, the completed exchange (user + assistant) is
	//   appended to the internal chatMessages list and re-sent as part of the
	//   formatted prompt on the next call. Pass false for one-shot queries.
	//
	// Returns false if a generation is already running or no model is loaded.
	bool SendPrompt(const std::string &userText,
	                const MT_LlamaGenerateParams &params,
	                bool rememberHistory = true);

	// Request the current generation to stop. Non-blocking.
	// OnComplete will still be called (with UserStop reason) once the
	// background thread exits.
	void Stop();

	// Clear conversation history and reset KV cache context.
	// Do not call while generating.
	void ClearHistory();

	// Thread-safe status accessor.
	Status GetStatus() const;

	// Last completed result. Valid after OnComplete has been called.
	// May be read from any thread — protected by internal mutex.
	MT_LlamaParseResult GetLastResult() const;

	// Per-agent thinking override. When set to ForceOn/ForceOff, takes precedence
	// over CLlamaService::GetEnableThinking(). UseService (default) defers to the
	// global AI setup setting.
	enum class ThinkingOverride { UseService, ForceOn, ForceOff };
	void SetThinkingOverride(ThinkingOverride o) { thinkingOverride = o; }
	ThinkingOverride GetThinkingOverride() const { return thinkingOverride; }

protected:
	// ── Override these in derived classes ────────────────────────────────────

	// Called for each streamed token belonging to the model's internal
	// reasoning (<think>…</think> or equivalent).
	virtual void OnThinkingToken(const std::string &token) {}

	// Called for each streamed token belonging to the final answer,
	// letter by letter (or sub-word piece by piece).
	virtual void OnAnswerToken(const std::string &token) {}

	// Called whenever the internal status changes.
	// Transitions: Idle → Thinking → Answering → Done/Error
	virtual void OnStatusChanged(Status newStatus) {}

	// Called once when generation ends. result contains:
	//   result.thinking   — full concatenated thinking text
	//   result.answer     — full concatenated answer text
	//   result.stopReason — why generation stopped
	virtual void OnComplete(const MT_LlamaParseResult &result) {}

private:
	CLlamaService    *llama;  // non-owning
	ThinkingOverride  thinkingOverride = ThinkingOverride::UseService;

	mutable std::mutex stateMutex;
	Status status    = Status::Idle;
	bool   answering = false; // true once the first Answer token arrives
	MT_LlamaParseResult lastResult;

	// Conversation history: vector of {role, content} pairs.
	// "user" and "assistant" roles, formatted via ApplyChatTemplate.
	std::vector<std::pair<std::string, std::string>> chatMessages;

	// Whether the current call should store the exchange in chatMessages.
	bool rememberCurrentHistory = true;

	// Pending user text — stored per-call for history bookkeeping.
	std::string pendingUserText;

	// Set status and fire OnStatusChanged outside the lock (avoids re-entrancy).
	void ChangeStatus(Status s);

	// Internal callbacks wired into CLlamaService::GenerateWithSegmentsAsync.
	void HandleSegment(MT_LlamaSegment seg, const std::string &token);
	void HandleDone(MT_LlamaParseResult result);
};
