#ifndef _CGuiViewDebugLog_h_
#define _CGuiViewDebugLog_h_

// Note, when MT_DEBUG_LOGS is 1, i.e. the verbose log macros are live,
// then we can log messages also to debug log view by setting USE_DEBUG_LOG_TO_VIEW
// We can skip logging messages to view, leaving only default console,
// and then we have only options to switch/remember which levels to print in log

void SYS_InitApplicationGuiLogger();

#include "CGuiView.h"

// Logger code is based on ImGui's example from imgui_demo
class CGuiViewDebugLog : public CGuiView
{
public:
	CGuiViewDebugLog(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY);
	CGuiViewDebugLog(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, const char *titleI18nKey, const char *stableId);
	virtual ~CGuiViewDebugLog();

	virtual void RenderImGui();
	virtual void DoLogic();

	virtual bool HasContextMenuItems();
	virtual void RenderContextMenuItems();
	
	void RenderLevelSwitch(u32 level, const char *name);
	
#if defined(USE_DEBUG_LOG_TO_VIEW)
	//
	ImGuiTextBuffer     Buf;
	ImGuiTextFilter     Filter;
	ImVector<int>       LineOffsets; // Index to lines offset. We maintain this with AddLog() calls.
	bool                AutoScroll;  // Keep scrolling if already at the bottom.

	void Clear();
	void AddLog(const char* fmt, ...) IM_FMTARGS(2);
#endif
};

extern CGuiViewDebugLog *guiViewDebugLog;

#endif 
