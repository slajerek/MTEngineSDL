#include "CViewAudioMixer.h"
#include "CGuiMain.h"
#include "SND_SoundEngine.h"
#include "CAudioChannel.h"
#include "CGuiFontManager.h"
#include <cmath>

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
	float dt = ImGui::GetIO().DeltaTime;

	gSoundEngine->LockMutex("CViewAudioMixer::RenderImGui");

	// Layout constants
	const float sliderWidth = 20.0f;
	const float vuWidth = 20.0f;
	const float waveformWidth = 60.0f;
	const float channelGap = 8.0f;
	const float bottomPadding = gDefaultFontSize * 4.0f;
	ImVec2 contentAvail = ImGui::GetContentRegionAvail();
	const float channelHeight = contentAvail.y - bottomPadding;

	if (channelHeight < 20.0f)
	{
		gSoundEngine->UnlockMutex("CViewAudioMixer::RenderImGui");
		SYS_ReleaseCharBuf(buf);
		PostRenderImGui();
		return;
	}

	// dB conversion: linear -> normalized [0,1] where 0=-60dB, 1=0dB
	auto toNormDb = [](float linear) -> float {
		if (linear < 0.001f) return 0.0f;
		float db = 20.0f * log10f(linear);
		if (db < -60.0f) db = -60.0f;
		return (db + 60.0f) / 60.0f;
	};

	ImDrawList *drawList = ImGui::GetWindowDrawList();
	ImVec2 windowPos = ImGui::GetCursorScreenPos();

	float xOffset = 0.0f;

	for (std::list<CAudioChannel *>::iterator it = audioChannels->begin(); it != audioChannels->end(); it++)
	{
		CAudioChannel *audioChannel = *it;
		float channelX = windowPos.x + xOffset;
		float channelY = windowPos.y;

		// --- Volume Slider ---
		sprintf(buf, "##VSlider%s", audioChannel->name);
		ImGui::SetCursorScreenPos(ImVec2(channelX, channelY));
		if (ImGui::VSliderFloat(buf, ImVec2(sliderWidth, channelHeight), &audioChannel->volume, 0.0f, 6.0f, ""))
		{
			audioChannel->StoreValuesToAppConfig();
		}

		// --- Read samples for VU and waveform ---
		// Request 2x display height for trigger search margin
		int waveformSamples = (int)channelHeight * 2;
		if (waveformSamples > CAudioChannel::PEEK_BUFFER_SIZE)
			waveformSamples = CAudioChannel::PEEK_BUFFER_SIZE;

		float *samples = new float[waveformSamples];
		int numPeeked = audioChannel->PeekRecentSamples(samples, waveformSamples);

		// Compute RMS and peak from last ~960 samples (or fewer)
		int vuSamples = numPeeked < 960 ? numPeeked : 960;
		float rms = 0.0f;
		float peak = 0.0f;
		if (vuSamples > 0)
		{
			int startIdx = numPeeked - vuSamples;
			for (int i = startIdx; i < numPeeked; i++)
			{
				float s = samples[i];
				rms += s * s;
				float absS = fabsf(s);
				if (absS > peak) peak = absS;
			}
			rms = sqrtf(rms / (float)vuSamples);
		}

		float rmsNorm = toNormDb(rms);
		float peakNorm = toNormDb(peak);

		// Peak hold with decay
		if (peakHoldLevels.find(audioChannel) == peakHoldLevels.end())
			peakHoldLevels[audioChannel] = 0.0f;

		float &peakHold = peakHoldLevels[audioChannel];
		if (peakNorm > peakHold)
			peakHold = peakNorm;
		else
			peakHold -= dt * 0.33f;
		if (peakHold < 0.0f) peakHold = 0.0f;

		// --- VU Meter ---
		float vuX = channelX + sliderWidth + 4.0f;
		float vuTop = channelY;
		float vuBottom = channelY + channelHeight;

		// Background
		drawList->AddRectFilled(ImVec2(vuX, vuTop), ImVec2(vuX + vuWidth, vuBottom),
								IM_COL32(30, 30, 30, 255));

		// RMS bar (bottom-up)
		float barHeight = rmsNorm * channelHeight;
		float barTop = vuBottom - barHeight;

		ImU32 barColor;
		if (rmsNorm < 0.8f)
			barColor = IM_COL32(0, 200, 0, 255);
		else if (rmsNorm < 0.95f)
			barColor = IM_COL32(220, 220, 0, 255);
		else
			barColor = IM_COL32(255, 0, 0, 255);

		if (barHeight > 0.5f)
			drawList->AddRectFilled(ImVec2(vuX, barTop), ImVec2(vuX + vuWidth, vuBottom), barColor);

		// Peak hold line
		if (peakHold > 0.01f)
		{
			float peakY = vuBottom - peakHold * channelHeight;
			drawList->AddLine(ImVec2(vuX, peakY), ImVec2(vuX + vuWidth, peakY),
							  IM_COL32(255, 255, 255, 200), 2.0f);
		}

		// --- Waveform ---
		float wfX = vuX + vuWidth + 4.0f;
		float wfCenterX = wfX + waveformWidth * 0.5f;
		float wfHalfWidth = waveformWidth * 0.45f;
		float wfTop = channelY;
		float wfBottom = channelY + channelHeight;

		// Background
		drawList->AddRectFilled(ImVec2(wfX, wfTop), ImVec2(wfX + waveformWidth, wfBottom),
								IM_COL32(20, 20, 30, 255));

		// Center line
		drawList->AddLine(ImVec2(wfCenterX, wfTop), ImVec2(wfCenterX, wfBottom),
						  IM_COL32(60, 60, 60, 255), 1.0f);

		// Waveform with rising-edge trigger stabilization
		if (numPeeked > 1)
		{
			ImU32 waveColor = IM_COL32(100, 255, 100, 200);
			// Use half the peeked buffer as display window (rest is trigger search margin)
			int drawSamples = numPeeked / 2;
			if (drawSamples < 2) drawSamples = 2;

			// Find trigger point using rising-edge zero-crossing
			// Search in the middle third of the buffer for stable triggering
			int searchStart = numPeeked / 4;
			int searchEnd = numPeeked * 3 / 4;
			if (searchEnd >= numPeeked) searchEnd = numPeeked - 1;

			// Find min/max in search region to compute trigger level
			float sMin = 1.0f, sMax = -1.0f;
			for (int i = searchStart; i < searchEnd; i++)
			{
				float v = samples[i];
				if (v < sMin) sMin = v;
				if (v > sMax) sMax = v;
			}
			float triggerLevel = (sMin + sMax) * 0.5f;

			// Find rising edge: first go below trigger, then above
			int triggerPos = searchStart;
			while (triggerPos < searchEnd && samples[triggerPos] > triggerLevel)
				triggerPos++;
			while (triggerPos < searchEnd && samples[triggerPos] <= triggerLevel)
				triggerPos++;

			// Center the display window on trigger
			int renderStart = triggerPos - drawSamples / 2;
			if (renderStart < 0) renderStart = 0;
			if (renderStart + drawSamples > numPeeked) renderStart = numPeeked - drawSamples;
			if (renderStart < 0) renderStart = 0;

			int actualDraw = numPeeked - renderStart;
			if (actualDraw > drawSamples) actualDraw = drawSamples;

			// Scale Y so the waveform always fills the view height
			float yStep = channelHeight / (float)(actualDraw > 1 ? actualDraw - 1 : 1);

			for (int i = 1; i < actualDraw; i++)
			{
				float s0 = samples[renderStart + i - 1];
				float s1 = samples[renderStart + i];

				if (s0 > 1.0f) s0 = 1.0f; else if (s0 < -1.0f) s0 = -1.0f;
				if (s1 > 1.0f) s1 = 1.0f; else if (s1 < -1.0f) s1 = -1.0f;

				float x0 = wfCenterX + s0 * wfHalfWidth;
				float x1 = wfCenterX + s1 * wfHalfWidth;
				float y0 = wfBottom - (float)(i - 1) * yStep;
				float y1 = wfBottom - (float)i * yStep;

				drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), waveColor, 1.0f);
			}
		}

		delete[] samples;

		// --- Below visualizations: Volume input, Name, Mute (flush-right) ---
		float channelTotalWidth = sliderWidth + vuWidth + waveformWidth + 8.0f;
		float belowY = channelY + channelHeight + 2.0f;

		// Row 1: Volume input (left) + Mute button (flush-right), same line
		sprintf(buf, "M##Mute%s", audioChannel->name);
		float muteButtonWidth = ImGui::CalcTextSize("M").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		float inputWidth = channelTotalWidth - muteButtonWidth - 4.0f;

		ImGui::SetCursorScreenPos(ImVec2(channelX, belowY));
		sprintf(buf, "##Vol%s", audioChannel->name);
		ImGui::SetNextItemWidth(inputWidth);
		if (ImGui::InputFloat(buf, &audioChannel->volume, 0, 0, "%1.1f"))
		{
			audioChannel->StoreValuesToAppConfig();
		}

		sprintf(buf, "M##Mute%s", audioChannel->name);
		float muteX = channelX + channelTotalWidth - muteButtonWidth;
		ImGui::SetCursorScreenPos(ImVec2(muteX, belowY));

		// Row 2: Channel name
		float row2Y = belowY + gDefaultFontSize * 1.5f;

		bool muted = audioChannel->isMuted;
		if (muted)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 0, 0, 1));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0, 0, 1));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 0, 0, 1));
		}
		if (ImGui::Button(buf))
		{
			audioChannel->isMuted = !audioChannel->isMuted;
			audioChannel->StoreValuesToAppConfig();
		}
		if (muted) ImGui::PopStyleColor(3);

		// Row 2: Channel name
		ImGui::SetCursorScreenPos(ImVec2(channelX, row2Y));
		ImGui::Text("%s", audioChannel->name);

		xOffset += sliderWidth + vuWidth + waveformWidth + 8.0f + channelGap;
	}

	// Clean up stale peak hold entries
	for (auto it = peakHoldLevels.begin(); it != peakHoldLevels.end(); )
	{
		bool found = false;
		for (auto *ch : *audioChannels)
		{
			if (ch == it->first) { found = true; break; }
		}
		if (!found)
			it = peakHoldLevels.erase(it);
		else
			++it;
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
