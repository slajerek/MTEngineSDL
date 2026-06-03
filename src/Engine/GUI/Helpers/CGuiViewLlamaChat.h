#pragma once

#include "CGuiView.h"
#include "CSystemFileDialogCallback.h"
#include "Sci/Llama/CLlamaService.h"
#include "Sci/Llama/CLlamaModelManager.h"
#include "Sci/Llama/CLlamaTypes.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct LlamaChatEntry
{
	bool isUser;
	std::string text;
	float thinkingTimeSec = 0.f;
	bool isComplete = true;
	bool thinkingPrefilled = false; // model output starts inside <think> block
	MT_LlamaStopReason stopReason = MT_LlamaStopReason::None;
};

class CGuiViewLlamaChat : public CGuiView, public CSystemFileDialogCallback
{
public:
	// llama is non-owning; caller must ensure it outlives this view.
	CGuiViewLlamaChat(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY,
	                  CLlamaService *llama);
	virtual ~CGuiViewLlamaChat();

	virtual void RenderImGui() override;

	// CSystemFileDialogCallback
	virtual void SystemDialogFileSaveSelected(CSlrString *path) override;
	virtual void SystemDialogFileOpenSelected(CSlrString *path) override;

	// Optional: point this at the loader's genParams to use user-configured
	// temperature / max_tokens / seed. If null, struct defaults are used.
	const MT_LlamaGenerateParams *genParamsSource = nullptr;

	// Optional: point this at the model manager to enable auto-save chat folder.
	// Non-owning. If set, every NewContext() creates a live .txt session file.
	CLlamaModelManager *modelManager = nullptr;

private:
	void RenderHistory();
	void SendMessage();
	void NewContext();
	void SaveChat(const std::string &path);
	void LoadChat(const std::string &path);
	void CopyConversationToClipboard();

	CLlamaService *llama; // non-owning

	std::vector<LlamaChatEntry> history;
	std::vector<std::pair<std::string,std::string>> chatMessages; // structured {role, content} pairs
	std::string committedContext; // full prompt text decoded into KV cache

	char inputBuf[4096] = {};
	std::vector<std::string> promptHistory;
	int promptHistoryIdx = -1;
	std::string pendingPrompt;

	// Streaming state
	std::string streamingText;
	std::mutex streamMutex;
	std::string pendingTokens;
	std::atomic<bool> generationDone{false};
	MT_LlamaStopReason pendingStopReason = MT_LlamaStopReason::None;
	bool thinkingPrefilled = false; // true when prompt ends with "<think>\n"
	std::chrono::steady_clock::time_point generationStart;

	bool scrollToBottom = false;
	bool focusInputNextFrame = false;

	// Live autosave — path set by NewContext() or lazily by EnsureLiveChatFileOpen(),
	// appended in SendMessage() / token callback.
	std::string liveChatFilePath;
	std::string GetEffectiveSystemPrompt() const;
	void EnsureLiveChatFileOpen(); // lazy-init: creates file on first SendMessage if folder is set
	void LiveAppend(const std::string &text);

	// Save dialog mode: which dialog triggered SystemDialogFileSaveSelected
	enum class SaveDialogMode { Chat, TokenDump };
	SaveDialogMode saveDialogMode = SaveDialogMode::Chat;

	void SaveTokenDump(const std::string &path);
};
