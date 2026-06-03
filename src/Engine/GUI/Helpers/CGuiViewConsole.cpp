#include "CGuiViewConsole.h"
#include "SYS_Threading.h"
#include "SYS_KeyCodes.h"
#include "SYS_SharedMemory.h"
#include "CSlrString.h"
#include "CSlrFont.h"
#include <cmath>

#define INVERT_CHAR 0x80

CGuiViewConsole::CGuiViewConsole(float posX, float posY, float posZ, float sizeX, float sizeY,
								 CSlrFont *font, float fontScale, int numLines, bool hasCommandLine, CGuiViewConsoleCallback *callback)
: CGuiView(posX, posY, posZ, sizeX, sizeY)
{
	imGuiNoWindowPadding = true;
	imGuiNoScrollbar = true;

	this->font = font;
	this->prompt[0] = 0x00;
	this->commandLine[0] = 0x00;
	this->SetFontScale(fontScale);
	this->SetNumLines(numLines);
	this->callback = callback;
	
	this->hasCommandLine = hasCommandLine;
	
	promptWidth = 0;
	
	maxCharsInLine = 61;
	
	numScrollLines = 0;
	numLinesInBuffer = 0;
	for (int i = 0; i < MAX_CONSOLE_SCROLL_LINES; i++)
	{
		lines[i] = NULL;
	}
	
	mutex = new CSlrMutex("CGuiViewConsole");
	
	lineHeight = font->GetLineHeight();

	textColorR = textColorG = textColorB = textColorA = 1.0f;

	hasSelection = false;
	isSelecting = false;
	selAnchor.row = selAnchor.col = 0;
	selCaret.row = selCaret.col = 0;

	ResetCommandLine();
}

CGuiViewConsole::~CGuiViewConsole()
{
	for (int i = 0; i < MAX_CONSOLE_SCROLL_LINES; i++)
	{
		if (lines[i] != NULL)
			delete [] lines[i];
	}
	delete mutex;
}

void CGuiViewConsole::SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY)
{
	CGuiView::SetPosition(posX, posY, posZ, sizeX, sizeY);
	this->SetNumLines(floor( ((float)sizeY / (float)lineHeight) - 1.5f) );
}

void CGuiViewConsole::SetLineHeight(float height)
{
	this->lineHeight = height;
	this->SetNumLines(floor( ((float)sizeY / (float)lineHeight) - 1.5f) );
}

void CGuiViewConsole::SetFontScale(float fontScale)
{
//	LOGD("CGuiViewConsole::SetFontScale: %f", fontScale);
	this->fontScale = fontScale;

	promptWidth = this->font->GetTextWidth(this->prompt, fontScale);
	lineHeight = font->GetCharHeight('@', fontScale); // * 8.0f;
	
	this->SetNumLines(floor( ((float)sizeY / (float)lineHeight) - 1.5f) );
}

void CGuiViewConsole::SetNumLines(int numLines)
{
	this->numLines = numLines;
}

void CGuiViewConsole::SetPrompt(char *prompt)
{
	strncpy(this->prompt, prompt, 254);
	this->prompt[255] = 0x00;
	
	promptWidth = this->font->GetTextWidth(this->prompt, fontScale);
}

void CGuiViewConsole::ResetCommandLine()
{
	// store history
	int len = strlen(commandLine);
	if (len > 0)
	{
		char *cmd = new char[len+1];
		strcpy(cmd, commandLine);
		commandLineHistory.push_back(cmd);
		
		if (commandLineHistory.size() > MAX_COMMAND_LINE_HISTORY)
		{
			cmd = commandLineHistory.front();
			commandLineHistory.pop_front();
			
			delete [] cmd;
		}
	}
	
	memset(commandLine, 0x00, MAX_CONSOLE_LINE_LENGTH);
	commandLineCursorPos = 0;
	commandLineHistoryIt = commandLineHistory.end();
}

