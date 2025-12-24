#include "CGuiMain.h"
#include "CGuiView.h"
#include "CConfigStorage.h"
#include "SYS_Threading.h"
#include "CGlobalKeyboardCallback.h"
#include "CGlobalLogicCallback.h"
#include "CGlobalOSWindowChangedCallback.h"
#include "CGlobalDropFileCallback.h"
#include "RES_ResourceManager.h"
#include "CSlrString.h"
#include "CLayoutManager.h"
#include "GAM_GamePads.h"
#include "CSlrKeyboardShortcuts.h"
#include "imgui_notify.h"

#define CONSOLE_FONT_SIZE_X		0.03125
#define CONSOLE_FONT_SIZE_Y		0.03125
#define CONSOLE_FONT_PITCH_X	0.035156251
#define CONSOLE_FONT_PITCH_Y	0.035156251

CGuiMain::CGuiMain()
{
	currentView = NULL;
	
	renderMutex = new CSlrMutex("CGuiMain::renderMutex");
	uiThreadTasksMutex = new CSlrMutex("CGuiMain::uiThreadTasksMutex");
	notificationMutex = new CSlrMutex("CGuiMain::notificationMutex");
	
	layoutManager = new CLayoutManager(this);

#if defined(LOAD_CONSOLE_FONT)
	imgConsoleFonts = RES_GetImage("/Engine/console-plain");
	imgConsoleFonts->ResourceSetPriority(RESOURCE_PRIORITY_STATIC);
	fntConsole = new CSlrFontBitmap("console", imgConsoleFonts,
									CONSOLE_FONT_SIZE_X, CONSOLE_FONT_SIZE_Y,
									CONSOLE_FONT_PITCH_X, CONSOLE_FONT_PITCH_Y);
	fntConsole->ResourceSetPriority(RESOURCE_PRIORITY_STATIC);
#endif
	
#if defined(LOAD_CONSOLE_INVERTED_FONT)
	imgConsoleInvertedFonts = RES_GetImage("/Engine/console-inverted-plain");
	imgConsoleInvertedFonts->ResourceSetPriority(RESOURCE_PRIORITY_STATIC);
	fntConsoleInverted = new CSlrFontBitmap("console-inverted",
											imgConsoleInvertedFonts,
											CONSOLE_FONT_SIZE_X, CONSOLE_FONT_SIZE_Y,
											CONSOLE_FONT_PITCH_X, CONSOLE_FONT_PITCH_Y);
	fntConsoleInverted->ResourceSetPriority(RESOURCE_PRIORITY_STATIC);
	//gScaleDownImages = tmp;
#endif
		
#if defined(LOAD_DEFAULT_UI_THEME)
	this->theme = new CGuiTheme("default");
	
#elif defined(INIT_DEFAULT_UI_THEME)
	this->theme = new CGuiTheme();
#else
	this->theme = NULL;
	//	this->imgBlack = NULL;
#endif
	
	this->focusedView = NULL;
	this->currentView = NULL;
	
#ifdef LOAD_DEFAULT_FONT
	fntEngineDefault = RES_GetFont("/Engine/default-font");
	fntEngineDefault->scaleAdjust = 0.25f;
	
	//	// default font:
	//	imgFontDefault = RES_GetImage("/Engine/default-font", true, true);
	//
	//	RES_DebugPrintResources();
	//
	//	CByteBuffer *fontData;
	//	fontData = new CByteBuffer(true, "/Engine/default-font", DEPLOY_FILE_TYPE_FONT);
	//	fntDefault = new CSlrFontProportional(fontData, imgFontDefault);
	//	fntDefault->scaleAdjust = 0.25f;
	//	delete fontData;
	
	fntShowMessage = fntEngineDefault;
#else
	imgFontDefault = NULL;
	fntShowMessage = NULL;
#endif

	isShiftPressed = false;
	isControlPressed = false;
	isSuperPressed = false;
	isAltPressed = false;
	
	isLeftShiftPressed = false;
	isLeftControlPressed = false;
	isLeftSuperPressed = false;
	isLeftAltPressed = false;
	
	isRightShiftPressed = false;
	isRightControlPressed = false;
	isRightSuperPressed = false;
	isRightAltPressed = false;

	isLeftMouseButtonPressed = false;
	isRightMouseButtonPressed = false;
	
	isMouseCursorVisible = true;
	
	moveStartTapPosX = moveStartTapPosY = movePrevTapPosX = movePrevTapPosY = moveStartRightClickPosX = moveStartRightClickPosY = movePrevRightClickPosX = movePrevRightClickPosY = -1;;

	viewResourceManager = NULL;
	layoutForThisFrame = NULL;
	layoutStoreOrRestore = LayoutStorageTask::StoreLayout;
	layoutJustRestored = false;
	layoutStoreCurrentInSettings = false;

	messageBoxTitle = NULL;
	messageBoxText = NULL;
	beginMessageBoxPopup = false;
	messageBoxCallback = NULL;
	
	currentLayoutBeforeFullScreen = NULL;
	temporaryLayoutToGoBackFromFullScreen = new CLayoutData();
	viewFullScreen = NULL;
	appWasFullScreenBeforeViewFullScreen = false;
	isChangingFullScreenState = false;
	
	keyboardShortcuts = new CSlrKeyboardShortcuts();

	mousePosX = 0;
	mousePosY = 0;
	
	mouseScrollWheelScaleX = 1.0f;

#if defined(WIN32)
	mouseScrollWheelScaleY = 5.0f;
#else
	mouseScrollWheelScaleY = 1.0f;
#endif
	
	layoutManager->LoadLayouts();
}

void CGuiMain::AddViewSkippingLayout(CGuiView *view)
{
//	LOGG("CGuiMain::AddViewNoLayout view=%x '%s'", view, view->name?view->name:"<NULL>");
	LockMutex();
	for (std::list<CGuiView *>::iterator it = views.begin(); it != views.end(); it++)
	{
		if (*it == view)
		{
			// view already added
			UnlockMutex();
			return;
		}
	}
	this->views.push_back(view);
	
	this->DebugPrintViews();
	UnlockMutex();
}

void CGuiMain::RemoveViewSkippingLayout(CGuiView *view)
{
//	LOGG("CGuiMain::RemoveViewNoLayout view=%x '%s'", view, view->name?view->name:"<NULL>");
	LockMutex();
	this->views.remove(view);
	this->DebugPrintViews();
	UnlockMutex();
}

void CGuiMain::AddViewToLayout(CGuiView *view)
{
//	LOGG("CGuiMain::AddViewToLayout view=%x '%s'", view, view->name?view->name:"<NULL>");
	u64 hash = GetHashCode64(view->name);

	LockMutex();
	layoutViews[hash] = view;
	UnlockMutex();
}

void CGuiMain::RemoveViewFromLayout(CGuiView *view)
{
//	LOGG("CGuiMain::RemoveViewFromLayout view=%x '%s'", view, view->name?view->name:"<NULL>");
	u64 hash = GetHashCode64(view->name);

	LockMutex();
	layoutViews.erase(hash);
	UnlockMutex();
}

void CGuiMain::AddView(CGuiView *view)
{
	AddViewToLayout(view);
	AddViewSkippingLayout(view);
}

void CGuiMain::RemoveView(CGuiView *view)
{
	RemoveViewFromLayout(view);
	RemoveViewSkippingLayout(view);
}

//
void CGuiMain::DebugPrintViews()
{
//	LOGD("CGuiMain::DebugPrintViews");
//	for (std::list<CGuiView *>::iterator it = views.begin(); it != views.end(); it++)
//	{
//		CGuiView *view = *it;
//		LOGD("         : %x '%s'", view, view->name ? view->name : "<NULL>");
//	}
//	LOGD("--------------------------------------------");
}

#define LAYOUT_VERSION 2

//
void CGuiMain::SerializeLayout(CLayoutData *layout)
{
//	LOGD("CGuiMain::SerializeLayout: layout=%x", layout);
	CByteBuffer *byteBuffer = layout->serializedLayoutBuffer;
	
	byteBuffer->Clear();
	
	// put version
	byteBuffer->PutU32(LAYOUT_VERSION);
	
	// put imgui ini
	size_t len = 0;
	const char *data = ImGui::SaveIniSettingsToMemory(&len);
	byteBuffer->PutU32((unsigned int)len+1);
	byteBuffer->PutBytes((u8*)data, (unsigned int)len+1);
	
	byteBuffer->PutU32((unsigned int)layoutViews.size());
	
	CByteBuffer *viewByteBuffer = new CByteBuffer();
	for (std::map<u64, CGuiView *>::iterator it = layoutViews.begin(); it != layoutViews.end(); it++)
	{
		CGuiView *view = it->second;
//		LOGG("serialize %s", view->name);
		byteBuffer->PutString(view->name);
		
		viewByteBuffer->Clear();
		view->SerializeLayout(viewByteBuffer);
		byteBuffer->PutByteBuffer(viewByteBuffer);
	}
	delete viewByteBuffer;
}

