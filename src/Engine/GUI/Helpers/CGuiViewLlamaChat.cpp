#include "CGuiViewLlamaChat.h"

#include "Sci/Llama/CLlamaStreamParser.h"
#include "SYS_FileSystem.h"
#include "imgui.h"
#include "imgui_md.h"
#include "CGuiFontManager.h"
#include "json.hpp"
#include "IconsFontAwesome_c.h"

#include "DBG_Log.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

using json = nlohmann::json;
using namespace ImGui;

// ─── Markdown renderer ───────────────────────────────────────────────────────

// Minimal imgui_md subclass for rendering LLM answer text.
// No image support, no URL opening (in-game context).
// Fonts are sourced from CGuiFontManager when markdown fonts are loaded.
struct LlamaChatMarkdown : public imgui_md
{
	ImFont *get_font() const override
	{
		if (!gGuiFontManager.markdownFontsLoaded)
			return nullptr; // fall back to current ImGui font
		if (m_is_code || (m_hlevel == 0 && !m_is_em && !m_is_strong))
			return m_is_code ? gGuiFontManager.fontMono : gGuiFontManager.fontRegular;
		if (m_is_strong && m_is_em) return gGuiFontManager.fontBoldItalic;
		if (m_is_strong)            return gGuiFontManager.fontBold;
		if (m_is_em)                return gGuiFontManager.fontItalic;
		return gGuiFontManager.fontBold; // headings
	}
	// BLOCK_CODE sets m_is_code but doesn't call set_font — do it here
	void BLOCK_CODE(const MD_BLOCK_CODE_DETAIL* d, bool e) override
	{
		imgui_md::BLOCK_CODE(d, e); // sets m_is_code = e
		if (e) ImGui::PushFont(get_font());
		else   ImGui::PopFont();
	}
	// SPAN_CODE (inline backtick) — base class is empty, wire it up
	void SPAN_CODE(bool e) override
	{
		m_is_code = e;
		if (e) ImGui::PushFont(get_font());
		else   ImGui::PopFont();
	}
	bool get_image(image_info &) const override { return false; }
	void open_url() const override {} // no browser in-game
	void soft_break() override { ImGui::NewLine(); }
};

static LlamaChatMarkdown s_mdRenderer;

CGuiViewLlamaChat::CGuiViewLlamaChat(const char *name, float posX, float posY, float posZ,
                                     float sizeX, float sizeY, CLlamaService *llama)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
, llama(llama)
{
}

CGuiViewLlamaChat::~CGuiViewLlamaChat()
{
	if (llama && llama->IsGenerating())
		llama->StopGeneration();
}

std::string CGuiViewLlamaChat::GetEffectiveSystemPrompt() const
{
	if (!llama) return "";
	return llama->GetSystemPrompt();
}

