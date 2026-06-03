#include "CGuiViewTerminal.h"

extern "C" {
#include "tmt.h"
}

#include "imgui.h"
#include "SYS_KeyCodes.h"
#include "SYS_Main.h"
#include "DBG_Log.h"

#include <cstring>

// Static callback wrapper compatible with C function pointer (TMTCALLBACK)
static void TmtCallbackWrapper(tmt_msg_t msg, struct TMT *vt, const void *a, void *p)
{
	CGuiViewTerminal *view = (CGuiViewTerminal *)p;
	if (msg == TMT_MSG_ANSWER)
	{
		// Terminal needs to send a response (e.g. device attributes query)
		const char *answer = (const char *)a;
		if (answer)
		{
			view->SendData(answer);
		}
	}
	// TMT_MSG_UPDATE, TMT_MSG_MOVED — handled during render via dirty flags
	// TMT_MSG_BELL — ignore for now
}

CGuiViewTerminal::CGuiViewTerminal(const char *name, float posX, float posY, float posZ,
								   float sizeX, float sizeY, int cols, int rows)
	: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
	, cols(cols), rows(rows), cursorBlinkTimer(0), cursorVisible(true)
	, vt(NULL)
{
	vt = tmt_open(rows, cols, TmtCallbackWrapper, this, NULL);

	// Standard VGA/ANSI color palette
	colorTable[0]  = IM_COL32(0, 0, 0, 255);        // Black
	colorTable[1]  = IM_COL32(170, 0, 0, 255);      // Red
	colorTable[2]  = IM_COL32(0, 170, 0, 255);      // Green
	colorTable[3]  = IM_COL32(170, 85, 0, 255);     // Yellow/Brown
	colorTable[4]  = IM_COL32(0, 0, 170, 255);      // Blue
	colorTable[5]  = IM_COL32(170, 0, 170, 255);    // Magenta
	colorTable[6]  = IM_COL32(0, 170, 170, 255);    // Cyan
	colorTable[7]  = IM_COL32(170, 170, 170, 255);  // White
	colorTable[8]  = IM_COL32(85, 85, 85, 255);     // Bright Black
	colorTable[9]  = IM_COL32(255, 85, 85, 255);    // Bright Red
	colorTable[10] = IM_COL32(85, 255, 85, 255);    // Bright Green
	colorTable[11] = IM_COL32(255, 255, 85, 255);   // Bright Yellow
	colorTable[12] = IM_COL32(85, 85, 255, 255);    // Bright Blue
	colorTable[13] = IM_COL32(255, 85, 255, 255);   // Bright Magenta
	colorTable[14] = IM_COL32(85, 255, 255, 255);   // Bright Cyan
	colorTable[15] = IM_COL32(255, 255, 255, 255);  // Bright White
}

CGuiViewTerminal::~CGuiViewTerminal()
{
	if (vt)
		tmt_close(vt);
}

void CGuiViewTerminal::ProcessInput(const uint8_t *data, size_t len)
{
	std::lock_guard<std::mutex> lock(vtMutex);
	tmt_write(vt, (const char *)data, len);
}

void CGuiViewTerminal::RenderImGui()
{
	PreRenderImGui();

	std::lock_guard<std::mutex> lock(vtMutex);

	const TMTSCREEN *s = tmt_screen(vt);
	const TMTPOINT *c = tmt_cursor(vt);

	if (!s)
	{
		PostRenderImGui();
		return;
	}

	// Update cursor blink (toggle every 0.5s)
	cursorBlinkTimer += ImGui::GetIO().DeltaTime;
	if (cursorBlinkTimer >= 0.5f)
	{
		cursorBlinkTimer -= 0.5f;
		cursorVisible = !cursorVisible;
	}

	// Calculate cell size from font
	ImVec2 charSize = ImGui::CalcTextSize("W");
	float cellW = charSize.x;
	float cellH = charSize.y;

	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImDrawList *drawList = ImGui::GetWindowDrawList();

	// Draw background fill for entire terminal area
	drawList->AddRectFilled(origin,
		ImVec2(origin.x + cellW * s->ncol, origin.y + cellH * s->nline),
		colorTable[0]);

	char utf8buf[8];

	for (size_t r = 0; r < s->nline; r++)
	{
		const TMTLINE *line = s->lines[r];
		for (size_t col = 0; col < s->ncol; col++)
		{
			const TMTCHAR *ch = &line->chars[col];

			// Determine colors
			int fgIdx = (ch->a.fg == TMT_COLOR_DEFAULT) ? 7 : (int)ch->a.fg - 1;
			int bgIdx = (ch->a.bg == TMT_COLOR_DEFAULT) ? 0 : (int)ch->a.bg - 1;
			if (ch->a.bold && fgIdx < 8) fgIdx += 8;
			if (ch->a.reverse) { int tmp = fgIdx; fgIdx = bgIdx; bgIdx = tmp; }

			if (fgIdx < 0) fgIdx = 7;
			if (bgIdx < 0) bgIdx = 0;
			if (fgIdx > 15) fgIdx = 15;
			if (bgIdx > 15) bgIdx = 15;

			float x = origin.x + col * cellW;
			float y = origin.y + r * cellH;

			// Background (skip if black/default to avoid overdraw)
			if (bgIdx != 0)
			{
				drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + cellW, y + cellH), colorTable[bgIdx]);
			}

			// Character
			wchar_t wc = ch->c;
			if (wc > 0x20 && wc < 0x7F)
			{
				// Fast path for ASCII
				char ascii[2] = { (char)wc, 0 };
				drawList->AddText(ImVec2(x, y), colorTable[fgIdx], ascii);
			}
			else if (wc > 0x7F)
			{
				// Convert wchar_t to UTF-8
				int len = 0;
				if (wc < 0x80) { utf8buf[0] = (char)wc; len = 1; }
				else if (wc < 0x800) { utf8buf[0] = (char)(0xC0 | (wc >> 6)); utf8buf[1] = (char)(0x80 | (wc & 0x3F)); len = 2; }
				else if (wc < 0x10000) { utf8buf[0] = (char)(0xE0 | (wc >> 12)); utf8buf[1] = (char)(0x80 | ((wc >> 6) & 0x3F)); utf8buf[2] = (char)(0x80 | (wc & 0x3F)); len = 3; }
				else { utf8buf[0] = (char)(0xF0 | (wc >> 18)); utf8buf[1] = (char)(0x80 | ((wc >> 12) & 0x3F)); utf8buf[2] = (char)(0x80 | ((wc >> 6) & 0x3F)); utf8buf[3] = (char)(0x80 | (wc & 0x3F)); len = 4; }
				utf8buf[len] = 0;
				drawList->AddText(ImVec2(x, y), colorTable[fgIdx], utf8buf);
			}
		}
	}

	// Draw cursor
	if (cursorVisible && c)
	{
		float cx = origin.x + c->c * cellW;
		float cy = origin.y + c->r * cellH;
		drawList->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + cellW, cy + cellH), IM_COL32(200, 200, 200, 180));
	}

	// Reserve space so ImGui knows the content size
	ImGui::Dummy(ImVec2(cellW * s->ncol, cellH * s->nline));

	tmt_clean(vt);

	PostRenderImGui();
}

