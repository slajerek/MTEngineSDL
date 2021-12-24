#include "GUI_Main.h"
#include "CGuiMain.h"
#include "MT_API.h"
#include "CConfigStorage.h"

CGuiMain *guiMain = NULL;
CConfigStorage *globalConfig = NULL;

void GUI_Init()
{
	MT_GuiPreInit();
	guiMain = new CGuiMain();
}

void GUI_Render()
{
//	LOGD("GUI_Render");
	
	/*
	ImVec2 mainWindowPos = ImGui::GetCursorScreenPos();
	LOGD("main window pos: %5.2f %5.2f", mainWindowPos.x, mainWindowPos.y);
	
	ImGuiIO& io = ImGui::GetIO();
	
	float px = io.MousePos.x - mainWindowPos.x;
	float py = io.MousePos.y - mainWindowPos.y;

//	ImVec2 p = ImGui::GetCursorScreenPos();
	LOGD("mousePos relative: %5.2f %5.2f", px, py);
	
	guiMain->mousePosX = px;
	guiMain->mousePosY = py;
*/
	
	guiMain->RenderImGui();
	
	MT_Render();
	
	/*
	bool show_demo_window = false;
//	bool show_demo_window = true;
	
	if (show_demo_window)
		ImGui::ShowDemoWindow(&show_demo_window);
	
	
	
	ImGui::Begin("Test 1");
	ImGui::BulletText("MTEngineSDL ImGui test 123 abc");
	
	 
	 // GET WINDOW POS & SIZE
	ImVec2 vMin = ImGui::GetWindowContentRegionMin();
	vMin.x += ImGui::GetWindowPos().x;
	vMin.y += ImGui::GetWindowPos().y;
	
	ImVec2 vMax = ImGui::GetWindowContentRegionMax();
	vMax.x += ImGui::GetWindowPos().x;
	vMax.y += ImGui::GetWindowPos().y;
	
	 
	//		LOGD("1 pos %5.2f %5.2f | %5.2f %5.2f", vMin.x, vMin.y, vMax.x, vMax.y);
	
	ImGui::End();
	
	
	ImGui::Begin("Test 2");
	ImGui::BulletText("222");
	ImGui::End();
	*/
}

// TODO me

void GUI_ShowVirtualKeyboard() {}
void GUI_HideVirtualKeyboard() {}

void GUI_SetPressConsumed(bool isConsumed) {}

void GUI_LockMutex()
{
	VID_LockRenderMutex();
}

void GUI_UnlockMutex()
{
	VID_UnlockRenderMutex();
}