void CGuiViewLlamaChat::RenderImGui()
{
	auto drainAndFinalize = [this]() {
		// --- Drain pending stream tokens ---
		{
			std::lock_guard<std::mutex> lock(streamMutex);
			if (!pendingTokens.empty())
			{
				streamingText += pendingTokens;
				pendingTokens.clear();
				scrollToBottom = true;
			}
		}

		// --- Finalize entry when generation is complete ---
		if (generationDone.exchange(false))
		{
			float elapsed = std::chrono::duration<float>(
				std::chrono::steady_clock::now() - generationStart).count();
			if (!history.empty() && !history.back().isUser && !history.back().isComplete)
			{
				history.back().text = streamingText;
				history.back().thinkingTimeSec = elapsed;
				history.back().isComplete = true;
				history.back().thinkingPrefilled = thinkingPrefilled;
				history.back().stopReason = pendingStopReason;

				chatMessages.push_back({"assistant", streamingText});

				// Update committedContext: use template if available, else naive append
				auto msgsWithSys = chatMessages;
				std::string effSys = GetEffectiveSystemPrompt();
				if (!effSys.empty())
					msgsWithSys.insert(msgsWithSys.begin(), {"system", effSys});
				std::string templated = llama ? llama->ApplyChatTemplate(msgsWithSys, false) : "";
				if (!templated.empty())
					committedContext = templated;
				else
					committedContext += streamingText + "\n";
			}
			streamingText.clear();
			scrollToBottom = true;
			focusInputNextFrame = true;
		}
	};

	drainAndFinalize();

	PreRenderImGui();

	// ESC while this window has focus — stop generation and focus input
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
	    ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		if (llama && llama->IsGenerating())
			llama->StopGeneration();
		focusInputNextFrame = true;
	}

	const float spacing = ImGui::GetStyle().ItemSpacing.x;

	// --- Top toolbar row: [New Context] [Save...] [Load...]  ...  [COPY] ---
	if (ImGui::Button("New Context"))
		NewContext();
	ImGui::SameLine();
	if (ImGui::Button("Save..."))
	{
		saveDialogMode = SaveDialogMode::Chat;
		std::list<CSlrString *> ext;
		CSlrString jsonExt("json");
		ext.push_back(&jsonExt);
		CSlrString title("Save Chat");
		SYS_DialogSaveFile(this, &ext, nullptr, &title, nullptr);
	}
	ImGui::SameLine();
	if (ImGui::Button("Load..."))
	{
		std::list<CSlrString *> ext;
		CSlrString jsonExt("json");
		ext.push_back(&jsonExt);
		CSlrString title("Load Chat");
		SYS_DialogOpenFile(this, &ext, nullptr, &title);
	}
	ImGui::SameLine();
	if (ImGui::Button("Dump Tokens..."))
	{
		saveDialogMode = SaveDialogMode::TokenDump;
		std::list<CSlrString *> ext;
		CSlrString txtExt("txt");
		ext.push_back(&txtExt);
		CSlrString title("Save Token Dump");
		SYS_DialogSaveFile(this, &ext, nullptr, &title, nullptr);
	}

	// Copy button — right-aligned, wide enough to click easily
	const float copyW = 120.f;
	ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - copyW);
	if (ImGui::Button(ICON_FA_CLIPBOARD " Copy##global", ImVec2(copyW, 0)))
		CopyConversationToClipboard();

	ImGui::Separator();

	// --- Dynamic Input Height Calculation ---
	bool isGenerating = llama && llama->IsGenerating();
	float sendW = 60.f;
	float stopW = 60.f;
	float btnW = isGenerating ? stopW : sendW;
	float inputW = ImGui::GetContentRegionAvail().x - btnW - spacing;

	// Calculate text height based on wrapping
	// ImGui::InputTextMultiline internally subtracts ScrollbarSize from the available wrap width when word-wrapping is enabled
	float wrapWidth = std::max(1.0f, inputW - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ScrollbarSize);
	ImVec2 textSize = ImGui::CalcTextSize(inputBuf, nullptr, false, wrapWidth);
	float minHeight = ImGui::GetFrameHeight();
	float maxHeight = ImGui::GetFrameHeight() * 6.0f; // Max ~6 lines
	float textHeight = textSize.y + ImGui::GetStyle().FramePadding.y * 2.0f;
	// If text ends with newline, ImGui::CalcTextSize doesn't add a new line height for the empty next line
	size_t len = strlen(inputBuf);
	if (len > 0 && inputBuf[len-1] == '\n') textHeight += ImGui::GetTextLineHeight();

	float inputH = std::max(minHeight, std::min(textHeight, maxHeight));
	float bottomH = inputH + ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().WindowPadding.y;

	// --- History scroll area ---
	if (ImGui::BeginChild("##chatHistory", ImVec2(-1.f, -bottomH), false))
	{
		RenderHistory();

		if (scrollToBottom)
		{
			ImGui::SetScrollHereY(1.f);
			scrollToBottom = false;
		}
	}
	ImGui::EndChild();

	ImGui::Separator();

	// --- Input row ---
	if (focusInputNextFrame && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
	{
		ImGui::SetKeyboardFocusHere();
		focusInputNextFrame = false;
	}

	ImGui::PushItemWidth(inputW);
	// We use CtrlEnterForNewLine so ImGui natively makes plain Enter return true (instead of adding a newline).
	// We handle Shift+Enter and History via a custom callback.
	int flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_NoHorizontalScroll | ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackAlways;
	
	auto chatInputCallback = [](ImGuiInputTextCallbackData* data) -> int {
		CGuiViewLlamaChat* pThis = (CGuiViewLlamaChat*)data->UserData;
		if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
			// Manually handle Shift+Enter to insert a newline. 
			// (Ctrl+Enter is handled natively by ImGui via ImGuiInputTextFlags_CtrlEnterForNewLine)
			if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
				bool shiftHeld = ImGui::GetIO().KeyShift;
				
				if (shiftHeld) {
					data->InsertChars(data->CursorPos, "\n");
					data->CursorPos++;
				}
			}
			
			// Custom history handling
			if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
				// Only navigate history if cursor is at the very beginning of the prompt 
				// or prompt is empty, to allow multiline navigation
				if (data->CursorPos == 0 && !pThis->promptHistory.empty()) {
					if (pThis->promptHistoryIdx == -1) {
						pThis->pendingPrompt = data->Buf;
						pThis->promptHistoryIdx = (int)pThis->promptHistory.size() - 1;
					} else if (pThis->promptHistoryIdx > 0) {
						pThis->promptHistoryIdx--;
					}
					data->DeleteChars(0, data->BufTextLen);
					data->InsertChars(0, pThis->promptHistory[pThis->promptHistoryIdx].c_str());
					data->CursorPos = data->BufTextLen; // move cursor to end of replacing text
					data->SelectionStart = data->SelectionEnd = data->CursorPos;
				}
			} else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
				// Only navigate history if cursor is at the very end of the prompt
				if (data->CursorPos == data->BufTextLen && pThis->promptHistoryIdx != -1) {
					pThis->promptHistoryIdx++;
					data->DeleteChars(0, data->BufTextLen);
					if (pThis->promptHistoryIdx >= (int)pThis->promptHistory.size()) {
						pThis->promptHistoryIdx = -1;
						data->InsertChars(0, pThis->pendingPrompt.c_str());
					} else {
						data->InsertChars(0, pThis->promptHistory[pThis->promptHistoryIdx].c_str());
					}
					data->CursorPos = data->BufTextLen;
					data->SelectionStart = data->SelectionEnd = data->CursorPos;
				}
			}
		}
		return 0;
	};

	bool sendPressed = false;
	bool textEntered = ImGui::InputTextMultiline("##chatInput", inputBuf, sizeof(inputBuf), ImVec2(inputW, inputH), flags, chatInputCallback, (void*)this);
	
	// textEntered is true when ENTER is pressed. 
	// We only send the message if Shift wasn't held (and Ctrl wasn't held, because Ctrl+Enter is natively inserting newline).
	if (textEntered && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) {
		sendPressed = true;
		ImGui::SetKeyboardFocusHere(-1);
	} else if (textEntered) {
		// ImGui returned true because Enter was pressed with a modifier (either Shift or Ctrl)
		// We need to keep focus here to continue typing.
		ImGui::SetKeyboardFocusHere(-1);
	}
	ImGui::PopItemWidth();

	ImGui::SameLine();

	// Align the button to the bottom of the input area
	float cursorY = ImGui::GetCursorPosY();
	if (inputH > minHeight)
	{
		ImGui::SetCursorPosY(cursorY + inputH - minHeight);
	}

	if (sendPressed)
	{
		if (isGenerating && llama)
		{
			llama->StopGeneration();
			drainAndFinalize();
		}
		SendMessage();
		isGenerating = llama && llama->IsGenerating();
	}

	if (isGenerating)
	{
		if (ImGui::Button("Stop", ImVec2(stopW, minHeight)))
		{
			if (llama) llama->StopGeneration();
			drainAndFinalize();
		}
	}
	else
	{
		if (ImGui::Button("Send", ImVec2(sendW, minHeight)))
		{
			SendMessage();
		}
	}

	// Restore cursor Y so PostRenderImGui doesn't shift
	if (inputH > minHeight)
	{
		ImGui::SetCursorPosY(cursorY);
	}

	PostRenderImGui();
}

