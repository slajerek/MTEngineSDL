#pragma once

#include "SYS_Defs.h"
#include "CGuiView.h"

class CGuiViewTableItem;

class CGuiViewTableWithFilterAndButtons : public CGuiView
{
public:
	CGuiViewTableWithFilterAndButtons(const char *name, float posX, float posY, float sizeX, float sizeY);
	CGuiViewTableWithFilterAndButtons(const char *name, float posX, float posY, float sizeX, float sizeY, const char *titleI18nKey, const char *stableId);

	virtual ~CGuiViewTableWithFilterAndButtons();

	virtual void RenderImGui();
	virtual void RenderImGuiTableView();

	std::vector<CGuiViewTableItem *> tableVisibleItems;
	int tableSelectedRow;
	char tableFilterBuf[1024];
	ImGuiTableSortSpecs* previousSortSpecs = nullptr;
	int previousNumberOfItems;

	virtual void TableInit();

	virtual void TableSetSelectedItem(CGuiViewTableItem *item);
	virtual void TableRefreshData();

	virtual void TableAddFilteredItems(const char *filterBuf);
	virtual void TableSortItems(const ImGuiTableSortSpecs* sort_specs, std::vector<CGuiViewTableItem *>& visible_items);
	virtual int  TableGetColumnCount();
	virtual float TableGetRowHeight();  // 0 = default (text height); override when middle columns use taller widgets
	virtual void TableSetupColumns();
	virtual void TableGetFirstColumnText(CGuiViewTableItem *item, char *buf);
	virtual void TableRenderMiddleColumns(CGuiViewTableItem *item);
	virtual void TableContinuePrintRow(CGuiViewTableItem *item);
	virtual void TableSelectedItem(CGuiViewTableItem *item);
	virtual void TableRenderButtons();
	virtual int  TableGetButtonsHeight();

private:
	bool tableRowSelectedByArrowKey;
};
