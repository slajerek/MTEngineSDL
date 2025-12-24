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

enum ImGuiToastType_
{
	ImGuiToastType_None,
	ImGuiToastType_Success,
	ImGuiToastType_Warning,
	ImGuiToastType_Error,
	ImGuiToastType_Info,
	ImGuiToastType_COUNT
};

enum SetFullScreenMode
{
	ViewEnterFullScreen,
	ViewLeaveFullScreen,
	MainWindowEnterFullScreen,
	MainWindowLeaveFullScreen
};

enum LayoutStorageTask
{
	StoreLayout,
	RestoreLayout
};

class CUiThreadTaskCallback
{
public:
	CUiThreadTaskCallback();
	virtual ~CUiThreadTaskCallback();
	
	void *uiThreadTaskCallbackUserData;
	
	// warning: the render mutex is unlocked!
	virtual void RunUIThreadTask();
};

class CUiMessageBoxCallback
{
public:
	CUiMessageBoxCallback();
	virtual ~CUiMessageBoxCallback();

	void *uiMessageBoxCallbackUserData;
	
	virtual void MessageBoxCallback();
};

class CUiMessageBoxCallbackRestartApplication : public CUiMessageBoxCallback
{
public:
	virtual void MessageBoxCallback();
};

class CGuiMain
{
public:
	CGuiMain();

	CLayoutManager *layoutManager;
	
	CGuiTheme *theme;
		
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
	void AddViewSkippingLayout(CGuiView *view);
	void RemoveView(CGuiView *view);
	void RemoveViewSkippingLayout(CGuiView *view);
	void RemoveAllViews();

	// layout views are views that should serialize layouts but may not be available (not in views), identified by u64 hash from name
	std::map<u64, CGuiView *> layoutViews;
	void AddViewToLayout(CGuiView *view);
	void RemoveViewFromLayout(CGuiView *view);

	void DebugPrintViews();
	
	//
	CLayoutData *layoutForThisFrame;
	LayoutStorageTask layoutStoreOrRestore;
	bool layoutStoreCurrentInSettings;
	void SerializeLayout(CLayoutData *layout);
	bool DeserializeLayout(CLayoutData *layout);
	bool layoutJustRestored;
	void StoreLayoutInSettingsAtEndOfThisFrame();
	
	void SetView(CGuiView *view);
	
	CGuiView *focusedView;
	void SetFocus(CGuiView *view);
	void SetInternalViewFocus(CGuiView *view);
	bool ClearInternalViewFocus();

	CGuiView *focusedViewThisFrameOnly;

	void RaiseMainWindow();
	void SetWindowOnTop(CGuiView *view);

	void CloseCurrentImGuiWindow();
	
	CGuiView *viewResourceManager;

	// modal dialog
	void ShowMessageBox(const char *title, const char *message);
	void ShowMessageBox(const char *title, const char *message, CUiMessageBoxCallback *messageBoxCallback);
	volatile bool beginMessageBoxPopup;
	char *messageBoxTitle;
	char *messageBoxText;
	CUiMessageBoxCallback *messageBoxCallback;
	
	// notification toast
	void ShowNotification(const char *title, const char *message);
	void ShowNotificationError(const char *title, const char *message);
	void ShowNotification(ImGuiToastType_ toastType, int dismissTime, const char *title, const char *message);
	
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
	
	volatile bool isLeftMouseButtonPressed;
	volatile bool isRightMouseButtonPressed;

	bool CheckKeyboardShortcut(u32 keyCode);
	bool KeyDown(u32 keyCode);
	bool KeyDownRepeat(u32 keyCode);
	bool KeyUp(u32 keyCode);
	bool KeyTextInput(const char *text);
	
	float moveStartTapPosX, moveStartTapPosY;
	float movePrevTapPosX, movePrevTapPosY;
	float moveStartRightClickPosX, moveStartRightClickPosY;
	float movePrevRightClickPosX, movePrevRightClickPosY;

	bool DoTap(float x, float y);
	bool DoFinishTap(float x, float y);
	bool DoRightClick(float x, float y);
	bool DoFinishRightClick(float x, float y);
	
