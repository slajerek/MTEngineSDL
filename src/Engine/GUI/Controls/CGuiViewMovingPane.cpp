#include "CGuiMain.h"
#include "SYS_Defs.h"
#include "DBG_Log.h"
#include "CGuiViewMovingPane.h"
#include "SYS_KeyCodes.h"
#include "VID_ImageBinding.h"

CGuiViewMovingPane::CGuiViewMovingPane(float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiView(posX, posY, posZ, sizeX, sizeY)
{
	this->name = "CGuiViewMovingPane";

	this->isBeingMoved = false;
	this->isForcedMovingMap = false;

	this->font = NULL;
	this->fontScale = 0.11f;
	
	//
	shouldDeallocImage = false;
	
	useMultiTouch = false;
	mouseInvert = false;

	cursorInside = false;
	cursorX = cursorY = 0.5f;
	
	imageWidth = 0;
	imageHeight = 0;
	rasterWidth = 0;
	rasterHeight = 0;
	
	imageData = NULL;
	image = NULL;

	imageChanged = false;
	
	ClearZoom();
	
	// derived constructor should load image and call InitImage()
}

void CGuiViewMovingPane::InitImage()
{
	imageData = NULL;
	
	CreateImageData();
	
	if (imageData == NULL)
		return;

	shouldDeallocImage = true;
	image = new CSlrImage(true, false);
	image->LoadImageForRebinding(imageData, RESOURCE_PRIORITY_STATIC);
	VID_PostImageBinding(image, NULL, BINDING_MODE_DONT_FREE_IMAGEDATA);

	renderTextureStartX = 0.0f;
	renderTextureEndX = ((float)imageWidth / (float)rasterWidth);
	renderTextureStartY = 0.0f;
	renderTextureEndY = ((float)imageHeight / (float)rasterHeight);
}

void CGuiViewMovingPane::CreateImageData()
{
	imageData = NULL;
//	CreateEmptyImageData(256, 256);
}

bool CGuiViewMovingPane::UpdateImageData()
{
	// this is virtual method to fill changed image data, returns true when image was changed.
	return imageChanged;
}

void CGuiViewMovingPane::CreateEmptyImageData(int imageWidth, int imageHeight)
{
	// images
	this->imageWidth = imageWidth;
	this->imageHeight = imageHeight;
	
	rasterWidth = NextPow2(imageWidth);
	rasterHeight = NextPow2(imageHeight);
	imageData = new CImageData(rasterWidth, rasterHeight, IMG_TYPE_RGBA);
	imageData->AllocImage(false, true);
}

CGuiViewMovingPane::~CGuiViewMovingPane()
{
	
}

void CGuiViewMovingPane::SetImageData(CImageData *imageIn)
{
	guiMain->LockMutex();
	
	if (this->image && shouldDeallocImage)
	{
		VID_PostImageDealloc(this->image);
	}
	this->image = NULL;
	
	if (this->imageData)
		delete this->imageData;
	this->imageData = NULL;
	
	if (imageIn == NULL)
	{
		guiMain->UnlockMutex();
		return;
	}

	this->imageWidth = imageIn->width;
	this->imageHeight = imageIn->height;
	
	rasterWidth = NextPow2(imageWidth);
	rasterHeight = NextPow2(imageHeight);
	imageData = new CImageData(rasterWidth, rasterHeight, IMG_TYPE_RGBA);
	imageData->AllocImage(false, true);
	
	for (int x = 0; x < imageWidth; x++)
	{
		for (int y = 0; y < imageHeight; y++)
		{
			u8 r,g,b,a;
			imageIn->GetPixel(x, y, &r, &g, &b, &a);
			imageData->SetPixel(x, y, r, g, b, a);
		}
	}

	shouldDeallocImage = true;
	image = new CSlrImage(true, false);
	image->LoadImageForRebinding(imageData, RESOURCE_PRIORITY_STATIC);
	
	VID_PostImageBinding(image, NULL, BINDING_MODE_DONT_FREE_IMAGEDATA);

	renderTextureStartX = 0.0f;
	renderTextureEndX = ((float)imageWidth / (float)rasterWidth);
	renderTextureStartY = 0.0f;
	renderTextureEndY = ((float)imageHeight / (float)rasterHeight);

	ClearZoom();
	
	guiMain->UnlockMutex();
}

void CGuiViewMovingPane::SetImage(CSlrImage *setImage)
{
	guiMain->LockMutex();
	if (shouldDeallocImage && this->image)
	{
		VID_PostImageDealloc(this->image);
	}

	// we are not owner of this image
	shouldDeallocImage = false;
	this->image = setImage;
	
	imageWidth = image->width;
	imageHeight = image->height;
	rasterWidth = image->rasterWidth;
	rasterHeight = image->rasterHeight;

	renderTextureStartX = 0.0f;
	renderTextureEndX = ((float)imageWidth / (float)rasterWidth);
	renderTextureStartY = 0.0f;
	renderTextureEndY = ((float)imageHeight / (float)rasterHeight);

	guiMain->UnlockMutex();
}

void CGuiViewMovingPane::SetPosition(float posX, float posY, float posZ, float sizeX, float sizeY)
{
//	LOGD("CGuiViewMovingPane::SetPosition");
	
	CGuiView::SetPosition(posX, posY, posZ, sizeX, sizeY);
	
	UpdateMapPosition();
}

void CGuiViewMovingPane::DoLogic()
{
	////
}

bool CGuiViewMovingPane::DoScrollWheel(float deltaX, float deltaY)
{
	LOGG("CGuiViewMovingPane::DoScrollWheel: %5.2f %5.2f", deltaX, deltaY);

	if (useMultiTouch)
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
		
		if (currentZoom < 1.0f)
			currentZoom = 1.0f;
		
		if (currentZoom > 60.0f)
			currentZoom = 60.0f;
		
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

	if (useMultiTouch)
	{
		if (cursorInside == false)
			return false;
		
//		if (mouseInvert)
//		{
//			difference = -difference;
//		}
		
		currentZoom = currentZoom + difference / 5.0f;
		
		if (currentZoom < 1.0f)
			currentZoom = 1.0f;
		
		if (currentZoom > 60.0f)
			currentZoom = 60.0f;
		
		this->ZoomMap(currentZoom);
		
		return true;
	}
	
	return false;
}

bool CGuiViewMovingPane::DoMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
//	LOGD("CGuiViewMovingPane::DoMove: x=%5.2f y=%5.2f distX=%5.2f distY=%5.2f diffX=%5.2f diffY=%5.2f",
//		 x, y, distX, distY, diffX, diffY);
	
	return true;
}

