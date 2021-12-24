/*
 *  CGuiViewSelectFile.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-03-25.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_VIEW_SELECT_FILE_
#define _GUI_VIEW_SELECT_FILE_

#include "SYS_Defs.h"
#include "CSlrImage.h"
#include "CGuiElement.h"
#include "SYS_FileSystem.h"
#include "CGuiView.h"
#include "CGuiList.h"
#include "CGuiButton.h"
#include <vector>

class CGuiViewSelectFileCallback;
class CGuiFolderFavorite;

class CGuiViewSelectFile : public CGuiView, public CGuiListCallback, public CGuiButtonCallback, public CHttpFileUploadedCallback
{
private:
public:
	CGuiViewSelectFile(float posX, float posY, float posZ, float sizeX, float sizeY, bool cancelButton, CGuiViewSelectFileCallback *callback);
	~CGuiViewSelectFile();

	void Render();
	void Render(float posX, float posY, float posZ, float sizeX, float sizeY);

	void LockRenderMutex();
	void UnlockRenderMutex();
	
	CSlrFont *font;

	void Init(std::list<CSlrString *> *extensions);
	void Init(CSlrString *directoryPath, std::list<CSlrString *> *extensions);
	void InitWithStartPath(CSlrString *directoryPath, std::list<CSlrString *> *extensions);
	//void InitNoStartPath(UTFString *directoryPath, std::list<UTFString *> *extensions);

	void InitFavorites(std::list<CGuiFolderFavorite *> favorites);
	std::vector<CGuiFolderFavorite *> favorites;
	std::vector<CGuiButton *> buttonsFavorites;

	CGuiViewSelectFileCallback *callback;

	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);

	virtual bool DoScrollWheel(float deltaX, float deltaY);

	std::list<CSlrString *> *extensions;
	CSlrString *startDirectoryPath;
	CSlrString *currentDirectoryPath;
	char displayDirectoryPath[4096];

	void UpdateDisplayDirectoryPath();

	CGuiList *listBoxFiles;

	std::vector<CFileItem *> *files;

	void ListElementSelected(CGuiList *listBox);

	void SetCallback(CGuiViewSelectFileCallback *callback);

	/*
	 bool DoTap(float x, float y);
	 bool DoDoubleTap(float x, float y);
	 bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	 bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	 void FinishTouches();
	 bool InitZoom();
	 bool DoZoomBy(float x, float y, float zoomValue, float difference);
	 */

	void SetPath(char *setPath);
	void Refresh();

	CGuiButton *btnCancel;
//	bool ButtonClicked(CGuiButton *button);
	bool ButtonPressed(CGuiButton *button);

	void HttpFileUploadedCallback();

	void SetFont(CSlrFont *font, float fontScale);

private:
	void DeleteItems();
	void UpdatePath();
};

class CGuiViewSelectFileCallback
{
public:
	virtual void FileSelected(CSlrString *filePath);
	virtual void FileSelectionCancelled();
};

#endif
//_GUI_VIEW_SELECT_FILE_

