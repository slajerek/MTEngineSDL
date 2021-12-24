#ifndef _CGUIMAIN_H_
#define _CGUIMAIN_H_

#include "GUI_Main.h"
#include "CSlrFontBitmap.h"
#include <list>
#include <map>

class CGlobalKeyboardCallback;
class CGlobalLogicCallback;
class CGlobalOSWindowChangedCallback;
class CGlobalDropFileCallback;
class CSlrKeyboardShortcut;
class CSlrKeyboardShortcuts;

class CLayoutManager;
class CLayoutData;

class CSlrMutex;
class CGuiTheme;
class CGuiElement;
class CGuiView;
class CGamePad;

class CUiThreadTaskCallback
{
public:
	CUiThreadTaskCallback();
	void *uiThreadTaskUserData;
	virtual void RunUIThreadTask();
};

class CGuiMain
{
public:
	CGuiMain();

	CLayoutManager *layoutManager;
	
	CGuiTheme *theme;
	
	CGuiElement *focusElement;
	
	CSlrFontBitmap *fntConsole;
	CSlrImage *imgConsoleFonts;
	CSlrFontBitmap *fntConsoleInverted;
	CSlrImage *imgConsoleInvertedFonts;
	
	CSlrImage *imgFontDefault;
	CSlrFont *fntEngineDefault;
	
	CSlrImage *imgFontShowMessage;
	CSlrFont *fntShowMessage;
	float showMessageScale;

	CGuiView *currentView;
	
	// currently available views
	std::list<CGuiView *> views;
	void AddView(CGuiView *view);
	void RemoveView(CGuiView *view);
	void RemoveAllViews();

	// layout views are views that should serialize layouts but may not be available (not in views), identified by u64 hash from name
	std::map<u64, CGuiView *> layoutViews;
	void AddLayoutView(CGuiView *view);
	void RemoveLayoutView(CGuiView *view);

	//
	CLayoutData *layoutForThisFrame;
	bool layoutStoreOrRestore;
	bool layoutStoreCurrentInSettings;
	void SerializeLayout(CByteBuffer *byteBuffer);
	bool DeserializeLayout(CByteBuffer *byteBuffer);
	bool layoutJustRestored;
	void StoreLayoutInSettingsAtEndOfThisFrame();
	
	void SetView(CGuiView *view);
	void SetViewFocus(CGuiElement *view);
	void ClearViewFocus();
	void SetWindowOnTop(CGuiElement *view);

	CGuiView *viewResourceManager;

	// async show message
	void ShowMessage(CSlrString *showMessage);
	void ShowMessage(const char *message);

	// modal dialog
	void ShowMessageBox(const char *title, const char *message);
	bool beginMessageBoxPopup;
	char *messageBoxTitle;
	char *messageBoxText;
	
	volatile bool isShiftPressed;
	volatile bool isControlPressed;
	volatile bool isSuperPressed;
	volatile bool isAltPressed;
	
	//	volatile bool wasShiftPressed;
	//	volatile bool wasControlPressed;
	//	volatile bool wasAltPressed;
	
	volatile bool isLeftShiftPressed;
	volatile bool isLeftControlPressed;
	volatile bool isLeftSuperPressed;
	volatile bool isLeftAltPressed;
	
	volatile bool isRightShiftPressed;
	volatile bool isRightControlPressed;
	volatile bool isRightSuperPressed;
	volatile bool isRightAltPressed;

	bool KeyDown(u32 keyCode);
	bool KeyDownRepeat(u32 keyCode);
	bool KeyUp(u32 keyCode);
	bool DoTap(float x, float y);
	bool DoFinishTap(float x, float y);
	bool DoRightClick(float x, float y);
	bool DoFinishRightClick(float x, float y);
	
	bool DoMove(float x, float y);
	void DoNotTouchedMove(float x, float y);
	void DoScrollWheel(float deltaX, float deltaY);

//	float prevZoomValue;
//	bool DoZoomBy(float x, float y, float zoomValue);

	bool DoGamePadButtonDown(CGamePad *gamePad, u8 button);
	bool DoGamePadButtonUp(CGamePad *gamePad, u8 button);
	bool DoGamePadAxisMotion(CGamePad *gamePad, u8 axis, int value);