bool CGuiViewMovingPane::FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
//	LOGD("CGuiViewMovingPane::DoMove: x=%5.2f y=%5.2f distX=%5.2f distY=%5.2f accelerationX=%5.2f accelerationY=%5.2f",
//		 x, y, distX, distY, accelerationX, accelerationY);

	return true;
}

bool CGuiViewMovingPane::DoRightClick(float x, float y)
{
	if (useMultiTouch == false)
	{
		if (IsInside(x, y))
		{
			this->accelerateX = 0.0f;
			this->accelerateY = 0.0f;
			isBeingMoved = true;
			return true;
		}
	}
	
	return false;
}

bool CGuiViewMovingPane::DoRightClickMove(float x, float y, float distX, float distY, float diffX, float diffY)
{
//	LOGD("CGuiViewMovingPane::DoRightClickMove");
	
	if (useMultiTouch == false)
	{
		this->accelerateX = 0.0f;
		this->accelerateY = 0.0f;
		MoveMap(diffX / sizeX * mapSizeX * 5.0f, diffY / sizeY * mapSizeY * 5.0f);
		return true;
	}
	
	return false;
}

bool CGuiViewMovingPane::FinishRightClickMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY)
{
	if (useMultiTouch == false && isBeingMoved)
	{
//		this->accelerateX = accelerationX / 10.0f;
//		this->accelerateY = accelerationY / 10.0f;
		isBeingMoved = false;
		return true;
	}
	
	return false;
}


bool CGuiViewMovingPane::DoNotTouchedMove(float x, float y)
{
//	LOGG("CGuiViewMovingPane::DoNotTouchedMove: x=%f y=%f", x, y);
	
	if (isForcedMovingMap)
	{
		float dx = -(prevMousePosX - x);
		float dy = -(prevMousePosY - y);
		
		MoveMap(dx / sizeX * mapSizeX * 5.0f, dy / sizeY * mapSizeY * 5.0f);
		
		prevMousePosX = x;
		prevMousePosY = y;

		this->accelerateX = 0.0f;
		this->accelerateY = 0.0f;
		return true;
	}
	
	// change x, y into position within memory map area (cursorX, cursorY)
	
	float px = (x - posX) / sizeX;
	float py = (y - posY) / sizeY;
	
	if (px < 0.0f
		|| px > 1.0f
		|| py < 0.0f
		|| py > 1.0f)
	{
		cursorInside = false;
		return false;
	}
	
	cursorX = px;
	cursorY = py;
	
	cursorInside = true;
	
	return false;
}

