#include "CGuiViewSearch.h"

// Note, this mimics usage of Cmd+Shift+O from Xcode

void CGuiViewSearchCallback::GuiViewSearchCompleted(u32 index)
{
}


CGuiViewSearch::CGuiViewSearch(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, CGuiViewSearchCallback *callback)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	this->callback = callback;
	this->userData = NULL;
	
	imGuiSkipKeyPressWhenIoWantsTextInput = false;
	hideWindowOnFocusLost = false;
	
	viewJustActivated = false;
}

CGuiViewSearch::~CGuiViewSearch()
{
}

void CGuiViewSearch::ActivateView()
{
	viewJustActivated = true;
	CenterImGuiWindowPosition();
}

bool CGuiViewSearch::KeyDownRepeat(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return KeyDown(keyCode, isShift, isAlt, isControl, isSuper);
}

void CGuiViewSearch::CompleteSelection()
{
	if (numFilteredItems < 1)
	{
		ActivateView();
	}
	else
	{
		if (callback)
		{
			callback->GuiViewSearchCompleted(selectedItemIndex);
		}
		else
		{
			LOGError("CGuiViewSearch::KeyDown ENTER: callback is NULL");
		}
	}
}

bool CGuiViewSearch::KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	if (keyCode == MTKEY_ARROW_DOWN)
	{
//		if (filter.IsActive())
		{
			if (highlightedFilteredItemIndex < numFilteredItems-1)
			{
				highlightedFilteredItemIndex++;
				highlightedItemChanged = true;
			}
		}
	}
	else if (keyCode == MTKEY_ARROW_UP)
	{
//		if (filter.IsActive())
		{
			if (highlightedFilteredItemIndex > 0)
			{
				highlightedFilteredItemIndex--;
				highlightedItemChanged = true;
			}
		}
	}
	else if (keyCode == MTKEY_ENTER)
	{
		CompleteSelection();
	}
	else if (keyCode == MTKEY_ESC)
	{
		SetVisible(false);
	}
	else
	{
		highlightedFilteredItemIndex = 0;
	}

	return true;
}

void CGuiViewSearch::RenderImGui()
{
	PreRenderImGui();
	
	if (viewJustActivated)
	{
		ImGui::SetKeyboardFocusHere(0);
	}
	
	struct Callback
	{
		static int InputTextCallback(ImGuiInputTextCallbackData* data)
		{
			// when view just appeared let's select whole text
			CGuiViewSearch *viewSearch = (CGuiViewSearch*)data->UserData;
			if (viewSearch->viewJustActivated)
			{
				data->SelectAll();
				viewSearch->viewJustActivated = false;
			}
			return 0;
		}
	};
	
	//	Note, we do not use filter.Draw, to allow us to use ImGuiInputTextCallbackData to select text on window appear
	ImGui::SetNextItemWidth(-0.001f);
	bool value_changed = ImGui::InputText("##CGuiSearchFilter", filter.InputBuf, IM_ARRAYSIZE(filter.InputBuf), ImGuiInputTextFlags_CallbackAlways, Callback::InputTextCallback, this);
	if (value_changed)
		filter.Build();

	
	ImGui::Separator();
	
	ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

	bool completed = false;
	
	
//	if (filter.IsActive())
	{
		if (ImGui::BeginTable("Search", 1))
		{
//			LOGD("numItems=%d", items.size());
			numFilteredItems = 0;
			selectedItemIndex = 0;
			int f = 0;
			for (int i = 0; i < items.size(); i++)
			{
				const char* line_start = items[i];
				int len = strlen(line_start);
				const char* line_end = line_start + len;
				if (filter.PassFilter(line_start, line_end))
				{
					ImGui::TableNextColumn();
					if (f == highlightedFilteredItemIndex)
					{
						selectedItemIndex = i;
						ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(0, 0, 1, 1)));
						if (highlightedItemChanged)
						{
							ImGui::SetScrollHereY();
							highlightedItemChanged = false;
						}
					}
					else
					{
						ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImVec4(0, 0, 0, 1)));
					}
	//				LOGD("  %d = %s", i, items[i]);
					ImGui::Text(items[i]);
					
					if (ImGui::IsItemClicked())
					{
//						LOGD("clicked item %d", i);
						highlightedFilteredItemIndex = f;
					}
					
					if (ImGui::IsMouseDoubleClicked(0))
					{
						completed = true;
					}
					
					numFilteredItems++;
					f++;
				}
			}
			
			ImGui::EndTable();
		}
	}

	ImGui::EndChild();
	
	if (highlightedFilteredItemIndex >= numFilteredItems)
	{
		highlightedFilteredItemIndex = 0;
	}
	
	if (completed)
	{
		CompleteSelection();
	}
	
	PostRenderImGui();
}

void CGuiViewSearch::ClearItems()
{
	items.clear();
	highlightedFilteredItemIndex = 0;
}

bool CGuiViewSearch::FocusLost()
{
	if (hideWindowOnFocusLost)
	{
		SetVisible(false);
	}
	return true;
}
