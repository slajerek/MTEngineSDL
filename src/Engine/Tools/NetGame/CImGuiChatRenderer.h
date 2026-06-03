#pragma once

#include "CChatHistory.h"
#include <vector>
#include <functional>

// Reusable ImGui chat renderer — used by game client views and lobby client views.
// Stateless rendering methods that operate on caller-owned data.
class CImGuiChatRenderer
{
public:
	// Per-instance state that the caller must keep alive across frames.
	struct ChatState {
		std::vector<ChatEntry> *messages = nullptr;
		bool *isLoadingHistory = nullptr;
		bool *hasMoreHistory = nullptr;
		bool wasAtBottom = true;
		int prevMessageCount = 0;
	};

	// Optional labels — callers can pass i18n-resolved strings.
	// Defaults to English if not provided.
	struct Labels {
		const char *loading;
		const char *beginningOfChat;
		const char *sendButton;
		Labels() : loading("Loading..."), beginningOfChat("--- beginning of chat ---"), sendButton("Send") {}
	};

	// Render the scrollable chat history area inside a BeginChild/EndChild.
	// chatHeight: height of the child area (0 = use remaining space).
	// onRequestHistory: called when user scrolls to top and more history is available.
	// labels: optional customizable text (for i18n support).
	static void RenderChatArea(ChatState &state, float chatHeight, std::function<void()> onRequestHistory, const Labels &labels = Labels());

	// Render the chat input field + Send button on one row.
	// Returns true when the user submitted a message (Enter or Send click).
	// On return true, buf contains the message text; caller should clear buf after sending.
	static bool RenderChatInput(char *buf, int bufSize, float sendBtnWidth = 80.0f, const char *sendLabel = "Send");
};