void CGuiViewMovingPane::ClearZoom()
{
	currentZoom = 1.0f;
//	textureStartX = textureStartY = 0.0f;
//	textureEndX = textureEndY = 1.0f;
//	UpdateTexturePosition(textureStartX, textureStartY, textureEndX, textureEndY);
	mapPosX = 0.0f;
	mapPosY = 0.0f;
	mapSizeX = 1.0f;
	mapSizeY = 1.0f;
	UpdateMapPosition();
}

void CGuiViewMovingPane::ZoomMap(float zoom)
{
//	LOGD("CGuiViewMovingPane::ZoomMap: %f", zoom);
	
	if (zoom < 1.0f)
	{
		ClearZoom();
		return;
	}
	
//	LOGD("cursorX=%f cursorY=%f", cursorX, cursorY);

	float fcx = (-mapPosX + cursorX) / mapSizeX;
//	LOGD("fcx=%f", fcx);
	
	mapSizeX = 1.0f*zoom;
	mapPosX = -fcx*mapSizeX + cursorX;
	
	float fcy = (-mapPosY + cursorY) / mapSizeY;
//	LOGD("fcy=%f", fcy);

	mapSizeY = 1.0f*zoom;
	mapPosY = -fcy*mapSizeY + cursorY;

	UpdateMapPosition();
}

void CGuiViewMovingPane::MoveMap(float diffX, float diffY)
{
	mapPosX += diffX*0.2f / mapSizeX;
	mapPosY += diffY*0.2f / mapSizeY;
	
	UpdateMapPosition();
}

void CGuiViewMovingPane::UpdateMapPosition()
{
	// convert (0.0, 1.0) to screen pos/size
	
	if (mapPosX > 0.0f)
		mapPosX = 0.0f;
	
	if (mapPosY > 0.0f)
		mapPosY = 0.0f;
	
	if (mapPosX - 1.0f < -mapSizeX)
		mapPosX = -mapSizeX+1.0f;
	
	if (mapPosY - 1.0f < -mapSizeY)
		mapPosY = -mapSizeY+1.0f;
	
	
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

	float iw = (float)imageWidth;
	float ih = (float)imageHeight;
	
	float fsx = (mapSizeX / iw);
	float fsy = (mapSizeY / ih);
	
	cellSizeX = fsx * sx;
	cellSizeY = fsy * sy;
	
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
	currentFontDataScale = fontScale * cellSizeX;
	
	guiMain->UnlockMutex(); //"CGuiViewMovingPane::UpdateMapPosition");
	
//	LOGD("UpdateMapPosition: mapSize=%5.2f %5.2f", mapSizeX, mapSizeY);
}

void CGuiViewMovingPane::PostRenderMovingPaneCustom()
{
}

