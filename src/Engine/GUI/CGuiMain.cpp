#include "CGuiMain.h"
#include "CGuiView.h"
#include "CConfigStorage.h"
#include "SYS_Threading.h"
#include "SYS_DefaultConfig.h"
#include "CGlobalKeyboardCallback.h"
#include "CGlobalLogicCallback.h"
#include "CGlobalLayoutCallback.h"
#include "CGlobalOSWindowChangedCallback.h"
#include "CGlobalDropFileCallback.h"
#include "RES_ResourceManager.h"
#include "CSlrString.h"
#include "CLayoutManager.h"
#include "GAM_GamePads.h"
#include "CSlrKeyboardShortcuts.h"
#include "CGuiViewUiDebug.h"
#include "CGuiViewDebugLog.h"
#include "imgui_notify.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

ImGuiDockNodeFlags GetAutoLayoutDockedPreserveScanLeafFlags(ImGuiDockNodeFlags flags, EAutoLayoutDockedPreserveScanTabBarMode tabBarMode)
{
	// Rebuilt preserve-scan leaves are not explicit dockspaces. Carrying this
	// root-only flag makes ImGui expect a HostWindow on child leaf nodes.
	flags &= ~ImGuiDockNodeFlags_DockSpace;

	if (tabBarMode == AutoLayoutDockedPreserveScanTabBarMode_NoTabBar)
	{
		flags |= ImGuiDockNodeFlags_NoTabBar;
		flags &= ~ImGuiDockNodeFlags_HiddenTabBar;
	}
	else if (tabBarMode == AutoLayoutDockedPreserveScanTabBarMode_TabBar)
	{
		flags &= ~ImGuiDockNodeFlags_NoTabBar;
		flags &= ~ImGuiDockNodeFlags_HiddenTabBar;
	}
	return flags;
}

namespace
{
	static std::string DebugDockPathForNode(ImGuiDockNode *node)
	{
		if (!node)
			return "missing";

		std::string path;
		ImGuiDockNode *current = node;
		while (current)
		{
			if (current->ParentNode)
			{
				const char *axis = "N";
				if (current->ParentNode->SplitAxis == ImGuiAxis_X)
					axis = "X";
				else if (current->ParentNode->SplitAxis == ImGuiAxis_Y)
					axis = "Y";

				int branch = -1;
				if (current->ParentNode->ChildNodes[0] == current)
					branch = 0;
				else if (current->ParentNode->ChildNodes[1] == current)
					branch = 1;

				char segment[32];
				snprintf(segment, sizeof(segment), "%s%d", axis, branch);
				if (path.empty())
					path = segment;
				else
					path = std::string(segment) + "/" + path;
			}
			current = current->ParentNode;
		}

		if (path.empty())
			path = "root";

		ImGuiDockNode *root = node;
		while (root->ParentNode)
			root = root->ParentNode;

		return std::string(root->IsFloatingNode() ? "floating:" : "main:") + path;
	}

	static std::string DebugDockPathForId(ImGuiID dockId)
	{
		if (dockId == 0)
			return "dockid=0";
		return DebugDockPathForNode(ImGui::DockBuilderGetNode(dockId));
	}

	static void DebugMarkDockTreeInvisible(ImGuiDockNode *node)
	{
		if (!node)
			return;
		node->IsVisible = false;
		if (node->ChildNodes[0])
			DebugMarkDockTreeInvisible(node->ChildNodes[0]);
		if (node->ChildNodes[1])
			DebugMarkDockTreeInvisible(node->ChildNodes[1]);
	}

	struct SAutoLayoutRect
	{
		float x;
		float y;
		float w;
		float h;
	};

	static inline bool RectContains(const SAutoLayoutRect &a, const SAutoLayoutRect &b)
	{
		return (b.x >= a.x && b.y >= a.y
				&& (b.x + b.w) <= (a.x + a.w)
				&& (b.y + b.h) <= (a.y + a.h));
	}

	static inline bool RectIsValid(const SAutoLayoutRect &r)
	{
		return (r.w > 0.0f && r.h > 0.0f);
	}

	static inline float RectArea(const SAutoLayoutRect &r)
	{
		return (r.w > 0.0f && r.h > 0.0f) ? (r.w * r.h) : 0.0f;
	}

	static inline bool RectIntersect(const SAutoLayoutRect &a, const SAutoLayoutRect &b, SAutoLayoutRect &out)
	{
		const float ax1 = a.x;
		const float ay1 = a.y;
		const float ax2 = a.x + a.w;
		const float ay2 = a.y + a.h;

		const float bx1 = b.x;
		const float by1 = b.y;
		const float bx2 = b.x + b.w;
		const float by2 = b.y + b.h;

		const float ix1 = std::max(ax1, bx1);
		const float iy1 = std::max(ay1, by1);
		const float ix2 = std::min(ax2, bx2);
		const float iy2 = std::min(ay2, by2);

		out.x = ix1;
		out.y = iy1;
		out.w = ix2 - ix1;
		out.h = iy2 - iy1;
		return RectIsValid(out);
	}

	static inline SAutoLayoutRect RectClampTo(const SAutoLayoutRect &r, const SAutoLayoutRect &bounds)
	{
		SAutoLayoutRect out = r;
		const float x1 = std::max(bounds.x, out.x);
		const float y1 = std::max(bounds.y, out.y);
		const float x2 = std::min(bounds.x + bounds.w, out.x + out.w);
		const float y2 = std::min(bounds.y + bounds.h, out.y + out.h);
		out.x = x1;
		out.y = y1;
		out.w = x2 - x1;
		out.h = y2 - y1;
		if (!RectIsValid(out))
			return {0.0f, 0.0f, 0.0f, 0.0f};
		return out;
	}

	static inline SAutoLayoutRect RectShrink(const SAutoLayoutRect &r, float eps)
	{
		if (eps <= 0.0f)
			return r;
		const float e = std::max(0.0f, eps);
		SAutoLayoutRect out;
		out.x = r.x + e;
		out.y = r.y + e;
		out.w = r.w - e * 2.0f;
		out.h = r.h - e * 2.0f;
		if (!RectIsValid(out))
			return {0.0f, 0.0f, 0.0f, 0.0f};
		return out;
	}

	static void RectSubtract(const SAutoLayoutRect &src, const SAutoLayoutRect &cut, std::vector<SAutoLayoutRect> &out)
	{
		SAutoLayoutRect inter;
		if (!RectIntersect(src, cut, inter))
		{
			out.push_back(src);
			return;
		}

		const float sx1 = src.x;
		const float sy1 = src.y;
		const float sx2 = src.x + src.w;
		const float sy2 = src.y + src.h;
		const float ix1 = inter.x;
		const float iy1 = inter.y;
		const float ix2 = inter.x + inter.w;
		const float iy2 = inter.y + inter.h;

		// Top
		if (iy1 > sy1)
			out.push_back({sx1, sy1, src.w, iy1 - sy1});
		// Bottom
		if (iy2 < sy2)
			out.push_back({sx1, iy2, src.w, sy2 - iy2});
		// Left
		if (ix1 > sx1)
			out.push_back({sx1, iy1, ix1 - sx1, inter.h});
		// Right
		if (ix2 < sx2)
			out.push_back({ix2, iy1, sx2 - ix2, inter.h});
	}

	static void PruneContainedFreeRects(std::vector<SAutoLayoutRect> &freeRects)
	{
		for (size_t i = 0; i < freeRects.size(); i++)
		{
			bool removedI = false;
			for (size_t j = i + 1; j < freeRects.size(); j++)
			{
				if (RectContains(freeRects[i], freeRects[j]))
				{
					freeRects.erase(freeRects.begin() + j);
					j--;
					continue;
				}
				if (RectContains(freeRects[j], freeRects[i]))
				{
					freeRects.erase(freeRects.begin() + i);
					removedI = true;
					break;
				}
			}
			if (removedI)
			{
				i--;
				continue;
			}
		}
	}

	// Simple guillotine packer. Places rectangles top-left in chosen free rect.
	static bool PackGuillotine(const std::vector<SAutoLayoutRect> &rects, float containerW, float containerH, bool splitHorizontalFirst,
						std::vector<SAutoLayoutRect> &outPlacements)
	{
		outPlacements.clear();
		outPlacements.resize(rects.size());

		std::vector<SAutoLayoutRect> freeRects;
		freeRects.reserve(rects.size() * 2 + 1);
		freeRects.push_back({0.0f, 0.0f, containerW, containerH});

		for (size_t i = 0; i < rects.size(); i++)
		{
			const float rw = rects[i].w;
			const float rh = rects[i].h;

			int best = -1;
			float bestScore = FLT_MAX;
			float bestShortSide = FLT_MAX;

			for (int f = 0; f < (int)freeRects.size(); f++)
			{
				const SAutoLayoutRect &fr = freeRects[f];
				if (rw > fr.w || rh > fr.h)
					continue;

				const float freeArea = fr.w * fr.h;
				const float rectArea = rw * rh;
				const float score = freeArea - rectArea;
				const float shortSide = std::min(fr.w - rw, fr.h - rh);

				if (score < bestScore || (score == bestScore && shortSide < bestShortSide))
				{
					bestScore = score;
					bestShortSide = shortSide;
					best = f;
				}
			}

			if (best < 0)
				return false;

			SAutoLayoutRect fr = freeRects[best];
			outPlacements[i] = {fr.x, fr.y, rw, rh};

			freeRects.erase(freeRects.begin() + best);

			SAutoLayoutRect right = {0};
			SAutoLayoutRect bottom = {0};
			if (splitHorizontalFirst)
			{
				// Prefer a large bottom area.
				right = {fr.x + rw, fr.y, fr.w - rw, rh};
				bottom = {fr.x, fr.y + rh, fr.w, fr.h - rh};
			}
			else
			{
				// Prefer a large right area.
				right = {fr.x + rw, fr.y, fr.w - rw, fr.h};
				bottom = {fr.x, fr.y + rh, rw, fr.h - rh};
			}

			if (right.w > 0.0f && right.h > 0.0f)
				freeRects.push_back(right);
			if (bottom.w > 0.0f && bottom.h > 0.0f)
				freeRects.push_back(bottom);

			PruneContainedFreeRects(freeRects);
		}

		return true;
	}

	static inline float ClampF(float v, float lo, float hi);

	struct SAutoLayoutFreeDockRect
	{
		SAutoLayoutRect r;
		ImGuiID nodeId;
	};

	static void PruneContainedFreeDockRects(std::vector<SAutoLayoutFreeDockRect> &freeRects)
	{
		for (size_t i = 0; i < freeRects.size(); i++)
		{
			bool removedI = false;
			for (size_t j = i + 1; j < freeRects.size(); j++)
			{
				if (RectContains(freeRects[i].r, freeRects[j].r))
				{
					freeRects.erase(freeRects.begin() + j);
					j--;
					continue;
				}
				if (RectContains(freeRects[j].r, freeRects[i].r))
				{
					freeRects.erase(freeRects.begin() + i);
					removedI = true;
					break;
				}
			}
			if (removedI)
			{
				i--;
				continue;
			}
		}
	}

	// Same guillotine strategy as PackGuillotine(), but also builds a matching dock-node split tree.
	// Returns leaf node IDs for each rectangle (in rects order).
	static bool PackGuillotineDockNodes(const std::vector<SAutoLayoutRect> &rects, float containerW, float containerH, bool splitHorizontalFirst,
										 ImGuiID rootNodeId, std::vector<ImGuiID> &outLeafNodeIds)
	{
		outLeafNodeIds.clear();
		outLeafNodeIds.resize(rects.size(), 0);

		std::vector<SAutoLayoutFreeDockRect> freeRects;
		freeRects.reserve(rects.size() * 2 + 1);
		freeRects.push_back({{0.0f, 0.0f, containerW, containerH}, rootNodeId});

		for (size_t i = 0; i < rects.size(); i++)
		{
			const float rw = rects[i].w;
			const float rh = rects[i].h;

			int best = -1;
			float bestScore = FLT_MAX;
			float bestShortSide = FLT_MAX;

			for (int f = 0; f < (int)freeRects.size(); f++)
			{
				const SAutoLayoutRect &fr = freeRects[f].r;
				if (rw > fr.w || rh > fr.h)
					continue;

				const float freeArea = fr.w * fr.h;
				const float rectArea = rw * rh;
				const float score = freeArea - rectArea;
				const float shortSide = std::min(fr.w - rw, fr.h - rh);

				if (score < bestScore || (score == bestScore && shortSide < bestShortSide))
				{
					bestScore = score;
					bestShortSide = shortSide;
					best = f;
				}
			}

			if (best < 0)
				return false;

			SAutoLayoutFreeDockRect fr = freeRects[best];
			freeRects.erase(freeRects.begin() + best);

			ImGuiID placedNodeId = fr.nodeId;
			ImGuiID rightNodeId = 0;
			ImGuiID bottomNodeId = 0;

			if (splitHorizontalFirst)
			{
				// Horizontal split (top/bottom) first, then vertical split inside the top region.
				ImGuiID topNodeId = fr.nodeId;
				if (rh < fr.r.h)
				{
					ImGuiID outUp = 0;
					ImGuiID outDown = 0;
					const float ratioH = ClampF(rh / fr.r.h, 0.0f, 1.0f);
					ImGui::DockBuilderSplitNode(fr.nodeId, ImGuiDir_Up, ratioH, &outUp, &outDown);
					topNodeId = outUp;
					bottomNodeId = outDown;
				}

				placedNodeId = topNodeId;
				if (rw < fr.r.w)
				{
					ImGuiID outLeft = 0;
					ImGuiID outRight = 0;
					const float ratioW = ClampF(rw / fr.r.w, 0.0f, 1.0f);
					ImGui::DockBuilderSplitNode(topNodeId, ImGuiDir_Left, ratioW, &outLeft, &outRight);
					placedNodeId = outLeft;
					rightNodeId = outRight;
				}
			}
			else
			{
				// Vertical split (left/right) first, then horizontal split inside the left region.
				ImGuiID leftNodeId = fr.nodeId;
				if (rw < fr.r.w)
				{
					ImGuiID outLeft = 0;
					ImGuiID outRight = 0;
					const float ratioW = ClampF(rw / fr.r.w, 0.0f, 1.0f);
					ImGui::DockBuilderSplitNode(fr.nodeId, ImGuiDir_Left, ratioW, &outLeft, &outRight);
					leftNodeId = outLeft;
					rightNodeId = outRight;
				}

				placedNodeId = leftNodeId;
				if (rh < fr.r.h)
				{
					ImGuiID outUp = 0;
					ImGuiID outDown = 0;
					const float ratioH = ClampF(rh / fr.r.h, 0.0f, 1.0f);
					ImGui::DockBuilderSplitNode(leftNodeId, ImGuiDir_Up, ratioH, &outUp, &outDown);
					placedNodeId = outUp;
					bottomNodeId = outDown;
				}
			}

			outLeafNodeIds[i] = placedNodeId;

			SAutoLayoutRect right = {0};
			SAutoLayoutRect bottom = {0};
			if (splitHorizontalFirst)
			{
				right = {fr.r.x + rw, fr.r.y, fr.r.w - rw, rh};
				bottom = {fr.r.x, fr.r.y + rh, fr.r.w, fr.r.h - rh};
			}
			else
			{
				right = {fr.r.x + rw, fr.r.y, fr.r.w - rw, fr.r.h};
				bottom = {fr.r.x, fr.r.y + rh, rw, fr.r.h - rh};
			}

			if (right.w > 0.0f && right.h > 0.0f && rightNodeId != 0)
				freeRects.push_back({right, rightNodeId});
			if (bottom.w > 0.0f && bottom.h > 0.0f && bottomNodeId != 0)
				freeRects.push_back({bottom, bottomNodeId});

			PruneContainedFreeDockRects(freeRects);
		}

		return true;
	}