void CGuiViewLlamaChat::RenderHistory()
{
	float availW = ImGui::GetContentRegionAvail().x;
	const float padH = 8.f;
	const float padV = 4.f;
	const float bubbleRadius = 5.f;
	const ImU32 userBubbleColor = IM_COL32(30, 100, 80, 220); // dark teal — distinct from button blue

	for (size_t i = 0; i < history.size(); i++)
	{
		const LlamaChatEntry &entry = history[i];
		ImGui::PushID((int)i);

		if (entry.isUser)
		{
			// Right-aligned bubble: occupies right 50% of the window
			float bubbleMaxW = availW * 0.5f - padH * 2.f;
			const char *txt = entry.text.c_str();
			ImVec2 textSize = ImGui::CalcTextSize(txt, nullptr, false, bubbleMaxW);
			float bubbleW = std::min(textSize.x + padH * 2.f, availW * 0.5f);
			float startX = availW - bubbleW;

			// Add an invisible button or group to capture right clicks for the context menu
			ImGui::SetCursorPosX(startX);
			
			ImVec2 screenPos = ImGui::GetCursorScreenPos();
			ImVec2 bMin(screenPos.x, screenPos.y);
			ImVec2 bMax(screenPos.x + bubbleW, screenPos.y + textSize.y + padV * 2.f);
			ImGui::GetWindowDrawList()->AddRectFilled(bMin, bMax, userBubbleColor, bubbleRadius);

			// Position text inside bubble
			ImGui::SetCursorPosX(startX + padH);
			float savedY = ImGui::GetCursorPosY();
			ImGui::SetCursorPosY(savedY + padV);
			
			ImGui::BeginGroup();
			ImGui::PushTextWrapPos(startX + bubbleW - padH);
			ImGui::TextUnformatted(txt);
			ImGui::PopTextWrapPos();
			ImGui::EndGroup();
	
			if (ImGui::BeginPopupContextItem("##copy_user"))
			{
				if (ImGui::MenuItem("Copy"))
				{
					ImGui::SetClipboardText(txt);
				}
				ImGui::EndPopup();
			}

			// Advance past the bubble bottom
			ImGui::SetCursorPosY(savedY + textSize.y + padV * 2.f);
		}
		else
		{
			// Left-aligned assistant entry
			const std::string &rawText = entry.isComplete ? entry.text : streamingText;
			// When thinking was prefilled in the prompt, the model output starts
			// inside a <think> block without the opening tag. Prepend it so
			// ParseThinkSegments classifies the content correctly.
			bool prefilled = entry.isComplete ? entry.thinkingPrefilled : thinkingPrefilled;
			std::string textForParse = prefilled ? "<think>" + rawText : rawText;
			auto segments = ParseThinkSegments(textForParse);

			// Header: "Thinking..." while generating, "Thought for Xs" when done
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.f));
			if (!entry.isComplete)
			{
				float elapsed = std::chrono::duration<float>(
					std::chrono::steady_clock::now() - generationStart).count();
				ImGui::Text("Thinking... %.0fs", elapsed);
			}
			else
			{
				ImGui::Text("Thought for %.1fs", entry.thinkingTimeSec);
			}
			ImGui::PopStyleColor();

			// Group the whole assistant response for the context menu
			ImGui::BeginGroup();

			// Render segments
			const float thinkIndent = 12.f;
			const float thinkBarW   = 3.f;
			const float thinkBarX   = 4.f;
			const ImU32 thinkBarColor = IM_COL32(90, 90, 90, 200);

			bool anyContent = false;
			for (const auto &seg : segments)
			{
				if (seg.isThinking)
				{
					// Dimmed, indented with a left vertical bar
					ImVec2 segStart = ImGui::GetCursorScreenPos();
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + thinkIndent);
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.8f));
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + availW * 0.82f - thinkIndent);
					ImGui::TextUnformatted(seg.text.c_str());
					ImGui::PopTextWrapPos();
					ImGui::PopStyleColor();
					ImVec2 segEnd = ImGui::GetCursorScreenPos();
					// Draw bar after text so we know the height
					ImGui::GetWindowDrawList()->AddRectFilled(
						ImVec2(segStart.x + thinkBarX, segStart.y),
						ImVec2(segStart.x + thinkBarX + thinkBarW, segEnd.y),
						thinkBarColor);
					anyContent = true;
				}
				else
				{
					// Render answer text as markdown (LLMs commonly emit formatted output)
					s_mdRenderer.print(seg.text.c_str(), seg.text.c_str() + seg.text.size());
					anyContent = true;
				}
			}
			if (!anyContent)
				ImGui::TextDisabled("...");
			
			// Show stop reason if generation is complete and not None
			if (entry.isComplete && entry.stopReason != MT_LlamaStopReason::None)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.8f));
				// Use a smaller font if available, or just rely on the dim color
				switch (entry.stopReason)
				{
					case MT_LlamaStopReason::EndOfTurn:
						ImGui::TextWrapped("(stopped: end of turn)");
						break;
					case MT_LlamaStopReason::MaxTokens:
						ImGui::TextWrapped("(stopped: max tokens limit reached)");
						break;
					case MT_LlamaStopReason::StopSequence:
						ImGui::TextWrapped("(stopped: stop sequence)");
						break;
					case MT_LlamaStopReason::UserStop:
						ImGui::TextWrapped("(stopped: user interrupt)");
						break;
					default: break;
				}
				ImGui::PopStyleColor();
			}
			
			ImGui::EndGroup(); // Close the group wrapping the model's segments
			
			if (ImGui::BeginPopupContextItem("##copy_assistant"))
			{
				if (ImGui::MenuItem("Copy"))
				{
					ImGui::SetClipboardText(rawText.c_str());
				}
				ImGui::EndPopup();
			}
		}

		ImGui::Spacing();
		ImGui::PopID();
	}
}

