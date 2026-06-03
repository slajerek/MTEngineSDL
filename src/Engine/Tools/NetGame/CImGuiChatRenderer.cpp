#include "CImGuiChatRenderer.h"
#include "imgui.h"
#include "CChatHistory.h"

using namespace ImGui;

void CImGuiChatRenderer::RenderChatArea(ChatState &state, float chatHeight, std::function<void()> onRequestHistory, const Labels &labels)
{
	if (!state.messages) return;

	if (chatHeight <= 0.0f) chatHeight = GetContentRegionAvail().y;
	if (chatHeight < 50.0f) chatHeight = 50.0f;

	BeginChild("##chatArea", ImVec2(0, chatHeight), true);

	int chatCount = (int)state.messages->size();

	// Load more history when scrolled to top
	if (GetScrollY() < 1.0f && chatCount > 0 && state.hasMoreHistory && *state.hasMoreHistory && state.isLoadingHistory && !*state.isLoadingHistory)
	{
		if (onRequestHistory) onRequestHistory();
	}

	// Show loading indicator
	if (state.isLoadingHistory && *state.isLoadingHistory)
	{
		TextDisabled("%s", labels.loading);
	}
	else if (state.hasMoreHistory && !*state.hasMoreHistory && chatCount > 0)
	{
		TextDisabled("%s", labels.beginningOfChat);
	}

	// Render chat messages (no ListClipper — TextWrapped produces variable-height items)
	for (int i = 0; i < chatCount; i++)
	{
		const ChatEntry &entry = (*state.messages)[i];
		TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "[%s]", entry.player.c_str());
		SameLine();
		TextWrapped("%s", entry.message.c_str());
	}

	// Handle scroll position for new messages and prepended history
	int newCount = chatCount - state.prevMessageCount;
	if (newCount > 0)
	{
		if (state.wasAtBottom)
		{
			SetScrollHereY(1.0f);
		}
		else if (state.prevMessageCount > 0)
		{
			float lineHeight = GetTextLineHeightWithSpacing();
			SetScrollY(GetScrollY() + lineHeight * newCount);
		}
	}

	state.wasAtBottom = (GetScrollY() >= GetScrollMaxY() - 1.0f);
	state.prevMessageCount = chatCount;

	EndChild();
}

bool CImGuiChatRenderer::RenderChatInput(char *buf, int bufSize, float sendBtnWidth, const char *sendLabel)
{
	bool sent = false;

	SetNextItemWidth(-sendBtnWidth - GetStyle().ItemSpacing.x);
	if (InputText("##chatInput", buf, bufSize, ImGuiInputTextFlags_EnterReturnsTrue))
	{
		sent = true;
		SetKeyboardFocusHere(-1);  // re-focus the InputText (previous item)
	}
	SameLine();
	if (Button(sendLabel, ImVec2(sendBtnWidth, 0)))
	{
		sent = true;
		SetKeyboardFocusHere(-1);  // re-focus the Button's previous = InputText
	}

	return sent;
}
