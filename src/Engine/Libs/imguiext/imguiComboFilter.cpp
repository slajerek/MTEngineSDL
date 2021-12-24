#include "imguiComboFilter.h"
#include "imgui_internal.h"

// fuzzy-completion combobox code by kovewnikov, from: https://github.com/ocornut/imgui/issues/1658
// corrections and bug fixes by Marcin "Slajerek" Skoczylas

/*
 //  Demo:
const char *hints[] = {
				"AnimGraphNode_CopyBone",
				"ce skipaa",
				"ce skipscreen",
				"ce skipsplash",
				"ce skipsplashscreen",
				"client_unit.cpp",
				"letrograd",
				"level",
				"leveler",
				"MacroCallback.cpp",
				"Miskatonic university",
				"MockAI.h",
				"MockGameplayTasks.h",
				"MovieSceneColorTrack.cpp",
				"r.maxfps",
				"r.maxsteadyfps",
				"reboot",
				"rescale",
				"reset",
				"resource",
				"restart",
				"retrocomputer",
				"retrograd",
				"return",
				"slomo 10",
				"SVisualLoggerLogsList.h",
				"The Black Knight",
			};
			static ComboFilterState s = {0, false};
			static char buf[128];
			static bool once = false;
			if(!once) {
				memcpy(buf, hints[0], strlen(hints[0]) + 1);
				once = true;
			}
			
			if( ComboFilter("my combofilter", buf, IM_ARRAYSIZE(buf), hints, IM_ARRAYSIZE(hints), s, NULL) ) {
				//...picking has occurred
			}
*/