void CGuiViewLlamaChat::SendMessage()
{
	if (!llama || !llama->HasModelLoaded()) return;
	if (llama->IsGenerating()) return;

	std::string msg(inputBuf);
	if (msg.empty()) return;
	inputBuf[0] = '\0';

	promptHistory.push_back(msg);
	promptHistoryIdx = -1;
	pendingPrompt.clear();

	history.push_back({true, msg, 0.f, true, false, MT_LlamaStopReason::None});
	history.push_back({false, "", 0.f, false, false, MT_LlamaStopReason::None});
	scrollToBottom = true;

	chatMessages.push_back({"user", msg});

	// Build messages with system prompt prepended (if set and template supports it)
	auto messagesWithSystem = chatMessages;
	std::string effSys = GetEffectiveSystemPrompt();
	if (!effSys.empty())
		messagesWithSystem.insert(messagesWithSystem.begin(), {"system", effSys});

	// Try model's built-in chat template, fall back to naive format
	std::string fullPrompt = llama->ApplyChatTemplate(messagesWithSystem, true);
	if (fullPrompt.empty())
	{
		LOGD("CGuiViewLlamaChat::SendMessage: chat template NOT available, using naive fallback");
		committedContext += "User: " + msg + "\nAssistant: ";
		fullPrompt = committedContext;
	}
	else
	{
		// For models that support thinking (Qwen3/3.5):
		// - enable_thinking=true:  append "<think>\n" to let the model think
		// - enable_thinking=false: append "<think>\n\n</think>\n\n" (empty think block)
		//   This is what the official Jinja template does — it prefills an empty
		//   thinking block so the model skips directly to the answer.
		thinkingPrefilled = false;
		if (llama->SupportsThinking())
		{
			if (llama->GetEnableThinking())
			{
				fullPrompt += "<think>\n";
				thinkingPrefilled = true;
			}
			else
				fullPrompt += "<think>\n\n</think>\n\n";
		}

		LOGD("CGuiViewLlamaChat::SendMessage: chat template applied, prompt length=%zu, thinking=%s",
			fullPrompt.size(), (llama->SupportsThinking() && llama->GetEnableThinking()) ? "ON" : "OFF");
	}

	streamingText.clear();
	generationDone = false;
	generationStart = std::chrono::steady_clock::now();

	// Lazy-init: create session file now if folder is set but no NewContext() was called
	EnsureLiveChatFileOpen();

	// Write user message to live file before generation starts
	LiveAppend("> user:\n" + msg + "\n\n> " + llama->GetBackendName() + ":\n");

	MT_LlamaGenerateParams genParams = genParamsSource ? *genParamsSource : MT_LlamaGenerateParams{};
	std::string stopSeq = llama->GetChatStopSequence();
	if (!stopSeq.empty())
	{
		genParams.stopSequences.push_back(stopSeq);
		LOGD("CGuiViewLlamaChat::SendMessage: stop sequence = \"%s\"", stopSeq.c_str());
	}
	else
	{
		LOGD("CGuiViewLlamaChat::SendMessage: WARNING - no stop sequence detected!");
	}


	llama->GenerateAsync(
		fullPrompt,
		genParams,
		[this](const std::string &token) {
			std::lock_guard<std::mutex> lock(streamMutex);
			pendingTokens += token;
			// Live-append token to session file
			LiveAppend(token);
		},
		[this](MT_LlamaStopReason reason) {
			pendingStopReason = reason;
			generationDone = true;
			// Separate turns with a blank line
			LiveAppend("\n\n");
		}
	);

	focusInputNextFrame = true;
}