bool CGuiViewTerminal::KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	const char *seq = MapKeyToVT100(keyCode, isShift, isAlt, isControl);
	if (seq)
	{
		SendData(seq);
		return true;
	}

	// Handle Ctrl+letter combinations
	if (isControl && keyCode >= 'a' && keyCode <= 'z')
	{
		uint8_t ch = (uint8_t)(keyCode - 'a' + 1); // Ctrl+A = 0x01, etc.
		SendData(&ch, 1);
		return true;
	}
	if (isControl && keyCode >= 'A' && keyCode <= 'Z')
	{
		uint8_t ch = (uint8_t)(keyCode - 'A' + 1);
		SendData(&ch, 1);
		return true;
	}

	return false;
}

bool CGuiViewTerminal::KeyTextInput(const char *text)
{
	if (text && text[0])
	{
		SendData((const uint8_t *)text, strlen(text));
		return true;
	}
	return false;
}

const char *CGuiViewTerminal::MapKeyToVT100(u32 keyCode, bool isShift, bool isAlt, bool isControl)
{
	switch (keyCode)
	{
		case MTKEY_ARROW_UP:    return TMT_KEY_UP;
		case MTKEY_ARROW_DOWN:  return TMT_KEY_DOWN;
		case MTKEY_ARROW_LEFT:  return TMT_KEY_LEFT;
		case MTKEY_ARROW_RIGHT: return TMT_KEY_RIGHT;
		case MTKEY_ENTER:       return "\r";
		case MTKEY_BACKSPACE:   return TMT_KEY_BACKSPACE;
		case MTKEY_TAB:         return "\t";
		case MTKEY_ESC:         return TMT_KEY_ESCAPE;
		case MTKEY_HOME:        return TMT_KEY_HOME;
		case MTKEY_END:         return TMT_KEY_END;
		case MTKEY_PAGE_UP:     return TMT_KEY_PAGE_UP;
		case MTKEY_PAGE_DOWN:   return TMT_KEY_PAGE_DOWN;
		case MTKEY_INSERT:      return TMT_KEY_INSERT;
		case MTKEY_DELETE:      return "\x7F";
		case MTKEY_F1:          return TMT_KEY_F1;
		case MTKEY_F2:          return TMT_KEY_F2;
		case MTKEY_F3:          return TMT_KEY_F3;
		case MTKEY_F4:          return TMT_KEY_F4;
		case MTKEY_F5:          return TMT_KEY_F5;
		case MTKEY_F6:          return TMT_KEY_F6;
		case MTKEY_F7:          return TMT_KEY_F7;
		case MTKEY_F8:          return TMT_KEY_F8;
		case MTKEY_F9:          return TMT_KEY_F9;
		case MTKEY_F10:         return TMT_KEY_F10;
		default: return NULL;
	}
}

void CGuiViewTerminal::SendData(const char *data)
{
	if (writeCallback && data)
		writeCallback((const uint8_t *)data, strlen(data));
}

void CGuiViewTerminal::SendData(const uint8_t *data, size_t len)
{
	if (writeCallback)
		writeCallback(data, len);
}

void CGuiViewTerminal::SetWriteCallback(std::function<void(const uint8_t*, size_t)> callback)
{
	writeCallback = callback;
}