	static inline float ClampF(float v, float lo, float hi)
	{
		if (v < lo)
			return lo;
		if (v > hi)
			return hi;
		return v;
	}
}

#define CONSOLE_FONT_SIZE_X		0.03125
#define CONSOLE_FONT_SIZE_Y		0.03125
#define CONSOLE_FONT_PITCH_X	0.035156251
#define CONSOLE_FONT_PITCH_Y	0.035156251

#define CREATE_MTENGINESDL_DEBUG_VIEW

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
	layoutStoreAfterFrameDelay = 0;
	autoLayoutRequested = false;
	autoLayoutRequestedMode = AutoLayoutMode_Preserve;
	autoLayoutDockedPreserveScanTabBarMode = AutoLayoutDockedPreserveScanTabBarMode_Default;
	autoLayoutSettingsWindowVisible = false;
	ResetAutoLayoutSettingsToDefaults();
	LoadAutoLayoutSettingsFromConfig();

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
	
#ifdef CREATE_MTENGINESDL_DEBUG_VIEW
	// this is MTEngineSDL debug view that can be added by user via main menu, mainly to debug multiple monitor DPI-issues
	viewUiDebug = new CGuiViewUiDebug(100, 100, -1, 500, 300);
	viewUiDebug->visible = false;
#else
	viewUiDebug = NULL;
#endif
	
	// this is MTEngineSDL debug log
#if !defined(GLOBAL_DEBUG_OFF)
	AddView(guiViewDebugLog);
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
	
	// Notify before any view deserializes. This lets app code reset globals
	// that views would otherwise leak across workspaces.
	for (std::list<CGlobalLayoutCallback *>::iterator it = globalLayoutCallbacks.begin();
		 it != globalLayoutCallbacks.end(); it++)
	{
		(*it)->GlobalLayoutWillDeserialize(layout);
	}

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