void CGuiViewLlamaChat::EnsureLiveChatFileOpen()
{
	if (!liveChatFilePath.empty()) return; // already have a file for this session
	if (!modelManager)             return;
	std::string folder = modelManager->GetAutoSaveChatFolder();
	if (folder.empty())            return;

	std::time_t now = std::time(nullptr);
	std::tm *tm = std::localtime(&now);
	char ts[32];
	std::strftime(ts, sizeof(ts), "%y%m%d-%H%M", tm);

	std::string modelId = modelManager ? modelManager->GetSelectedModelId() : "unknown";
	std::string filename = std::string(ts) + "-" + modelId + ".txt";

	if (folder.back() != '/' && folder.back() != '\\')
		folder += '/';

	liveChatFilePath = folder + filename;
}

void CGuiViewLlamaChat::LiveAppend(const std::string &text)
{
	if (liveChatFilePath.empty())
		return;
	std::ofstream f(liveChatFilePath, std::ios::app);
	if (f.is_open())
	{
		f << text;
		f.flush();
	}
}

void CGuiViewLlamaChat::NewContext()
{
	if (!llama) return;
	if (llama->IsGenerating())
		llama->StopGeneration();
	llama->ClearContext();
	history.clear();
	chatMessages.clear();
	committedContext.clear();
	streamingText.clear();
	generationDone = false;
	scrollToBottom = false;

	// Start a new live session file if an autosave folder is configured
	liveChatFilePath.clear();
	if (modelManager)
	{
		std::string folder = modelManager->GetAutoSaveChatFolder();
		if (!folder.empty())
		{
			// Timestamp: YYMMDD-HHMM
			std::time_t now = std::time(nullptr);
			std::tm *tm = std::localtime(&now);
			char ts[32];
			std::strftime(ts, sizeof(ts), "%y%m%d-%H%M", tm);

			std::string modelId = modelManager ? modelManager->GetSelectedModelId() : "unknown";
			std::string filename = std::string(ts) + "-" + modelId + ".txt";

			// Ensure folder ends with separator
			if (!folder.empty() && folder.back() != '/' && folder.back() != '\\')
				folder += '/';

			liveChatFilePath = folder + filename;
		}
	}
}

