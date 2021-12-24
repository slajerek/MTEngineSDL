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

#define CONSOLE_FONT_SIZE_X		0.03125
#define CONSOLE_FONT_SIZE_Y		0.03125
#define CONSOLE_FONT_PITCH_X	0.035156251
#define CONSOLE_FONT_PITCH_Y	0.035156251

CGuiMain::CGuiMain()
{
	currentView = NULL;
	
	globalConfig = new CConfigStorage();
	renderMutex = new CSlrMutex("CGuiMain::renderMutex");
	uiThreadTasksMutex = new CSlrMutex("CGuiMain::uiThreadTasksMutex");
	
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
	
	this->focusElement = NULL;
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

	isMouseCursorVisible = true;

	viewResourceManager = NULL;
	layoutForThisFrame = NULL;
	layoutJustRestored = false;
	layoutStoreCurrentInSettings = false;
	
	messageBoxTitle = NULL;
	messageBoxText = NULL;
	beginMessageBoxPopup = false;

	keyboardShortcuts = new CSlrKeyboardShortcuts();

	mouseScrollWheelScaleX = 1.0f;

#if defined(WIN32)
	mouseScrollWheelScaleY = 5.0f;
#else
	mouseScrollWheelScaleY = 1.0f;
#endif
	
	layoutManager->LoadLayouts();
}

void CGuiMain::AddView(CGuiView *view)
{
	this->views.push_back(view);
}

void CGuiMain::RemoveView(CGuiView *view)
{
	this->views.remove(view);
}

void CGuiMain::AddLayoutView(CGuiView *view)
{
	u64 hash = GetHashCode64(view->name);
	layoutViews[hash] = view;
}

void CGuiMain::RemoveLayoutView(CGuiView *view)
{
	u64 hash = GetHashCode64(view->name);
	layoutViews.erase(hash);
}

//
void CGuiMain::SerializeLayout(CByteBuffer *byteBuffer)
{
	byteBuffer->Clear();
	
	// put imgui ini
	size_t len = 0;
	const char *data = ImGui::SaveIniSettingsToMemory(&len);
	byteBuffer->PutU32(len+1);
	byteBuffer->PutBytes((u8*)data, len+1);
	
	byteBuffer->PutU32(layoutViews.size());
	for (std::map<u64, CGuiView *>::iterator it = layoutViews.begin(); it != layoutViews.end(); it++)
	{
		CGuiView *view = it->second;
		LOGG("serialize %s", view->name);
		byteBuffer->PutString(view->name);
		view->SerializeLayout(byteBuffer);
	}
}