// returns if failed
bool CGuiMain::DeserializeLayout(CLayoutData *layout)
{
//	LOGD("CGuiMain::DeserializeLayout: layout=%x", layout);
	CByteBuffer *byteBuffer = layout->serializedLayoutBuffer;
	
	if (!byteBuffer ||byteBuffer->length < 1)
		return false;
	
	byteBuffer->Rewind();
	
	u32 version;
	u32 len;
	
	version = byteBuffer->GetU32();
//	LOGD("read %d", version);
	len = byteBuffer->GetU32();
	
	LOGD("CGuiMain::DeserializeLayout: version=%d", version);
	
	if (version > LAYOUT_VERSION)
		return false;
	
	u8 *data = byteBuffer->GetBytes(len);

	// debug dump layout
//	FILE *fp = fopen("/Users/mars/Desktop/layout.txt", "wb");
//	fwrite(data, len, 1, fp);
//	fclose(fp);
	
	ImGui::LoadIniSettingsFromMemory((const char*)data);

	delete [] data;
	
	u32 numViews = byteBuffer->GetU32();
	for (u32 i = 0; i < numViews; i++)
	{
		char *str = byteBuffer->GetString();
		CByteBuffer *viewByteBuffer;
		
		if (version > 0)
		{
			viewByteBuffer = byteBuffer->GetByteBuffer();;
		}
		else
		{
			// old version
			viewByteBuffer = byteBuffer;
		}
		
		u64 hash = GetHashCode64(str);
//		LOGD("  view %s hash %x", str, hash);
		std::map<u64, CGuiView *>::iterator it = layoutViews.find(hash);
		if (it == layoutViews.end())
		{
			// view not found? error, break restore
//			LOGError("   view not found %s hash %08x, skipping", str, hash);
			STRFREE(str);
			
			if (version > 0)
				delete viewByteBuffer;
			continue;
		}
		
		STRFREE(str);

		CGuiView *view = it->second;
		bool isCorrect = view->DeserializeLayout(viewByteBuffer, version);
		
		if (version > 0)
			delete viewByteBuffer;
		
		if (isCorrect == false)
		{
			return false;
		}
	}
	
	return true;
}


void CGuiMain::SetView(CGuiView *view)
{
	LOGG("CGuiMain::SetView: %s", view->name);
	if (currentView != NULL)
	{
		currentView->DeactivateView();
	}
	this->currentView = view;
	this->currentView->ActivateView();
}

void CGuiMain::SetFocus(CGuiView *view)
{
	view->SetVisible(true);

	if (!view->imGuiWindow)
	{
		LOGWarning("CGuiMain::SetFocus: %s has no imGuiWindow", view->name ? view->name : "NULL");
		return;
	}

	guiMain->LockMutex();
	ImGui::FocusWindow(view->imGuiWindow);
	SetInternalViewFocus(view);
	guiMain->UnlockMutex();
}

void CGuiMain::SetInternalViewFocus(CGuiView *viewToFocus)
{
	LOGG("CGuiMain::SetInternalViewFocus: %s", viewToFocus ? viewToFocus->name : "NULL");
	if (this->focusedView != viewToFocus)
	{
//		skip check, ImGui controls is view is focusable. if (viewToFocus->IsFocusableElement())
		{
//			LOGG("... ClearViewFocus: %s", viewToFocus->name);
			if (this->ClearInternalViewFocus())
			{
//				LOGG("... SetFocus: %s", viewToFocus->name);
				if (viewToFocus->WillReceiveFocus())
				{
					LOGG("... focusElement: %s", viewToFocus->name);
					this->focusedView = viewToFocus;
				}
			}
		}
	}
	
	focusedViewThisFrameOnly = viewToFocus;
}

// clear focus, returns true when current focused view allows that
bool CGuiMain::ClearInternalViewFocus()
{
	LOGG("CGuiMain::ClearInternalViewFocus");
	if (focusedView != NULL)
	{
		if (focusedView->WillClearFocus())
		{
			focusedView = NULL;
			return true;
		}
		return false;
	}
	return true;
}

void CGuiMain::AddKeyboardShortcut(CSlrKeyboardShortcut *keyboardShortcut)
{
	LOGG("CGuiMain::AddKeyboardShortcut: name=%s %s zone=%d", keyboardShortcut->name, keyboardShortcut->cstr, keyboardShortcut->zone);
	this->keyboardShortcuts->AddShortcut(keyboardShortcut);
}

void CGuiMain::RemoveKeyboardShortcut(CSlrKeyboardShortcut *keyboardShortcut)
{
	LOGD("CGuiMain::RemoveKeyboardShortcut: name=%s %s", keyboardShortcut->name, keyboardShortcut->cstr);
	this->keyboardShortcuts->RemoveShortcut(keyboardShortcut);
}