void CGuiViewLlamaChat::SystemDialogFileSaveSelected(CSlrString *path)
{
	if (!path) return;
	char *c = path->GetUTF8();
	if (c)
	{
		if (saveDialogMode == SaveDialogMode::TokenDump)
			SaveTokenDump(c);
		else
			SaveChat(c);
		free(c);
	}
}

void CGuiViewLlamaChat::SystemDialogFileOpenSelected(CSlrString *path)
{
	if (!path) return;
	char *c = path->GetUTF8();
	if (c) { LoadChat(c); free(c); }
}

void CGuiViewLlamaChat::SaveChat(const std::string &path)
{
	json j;
	j["entries"] = json::array();
	for (const auto &e : history)
	{
		json entry;
		entry["isUser"] = e.isUser;
		entry["text"] = e.text;
		if (!e.isUser)
		{
			entry["thinkingTimeSec"] = e.thinkingTimeSec;
			entry["thinkingPrefilled"] = e.thinkingPrefilled;
			entry["stopReason"] = (int)e.stopReason;
		}
		j["entries"].push_back(entry);
	}
	j["committedContext"] = committedContext;

	j["chatMessages"] = json::array();
	for (const auto &m : chatMessages)
	{
		json msg;
		msg["role"] = m.first;
		msg["content"] = m.second;
		j["chatMessages"].push_back(msg);
	}

	std::ofstream f(path);
	if (f.is_open())
		f << j.dump(2);
}

void CGuiViewLlamaChat::CopyConversationToClipboard()
{
	if (history.empty()) return;

	std::string modelName = llama ? llama->GetBackendName() : "assistant";

	std::ostringstream out;
	for (const auto &e : history)
	{
		if (!e.isComplete) continue;
		if (e.isUser)
		{
			out << "> user:\n" << e.text << "\n\n";
		}
		else
		{
			out << "> " << modelName << ":\n" << e.text << "\n\n";
		}
	}

	std::string result = out.str();
	// trim trailing newlines
	while (result.size() >= 2 && result[result.size()-1] == '\n' && result[result.size()-2] == '\n')
		result.pop_back();

	ImGui::SetClipboardText(result.c_str());
}

