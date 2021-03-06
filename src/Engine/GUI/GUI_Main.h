#ifndef _GUI_MAIN_H_
#define _GUI_MAIN_H_

#include "SYS_Platform.h"
#include "SYS_Main.h"
#include "VID_Main.h"
#include "CGuiMain.h"
#include "CSlrString.h"
#include "SYS_KeyCodes.h"

#include <GL/gl3w.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "imguihelper.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"

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
extern CConfigStorage *globalConfig;


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


//// TODO: code review this
//#define UTFString char
//#define MAKEUTF(val) val
//#define UTFALLOC(val) strdup(val)
//#define UTFALLOCFROMC(val) strdup(val)
//#define UTFTOC(val) (val)
//#define UTFRELEASE(val) free(val)
//

void GUI_SetPressConsumed(bool isConsumed);


#endif
 