	bool DoMove(float x, float y);
	bool DoRightClickMove(float x, float y);
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
	
	//
	CGuiView *FindTopWindow(float x, float y);
	bool IsViewHidden(CGuiView *view);
	
	//
	void MergeIconsWithLatestFont(float fontSize);
	void CreateUiFontsTexture(float fontSize);
	
	// when going full screen a layout is saved and restored when going back to windowed mode.
	// because currentLayout may have doNotUpdateViewsPositions we make a temporary copy
	CLayoutData *temporaryLayoutToGoBackFromFullScreen;
	
	// this is backup of currentLayout (may have the doNotUpdateViewsPositions set to true)
	CLayoutData *currentLayoutBeforeFullScreen;

	// view that is now full screen (NULL=windowed mode)
	CGuiView *viewFullScreen;
	
	bool appWasFullScreenBeforeViewFullScreen;
	
	bool isChangingFullScreenState;

	// go full screen
	void SetViewFullScreen(SetFullScreenMode setFullScreenMode, CGuiView *view, float fullScreenSizeX, float fullScreenSizeY);
	void SetViewFullScreen(SetFullScreenMode setFullScreenMode, CGuiView *view);
	bool IsViewFullScreen();
	
	//
	void SetApplicationWindowAlwaysOnTop(bool alwaysOnTop);
	
	bool IsApplicationWindowFullScreen();
	void SetApplicationWindowFullScreen(bool isFullScreen);
	
	ImVec4 ImGuiCol_WindowBg_Backup;
	ImVec4 ImGuiCol_DockingEmptyBg_Backup;
	void SetImGuiStyleWindowFullScreenBackground();
	void SetImGuiStyleWindowBackupBackground();

	bool IsMouseCursorVisible();
	void SetMouseCursorVisible(bool isVisible);
		
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
	void UpdateLayouts();
	void PostRenderEndFrame();

	//
	void LockMutex();
	bool TryLockMutex();
	void UnlockMutex();
	
private:
	CSlrMutex *renderMutex;
	CSlrMutex *notificationMutex;
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

class CUiThreadTaskSetViewFullScreen : public CUiThreadTaskCallback
{
public:
	CUiThreadTaskSetViewFullScreen(SetFullScreenMode setFullScreenMode, CGuiView *view, float fullScreenSizeX, float fullScreenSizeY);
	
	SetFullScreenMode setFullScreenMode;
	CGuiView *view;
	float fullScreenSizeX, fullScreenSizeY;
	virtual void RunUIThreadTask();
};

class CUiThreadTaskSetAlwaysOnTop : public CUiThreadTaskCallback
{
public:
	CUiThreadTaskSetAlwaysOnTop(CGuiView *view, bool isAlwaysOnTop);
	
	CGuiView *view;
	bool isAlwaysOnTop;
	virtual void RunUIThreadTask();
};

class CUiThreadTaskSetViewFocus : public CUiThreadTaskCallback
{
public:
	CUiThreadTaskSetViewFocus(CGuiView *view);
	
	CGuiView *view;
	virtual void RunUIThreadTask();
};

class CUiThreadTaskSetViewVisible : public CUiThreadTaskCallback
{
public:
	CUiThreadTaskSetViewVisible(CGuiView *view, bool isVisible);
	
	CGuiView *view;
	volatile bool isVisible;
	virtual void RunUIThreadTask();
};

class CUiThreadTaskRecreateUiFonts : public CUiThreadTaskCallback
{
public:
	CUiThreadTaskRecreateUiFonts();
	virtual void RunUIThreadTask();
};

class CUiThreadTaskRaiseMainWindow : public CUiThreadTaskCallback
{
public:
	virtual void RunUIThreadTask();
};

class CUiThreadTaskSetLayout : public CUiThreadTaskCallback
{
public:
	CUiThreadTaskSetLayout(CLayoutData *layoutData, bool saveCurrentLayout);
	
	CLayoutData *layoutData;
	bool saveCurrentLayout;
	virtual void RunUIThreadTask();
};

extern CGuiMain *guiMain;

#endif