void CGuiViewConsole::PrintSingleLine(char *text)
{
	mutex->Lock();

	if (lines[0] != NULL)
	{
		delete [] lines[0];
	}
	
	for (int i = 1; i < MAX_CONSOLE_SCROLL_LINES; i++)
	{
		lines[i-1] = lines[i];
	}
	
	int len = strlen(text);
	char *buf = new char [len+1];
	strcpy(buf, text);
	lines[MAX_CONSOLE_SCROLL_LINES-1] = buf;
	
	numLinesInBuffer++;
	if (numLinesInBuffer == MAX_CONSOLE_SCROLL_LINES+1)
	{
		numLinesInBuffer = MAX_CONSOLE_SCROLL_LINES;
	}

	OnLineAppended();
	mutex->Unlock();
}

void CGuiViewConsole::OnLineAppended()
{
	if (!hasSelection)
		return;
	if (selAnchor.row != CONSOLE_ROW_COMMANDLINE) selAnchor.row--;
	if (selCaret.row  != CONSOLE_ROW_COMMANDLINE) selCaret.row--;
	// if any real row scrolled past the top of storage, drop the selection
	if ((selAnchor.row != CONSOLE_ROW_COMMANDLINE && selAnchor.row < 0)
		|| (selCaret.row != CONSOLE_ROW_COMMANDLINE && selCaret.row < 0))
	{
		hasSelection = false;
	}
}

void CGuiViewConsole::PrintLine(CSlrString *str)
{
	char *buf = str->GetUTF8();
	
	this->PrintSingleLine(buf);
	
	delete [] buf;
}

void CGuiViewConsole::PrintLine(const char *format, ...)
{
	char buffer[MAX_CONSOLE_LINE_LENGTH];
	memset(buffer, 0x00, MAX_CONSOLE_LINE_LENGTH);
	
	va_list args;
	
	va_start(args, format);
	vsnprintf(buffer, MAX_CONSOLE_LINE_LENGTH, format, args);
	va_end(args);
	
	PrintSingleLine(buffer);
}

void CGuiViewConsole::PrintString(char *text)
{
	char *buffer = new char[MAX_CONSOLE_LINE_LENGTH];
	memset(buffer, 0x00, MAX_CONSOLE_LINE_LENGTH);
	
	char *t = text;
	int charsCount = 0;
	while(*t != 0x00)
	{
		if (*t == '\r')
		{
			t++;
			continue;
		}
		
		if (*t == '\n')
		{
			t++;
			buffer[charsCount] = 0x00;
			charsCount = maxCharsInLine;
		}
		
		if (charsCount == maxCharsInLine)
		{
			buffer[charsCount] = 0x00;
			PrintSingleLine(buffer);
			charsCount = 0;
		}
		
		buffer[charsCount] = *t;
		
		charsCount++;
		t++;
	}
}

