#include "CGuiViewTableWithFilterAndButtons.h"
#include "CGuiViewTableItem.h"

using namespace ImGui;

CGuiViewTableWithFilterAndButtons::CGuiViewTableWithFilterAndButtons(const char *name, float posX, float posY, float sizeX, float sizeY)
: CGuiView(name, posX, posY, -1, sizeX, sizeY)
{
	tableSelectedRow = -1;
	previousNumberOfItems = 0;
	tableFilterBuf[0] = 0;
	tableRowSelectedByArrowKey = false;
}

void CGuiViewTableWithFilterAndButtons::RenderImGui()
{
	PreRenderImGui();
	RenderImGuiTableView();
	PostRenderImGui();
}

void CGuiViewTableWithFilterAndButtons::RenderImGuiTableView()
{
	// Space reserved for buttons at the bottom
	float buttonsHeight = TableGetButtonsHeight();

	// Calculate available height for table
	float tableHeight = ImGui::GetContentRegionAvail().y - buttonsHeight;

	ImU32 changedRowBgColor = ImGui::GetColorU32(ImVec4(0.7f, 0.3f, 0.3f, 0.65f));

	if (ImGui::InputTextWithHint("##Filter", "Search...", tableFilterBuf, IM_ARRAYSIZE(tableFilterBuf)))
	{
		tableVisibleItems.clear();
		TableAddFilteredItems(tableFilterBuf);
	}

	if (tableFilterBuf[0] == '\0' && tableVisibleItems.size() == 0)
	{
		tableVisibleItems.clear();
		TableAddFilteredItems(tableFilterBuf);
	}

	if (previousNumberOfItems != tableVisibleItems.size())
	{
		TableRefreshData();
		previousNumberOfItems = tableVisibleItems.size();
	}

	ImGui::BeginChild("ListTableChild", ImVec2(0, tableHeight), true, ImGuiWindowFlags_NoBackground);
	ImGuiStyle& style = ImGui::GetStyle();

	float backupItemSpacingY = style.ItemSpacing.y;
	float backupWindowPaddingX = style.WindowPadding.x;
	float backupWindowPaddingY = style.WindowPadding.y;
	style.ItemSpacing.y = 0.0f;
	style.WindowPadding = ImVec2(0, 0);

	if (BeginTable("##List", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable))
	{
		TableSetupColumns();

		ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();

		if (sortSpecs && (sortSpecs->SpecsDirty || sortSpecs != previousSortSpecs))
		{
			TableSortItems(sortSpecs, tableVisibleItems);
			sortSpecs->SpecsDirty = false;
			previousSortSpecs = sortSpecs;
		}

		ImGuiListClipper clipper;
		clipper.Begin(tableVisibleItems.size());

		char *buf = SYS_GetCharBuf();

		while (clipper.Step())
		{
			for (int row_n = clipper.DisplayStart; row_n < clipper.DisplayEnd; row_n++)
			{
				auto it = tableVisibleItems.begin();
				std::advance(it, row_n);
				if (it == tableVisibleItems.end())
					break;

				CGuiViewTableItem *item = *it;

				TableNextRow();
				TableNextColumn();

				TableGetFirstColumnText(item, buf);

				ImGui::PushID(item->GetID()); // Unique ID per row
				bool row_selected = (tableSelectedRow == item->GetID());
				if (Selectable(buf, row_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 0)))
				{
					tableSelectedRow = item->GetID();

					TableSelectedItem(item);

				}

				if (row_selected && tableRowSelectedByArrowKey)
				{
					ImGui::SetScrollHereY(0.5f);
					tableRowSelectedByArrowKey = false;
				}

				TableNextColumn();
				TableContinuePrintRow(item);

				ImGui::PopID();
			}
		}

		SYS_ReleaseCharBuf(buf);

		EndTable();
	}

	ImGui::EndChild(); // end table region
	style.ItemSpacing.y = backupItemSpacingY;
	style.WindowPadding = ImVec2(backupWindowPaddingX, backupWindowPaddingY);
	TableRenderButtons();

	//
	// Handle arrow key navigation
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
	{
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))
		{
			int currentIndex = -1;
			int i = 0;

			for (auto item : tableVisibleItems)
			{
				if (item->GetID() == tableSelectedRow)
				{
					currentIndex = i;
					break;
				}
				i++;
			}

			if (currentIndex != -1)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && currentIndex > 0)
				{
					auto it = tableVisibleItems.begin();
					std::advance(it, currentIndex - 1);
					tableSelectedRow = (*it)->GetID();
					TableSetSelectedItem(*it);
					tableRowSelectedByArrowKey = true;
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && currentIndex + 1 < (int)tableVisibleItems.size())
				{
					auto it = tableVisibleItems.begin();
					std::advance(it, currentIndex + 1);
					tableSelectedRow = (*it)->GetID();
					TableSetSelectedItem(*it);
					tableRowSelectedByArrowKey = true;
				}
			}
			else if (!tableVisibleItems.empty())
			{
				// If nothing selected, select the first item on arrow key
				auto it = tableVisibleItems.begin();
				tableSelectedRow = (*it)->GetID();
				TableSetSelectedItem(*it);
				tableRowSelectedByArrowKey = true;
			}
		}
	}
}

void CGuiViewTableWithFilterAndButtons::TableInit()
{
	tableSelectedRow = -1;
	previousNumberOfItems = -1;
	tableFilterBuf[0] = 0;
	TableSelectedItem(NULL);
}

void CGuiViewTableWithFilterAndButtons::TableSetSelectedItem(CGuiViewTableItem *item)
{
	for (CGuiViewTableItem *i : tableVisibleItems)
	{
		if (item == i)
		{
			tableSelectedRow = i->GetID();
			break;
		}
	}
	TableSelectedItem(item);
}

void CGuiViewTableWithFilterAndButtons::TableRefreshData()
{
	tableVisibleItems.clear();
	TableAddFilteredItems(tableFilterBuf);
}

// override this:
void CGuiViewTableWithFilterAndButtons::TableAddFilteredItems(const char *filterBuf)
{
	LOGWarning("CGuiViewTableWithFilterAndButtons::TableAddFilteredItems");
}

void CGuiViewTableWithFilterAndButtons::TableSortItems(const ImGuiTableSortSpecs* sort_specs, std::vector<CGuiViewTableItem *>& visible_items)
{
	LOGWarning("CGuiViewTableWithFilterAndButtons::SortItems: should be overridden (not implemented)");
}

void CGuiViewTableWithFilterAndButtons::TableSetupColumns()
{
}

// print column #0
void CGuiViewTableWithFilterAndButtons::TableGetFirstColumnText(CGuiViewTableItem *item, char *buf)
{
}

// print columns #1+
void CGuiViewTableWithFilterAndButtons::TableContinuePrintRow(CGuiViewTableItem *item)
{
}

void CGuiViewTableWithFilterAndButtons::TableSelectedItem(CGuiViewTableItem *item)
{
}

int  CGuiViewTableWithFilterAndButtons::TableGetButtonsHeight()
{
	return 0;
}

void CGuiViewTableWithFilterAndButtons::TableRenderButtons()
{
}


CGuiViewTableWithFilterAndButtons::~CGuiViewTableWithFilterAndButtons()
{

}