// returns if failed
bool CGuiMain::DeserializeLayout(CByteBuffer *byteBuffer)
{
	if (!byteBuffer ||byteBuffer->length < 1)
		return false;
	
	byteBuffer->Rewind();
	
	u32 len = byteBuffer->GetU32();
	u8 *data = byteBuffer->GetBytes(len);
	
	ImGui::LoadIniSettingsFromMemory((const char*)data);
	
	delete [] data;
	
	u32 numViews = byteBuffer->GetU32();
	for (u32 i = 0; i < numViews; i++)
	{
		char *str = byteBuffer->GetString();
		u64 hash = GetHashCode64(str);
		LOGG("  view %s hash %x", str, hash);
		std::map<u64, CGuiView *>::iterator it = layoutViews.find(hash);
		if (it == layoutViews.end())
		{
			// view not found? error, break restore
			LOGD("   view not found %s hash %08x", str, hash);
			STRFREE(str);
			return false;
		}
		
		STRFREE(str);

		CGuiView *view = it->second;
		if (view->DeserializeLayout(byteBuffer) == false)
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

void CGuiMain::SetViewFocus(CGuiElement *viewToFocus)
{
	if (this->focusElement != viewToFocus)
	{
		this->ClearViewFocus();
		
		if (viewToFocus->SetFocus())
		{
			this->focusElement = viewToFocus;
			viewToFocus->hasFocus = true;
		}
	}
}

void CGuiMain::ClearViewFocus()
{
	if (focusElement != NULL)
	{
		focusElement->FocusLost();
		focusElement->hasFocus = false;
	}
	else
	{
		focusElement = NULL;
	}
//	LOGTODO("change viewC64 subviews to CGuiMain subviews so we can clear focus properly");
//	for (std::list<CGuiView *>::iterator itView = views.begin(); itView != views.end(); itView++)
//	{
//		CGuiElement *guiElement = (*itView);
//		guiElement->hasFocus = false;
//	}
	
}

void CGuiMain::AddKeyboardShortcut(CSlrKeyboardShortcut *keyboardShortcut)
{
	LOGD("CGuiMain::AddKeyboardShortcut: name=%s %s", keyboardShortcut->name, keyboardShortcut->cstr);
	this->keyboardShortcuts->AddShortcut(keyboardShortcut);
}

void CGuiMain::RemoveKeyboardShortcut(CSlrKeyboardShortcut *keyboardShortcut)
{
	LOGD("CGuiMain::RemoveKeyboardShortcut: name=%s %s", keyboardShortcut->name, keyboardShortcut->cstr);
	this->keyboardShortcuts->RemoveShortcut(keyboardShortcut);
}

bool CGuiMain::KeyDown(u32 keyCode)
{
	ImGuiIO& io = ImGui::GetIO();
	
	LOGI("CGuiMain::KeyDown: keyCode=%d (0x%2.2x = %c) isShift=%s isAlt=%s isControl=%s isSuper=%s | io.WantTextInput=%s", keyCode, keyCode, keyCode,
		 STRBOOL(isShiftPressed), STRBOOL(isAltPressed), STRBOOL(isControlPressed), STRBOOL(isSuperPressed), STRBOOL(io.WantTextInput));

	if (io.WantTextInput)
		return false;
	
	// check not active views first. we iterate top-down and if mouse is on a window we pass the key in a special event
	// this is to allow key presses even if window is not focused, for example space-bar moving content like in image viewer
	
	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL)
		{
			if (!view->visible)
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
	
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL)
		{
			view->KeyDownGlobal(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
		}
	}
	
	
	// then change current view
	if (this->currentView != NULL)
	{
		LOGI("                > currentView=%s", currentView->name);
		if (this->currentView->KeyDown(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
		{
			return true;
		}
	}
	
	if (this->focusElement)
	{
		// consumed?
		LOGI("CGuiMain::KeyDown: keyDown focusElement=%s", this->focusElement->name);
		if (this->focusElement->KeyDown(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
		{
			return true;
		}
	}
	
	LOGI("CGuiMain::KeyDown: not consumed by focusElement");
	
	for (std::list<CGlobalKeyboardCallback *>::const_iterator itKeybardCallbacks =
			this->globalKeyboardCallbacks.begin();
			itKeybardCallbacks != this->globalKeyboardCallbacks.end();
			itKeybardCallbacks++)
	{
		CGlobalKeyboardCallback *callback =
		(CGlobalKeyboardCallback *) *itKeybardCallbacks;
		if (callback->GlobalKeyDownCallback(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed) == true)
		{
			return true;
		}
	}
	
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
	
	
	return false;
}

bool CGuiMain::KeyDownRepeat(u32 keyCode)
{
	ImGuiIO& io = ImGui::GetIO();

	LOGI("CGuiMain::KeyDownRepeat: keyCode=%d (0x%2.2x = %c) isShift=%s isAlt=%s isControl=%s isSuper=%s | io.WantTextInput=%s", keyCode, keyCode, keyCode,
		 STRBOOL(isShiftPressed), STRBOOL(isAltPressed), STRBOOL(isControlPressed), STRBOOL(isSuperPressed), STRBOOL(io.WantTextInput));

	if (io.WantTextInput)
		return false;
	
	if (this->currentView != NULL)
	{
		LOGI("                > currentView=%s", currentView->name);
		if (this->currentView->KeyDownRepeat(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
		{
			return true;
		}
	}
	
	if (this->focusElement)
	{
		// consumed?
		LOGI("KeyDownRepeat focusElement=%s", this->focusElement->name);
		if (this->focusElement->KeyDownRepeat(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
		{
			return true;
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
	
	return false;
}

bool CGuiMain::KeyUp(u32 keyCode)
{
	ImGuiIO& io = ImGui::GetIO();

	LOGI("CGuiMain::KeyUp: keyCode=%d (0x%2.2x = %c) isShift=%s isAlt=%s isControl=%s | io.WantTextInput=%s", keyCode, keyCode, keyCode,
		 STRBOOL(isShiftPressed), STRBOOL(isAltPressed), STRBOOL(isControlPressed), STRBOOL(io.WantTextInput));

	if (io.WantTextInput)
		return false;

	// check not active views first. we iterate top-down and if mouse is on a window we pass the key in a special event
	// this is to allow key presses even if window is not fouces, for example space-bar moving of content like in image viewer
	
	// iterate top-down by ImGui windows
	ImGuiContext *context = ImGui::GetCurrentContext();
	
	for (int i = context->Windows.Size - 1; i >= 0; i--) // Iterate front to back
	{
		ImGuiWindow *window = context->Windows[i];
		if (window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL)
		{
			if (!view->visible)
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
			view->KeyUpGlobal(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
		}
	}
	
	//
	if (this->currentView != NULL)
	{
		if (this->currentView->KeyUp(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
		{
			return true;
		}
	}
		
	if (this->focusElement)
	{
		// consumed?
		LOGD("keyUp focusElement=%s", this->focusElement->name);
		if (this->focusElement->KeyUp(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed))
		{
			return true;
		}
	}
	
	for (std::list<CGlobalKeyboardCallback *>::const_iterator itKeybardCallbacks =
			this->globalKeyboardCallbacks.begin();
			itKeybardCallbacks != this->globalKeyboardCallbacks.end();
			itKeybardCallbacks++)
	{
		CGlobalKeyboardCallback *callback =
		(CGlobalKeyboardCallback *) *itKeybardCallbacks;
		if (callback->GlobalKeyUpCallback(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed) == true)
			return true;
	}

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
			if (window->Hidden)
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
	
	if (IsOnAnyOpenedPopup(x, y))
	{
//		LOGI("...is on popup, skipping tap");
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
		if (window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL && view->IsVisible())
		{
//			LOGI("....view=%s", view->name);
			if (view->IsInside(x, y))
			{
//				LOGI("....... IsInside, DoTap()");
				if (view->DoTap(x, y))
				{
//					LOGI("......... view %s consumed tap", view->name);
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
	
	LOGI("CGuiMain: DoTap finished (not consumed)");
	return false;
}

bool CGuiMain::DoFinishTap(float x, float y)
{
	LOGI("CGuiMain: DoFinishTap: px=%3.2f; py=%3.2f;", x, y);

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
		if (window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL && view->IsVisible())
		{
			if (view->DoFinishTap(x, y))
			{
				return true;
			}
		}
	}
	
	if (focusElement)
	{
		if (focusElement->DoFinishTap(x, y))
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




bool CGuiMain::DoRightClick(float x, float y)
{
	LOGI("CGuiMain: DoRightClick: px=%3.2f; py=%3.2f;", x, y);
	
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
		if (window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL && view->IsVisible())
		{
//			LOGD("....view=%s", view->name);
			if (view->IsInside(x, y))
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
		if (window->Hidden)
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
	
	if (focusElement)
	{
		if (focusElement->DoFinishRightClick(x, y))
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
	
	if (this->currentView != NULL)
	{
		// TODO: FIXME, DoMove temporarily does not forward these values
		if (this->currentView->DoMove(x, y, 0, 0, 0, 0))
		{
			return true;
		}
	}
		
	if (this->focusElement)
	{
		// consumed?
		if (this->focusElement->DoMove(x, y, 0, 0, 0, 0))
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
		if (window->Hidden)
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
	
	if (currentView)
	{
		currentView->DoNotTouchedMove(x, y);
//		return;
	}
	
	//	LOGD("--- DoNotTouchedMove ---");
	for (std::list<CGuiView *>::iterator itView = this->views.begin(); itView != this->views.end(); itView++)
	{
		CGuiView *view = *itView;
		if (!view->visible)
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
		if (window->Hidden)
			continue;

		CGuiView *view = (CGuiView*)window->userData;

		if (view != NULL)
		{
			LOGG("  view->name=%s visible=%s", view->name, STRBOOL(view->visible));
			
			if (!view->visible)
				continue;
			
//			LOGG("  view->IsInside(%f %f)  window: posX=%5.2f posEndX=%5.2f posY=%5.2f posEndY=%5.2f", mousePosX, mousePosY, view->windowPosX, view->windowPosEndX, view->windowPosY, view->windowPosEndY);
			if (view->IsInside(mousePosX, mousePosY))
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
		
		LOGG("  view->IsInside(%f %f)", mousePosX, mousePosY);
		if (view->IsInside(mousePosX, mousePosY))
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
	if (focusElement != NULL)
	{
		focusElement->DoScrollWheel(deltaX, deltaY);
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


void CGuiMain::SetWindowOnTop(CGuiElement *view)
{
	// deprecated
	LOGTODO("CGuiMain::SetWindowOnTop");
}

void CGuiMain::ShowMessageBox(const char *title, const char *message)
{
	guiMain->LockMutex();
	if (messageBoxTitle != NULL)
		STRFREE(messageBoxTitle);
	if (messageBoxText != NULL)
		STRFREE(messageBoxText);
	messageBoxTitle = STRALLOC(title);
	messageBoxText = STRALLOC(message);
	beginMessageBoxPopup = true;
	guiMain->UnlockMutex();
}

void CGuiMain::ShowMessage(const char *message)
{
	// TODO
}

void CGuiMain::ShowMessage(CSlrString *showMessage)
{
	char *cStr = showMessage->GetStdASCII();
	this->ShowMessage(cStr);
	delete [] cStr;
}

void CGuiMain::RenderImGui()
{
	ImGuiIO& io = ImGui::GetIO();
	ImGuiContext *context = ImGui::GetCurrentContext();
	
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
		ClearViewFocus();
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

	if (layoutJustRestored)
	{
		layoutJustRestored = false;
	}
	
	if (layoutStoreCurrentInSettings)
	{
		// store previous layout
		if (layoutManager->currentLayout != NULL
			&& layoutManager->currentLayout->doNotUpdateViewsPositions == false)
		{
			layoutManager->currentLayout->serializedLayoutBuffer->Clear();
			SerializeLayout(layoutManager->currentLayout->serializedLayoutBuffer);
			layoutManager->StoreLayouts();
		}
		
		layoutStoreCurrentInSettings = false;
	}
	
	if (layoutForThisFrame != NULL)
	{
		// TODO: add enum for this
		if (layoutStoreOrRestore)
		{
			// true means store
			this->SerializeLayout(layoutForThisFrame->serializedLayoutBuffer);
			layoutForThisFrame = NULL;
		}
		else
		{
			// false means restore
			
			// store previous layout
			if (layoutManager->currentLayout != NULL
				&& layoutManager->currentLayout->doNotUpdateViewsPositions == false)
			{
				layoutManager->currentLayout->serializedLayoutBuffer->Clear();
				SerializeLayout(layoutManager->currentLayout->serializedLayoutBuffer);
				layoutManager->StoreLayouts();
			}

			this->DeserializeLayout(layoutForThisFrame->serializedLayoutBuffer);
			layoutManager->currentLayout = layoutForThisFrame;
			layoutForThisFrame = NULL;
			layoutJustRestored = true;
		}
	}
	
	//
	if (beginMessageBoxPopup)
	{
		ImGui::OpenPopup(messageBoxTitle);
		beginMessageBoxPopup = false;
	}
	if (messageBoxTitle)
	{
		// Always center this window when appearing
		ImGuiPlatformIO platformIO = ImGui::GetPlatformIO();
		ImVec2 center = platformIO.Monitors[0].MainSize;
		center.x = center.x * 0.5f;
		center.y = center.y * 0.5f;
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(200, 75));
			
		bool popen = true;
		if (ImGui::BeginPopupModal(messageBoxTitle, &popen, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text(messageBoxText);
			if (ImGui::Button("  OK  "))
			{
				STRFREE(messageBoxTitle);
				STRFREE(messageBoxText);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		ImGui::PopStyleVar();
	}
	
	// check UI tasks
	uiThreadTasksMutex->Lock();
	while(!uiThreadTasks.empty())
	{
		CUiThreadTaskCallback *taskCallback = uiThreadTasks.front();
		uiThreadTasks.pop_front();
		taskCallback->RunUIThreadTask();
	}
	uiThreadTasksMutex->Unlock();
}

void CGuiMain::StoreLayoutInSettingsAtEndOfThisFrame()
{
	layoutStoreCurrentInSettings = true;
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

//

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

CUiThreadTaskCallback::CUiThreadTaskCallback()
{
	uiThreadTaskUserData = NULL;
}

void CUiThreadTaskCallback::RunUIThreadTask()
{
}

void CUiThreadTaskSetView::RunUIThreadTask()
{
	LOGD("CUiThreadTaskSetView::RunUIThreadTask");
	
	// TODO: FIX ME  guiMain->SetViewAsync(this->view);
}

CUiThreadTaskSetMouseCursorVisible::CUiThreadTaskSetMouseCursorVisible(bool mouseCursorVisible)
{
	this->mouseCursorVisible = mouseCursorVisible;
}
	
void CUiThreadTaskSetMouseCursorVisible::RunUIThreadTask()
{
//	LOGD(" CUiThreadTaskSetMouseCursorVisible::RunUIThreadTask");
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

CUiThreadTaskSetFullScreen::CUiThreadTaskSetFullScreen(bool isFullScreen)
{
	this->isFullScreen = isFullScreen;
}
	
void CUiThreadTaskSetFullScreen::RunUIThreadTask()
{
	// TODO: VID_SetWindowFullScreen(isFullScreen);
}

CUiThreadTaskSetAlwaysOnTop::CUiThreadTaskSetAlwaysOnTop(bool isAlwaysOnTop)
{
	this->isAlwaysOnTop = isAlwaysOnTop;
}
	
void CUiThreadTaskSetAlwaysOnTop::RunUIThreadTask()
{
	VID_SetWindowAlwaysOnTop(isAlwaysOnTop);
}

void CGuiMain::SetFocus(CGuiElement *element)
{
	LOGTODO("CGuiMain::SetFocus NOT IMPLEMENTED");
}

void CGuiMain::SetMouseCursorVisible(bool isVisible)
{
//	LOGTODO("CGuiMain::SetMouseCursorVisible NOT IMPLEMENTED");
}

void CGuiMain::SetApplicationWindowFullScreen(bool isFullScreen)
{
	LOGTODO("CGuiMain::SetApplicationWindowFullScreen NOT IMPLEMENTED");
}

void CGuiMain::SetApplicationWindowAlwaysOnTop(bool alwaysOnTop)
{
	LOGTODO("CGuiMain::SetApplicationWindowAlwaysOnTop NOT IMPLEMENTED");
}

void CGuiMain::RemoveAllViews()
{
	LOGTODO("CGuiMain::RemoveAllViews NOT IMPLEMENTED");
}