bool CGuiViewConsole::KeyDown(u32 keyCode)
{
	LOGD("CGuiViewConsole::KeyDown: %c %x", keyCode, keyCode);
	if (!hasCommandLine)
		return false;
	
	bool consumed = false;
	mutex->Lock();
	
	if (keyCode == MTKEY_ENTER)
	{
		consumed = true;
		callback->GuiViewConsoleExecuteCommand(this->commandLine);
	}
	else if (keyCode == MTKEY_BACKSPACE)
	{
		consumed = true;
		if (commandLineCursorPos > 0)
		{
			char *s = commandLine + commandLineCursorPos;
			char *d = commandLine + commandLineCursorPos-1;
			for (int i = 0; i < MAX_CONSOLE_LINE_LENGTH-commandLineCursorPos-1; i++)
			{
				*d = *s;
				d++; s++;
			}
			
			commandLine[MAX_CONSOLE_LINE_LENGTH-commandLineCursorPos-1] = 0x00;
			commandLineCursorPos--;
		}
	}
	else if (keyCode == MTKEY_ARROW_LEFT)
	{
		consumed = true;
		if (commandLineCursorPos > 0)
			commandLineCursorPos--;
	}
	else if (keyCode == MTKEY_ARROW_RIGHT)
	{
		consumed = true;
		if (commandLineCursorPos < MAX_CONSOLE_LINE_LENGTH-2)
		{
			if (commandLine[commandLineCursorPos] != 0x00)
			{
				commandLineCursorPos++;
			}
		}
	}
	else if (keyCode == MTKEY_ARROW_UP)
	{
		consumed = true;
		if (!commandLineHistory.empty())
		{
			if (commandLineHistoryIt == commandLineHistory.end())
			{
				strcpy(backupCommandLine, commandLine);
			}

			if (commandLineHistoryIt != commandLineHistory.begin())
			{
				commandLineHistoryIt--;
				strcpy(commandLine, *commandLineHistoryIt);
				commandLineCursorPos = strlen(commandLine);
			}			
		}
	}
	else if (keyCode == MTKEY_ARROW_DOWN)
	{
		consumed = true;
		if (!commandLineHistory.empty())
		{
			if (commandLineHistoryIt != commandLineHistory.end())
			{
				commandLineHistoryIt++;
				if (commandLineHistoryIt != commandLineHistory.end())
				{
					strcpy(commandLine, *commandLineHistoryIt);
				}
				else
				{
					strcpy(commandLine, backupCommandLine);
				}
				commandLineCursorPos = strlen(commandLine);
			}
		}
	}
	else if (keyCode > MTKEY_SPECIAL_KEYS_START)
	{
	}
	else
	{
//		consumed = true;
//		LOGD("commandLine=%s", commandLine);
//		if (commandLine[commandLineCursorPos] == 0x00)
//		{
//			commandLine[commandLineCursorPos+1] = 0x00;
//		}
//		else
//		{
//			// move chars right
//			for (int i = MAX_CONSOLE_LINE_LENGTH-2; i >= commandLineCursorPos; i--)
//			{
//				commandLine[i+1] = commandLine[i];
//			}
//		}
//		commandLine[commandLineCursorPos] = keyCode;
//		if (commandLineCursorPos < MAX_CONSOLE_LINE_LENGTH-3)
//			commandLineCursorPos++;
	}
	
	mutex->Unlock();
	return consumed;
}

bool CGuiViewConsole::KeyTextInput(const char *text)
{
	mutex->Lock();
	u8 keyCode = text[0];
	LOGD("commandLine=%s", commandLine);
	if (commandLine[commandLineCursorPos] == 0x00)
	{
		commandLine[commandLineCursorPos+1] = 0x00;
	}
	else
	{
		// move chars right
		for (int i = MAX_CONSOLE_LINE_LENGTH-2; i >= commandLineCursorPos; i--)
		{
			commandLine[i+1] = commandLine[i];
		}
	}
	commandLine[commandLineCursorPos] = keyCode;
	if (commandLineCursorPos < MAX_CONSOLE_LINE_LENGTH-3)
		commandLineCursorPos++;

	mutex->Unlock();
	return true;
}

