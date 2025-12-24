#include "DBG_Log.h"
#include "SYS_Defs.h"
#if !defined(GLOBAL_DEBUG_OFF)

#include "CGuiViewDebugLog.h"
#include "CGuiMain.h"
#include "SYS_DefaultConfig.h"

CGuiViewDebugLog *guiViewDebugLog = NULL;

void SYS_InitApplicationGuiLogger()
{
	if (guiViewDebugLog != NULL)
	{
		guiViewDebugLog = new CGuiViewDebugLog("Debug Log", 50, 50, -1, 200, 200);
	}

	u32 defaultLogLevel = LOG_GetCurrentLogLevel();
	int logLevel;
	gApplicationDefaultConfig->GetInt("LogLevel", &logLevel, (int)defaultLogLevel);
	LOG_SetCurrentLogLevel((u32)logLevel);
}

CGuiViewDebugLog::CGuiViewDebugLog(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{	
#if defined(USE_DEBUG_LOG_TO_VIEW)
	AutoScroll = true;
	Clear();
#endif
}

CGuiViewDebugLog::~CGuiViewDebugLog()
{
}
void CGuiViewDebugLog::DoLogic()
{
	CGuiView::DoLogic();
}

void CGuiViewDebugLog::RenderLevelSwitch(u32 level, const char *name)
{
	bool isSetLevel = LOG_IsSetLevel(level);
	if (ImGui::Checkbox(name, &isSetLevel))
	{
		LOG_SetLevel(level, isSetLevel);
		int logLevel = LOG_GetCurrentLogLevel();
		gApplicationDefaultConfig->SetInt("LogLevel", &logLevel);
	}
}

void CGuiViewDebugLog::RenderImGui()
{
	PreRenderImGui();

#if defined(FULL_LOG)
#else
	
	RenderLevelSwitch(DBGLVL_MAIN, "MAIN");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_DEBUG, "DEBUG");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_RES, "RES");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_INPUT, "INPUT");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_GUI, "GUI");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_AUDIO, "AUDIO");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_MEMORY, "MEMORY");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_SCRIPT, "DATA");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_DEBUG2, "DEBUG2");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_TODO, "TODO");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_WARN, "WARN");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_ERROR, "ERROR");

	RenderLevelSwitch(DBGLVL_VICE_DEBUG, "VICE-D");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_VICE_MAIN, "VICE");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_VICE_VERBOSE, "VICE-V");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_ATARI_DEBUG, "ATARI-D");
	ImGui::SameLine();
	RenderLevelSwitch(DBGLVL_ATARI_MAIN, "ATARI");
	
#endif

#if defined(USE_DEBUG_LOG_TO_VIEW)
	// Options menu
	if (ImGui::BeginPopup("Options"))
	{
		ImGui::Checkbox("Auto-scroll", &AutoScroll);
		ImGui::EndPopup();
	}

	// Main window
	if (ImGui::Button("Options"))
		ImGui::OpenPopup("Options");
	ImGui::SameLine();
	bool clear = ImGui::Button("Clear");
	ImGui::SameLine();
	bool copy = ImGui::Button("Copy");
	ImGui::SameLine();
	Filter.Draw("Filter", -100.0f);

	ImGui::Separator();
	
	/////////////////////////////////////////////////////////////
	LOG_LockMutex();
	
	ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

	if (clear)
		Clear();
	if (copy)
		ImGui::LogToClipboard();

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
	const char* buf = Buf.begin();
	const char* buf_end = Buf.end();
	if (Filter.IsActive())
	{
		// In this example we don't use the clipper when Filter is enabled.
		// This is because we don't have a random access on the result on our filter.
		// A real application processing logs with ten of thousands of entries may want to store the result of
		// search/filter.. especially if the filtering function is not trivial (e.g. reg-exp).
		for (int line_no = 0; line_no < LineOffsets.Size; line_no++)
		{
			const char* line_start = buf + LineOffsets[line_no];
			const char* line_end = (line_no + 1 < LineOffsets.Size) ? (buf + LineOffsets[line_no + 1] - 1) : buf_end;
			if (Filter.PassFilter(line_start, line_end))
				ImGui::TextUnformatted(line_start, line_end);
		}
	}
	else
	{
		// The simplest and easy way to display the entire buffer:
		//   ImGui::TextUnformatted(buf_begin, buf_end);
		// And it'll just work. TextUnformatted() has specialization for large blob of text and will fast-forward
		// to skip non-visible lines. Here we instead demonstrate using the clipper to only process lines that are
		// within the visible area.
		// If you have tens of thousands of items and their processing cost is non-negligible, coarse clipping them
		// on your side is recommended. Using ImGuiListClipper requires
		// - A) random access into your data
		// - B) items all being the  same height,
		// both of which we can handle since we an array pointing to the beginning of each line of text.
		// When using the filter (in the block of code above) we don't have random access into the data to display
		// anymore, which is why we don't use the clipper. Storing or skimming through the search result would make
		// it possible (and would be recommended if you want to search through tens of thousands of entries).
		ImGuiListClipper clipper;
		clipper.Begin(LineOffsets.Size);
		while (clipper.Step())
		{
			for (int line_no = clipper.DisplayStart; line_no < clipper.DisplayEnd; line_no++)
			{
				const char* line_start = buf + LineOffsets[line_no];
				const char* line_end = (line_no + 1 < LineOffsets.Size) ? (buf + LineOffsets[line_no + 1] - 1) : buf_end;
				ImGui::TextUnformatted(line_start, line_end);
			}
		}
		clipper.End();
	}
	ImGui::PopStyleVar();

	if (AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
		ImGui::SetScrollHereY(1.0f);

	LOG_UnlockMutex();
	ImGui::EndChild();
#endif
	
	PostRenderImGui();
}

#if defined(USE_DEBUG_LOG_TO_VIEW)

void CGuiViewDebugLog::Clear()
{
	Buf.clear();
	LineOffsets.clear();
	LineOffsets.push_back(0);
}

void CGuiViewDebugLog::AddLog(const char* fmt, ...) IM_FMTARGS(2)
{
	int old_size = Buf.size();
	va_list args;
	va_start(args, fmt);
	Buf.appendfv(fmt, args);
	va_end(args);
	for (int new_size = Buf.size(); old_size < new_size; old_size++)
		if (Buf[old_size] == '\n')
			LineOffsets.push_back(old_size + 1);
}
#endif

bool CGuiViewDebugLog::HasContextMenuItems()
{
	return false;
}

void CGuiViewDebugLog::RenderContextMenuItems()
{
}


#else

void SYS_InitApplicationGuiLogger()
{
}

#endif
