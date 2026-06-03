// ImGuiToolbar.h — Reusable toolbar widget for ImGui
// Displays icon buttons in a flow layout (left-to-right, wrap to next row).
// Right-click opens a context menu with an icon scale slider.
//
// Two modes:
//   1. Default (wrapping): buttons use iconScale for size, wrap to next row.
//   2. Horizontal scaling: set horizontalScaling=true and numButtons before BeginToolbar.
//      Buttons fill available width equally and scale with the window.
//      Separator() is a no-op in this mode (all buttons stay on one row).
//
// Usage:
//   ImGuiToolbar toolbar;
//   toolbar.horizontalScaling = true;
//   toolbar.numButtons = 5;
//   if (toolbar.BeginToolbar("myToolbar")) {
//       if (toolbar.Button(ICON_FA_PLUS, "Add"))   { /* ... */ }
//       if (toolbar.Button(ICON_FA_TRASH, "Delete")) { /* ... */ }
//       toolbar.EndToolbar();
//   }

#pragma once

#include "imgui.h"

struct ImGuiToolbar
{
	float iconScale = 1.5f;   // persistent, caller stores the struct (used in default mode)

	// Horizontal scaling mode: all buttons on one row, sized to fill available width
	bool horizontalScaling = false;
	int  numButtons = 0;      // must be set when horizontalScaling is true

	bool BeginToolbar(const char* strId)
	{
		_strId = strId;
		ImGuiStyle& style = ImGui::GetStyle();

		_availWidth = ImGui::GetContentRegionAvail().x;
		_startCursorX = ImGui::GetCursorPosX();
		_isFirstButton = true;
		_isOpen = true;

		if (horizontalScaling && numButtons > 0)
		{
			float spacing = style.ItemSpacing.x;
			float maxByWidth = (_availWidth - spacing * (numButtons - 1)) / numButtons;
			float availHeight = ImGui::GetContentRegionAvail().y;
			_buttonSize = (maxByWidth < availHeight) ? maxByWidth : availHeight;
			if (_buttonSize < 20.0f) _buttonSize = 20.0f;
		}
		else
		{
			_buttonSize = style.FontSizeBase * iconScale;
		}

		ImGui::BeginGroup();
		return true;
	}

	bool Button(const char* icon, const char* tooltip = nullptr, bool enabled = true)
	{
		if (!_isOpen) return false;

		if (!_isFirstButton)
		{
			if (horizontalScaling)
			{
				ImGui::SameLine();
			}
			else
			{
				// Flow layout: wrap to next line if button won't fit
				float cursorX = ImGui::GetCursorPosX();
				float nextEnd = cursorX + ImGui::GetStyle().ItemSpacing.x + _buttonSize;
				if (nextEnd <= _startCursorX + _availWidth)
					ImGui::SameLine();
			}
		}
		_isFirstButton = false;

		if (!enabled)
			ImGui::BeginDisabled(true);

		// Render the icon glyph smaller than the button so it fits inside the
		// frame, then centre it on the glyph's visual bounding box. ImGui's
		// label centring uses the text line box (full font height + glyph
		// advance), which leaves icons visibly off-centre — so the glyph is
		// drawn manually instead.
		float fontSize = horizontalScaling ? (_buttonSize * 0.6f) : (_buttonSize * 0.7f);

		ImVec2 btnMin = ImGui::GetCursorScreenPos();
		ImGui::PushID(icon);
		bool pressed = ImGui::Button("##b", ImVec2(_buttonSize, _buttonSize));
		ImGui::PopID();

		// Decode the first UTF-8 code point of the icon string.
		unsigned int cp = 0;
		const unsigned char* s = (const unsigned char*)icon;
		if (s[0] < 0x80)                cp = s[0];
		else if ((s[0] & 0xE0) == 0xC0) cp = ((s[0] & 0x1Fu) << 6) | (s[1] & 0x3Fu);
		else if ((s[0] & 0xF0) == 0xE0) cp = ((s[0] & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
		else if ((s[0] & 0xF8) == 0xF0) cp = ((s[0] & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);

		ImFont* font = ImGui::GetFont();
		ImFontBaked* baked = font->GetFontBaked(fontSize);
		const ImFontGlyph* glyph = baked ? baked->FindGlyph((ImWchar)cp) : NULL;
		if (glyph)
		{
			ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
			ImVec2 pos(btnMin.x + _buttonSize * 0.5f - (glyph->X0 + glyph->X1) * 0.5f,
			           btnMin.y + _buttonSize * 0.5f - (glyph->Y0 + glyph->Y1) * 0.5f);
			ImGui::GetWindowDrawList()->AddText(font, fontSize, pos, col, icon);
		}

		if (!enabled)
			ImGui::EndDisabled();

		if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			ImGui::SetItemTooltip("%s", tooltip);

		return pressed;
	}

	void Separator()
	{
		if (horizontalScaling) return; // no-op in horizontal mode

		// Force wrap to next line + add vertical spacing
		_isFirstButton = true;
		ImGui::Spacing();
	}

	void EndToolbar()
	{
		if (!_isOpen) return;
		_isOpen = false;

		ImGui::EndGroup();

		// Right-click context menu on the toolbar area
		char popupId[128];
		snprintf(popupId, sizeof(popupId), "##toolbar_ctx_%s", _strId);
		ImGui::OpenPopupOnItemClick(popupId, ImGuiPopupFlags_MouseButtonRight);
		if (ImGui::BeginPopup(popupId))
		{
			if (!horizontalScaling)
				ImGui::SliderFloat("Icon Scale", &iconScale, 0.5f, 4.0f, "%.1f");
			ImGui::EndPopup();
		}
	}

private:
	// Per-frame state (reset in BeginToolbar)
	const char* _strId = nullptr;
	bool        _isOpen = false;
	float       _buttonSize = 0.0f;
	float       _availWidth = 0.0f;
	float       _startCursorX = 0.0f;
	bool        _isFirstButton = true;
};