void CGuiViewMovingPane::RenderImGui()
{
	if (image == NULL)
		return;
	
	PreRenderImGui();
	
	bool renderMapValuesInThisFrame = renderMapValues;
	
//	frameCounter++;
		
	float gap = 1.0f;
	
	BlitRectangle(posX-gap, posY-gap, -1, sizeX+gap*2, sizeY+gap*2, 0.3, 0.3, 0.3, 0.7f);
	
	// blit
	
	VID_SetClipping(posX, posY, sizeX, sizeY);
	
	
	//	LOGD("renderTextureStartX=%f renderTextureEndX=%f renderTextureStartY=%f renderTextureEndY=%f",
	//		 renderTextureStartX, renderTextureEndX, renderTextureStartY, renderTextureEndY);

	if (UpdateImageData())
	{
		image->ReBindImage();
	}

	if (image->isBound)
	{
		Blit(image, renderMapPosX, renderMapPosY, -1, renderMapSizeX, renderMapSizeY,
			 renderTextureStartX,
			 renderTextureStartY,
			 renderTextureEndX,
			 renderTextureEndY);
	}
	
	PostRenderMovingPaneCustom();

	//
	
	
	/*
	if (renderMapValuesInThisFrame)
	{
		float cx, cy;
		
		char buf[128];
		
		cy = cellStartY;
		int lineAddr = cellStartIndex;
		for (int iy = 0; iy < numCellsInHeight; iy++)
		{
			cx = cellStartX;
			int addr = lineAddr;
			for (int ix = 0; ix < numCellsInWidth; ix++)
			{
				if (addr > ramSize)
					continue;
				
				CViewMemoryMapCell *cell = memoryCells[addr];
				
				float z = 1.0f - (cell->sr + cell->sg + cell->sb)/2.5f;
				
				float tcr = z; //1.0f - cell->sr;
				float tcg = z; //1.0f - cell->sg;
				float tcb = z; //1.0f - cell->sb;
				
				
				
				float px = cx + textAddrGapX;
				float py = cy + textAddrGapY;
				sprintf(buf, "%04X", addr);
				font->BlitTextColor(buf, px, py, posZ, currentFontAddrScale, tcr, tcg, tcb, 1.0f);
				
				px = cx + textDataGapX;
				py = cy + textDataGapY;
				
				uint8 val = memoryBuffer[addr];
				sprintf(buf, "%02X", val);
				font->BlitTextColor(buf, px, py, posZ, currentFontDataScale, tcr, tcg, tcb, 1.0f);
				
				px = cx + textCodeCenterX;
				py = cy + textCodeGapY;
				
				if (cell->isExecuteCode)
				{
					int mode = opcodes[val].addressingMode;
					const char *name = opcodes[val].name;
					
					buf[0] = name[0];
					buf[1] = name[1];
					buf[2] = name[2];
					
					char *buf3 = buf+3;
					
					switch(mode)
					{
						default:
						case ADDR_IMP:
							*buf3 = 0x00;
							px -= textCodeWidth3h;
							break;
						case ADDR_IMM:
							strcpy(buf3, " #..");
							px -= textCodeWidth7h;
							break;
						case ADDR_ZP:
							strcpy(buf3, " ..");
							px -= textCodeWidth6h;
							break;
						case ADDR_ZPX:
							strcpy(buf3, " ..,X");
							px -= textCodeWidth8h;
							break;
						case ADDR_ZPY:
							strcpy(buf3, " ..,Y");
							px -= textCodeWidth8h;
							break;
						case ADDR_IZX:
							strcpy(buf3, " (..,X)");
							px -= textCodeWidth10h;
							break;
						case ADDR_IZY:
							strcpy(buf3, " (..),Y");
							px -= textCodeWidth10h;
							break;
						case ADDR_ABS:
							strcpy(buf3, " ....");
							px -= textCodeWidth8h;
							break;
						case ADDR_ABX:
							strcpy(buf3, " ....,X");
							px -= textCodeWidth10h;
							break;
						case ADDR_ABY:
							strcpy(buf3, " ....,Y");
							px -= textCodeWidth10h;
							break;
						case ADDR_IND:
							strcpy(buf3, " (....)");
							px -= textCodeWidth10h;
							break;
						case ADDR_REL:
							strcpy(buf3, " ....");
							px -= textCodeWidth8h;
							break;
					}
					font->BlitTextColor(buf, px, py, posZ, currentFontCodeScale, tcr, tcg, tcb, 1.0f);
				}
				else
				{
					HexDigitToBinary(((val >> 4) & 0x0F), buf);
					buf[4] = ' ';
					HexDigitToBinary((val & 0x0F), buf + 5);
					buf[9] = 0x00;
					font->BlitTextColor(buf, px - textCodeWidth9h, py, posZ, currentFontCodeScale, tcr, tcg, tcb, 1.0f);
				}
				
				addr++;
				cx += cellSizeX;
			}
			
			lineAddr += imageWidth;
			
			if (lineAddr > ramSize)
				break;
			
			cy += cellSizeY;
		}
		
		// paint grid
		cy = cellStartY;
		for (int iy = 0; iy < numCellsInHeight; iy++)
		{
			BlitLine(posX, cy, posEndX, cy, posZ, 1.0f, 0.0f, 1.0f, 0.9f);
			cy += cellSizeY;
		}
		
		cx = cellStartX;
		for (int ix = 0; ix < numCellsInWidth; ix++)
		{
			BlitLine(cx, posY, cx, posEndY, posZ, 1.0f, 0.0f, 0.0f, 0.9f);
			cx += cellSizeX;
		}
	}
		*/
	
	//
	
	
	VID_ResetClipping();
	
	//	// debug cursor
	//	float px = cursorX * sizeX + posX;
	//	float py = cursorY * sizeY + posY;
	//	BlitPlus(px, py, posZ, 15, 15, 1, 0, 0, 1);

	PostRenderImGui();
}

void CGuiViewMovingPane::ScreenPosToImagePos(float screenX, float screenY, int *imageX, int *imageY)
{
	float px = (screenX - posX) - (cellStartX - posX);
	float py = (screenY - posY) - (cellStartY - posY);
	
	*imageX = floor((px / cellSizeX) + cellStartIndexX);
	*imageY = floor((py / cellSizeY) + cellStartIndexY);
}

bool CGuiViewMovingPane::DoTap(float x, float y)
{
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
						viewDataDump->viewDisassemble->ScrollToAddress(addr);
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
							viewDataDump->viewDisassemble->ScrollToAddress(cell->readPC);
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
							viewDataDump->viewDisassemble->ScrollToAddress(cell->writePC);
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
	
	return false;
}

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

