#include "CGuiMain.h"
#include "SYS_Defs.h"
#include "DBG_Log.h"
#include "CGuiViewMovingPane.h"
#include "SYS_KeyCodes.h"
#include "VID_ImageBinding.h"

// TODO: add post-render to support mouse cursor change, we need to change cursor every frame when being moved from click-down to click-up
//if (changeCursorOnMove)
//{
//	ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
//}


CGuiViewMovingPane::CGuiViewMovingPane(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY, float paneWidth, float paneHeight)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	this->paneWidth = paneWidth;
	this->paneHeight = paneHeight;

	isForcedMovingMap = false;

	SetMovingPaneStyle(MovingPaneStyle_RightClick);
//	changeCursorOnMove = true;
	mouseInvert = false;

	cursorInside = false;
	zoomCursorX = zoomCursorY = 0.5f;
		
	minZoom = 1.0f;
	maxZoom = 60.0f;
	ClearZoom();
}

void CGuiViewMovingPane::SetMovingPaneStyle(MovingPaneStyle movingPaneStyle)
{
	this->movingPaneStyle = movingPaneStyle;
}

CGuiViewMovingPane::~CGuiViewMovingPane()
{
	
}

void CGuiViewMovingPane::SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY)
{
//	LOGD("CGuiViewMovingPane::SetPosition");
	CGuiView::SetPosition(posX, posY, posZ, sizeX, sizeY);
	
	UpdatePositionAndZoom();
}

bool CGuiViewMovingPane::DoScrollWheel(float deltaX, float deltaY)
{
	LOGG("CGuiViewMovingPane::DoScrollWheel: %5.2f %5.2f", deltaX, deltaY);

	if (movingPaneStyle == MovingPaneStyle_MultiTouch)
	{
		MoveMap(deltaX, deltaY);
	}
	else
	{
		if (cursorInside == false)
		{
			return false;
		}
		
		if (mouseInvert)
		{
			deltaY = -deltaY;
		}
		
		currentZoom = currentZoom + deltaY / 5.0f * cellSizeX * 0.1f;
		
		if (currentZoom < minZoom)
			currentZoom = minZoom;
		
		if (currentZoom > maxZoom)
			currentZoom = maxZoom;
		
		this->ZoomMap(currentZoom);
		
		return true;
	}
	
	return true;
}

bool CGuiViewMovingPane::InitZoom()
{
//	LOGD("CGuiViewMovingPane::InitZoom");
	return true;
}

bool CGuiViewMovingPane::DoZoomBy(float x, float y, float zoomValue, float difference)
{
	LOGD("CGuiViewMovingPane::DoZoomBy: x=%5.2f y=%5.2f zoomValue=%5.2f diff=%5.2f", x, y, zoomValue, difference);

	if (movingPaneStyle == MovingPaneStyle_MultiTouch)
	{
		if (cursorInside == false)
			return false;
		
//		if (mouseInvert)
//		{
//			difference = -difference;
//		}
		
		currentZoom = currentZoom + difference / 5.0f;
		
		if (currentZoom < minZoom)
			currentZoom = minZoom;
		
		if (currentZoom > maxZoom)
			currentZoom = maxZoom;
		
		this->ZoomMap(currentZoom);
		
		return true;
	}
	
	return false;
}

bool CGuiViewMovingPane::DoTap(float x, float y)
{
	if (movingPaneStyle == MovingPaneStyle_LeftClick)
	{
		if (IsInside(x, y))
		{
			isForcedMovingMap = true;
			prevMousePosX = guiMain->mousePosX;
			prevMousePosY = guiMain->mousePosY;
//			if (changeCursorOnMove)
//			{
//				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
//			}
			return true;
		}
	}
	
	return false;
}

bool CGuiViewMovingPane::DoFinishTap(float x, float y)
{
	if (movingPaneStyle == MovingPaneStyle_LeftClick)
	{
		isForcedMovingMap = false;
//		if (changeCursorOnMove)
//		{
//			ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
//		}
		
		if (IsInside(x, y))
		{
			return true;
		}
	}
	
	return false;
}

bool CGuiViewMovingPane::DoRightClick(float x, float y)
{
//	LOGG("CGuiViewMovingPane::DoRightClick");

	if (movingPaneStyle == MovingPaneStyle_RightClick)
	{
		if (IsInside(x, y))
		{
			isForcedMovingMap = true;
			prevMousePosX = guiMain->mousePosX;
			prevMousePosY = guiMain->mousePosY;
//			if (changeCursorOnMove)
//			{
//				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
//			}
			return true;
		}
	}
	
	return false;
}

bool CGuiViewMovingPane::DoFinishRightClick(float x, float y)
{
	if (movingPaneStyle == MovingPaneStyle_RightClick)
	{
		if (IsInside(x, y))
		{
			isForcedMovingMap = false;
//			if (changeCursorOnMove)
//			{
//				ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
//			}
			return true;
		}
	}
	
	return false;
}

