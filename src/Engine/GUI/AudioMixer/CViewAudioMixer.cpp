#include "CViewAudioMixer.h"
#include "CGuiMain.h"
#include "SND_SoundEngine.h"
#include "CAudioChannel.h"
#include "VID_Fonts.h"

CViewAudioMixer::CViewAudioMixer(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, std::list<CAudioChannel *> *audioChannels)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	this->audioChannels = audioChannels;
}

CViewAudioMixer::~CViewAudioMixer()
{
}

void CViewAudioMixer::DoLogic()
{
	CGuiView::DoLogic();
}

void CViewAudioMixer::RenderImGui()
{
	PreRenderImGui();
	
	char *buf = SYS_GetCharBuf();

	gSoundEngine->LockMutex("CViewAudioMixer::RenderImGui");
		
	if (ImGui::BeginTable(this->name, audioChannels->size(), ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable | ImGuiTableFlags_Borders))
	{
		for (std::list<CAudioChannel *>::iterator it = audioChannels->begin(); it != audioChannels->end(); it++)
		{
			CAudioChannel *audioChannel = *it;

			sprintf(buf, "##AudioMixerVolumeSlider%s", audioChannel->name);
			
			ImGui::TableNextColumn();
			if (ImGui::VSliderFloat(buf, ImVec2(20, this->sizeY - gDefaultFontSize*6.0), &audioChannel->volume, 0.0f, 6.0f, ""))
			{
				audioChannel->StoreValuesToAppConfig();
			}
		}

		for (std::list<CAudioChannel *>::iterator it = audioChannels->begin(); it != audioChannels->end(); it++)
		{
			CAudioChannel *audioChannel = *it;

			sprintf(buf, "##AudioMixerVolumeValue%s", audioChannel->name);
			
			ImGui::TableNextColumn();
			if (ImGui::InputFloat(buf, &audioChannel->volume, 0, 0, "%1.1f"))
			{
				audioChannel->StoreValuesToAppConfig();
			}
		}

		for (std::list<CAudioChannel *>::iterator it = audioChannels->begin(); it != audioChannels->end(); it++)
		{
			CAudioChannel *audioChannel = *it;

			sprintf(buf, "MUTE##AudioMixerMute%s", audioChannel->name);
			
			ImGui::TableNextColumn();
			bool c = audioChannel->isMuted;
			if (c)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(255, 0, 0, 255));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(200, 0, 0, 255));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(255, 0, 0, 255));
			}
			if (ImGui::Button(buf))
			{
				audioChannel->isMuted = !audioChannel->isMuted;
				audioChannel->StoreValuesToAppConfig();
			}
			if (c)
			{
				ImGui::PopStyleColor(3);
			}
		}

		for (std::list<CAudioChannel *>::iterator it = audioChannels->begin(); it != audioChannels->end(); it++)
		{
			CAudioChannel *audioChannel = *it;

			ImGui::TableNextColumn();
			ImGui::Text("%s", audioChannel->name);
		}

		ImGui::EndTable();
	}
	
	
	gSoundEngine->UnlockMutex("CViewAudioMixer::RenderImGui");
	
	SYS_ReleaseCharBuf(buf);
	
	PostRenderImGui();
}

//@returns is consumed
bool CViewAudioMixer::DoTap(float x, float y)
{
	LOGG("CViewAudioMixer::DoTap:  x=%f y=%f", x, y);
	return CGuiView::DoTap(x, y);
}

bool CViewAudioMixer::DoFinishTap(float x, float y)
{
	LOGG("CViewAudioMixer::DoFinishTap: %f %f", x, y);
	return CGuiView::DoFinishTap(x, y);
}

//@returns is consumed
bool CViewAudioMixer::DoDoubleTap(float x, float y)
{
	LOGG("CViewAudioMixer::DoDoubleTap:  x=%f y=%f", x, y);
	return CGuiView::DoDoubleTap(x, y);
}

bool CViewAudioMixer::DoFinishDoubleTap(float x, float y)
{
	LOGG("CViewAudioMixer::DoFinishTap: %f %f", x, y);
	return CGuiView::DoFinishDoubleTap(x, y);
}


bool CViewAudioMixer::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	return CGuiView::DoMove(x, y, distX, distY, diffX, diffY);
}

bool CViewAudioMixer::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	return CGuiView::FinishMove(x, y, distX, distY, accelerationX, accelerationY);
}

bool CViewAudioMixer::DoRightClick(float x, float y)
{
	return CGuiView::DoRightClick(x, y);
}

bool CViewAudioMixer::DoFinishRightClick(float x, float y)
{
	return CGuiView::CGuiElement::DoFinishRightClick(x, y);
}

bool CViewAudioMixer::DoRightClickMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	return CGuiView::DoRightClickMove(x, y, distX, distY, diffX, diffY);
}

bool CViewAudioMixer::FinishRightClickMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	return CGuiView::CGuiElement::FinishRightClickMove(x, y, distX, distY, accelerationX, accelerationY);
}

bool CViewAudioMixer::DoNotTouchedMove(float x, float y)
{
	return CGuiView::DoNotTouchedMove(x, y);
}

bool CViewAudioMixer::InitZoom()
{
	return CGuiView::InitZoom();
}

bool CViewAudioMixer::DoZoomBy(float x, float y, float zoomValue, float difference)
{
	return CGuiView::DoZoomBy(x, y, zoomValue, difference);
}

bool CViewAudioMixer::DoScrollWheel(float deltaX, float deltaY)
{
	return CGuiView::DoScrollWheel(deltaX, deltaY);
}

bool CViewAudioMixer::DoMultiTap(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiTap(touch, x, y);
}

bool CViewAudioMixer::DoMultiMove(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiMove(touch, x, y);
}

bool CViewAudioMixer::DoMultiFinishTap(COneTouchData *touch, float x, float y)
{
	return CGuiView::DoMultiFinishTap(touch, x, y);
}

bool CViewAudioMixer::KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return CGuiView::KeyDown(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CViewAudioMixer::KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return CGuiView::KeyUp(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CViewAudioMixer::KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return CGuiView::KeyPressed(keyCode, isShift, isAlt, isControl, isSuper);
}

bool CViewAudioMixer::DoGamePadButtonDown(CGamePad *gamePad, u8 button)
{
	return CGuiView::DoGamePadButtonDown(gamePad, button);
}

bool CViewAudioMixer::DoGamePadButtonUp(CGamePad *gamePad, u8 button)
{
	return CGuiView::DoGamePadButtonUp(gamePad, button);
}

bool CViewAudioMixer::DoGamePadAxisMotion(CGamePad *gamePad, u8 axis, int value)
{
	return CGuiView::DoGamePadAxisMotion(gamePad, axis, value);
}

bool CViewAudioMixer::HasContextMenuItems()
{
	return false;
}

void CViewAudioMixer::RenderContextMenuItems()
{
}

void CViewAudioMixer::ActivateView()
{
	LOGG("CViewAudioMixer::ActivateView()");
}

void CViewAudioMixer::DeactivateView()
{
	LOGG("CViewAudioMixer::DeactivateView()");
}