void CGuiViewConsole::Render()
{
//	LOGD("fontScale: %f lineHeight=%f", fontScale, lineHeight);

	float lineHeightThisLine = lineHeight;
	mutex->Lock();

	float px = posX + 1.5f;
	float py = posY + 3.0f;

	bool haveSel = hasSelection;
	CConsoleTextPos mn, mx;
	if (haveSel) OrderedSelection(&mn, &mx);
	const float hr = 0.30f, hg = 0.45f, hb = 0.95f, ha = 0.45f; // translucent blue

	int lineNum = MAX_CONSOLE_SCROLL_LINES - numLines - numScrollLines;

	for (int i = 0; i < numLines; i++)
	{
		if (lineNum >= 0)
		{
			if (haveSel && lineNum >= mn.row && lineNum <= mx.row
				&& lineNum != CONSOLE_ROW_COMMANDLINE)
			{
				int len = GetRowLength(lineNum);
				int sCol = (lineNum == mn.row) ? mn.col : 0;
				int eCol = (lineNum == mx.row) ? mx.col : len;
				if (sCol < 0) sCol = 0;
				if (eCol > len) eCol = len;
				if (eCol > sCol)
				{
					float xs = RowColToPixelX(lineNum, sCol);
					float xe = RowColToPixelX(lineNum, eCol);
					BlitFilledRectangle(xs, py, posZ, xe - xs, lineHeightThisLine, hr, hg, hb, ha);
				}
			}

			char *lineText = lines[lineNum];
			if (lineText != NULL)
				font->BlitTextColor(lineText, px, py, posZ, fontScale, textColorR, textColorG, textColorB, textColorA);
		}
		py += lineHeightThisLine;
		lineNum++;
	}

	if (hasCommandLine)
	{
		if (haveSel && mx.row == CONSOLE_ROW_COMMANDLINE)
		{
			int len = (int)strlen(commandLine);
			int sCol = (mn.row == CONSOLE_ROW_COMMANDLINE) ? mn.col : 0;
			int eCol = mx.col;
			if (sCol < 0) sCol = 0;
			if (eCol > len) eCol = len;
			if (eCol > sCol)
			{
				float xs = RowColToPixelX(CONSOLE_ROW_COMMANDLINE, sCol);
				float xe = RowColToPixelX(CONSOLE_ROW_COMMANDLINE, eCol);
				BlitFilledRectangle(xs, py, posZ, xe - xs, lineHeightThisLine, hr, hg, hb, ha);
			}
		}

		font->BlitTextColor(this->prompt, px, py, posZ, fontScale, textColorR, textColorG, textColorB, textColorA);
		px += promptWidth;

		bool cursorPainted = false;
		int l = strlen(commandLine);
		for (int i = 0; i < l; i++)
		{
			if (commandLine[i] == 0x00) break;
			u16 chrOrig = commandLine[i];
			u16 chrDraw = chrOrig;
			if (i == commandLineCursorPos) { cursorPainted = true; chrDraw += INVERT_CHAR; }
			font->BlitCharColor(chrDraw, px, py, posZ, fontScale, textColorR, textColorG, textColorB, textColorA);
			px += font->GetCharWidth(chrOrig, fontScale);
		}
		if (cursorPainted == false)
		{
			const u16 cursorChar = ' ' + INVERT_CHAR;
			font->BlitCharColor(cursorChar, px, py, posZ, fontScale, textColorR, textColorG, textColorB, textColorA);
		}
	}

	mutex->Unlock();
}

bool CGuiViewConsole::DoScrollWheel(float deltaX, float deltaY)
{
	LOGD("CGuiViewConsole::DoScrollWheel: %f", deltaY);

	if (numLinesInBuffer < numLines)
		return true;

	numScrollLines += (int)deltaY;

	if (numScrollLines < 0)
	{
		numScrollLines = 0;
	}

	if (numScrollLines > numLinesInBuffer-numLines)
	{
		numScrollLines = numLinesInBuffer-numLines;
	}

	LOGD("numLines=%d numScrollLines=%d numLinesInBuffer=%d",
		 numLines, numScrollLines, numLinesInBuffer);
	return true;
}

const char *CGuiViewConsole::GetRowText(int row)
{
	if (row == CONSOLE_ROW_COMMANDLINE)
		return commandLine;
	if (row < 0 || row >= MAX_CONSOLE_SCROLL_LINES)
		return NULL;
	return lines[row];
}

int CGuiViewConsole::GetRowLength(int row)
{
	const char *t = GetRowText(row);
	return t ? (int)strlen(t) : 0;
}