void CGuiViewLlamaChat::LoadChat(const std::string &path)
{
	if (!llama) return;
	if (llama->IsGenerating()) llama->StopGeneration();

	std::ifstream f(path);
	if (!f.is_open()) return;

	json j;
	try { j = json::parse(f); }
	catch (...) { return; }

	NewContext(); // clear existing state

	if (j.contains("entries") && j["entries"].is_array())
	{
		for (const auto &e : j["entries"])
		{
			LlamaChatEntry entry;
			entry.isUser = e.value("isUser", true);
			entry.text = e.value("text", "");
			entry.thinkingTimeSec = e.value("thinkingTimeSec", 0.f);
			entry.thinkingPrefilled = e.value("thinkingPrefilled", false);
			entry.stopReason = (MT_LlamaStopReason)e.value("stopReason", 0);
			entry.isComplete = true;
			history.push_back(entry);
		}
	}

	if (j.contains("chatMessages") && j["chatMessages"].is_array())
	{
		for (const auto &m : j["chatMessages"])
			chatMessages.push_back({m.value("role", ""), m.value("content", "")});
	}
	else
	{
		// Rebuild chatMessages from history entries for legacy saves
		for (const auto &e : history)
			chatMessages.push_back({e.isUser ? "user" : "assistant", e.text});
	}

	// Rebuild committedContext using template if available
	if (!chatMessages.empty() && llama)
	{
		auto msgsWithSys = chatMessages;
		std::string effSys = GetEffectiveSystemPrompt();
		if (!effSys.empty())
			msgsWithSys.insert(msgsWithSys.begin(), {"system", effSys});
		std::string templated = llama->ApplyChatTemplate(msgsWithSys, false);
		if (!templated.empty())
			committedContext = templated;
	}
	if (committedContext.empty() && j.contains("committedContext"))
		committedContext = j["committedContext"].get<std::string>();

	scrollToBottom = true;

	// Rehydrate the KV cache by processing the committed context (no sampling)
	if (!committedContext.empty() && llama->HasModelLoaded())
	{
		MT_LlamaGenerateParams rehydrateParams;
		rehydrateParams.max_tokens = 0; // process prompt only, no generation
		llama->GenerateAsync(committedContext, rehydrateParams, nullptr, nullptr);
	}
}

void CGuiViewLlamaChat::SaveTokenDump(const std::string &path)
{
	if (!llama) return;

	auto rawTokens = llama->GetRawTokens();

	std::ofstream f(path);
	if (!f.is_open()) return;

	f << "=== Token Dump ===\n";
	f << "Total tokens: " << rawTokens.size() << "\n";
	f << "Model: " << llama->GetBackendName() << "\n";
	f << "committedContext length: " << committedContext.size() << "\n";

	// Chat template info
	std::string stopSeq = llama->GetChatStopSequence();
	f << "Stop sequence: \"" << stopSeq << "\"\n";

	// Template test
	std::vector<std::pair<std::string,std::string>> testMsgs = {{"user", "test"}};
	std::string tmplResult = llama->ApplyChatTemplate(testMsgs, true);
	f << "Chat template active: " << (tmplResult.empty() ? "NO (fallback to naive)" : "YES") << "\n";
	if (!tmplResult.empty())
		f << "Template sample: " << tmplResult << "\n";

	f << "\n=== Committed Context (prompt sent to model) ===\n";
	f << committedContext << "\n";

	f << "\n=== Raw Tokens (id | special_text) ===\n";
	for (size_t i = 0; i < rawTokens.size(); i++)
	{
		const auto &[id, piece] = rawTokens[i];
		// Show token ID, escaped piece
		f << "[" << i << "] id=" << id << " |";
		for (char c : piece)
		{
			if (c == '\n') f << "\\n";
			else if (c == '\r') f << "\\r";
			else if (c == '\t') f << "\\t";
			else f << c;
		}
		f << "|\n";
	}

	f << "\n=== Concatenated Output (with special tokens) ===\n";
	for (const auto &[id, piece] : rawTokens)
		f << piece;
	f << "\n";
}