	// emulated button presses (analog to dpad)
	bool DoGamePadAxisMotionButtonDown(CGamePad *gamePad, u8 button);
	bool DoGamePadAxisMotionButtonUp(CGamePad *gamePad, u8 button);

	void DoDropFile(u32 windowId, char *filePath);

	std::list<CGlobalKeyboardCallback *> globalKeyboardCallbacks;
	void AddGlobalKeyboardCallback(CGlobalKeyboardCallback *callback);
	void RemoveGlobalKeyboardCallback(CGlobalKeyboardCallback *callback);
	void ClearGlobalKeyboardCallbacks();
	
	std::list<CGlobalLogicCallback *> globalLogicCallbacks;
	void AddGlobalLogicCallback(CGlobalLogicCallback *callback);
	void RemoveGlobalLogicCallback(CGlobalLogicCallback *callback);
	void ClearGlobalLogicCallbacks();
	
	std::list<CGlobalOSWindowChangedCallback *> globalOSWindowChangedCallbacks;
	void AddGlobalOSWindowChangedCallback(CGlobalOSWindowChangedCallback *callback);
	void RemoveGlobalOSWindowChangedCallback(CGlobalOSWindowChangedCallback *callback);
	void ClearGlobalOSWindowChangedCallbacks();
	void NotifyGlobalOSWindowChangedCallbacks();

	std::list<CGlobalDropFileCallback *> globalDropFileCallbacks;
	void AddGlobalDropFileCallback(CGlobalDropFileCallback *callback);
	void RemoveGlobalDropFileCallback(CGlobalDropFileCallback *callback);
	void ClearGlobalDropFileCallbacks();
	void NotifyGlobalDropFileCallbacks(char *filePath, bool consumedByView);

	std::list<CUiThreadTaskCallback *> uiThreadTasks;
	CSlrMutex *uiThreadTasksMutex;
	void AddUiThreadTask(CUiThreadTaskCallback *taskCallback);
	
	void SetApplicationWindowAlwaysOnTop(bool alwaysOnTop);
	void SetApplicationWindowFullScreen(bool isFullScreen);
	void SetMouseCursorVisible(bool isVisible);
	void SetFocus(CGuiElement *element);
	
	//
	CSlrKeyboardShortcuts *keyboardShortcuts;
	void AddKeyboardShortcut(CSlrKeyboardShortcut *keyboardShortcut);
	void RemoveKeyboardShortcut(CSlrKeyboardShortcut *keyboardShortcut);

	//
	float mousePosX;
	float mousePosY;
	bool isMouseCursorVisible;
	
	//
	float mouseScrollWheelScaleX;
	float mouseScrollWheelScaleY;
	
	bool IsOnAnyOpenedPopup(float px, float py);
	
	//
	void RenderImGui();

	//
	void LockMutex();
	bool TryLockMutex();
	void UnlockMutex();
	
private:
	CSlrMutex *renderMutex;
};

// TODO: MOVE ME TO CPP
class CUiThreadTaskSetView : public CUiThreadTaskCallback
{
public:
	CUiThreadTaskSetView(CGuiView *view) { this->view = view; };
	CGuiView *view;
	virtual void RunUIThreadTask();
};

class CUiThreadTaskShowMessage : public CUiThreadTaskCallback
{
public:
	CUiThreadTaskShowMessage(char *showMessage, float showMessageColorR,
						 float showMessageColorG, float showMessageColorB);
	char *showMessage;
	float showMessageColorR;
	float showMessageColorG;
	float showMessageColorB;
	
	virtual void RunUIThreadTask();
};

class CUiThreadTaskSetMouseCursorVisible : public CUiThreadTaskCallback
{
public:
	CUiThreadTaskSetMouseCursorVisible(bool mouseCursorVisible);
	
	bool mouseCursorVisible;
	virtual void RunUIThreadTask();
};

class CUiThreadTaskSetFullScreen : public CUiThreadTaskCallback
{
public:
	CUiThreadTaskSetFullScreen(bool isFullScreen);
	
	bool isFullScreen;
	virtual void RunUIThreadTask();
};

class CUiThreadTaskSetAlwaysOnTop : public CUiThreadTaskCallback
{
public:
	CUiThreadTaskSetAlwaysOnTop(bool isAlwaysOnTop);
	
	bool isAlwaysOnTop;
	virtual void RunUIThreadTask();
};


extern CGuiMain *guiMain;

#endif
