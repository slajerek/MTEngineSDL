#ifndef _CGUIVIEWCONSOLE_H_
#define _CGUIVIEWCONSOLE_H_

#include "SYS_Defs.h"
#include "CGuiView.h"
#include <list>
#include <string>

#define MAX_CONSOLE_LINE_LENGTH 512
#define MAX_COMMAND_LINE_HISTORY	64

#define MAX_CONSOLE_SCROLL_LINES	50000

class CSlrFont;
class CSlrMutex;
class CSlrString;

class CGuiViewConsoleCallback
{
public:
	virtual void GuiViewConsoleExecuteCommand(char *commandText) {};
};

class CGuiViewConsole : public CGuiView
{
public:
	CGuiViewConsole(float posX, float posY, float posZ, float sizeX, float sizeY,
					CSlrFont *font, float fontScale, int numLines, bool hasCommandLine, CGuiViewConsoleCallback *callback);
	virtual ~CGuiViewConsole();
	virtual void SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY);

	CGuiViewConsoleCallback *callback;
	
	CSlrFont *font;
	float fontScale;
	
	CSlrMutex *mutex;
	
	bool hasCommandLine;
	
	int numLines;
	
	int maxCharsInLine;
	int numScrollLines;
	int numLinesInBuffer;

	void SetLineHeight(float height);
	float lineHeight;
	
	char prompt[256];
	float promptWidth;
	
	char *lines[MAX_CONSOLE_SCROLL_LINES];
	
	char commandLine[MAX_CONSOLE_LINE_LENGTH];
	int commandLineCursorPos;

	char backupCommandLine[MAX_CONSOLE_LINE_LENGTH];
	
	std::list<char *> commandLineHistory;
	std::list<char *>::iterator commandLineHistoryIt;
	
	void SetPrompt(char *prompt);
	
	void SetFontScale(float fontScale);
	void SetNumLines(int numLines);
	
	void ResetCommandLine();
	
	void PrintSingleLine(char *text);
	void PrintLine(const char *format, ...);
	void PrintLine(CSlrString *str);
	
	void PrintString(char *text);
	
	virtual bool KeyDown(u32 keyCode);
	virtual bool KeyTextInput(const char *text);
	virtual bool DoScrollWheel(float deltaX, float deltaY);

	void Render();

	float textColorR, textColorG, textColorB, textColorA;

	// --- text selection ---
	// Sentinel row meaning "the command line" — sorts after every scrollback row.
	static const int CONSOLE_ROW_COMMANDLINE = MAX_CONSOLE_SCROLL_LINES;

	struct CConsoleTextPos
	{
		int row;   // absolute index into lines[]; CONSOLE_ROW_COMMANDLINE == command line
		int col;   // character column within that row's text
	};

	bool hasSelection;
	bool isSelecting;            // mouse button held + dragging
	CConsoleTextPos selAnchor;
	CConsoleTextPos selCaret;

	// row/column helpers
	const char *GetRowText(int row);             // lines[row] or commandLine; may be NULL
	int GetRowLength(int row);                   // strlen of GetRowText, 0 if NULL
	// snapNearestBoundary=true  → snap to nearest boundary (for drag caret)
	// snapNearestBoundary=false → floor to the cell under the cursor (for click anchor)
	CConsoleTextPos ScreenToConsolePos(float x, float y, bool snapNearestBoundary = true);
	static bool PosLess(const CConsoleTextPos &a, const CConsoleTextPos &b);
	void OrderedSelection(CConsoleTextPos *outMin, CConsoleTextPos *outMax) const;
	float RowColToPixelX(int row, int col);      // left edge X of a column on a row
	void ClearSelection();

	// selection logic (pure)
	std::string BuildSelectionText();
	void PasteText(const char *text);
	void SelectAll();

	// clipboard wrappers (OS I/O)
	void CopySelectionToClipboard();
	void PasteFromClipboard();

	// called by PrintSingleLine after the buffer shifts up by one line
	void OnLineAppended();

	// mouse handlers
	virtual bool DoTap(float x, float y);
	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool DoFinishTap(float x, float y);
	virtual bool DoRightClick(float x, float y);
};

#endif //_CGUIVIEWCONSOLE_H_