bool CGuiViewMovingPane::DoNotTouchedMove(float x, float y)
{
	LOGG("CGuiViewMovingPane::DoNotTouchedMove: x=%f y=%f", x, y);
	
	if (isForcedMovingMap)
	{
		float dx = -(prevMousePosX - x);
		float dy = -(prevMousePosY - y);
		
		MoveMap(dx / sizeX * mapSizeX * 5.0f, dy / sizeY * mapSizeY * 5.0f);
		
		prevMousePosX = x;
		prevMousePosY = y;
		
		return true;
	}
	
	if (IsInside(x, y) == false)
	{
		cursorInside = false;
	}
	
	// change x, y into position within memory map area (cursorX, cursorY)
	
	float px = (x - posX) / sizeX;
	float py = (y - posY) / sizeY;

	px = ImClamp(px, 0.0f, 1.0f);
	py = ImClamp(py, 0.0f, 1.0f);
	
	zoomCursorX = px;
	zoomCursorY = py;
	
	cursorInside = true;
	
	return false;
}

void CGuiViewMovingPane::ClearZoom()
{
	currentZoom = 1.0f;
	mapPosX = 0.0f;
	mapPosY = 0.0f;
	mapSizeX = 1.0f;
	mapSizeY = 1.0f;
	UpdatePositionAndZoom();
}

void CGuiViewMovingPane::ZoomMap(float zoom)
{
//	LOGD("CGuiViewMovingPane::ZoomMap: %f", zoom);
	
	if (zoom < minZoom)
	{
		ClearZoom();
		return;
	}
	
//	LOGD("cursorX=%f cursorY=%f", cursorX, cursorY);

	float fcx = (-mapPosX + zoomCursorX) / mapSizeX;
//	LOGD("fcx=%f", fcx);
	
	mapSizeX = 1.0f*zoom;
	mapPosX = -fcx*mapSizeX + zoomCursorX;
	
	float fcy = (-mapPosY + zoomCursorY) / mapSizeY;
//	LOGD("fcy=%f", fcy);

	mapSizeY = 1.0f*zoom;
	mapPosY = -fcy*mapSizeY + zoomCursorY;

	UpdatePositionAndZoom();
}

void CGuiViewMovingPane::MoveMap(float diffX, float diffY)
{
	mapPosX += diffX*0.2f / mapSizeX;
	mapPosY += diffY*0.2f / mapSizeY;
	
	UpdatePositionAndZoom();
}

void CGuiViewMovingPane::UpdatePositionAndZoom()
{
	// convert (0.0, 1.0) to screen pos/size
	
//	if (mapPosX > 0.0f)
//		mapPosX = 0.0f;
//	
//	if (mapPosY > 0.0f)
//		mapPosY = 0.0f;
//	
//	if (mapPosX - 1.0f < -mapSizeX)
//		mapPosX = -mapSizeX+1.0f;
//	
//	if (mapPosY - 1.0f < -mapSizeY)
//		mapPosY = -mapSizeY+1.0f;
	
	
	guiMain->LockMutex(); //"CViewMemoryMap::UpdateMapPosition");
	
	float sx = sizeX;
	float sy = sizeY;

	renderMapPosX = mapPosX * sx + posX;
	renderMapPosY = mapPosY * sy + posY;
	
	renderMapSizeX = mapSizeX * sx;
	renderMapSizeY = mapSizeY * sy;
	
	if (mapSizeX > 8.0f)
	{
		renderMapValues = true;
	}
	else
	{
		renderMapValues = false;
	}

	float iw = (float)paneWidth;
	float ih = (float)paneHeight;
	
	float fsx = (mapSizeX / iw);
	float fsy = (mapSizeY / ih);
	
//	LOGD("msx=%f msy=%f", mapSizeX, mapSizeY);
//	LOGD("iw=%f ih=%f fsx=%f fsy=%f", iw, ih, fsx, fsy);
	
	cellSizeX = fsx * sx;
	cellSizeY = fsy * sy;
	
//	LOGD("cellSizeX=%f y=%f", cellSizeX, cellSizeY);
	
	float cx = floor( (-mapPosX) / fsx);
	float cy = floor( (-mapPosY) / fsy);
	
	cellStartX = cellSizeX * cx + renderMapPosX;
	cellStartY = cellSizeY * cy + renderMapPosY;
	cellStartIndex = cy * iw + cx;
	cellStartIndexX = cx;
	cellStartIndexY = cy;
	
	float nx = ceil(sx / cellSizeX);
	float ny = ceil(sy / cellSizeY);
	numCellsInWidth = (int)(nx+1);
	numCellsInHeight = (int)(ny+1);
	
	////
	guiMain->UnlockMutex(); //"CGuiViewMovingPane::UpdateMapPosition");
	
//	LOGD("UpdateMapPosition: mapSize=%5.2f %5.2f", mapSizeX, mapSizeY);
}

void CGuiViewMovingPane::RenderMovingPane()
{
}