bool CGuiViewConsole::PosLess(const CConsoleTextPos &a, const CConsoleTextPos &b)
{
	if (a.row != b.row) return a.row < b.row;
	return a.col < b.col;
}

void CGuiViewConsole::OrderedSelection(CConsoleTextPos *outMin, CConsoleTextPos *outMax) const
{
	if (PosLess(selCaret, selAnchor)) { *outMin = selCaret; *outMax = selAnchor; }
	else                              { *outMin = selAnchor; *outMax = selCaret; }
}

void CGuiViewConsole::ClearSelection()
{
	hasSelection = false;
}

float CGuiViewConsole::RowColToPixelX(int row, int col)
{
	float cx = posX + 1.5f;
	if (row == CONSOLE_ROW_COMMANDLINE)
		cx += promptWidth;
	const char *t = GetRowText(row);
	if (!t) return cx;
	int len = (int)strlen(t);
	if (col > len) col = len;
	for (int i = 0; i < col; i++)
		cx += font->GetCharWidth(t[i], fontScale);
	return cx;
}

std::string CGuiViewConsole::BuildSelectionText()
{
	if (!hasSelection)
		return std::string();

	CConsoleTextPos mn, mx;
	OrderedSelection(&mn, &mx);

	std::string out;
	for (int row = mn.row; row <= mx.row; row++)
	{
		const char *t = GetRowText(row);
		int len = t ? (int)strlen(t) : 0;
		int startCol = (row == mn.row) ? mn.col : 0;
		int endCol   = (row == mx.row) ? mx.col : len;
		if (startCol < 0) startCol = 0;
		if (endCol > len) endCol = len;
		if (endCol > startCol && t)
			out.append(t + startCol, t + endCol);
		if (row != mx.row)
			out.push_back('\n');
		if (row == CONSOLE_ROW_COMMANDLINE)
			break; // command line is the final row in any ordering
	}
	return out;
}

void CGuiViewConsole::PasteText(const char *text)
{
	if (text == NULL) return;
	// CSlrMutex wraps std::recursive_mutex (non-SDL path), so the same thread can
	// re-enter it. The production callback (CViewMonitorConsole::GuiViewConsoleExecuteCommand)
	// calls mutex->Lock() itself — that is safe here because of recursion.
	// We therefore hold the mutex across the entire paste loop, which keeps the
	// command-line buffer consistent against concurrent render/keyboard threads.
	mutex->Lock();
	for (const char *p = text; *p; p++)
	{
		char ch = *p;
		if (ch == '\r')
			continue;
		if (ch == '\n')
		{
			// execute current command line via the same path as Enter;
			// the callback records history and resets the command line
			// (buffer AND cursor) on every path — do not touch cursor here.
			if (callback)
				callback->GuiViewConsoleExecuteCommand(commandLine);
			continue;
		}
		// insert printable char at cursor (single-line, capped at 512)
		if (commandLineCursorPos < MAX_CONSOLE_LINE_LENGTH - 2)
		{
			if (commandLine[commandLineCursorPos] == 0x00)
			{
				commandLine[commandLineCursorPos + 1] = 0x00;
			}
			else
			{
				for (int i = MAX_CONSOLE_LINE_LENGTH - 2; i >= commandLineCursorPos; i--)
					commandLine[i + 1] = commandLine[i];
			}
			commandLine[commandLineCursorPos] = ch;
			commandLineCursorPos++;
		}
	}
	mutex->Unlock();
}

void CGuiViewConsole::SelectAll()
{
	mutex->Lock();
	// find the first non-NULL scrollback line
	int firstRow = -1;
	for (int i = 0; i < MAX_CONSOLE_SCROLL_LINES; i++)
	{
		if (lines[i] != NULL) { firstRow = i; break; }
	}
	int cmdLen = (int)strlen(commandLine);

	if (firstRow < 0 && cmdLen == 0)
	{
		hasSelection = false;     // nothing to select
		mutex->Unlock();
		return;
	}

	if (firstRow < 0)
	{
		selAnchor.row = CONSOLE_ROW_COMMANDLINE;
		selAnchor.col = 0;
	}
	else
	{
		selAnchor.row = firstRow;
		selAnchor.col = 0;
	}
	selCaret.row = CONSOLE_ROW_COMMANDLINE;
	selCaret.col = cmdLen;
	hasSelection = true;
	mutex->Unlock();
}