bool ImGui::ComboFilter(const char *label, char *buffer, int bufferlen, const char **hints, int num_hints, ComboFilterState &s, ComboFilterCallback *callback, ImGuiComboFlags flags)
{
	using namespace ImGui;
		
	s.selectionChanged = false;
	
	// Always consume the SetNextWindowSizeConstraint() call in our early return paths
	ImGuiContext& g = *GImGui;

	ImGuiWindow* window = GetCurrentWindow();
	if (window->SkipItems)
		return false;

	// this does not work
//	ImGui::SetFocusID(ImGui::GetID(label), window);
//	g.NavDisableHighlight = false;
	
	const ImGuiID popupId = window->GetID(label);
	bool popupIsAlreadyOpened = IsPopupOpen(popupId, 0); //ImGuiPopupFlags_AnyPopupLevel);
	bool popupNeedsToBeOpened = callback ? callback->ComboFilterShouldOpenPopupCallback(label, buffer, bufferlen, hints, num_hints, &s)
											: (buffer[0] != 0) && strcmp(buffer, hints[s.activeIdx]);
	bool popupJustOpened = false;
	
	IM_ASSERT((flags & (ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_NoPreview)) != (ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_NoPreview)); // Can't use both flags together

	const ImGuiStyle& style = g.Style;
	
	const float arrow_size = (flags & ImGuiComboFlags_NoArrowButton) ? 0.0f : GetFrameHeight();
	const ImVec2 label_size = CalcTextSize(label, NULL, true);
	const float expected_w = CalcItemWidth();
	const float w = (flags & ImGuiComboFlags_NoPreview) ? arrow_size : expected_w;
	const ImRect frame_bb(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + w, window->DC.CursorPos.y + label_size.y + style.FramePadding.y*2.0f));
	const ImRect total_bb(frame_bb.Min, ImVec2((label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f) + frame_bb.Max.x, frame_bb.Max.y));
	const float value_x2 = ImMax(frame_bb.Min.x, frame_bb.Max.x - arrow_size);
	ItemSize(total_bb, style.FramePadding.y);
	if (!ItemAdd(total_bb, popupId, &frame_bb))
		return false;
	
	
	bool hovered, held;
	bool pressed = ButtonBehavior(frame_bb, popupId, &hovered, &held);
	
	if(!popupIsAlreadyOpened) {
		const ImU32 frame_col = GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
		RenderNavHighlight(frame_bb, popupId);
		if (!(flags & ImGuiComboFlags_NoPreview))
			window->DrawList->AddRectFilled(frame_bb.Min, ImVec2(value_x2, frame_bb.Max.y), frame_col, style.FrameRounding, (flags & ImGuiComboFlags_NoArrowButton) ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
	}
	if (!(flags & ImGuiComboFlags_NoArrowButton))
	{
		ImU32 bg_col = GetColorU32((popupIsAlreadyOpened || hovered) ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
		ImU32 text_col = GetColorU32(ImGuiCol_Text);
		window->DrawList->AddRectFilled(ImVec2(value_x2, frame_bb.Min.y), frame_bb.Max, bg_col, style.FrameRounding, (w <= arrow_size) ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersRight);
		if (value_x2 + arrow_size - style.FramePadding.x <= frame_bb.Max.x)
			RenderArrow(window->DrawList, ImVec2(value_x2 + style.FramePadding.y, frame_bb.Min.y + style.FramePadding.y), text_col, ImGuiDir_Down, 1.0f);
	}
	
	if(!popupIsAlreadyOpened)
	{
		RenderFrameBorder(frame_bb.Min, frame_bb.Max, style.FrameRounding);
		if (buffer != NULL && !(flags & ImGuiComboFlags_NoPreview))
			
			RenderTextClipped(ImVec2(frame_bb.Min.x + style.FramePadding.x, frame_bb.Min.y + style.FramePadding.y), ImVec2(value_x2, frame_bb.Max.y), buffer, NULL, NULL, ImVec2(0.0f,0.0f));

		if ((pressed || g.NavActivateId == popupId || popupNeedsToBeOpened) && !popupIsAlreadyOpened)
		{
			if (window->DC.NavLayerCurrent == 0)
				window->NavLastIds[0] = popupId;
			OpenPopupEx(popupId);
			popupIsAlreadyOpened = true;
			popupJustOpened = true;
		}
	}
	
	if (label_size.x > 0)
	{
		RenderText(ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x, frame_bb.Min.y + style.FramePadding.y), label);
	}
	
	if (!popupIsAlreadyOpened) {
		return false;
	}
	
	const float totalWMinusArrow = w - arrow_size;
	struct ImGuiSizeCallbackWrapper {
		static void sizeCallback(ImGuiSizeCallbackData* data)
		{
			float* totalWMinusArrow = (float*)(data->UserData);
			data->DesiredSize = ImVec2(*totalWMinusArrow, 200.f);
		}
	};
	SetNextWindowSizeConstraints(ImVec2(0 ,0), ImVec2(totalWMinusArrow, 150.f), ImGuiSizeCallbackWrapper::sizeCallback, (void*)&totalWMinusArrow);

	char name[16];
	ImFormatString(name, IM_ARRAYSIZE(name), "##Combo_%02d", g.BeginPopupStack.Size); // Recycle windows based on depth

	// Peek into expected window size so we can position it
	if (ImGuiWindow* popup_window = FindWindowByName(name))
	{
		if (popup_window->WasActive)
		{
			ImVec2 size_expected = CalcWindowNextAutoFitSize(popup_window);
			if (flags & ImGuiComboFlags_PopupAlignLeft)
				popup_window->AutoPosLastDirection = ImGuiDir_Left;
			ImRect r_outer = GetPopupAllowedExtentRect(popup_window);
			ImVec2 pos = FindBestWindowPosForPopupEx(frame_bb.GetBL(), size_expected, &popup_window->AutoPosLastDirection, r_outer, frame_bb, ImGuiPopupPositionPolicy_ComboBox);
			
			pos.y -= label_size.y + style.FramePadding.y*2.0f;
			
			SetNextWindowPos(pos);
		}
	}
	
	// Horizontally align ourselves with the framed text
	ImGuiWindowFlags window_flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_Popup | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
//    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.FramePadding.x, style.WindowPadding.y));
	bool ret = Begin(name, NULL, window_flags);

	ImGui::PushItemWidth(ImGui::GetWindowWidth());
	ImGui::SetCursorPos(ImVec2(0.f, window->DC.CurrLineTextBaseOffset));
	if(popupJustOpened)
	{
		ImGui::SetKeyboardFocusHere(0);
	}
	
	bool done = InputTextEx("##inputText", NULL, buffer, bufferlen, ImVec2(0, 0),
							ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue,
							NULL, NULL);
	ImGui::PopItemWidth();

	if(s.activeIdx < 0) {
		IM_ASSERT(false); //Undefined behaviour
		return false;
	}
	
	
	if (!ret)
	{
		ImGui::EndChild();
		ImGui::PopItemWidth();
		EndPopup();
		IM_ASSERT(0);   // This should never happen as we tested for IsPopupOpen() above
		return false;
	}
	
	
	ImGuiWindowFlags window_flags2 =  ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus; //0; //ImGuiWindowFlags_HorizontalScrollbar
	ImGui::BeginChild("ChildL", ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y), false, window_flags2);
	
	struct fuzzy {
		static int score( const char *str1, const char *str2 ) {
			int score = 0, consecutive = 0, maxerrors = 0;
			int charNum = 0;
			while( *str1 && *str2 )
			{
				int is_leading = (*str1 & 64) && !(str1[1] & 64);
				if( (*str1 & ~32) == (*str2 & ~32) )
				{
					int had_separator = 0;
					if (charNum != 0)
					{
						had_separator = (str1[-1] <= 32);
					}
					
					int x = had_separator || is_leading ? 10 : consecutive * 5;
					consecutive = 1;
					score += x;
					++str2;
				}
				else
				{
					int x = -1, y = is_leading * -3;
					consecutive = 0;
					score += x;
					maxerrors += y;
				}
				++str1;
				++charNum;
			}
			return score + (maxerrors < -9 ? -9 : maxerrors);
		}
		static int search( const char *str, int num, const char *words[] ) {
			int scoremax = 0;
			int best = -1;
			for( int i = 0; i < num; ++i ) {
				int score = fuzzy::score( words[i], str );
				int record = ( score >= scoremax );
				int draw = ( score == scoremax );
				if( record ) {
					scoremax = score;
					if( !draw ) best = i;
					else best = best >= 0 && strlen(words[best]) < strlen(words[i]) ? best : i;
				}
			}
			return best;
		}
	};
	
	bool selectionChangedLocal = false;
	if (buffer[0] != '\0')
	{
		int new_idx = fuzzy::search( buffer, num_hints, hints );
		int idx = new_idx >= 0 ? new_idx : s.activeIdx;
		s.selectionChanged = s.activeIdx != idx;
		selectionChangedLocal = s.selectionChanged;
		s.activeIdx = idx;
	}
	
	bool arrowScroll = false;
	
	if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_UpArrow)))
	{
		if (s.activeIdx > 0)
		{
			s.activeIdx--;
			arrowScroll = true;
			ImGui::SetWindowFocus();
		}
	}
	if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_DownArrow)))
	{
		if (s.activeIdx < num_hints-1)
		{
			s.activeIdx++;
			arrowScroll = true;
			ImGui::SetWindowFocus();
		}
	}
	
	if(done && !arrowScroll) {
		CloseCurrentPopup();
	}
	
	bool done2 = false;
	
	for (int n = 0; n < num_hints; n++)
	{
		bool is_selected = n == s.activeIdx;
		if (is_selected && (IsWindowAppearing() || selectionChangedLocal))
		{
			 SetScrollHereY();
//            ImGui::SetItemDefaultFocus();
		}
		
		if (is_selected && arrowScroll)
		{
			SetScrollHereY();
		}
		
		if (ImGui::Selectable(hints[n], is_selected))
		{
			s.selectionChanged = s.activeIdx != n;
			s.activeIdx = n;
			strcpy(buffer, hints[n]);
			CloseCurrentPopup();
			
			done2 = true;
		}
	}
	
	if (arrowScroll)
	{
		strcpy(buffer, hints[s.activeIdx]);
		ImGuiWindow* window = FindWindowByName(name);
		const ImGuiID id = window->GetID("##inputText");
		ImGuiInputTextState* state = GetInputTextState(id);
		
		const char* buf_end = NULL;
		state->CurLenW = ImTextStrFromUtf8(state->TextW.Data, state->TextW.Size, buffer, NULL, &buf_end);
		state->CurLenA = (int)(buf_end - buffer);
		state->CursorClamp();
	}

	ImGui::EndChild();
	EndPopup();

	bool ret1 = (s.selectionChanged && !strcmp(hints[s.activeIdx], buffer));
	
	bool widgetRet = done || done2 || ret1;
	
	return widgetRet;
}

bool ImGui::ComboFilterCallback::ComboFilterShouldOpenPopupCallback(const char *label, char *buffer, int bufferlen, const char **hints, int num_hints, ImGui::ComboFilterState *s)
{
	return (buffer[0] != 0) && strcmp(buffer, hints[s->activeIdx]);
}