bool CGuiMain::CheckKeyboardShortcut(u32 keyCode)
{
	u32 keyCodeBare = SYS_GetBareKey(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
	
	std::list<u32> zones;
	zones.push_back(MT_KEYBOARD_SHORTCUT_GLOBAL);
	CSlrKeyboardShortcut *shortcut = this->keyboardShortcuts->FindShortcut(zones, keyCodeBare,
																		   isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
	
	if (shortcut != NULL)
	{
		shortcut->DebugPrint();
	
		if (shortcut->callback)
		{
			if (shortcut->callback->ProcessKeyboardShortcut(MT_KEYBOARD_SHORTCUT_GLOBAL, MT_ACTION_TYPE_KEYBOARD_SHORTCUT, shortcut))
				return true;
		}
	}
	return false;
}

bool CGuiMain::KeyDown(u32 keyCode)
{
	ImGuiIO& io = ImGui::GetIO();
	
	LOGI("CGuiMain::KeyDown: keyCode=%d (0x%2.2x = %c) isShift=%s isAlt=%s isControl=%s isSuper=%s | io.WantTextInput=%s", keyCode, keyCode, keyCode,
		 STRBOOL(isShiftPressed), STRBOOL(isAltPressed), STRBOOL(isControlPressed), STRBOOL(isSuperPressed), STRBOOL(io.WantTextInput));

	// pre-event first, sent always
	for (std::list<CGlobalKeyboardCallback *>::const_iterator itKeybardCallbacks =
			this->globalKeyboardCallbacks.begin();
			itKeybardCallbacks != this->globalKeyboardCallbacks.end();
			itKeybardCallbacks++)
	{
		CGlobalKeyboardCallback *callback = (CGlobalKeyboardCallback *) *itKeybardCallbacks;
		callback->GlobalPreKeyDownCallback(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
	}

	// check not active views first. we iterate top-down and if mouse is on a window we pass the key in a special event
	// this is to allow key presses even if window is not focused, for example space-bar moving content like in image viewer
	
	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (!window->WasActive || window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL)
		{
			if (!view->visible)
				continue;
			
			if (io.WantTextInput && view->imGuiSkipKeyPressWhenIoWantsTextInput == true)
				continue;
			
			if (view->IsInside(mousePosX, mousePosY))
			{
				if (view->KeyDownOnMouseHover(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
				{
					return true;
				}
				
				break;
			}
		}
	}
	
	// check view in focus
	if (this->focusedView)
	{
		if ((io.WantTextInput && focusedView->imGuiSkipKeyPressWhenIoWantsTextInput == false)
			|| io.WantTextInput == false)
		{
			// consumed?
			LOGI("CGuiMain::KeyDown: keyDown focusElement=%s", this->focusedView->name);
			if (this->focusedView->KeyDown(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
			{
				return true;
			}
		}
	}

	// then change current view
	if (this->currentView != NULL)
	{
		if ((io.WantTextInput && currentView->imGuiSkipKeyPressWhenIoWantsTextInput == false)
			|| io.WantTextInput == false)
		{
			LOGI("                > currentView=%s", currentView->name);
			if (currentView->KeyDown(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
			{
				return true;
			}
		}
	}
	
	// then global key
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL)
		{
			view->KeyDownGlobal(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
		}
	}
	
	LOGI("CGuiMain::KeyDown: not consumed by focusElement");
	
	for (std::list<CGlobalKeyboardCallback *>::const_iterator itKeybardCallbacks =
			this->globalKeyboardCallbacks.begin();
			itKeybardCallbacks != this->globalKeyboardCallbacks.end();
			itKeybardCallbacks++)
	{
		CGlobalKeyboardCallback *callback = (CGlobalKeyboardCallback *) *itKeybardCallbacks;
		if (callback->GlobalKeyDownCallback(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed) == true)
		{
			return true;
		}
	}
	
	// keyboard shortcuts
	if (CheckKeyboardShortcut(keyCode))
	{
		return true;
	}
	
	/*
	 // keyboard shortcuts
	u32 keyCodeBare = SYS_GetBareKey(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
	
	std::list<u32> zones;
	zones.push_back(MT_KEYBOARD_SHORTCUT_GLOBAL);
	CSlrKeyboardShortcut *shortcut = this->keyboardShortcuts->FindShortcut(zones, keyCodeBare,
																		   isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
	
	if (shortcut != NULL)
	{
		shortcut->DebugPrint();
	
		if (shortcut->callback)
		{
			if (shortcut->callback->ProcessKeyboardShortcut(MT_KEYBOARD_SHORTCUT_GLOBAL, MT_ACTION_TYPE_KEYBOARD_SHORTCUT, shortcut))
				return true;
		}
	}
	 */
	
	// then change current view
	if (this->currentView != NULL)
	{
		if ((io.WantTextInput && currentView->imGuiSkipKeyPressWhenIoWantsTextInput == false)
			|| io.WantTextInput == false)
		{
			LOGI("                > PostKeyDown currentView=%s", currentView->name);
			if (currentView->PostKeyDown(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
			{
				return true;
			}
		}
	}

	return false;
}

bool CGuiMain::KeyDownRepeat(u32 keyCode)
{
	ImGuiIO& io = ImGui::GetIO();

	LOGI("CGuiMain::KeyDownRepeat: keyCode=%d (0x%2.2x = %c) isShift=%s isAlt=%s isControl=%s isSuper=%s | io.WantTextInput=%s", keyCode, keyCode, keyCode,
		 STRBOOL(isShiftPressed), STRBOOL(isAltPressed), STRBOOL(isControlPressed), STRBOOL(isSuperPressed), STRBOOL(io.WantTextInput));

	if (this->focusedView)
	{
		if ((io.WantTextInput && focusedView->imGuiSkipKeyPressWhenIoWantsTextInput == false)
			|| io.WantTextInput == false)
		{
			// consumed?
			LOGI("KeyDownRepeat focusElement=%s", this->focusedView->name);
			if (focusedView->KeyDownRepeat(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
			{
				return true;
			}
		}
	}

	if (this->currentView != NULL)
	{
		if ((io.WantTextInput && currentView->imGuiSkipKeyPressWhenIoWantsTextInput == false)
			|| io.WantTextInput == false)
		{
			LOGI("                > currentView=%s", currentView->name);
			if (currentView->KeyDownRepeat(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
			{
				return true;
			}
		}
	}
	

//	for (std::list<CGlobalKeyboardCallback *>::const_iterator itKeybardCallbacks =
//			this->globalKeyboardCallbacks.begin();
//			itKeybardCallbacks != this->globalKeyboardCallbacks.end();
//			itKeybardCallbacks++)
//	{
//		CGlobalKeyboardCallback *callback =
//		(CGlobalKeyboardCallback *) *itKeybardCallbacks;
//		if (callback->GlobalKeyDownCallback(keyCode, isShiftPressed, isAltPressed, isControlPressed) == true)
//		{
//			return true;
//		}
//	}
	
	if (io.WantTextInput)
		return false;
	
	// keyboard shortcuts
	if (CheckKeyboardShortcut(keyCode))
	{
		return true;
	}

	return false;
}

bool CGuiMain::KeyUp(u32 keyCode)
{
	ImGuiIO& io = ImGui::GetIO();

	LOGI("CGuiMain::KeyUp: keyCode=%d (0x%2.2x = %c) isShift=%s isAlt=%s isControl=%s | io.WantTextInput=%s", keyCode, keyCode, keyCode,
		 STRBOOL(isShiftPressed), STRBOOL(isAltPressed), STRBOOL(isControlPressed), STRBOOL(io.WantTextInput));

	if (io.WantTextInput)
		return false;

	// pre-event first, sent always
	for (std::list<CGlobalKeyboardCallback *>::const_iterator itKeybardCallbacks =
			this->globalKeyboardCallbacks.begin();
			itKeybardCallbacks != this->globalKeyboardCallbacks.end();
			itKeybardCallbacks++)
	{
		CGlobalKeyboardCallback *callback = (CGlobalKeyboardCallback *) *itKeybardCallbacks;
		callback->GlobalPreKeyUpCallback(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
	}

	// check not active views first. we iterate top-down and if mouse is on a window we pass the key in a special event
	// this is to allow key presses even if window is not fouces, for example space-bar moving of content like in image viewer
	
	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (!window->WasActive || window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL)
		{
			if (!view->visible)
				continue;
			
			if (io.WantTextInput && view->imGuiSkipKeyPressWhenIoWantsTextInput == true)
				continue;
			
			if (view->IsInside(mousePosX, mousePosY))
			{
				if (view->KeyUpOnMouseHover(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
				{
					return true;
				}
				
				break;
			}
		}
	}

	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL)
		{
			if (io.WantTextInput && view->imGuiSkipKeyPressWhenIoWantsTextInput == true)
				continue;
			
			view->KeyUpGlobal(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
		}
	}
	
	if (this->focusedView)
	{
		if ((io.WantTextInput && focusedView->imGuiSkipKeyPressWhenIoWantsTextInput == false)
			|| io.WantTextInput == false)
		{
			// consumed?
	//		LOGI("keyUp focusElement=%s", this->focusElement->name);
			if (focusedView->KeyUp(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
			{
				return true;
			}
		}
	}
	
	//
	if (this->currentView != NULL)
	{
		if ((io.WantTextInput && currentView->imGuiSkipKeyPressWhenIoWantsTextInput == false)
			|| io.WantTextInput == false)
		{
			if (currentView->KeyUp(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
			{
				return true;
			}
		}
	}
		
	if (io.WantTextInput)
		return false;
	
	for (std::list<CGlobalKeyboardCallback *>::const_iterator itKeybardCallbacks =
			this->globalKeyboardCallbacks.begin();
			itKeybardCallbacks != this->globalKeyboardCallbacks.end();
			itKeybardCallbacks++)
	{
		CGlobalKeyboardCallback *callback = (CGlobalKeyboardCallback *) *itKeybardCallbacks;
		if (callback->GlobalKeyUpCallback(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed) == true)
			return true;
	}

	return false;
}

bool CGuiMain::KeyTextInput(const char *text)
{
	ImGuiIO& io = ImGui::GetIO();
	
	LOGI("CGuiMain::KeyTextInput: text=%s", text);

	// check not active views first. we iterate top-down and if mouse is on a window we pass the key in a special event
	// this is to allow key presses even if window is not focused, for example space-bar moving content like in image viewer
	
	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (!window->WasActive || window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL)
		{
			if (!view->visible)
				continue;
			
			if (io.WantTextInput && view->imGuiSkipKeyPressWhenIoWantsTextInput == true)
				continue;

			if (view->IsInside(mousePosX, mousePosY))
			{
				if (view->KeyTextInputOnMouseHover(text))
				{
					return true;
				}
				
				break;
			}
		}
	}
	
//	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
//	{
//		ImGuiWindow *window = context->Windows[i];
//		CGuiView *view = (CGuiView*)window->userData;
//
//		if (view != NULL)
//		{
//			view->KeyTextInputGlobal(text);
//		}
//	}
	
	
	if (this->focusedView)
	{
		if ((io.WantTextInput && focusedView->imGuiSkipKeyPressWhenIoWantsTextInput == false)
			|| io.WantTextInput == false)
		{
			// consumed?
			LOGI("CGuiMain::KeyTextInput: focusElement=%s", this->focusedView->name);
			if (focusedView->KeyTextInput(text))
			{
				return true;
			}
		}
	}

	// then change current view
	if (this->currentView != NULL)
	{
		if ((io.WantTextInput && currentView->imGuiSkipKeyPressWhenIoWantsTextInput == false)
			|| io.WantTextInput == false)
		{
			LOGI("                > currentView=%s", currentView->name);
			if (currentView->KeyTextInput(text))
			{
				return true;
			}
		}
	}
	
	LOGI("CGuiMain::KeyTextInput: not consumed by focusElement, io.WantTextInput=%s", STRBOOL(io.WantTextInput));
	
	if (io.WantTextInput)
		return false;
	
	for (std::list<CGlobalKeyboardCallback *>::const_iterator itKeybardCallbacks =
			this->globalKeyboardCallbacks.begin();
			itKeybardCallbacks != this->globalKeyboardCallbacks.end();
			itKeybardCallbacks++)
	{
		CGlobalKeyboardCallback *callback =
		(CGlobalKeyboardCallback *) *itKeybardCallbacks;
		if (callback->GlobalKeyTextInputCallback(text) == true)
		{
			return true;
		}
	}
	
//	// keyboard shortcuts
//	u32 keyCodeBare = SYS_GetBareKey(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
//
//	std::list<u32> zones;
//	zones.push_back(MT_KEYBOARD_SHORTCUT_GLOBAL);
//	CSlrKeyboardShortcut *shortcut = this->keyboardShortcuts->FindShortcut(zones, keyCodeBare,
//																		   isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
//
//	if (shortcut != NULL)
//	{
//		shortcut->DebugPrint();
//
//		if (shortcut->callback)
//		{
//			if (shortcut->callback->ProcessKeyboardShortcut(MT_KEYBOARD_SHORTCUT_GLOBAL, MT_ACTION_TYPE_KEYBOARD_SHORTCUT, shortcut))
//				return true;
//		}
//	}
//
	return false;

}

bool CGuiMain::IsOnAnyOpenedPopup(float px, float py)
{
	ImGuiContext *context = ImGui::GetCurrentContext();
	
	for (int n = 0; n < context->OpenPopupStack.Size; n++)
	{
//		LOGD("context->OpenPopupStack[n].Window->Pos.x=%f context->OpenPopupStack[n].Window->Pos.y=%f",
//			 context->OpenPopupStack[n].Window->Pos.x, context->OpenPopupStack[n].Window->Pos.y);
		
		if (context->OpenPopupStack[n].Window == NULL)
			continue;
		
		if (   (context->OpenPopupStack[n].Window->Pos.x < px)
			&& (px < context->OpenPopupStack[n].Window->Pos.x + context->OpenPopupStack[n].Window->Size.x)
			&& (context->OpenPopupStack[n].Window->Pos.y < py)
			&& (py < context->OpenPopupStack[n].Window->Pos.y + context->OpenPopupStack[n].Window->Size.y))
		{
			return true;
		}
	}
	
	return false;
}

void CGuiMain::DoDropFile(u32 windowId, char *filePath)
{
	bool consumedByView = false;
	if (IsOnAnyOpenedPopup(mousePosX, mousePosY) == false)
	{
		// iterate top-down by ImGui windows
		ImGuiContext *context = ImGui::GetCurrentContext();
		for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
		{
			ImGuiWindow *window = context->Windows[i];
			if (!window->WasActive || window->Hidden)
				continue;
			
			CGuiView *view = (CGuiView*)window->userData;

			if (view != NULL && view->IsVisible())
			{
	//			LOGD("....view=%s", view->name);
				if (view->IsInside(mousePosX, mousePosY))
				{
	//				LOGD("....... IsInside, DoDropFile()");
					if (view->DoDropFile(filePath))
					{
	//					LOGD("......... view %s consumed DropFile", view->name);
						consumedByView = true;
						break;
					}
				}
			}
		}
	}

	NotifyGlobalDropFileCallbacks(filePath, consumedByView);
	
//	LOGI("CGuiMain: DoDropFile finished (not consumed)");
	return;
}

bool CGuiMain::DoTap(float x, float y)
{
	LOGI("CGuiMain: DoTap: px=%3.2f; py=%3.2f;", x, y);
	
	isLeftMouseButtonPressed = true;
	
	moveStartTapPosX = movePrevTapPosX = x;
	moveStartTapPosY = movePrevTapPosY = y;
	
	if (IsOnAnyOpenedPopup(x, y))
	{
		LOGI("...is on popup, skipping tap");
		return false;
	}
	
#if !defined(FINAL_RELEASE)
	if (x > SCREEN_WIDTH - 20 && y < 20)
		//if (x > SCREEN_WIDTH - 20 && y > SCREEN_HEIGHT - 20)
		//if (x < 20 && y > SCREEN_HEIGHT - 20)
	{
		StartResourceManager();
		return true;
	}
#endif
	
	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		CGuiView *view = (CGuiView*)window->userData;
		
		if (view != NULL)
		{
			LOGI("....view=%s", view->name);
		}
		else
		{
			LOGI("....view=NULL");
		}

//		LOGD("window->Hidden=%s window->WasActive=%s", STRBOOL(window->Hidden), STRBOOL(window->WasActive));
		
		if (!window->WasActive || window->Hidden)
			continue;


		if (view != NULL && view->IsVisible())
		{
			LOGI("....view=%s IsInside?", view->name);
			if (view->IsInsideView(x, y))
			{
				LOGI("....... IsInsideView, DoTap(): %s", view->name);
				if (view->DoTap(x, y))
				{
					LOGI("......... view %s consumed tap", view->name);
					return true;
				}
			}
			else
			{
				LOGI("....view=%s not IsInsideView");
			}
		}
	}
	
//	if (focusElement)
//	{
//		if (focusElement->DoTap(x, y))
//			return true;
//	}
	
//	for (std::list<CGuiView *>::iterator itView = this->views.begin(); itView != this->views.end(); itView++)
//	{
//		CGuiView *view = *itView;
//		if (!view->visible)
//			continue;
//		
//		if (view->DoTap(x, y))
//			return true;
//	}
	
	LOGI("CGuiMain: DoTap finished (not consumed)");
	return false;
}

bool CGuiMain::DoFinishTap(float x, float y)
{
	LOGI("CGuiMain: DoFinishTap: px=%3.2f; py=%3.2f;", x, y);

	isLeftMouseButtonPressed = false;
	
	moveStartTapPosX = moveStartTapPosY = movePrevTapPosX = movePrevTapPosY = -1;
	
	if (IsOnAnyOpenedPopup(x, y))
	{
//		LOGI("...is on popup, skipping tap");
		return false;
	}
	
	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (!window->WasActive || window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;
		LOGI("...view=%s visible=%s", view ? view->name : "NULL", view ? STRBOOL(view->IsVisible()) : "");
		
		if (view != NULL && view->IsVisible())
		{
			LOGI("....view=%s IsInside?", view->name);
			if (view->IsInsideView(x, y))
			{
				LOGI("....... IsInsideView, DoFinishTap(): %s", view->name);
				if (view->DoFinishTap(x, y))
				{
					LOGI("......... view %s consumed tap", view->name);
					return true;
				}
			}
			else
			{
				LOGI("....view=%s not IsInsideView");
			}
		}
	}
	
//	LOGI("...check focusElement=%s", focusElement ? focusElement->name : "NULL");
	if (focusedView)
	{
		if (focusedView->DoFinishTap(x, y))
		{
//			LOGI("...focusElement DoFinishTap consumed");
			return true;
		}
	}
	
//	for (std::list<CGuiView *>::iterator itView = this->views.begin(); itView != this->views.end(); itView++)
//	{
//		CGuiView *view = *itView;
//		if (!view->visible)
//			continue;
//
//		if (view->DoFinishTap(x, y))
//			return true;
//	}

//	LOGI("...DoFinishTap not consumed");
	return false;
}

bool CGuiMain::DoRightClick(float x, float y)
{
	LOGI("CGuiMain: DoRightClick: px=%3.2f; py=%3.2f;", x, y);
	
	isRightMouseButtonPressed = true;
	
	moveStartRightClickPosX = movePrevRightClickPosX = x;
	moveStartRightClickPosY = movePrevRightClickPosY = y;
	
	if (IsOnAnyOpenedPopup(x, y))
	{
		LOGI("...is on popup, skipping right-click");
		return false;
	}
	
#if !defined(FINAL_RELEASE)
	if (x > SCREEN_WIDTH - 20 && y < 20)
		//if (x > SCREEN_WIDTH - 20 && y > SCREEN_HEIGHT - 20)
		//if (x < 20 && y > SCREEN_HEIGHT - 20)
	{
		StartResourceManager();
		return true;
	}
#endif
	
	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (!window->WasActive || window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL && view->IsVisible())
		{
//			LOGD("....view=%s", view->name);
			if (view->IsInsideView(x, y))
			{
//				LOGD("....... IsInside, DoTap()");
				if (view->DoRightClick(x, y))
				{
//					LOGD("......... view %s consumed tap", view->name);
					return true;
				}
			}
		}
	}
	
//	if (focusElement)
//	{
//		if (focusElement->DoTap(x, y))
//			return true;
//	}
	
//	for (std::list<CGuiView *>::iterator itView = this->views.begin(); itView != this->views.end(); itView++)
//	{
//		CGuiView *view = *itView;
//		if (!view->visible)
//			continue;
//
//		if (view->DoTap(x, y))
//			return true;
//	}
	
	LOGI("CGuiMain: DoRightClick finished (not consumed)");
	return false;
}

bool CGuiMain::DoFinishRightClick(float x, float y)
{
	LOGI("CGuiMain: DoFinishRightClick: px=%3.2f; py=%3.2f;", x, y);

	isRightMouseButtonPressed = false;
	
	moveStartRightClickPosX = moveStartRightClickPosY = movePrevRightClickPosX = movePrevRightClickPosY = -1;
	
	if (IsOnAnyOpenedPopup(x, y))
	{
		LOGI("...is on popup, skipping tap");
		return false;
	}
	
	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (!window->WasActive || window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL && view->IsVisible())
		{
			if (view->DoFinishRightClick(x, y))
			{
				return true;
			}
		}
	}
	
	if (focusedView)
	{
		if (focusedView->DoFinishRightClick(x, y))
			return true;
	}
	
//	for (std::list<CGuiView *>::iterator itView = this->views.begin(); itView != this->views.end(); itView++)
//	{
//		CGuiView *view = *itView;
//		if (!view->visible)
//			continue;
//
//		if (view->DoFinishTap(x, y))
//			return true;
//	}

	return false;
}

bool CGuiMain::DoMove(float x, float y)
{
	isLeftMouseButtonPressed = true;
	
	if (IsOnAnyOpenedPopup(x, y))
	{
		LOGI("...is on popup, skipping move");
		return false;
	}
	
#if !defined(FINAL_RELEASE)
	if (x > SCREEN_WIDTH - 20 && y < 20)
		//if (x > SCREEN_WIDTH - 20 && y > SCREEN_HEIGHT - 20)
		//if (x < 20 && y > SCREEN_HEIGHT - 20)
	{
		StartResourceManager();
		return true;
	}
#endif
	
	float distX = x - moveStartTapPosX;
	float distY = y - moveStartTapPosY;
	float diffX = x - movePrevTapPosX;
	float diffY = y - movePrevTapPosY;
	movePrevTapPosX = x;
	movePrevTapPosY = y;
	
	if (this->currentView != NULL)
	{
		// TODO: FIXME, DoMove temporarily does not forward these values
		if (this->currentView->DoMove(x, y, distX, distY, diffX, diffY))
		{
			return true;
		}
	}
		
	if (this->focusedView)
	{
		// consumed?
		if (this->focusedView->DoMove(x, y, distX, distY, diffX, diffY))
		{
			return true;
		}
	}
	return false;
	
	/*
	
	
	
	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (!window->WasActive || window->Hidden)
			continue;
	 
		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL && view->IsVisible())
		{
			// TODO: FIXME, DoMove temporarily does not forward these values
			if (view->DoMove(x, y, 0, 0, 0, 0))
			{
				return true;
			}
		}
	}
	*/
	return false;
}

bool CGuiMain::DoRightClickMove(float x, float y)
{
//	LOGD("CGuiMain::DoRightClickMove");
	
	isRightMouseButtonPressed = true;
	
	if (IsOnAnyOpenedPopup(x, y))
	{
		LOGI("...is on popup, skipping move");
		return false;
	}
	
#if !defined(FINAL_RELEASE)
	if (x > SCREEN_WIDTH - 20 && y < 20)
		//if (x > SCREEN_WIDTH - 20 && y > SCREEN_HEIGHT - 20)
		//if (x < 20 && y > SCREEN_HEIGHT - 20)
	{
		StartResourceManager();
		return true;
	}
#endif
	
	float distX = x - moveStartRightClickPosX;
	float distY = y - moveStartRightClickPosY;
	float diffX = x - movePrevRightClickPosX;
	float diffY = y - movePrevRightClickPosY;
	movePrevRightClickPosX = x;
	movePrevRightClickPosY = y;

	if (this->currentView != NULL)
	{
		// TODO: FIXME, DoRightClickMove temporarily does not forward these values
		if (this->currentView->DoRightClickMove(x, y, distX, distY, diffX, diffY))
		{
			return true;
		}
	}
		
	if (this->focusedView)
	{
		// consumed?
		if (this->focusedView->DoRightClickMove(x, y, distX, distY, diffX, diffY))
		{
			return true;
		}
	}
	return false;
	
	/*
	
	
	
	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (!window->WasActive || window->Hidden)
			continue;
	 
		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL && view->IsVisible())
		{
			// TODO: FIXME, DoMove temporarily does not forward these values
			if (view->DoMove(x, y, 0, 0, 0, 0))
			{
				return true;
			}
		}
	}
	*/
	return false;
}


void CGuiMain::DoNotTouchedMove(float x, float y)
{
	mousePosX = x;
	mousePosY = y;
	
	if (currentView && currentView->IsVisible())
	{
		currentView->DoNotTouchedMove(x, y);
//		return;
	}
	
	//	LOGD("--- DoNotTouchedMove ---");
	for (std::list<CGuiView *>::iterator itView = this->views.begin(); itView != this->views.end(); itView++)
	{
		CGuiView *view = *itView;
		if (!view->IsVisible())
			continue;
		
		view->DoNotTouchedMove(x, y);
		
		// TODO: refactor DoNotTouchedMove to not return anything
	}
}

void CGuiMain::DoScrollWheel(float deltaX, float deltaY)
{
	LOGG("CGuiMain::DoScrollWheel: %f %f", deltaX, deltaY);
	
	deltaX *= mouseScrollWheelScaleX;
	deltaY *= mouseScrollWheelScaleY;

	if (IsOnAnyOpenedPopup(mousePosX, mousePosY))
	{
		LOGI("...is on popup, skipping scroll wheel");
		return;
	}
	
	// TODO: iteration top-down by ImGui windows is not valid after system open file dialog on macos

	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (!window->WasActive || window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL)
		{
			LOGG("  view->name=%s visible=%s", view->name, STRBOOL(view->visible));
			
			if (!view->visible)
				continue;
			
			LOGG("  view->IsInside(%f %f)  window: posX=%5.2f posEndX=%5.2f posY=%5.2f posEndY=%5.2f", mousePosX, mousePosY, view->windowPosX, view->windowPosEndX, view->windowPosY, view->windowPosEndY);
			if (view->IsInsideView(mousePosX, mousePosY))
			{
				LOGG("  view %s ->DoScrollWheel(%f %f)", view->name, deltaX, deltaY);
				if (view->DoScrollWheel(deltaX, deltaY))
				{
					LOGG("  view %s consumed scroll wheel event", view->name);
					return;
				}
				else
				{
//					LOGG("  view %s NOT consumed scroll wheel event", view->name);					
				}
			}
			else
			{
//				LOGG("  NOT inside view %s", view->name);
			}
		}
	}
	
	/*
	for (std::list<CGuiView *>::iterator itView = views.begin(); itView != views.end(); itView++)
	{
		CGuiView *view = *itView;
		
		LOGG("  view->name=%s visible=%s", view->name, STRBOOL(view->visible));
		
		if (!view->visible)
			continue;
		
		LOGG("  view->IsInsideView(%f %f)", mousePosX, mousePosY);
		if (view->IsInsideView(mousePosX, mousePosY))
		{
			LOGG("  view %s ->DoScrollWheel(%f %f)", view->name, deltaX, deltaY);
			if (view->DoScrollWheel(deltaX, deltaY))
			{
				return;
			}
		}
	}
	*/
	
	// if not then by focus
	if (focusedView != NULL)
	{
		focusedView->DoScrollWheel(deltaX, deltaY);
	}
}

bool CGuiMain::DoGamePadButtonDown(CGamePad *gamePad, u8 button)
{
	LOGI("CGuiMain::DoGamePadButtonDown: name=%s button=%d", gamePad->name, button);

	// game pads are special so all views should receive the event
	for (std::list<CGuiView *>::iterator it = views.begin(); it != views.end(); it++)
	{
		CGuiView *view = *it;
		view->DoGamePadButtonDown(gamePad, button);
	}

	return false;
}

bool CGuiMain::DoGamePadButtonUp(CGamePad *gamePad, u8 button)
{
	LOGI("CGuiMain::DoGamePadButtonUp: name=%s button=%d", gamePad->name, button);

	// game pads are special so all views should receive the event
	for (std::list<CGuiView *>::iterator it = views.begin(); it != views.end(); it++)
	{
		CGuiView *view = *it;
		view->DoGamePadButtonUp(gamePad, button);
	}

	return false;
}

bool CGuiMain::DoGamePadAxisMotion(CGamePad *gamePad, u8 axis, int value)
{
//	LOGI("CGuiMain::DoGamePadAxisMotion: name=%s axis=%d value=%d", gamePad->name, axis, value);

	// game pads are special so all views should receive the event
	for (std::list<CGuiView *>::iterator it = views.begin(); it != views.end(); it++)
	{
		CGuiView *view = *it;
		view->DoGamePadAxisMotion(gamePad, axis, value);
	}
	
	// generate dpad button DoGamePadAxisMotionButtonDown/Up based on analog stick
	gamePad->GamePadAxisMotionToButtonEvent(axis, value);

	return false;
}

// these are emulated button presses, from analog stick to dpad, normally they should be routed by view to normal DoGamePadButtonDown
bool CGuiMain::DoGamePadAxisMotionButtonDown(CGamePad *gamePad, u8 button)
{
	LOGI("CGuiMain::DoGamePadAxisMotionButtonDown: name=%s button=%d", gamePad->name, button);

	// game pads are special so all views should receive the event
	for (std::list<CGuiView *>::iterator it = views.begin(); it != views.end(); it++)
	{
		CGuiView *view = *it;
		view->DoGamePadAxisMotionButtonDown(gamePad, button);
	}

	return false;
}

bool CGuiMain::DoGamePadAxisMotionButtonUp(CGamePad *gamePad, u8 button)
{
	LOGI("CGuiMain::DoGamePadAxisMotionButtonUp: name=%s button=%d", gamePad->name, button);

	// game pads are special so all views should receive the event
	for (std::list<CGuiView *>::iterator it = views.begin(); it != views.end(); it++)
	{
		CGuiView *view = *it;
		view->DoGamePadAxisMotionButtonUp(gamePad, button);
	}

	return false;
}


void CGuiMain::SetWindowOnTop(CGuiView *view)
{
	// deprecated
	LOGTODO("CGuiMain::SetWindowOnTop");
}

void CGuiMain::ShowMessageBox(const char *title, const char *message)
{
	ShowMessageBox(title, message, NULL);
}

void CGuiMain::ShowMessageBox(const char *title, const char *message, CUiMessageBoxCallback *messageBoxCallback)
{
	LockMutex();
	if (messageBoxTitle != NULL)
		STRFREE(messageBoxTitle);
	if (messageBoxText != NULL)
		STRFREE(messageBoxText);
	messageBoxTitle = STRALLOC(title);
	messageBoxText = STRALLOC(message);
	beginMessageBoxPopup = true;
	this->messageBoxCallback = messageBoxCallback;
	UnlockMutex();
}

void CGuiMain::ShowNotification(const char *title, const char *message)
{
	notificationMutex->Lock();
//	ImGui::InsertNotification({ ImGuiToastType_Success, title, 3000, message });
	ImGui::InsertNotification({ ImGuiToastType_Info, title, 3000, message });
	notificationMutex->Unlock();
}

void CGuiMain::ShowNotificationError(const char *title, const char *message)
{
	notificationMutex->Lock();
	ImGui::InsertNotification({ ImGuiToastType_Error, title, 3000, message });
	notificationMutex->Unlock();
}

void CGuiMain::ShowNotification(ImGuiToastType_ toastType, int dismissTime, const char *title, const char *message)
{
	notificationMutex->Lock();
	ImGui::InsertNotification({ toastType, title, dismissTime, message });
	notificationMutex->Unlock();
}

void CGuiMain::RenderImGui()
{
	focusedViewThisFrameOnly = NULL;
	
	ImGuiIO& io = ImGui::GetIO();
//	ImGuiContext *context = ImGui::GetCurrentContext();
	
//	LOGD("mousePoxX=%f mousePosY=%f", mousePosX, mousePosY);
//	LOGD("	io.WantCaptureMouse=%s 	io.WantCaptureKeyboard=%s  io.WantTextInput=%s  io.context.OpenPopupStack.Size=%d",
//		 STRBOOL(io.WantCaptureMouse),
//		 STRBOOL(io.WantCaptureKeyboard),
//		 STRBOOL(io.WantTextInput),
//		 context->OpenPopupStack.Size);
	
	if (currentView != NULL)
	{
		currentView->RenderImGui();
	}
			
//	if (ImGui::IsAnyWindowFocused() == false)
	if (io.WantCaptureMouse == false)
	{
		ClearInternalViewFocus();
	}
	
	//
	for (std::list<CGuiView *>::iterator itView = this->views.begin(); itView != this->views.end(); itView++)
	{
		CGuiView *view = *itView;
		if (!view->visible)
			continue;
		
		view->RenderImGui();
		
		// TODO: refactor DoNotTouchedMove to not return anything
	}

	////////////////////
	
	// notifications
	notificationMutex->Lock();
	ImGui::RenderNotifications();
	notificationMutex->Unlock();

	// message boxes
	if (beginMessageBoxPopup)
	{
		ImGui::OpenPopup(messageBoxTitle);
		beginMessageBoxPopup = false;
	}
	if (messageBoxTitle)
	{
		// Center this window inside the main application window (not the monitor)
		ImGuiViewport* viewport = ImGui::GetMainViewport(); // or GetWindowViewport() if called inside another window

		// Use WorkPos/WorkSize so the centering ignores menu bars and dockspace toolbars
		ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
					  viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);

		// Ensure the window spawns on the intended viewport and is centered there
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

		// Keep your min size (optional)
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(200, 75));
			
		bool popen = true;
		if (ImGui::BeginPopupModal(messageBoxTitle, &popen, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("%s", messageBoxText);
			if (ImGui::Button("  OK  "))
			{
				STRFREE(messageBoxTitle);
				STRFREE(messageBoxText);
				ImGui::CloseCurrentPopup();
				
				if (messageBoxCallback)
				{
					messageBoxCallback->MessageBoxCallback();
				}
				messageBoxCallback = NULL;
			}
			ImGui::EndPopup();
		}
		ImGui::PopStyleVar();
	}
	
	if (focusedViewThisFrameOnly == NULL)
	{
		ClearInternalViewFocus();
//		LOGD("focusedViewThisFrameOnly=%x focusedView=%x", focusedViewThisFrameOnly, focusedView);
	}
	
	LOGG("Render: focusedView=%x focusedViewThisFrameOnly=%x", focusedView, focusedViewThisFrameOnly);
}

void CGuiMain::UpdateLayouts()
{
	//
	if (layoutJustRestored)
	{
		layoutJustRestored = false;
	}
	
	if (layoutStoreCurrentInSettings)
	{
		LOGG("layoutStoreCurrentInSettings: store layout now");
		// store previous layout
		if (layoutManager->currentLayout != NULL
			&& layoutManager->currentLayout->doNotUpdateViewsPositions == false)
		{
			layoutManager->currentLayout->serializedLayoutBuffer->Clear();
			SerializeLayout(layoutManager->currentLayout);
			layoutManager->StoreLayouts();
		}
		
		layoutStoreCurrentInSettings = false;
	}
	
	if (layoutForThisFrame != NULL)
	{
		if (layoutStoreOrRestore == LayoutStorageTask::StoreLayout)
		{
			LOGG("CGuiMain::RenderImGui: LayoutStorageTask::StoreLayout");
			this->SerializeLayout(layoutForThisFrame);
			layoutForThisFrame = NULL;
		}
		else if (layoutStoreOrRestore == LayoutStorageTask::RestoreLayout)
		{
			LOGG("CGuiMain::RenderImGui: LayoutStorageTask::RestoreLayout");
			// store previous layout
			if (layoutManager->currentLayout != NULL
				&& layoutManager->currentLayout->doNotUpdateViewsPositions == false)
			{
				layoutManager->currentLayout->serializedLayoutBuffer->Clear();
				SerializeLayout(layoutManager->currentLayout);
				layoutManager->StoreLayouts();
			}

			this->DeserializeLayout(layoutForThisFrame);
			
			layoutManager->currentLayout = layoutForThisFrame;
			layoutForThisFrame = NULL;
			layoutJustRestored = true;
		}
		else LOGError("CGuiMain::RenderImGui: unknown LayoutStorageTask=%d", layoutStoreOrRestore);
	}

}

void CGuiMain::PostRenderEndFrame()
{
//	LOGD("CGuiMain::PostRenderEndFrame");
	
	// check UI tasks
	uiThreadTasksMutex->Lock();
	while(!uiThreadTasks.empty())
	{
		CUiThreadTaskCallback *taskCallback = uiThreadTasks.front();
		uiThreadTasks.pop_front();
		taskCallback->RunUIThreadTask();
	}
	uiThreadTasksMutex->Unlock();

//	LOGD("CGuiMain::PostRenderEndFrame DONE");
}


void CGuiMain::StoreLayoutInSettingsAtEndOfThisFrame()
{
	LOGI("StoreLayoutInSettingsAtEndOfThisFrame");
	layoutStoreCurrentInSettings = true;
}

// iterate top-down by ImGui windows and find most top in x,y
CGuiView* CGuiMain::FindTopWindow(float x, float y)
{
//	LOGI("....FindTopWindow %f %f", x, y);
	LockMutex();
	
	ImGuiContext *context = ImGui::GetCurrentContext();
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (!window->WasActive || window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL && view->IsVisible() && !view->IsHidden())
		{
//			LOGI("....FindTopWindow: view=%s view->visible=%s", view->name, STRBOOL(view->visible));
			if (view->IsInsideWindow(x, y))
			{
//				LOGI("....FindTopWindow: inside window");
				UnlockMutex();
				return view;
			}
		}
	}
	
	UnlockMutex();
	return NULL;
}

// TODO: move to VID_* or move VID_AlwaysOnTop here
// returns if view is hidden (i.e. hidden or minimized)
bool CGuiMain::IsViewHidden(CGuiView *view)
{
	SDL_Window *window = VID_GetSDLWindowFromCGuiView(view);

	if (!window)
		return true;

	// check main window
	Uint32 flags = SDL_GetWindowFlags(window);
	
	if (flags & SDL_WINDOW_HIDDEN
		|| flags & SDL_WINDOW_MINIMIZED)
	{
		return true;
	}
	
	return false;
}

void CGuiMain::AddUiThreadTask(CUiThreadTaskCallback *taskCallback)
{
	uiThreadTasksMutex->Lock();
	uiThreadTasks.push_back(taskCallback);
	uiThreadTasksMutex->Unlock();
}

void CGlobalLogicCallback::GlobalLogicCallback() {
}

void CGuiMain::ClearGlobalLogicCallbacks() {
	this->globalLogicCallbacks.clear();
}

void CGuiMain::AddGlobalLogicCallback(CGlobalLogicCallback *callback) {
	for (std::list<CGlobalLogicCallback *>::iterator it =
			this->globalLogicCallbacks.begin();
			it != this->globalLogicCallbacks.end(); it++) {
		CGlobalLogicCallback *val = (*it);
		if (val == callback) {
			LOGWarning("AddGlobalLogicCallback: double callback");
			return;
		}
	}
	this->globalLogicCallbacks.push_back(callback);
}

void CGuiMain::RemoveGlobalLogicCallback(CGlobalLogicCallback *callback) {
	this->globalLogicCallbacks.remove(callback);
}

//
void CGlobalOSWindowChangedCallback::GlobalOSWindowChangedCallback() {
}

void CGuiMain::ClearGlobalOSWindowChangedCallbacks() {
	this->globalOSWindowChangedCallbacks.clear();
}

void CGuiMain::AddGlobalOSWindowChangedCallback(CGlobalOSWindowChangedCallback *callback)
{
	for (std::list<CGlobalOSWindowChangedCallback *>::iterator it =
			this->globalOSWindowChangedCallbacks.begin();
			it != this->globalOSWindowChangedCallbacks.end(); it++)
	{
		CGlobalOSWindowChangedCallback *val = (*it);
		if (val == callback)
		{
			LOGWarning("AddGlobalOSWindowChangedCallback: double callback");
			return;
		}
	}
	this->globalOSWindowChangedCallbacks.push_back(callback);
}

void CGuiMain::RemoveGlobalOSWindowChangedCallback(CGlobalOSWindowChangedCallback *callback) {
	this->globalOSWindowChangedCallbacks.remove(callback);
}

void CGuiMain::NotifyGlobalOSWindowChangedCallbacks()
{
	for (std::list<CGlobalOSWindowChangedCallback *>::const_iterator it = this->globalOSWindowChangedCallbacks.begin();
			it != this->globalOSWindowChangedCallbacks.end();
			it++)
	{
		CGlobalOSWindowChangedCallback *callback = (CGlobalOSWindowChangedCallback *) *it;
		callback->GlobalOSWindowChangedCallback();
	}
}

//
void CGlobalDropFileCallback::GlobalDropFileCallback(char *filePath, bool consumedByView) {
}

void CGuiMain::ClearGlobalDropFileCallbacks() {
	this->globalDropFileCallbacks.clear();
}

void CGuiMain::AddGlobalDropFileCallback(CGlobalDropFileCallback *callback)
{
	for (std::list<CGlobalDropFileCallback *>::iterator it =
			this->globalDropFileCallbacks.begin();
			it != this->globalDropFileCallbacks.end(); it++)
	{
		CGlobalDropFileCallback *val = (*it);
		if (val == callback)
		{
			LOGWarning("AddGlobalDropFileCallback: double callback");
			return;
		}
	}
	this->globalDropFileCallbacks.push_back(callback);
}

void CGuiMain::RemoveGlobalDropFileCallback(CGlobalDropFileCallback *callback) {
	this->globalDropFileCallbacks.remove(callback);
}

void CGuiMain::NotifyGlobalDropFileCallbacks(char *filePath, bool consumedByView)
{
	for (std::list<CGlobalDropFileCallback *>::const_iterator it = this->globalDropFileCallbacks.begin();
			it != this->globalDropFileCallbacks.end();
			it++)
	{
		CGlobalDropFileCallback *callback = (CGlobalDropFileCallback *) *it;
		callback->GlobalDropFileCallback(filePath, consumedByView);
	}
}

void CGuiMain::MergeIconsWithLatestFont(float fontSize)
{
	ImGui::MergeIconsWithLatestFont(fontSize);
}

void CGuiMain::CreateUiFontsTexture(float fontSize)
{
//	MergeIconsWithLatestFont(fontSize);
	gRenderBackend->CreateFontsTexture();
}

//
void CGlobalKeyboardCallback::GlobalPreKeyDownCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
}

void CGlobalKeyboardCallback::GlobalPreKeyUpCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
}

bool CGlobalKeyboardCallback::GlobalKeyDownCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return false;
}

bool CGlobalKeyboardCallback::GlobalKeyUpCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return false;
}

bool CGlobalKeyboardCallback::GlobalKeyPressCallback(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	return false;
}

bool CGlobalKeyboardCallback::GlobalKeyTextInputCallback(const char *text)
{
	return false;
}

void CGuiMain::ClearGlobalKeyboardCallbacks()
{
	this->globalKeyboardCallbacks.clear();
}

void CGuiMain::AddGlobalKeyboardCallback(CGlobalKeyboardCallback *callback)
{
	for (std::list<CGlobalKeyboardCallback *>::iterator it =
			this->globalKeyboardCallbacks.begin();
			it != this->globalKeyboardCallbacks.end(); it++)
	{
		CGlobalKeyboardCallback *val = (*it);
		if (val == callback)
		{
			LOGWarning("AddGlobalKeyboardCallback: double callback");
			return;
		}
	}
	this->globalKeyboardCallbacks.push_back(callback);
}

void CGuiMain::RemoveGlobalKeyboardCallback(CGlobalKeyboardCallback *callback)
{
	this->globalKeyboardCallbacks.remove(callback);
}

void CGuiMain::LockMutex()
{
	renderMutex->Lock();
}

bool CGuiMain::TryLockMutex()
{
	return renderMutex->TryLock();
}

void CGuiMain::UnlockMutex()
{
	renderMutex->Unlock();
}

CUiMessageBoxCallback::CUiMessageBoxCallback()
{
	uiMessageBoxCallbackUserData = NULL;
}

CUiMessageBoxCallback::~CUiMessageBoxCallback()
{
}

void CUiMessageBoxCallback::MessageBoxCallback()
{
}

void CUiMessageBoxCallbackRestartApplication::MessageBoxCallback()
{
	SYS_RestartApplication();
}

CUiThreadTaskCallback::CUiThreadTaskCallback()
{
	uiThreadTaskCallbackUserData = NULL;
}
	
CUiThreadTaskCallback::~CUiThreadTaskCallback()
{
}

void CUiThreadTaskCallback::RunUIThreadTask()
{
}

void CUiThreadTaskSetView::RunUIThreadTask()
{
	LOGG("CUiThreadTaskSetView::RunUIThreadTask");
	
	// TODO: FIX ME  guiMain->SetViewAsync(this->view);
}

void CGuiMain::RaiseMainWindow()
{
	CUiThreadTaskRaiseMainWindow *task = new CUiThreadTaskRaiseMainWindow();
	guiMain->AddUiThreadTask(task);
}

void CGuiMain::CloseCurrentImGuiWindow()
{
	LockMutex();
	for (std::list<CGuiView *>::iterator it = views.begin(); it != views.end(); it++)
	{
		CGuiView *view = *it;
		if (view->HasFocus())
		{
			view->SetVisible(false);
			break;
		}
	}
	UnlockMutex();
}

void CUiThreadTaskRaiseMainWindow::RunUIThreadTask()
{
	VID_RaiseMainWindow();
}

CUiThreadTaskSetMouseCursorVisible::CUiThreadTaskSetMouseCursorVisible(bool mouseCursorVisible)
{
	this->mouseCursorVisible = mouseCursorVisible;
}
	
void CUiThreadTaskSetMouseCursorVisible::RunUIThreadTask()
{
	LOGG("CUiThreadTaskSetMouseCursorVisible::RunUIThreadTask: VID_IsMouseCursorVisible=%d, set=%d", VID_IsMouseCursorVisible(), mouseCursorVisible);
	if (VID_IsMouseCursorVisible() == mouseCursorVisible)
	{
		return;
	}
	
	if (mouseCursorVisible)
	{
		VID_ShowMouseCursor();
	}
	else
	{
		VID_HideMouseCursor();
	}
	
	guiMain->isMouseCursorVisible = mouseCursorVisible;
}

CUiThreadTaskSetAlwaysOnTop::CUiThreadTaskSetAlwaysOnTop(CGuiView *view, bool isAlwaysOnTop)
{
	this->view = view;
	this->isAlwaysOnTop = isAlwaysOnTop;
}
	
void CUiThreadTaskSetAlwaysOnTop::RunUIThreadTask()
{
	if (view == NULL)
	{
		VID_SetMainWindowAlwaysOnTop(isAlwaysOnTop);
	}
	else
	{
		VID_SetWindowAlwaysOnTop(view, isAlwaysOnTop);
	}
}

void CGuiMain::SetMouseCursorVisible(bool isVisible)
{
	if (IsMouseCursorVisible() == isVisible)
		return;
	
	CUiThreadTaskSetMouseCursorVisible *task = new CUiThreadTaskSetMouseCursorVisible(isVisible);
	AddUiThreadTask(task);
}

bool CGuiMain::IsMouseCursorVisible()
{
	return VID_IsMouseCursorVisible();
}

bool CGuiMain::IsApplicationWindowFullScreen()
{
	return VID_IsMainApplicationWindowFullScreen();
}

void CGuiMain::SetApplicationWindowFullScreen(bool isFullScreen)
{
	VID_SetMainApplicationWindowFullScreen(isFullScreen);
}

void CGuiMain::SetImGuiStyleWindowFullScreenBackground()
{
	ImGuiCol_WindowBg_Backup = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
	ImGuiCol_DockingEmptyBg_Backup = ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg];

	ImVec4 fullScreenBackgroundColor = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
	ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = fullScreenBackgroundColor;
	ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg] = fullScreenBackgroundColor;
}

void CGuiMain::SetImGuiStyleWindowBackupBackground()
{
	ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImGuiCol_WindowBg_Backup;
	ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg] = ImGuiCol_DockingEmptyBg_Backup;
}


void CGuiMain::SetApplicationWindowAlwaysOnTop(bool alwaysOnTop)
{
	CUiThreadTaskSetAlwaysOnTop *task = new CUiThreadTaskSetAlwaysOnTop(NULL, alwaysOnTop);
	AddUiThreadTask(task);
}

void CGuiMain::RemoveAllViews()
{
	LOGTODO("CGuiMain::RemoveAllViews NOT IMPLEMENTED");
}

// TODO: fullscreen: there may be a situation that layout was not stored and async task loop was started between render unlock and renderPostEndFrame. check on other platforms. remove debug logs when it is confirmed working

// go fullscreen with one view (for example emulator screen), or get back to windowed mode when view is NULL
void CGuiMain::SetViewFullScreen(SetFullScreenMode setFullScreenMode, CGuiView *view)
{
	SetViewFullScreen(setFullScreenMode, view, view ? view->sizeX : 0, view ? view->sizeY : 0);
}

const char *GetFullScreenModeText(SetFullScreenMode setFullScreenMode)
{
	switch(setFullScreenMode)
	{
		case SetFullScreenMode::MainWindowEnterFullScreen:
			return "MainWindowEnterFullScreen";
		case SetFullScreenMode::MainWindowLeaveFullScreen:
			return "MainWindowLeaveFullScreen";
		case SetFullScreenMode::ViewEnterFullScreen:
			return "ViewEnterFullScreen";
		case SetFullScreenMode::ViewLeaveFullScreen:
			return "ViewLeaveFullScreen";
	}
	return "UnknownFullScreen";
}

void CGuiMain::SetViewFullScreen(SetFullScreenMode setFullScreenMode, CGuiView *view, float fullScreenSizeX, float fullScreenSizeY)
{
	LOGD("CGuiMain::SetViewFullScreen: setFullScreenMode=%s view=%s currentLayout=%x '%s'", GetFullScreenModeText(setFullScreenMode), view ? view->name : "NULL", layoutManager->currentLayout, layoutManager->currentLayout ? layoutManager->currentLayout->layoutName : "NULL");

	if (setFullScreenMode == SetFullScreenMode::ViewEnterFullScreen
		|| setFullScreenMode == SetFullScreenMode::MainWindowEnterFullScreen)
	{
		if (setFullScreenMode == SetFullScreenMode::ViewEnterFullScreen && view == NULL)
		{
			LOGError("CGuiMain::SetViewFullScreen: ViewEnterFullScreen view==NULL");
			return;
		}
		
		if (IsViewFullScreen())
		{
			LOGError("CGuiMain::SetViewFullScreen: %s is already fullscreen", view->name);
			return;
		}
		
		if (currentLayoutBeforeFullScreen != NULL)
		{
			LOGError("CGuiMain::SetViewFullScreen: currentLayoutBeforeFullScreen != NULL");
			return;
		}
		
		LockMutex();
		
		isChangingFullScreenState = true;
		
		// when going full screen a layout is saved and restored when going back to windowed mode.
		// because currentLayout may have doNotUpdateViewsPositions we make a temporary copy
		// this is backup of currentLayout (may have the doNotUpdateViewsPositions set to true)
		currentLayoutBeforeFullScreen = layoutManager->currentLayout;
		layoutManager->currentLayout = NULL;
		
		// create a temporary layout to hold views to go back to windowed mode
		layoutForThisFrame = temporaryLayoutToGoBackFromFullScreen;
		layoutStoreOrRestore = LayoutStorageTask::StoreLayout;
		
		// the fullscreen mode will be started after layout is stored in an async task
		CUiThreadTaskSetViewFullScreen *task = new CUiThreadTaskSetViewFullScreen(setFullScreenMode, view, fullScreenSizeX, fullScreenSizeY);
		AddUiThreadTask(task);
				
		UnlockMutex();
	}
	else if (setFullScreenMode == SetFullScreenMode::ViewLeaveFullScreen
			 || setFullScreenMode == SetFullScreenMode::MainWindowLeaveFullScreen)
	{
		// go back to windowed mode
		if (temporaryLayoutToGoBackFromFullScreen == NULL)
		{
			LOGError("CGuiMain::SetViewFullScreen: temporaryLayoutToGoBackFromFullScreen == NULL");
			return;
		}
		
		LockMutex();

		layoutForThisFrame = temporaryLayoutToGoBackFromFullScreen;
		layoutStoreOrRestore = LayoutStorageTask::RestoreLayout;
		
		// the window mode will be restored after layout is restored in async task
		CUiThreadTaskSetViewFullScreen *task = new CUiThreadTaskSetViewFullScreen(setFullScreenMode, NULL, 0, 0);
		AddUiThreadTask(task);
		
		UnlockMutex();
	}
}

bool CGuiMain::IsViewFullScreen()
{
	return (viewFullScreen != NULL) || isChangingFullScreenState;
}

CUiThreadTaskSetViewFullScreen::CUiThreadTaskSetViewFullScreen(SetFullScreenMode setFullScreenMode, CGuiView *view, float fullScreenSizeX, float fullScreenSizeY)
{
	this->setFullScreenMode = setFullScreenMode;
	this->view = view;
	this->fullScreenSizeX = fullScreenSizeX;
	this->fullScreenSizeY = fullScreenSizeY;
}
	
void CUiThreadTaskSetViewFullScreen::RunUIThreadTask()
{
	LOGD("CUiThreadTaskSetViewFullScreen::RunUIThreadTask");
	
	//
	if (setFullScreenMode == SetFullScreenMode::ViewEnterFullScreen)
	{
		if (view == NULL)
		{
			LOGError("CUiThreadTaskSetViewFullScreen: view==NULL");
			return;
		}
		
		// view goes fullscreen
		LOGD("CUiThreadTaskSetViewFullScreen: fullscreen, view=%s", view->name);

		guiMain->viewFullScreen = view;
		view->visible = true;
		view->SetFullScreenViewSize(fullScreenSizeX, fullScreenSizeY);
		
		// make all other views invisible
		for (std::list<CGuiView *>::iterator it = guiMain->views.begin(); it != guiMain->views.end(); it++)
		{
			CGuiView *itView = *it;
			if (itView == view)
			{
				continue;
			}
			itView->SetVisible(false);
		}

		// make black background
		guiMain->SetImGuiStyleWindowFullScreenBackground();

		// the fullscreen mode is started after frame has been rendered and layout stored
		if (VID_IsMainApplicationWindowFullScreen())
		{
			LOGD("set appWasFullScreenBeforeViewFullScreen true");
			// main window is already in fullscreen
			guiMain->appWasFullScreenBeforeViewFullScreen = true;
		}
		else
		{
			LOGD("set appWasFullScreenBeforeViewFullScreen false, go fullscreen");
			guiMain->appWasFullScreenBeforeViewFullScreen = false;

			VID_SetMainApplicationWindowFullScreen(true);
		}
	}
	else if (setFullScreenMode == SetFullScreenMode::MainWindowEnterFullScreen)
	{
		VID_SetMainApplicationWindowFullScreen(true);
	}
	
	else if (setFullScreenMode == SetFullScreenMode::ViewLeaveFullScreen)
	{
		LOGD("CUiThreadTaskSetViewFullScreen: view goes back to windowed");
		
		// reset background to previous/backup from black background
		guiMain->SetImGuiStyleWindowBackupBackground();
		
		if (guiMain->appWasFullScreenBeforeViewFullScreen == false)
		{
			LOGD("appWasFullScreenBeforeViewFullScreen is false, go to window");
			
			// the fullscreen mode is stopped before layout is restored
			VID_SetMainApplicationWindowFullScreen(false);
		}
		
		LOGD(".......... in PostRenderEndFrame set BACK currentLayout to currentLayoutBeforeFullScreen");

		guiMain->layoutManager->currentLayout = guiMain->currentLayoutBeforeFullScreen;
		guiMain->currentLayoutBeforeFullScreen = NULL;
		
		guiMain->viewFullScreen = NULL;
		guiMain->isChangingFullScreenState = false;
	}
	else if (setFullScreenMode == SetFullScreenMode::MainWindowLeaveFullScreen)
	{
		LOGD("CUiThreadTaskSetViewFullScreen: mainwindow goes back to windowed");
		
		// reset background to previous/backup from black background
		guiMain->SetImGuiStyleWindowBackupBackground();
		
		// the fullscreen mode is stopped before layout is restored
		VID_SetMainApplicationWindowFullScreen(false);

		// TODO: store full screen layout
		
		LOGD(".......... in PostRenderEndFrame set BACK currentLayout to currentLayoutBeforeFullScreen");

		guiMain->layoutManager->currentLayout = guiMain->currentLayoutBeforeFullScreen;
		guiMain->currentLayoutBeforeFullScreen = NULL;
				
		guiMain->viewFullScreen = NULL;
		guiMain->isChangingFullScreenState = false;
	}
}

CUiThreadTaskSetLayout::CUiThreadTaskSetLayout(CLayoutData *layoutData, bool saveCurrentLayout)
{
	this->layoutData = layoutData;
	this->saveCurrentLayout = saveCurrentLayout;
}

void CUiThreadTaskSetLayout::RunUIThreadTask()
{
	guiMain->layoutManager->SetLayoutAsync(layoutData, saveCurrentLayout);
}

CUiThreadTaskSetViewFocus::CUiThreadTaskSetViewFocus(CGuiView *view)
{
	this->view = view;
}

void CUiThreadTaskSetViewFocus::RunUIThreadTask()
{
	guiMain->SetFocus(view);
}

CUiThreadTaskSetViewVisible::CUiThreadTaskSetViewVisible(CGuiView *view, bool isVisible)
{
	this->view = view;
	this->isVisible = isVisible;
}

void CUiThreadTaskSetViewVisible::RunUIThreadTask()
{
	view->visible = isVisible;
}


