#ifndef _GUI_MAIN_H_
#define _GUI_MAIN_H_

#include "SYS_Platform.h"
#include "SYS_Main.h"
#include "VID_Main.h"
#include "CGuiMain.h"
#include "CSlrString.h"
#include "SYS_KeyCodes.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "imguihelper.h"
#include "imgui_toggle.h"
#include "imgui_impl_sdl2.h"

#define GUI_GAP_WIDTH 2

void GUI_Init();
void GUI_Render();
void GUI_PostRenderEndFrame();
void GUI_LockMutex();
void GUI_UnlockMutex();

void GUI_ShowVirtualKeyboard();
void GUI_HideVirtualKeyboard();

class CGuiMain;
class CConfigStorage;
extern CGuiMain *guiMain;

struct ImGuiWindowSizeConstraints
{
	static void KeepContentAspect(ImGuiSizeCallbackData* data)
	{
		float *aspect = (float*)data->UserData;
		ImVec2 vMin = ImGui::GetWindowContentRegionMin();
		if (data->DesiredSize.x > data->DesiredSize.y)
		{
			data->DesiredSize.y = (data->DesiredSize.x / *aspect) + vMin.y;
		}
		else
		{
			data->DesiredSize.x = (data->DesiredSize.y - vMin.y) * *aspect;
		}
	}
};

void GUI_SetPressConsumed(bool isConsumed);

#endif
 