void CGuiViewMovingPane::RenderImGui()
{
	PreRenderImGui();
	
//	frameCounter++;
		
	float gap = 1.0f;
	
	BlitRectangle(posX-gap, posY-gap, -1, sizeX+gap*2, sizeY+gap*2, 0.3, 0.3, 0.3, 0.7f);
	
	// blit
	
	VID_SetClipping(posX, posY, sizeX, sizeY);
		
	//	LOGD("renderTextureStartX=%f renderTextureEndX=%f renderTextureStartY=%f renderTextureEndY=%f",
	//		 renderTextureStartX, renderTextureEndX, renderTextureStartY, renderTextureEndY);

//	if (image->isBound)
//	{
//		Blit(image, renderMapPosX, renderMapPosY, -1, renderMapSizeX, renderMapSizeY,
//			 renderTextureStartX,
//			 renderTextureStartY,
//			 renderTextureEndX,
//			 renderTextureEndY);
//	}
	
	RenderMovingPane();

	VID_ResetClipping();
	
	//	// debug cursor
	//	float px = cursorX * sizeX + posX;
	//	float py = cursorY * sizeY + posY;
	//	BlitPlus(px, py, posZ, 15, 15, 1, 0, 0, 1);

	PostRenderImGui();
}

void CGuiViewMovingPane::ScreenPosToPanePos(float screenX, float screenY, int *imageX, int *imageY)
{
	float px = (screenX - posX) - (cellStartX - posX);
	float py = (screenY - posY) - (cellStartY - posY);
	
	*imageX = floor((px / cellSizeX) + cellStartIndexX);
	*imageY = floor((py / cellSizeY) + cellStartIndexY);
}

//bool CGuiViewMovingPane::DoTap(float x, float y)
//{
//	LOGD("CGuiViewMovingPane::DoTap: x=%f y=%f", x, y);
	
	/*
	float cx, cy;
	
	cy = cellStartY;
	int lineAddr = cellStartIndex;
	for (int iy = 0; iy < numCellsInHeight; iy++)
	{
		cx = cellStartX;
		int addr = lineAddr;
		for (int ix = 0; ix < numCellsInWidth; ix++)
		{
			if (cx <= x && x <= cx+cellSizeX
				&& cy <= y && y <= cy+cellSizeY)
			{
				if (addr < 0)
				{
					addr = 0;
				}
				else if (addr > 0xFFFF)
				{
					addr = 0xFFFF;
				}
				
				///
				if (addr == previousClickAddr)
				{
					unsigned long time = SYS_GetCurrentTimeInMillis() - previousClickTime;
					if (time < c64SettingsDoubleClickMS)
					{
						// double click
						viewDataDump->viewDisassembly->ScrollToAddress(addr);
					}
				}
				
				previousClickTime = SYS_GetCurrentTimeInMillis();
				previousClickAddr = addr;
				
				if (guiMain->isControlPressed)
				{
					CViewMemoryMapCell *cell = this->memoryCells[addr];
					
					if (guiMain->isShiftPressed)
					{
						if (cell->readPC != -1)
						{
							viewDataDump->viewDisassembly->ScrollToAddress(cell->readPC);
						}
						
						if (guiMain->isAltPressed)
						{
							if (debugInterface->snapshotsManager)
							{
								LOGM("============######################### RESTORE TO READ CYCLE=%d", cell->readCycle);
								debugInterface->snapshotsManager->RestoreSnapshotByCycle(cell->readCycle);
							}
						}
					}
					else
					{
						if (cell->writePC != -1)
						{
							viewDataDump->viewDisassembly->ScrollToAddress(cell->writePC);
						}
						
						if (guiMain->isAltPressed)
						{
							if (debugInterface->snapshotsManager)
							{
								LOGM("============######################### RESTORE TO WRITE CYCLE=%d", cell->writeCycle);
								debugInterface->snapshotsManager->RestoreSnapshotByCycle(cell->writeCycle);
							}
						}
					}
				}

				viewDataDump->ScrollToAddress(addr);
				
				return true;
			}
			
			addr++;
			cx += cellSizeX;
		}
		
		lineAddr += imageWidth;
		
		if (lineAddr > ramSize)
			break;
		
		cy += cellSizeY;
	}
	*/
	
//	return false;
//}

bool CGuiViewMovingPane::KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	if (keyCode == MTKEY_SPACEBAR)
	{
		isForcedMovingMap = true;
		prevMousePosX = guiMain->mousePosX;
		prevMousePosY = guiMain->mousePosY;
		return true;
	}
	
	return false;
}

bool CGuiViewMovingPane::KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	if (keyCode == MTKEY_SPACEBAR && !isShift && !isAlt && !isControl)
	{
		isForcedMovingMap = false;
		return true;
	}
	
	return false;
}

bool CGuiViewMovingPane::KeyDownOnMouseHover(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	if (keyCode == MTKEY_SPACEBAR && !isShift && !isAlt && !isControl)
	{
		if (this->KeyDown(keyCode, isShift, isAlt, isControl, isSuper))
				return true;
	}
	
	return false;
}

bool CGuiViewMovingPane::KeyUpGlobal(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper)
{
	if (keyCode == MTKEY_SPACEBAR && !isShift && !isAlt && !isControl)
	{
		this->KeyUp(keyCode, isShift, isAlt, isControl, isSuper);
	}
	
	return false;
}