bool CGuiViewConsole::DoTap(float x, float y)
{
	mutex->Lock();
	selAnchor = ScreenToConsolePos(x, y, false);  // floor: anchor to the cell under the cursor
	selCaret = selAnchor;
	isSelecting = true;
	hasSelection = false;
	mutex->Unlock();
	return true;
}

bool CGuiViewConsole::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
	if (!isSelecting)
		return false;
	mutex->Lock();
	selCaret = ScreenToConsolePos(x, y);
	hasSelection = !(selCaret.row == selAnchor.row && selCaret.col == selAnchor.col);
	mutex->Unlock();
	return true;
}

bool CGuiViewConsole::DoFinishTap(float x, float y)
{
	isSelecting = false;
	return true;
}

bool CGuiViewConsole::DoRightClick(float x, float y)
{
	mutex->Lock();
	CConsoleTextPos p = ScreenToConsolePos(x, y, false);  // floor: caret to the cell under the cursor
	if (hasSelection)
	{
		CConsoleTextPos mn, mx;
		OrderedSelection(&mn, &mx);
		if (PosLess(p, mn) || PosLess(mx, p))
		{
			selAnchor = selCaret = p;
			hasSelection = false;
		}
	}
	else
	{
		selAnchor = selCaret = p;
	}
	mutex->Unlock();
	return true;
}

CGuiViewConsole::CConsoleTextPos CGuiViewConsole::ScreenToConsolePos(float x, float y, bool snapNearestBoundary)
{
	CConsoleTextPos p;
	float py0 = posY + 3.0f;
	int rel = (int)floor((y - py0) / lineHeight);
	if (rel < 0) rel = 0;

	if (hasCommandLine && rel >= numLines)
	{
		p.row = CONSOLE_ROW_COMMANDLINE;
	}
	else
	{
		if (rel >= numLines) rel = (numLines > 0) ? numLines - 1 : 0;
		p.row = MAX_CONSOLE_SCROLL_LINES - numLines - numScrollLines + rel;
		if (p.row < 0) p.row = 0;
		if (p.row >= MAX_CONSOLE_SCROLL_LINES) p.row = MAX_CONSOLE_SCROLL_LINES - 1;
	}

	float cx = posX + 1.5f;
	if (p.row == CONSOLE_ROW_COMMANDLINE)
		cx += promptWidth;
	const char *t = GetRowText(p.row);
	if (!t) { p.col = 0; return p; }
	int len = (int)strlen(t);
	int col = 0;
	for (; col < len; col++)
	{
		float w = font->GetCharWidth(t[col], fontScale);
		// nearest-boundary: snap at half-width (forgiving drag extension)
		// floor: snap at full width (click lands in the cell under the cursor)
		float snap = snapNearestBoundary ? (w * 0.5f) : w;
		if (x < cx + snap) break;
		cx += w;
	}
	p.col = col;
	return p;
}

void CGuiViewConsole::CopySelectionToClipboard()
{
	std::string s = BuildSelectionText();
	if (s.empty())
		return;                      // never clobber the clipboard with ""
	CSlrString *str = new CSlrString((char *)s.c_str());
	SYS_SetClipboardAsSlrString(str);
	delete str;
}

void CGuiViewConsole::PasteFromClipboard()
{
	CSlrString *c = SYS_GetClipboardAsSlrString();
	if (c == NULL)
		return;
	char *u = c->GetUTF8();
	if (u)
	{
		PasteText(u);
		delete [] u;
	}
	delete c;
}

