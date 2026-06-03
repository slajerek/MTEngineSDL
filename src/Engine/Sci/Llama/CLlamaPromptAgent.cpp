#include "CLlamaPromptAgent.h"
#include "CLlamaService.h"

#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────

CLlamaPromptAgent::CLlamaPromptAgent(CLlamaService *llama)
	: llama(llama)
{
	assert(llama && "CLlamaPromptAgent: CLlamaService pointer must not be null");
}

CLlamaPromptAgent::~CLlamaPromptAgent() = default;

// ─── Public API ──────────────────────────────────────────────────────────────

bool CLlamaPromptAgent::SendPrompt(const std::string &userText,
                                   const MT_LlamaGenerateParams &params,
                                   bool rememberHistory)
{
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		if (status == Status::Thinking || status == Status::Answering)
			return false; // already generating
	}

	if (!llama->HasModelLoaded())
		return false;

	// Persist call-level settings for use in HandleDone.
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		rememberCurrentHistory = rememberHistory;
		pendingUserText        = userText;
		lastResult             = {};
		answering              = false;

		// Add user turn so the formatted prompt includes it.
		// The assistant reply is appended in HandleDone after generation.
		chatMessages.push_back({"user", userText});
	}

	// Format prompt via the model's built-in chat template.
	std::string prompt;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		prompt = llama->ApplyChatTemplate(chatMessages, /*addAssistantPrefix=*/true);
	}
	if (prompt.empty())
		prompt = userText; // fallback if no template available

	// For models that support thinking (Qwen3/3.5):
	// - enable_thinking=true:  append "<think>\n" to let the model think
	// - enable_thinking=false: append "<think>\n\n</think>\n\n" (empty think block)
	//   The Jinja template's enable_thinking variable isn't accessible via the C API,
	//   so we control it by prefilling the assistant response with an empty/open
	//   thinking block so the model skips or starts reasoning accordingly.
	//
	// thinkingOverride lets per-agent code (e.g. translation pipeline) force thinking
	// on or off regardless of the global AI setup setting.
	bool thinkingPrefilled = false;
	if (llama->SupportsThinking())
	{
		bool enableThinking;
		switch (thinkingOverride)
		{
			case ThinkingOverride::ForceOn:  enableThinking = true;  break;
			case ThinkingOverride::ForceOff: enableThinking = false; break;
			default:                         enableThinking = llama->GetEnableThinking(); break;
		}

		if (enableThinking)
		{
			prompt += "<think>\n";
			thinkingPrefilled = true;
		}
		else
		{
			prompt += "<think>\n\n</think>\n\n";
		}
	}

	// Transition Idle → Thinking.
	ChangeStatus(Status::Thinking);

	bool started = llama->GenerateWithSegmentsAsync(
		prompt,
		params,
		[this](MT_LlamaSegment seg, const std::string &token) {
			HandleSegment(seg, token);
		},
		[this](MT_LlamaParseResult result) {
			HandleDone(std::move(result));
		},
		thinkingPrefilled
	);

	if (!started)
	{
		// Generation couldn't start — roll back.
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			if (!chatMessages.empty())
				chatMessages.pop_back();
		}
		ChangeStatus(Status::Error);
		return false;
	}

	return true;
}

void CLlamaPromptAgent::Stop()
{
	llama->StopGeneration();
}

void CLlamaPromptAgent::ClearHistory()
{
	std::lock_guard<std::mutex> lock(stateMutex);
	chatMessages.clear();
	lastResult = {};
	status     = Status::Idle;
	answering  = false;
	// Note: call llama->ClearContext() separately if you want to flush KV cache.
}

CLlamaPromptAgent::Status CLlamaPromptAgent::GetStatus() const
{
	std::lock_guard<std::mutex> lock(stateMutex);
	return status;
}

MT_LlamaParseResult CLlamaPromptAgent::GetLastResult() const
{
	std::lock_guard<std::mutex> lock(stateMutex);
	return lastResult;
}

// ─── Private ─────────────────────────────────────────────────────────────────

// Change status and call OnStatusChanged *outside* the lock to avoid re-entrancy.
void CLlamaPromptAgent::ChangeStatus(Status s)
{
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		status = s;
	}
	OnStatusChanged(s); // called without holding the lock
}

void CLlamaPromptAgent::HandleSegment(MT_LlamaSegment seg, const std::string &token)
{
	if (seg == MT_LlamaSegment::Thinking)
	{
		OnThinkingToken(token);
	}
	else // MT_LlamaSegment::Answer
	{
		// First answer token — transition Thinking → Answering once.
		bool needTransition = false;
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			if (!answering)
			{
				answering = true;
				status    = Status::Answering;
				needTransition = true;
			}
		}
		if (needTransition)
			OnStatusChanged(Status::Answering);

		OnAnswerToken(token);
	}
}

void CLlamaPromptAgent::HandleDone(MT_LlamaParseResult result)
{
	// Store result and optionally push assistant reply to history.
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		lastResult = result;

		if (rememberCurrentHistory && !result.answer.empty())
			chatMessages.push_back({"assistant", result.answer});
	}

	// If the user explicitly stopped generation, transition to Stopped and skip OnComplete.
	if (result.stopReason == MT_LlamaStopReason::UserStop)
	{
		ChangeStatus(Status::Stopped);
		return;
	}

	ChangeStatus(Status::Done);
	OnComplete(result);
}