bool CGuiMain::FocusTraverseVisibleViews(bool reverse)
{
	ImGuiContext *context = ImGui::GetCurrentContext();
	if (!context)
		return false;

	std::vector<CGuiView *> candidates;
	candidates.reserve(this->views.size() + 1);

	auto addCandidate = [&candidates](CGuiView *view)
	{
		if (!view)
			return;
		if (!view->visible)
			return;
		if (!view->imGuiWindow)
			return;
		if (!view->imGuiWindow->WasActive || view->imGuiWindow->Hidden)
			return;
		if ((view->imGuiWindow->Flags & (ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoNavInputs)) == (ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoNavInputs))
			return;
		for (CGuiView *v : candidates)
			if (v == view)
				return;
		candidates.push_back(view);
	};

	addCandidate(this->currentView);
	for (auto it = this->views.begin(); it != this->views.end(); it++)
		addCandidate(*it);

	if (candidates.empty())
		return false;

	CGuiView *current = this->focusedView;
	if (!current && context->NavWindow)
		current = (CGuiView *)context->NavWindow->userData;

	if (!current)
	{
		SetFocus(candidates.front());
		return true;
	}

	int currentIndex = -1;
	for (int i = 0; i < (int)candidates.size(); i++)
	{
		if (candidates[i] == current)
		{
			currentIndex = i;
			break;
		}
	}

	if (currentIndex < 0)
	{
		SetFocus(candidates.front());
		return true;
	}

	if (candidates.size() < 2)
		return true;

	int nextIndex;
	if (reverse)
		nextIndex = (currentIndex - 1 + (int)candidates.size()) % (int)candidates.size();
	else
		nextIndex = (currentIndex + 1) % (int)candidates.size();

	CGuiView *next = candidates[nextIndex];
	if (next)
		SetFocus(next);
	return true;
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

	// View focus traversal: Cmd+` (macOS) / Ctrl+` (Windows/Linux)
	// Shift+Cmd+` / Shift+Ctrl+` traverses in reverse.
	if (isControlPressed && !isAltPressed && !isSuperPressed)
	{
		u32 keyCodeBare = SYS_GetBareKey(keyCode, isShiftPressed, isAltPressed, isControlPressed, isSuperPressed);
		if (keyCodeBare == '`')
		{
			bool reverse = isShiftPressed;
			FocusTraverseVisibleViews(reverse);
			return true;
		}
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

void CGuiMain::RequestAutoLayoutVisibleViews()
{
	autoLayoutRequestedMode = AutoLayoutMode_Preserve;
	autoLayoutRequested = true;
}

void CGuiMain::RequestAutoLayoutVisibleViewsCompact()
{
	autoLayoutRequestedMode = AutoLayoutMode_Compact;
	autoLayoutRequested = true;
}

void CGuiMain::RequestAutoLayoutVisibleViewsDocked()
{
	autoLayoutRequestedMode = AutoLayoutMode_Docked;
	autoLayoutRequested = true;
}

void CGuiMain::RequestAutoLayoutVisibleViewsDockedPreserveScan()
{
	RequestAutoLayoutVisibleViewsDockedPreserveScan(AutoLayoutDockedPreserveScanTabBarMode_Default);
}

void CGuiMain::RequestAutoLayoutVisibleViewsDockedPreserveScan(EAutoLayoutDockedPreserveScanTabBarMode tabBarMode)
{
	autoLayoutRequestedMode = AutoLayoutMode_DockedPreserveScan;
	autoLayoutDockedPreserveScanTabBarMode = tabBarMode;
	autoLayoutRequested = true;
}

void CGuiMain::OpenAutoLayoutSettingsWindow()
{
	autoLayoutSettingsWindowVisible = true;
}

void CGuiMain::RenderDockSpacesOverViewports()
{
	dockSpaceIdByViewportId.clear();
	ImGuiContext *context = ImGui::GetCurrentContext();
	if (!context)
		return;

	ImGuiPlatformIO &platformIO = ImGui::GetPlatformIO();
	for (int i = 0; i < platformIO.Viewports.Size; i++)
	{
		ImGuiViewport *viewport = platformIO.Viewports[i];
		if (!viewport)
			continue;
		const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, viewport);
		dockSpaceIdByViewportId[viewport->ID] = dockspaceId;
	}
}

void CGuiMain::ResetAutoLayoutSettingsToDefaults()
{
	autoLayoutMargin = 8.0f;
	autoLayoutGap = 8.0f;
	autoLayoutMinWindowSize = 24.0f;
	autoLayoutShrinkExpMin = 0.85f;
	autoLayoutShrinkExpMax = 1.25f;
}

void CGuiMain::LoadAutoLayoutSettingsFromConfig()
{
	if (!gApplicationDefaultConfig)
		return;

	gApplicationDefaultConfig->GetFloat("uiAutoLayoutMargin", &autoLayoutMargin, autoLayoutMargin);
	gApplicationDefaultConfig->GetFloat("uiAutoLayoutGap", &autoLayoutGap, autoLayoutGap);
	gApplicationDefaultConfig->GetFloat("uiAutoLayoutMinWindowSize", &autoLayoutMinWindowSize, autoLayoutMinWindowSize);
	gApplicationDefaultConfig->GetFloat("uiAutoLayoutShrinkExpMin", &autoLayoutShrinkExpMin, autoLayoutShrinkExpMin);
	gApplicationDefaultConfig->GetFloat("uiAutoLayoutShrinkExpMax", &autoLayoutShrinkExpMax, autoLayoutShrinkExpMax);

	autoLayoutMargin = ClampF(autoLayoutMargin, 0.0f, 256.0f);
	autoLayoutGap = ClampF(autoLayoutGap, 0.0f, 256.0f);
	autoLayoutMinWindowSize = ClampF(autoLayoutMinWindowSize, 8.0f, 2048.0f);
	autoLayoutShrinkExpMin = ClampF(autoLayoutShrinkExpMin, 0.10f, 3.0f);
	autoLayoutShrinkExpMax = ClampF(autoLayoutShrinkExpMax, 0.10f, 3.0f);
	if (autoLayoutShrinkExpMax < autoLayoutShrinkExpMin)
		autoLayoutShrinkExpMax = autoLayoutShrinkExpMin;
}

bool CGuiMain::SaveAutoLayoutSettingsToConfig()
{
	if (!gApplicationDefaultConfig)
		return false;

	autoLayoutMargin = ClampF(autoLayoutMargin, 0.0f, 256.0f);
	autoLayoutGap = ClampF(autoLayoutGap, 0.0f, 256.0f);
	autoLayoutMinWindowSize = ClampF(autoLayoutMinWindowSize, 8.0f, 2048.0f);
	autoLayoutShrinkExpMin = ClampF(autoLayoutShrinkExpMin, 0.10f, 3.0f);
	autoLayoutShrinkExpMax = ClampF(autoLayoutShrinkExpMax, 0.10f, 3.0f);
	if (autoLayoutShrinkExpMax < autoLayoutShrinkExpMin)
		autoLayoutShrinkExpMax = autoLayoutShrinkExpMin;

	gApplicationDefaultConfig->SetFloatSkipConfigSave("uiAutoLayoutMargin", &autoLayoutMargin);
	gApplicationDefaultConfig->SetFloatSkipConfigSave("uiAutoLayoutGap", &autoLayoutGap);
	gApplicationDefaultConfig->SetFloatSkipConfigSave("uiAutoLayoutMinWindowSize", &autoLayoutMinWindowSize);
	gApplicationDefaultConfig->SetFloatSkipConfigSave("uiAutoLayoutShrinkExpMin", &autoLayoutShrinkExpMin);
	gApplicationDefaultConfig->SetFloatSkipConfigSave("uiAutoLayoutShrinkExpMax", &autoLayoutShrinkExpMax);

	return gApplicationDefaultConfig->SaveConfig();
}

void CGuiMain::RunAutoLayoutIfRequested()
{
	if (!autoLayoutRequested)
		return;
	const EAutoLayoutMode requestMode = autoLayoutRequestedMode;
	const EAutoLayoutDockedPreserveScanTabBarMode preserveScanTabBarMode = autoLayoutDockedPreserveScanTabBarMode;
	// Consume request.
	autoLayoutRequested = false;
	autoLayoutDockedPreserveScanTabBarMode = AutoLayoutDockedPreserveScanTabBarMode_Default;

	ImGuiContext *context = ImGui::GetCurrentContext();
	if (!context)
		return;

	// Layout parameters (persisted in application default config).
	const float margin = ClampF(autoLayoutMargin, 0.0f, 256.0f);
	const float gap = ClampF(autoLayoutGap, 0.0f, 256.0f);
	const float minSize = ClampF(autoLayoutMinWindowSize, 8.0f, 2048.0f);
	const float expMin = ClampF(autoLayoutShrinkExpMin, 0.10f, 3.0f);
	const float expMax = ClampF(autoLayoutShrinkExpMax, expMin, 3.0f);

	auto enumVisibleViews = [&](auto &&fn)
	{
		fn(this->currentView);
		for (auto it = this->views.begin(); it != this->views.end(); it++)
			fn(*it);
	};

	auto getDockspaceIdForViewport = [&](ImGuiViewport *viewport) -> ImGuiID
	{
		if (!viewport)
			return 0;
		auto it = dockSpaceIdByViewportId.find(viewport->ID);
		if (it != dockSpaceIdByViewportId.end())
			return it->second;
		// Fallback: create it on demand.
		const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, viewport);
		dockSpaceIdByViewportId[viewport->ID] = dockspaceId;
		return dockspaceId;
	};

	struct SInactiveDockNode
	{
		bool isLeaf = false;
		ImGuiID originalDockId = 0;
		ImGuiAxis splitAxis = ImGuiAxis_None;
		float ratio = 0.5f;
		int leafCount = 0;
		int child[2] = {-1, -1};
	};

	struct SInactiveDockTree
	{
		std::vector<SInactiveDockNode> nodes;
		int rootIndex = -1;
	};

	auto appendInactiveInternalNode = [](ImGuiDockNode *node, int c0, int c1,
									 std::vector<SInactiveDockNode> &nodes) -> int
	{
		if (!node || (c0 < 0 && c1 < 0))
			return -1;

		SInactiveDockNode n;
		n.isLeaf = false;
		n.originalDockId = node->ID;
		n.splitAxis = node->SplitAxis;
		n.child[0] = c0;
		n.child[1] = c1;
		n.leafCount = (c0 >= 0 ? nodes[c0].leafCount : 0) + (c1 >= 0 ? nodes[c1].leafCount : 0);

		float s0 = (node->SplitAxis == ImGuiAxis_X)
			? (node->ChildNodes[0] ? node->ChildNodes[0]->SizeRef.x : 1.0f)
			: (node->ChildNodes[0] ? node->ChildNodes[0]->SizeRef.y : 1.0f);
		float s1 = (node->SplitAxis == ImGuiAxis_X)
			? (node->ChildNodes[1] ? node->ChildNodes[1]->SizeRef.x : 1.0f)
			: (node->ChildNodes[1] ? node->ChildNodes[1]->SizeRef.y : 1.0f);
		float denom = s0 + s1;
		n.ratio = (denom > 0.001f) ? (s0 / denom) : 0.5f;

		nodes.push_back(n);
		return (int)nodes.size() - 1;
	};

	auto getDockRootId = [](ImGuiID dockId) -> ImGuiID
	{
		ImGuiDockNode *node = ImGui::DockBuilderGetNode(dockId);
		if (!node)
			return 0;
		while (node->ParentNode)
			node = node->ParentNode;
		return node->ID;
	};

	auto isDockNodeInSubtree = [](ImGuiID dockId, ImGuiID subtreeRootId) -> bool
	{
		if (dockId == 0 || subtreeRootId == 0)
			return false;
		ImGuiDockNode *node = ImGui::DockBuilderGetNode(dockId);
		while (node)
		{
			if (node->ID == subtreeRootId)
				return true;
			node = node->ParentNode;
		}
		return false;
	};

	auto captureDockSubtree = [&](auto &&self, ImGuiDockNode *node,
							   const std::unordered_set<ImGuiID> &inactiveDockIds,
							   std::vector<SInactiveDockNode> &nodes) -> int
	{
		if (!node)
			return -1;

		if (node->ChildNodes[0] || node->ChildNodes[1])
		{
			int c0 = self(self, node->ChildNodes[0], inactiveDockIds, nodes);
			int c1 = self(self, node->ChildNodes[1], inactiveDockIds, nodes);
			return appendInactiveInternalNode(node, c0, c1, nodes);
		}

		if (inactiveDockIds.count(node->ID) == 0)
			return -1;

		SInactiveDockNode n;
		n.isLeaf = true;
		n.originalDockId = node->ID;
		n.leafCount = 1;
		nodes.push_back(n);
		return (int)nodes.size() - 1;
	};

	auto captureInactiveTrees = [&](ImGuiContext *ctx, ImGuiID dockspaceId) -> std::vector<SInactiveDockTree>
	{
		std::vector<SInactiveDockTree> trees;

		std::unordered_set<ImGuiID> inactiveDockIds;
		for (int wi = 0; wi < ctx->Windows.Size; wi++)
		{
			ImGuiWindow *w = ctx->Windows[wi];
			if (!w || w->DockId == 0 || w->WasActive)
				continue;
			if (getDockRootId(w->DockId) != dockspaceId)
				continue;
			inactiveDockIds.insert(w->DockId);
		}
		if (inactiveDockIds.empty())
			return trees;

		ImGuiDockNode *dockRoot = ImGui::DockBuilderGetNode(dockspaceId);
		if (!dockRoot)
			return trees;

		if (!dockRoot->ChildNodes[0] && !dockRoot->ChildNodes[1])
		{
			if (inactiveDockIds.count(dockRoot->ID))
			{
				SInactiveDockTree tree;
				SInactiveDockNode n;
				n.isLeaf = true;
				n.originalDockId = dockRoot->ID;
				n.leafCount = 1;
				tree.nodes.push_back(n);
				tree.rootIndex = 0;
				trees.push_back(std::move(tree));
			}
			return trees;
		}

		SInactiveDockTree tree;
		int root = captureDockSubtree(captureDockSubtree, dockRoot, inactiveDockIds, tree.nodes);
		if (root >= 0)
		{
			tree.rootIndex = root;
			trees.push_back(std::move(tree));
		}

		return trees;
	};

	auto rebuildInactiveSubtree = [](auto &&self, const SInactiveDockTree &tree,
								  int nodeIdx, ImGuiID dockId,
								  std::vector<std::pair<int, ImGuiID>> &outLeafMap) -> void
	{
		if (nodeIdx < 0 || nodeIdx >= (int)tree.nodes.size())
			return;
		const SInactiveDockNode &node = tree.nodes[nodeIdx];

		if (node.isLeaf)
		{
			outLeafMap.push_back({nodeIdx, dockId});
			return;
		}

		ImGuiID child0Id = 0;
		ImGuiID child1Id = 0;
		ImGuiDir dir = (node.splitAxis == ImGuiAxis_X) ? ImGuiDir_Left : ImGuiDir_Up;
		float ratio = ClampF(node.ratio, 0.05f, 0.95f);
		ImGui::DockBuilderSplitNode(dockId, dir, ratio, &child0Id, &child1Id);

		if (node.child[0] >= 0)
			self(self, tree, node.child[0], child0Id, outLeafMap);
		if (node.child[1] >= 0)
			self(self, tree, node.child[1], child1Id, outLeafMap);
	};

	auto rebuildAllInactiveTrees = [&](const std::vector<SInactiveDockTree> &trees,
								  ImGuiID inactiveRootId,
								  const std::vector<std::pair<ImGuiWindow*, ImGuiID>> &windowDockIds)
		-> std::unordered_map<ImGuiWindow*, ImGuiID>
	{
		std::unordered_map<ImGuiWindow*, ImGuiID> windowToNewDockId;
		if (trees.empty())
			return windowToNewDockId;

		std::vector<ImGuiID> treeRoots;
		if (trees.size() == 1)
		{
			treeRoots.push_back(inactiveRootId);
		}
		else
		{
			ImGuiID remaining = inactiveRootId;
			for (size_t i = 0; i < trees.size() - 1; i++)
			{
				ImGuiID thisTree = 0;
				ImGuiID rest = 0;
				float ratio = 1.0f / (float)(trees.size() - i);
				ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Up, ratio, &thisTree, &rest);
				treeRoots.push_back(thisTree);
				remaining = rest;
			}
			treeRoots.push_back(remaining);
		}

		for (size_t ti = 0; ti < trees.size(); ti++)
		{
			const SInactiveDockTree &tree = trees[ti];
			if (tree.rootIndex < 0)
				continue;

			std::vector<std::pair<int, ImGuiID>> leafMap;
			rebuildInactiveSubtree(rebuildInactiveSubtree, tree, tree.rootIndex, treeRoots[ti], leafMap);

			std::unordered_map<ImGuiID, ImGuiID> originalToNew;
			for (const auto &lm : leafMap)
			{
				int leafIdx = lm.first;
				ImGuiID newDockId = lm.second;
				if (leafIdx >= 0 && leafIdx < (int)tree.nodes.size())
				{
					ImGuiID origId = tree.nodes[leafIdx].originalDockId;
					if (origId != 0)
						originalToNew[origId] = newDockId;
				}
			}

			for (const auto &wd : windowDockIds)
			{
				auto it = originalToNew.find(wd.second);
				if (it != originalToNew.end())
					windowToNewDockId[wd.first] = it->second;
			}
		}

		return windowToNewDockId;
	};

	// Dock visible windows to the main dockspace of their current viewport.
	// In Docked modes we rebuild the dockspace split tree.
	// - Docked: packs/group-tiles windows and docks them (may move windows significantly).
	// - DockedPreserveScan: reconstructs a dock split tree from current on-screen positions and z-order.
	if (requestMode == AutoLayoutMode_DockedPreserveScan)
	{
		ImGuiPlatformIO &platformIO = ImGui::GetPlatformIO();
		bool anyDocked = false;

		struct SScanItem
		{
			CGuiView *view;
			ImGuiWindow *win;
			int z;
			ImGuiDockNodeFlags sourceDockNodeFlags;
			SAutoLayoutRect rect;      // current window rect in viewport Work coords
			SAutoLayoutRect target;    // effective rect after occlusion (largest visible chunk)
			float cx;
			float cy;
		};

		struct SScanTreeNode
		{
			bool isLeaf;
			bool splitVertical; // true: Left/Right, false: Up/Down
			int a;
			int b;
			int itemIndex; // leaf
			float ratio;   // split ratio for DockBuilderSplitNode
		};

		const float overlapTol = ClampF(gap, 2.0f, 64.0f);
		const float minVisibleDim = minSize;
		const int maxVisiblePieces = 16;
		auto getPreserveScanRect = [&](ImGuiWindow *win, ImGuiViewport *viewport) -> SAutoLayoutRect
		{
			if (preserveScanTabBarMode == AutoLayoutDockedPreserveScanTabBarMode_NoTabBar)
			{
				const float x0 = win->InnerRect.Min.x - viewport->WorkPos.x;
				const float y0 = win->InnerRect.Min.y - viewport->WorkPos.y;
				const float w = win->InnerRect.Max.x - win->InnerRect.Min.x;
				const float h = win->InnerRect.Max.y - win->InnerRect.Min.y;
				return {x0, y0, w, h};
			}

			const float x0 = win->Pos.x - viewport->WorkPos.x;
			const float y0 = win->Pos.y - viewport->WorkPos.y;
			return {x0, y0, win->Size.x, win->Size.y};
		};

		for (int vpIndex = 0; vpIndex < platformIO.Viewports.Size; vpIndex++)
		{
			ImGuiViewport *viewport = platformIO.Viewports[vpIndex];
			if (!viewport)
				continue;

			const ImGuiID dockspaceId = getDockspaceIdForViewport(viewport);
			if (dockspaceId == 0)
				continue;

			const float containerW = viewport->WorkSize.x;
			const float containerH = viewport->WorkSize.y;
			if (containerW <= 32.0f || containerH <= 32.0f)
				continue;

			// Window z-order lookup: higher index is closer to front.
			std::unordered_map<ImGuiWindow *, int> zIndex;
			zIndex.reserve((size_t)context->Windows.Size * 2);
			for (int i = 0; i < context->Windows.Size; i++)
			{
				ImGuiWindow *w = context->Windows[i];
				if (w)
					zIndex[w] = i;
			}

			std::vector<SScanItem> items;
			items.reserve(this->views.size() + 1);
			enumVisibleViews([&](CGuiView *view)
			{
				if (!view)
					return;
				if (!view->visible)
					return;
				if (this->viewFullScreen == view)
					return;
				if (!view->imGuiWindow)
					return;

				ImGuiWindow *win = view->imGuiWindow;
				if (!win || !win->Viewport)
					return;
				if (win->Viewport != viewport)
					return;
				if (!win->WasActive || win->Hidden)
					return;

				const SAutoLayoutRect scanRect = getPreserveScanRect(win, viewport);
				if (!(scanRect.w > 1.0f && scanRect.h > 1.0f))
					return;

				SScanItem it;
				it.view = view;
				it.win = win;
				auto zi = zIndex.find(win);
				it.z = (zi != zIndex.end()) ? zi->second : 0;
				it.sourceDockNodeFlags = 0;
				if (win->DockId != 0)
				{
					ImGuiDockNode *sourceNode = ImGui::DockBuilderGetNode(win->DockId);
					if (sourceNode != NULL)
						it.sourceDockNodeFlags = sourceNode->LocalFlags;
				}
				it.rect = scanRect;
				it.target = {0.0f, 0.0f, 0.0f, 0.0f};
				it.cx = scanRect.x + scanRect.w * 0.5f;
				it.cy = scanRect.y + scanRect.h * 0.5f;
				items.push_back(it);
			});

			if (items.empty())
				continue;

			std::sort(items.begin(), items.end(), [](const SScanItem &a, const SScanItem &b)
			{
				return a.z > b.z;
			});

			const SAutoLayoutRect bounds = {0.0f, 0.0f, containerW, containerH};
			std::vector<SAutoLayoutRect> occluders;
			occluders.reserve(items.size());

			std::vector<SScanItem> kept;
			kept.reserve(items.size());
			for (SScanItem &it : items)
			{
				SAutoLayoutRect r = RectClampTo(it.rect, bounds);
				if (!RectIsValid(r))
					continue;

				std::vector<SAutoLayoutRect> visible;
				visible.reserve(8);
				visible.push_back(r);

				for (const SAutoLayoutRect &occRaw : occluders)
				{
					SAutoLayoutRect occ = RectShrink(occRaw, overlapTol);
					if (!RectIsValid(occ))
						continue;
					occ = RectClampTo(occ, bounds);
					if (!RectIsValid(occ))
						continue;

					std::vector<SAutoLayoutRect> next;
					next.reserve(visible.size() * 4);
					for (const SAutoLayoutRect &vr : visible)
						RectSubtract(vr, occ, next);

					visible.clear();
					for (const SAutoLayoutRect &nr : next)
					{
						SAutoLayoutRect cl = RectClampTo(nr, bounds);
						if (RectIsValid(cl))
							visible.push_back(cl);
					}

					if (visible.empty())
						break;
					if ((int)visible.size() > maxVisiblePieces)
					{
						std::sort(visible.begin(), visible.end(), [](const SAutoLayoutRect &a, const SAutoLayoutRect &b)
						{
							return RectArea(a) > RectArea(b);
						});
						visible.resize(maxVisiblePieces);
					}
				}

				SAutoLayoutRect bestVis = {0.0f, 0.0f, 0.0f, 0.0f};
				float bestArea = 0.0f;
				float visibleArea = 0.0f;
				for (const SAutoLayoutRect &vr : visible)
				{
					const float a = RectArea(vr);
					visibleArea += a;
					if (a > bestArea)
					{
						bestArea = a;
						bestVis = vr;
					}
				}

				const float fullArea = RectArea(r);
				const bool wasOccluded = (visibleArea + 0.5f < fullArea);

				if (bestArea <= 0.0f)
				{
					if (it.view)
						it.view->SetVisible(false);
					continue;
				}
				if (wasOccluded && (bestVis.w < minVisibleDim || bestVis.h < minVisibleDim))
				{
					if (it.view)
						it.view->SetVisible(false);
					continue;
				}

				it.target = bestVis;
				it.cx = bestVis.x + bestVis.w * 0.5f;
				it.cy = bestVis.y + bestVis.h * 0.5f;
				kept.push_back(it);
				occluders.push_back(r);
			}

			if (kept.empty())
				continue;

			std::vector<SScanTreeNode> nodes;
			nodes.reserve(kept.size() * 2);

			auto buildTree = [&](auto &&self, const std::vector<int> &sub, const SAutoLayoutRect &b) -> int
			{
				if (sub.empty())
					return -1;
				if (sub.size() == 1)
				{
					SScanTreeNode n;
					n.isLeaf = true;
					n.splitVertical = true;
					n.a = -1;
					n.b = -1;
					n.itemIndex = sub[0];
					n.ratio = 0.5f;
					nodes.push_back(n);
					return (int)nodes.size() - 1;
				}

				struct SCandidate
				{
					bool vertical;
					float score;
					int k;
					float cut;
					std::vector<int> sorted;
				};

				auto bestSplit = [&](bool vertical) -> SCandidate
				{
					SCandidate best;
					best.vertical = vertical;
					best.score = -FLT_MAX;
					best.k = 1;
					best.cut = vertical ? (b.x + b.w * 0.5f) : (b.y + b.h * 0.5f);
					best.sorted = sub;
					std::sort(best.sorted.begin(), best.sorted.end(), [&](int ia, int ib)
					{
						return vertical ? (kept[ia].cx < kept[ib].cx) : (kept[ia].cy < kept[ib].cy);
					});

					const int n = (int)best.sorted.size();
					std::vector<float> prefixMax(n, 0.0f);
					std::vector<float> suffixMin(n, 0.0f);
					for (int i = 0; i < n; i++)
					{
						const SAutoLayoutRect &r = kept[best.sorted[i]].target;
						const float end = vertical ? (r.x + r.w) : (r.y + r.h);
						prefixMax[i] = (i == 0) ? end : std::max(prefixMax[i - 1], end);
					}
					for (int i = n - 1; i >= 0; i--)
					{
						const SAutoLayoutRect &r = kept[best.sorted[i]].target;
						const float start = vertical ? r.x : r.y;
						suffixMin[i] = (i == n - 1) ? start : std::min(suffixMin[i + 1], start);
					}

					int bestBalance = INT_MAX;
					for (int k = 1; k < n; k++)
					{
						const float maxEnd = prefixMax[k - 1];
						const float minStart = suffixMin[k];
						const float sep = minStart - maxEnd;
						float score = sep;
						if (sep < 0.0f)
						{
							const float overlap = -sep;
							if (overlap <= overlapTol)
								score = -overlap;
							else
								score = -(overlapTol + (overlap - overlapTol) * 10.0f);
						}

						const int balance = std::abs(k - (n - k));
						if (score > best.score + 0.0001f
							|| (fabs(score - best.score) <= 0.0001f && balance < bestBalance))
						{
							best.score = score;
							best.k = k;
							best.cut = (maxEnd + minStart) * 0.5f;
							bestBalance = balance;
						}
					}
					return best;
				};

				SCandidate candV = bestSplit(true);
				SCandidate candH = bestSplit(false);
				SCandidate best = (candH.score > candV.score + 0.0001f) ? candH : candV;
				if (fabs(candH.score - candV.score) <= 0.0001f)
				{
					// Tie-break: prefer split along the longer axis.
					if (b.w < b.h)
						best = candH;
				}

				const int n = (int)best.sorted.size();
				const int k = std::max(1, std::min(best.k, n - 1));

				std::vector<int> left(best.sorted.begin(), best.sorted.begin() + k);
				std::vector<int> right(best.sorted.begin() + k, best.sorted.end());

				SScanTreeNode node;
				node.isLeaf = false;
				node.splitVertical = best.vertical;
				node.itemIndex = -1;
				node.a = -1;
				node.b = -1;
				node.ratio = 0.5f;

				if (best.vertical)
				{
					const float denom = std::max(1.0f, b.w);
					float ratio = (best.cut - b.x) / denom;
					float minR = (minSize / denom);
					minR = ClampF(minR, 0.0f, 0.49f);
					ratio = ClampF(ratio, std::max(0.05f, minR), std::min(0.95f, 1.0f - minR));
					node.ratio = ratio;

					const float wLeft = b.w * ratio;
					SAutoLayoutRect bLeft = {b.x, b.y, wLeft, b.h};
					SAutoLayoutRect bRight = {b.x + wLeft, b.y, b.w - wLeft, b.h};
					node.a = self(self, left, bLeft);
					node.b = self(self, right, bRight);
				}
				else
				{
					const float denom = std::max(1.0f, b.h);
					float ratio = (best.cut - b.y) / denom;
					float minR = (minSize / denom);
					minR = ClampF(minR, 0.0f, 0.49f);
					ratio = ClampF(ratio, std::max(0.05f, minR), std::min(0.95f, 1.0f - minR));
					node.ratio = ratio;

					const float hUp = b.h * ratio;
					SAutoLayoutRect bUp = {b.x, b.y, b.w, hUp};
					SAutoLayoutRect bDown = {b.x, b.y + hUp, b.w, b.h - hUp};
					node.a = self(self, left, bUp);
					node.b = self(self, right, bDown);
				}

				nodes.push_back(node);
				return (int)nodes.size() - 1;
			};

			std::vector<int> rootItems;
			rootItems.reserve(kept.size());
			for (int i = 0; i < (int)kept.size(); i++)
				rootItems.push_back(i);

			const int root = buildTree(buildTree, rootItems, bounds);
			if (root < 0)
				continue;

			auto inactiveTrees = captureInactiveTrees(context, dockspaceId);
			if (inactiveTrees.size() == 1)
			{
				const SInactiveDockTree &tree = inactiveTrees[0];
				if (tree.rootIndex >= 0 && tree.rootIndex < (int)tree.nodes.size())
				{
					const SInactiveDockNode &rootNode = tree.nodes[tree.rootIndex];
					LOGD("AutoLayout preserve-scan: inactive tree root isLeaf=%d splitAxis=%d child0=%d child1=%d ratio=%f",
						 (int)rootNode.isLeaf, (int)rootNode.splitAxis, rootNode.child[0], rootNode.child[1], rootNode.ratio);
				}
			}

			bool preserveInactiveInPlace = false;
			ImGuiID preservedInactiveRootId = 0;
			ImGuiID preservedActiveRootId = 0;
			int preservedInactiveRootIndex = -1;
			ImGuiDockNode *dockRootBeforeRemove = ImGui::DockBuilderGetNode(dockspaceId);
			if (inactiveTrees.size() == 1 && dockRootBeforeRemove
				&& dockRootBeforeRemove->ChildNodes[0] && dockRootBeforeRemove->ChildNodes[1])
			{
				const SInactiveDockTree &tree = inactiveTrees[0];
				if (tree.rootIndex >= 0 && tree.rootIndex < (int)tree.nodes.size())
				{
					const SInactiveDockNode &rootNode = tree.nodes[tree.rootIndex];
					if (!rootNode.isLeaf && rootNode.originalDockId == dockspaceId)
					{
						const bool has0 = (rootNode.child[0] >= 0);
						const bool has1 = (rootNode.child[1] >= 0);
						int inactiveBranch = -1;
						if (has0 != has1)
							inactiveBranch = has0 ? 0 : 1;
						else if (has0 && has1)
						{
							const int count0 = tree.nodes[rootNode.child[0]].leafCount;
							const int count1 = tree.nodes[rootNode.child[1]].leafCount;
							inactiveBranch = (count0 >= count1) ? 0 : 1;
						}

						if (inactiveBranch >= 0)
						{
							const int inactiveChildIndex = rootNode.child[inactiveBranch];
							const int activeBranch = 1 - inactiveBranch;
							if (inactiveChildIndex >= 0 && inactiveChildIndex < (int)tree.nodes.size())
							{
								preservedInactiveRootId = tree.nodes[inactiveChildIndex].originalDockId;
								preservedActiveRootId = dockRootBeforeRemove->ChildNodes[activeBranch]->ID;
								preservedInactiveRootIndex = inactiveChildIndex;
								preserveInactiveInPlace = (preservedInactiveRootId != 0 && preservedActiveRootId != 0);
							}
						}
					}
				}
			}

			// Protect inactive windows from DockSettingsRenameNodeReferences corruption.
			// Rebuild their inactive subtree below and remap to fresh DockIds.
			struct SNonVisibleDock { ImGuiWindow *win; ImGuiID dockId; const char *name; bool inPreservedInactiveRoot; };
			std::vector<SNonVisibleDock> savedNonVisible;
			for (int wi = 0; wi < context->Windows.Size; wi++)
			{
				ImGuiWindow *w = context->Windows[wi];
				if (!w || w->DockId == 0 || w->WasActive)
					continue;
				if (getDockRootId(w->DockId) != dockspaceId)
					continue;
				const bool inPreservedInactiveRoot = preserveInactiveInPlace && isDockNodeInSubtree(w->DockId, preservedInactiveRootId);
				savedNonVisible.push_back({w, w->DockId, w->Name, inPreservedInactiveRoot});
				if (!inPreservedInactiveRoot)
					w->DockId = 0;
			}
			for (const auto &s : savedNonVisible)
				LOGD("AutoLayout preserve-scan [pre-remove]: hidden '%s' dockId=%08x path=%s", s.name, s.dockId, DebugDockPathForId(s.dockId).c_str());

			std::vector<std::pair<ImGuiWindow*, ImGuiID>> inactiveWindowDockIds;
			for (const auto &s : savedNonVisible)
				inactiveWindowDockIds.push_back({s.win, s.dockId});

			const ImGuiID rebuildRootId = preserveInactiveInPlace ? preservedActiveRootId : dockspaceId;
			ImGui::DockBuilderRemoveNodeDockedWindows(rebuildRootId, true);
			ImGui::DockBuilderRemoveNodeChildNodes(rebuildRootId);
			for (const auto &s : savedNonVisible)
				LOGD("AutoLayout preserve-scan [post-remove]: hidden '%s' dockId=%08x path=%s", s.name, s.dockId, DebugDockPathForId(s.dockId).c_str());

			ImGui::DockBuilderSetNodePos(dockspaceId, viewport->WorkPos);
			ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

			ImGuiID activeRootId = dockspaceId;
			ImGuiID inactiveRootId = 0;
			int inactiveRebuildRootIndex = -1;
			bool inactiveEmbeddedAtRoot = false;
			if (preserveInactiveInPlace)
			{
				activeRootId = preservedActiveRootId;
				inactiveRootId = preservedInactiveRootId;
				inactiveRebuildRootIndex = preservedInactiveRootIndex;
				inactiveEmbeddedAtRoot = true;
				LOGD("AutoLayout preserve-scan: preserving inactive subtree in place inactiveRoot=%08x activeRoot=%08x",
					 inactiveRootId, activeRootId);
			}
			else if (inactiveTrees.size() == 1)
			{
				const SInactiveDockTree &tree = inactiveTrees[0];
				if (tree.rootIndex >= 0 && tree.rootIndex < (int)tree.nodes.size())
				{
					const SInactiveDockNode &rootNode = tree.nodes[tree.rootIndex];
					if (!rootNode.isLeaf)
					{
						const bool has0 = (rootNode.child[0] >= 0);
						const bool has1 = (rootNode.child[1] >= 0);
						if (has0 != has1)
						{
							ImGuiDir dir = (rootNode.splitAxis == ImGuiAxis_X) ? ImGuiDir_Left : ImGuiDir_Up;
							float ratio = ClampF(rootNode.ratio, 0.05f, 0.95f);
							ImGuiID child0Id = 0;
							ImGuiID child1Id = 0;
							ImGui::DockBuilderSplitNode(dockspaceId, dir, ratio, &child0Id, &child1Id);
							if (has0)
							{
								inactiveRootId = child0Id;
								activeRootId = child1Id;
								inactiveRebuildRootIndex = rootNode.child[0];
							}
							else
							{
								activeRootId = child0Id;
								inactiveRootId = child1Id;
								inactiveRebuildRootIndex = rootNode.child[1];
							}
							inactiveEmbeddedAtRoot = true;
							LOGD("AutoLayout preserve-scan: embedding inactive subtree at original root split dir=%d ratio=%f inactiveRoot=%08x activeRoot=%08x",
								 (int)dir, ratio, inactiveRootId, activeRootId);
						}
						else if (has0 && has1)
						{
							const int count0 = tree.nodes[rootNode.child[0]].leafCount;
							const int count1 = tree.nodes[rootNode.child[1]].leafCount;
							const bool preserveChild0 = (count0 >= count1);
							ImGuiDir dir = (rootNode.splitAxis == ImGuiAxis_X) ? ImGuiDir_Left : ImGuiDir_Up;
							float ratio = ClampF(rootNode.ratio, 0.05f, 0.95f);
							ImGuiID child0Id = 0;
							ImGuiID child1Id = 0;
							ImGui::DockBuilderSplitNode(dockspaceId, dir, ratio, &child0Id, &child1Id);
							if (preserveChild0)
							{
								inactiveRootId = child0Id;
								activeRootId = child1Id;
								inactiveRebuildRootIndex = rootNode.child[0];
							}
							else
							{
								activeRootId = child0Id;
								inactiveRootId = child1Id;
								inactiveRebuildRootIndex = rootNode.child[1];
							}
							inactiveEmbeddedAtRoot = true;
							LOGD("AutoLayout preserve-scan: embedding dominant inactive subtree at original root split dir=%d ratio=%f count0=%d count1=%d inactiveRoot=%08x activeRoot=%08x",
								 (int)dir, ratio, count0, count1, inactiveRootId, activeRootId);
						}
					}
				}
			}
			if (!inactiveEmbeddedAtRoot && !inactiveTrees.empty())
				ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.999f, &activeRootId, &inactiveRootId);

			std::vector<ImGuiID> dockIdByItem;
			dockIdByItem.assign(kept.size(), 0);

			auto buildDock = [&](auto &&self, ImGuiID nodeId, int nodeIdx)
			{
				if (nodeIdx < 0 || nodeIdx >= (int)nodes.size())
					return;
				const SScanTreeNode &n = nodes[nodeIdx];
				if (n.isLeaf)
				{
					const int idx = n.itemIndex;
					if (idx >= 0 && idx < (int)dockIdByItem.size())
						dockIdByItem[idx] = nodeId;
					return;
				}

				if (n.splitVertical)
				{
					ImGuiID outLeft = 0;
					ImGuiID outRight = 0;
					ImGui::DockBuilderSplitNode(nodeId, ImGuiDir_Left, n.ratio, &outLeft, &outRight);
					self(self, outLeft, n.a);
					self(self, outRight, n.b);
				}
				else
				{
					ImGuiID outUp = 0;
					ImGuiID outDown = 0;
					ImGui::DockBuilderSplitNode(nodeId, ImGuiDir_Up, n.ratio, &outUp, &outDown);
					self(self, outUp, n.a);
					self(self, outDown, n.b);
				}
			};
			auto applyPreserveScanLeafFlags = [&](ImGuiID dockId, ImGuiDockNodeFlags sourceFlags)
			{
				if (dockId == 0)
					return;

				ImGuiDockNode *node = ImGui::DockBuilderGetNode(dockId);
				if (node == NULL)
					return;

				ImGuiDockNodeFlags flags = GetAutoLayoutDockedPreserveScanLeafFlags(sourceFlags, preserveScanTabBarMode);
				if (flags != node->LocalFlags)
					node->SetLocalFlags(flags);
			};

			buildDock(buildDock, activeRootId, root);
			for (size_t i = 0; i < dockIdByItem.size() && i < kept.size(); i++)
				applyPreserveScanLeafFlags(dockIdByItem[i], kept[i].sourceDockNodeFlags);

			std::unordered_map<ImGuiWindow*, ImGuiID> inactiveWindowNewDockIds;
			if (inactiveRootId != 0 && !inactiveTrees.empty())
			{
				if (preserveInactiveInPlace)
				{
					for (const auto &wd : inactiveWindowDockIds)
					{
						if (isDockNodeInSubtree(wd.second, inactiveRootId))
							inactiveWindowNewDockIds[wd.first] = wd.second;
					}
				}
				else if (inactiveEmbeddedAtRoot && inactiveTrees.size() == 1 && inactiveRebuildRootIndex >= 0)
				{
					const SInactiveDockTree &tree = inactiveTrees[0];
					std::vector<std::pair<int, ImGuiID>> leafMap;
					rebuildInactiveSubtree(rebuildInactiveSubtree, tree, inactiveRebuildRootIndex, inactiveRootId, leafMap);
					std::unordered_map<ImGuiID, ImGuiID> originalToNew;
					for (const auto &lm : leafMap)
					{
						int leafIdx = lm.first;
						ImGuiID newDockId = lm.second;
						if (leafIdx >= 0 && leafIdx < (int)tree.nodes.size())
						{
							ImGuiID origId = tree.nodes[leafIdx].originalDockId;
							if (origId != 0)
								originalToNew[origId] = newDockId;
						}
					}
					for (const auto &wd : inactiveWindowDockIds)
					{
						auto it = originalToNew.find(wd.second);
						if (it != originalToNew.end())
							inactiveWindowNewDockIds[wd.first] = it->second;
					}
				}
				else
				{
					inactiveWindowNewDockIds = rebuildAllInactiveTrees(inactiveTrees, inactiveRootId, inactiveWindowDockIds);
				}
			}

			ImGui::DockBuilderFinish(dockspaceId);
			for (const auto &s : savedNonVisible)
				LOGD("AutoLayout preserve-scan [post-finish]: hidden '%s' dockId=%08x path=%s", s.name, s.dockId, DebugDockPathForId(s.dockId).c_str());

			for (const auto &s : savedNonVisible)
			{
				auto it = inactiveWindowNewDockIds.find(s.win);
				if (it != inactiveWindowNewDockIds.end())
				{
					s.win->DockId = it->second;
					LOGD("AutoLayout preserve-scan [restore-map]: hidden '%s' oldDockId=%08x newDockId=%08x newPath=%s",
						 s.name, s.dockId, s.win->DockId, DebugDockPathForId(s.win->DockId).c_str());
				}
				else
				{
					s.win->DockId = 0;
					LOGD("AutoLayout preserve-scan [restore-map]: hidden '%s' oldDockId=%08x left undocked",
						 s.name, s.dockId);
				}
			}
			for (const auto &s : savedNonVisible)
				LOGD("AutoLayout preserve-scan [post-restore]: hidden '%s' actualDockId=%08x path=%s",
					 s.name, s.win->DockId, DebugDockPathForId(s.win->DockId).c_str());

			if (inactiveRootId != 0 && !preserveInactiveInPlace)
			{
				ImGuiDockNode *inactiveRootNode = ImGui::DockBuilderGetNode(inactiveRootId);
				if (inactiveRootNode)
				{
					inactiveRootNode->SetLocalFlags(inactiveRootNode->LocalFlags | ImGuiDockNodeFlags_KeepAliveOnly);
					DebugMarkDockTreeInvisible(inactiveRootNode);
				}
			}

			for (size_t i = 0; i < kept.size(); i++)
			{
				if (!kept[i].view)
					continue;
				ImGuiID dockId = (i < dockIdByItem.size()) ? dockIdByItem[i] : 0;
				if (dockId == 0)
					dockId = dockspaceId;
				kept[i].view->DockToImGuiDockspace(dockId, viewport->ID);
			}

			anyDocked = true;
		}

		if (anyDocked)
			layoutStoreAfterFrameDelay = 2;
		return;
	}

	if (requestMode == AutoLayoutMode_Docked)
	{
		ImGuiPlatformIO &platformIO = ImGui::GetPlatformIO();
		bool anyDocked = false;

		struct SDockItem
		{
			CGuiView *view;
			float baseOuterW;
			float baseOuterH;
			float baseInnerW;
			float baseInnerH;
			float decorW;
			float decorH;
			float baseArea;
			float targetCenterX;
			float targetCenterY;
		};

		struct SDockSolution
		{
			float sGlobal;
			float movementCost;
			int sortMode;
			bool splitHorizontalFirst;
			std::vector<float> w;
			std::vector<float> h;
			std::vector<int> order;
			std::vector<SAutoLayoutRect> rects;   // w/h only, in order
			std::vector<SAutoLayoutRect> placed;  // in order
		};

		for (int vpIndex = 0; vpIndex < platformIO.Viewports.Size; vpIndex++)
		{
			ImGuiViewport *viewport = platformIO.Viewports[vpIndex];
			if (!viewport)
				continue;

			const ImGuiID dockspaceId = getDockspaceIdForViewport(viewport);
			if (dockspaceId == 0)
				continue;

			const float containerW = viewport->WorkSize.x;
			const float containerH = viewport->WorkSize.y;
			if (containerW <= 32.0f || containerH <= 32.0f)
				continue;

			std::vector<SDockItem> items;
			items.reserve(this->views.size() + 1);
			enumVisibleViews([&](CGuiView *view)
			{
				if (!view)
					return;
				if (!view->visible)
					return;
				if (this->viewFullScreen == view)
					return;
				if (!view->imGuiWindow)
					return;

				ImGuiWindow *win = view->imGuiWindow;
				if (!win || !win->Viewport)
					return;
				if (win->Viewport != viewport)
					return;

				SDockItem it;
				it.view = view;
				float ow = view->windowSizeX;
				float oh = view->windowSizeY;
				if (!(ow > 1.0f && oh > 1.0f))
				{
					ow = view->sizeX;
					oh = view->sizeY;
				}
				it.baseOuterW = ow;
				it.baseOuterH = oh;

				float iw = view->windowInnerRectSizeX;
				float ih = view->windowInnterRectSizeY;
				if (iw > 1.0f && ih > 1.0f)
				{
					it.baseInnerW = iw;
					it.baseInnerH = ih;
					it.decorW = std::max(0.0f, ow - iw);
					it.decorH = std::max(0.0f, oh - ih);
				}
				else
				{
					it.baseInnerW = 0.0f;
					it.baseInnerH = 0.0f;
					it.decorW = 0.0f;
					it.decorH = 0.0f;
				}

				it.baseArea = it.baseOuterW * it.baseOuterH;
				it.targetCenterX = (win->Pos.x + win->Size.x * 0.5f) - viewport->WorkPos.x;
				it.targetCenterY = (win->Pos.y + win->Size.y * 0.5f) - viewport->WorkPos.y;
				items.push_back(it);
			});

			if (items.empty())
				continue;

			enum EDockedAutoLayoutGroupType
			{
				DockGroup_Single = 0,
				DockGroup_Numeric = 1,
				DockGroup_Prefix = 2
			};

			struct SDockGroupMember
			{
				int itemIndex;
				std::string roleKey; // only meaningful for numeric groups
				float cx;
				float cy;
				float u;
				float v;
			};

			struct SDockGroupTreeNode
			{
				bool isLeaf;
				bool splitVertical; // true: Left/Right, false: Up/Down
				int a;
				int b;
				int itemIndex; // leaf
			};

			struct SDockGroup
			{
				EDockedAutoLayoutGroupType type;
				std::string key;
				std::string numericId;
				std::vector<SDockGroupMember> members;
				std::vector<SDockGroupTreeNode> nodes;
				int rootNode;
				float bboxMinX;
				float bboxMinY;
				float bboxMaxX;
				float bboxMaxY;
				float targetCenterX;
				float targetCenterY;
			};

			auto trimRightSpaces = [](std::string &s)
			{
				while (!s.empty())	
				{
					char c = s.back();
					if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
						break;
					s.pop_back();
				}
			};
			auto trimLeftSpaces = [](std::string &s)
			{
				size_t i = 0;
				while (i < s.size())
				{
					char c = s[i];
					if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
						break;
					i++;
				}
				if (i > 0)
					s.erase(0, i);
			};
			auto extractNumericFromStableId = [](const std::string &stableId, std::string &outRoleKey, std::string &outNum) -> bool
			{
				if (stableId.empty())
					return false;
				size_t dot = stableId.find_last_of('.');
				if (dot == std::string::npos || dot + 1 >= stableId.size())
					return false;
				bool allDigits = true;
				for (size_t i = dot + 1; i < stableId.size(); i++)
				{
					if (!std::isdigit((unsigned char)stableId[i]))
					{
						allDigits = false;
						break;
					}
				}
				if (!allDigits)
					return false;
				outRoleKey.assign(stableId.begin(), stableId.begin() + dot);
				outNum.assign(stableId.begin() + dot + 1, stableId.end());
				return (!outRoleKey.empty() && !outNum.empty());
			};
			auto extractNumericFromLabel = [&](std::string label, std::string &outRoleKey, std::string &outNum) -> bool
			{
				trimRightSpaces(label);
				if (label.empty())
					return false;

				// Pattern 1: "...#<digits>"
				{
					size_t hash = label.find_last_of('#');
					if (hash != std::string::npos && hash + 1 < label.size())
					{
						size_t i = hash + 1;
						if (std::isdigit((unsigned char)label[i]))
						{
							size_t j = i;
							while (j < label.size() && std::isdigit((unsigned char)label[j]))
								j++;
							if (j == label.size())
							{
								size_t roleEnd = hash;
								while (roleEnd > 0 && (label[roleEnd - 1] == ' ' || label[roleEnd - 1] == '\t'))
									roleEnd--;
								outRoleKey.assign(label.begin(), label.begin() + roleEnd);
								outNum.assign(label.begin() + i, label.end());
								trimRightSpaces(outRoleKey);
								if (!outRoleKey.empty() && !outNum.empty())
									return true;
							}
						}
					}
				}

				// Pattern 2: "... <digits>" (only if the digit run is preceded by whitespace)
				size_t end = label.size();
				size_t digitStart = end;
				while (digitStart > 0 && std::isdigit((unsigned char)label[digitStart - 1]))
					digitStart--;
				if (digitStart == end)
					return false;
				if (digitStart == 0)
					return false;
				if (!std::isspace((unsigned char)label[digitStart - 1]))
					return false;
				outRoleKey.assign(label.begin(), label.begin() + digitStart);
				trimRightSpaces(outRoleKey);
				outNum.assign(label.begin() + digitStart, label.end());
				trimLeftSpaces(outNum);
				return (!outRoleKey.empty() && !outNum.empty());
			};
			auto extractPrefixToken = [&](std::string label) -> std::string
			{
				trimLeftSpaces(label);
				trimRightSpaces(label);
				if (label.empty())
					return std::string();
				size_t end = 0;
				while (end < label.size() && !std::isspace((unsigned char)label[end]))
					end++;
				if (end == 0)
					return std::string();
				return label.substr(0, end);
			};
			auto medianOf = [](std::vector<float> v) -> float
			{
				if (v.empty())
					return 0.5f;
				std::sort(v.begin(), v.end());
				const size_t n = v.size();
				if ((n & 1) == 1)
					return v[n / 2];
				return (v[n / 2 - 1] + v[n / 2]) * 0.5f;
			};

			struct SItemGroupInfo
			{
				bool isNumeric = false;
				std::string numericId;
				std::string roleKey;
				std::string token;
			};
			std::vector<SItemGroupInfo> itemInfo(items.size());
			std::unordered_map<std::string, int> tokenCounts;
			tokenCounts.reserve(items.size() * 2);

			for (size_t i = 0; i < items.size(); i++)
			{
				CGuiView *view = items[i].view;
				if (!view)
					continue;

				std::string label;
				view->BuildImGuiWindowLabel(label);
				trimLeftSpaces(label);
				trimRightSpaces(label);

				std::string roleKey;
				std::string num;
				if (extractNumericFromStableId(view->imGuiStableId, roleKey, num)
					|| extractNumericFromLabel(label, roleKey, num))
				{
					itemInfo[i].isNumeric = true;
					itemInfo[i].numericId = num;
					itemInfo[i].roleKey = roleKey;
					continue;
				}

				std::string token = extractPrefixToken(label);
				if (!token.empty())
				{
					itemInfo[i].token = token;
					tokenCounts[token]++;
				}
			}

			std::vector<SDockGroup> groups;
			groups.reserve(items.size());
			std::unordered_map<std::string, int> groupIndexByKey;
			groupIndexByKey.reserve(items.size() * 2);

			auto createGroup = [&](EDockedAutoLayoutGroupType type, const std::string &key, const std::string &numericId) -> int
			{
				SDockGroup g;
				g.type = type;
				g.key = key;
				g.numericId = numericId;
				g.rootNode = -1;
				g.bboxMinX = FLT_MAX;
				g.bboxMinY = FLT_MAX;
				g.bboxMaxX = -FLT_MAX;
				g.bboxMaxY = -FLT_MAX;
				g.targetCenterX = 0.0f;
				g.targetCenterY = 0.0f;
				groups.push_back(g);
				return (int)groups.size() - 1;
			};

			auto addMemberToGroup = [&](int groupIndex, int itemIndex)
			{
				if (groupIndex < 0 || groupIndex >= (int)groups.size())
					return;
				if (itemIndex < 0 || itemIndex >= (int)items.size())
					return;

				CGuiView *view = items[itemIndex].view;
				if (!view || !view->imGuiWindow)
					return;
				ImGuiWindow *win = view->imGuiWindow;
				float x0 = win->Pos.x - viewport->WorkPos.x;
				float y0 = win->Pos.y - viewport->WorkPos.y;
				float x1 = x0 + win->Size.x;
				float y1 = y0 + win->Size.y;

				SDockGroup &g = groups[groupIndex];
				g.bboxMinX = std::min(g.bboxMinX, x0);
				g.bboxMinY = std::min(g.bboxMinY, y0);
				g.bboxMaxX = std::max(g.bboxMaxX, x1);
				g.bboxMaxY = std::max(g.bboxMaxY, y1);

				SDockGroupMember m;
				m.itemIndex = itemIndex;
				m.roleKey = itemInfo[itemIndex].roleKey;
				m.cx = items[itemIndex].targetCenterX;
				m.cy = items[itemIndex].targetCenterY;
				m.u = 0.5f;
				m.v = 0.5f;
				g.members.push_back(m);
			};

			for (size_t i = 0; i < items.size(); i++)
			{
				if (itemInfo[i].isNumeric && !itemInfo[i].numericId.empty())
				{
					std::string key = "N:" + itemInfo[i].numericId;
					auto it = groupIndexByKey.find(key);
					int gi = -1;
					if (it == groupIndexByKey.end())
					{
						gi = createGroup(DockGroup_Numeric, key, itemInfo[i].numericId);
						groupIndexByKey[key] = gi;
					}
					else
					{
						gi = it->second;
					}
					addMemberToGroup(gi, (int)i);
					continue;
				}

				if (!itemInfo[i].token.empty())
				{
					auto itCount = tokenCounts.find(itemInfo[i].token);
					if (itCount != tokenCounts.end() && itCount->second >= 2)
					{
						std::string key = "P:" + itemInfo[i].token;
						auto it = groupIndexByKey.find(key);
						int gi = -1;
						if (it == groupIndexByKey.end())
						{
							gi = createGroup(DockGroup_Prefix, key, std::string());
							groupIndexByKey[key] = gi;
						}
						else
						{
							gi = it->second;
						}
						addMemberToGroup(gi, (int)i);
						continue;
					}
				}

				// Singleton group
				std::string key = "S:" + std::to_string((int)i);
				int gi = createGroup(DockGroup_Single, key, std::string());
				addMemberToGroup(gi, (int)i);
			}

			for (SDockGroup &g : groups)
			{
				const float gw = std::max(1.0f, g.bboxMaxX - g.bboxMinX);
				const float gh = std::max(1.0f, g.bboxMaxY - g.bboxMinY);
				g.targetCenterX = (g.bboxMinX + g.bboxMaxX) * 0.5f;
				g.targetCenterY = (g.bboxMinY + g.bboxMaxY) * 0.5f;
				for (SDockGroupMember &m : g.members)
				{
					m.u = ClampF((m.cx - g.bboxMinX) / gw, 0.0f, 1.0f);
					m.v = ClampF((m.cy - g.bboxMinY) / gh, 0.0f, 1.0f);
				}
			}

			// Detect a common pattern among numeric groups (e.g. Map/Buildings/Player #1/#2/#3).
			bool numericPatternStable = false;
			std::unordered_map<std::string, ImVec2> numericConsensusUv;
			{
				std::vector<std::unordered_map<std::string, ImVec2>> posByRole;
				posByRole.reserve(groups.size());
				std::unordered_map<std::string, std::vector<float>> roleUs;
				std::unordered_map<std::string, std::vector<float>> roleVs;
				int numericGroupsCount = 0;

				for (const SDockGroup &g : groups)
				{
					if (g.type != DockGroup_Numeric)
						continue;
					std::unordered_map<std::string, ImVec2> pos;
					for (const SDockGroupMember &m : g.members)
					{
						if (m.roleKey.empty())
							continue;
						pos[m.roleKey] = ImVec2(m.u, m.v);
						roleUs[m.roleKey].push_back(m.u);
						roleVs[m.roleKey].push_back(m.v);
					}
					posByRole.push_back(std::move(pos));
					numericGroupsCount++;
				}

				std::vector<std::string> roles;
				roles.reserve(roleUs.size());
				for (auto &kv : roleUs)
				{
					if (kv.second.size() >= 2)
						roles.push_back(kv.first);
				}

				const float relEps = 0.08f;
				int stableRel = 0;
				float stableRelFracSum = 0.0f;
				for (size_t a = 0; a < roles.size(); a++)
				{
					for (size_t b = a + 1; b < roles.size(); b++)
					{
						const std::string &ra = roles[a];
						const std::string &rb = roles[b];

						// X relation (left/right)
						int pos = 0;
						int neg = 0;
						for (auto &posMap : posByRole)
						{
							auto ita = posMap.find(ra);
							auto itb = posMap.find(rb);
							if (ita == posMap.end() || itb == posMap.end())
								continue;
							const float dx = ita->second.x - itb->second.x;
							if (fabs(dx) <= relEps)
								continue;
							if (dx < 0.0f)
								neg++;
							else
								pos++;
						}
						const int votesX = pos + neg;
						if (votesX >= 2)
						{
							const float frac = (float)std::max(pos, neg) / (float)votesX;
							if (frac >= 0.75f)
							{
								stableRel++;
								stableRelFracSum += frac;
							}
						}

						// Y relation (above/below)
						pos = 0;
						neg = 0;
						for (auto &posMap : posByRole)
						{
							auto ita = posMap.find(ra);
							auto itb = posMap.find(rb);
							if (ita == posMap.end() || itb == posMap.end())
								continue;
							const float dy = ita->second.y - itb->second.y;
							if (fabs(dy) <= relEps)
								continue;
							if (dy < 0.0f)
								neg++;
							else
								pos++;
						}
						const int votesY = pos + neg;
						if (votesY >= 2)
						{
							const float frac = (float)std::max(pos, neg) / (float)votesY;
							if (frac >= 0.75f)
							{
								stableRel++;
								stableRelFracSum += frac;
							}
						}
					}
				}

				if (numericGroupsCount >= 2 && stableRel >= 2)
				{
					const float avgFrac = stableRelFracSum / (float)stableRel;
					if (avgFrac >= 0.80f)
						numericPatternStable = true;
				}

				if (numericPatternStable)
				{
					for (auto &kv : roleUs)
					{
						auto itv = roleVs.find(kv.first);
						if (kv.second.size() < 2 || itv == roleVs.end() || itv->second.size() < 2)
							continue;
						const float mu = ClampF(medianOf(kv.second), 0.0f, 1.0f);
						const float mv = ClampF(medianOf(itv->second), 0.0f, 1.0f);
						numericConsensusUv[kv.first] = ImVec2(mu, mv);
					}
				}
			}

			struct SPoint
			{
				int itemIndex;
				float u;
				float v;
			};
			auto buildSplitTree = [&](const std::vector<SPoint> &pts, std::vector<SDockGroupTreeNode> &outNodes) -> int
			{
				outNodes.clear();
				if (pts.empty())
					return -1;
				std::vector<int> idxs;
				idxs.reserve(pts.size());
				for (size_t i = 0; i < pts.size(); i++)
					idxs.push_back((int)i);

				auto rec = [&](auto &&self, std::vector<int> sub, int depth) -> int
				{
					if (sub.size() == 1)
					{
						SDockGroupTreeNode n;
						n.isLeaf = true;
						n.splitVertical = true;
						n.a = -1;
						n.b = -1;
						n.itemIndex = pts[sub[0]].itemIndex;
						outNodes.push_back(n);
						return (int)outNodes.size() - 1;
					}

					float minU = FLT_MAX;
					float maxU = -FLT_MAX;
					float minV = FLT_MAX;
					float maxV = -FLT_MAX;
					for (int pi : sub)
					{
						minU = std::min(minU, pts[pi].u);
						maxU = std::max(maxU, pts[pi].u);
						minV = std::min(minV, pts[pi].v);
						maxV = std::max(maxV, pts[pi].v);
					}
					const float rangeU = maxU - minU;
					const float rangeV = maxV - minV;
					const bool splitVertical = (rangeU >= rangeV);

					auto cmp = [&](int a, int b)
					{
						const SPoint &pa = pts[a];
						const SPoint &pb = pts[b];
						if (splitVertical)
						{
							if (pa.u != pb.u)
								return pa.u < pb.u;
							if (pa.v != pb.v)
								return pa.v < pb.v;
						}
						else
						{
							if (pa.v != pb.v)
								return pa.v < pb.v;
							if (pa.u != pb.u)
								return pa.u < pb.u;
						}
						return pa.itemIndex < pb.itemIndex;
					};
					std::sort(sub.begin(), sub.end(), cmp);
					size_t mid = sub.size() / 2;
					if (mid == 0)
						mid = 1;
					std::vector<int> left(sub.begin(), sub.begin() + mid);
					std::vector<int> right(sub.begin() + mid, sub.end());
					int na = self(self, left, depth + 1);
					int nb = self(self, right, depth + 1);

					SDockGroupTreeNode n;
					n.isLeaf = false;
					n.splitVertical = splitVertical;
					n.a = na;
					n.b = nb;
					n.itemIndex = -1;
					outNodes.push_back(n);
					return (int)outNodes.size() - 1;
				};

				return rec(rec, idxs, 0);
			};

			// Build group split trees.
			for (SDockGroup &g : groups)
			{
				g.nodes.clear();
				g.rootNode = -1;
				if (g.members.empty())
					continue;
				if (g.members.size() == 1)
				{
					SDockGroupTreeNode n;
					n.isLeaf = true;
					n.splitVertical = true;
					n.a = -1;
					n.b = -1;
					n.itemIndex = g.members[0].itemIndex;
					g.nodes.push_back(n);
					g.rootNode = 0;
					continue;
				}

				std::vector<SPoint> pts;
				pts.reserve(g.members.size());
				for (const SDockGroupMember &m : g.members)
				{
					float u = m.u;
					float v = m.v;
					if (g.type == DockGroup_Numeric && numericPatternStable && !m.roleKey.empty())
					{
						auto it = numericConsensusUv.find(m.roleKey);
						if (it != numericConsensusUv.end())
						{
							u = it->second.x;
							v = it->second.y;
						}
					}
					pts.push_back({m.itemIndex, ClampF(u, 0.0f, 1.0f), ClampF(v, 0.0f, 1.0f)});
				}

				g.rootNode = buildSplitTree(pts, g.nodes);
			}

			float minArea = FLT_MAX;
			float maxArea = 0.0f;
			for (const SDockItem &it : items)
			{
				minArea = std::min(minArea, it.baseArea);
				maxArea = std::max(maxArea, it.baseArea);
			}
			const float areaRange = std::max(1.0f, maxArea - minArea);
			const float gapDock = 0.0f;

				auto trySolveVariant = [&](float sGlobal, int sortMode, bool splitHorizontalFirst, SDockSolution &out) -> bool
				{
					out.sGlobal = sGlobal;
					out.sortMode = sortMode;
					out.splitHorizontalFirst = splitHorizontalFirst;
					out.w.assign(items.size(), 0.0f);
					out.h.assign(items.size(), 0.0f);
					out.order.clear();
					out.rects.clear();
					out.placed.clear();
					out.movementCost = 0.0f;

					struct SPack
					{
						int idx;
						float pw;
						float ph;
						float area;
						float maxDim;
						float tcx;
						float tcy;
					};
					std::vector<SPack> pack;
					pack.reserve(groups.size());

					// First compute target sizes for all leaf windows.
					for (size_t i = 0; i < items.size(); i++)
					{
						const SDockItem &it = items[i];
						const float norm = ClampF((it.baseArea - minArea) / areaRange, 0.0f, 1.0f);
						const float exp = expMin + (expMax - expMin) * norm;
						float si = powf(ClampF(sGlobal, 0.01f, 1.0f), exp);
						si = ClampF(si, 0.01f, 1.0f);

					float ow = it.baseOuterW;
					float oh = it.baseOuterH;
					if (it.baseInnerW > 1.0f && it.baseInnerH > 1.0f)
					{
						const float iw = it.baseInnerW * si;
						const float ih = it.baseInnerH * si;
						ow = iw + it.decorW;
						oh = ih + it.decorH;
					}
					else
					{
						ow = ow * si;
						oh = oh * si;
					}

						ow = std::max(minSize, ow);
						oh = std::max(minSize, oh);

						out.w[i] = ow;
						out.h[i] = oh;
					}

					struct SSize
					{
						float w;
						float h;
					};

					auto evalGroupNodeSize = [&](auto &&self, const SDockGroup &g, int nodeIdx) -> SSize
					{
						if (nodeIdx < 0 || nodeIdx >= (int)g.nodes.size())
							return {0.0f, 0.0f};
						const SDockGroupTreeNode &n = g.nodes[nodeIdx];
						if (n.isLeaf)
						{
							const int idx = n.itemIndex;
							if (idx < 0 || idx >= (int)items.size())
								return {0.0f, 0.0f};
							return {out.w[idx], out.h[idx]};
						}
						SSize a = self(self, g, n.a);
						SSize b = self(self, g, n.b);
						if (n.splitVertical)
							return {a.w + b.w, std::max(a.h, b.h)};
						return {std::max(a.w, b.w), a.h + b.h};
					};

					// Then pack groups (singletons are treated as groups of size 1).
					for (size_t gi = 0; gi < groups.size(); gi++)
					{
						const SDockGroup &g = groups[gi];
						if (g.rootNode < 0 || g.nodes.empty() || g.members.empty())
							continue;
						const SSize gs = evalGroupNodeSize(evalGroupNodeSize, g, g.rootNode);
						const float pw = gs.w + gapDock;
						const float ph = gs.h + gapDock;
						if (pw > containerW || ph > containerH)
							return false;

						SPack p;
						p.idx = (int)gi;
						p.pw = pw;
						p.ph = ph;
						p.area = pw * ph;
						p.maxDim = std::max(pw, ph);
						p.tcx = g.targetCenterX;
						p.tcy = g.targetCenterY;
						pack.push_back(p);
					}

					if (pack.empty())
						return false;

					auto cmpArea = [](const SPack &a, const SPack &b)
					{
						if (a.area != b.area)
							return a.area > b.area;
					if (a.maxDim != b.maxDim)
						return a.maxDim > b.maxDim;
					if (a.tcy != b.tcy)
						return a.tcy < b.tcy;
					return a.tcx < b.tcx;
				};
				auto cmpMaxDim = [](const SPack &a, const SPack &b)
				{
					if (a.maxDim != b.maxDim)
						return a.maxDim > b.maxDim;
					if (a.area != b.area)
						return a.area > b.area;
					if (a.tcy != b.tcy)
						return a.tcy < b.tcy;
					return a.tcx < b.tcx;
				};
				auto cmpPos = [](const SPack &a, const SPack &b)
				{
					if (a.tcy != b.tcy)
						return a.tcy < b.tcy;
					if (a.tcx != b.tcx)
						return a.tcx < b.tcx;
					if (a.area != b.area)
						return a.area > b.area;
					return a.maxDim > b.maxDim;
				};

				if (sortMode == 0)
					std::sort(pack.begin(), pack.end(), cmpArea);
				else if (sortMode == 1)
					std::sort(pack.begin(), pack.end(), cmpMaxDim);
				else
					std::sort(pack.begin(), pack.end(), cmpPos);

				out.order.reserve(pack.size());
				out.rects.reserve(pack.size());
				for (const SPack &p : pack)
				{
					out.order.push_back(p.idx);
					out.rects.push_back({0.0f, 0.0f, p.pw, p.ph});
				}

				if (!PackGuillotine(out.rects, containerW, containerH, splitHorizontalFirst, out.placed))
					return false;

					out.movementCost = 0.0f;
					for (size_t k = 0; k < pack.size(); k++)
					{
						const int idx = pack[k].idx;
						if (idx < 0 || idx >= (int)groups.size())
							continue;
						const float tcx = ClampF(groups[idx].targetCenterX, 0.0f, containerW);
						const float tcy = ClampF(groups[idx].targetCenterY, 0.0f, containerH);
						const float cx = out.placed[k].x + out.placed[k].w * 0.5f;
						const float cy = out.placed[k].y + out.placed[k].h * 0.5f;
						const float dx = cx - tcx;
						const float dy = cy - tcy;
						out.movementCost += dx * dx + dy * dy;
					}

				return true;
			};

			auto solveBestScaleVariant = [&](int sortMode, bool splitHorizontalFirst, SDockSolution &outBest) -> bool
			{
				SDockSolution sol;
				if (trySolveVariant(1.0f, sortMode, splitHorizontalFirst, sol))
				{
					outBest = sol;
					return true;
				}

				float lo = 0.05f;
				float hi = 1.0f;
				SDockSolution solLo;
				if (!trySolveVariant(lo, sortMode, splitHorizontalFirst, solLo))
					return false;
				outBest = solLo;
				for (int iter = 0; iter < 22; iter++)
				{
					const float mid = (lo + hi) * 0.5f;
					SDockSolution solMid;
					if (trySolveVariant(mid, sortMode, splitHorizontalFirst, solMid))
					{
						outBest = solMid;
						lo = mid;
					}
					else
					{
						hi = mid;
					}
				}
				return true;
			};

			bool any = false;
			SDockSolution best;
			for (int sortMode = 0; sortMode < 3; sortMode++)
			{
				for (int splitMode = 0; splitMode < 2; splitMode++)
				{
					SDockSolution sol;
					if (!solveBestScaleVariant(sortMode, splitMode == 1, sol))
						continue;
					if (!any
						|| sol.sGlobal > best.sGlobal + 0.0001f
						|| (fabs(sol.sGlobal - best.sGlobal) <= 0.0001f && sol.movementCost < best.movementCost))
					{
						best = sol;
						any = true;
					}
				}
			}

			if (!any)
				continue;

			// Protect inactive windows (stopped emulator) from DockSettingsRenameNodeReferences
			// corruption. Temporarily zero their DockId; after the rebuild they are left
			// undocked (DockId=0) — their positions/sizes stay as-is, they just float.
			struct SNonVisibleDock { ImGuiWindow *win; ImGuiID dockId; const char *name; };
			std::vector<SNonVisibleDock> savedNonVisible;
			for (int wi = 0; wi < context->Windows.Size; wi++)
			{
				ImGuiWindow *w = context->Windows[wi];
				if (!w || w->DockId == 0 || w->WasActive)
					continue;
				savedNonVisible.push_back({w, w->DockId, w->Name});
				w->DockId = 0;
			}
			for (const auto &s : savedNonVisible)
				LOGD("AutoLayout preserve-scan [pre-remove]: hidden '%s' dockId=%08x path=%s", s.name, s.dockId, DebugDockPathForId(s.dockId).c_str());

			ImGui::DockBuilderRemoveNodeDockedWindows(dockspaceId, true);
			ImGui::DockBuilderRemoveNodeChildNodes(dockspaceId);
			for (const auto &s : savedNonVisible)
				LOGD("AutoLayout preserve-scan [post-remove]: hidden '%s' dockId=%08x path=%s", s.name, s.dockId, DebugDockPathForId(s.dockId).c_str());

			ImGui::DockBuilderSetNodePos(dockspaceId, viewport->WorkPos);
			ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

			ImGuiID activeRootId = dockspaceId;

				std::vector<ImGuiID> leafNodeIds;
				if (!PackGuillotineDockNodes(best.rects, containerW, containerH, best.splitHorizontalFirst, activeRootId, leafNodeIds))
					continue;

				// Build sub-dock trees inside each packed group and dock windows to the leaf nodes.
				std::vector<ImGuiID> dockIdByItem;
				dockIdByItem.assign(items.size(), 0);

				struct SSize
				{
					float w;
					float h;
				};

				auto buildGroupDockTree = [&](const SDockGroup &g, ImGuiID groupRootDockId)
				{
					if (g.rootNode < 0 || g.nodes.empty())
						return;
					if (g.members.empty())
						return;
					if (g.nodes.size() == 1 && g.nodes[0].isLeaf)
					{
						const int idx = g.nodes[0].itemIndex;
						if (idx >= 0 && idx < (int)dockIdByItem.size())
							dockIdByItem[idx] = groupRootDockId;
						return;
					}

					std::vector<SSize> nodeSizes;
					std::vector<bool> computed;
					nodeSizes.assign(g.nodes.size(), {0.0f, 0.0f});
					computed.assign(g.nodes.size(), false);

					auto computeSize = [&](auto &&self, int nodeIdx) -> SSize
					{
						if (nodeIdx < 0 || nodeIdx >= (int)g.nodes.size())
							return {0.0f, 0.0f};
						if (computed[nodeIdx])
							return nodeSizes[nodeIdx];
						computed[nodeIdx] = true;
						const SDockGroupTreeNode &n = g.nodes[nodeIdx];
						if (n.isLeaf)
						{
							const int idx = n.itemIndex;
							if (idx < 0 || idx >= (int)items.size())
								return {0.0f, 0.0f};
							nodeSizes[nodeIdx] = {best.w[idx], best.h[idx]};
							return nodeSizes[nodeIdx];
						}
						SSize a = self(self, n.a);
						SSize b = self(self, n.b);
						if (n.splitVertical)
							nodeSizes[nodeIdx] = {a.w + b.w, std::max(a.h, b.h)};
						else
							nodeSizes[nodeIdx] = {std::max(a.w, b.w), a.h + b.h};
						return nodeSizes[nodeIdx];
					};

					computeSize(computeSize, g.rootNode);

					auto buildDock = [&](auto &&self, ImGuiID nodeId, int nodeIdx)
					{
						if (nodeIdx < 0 || nodeIdx >= (int)g.nodes.size())
							return;
						const SDockGroupTreeNode &n = g.nodes[nodeIdx];
						if (n.isLeaf)
						{
							const int idx = n.itemIndex;
							if (idx >= 0 && idx < (int)dockIdByItem.size())
								dockIdByItem[idx] = nodeId;
							return;
						}

						const SSize a = nodeSizes[n.a];
						const SSize b = nodeSizes[n.b];
						if (n.splitVertical)
						{
							const float denom = std::max(1.0f, a.w + b.w);
							float ratio = ClampF(a.w / denom, 0.05f, 0.95f);
							ImGuiID outLeft = 0;
							ImGuiID outRight = 0;
							ImGui::DockBuilderSplitNode(nodeId, ImGuiDir_Left, ratio, &outLeft, &outRight);
							self(self, outLeft, n.a);
							self(self, outRight, n.b);
						}
						else
						{
							const float denom = std::max(1.0f, a.h + b.h);
							float ratio = ClampF(a.h / denom, 0.05f, 0.95f);
							ImGuiID outUp = 0;
							ImGuiID outDown = 0;
							ImGui::DockBuilderSplitNode(nodeId, ImGuiDir_Up, ratio, &outUp, &outDown);
							self(self, outUp, n.a);
							self(self, outDown, n.b);
						}
					};

					buildDock(buildDock, groupRootDockId, g.rootNode);
				};

				for (size_t k = 0; k < leafNodeIds.size() && k < best.order.size(); k++)
				{
					const int gi = best.order[k];
					if (gi < 0 || gi >= (int)groups.size())
						continue;
					buildGroupDockTree(groups[gi], leafNodeIds[k]);
				}

				ImGui::DockBuilderFinish(dockspaceId);

				// Leave inactive windows undocked — no split needed, no 32px strip.
				for (const auto &s : savedNonVisible)
				{
					s.win->DockId = 0;
					LOGD("AutoLayout preserve-scan [restore]: hidden '%s' left undocked", s.name);
				}

				for (size_t i = 0; i < items.size(); i++)
				{
					if (!items[i].view)
						continue;
					ImGuiID dockId = (i < dockIdByItem.size()) ? dockIdByItem[i] : 0;
					if (dockId == 0)
						dockId = dockspaceId;
					items[i].view->DockToImGuiDockspace(dockId, viewport->ID);
				}

				anyDocked = true;
			}

		if (anyDocked)
			layoutStoreAfterFrameDelay = 2;
		return;
	}

	ImGuiPlatformIO &platformIO = ImGui::GetPlatformIO();
	bool anyApplied = false;

	auto layoutViewport = [&](ImGuiViewport *viewport)
	{
		if (!viewport)
			return false;

		const float containerW = viewport->WorkSize.x - margin * 2.0f;
		const float containerH = viewport->WorkSize.y - margin * 2.0f;
		if (containerW <= 32.0f || containerH <= 32.0f)
			return false;

	struct SItem
	{
		int index;
		bool isDockRoot;
		CGuiView *view;
		ImGuiID dockRootId;
		float baseOuterW;
		float baseOuterH;
		float baseAbsX;
		float baseAbsY;
		float baseInnerW;
		float baseInnerH;
		float decorW;
		float decorH;
		float baseArea;
	};

		std::vector<SItem> items;
		items.reserve(this->views.size() + 1);

		std::unordered_map<ImGuiID, int> dockRootIndex;
		// Add a view as either an individual floating window or as a part of a floating dock-root group.
		auto addViewToItems = [&](CGuiView *view)
		{
			if (!view)
				return;
			if (!view->visible)
				return;
			if (this->viewFullScreen == view)
				return;
			// Only arrange views that are actual ImGui windows (have been instantiated at least once).
			if (!view->imGuiWindow)
				return;

			ImGuiWindow *win = view->imGuiWindow;
			if (!win || !win->Viewport || win->Viewport != viewport)
				return;

			// Prefer dock grouping if the view is docked into a floating dock node.
			if (win->DockNode)
			{
				ImGuiDockNode *root = ImGui::DockNodeGetRootNode(win->DockNode);
				if (root && root->IsFloatingNode())
				{
					auto it = dockRootIndex.find(root->ID);
					if (it == dockRootIndex.end())
					{
						SItem item;
						item.index = (int)items.size();
						item.isDockRoot = true;
						item.view = NULL;
						item.dockRootId = root->ID;
						item.baseOuterW = root->Size.x;
						item.baseOuterH = root->Size.y;
						item.baseAbsX = root->Pos.x;
						item.baseAbsY = root->Pos.y;
						item.baseInnerW = 0.0f;
						item.baseInnerH = 0.0f;
						item.decorW = 0.0f;
						item.decorH = 0.0f;
						item.baseArea = item.baseOuterW * item.baseOuterH;
						items.push_back(item);
						dockRootIndex[root->ID] = item.index;
					}
					return;
				}

				// Docked into a dockspace (main or custom). Preserve/Compact: do not alter dockspace trees.
				return;
			}

			// Non-docked floating window: treat as a standalone window.
			SItem item;
			item.index = (int)items.size();
			item.isDockRoot = false;
			item.view = view;
			item.dockRootId = 0;

			float ow = view->windowSizeX;
			float oh = view->windowSizeY;
			if (!(ow > 1.0f && oh > 1.0f))
			{
				ow = view->sizeX;
				oh = view->sizeY;
			}
			item.baseOuterW = ow;
			item.baseOuterH = oh;
			item.baseAbsX = view->windowPosX;
			item.baseAbsY = view->windowPosY;
			item.baseArea = ow * oh;

			float iw = view->windowInnerRectSizeX;
			float ih = view->windowInnterRectSizeY;
			if (iw > 1.0f && ih > 1.0f)
			{
				item.baseInnerW = iw;
				item.baseInnerH = ih;
				item.decorW = std::max(0.0f, ow - iw);
				item.decorH = std::max(0.0f, oh - ih);
			}
			else
			{
				item.baseInnerW = 0.0f;
				item.baseInnerH = 0.0f;
				item.decorW = 0.0f;
				item.decorH = 0.0f;
			}

			items.push_back(item);
		};

		addViewToItems(this->currentView);
		for (auto it = this->views.begin(); it != this->views.end(); it++)
			addViewToItems(*it);

		if (items.empty())
			return false;

	float minArea = FLT_MAX;
	float maxArea = 0.0f;
	for (const SItem &it : items)
	{
		minArea = std::min(minArea, it.baseArea);
		maxArea = std::max(maxArea, it.baseArea);
	}
	const float areaRange = std::max(1.0f, maxArea - minArea);

	const float originX = viewport->WorkPos.x + margin;
	const float originY = viewport->WorkPos.y + margin;

	std::vector<float> targetCenterX;
	std::vector<float> targetCenterY;
	targetCenterX.reserve(items.size());
	targetCenterY.reserve(items.size());
	for (const SItem &it : items)
	{
		targetCenterX.push_back((it.baseAbsX + it.baseOuterW * 0.5f) - originX);
		targetCenterY.push_back((it.baseAbsY + it.baseOuterH * 0.5f) - originY);
	}

	struct SSolution
	{
		float sGlobal;
		std::vector<float> w;
		std::vector<float> h;
		std::vector<float> x;
		std::vector<float> y;
	};

	struct SPack
	{
		int idx;
		float pw;
		float ph;
		float area;
		float maxDim;
	};

	auto buildPack = [&](float sGlobal, SSolution &out, std::vector<SPack> &pack, std::vector<float> &packW, std::vector<float> &packH, std::vector<float> &packArea) -> bool
	{
		out.sGlobal = sGlobal;
		out.w.assign(items.size(), 0.0f);
		out.h.assign(items.size(), 0.0f);
		out.x.assign(items.size(), 0.0f);
		out.y.assign(items.size(), 0.0f);
		pack.clear();
		pack.reserve(items.size());
		packW.assign(items.size(), 0.0f);
		packH.assign(items.size(), 0.0f);
		packArea.assign(items.size(), 0.0f);

		for (size_t i = 0; i < items.size(); i++)
		{
			const SItem &it = items[i];
			const float norm = ClampF((it.baseArea - minArea) / areaRange, 0.0f, 1.0f);
			const float exp = expMin + (expMax - expMin) * norm;
			float si = powf(ClampF(sGlobal, 0.01f, 1.0f), exp);
			si = ClampF(si, 0.01f, 1.0f);

			float ow = it.baseOuterW;
			float oh = it.baseOuterH;
			if (!it.isDockRoot && it.baseInnerW > 1.0f && it.baseInnerH > 1.0f)
			{
				// Keep decorations constant (title/tab bars do not scale).
				const float iw = it.baseInnerW * si;
				const float ih = it.baseInnerH * si;
				ow = iw + it.decorW;
				oh = ih + it.decorH;
			}
			else
			{
				// Fallback: scale whole outer rect.
				ow = ow * si;
				oh = oh * si;
			}

			ow = std::max(minSize, ow);
			oh = std::max(minSize, oh);

			const float pw = ow + gap;
			const float ph = oh + gap;
			if (pw > containerW || ph > containerH)
				return false;

			out.w[i] = ow;
			out.h[i] = oh;
			packW[i] = pw;
			packH[i] = ph;
			packArea[i] = pw * ph;

			SPack p;
			p.idx = (int)i;
			p.pw = pw;
			p.ph = ph;
			p.area = pw * ph;
			p.maxDim = std::max(pw, ph);
			pack.push_back(p);
		}

		return true;
	};

	auto trySolveCompact = [&](float sGlobal, SSolution &out) -> bool
	{
		std::vector<SPack> pack;
		std::vector<float> packW;
		std::vector<float> packH;
		std::vector<float> packArea;
		if (!buildPack(sGlobal, out, pack, packW, packH, packArea))
			return false;

		auto runPackVariant = [&](bool sortByMaxDim, bool splitHorizontalFirst, float &outCost) -> bool
		{
			std::vector<SPack> order = pack;
			if (sortByMaxDim)
			{
				std::sort(order.begin(), order.end(), [](const SPack &a, const SPack &b)
				{
					if (a.maxDim != b.maxDim)
						return a.maxDim > b.maxDim;
					return a.area > b.area;
				});
			}
			else
			{
				std::sort(order.begin(), order.end(), [](const SPack &a, const SPack &b)
				{
					if (a.area != b.area)
						return a.area > b.area;
					return a.maxDim > b.maxDim;
				});
			}

			std::vector<SAutoLayoutRect> rects;
			rects.reserve(order.size());
			for (const SPack &p : order)
				rects.push_back({0.0f, 0.0f, p.pw, p.ph});

			std::vector<SAutoLayoutRect> placed;
			if (!PackGuillotine(rects, containerW, containerH, splitHorizontalFirst, placed))
				return false;

			float usedMaxX = 0.0f;
			float usedMaxY = 0.0f;
			for (size_t i = 0; i < placed.size(); i++)
			{
				const SPack &p = order[i];
				const SAutoLayoutRect &r = placed[i];
				out.x[p.idx] = r.x;
				out.y[p.idx] = r.y;
				usedMaxX = std::max(usedMaxX, r.x + r.w);
				usedMaxY = std::max(usedMaxY, r.y + r.h);
			}

			outCost = usedMaxX * usedMaxY;
			return true;
		};

		bool any = false;
		SSolution best = out;
		float bestCost = FLT_MAX;
		for (int sortMode = 0; sortMode < 2; sortMode++)
		{
			for (int splitMode = 0; splitMode < 2; splitMode++)
			{
				SSolution candidate = out;
				float cost = 0.0f;
				if (runPackVariant(sortMode == 1, splitMode == 1, cost))
				{
					candidate.x = out.x;
					candidate.y = out.y;
					any = true;
					if (cost < bestCost)
					{
						bestCost = cost;
						best = candidate;
					}
				}
			}
		}

		if (!any)
			return false;
		out = best;
		return true;
	};

	auto trySolvePreserve = [&](float sGlobal, SSolution &out) -> bool
	{
		std::vector<SPack> pack;
		std::vector<float> packW;
		std::vector<float> packH;
		std::vector<float> packArea;
		if (!buildPack(sGlobal, out, pack, packW, packH, packArea))
			return false;

		auto solveFromDesired = [&](const std::vector<float> &desiredX, const std::vector<float> &desiredY) -> bool
		{
			std::vector<float> x = desiredX;
			std::vector<float> y = desiredY;

			auto clampAll = [&]()
			{
				for (size_t i = 0; i < x.size(); i++)
				{
					x[i] = ClampF(x[i], 0.0f, std::max(0.0f, containerW - packW[i]));
					y[i] = ClampF(y[i], 0.0f, std::max(0.0f, containerH - packH[i]));
				}
			};

			auto isOverlapping = [&]() -> bool
			{
				for (size_t i = 0; i < x.size(); i++)
				{
					for (size_t j = i + 1; j < x.size(); j++)
					{
						const float ax0 = x[i];
						const float ay0 = y[i];
						const float ax1 = ax0 + packW[i];
						const float ay1 = ay0 + packH[i];
						const float bx0 = x[j];
						const float by0 = y[j];
						const float bx1 = bx0 + packW[j];
						const float by1 = by0 + packH[j];
						const float ox = std::min(ax1, bx1) - std::max(ax0, bx0);
						const float oy = std::min(ay1, by1) - std::max(ay0, by0);
						if (ox > 0.0f && oy > 0.0f)
							return true;
					}
				}
				return false;
			};

			clampAll();

			std::vector<float> prefCx;
			std::vector<float> prefCy;
			prefCx.reserve(x.size());
			prefCy.reserve(x.size());
			for (size_t i = 0; i < x.size(); i++)
			{
				prefCx.push_back(desiredX[i] + packW[i] * 0.5f);
				prefCy.push_back(desiredY[i] + packH[i] * 0.5f);
			}

			const int maxIter = 80;
			const float sepEps = 0.01f;
			for (int iter = 0; iter < maxIter; iter++)
			{
				bool hadOverlap = false;
				for (size_t i = 0; i < x.size(); i++)
				{
					for (size_t j = i + 1; j < x.size(); j++)
					{
						const float ax0 = x[i];
						const float ay0 = y[i];
						const float ax1 = ax0 + packW[i];
						const float ay1 = ay0 + packH[i];
						const float bx0 = x[j];
						const float by0 = y[j];
						const float bx1 = bx0 + packW[j];
						const float by1 = by0 + packH[j];
						float ox = std::min(ax1, bx1) - std::max(ax0, bx0);
						float oy = std::min(ay1, by1) - std::max(ay0, by0);
						if (!(ox > 0.0f && oy > 0.0f))
							continue;
						hadOverlap = true;

						auto separateX = [&]()
						{
							int left = (prefCx[i] <= prefCx[j]) ? (int)i : (int)j;
							int right = (left == (int)i) ? (int)j : (int)i;
							const float sumArea = packArea[left] + packArea[right] + 0.001f;
							float moveL = (ox + sepEps) * (packArea[left] / sumArea);
							float moveR = (ox + sepEps) * (packArea[right] / sumArea);

							const float maxL = x[left];
							const float maxR = (containerW - packW[right]) - x[right];
							float actualL = std::min(moveL, std::max(0.0f, maxL));
							float actualR = std::min(moveR, std::max(0.0f, maxR));
							float rem = (ox + sepEps) - (actualL + actualR);
							if (rem > 0.0f)
							{
								const float extraL = std::min(rem, std::max(0.0f, maxL - actualL));
								actualL += extraL;
								rem -= extraL;
								const float extraR = std::min(rem, std::max(0.0f, maxR - actualR));
								actualR += extraR;
								rem -= extraR;
							}
							x[left] -= actualL;
							x[right] += actualR;
						};

						auto separateY = [&]()
						{
							int top = (prefCy[i] <= prefCy[j]) ? (int)i : (int)j;
							int bottom = (top == (int)i) ? (int)j : (int)i;
							const float sumArea = packArea[top] + packArea[bottom] + 0.001f;
							float moveT = (oy + sepEps) * (packArea[top] / sumArea);
							float moveB = (oy + sepEps) * (packArea[bottom] / sumArea);

							const float maxT = y[top];
							const float maxB = (containerH - packH[bottom]) - y[bottom];
							float actualT = std::min(moveT, std::max(0.0f, maxT));
							float actualB = std::min(moveB, std::max(0.0f, maxB));
							float rem = (oy + sepEps) - (actualT + actualB);
							if (rem > 0.0f)
							{
								const float extraT = std::min(rem, std::max(0.0f, maxT - actualT));
								actualT += extraT;
								rem -= extraT;
								const float extraB = std::min(rem, std::max(0.0f, maxB - actualB));
								actualB += extraB;
								rem -= extraB;
							}
							y[top] -= actualT;
							y[bottom] += actualB;
						};

						if (ox < oy)
						{
							separateX();
							clampAll();
							const float nax0 = x[i];
							const float nay0 = y[i];
							const float nax1 = nax0 + packW[i];
							const float nay1 = nay0 + packH[i];
							const float nbx0 = x[j];
							const float nby0 = y[j];
							const float nbx1 = nbx0 + packW[j];
							const float nby1 = nby0 + packH[j];
							const float nox = std::min(nax1, nbx1) - std::max(nax0, nbx0);
							const float noy = std::min(nay1, nby1) - std::max(nay0, nby0);
							if (nox > 0.0f && noy > 0.0f)
								separateY();
						}
						else
						{
							separateY();
							clampAll();
							const float nax0 = x[i];
							const float nay0 = y[i];
							const float nax1 = nax0 + packW[i];
							const float nay1 = nay0 + packH[i];
							const float nbx0 = x[j];
							const float nby0 = y[j];
							const float nbx1 = nbx0 + packW[j];
							const float nby1 = nby0 + packH[j];
							const float nox = std::min(nax1, nbx1) - std::max(nax0, nbx0);
							const float noy = std::min(nay1, nby1) - std::max(nay0, nby0);
							if (nox > 0.0f && noy > 0.0f)
								separateX();
						}
					}
				}

				clampAll();
				if (!hadOverlap)
					break;
			}

			if (isOverlapping())
				return false;
			out.x = x;
			out.y = y;
			return true;
		};

		// Candidate A: keep current window centers (translate as a whole if it fits).
		std::vector<float> desiredX;
		std::vector<float> desiredY;
		desiredX.reserve(items.size());
		desiredY.reserve(items.size());
		for (size_t i = 0; i < items.size(); i++)
		{
			desiredX.push_back(targetCenterX[i] - out.w[i] * 0.5f);
			desiredY.push_back(targetCenterY[i] - out.h[i] * 0.5f);
		}

		float minX = FLT_MAX;
		float minY = FLT_MAX;
		float maxX = -FLT_MAX;
		float maxY = -FLT_MAX;
		for (size_t i = 0; i < items.size(); i++)
		{
			minX = std::min(minX, desiredX[i]);
			minY = std::min(minY, desiredY[i]);
			maxX = std::max(maxX, desiredX[i] + packW[i]);
			maxY = std::max(maxY, desiredY[i] + packH[i]);
		}
		const float layoutW = maxX - minX;
		const float layoutH = maxY - minY;
		float shiftX = 0.0f;
		float shiftY = 0.0f;
		if (layoutW <= containerW)
			shiftX = ClampF(0.0f, -minX, containerW - maxX);
		if (layoutH <= containerH)
			shiftY = ClampF(0.0f, -minY, containerH - maxY);
		for (size_t i = 0; i < items.size(); i++)
		{
			desiredX[i] += shiftX;
			desiredY[i] += shiftY;
		}
		if (solveFromDesired(desiredX, desiredY))
			return true;

		// Candidate B: normalize current centers into the container.
		float minCX = FLT_MAX;
		float minCY = FLT_MAX;
		float maxCX = -FLT_MAX;
		float maxCY = -FLT_MAX;
		for (size_t i = 0; i < items.size(); i++)
		{
			minCX = std::min(minCX, targetCenterX[i]);
			minCY = std::min(minCY, targetCenterY[i]);
			maxCX = std::max(maxCX, targetCenterX[i]);
			maxCY = std::max(maxCY, targetCenterY[i]);
		}
		const float rangeCX = std::max(1.0f, maxCX - minCX);
		const float rangeCY = std::max(1.0f, maxCY - minCY);
		desiredX.clear();
		desiredY.clear();
		for (size_t i = 0; i < items.size(); i++)
		{
			const float u = ClampF((targetCenterX[i] - minCX) / rangeCX, 0.0f, 1.0f);
			const float v = ClampF((targetCenterY[i] - minCY) / rangeCY, 0.0f, 1.0f);
			desiredX.push_back(u * std::max(0.0f, containerW - packW[i]));
			desiredY.push_back(v * std::max(0.0f, containerH - packH[i]));
		}
		return solveFromDesired(desiredX, desiredY);
	};

	auto solveBestScale = [&](auto &&solver, SSolution &outBest) -> bool
	{
		SSolution sol;
		if (solver(1.0f, sol))
		{
			outBest = sol;
			return true;
		}

		float lo = 0.05f;
		float hi = 1.0f;
		SSolution solLo;
		if (!solver(lo, solLo))
			return false;
		outBest = solLo;
		for (int iter = 0; iter < 22; iter++)
		{
			const float mid = (lo + hi) * 0.5f;
			SSolution solMid;
			if (solver(mid, solMid))
			{
				outBest = solMid;
				lo = mid;
			}
			else
			{
				hi = mid;
			}
		}
		return true;
	};

	SSolution best;
	bool solved = false;
	if (requestMode == AutoLayoutMode_Compact)
	{
		solved = solveBestScale(trySolveCompact, best);
	}
	else
	{
		solved = solveBestScale(trySolvePreserve, best);
		if (!solved)
		{
			// Fallback to compact pack if preserve solver fails.
			solved = solveBestScale(trySolveCompact, best);
		}
	}
	if (!solved)
		return false;

		const ImVec2 origin = viewport->WorkPos + ImVec2(margin, margin);
		for (const SItem &it : items)
		{
			const float x = floorf(origin.x + best.x[it.index] + 0.5f);
			const float y = floorf(origin.y + best.y[it.index] + 0.5f);
			const float w = best.w[it.index];
			const float h = best.h[it.index];

			if (it.isDockRoot)
			{
				ImGui::DockBuilderSetNodePos(it.dockRootId, ImVec2(x, y));
				ImGui::DockBuilderSetNodeSize(it.dockRootId, ImVec2(w, h));
			}
			else if (it.view)
			{
				it.view->SetNewImGuiWindowPositionAbsolute(x, y, viewport->ID);
				it.view->SetNewImGuiWindowSize(w, h);
			}
		}

		return true;
	};

	for (int i = 0; i < platformIO.Viewports.Size; i++)
	{
		if (layoutViewport(platformIO.Viewports[i]))
			anyApplied = true;
	}

	if (anyApplied)
		StoreLayoutInSettingsAtEndOfThisFrame();
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

	// Auto layout settings (engine)
	if (autoLayoutSettingsWindowVisible)
	{
		ImGui::SetNextWindowSize(ImVec2(480, 260), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Auto Layout Settings###MTEngineSDL.AutoLayoutSettings", &autoLayoutSettingsWindowVisible))
		{
			ImGui::TextUnformatted("Preserve/Compact: arranges visible floating windows and floating dock groups.");
			ImGui::TextUnformatted("Docked: tiles visible views in the main dockspace (per viewport).");
			ImGui::Separator();

			ImGui::DragFloat("Margin", &autoLayoutMargin, 1.0f, 0.0f, 256.0f, "%.0f px");
			if (ImGui::IsItemDeactivatedAfterEdit() && gApplicationDefaultConfig)
			{
				autoLayoutMargin = ClampF(autoLayoutMargin, 0.0f, 256.0f);
				gApplicationDefaultConfig->SetFloat("uiAutoLayoutMargin", &autoLayoutMargin);
			}

			ImGui::DragFloat("Gap", &autoLayoutGap, 1.0f, 0.0f, 256.0f, "%.0f px");
			if (ImGui::IsItemDeactivatedAfterEdit() && gApplicationDefaultConfig)
			{
				autoLayoutGap = ClampF(autoLayoutGap, 0.0f, 256.0f);
				gApplicationDefaultConfig->SetFloat("uiAutoLayoutGap", &autoLayoutGap);
			}
			ImGui::DragFloat("Min Window Size", &autoLayoutMinWindowSize, 1.0f, 8.0f, 2048.0f, "%.0f px");
			ImGui::DragFloat("Shrink Exponent (Small)", &autoLayoutShrinkExpMin, 0.01f, 0.10f, 3.0f, "%.2f");
			ImGui::DragFloat("Shrink Exponent (Large)", &autoLayoutShrinkExpMax, 0.01f, 0.10f, 3.0f, "%.2f");
			if (autoLayoutShrinkExpMax < autoLayoutShrinkExpMin)
				autoLayoutShrinkExpMax = autoLayoutShrinkExpMin;

			ImGui::Separator();

			if (ImGui::Button("Run Auto Layout"))
			{
				RequestAutoLayoutVisibleViews();
			}
			ImGui::SameLine();
			if (ImGui::Button("Run Compact Auto Layout"))
			{
				RequestAutoLayoutVisibleViewsCompact();
			}
			ImGui::SameLine();
			if (ImGui::Button("Dock to Main Dockspace"))
			{
				RequestAutoLayoutVisibleViewsDocked();
			}
			ImGui::SameLine();
			if (ImGui::Button("Dock Preserve Scan"))
			{
				RequestAutoLayoutVisibleViewsDockedPreserveScan();
			}

			if (ImGui::Button("Save"))
			{
				if (SaveAutoLayoutSettingsToConfig())
					ShowNotification("Auto Layout", "Settings saved");
				else
					ShowNotificationError("Auto Layout", "Failed to save settings");
			}
			ImGui::SameLine();
			if (ImGui::Button("Defaults"))
			{
				ResetAutoLayoutSettingsToDefaults();
			}
			ImGui::SameLine();
			ImGui::Text("Config: %s", APPLICATION_DEFAULT_CONFIG_HJSON_FILE_PATH);
		}
		ImGui::End();
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
			const bool enterPressed = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
			if (ImGui::Button("  OK  ") || enterPressed)
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
	
	if (layoutStoreAfterFrameDelay > 0)
	{
		layoutStoreAfterFrameDelay--;
		if (layoutStoreAfterFrameDelay == 0)
			layoutStoreCurrentInSettings = true;
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
			}

			this->DeserializeLayout(layoutForThisFrame);

			layoutManager->currentLayout = layoutForThisFrame;
			layoutForThisFrame = NULL;
			layoutJustRestored = true;

			// always persist after layout switch so currentLayoutName is up to date
			layoutManager->StoreLayouts();
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

void CGlobalLayoutCallback::GlobalLayoutWillDeserialize(CLayoutData *layout) {
}

void CGuiMain::ClearGlobalLayoutCallbacks() {
	this->globalLayoutCallbacks.clear();
}

void CGuiMain::AddGlobalLayoutCallback(CGlobalLayoutCallback *callback) {
	for (std::list<CGlobalLayoutCallback *>::iterator it =
			this->globalLayoutCallbacks.begin();
			it != this->globalLayoutCallbacks.end(); it++) {
		CGlobalLayoutCallback *val = (*it);
		if (val == callback) {
			LOGWarning("AddGlobalLayoutCallback: double callback");
			return;
		}
	}
	this->globalLayoutCallbacks.push_back(callback);
}

void CGuiMain::RemoveGlobalLayoutCallback(CGlobalLayoutCallback *callback) {
	this->globalLayoutCallbacks.remove(callback);
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
