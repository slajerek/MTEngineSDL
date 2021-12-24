/*
 *  CGuiViewSelectFolder.h
 *  MobiTracker
 *
 *  Created by Marcin Skoczylas on 10-03-25.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_VIEW_SELECT_FOLDER_
#define _GUI_VIEW_SELECT_FOLDER_

#include "SYS_Defs.h"
#include "CSlrImage.h"
#include "CGuiElement.h"
#include "SYS_FileSystem.h"
#include "CGuiView.h"
#include "CGuiList.h"
#include "CGuiButton.h"
#include <vector>

class CGuiViewSelectFolderCallback;
class CGuiFolderFavorite;

class CGuiViewSelectFolder : public CGuiView, public CGuiListCallback, public CGuiButtonCallback, public CHttpFileUploadedCallback
{
private:
public:
	CGuiViewSelectFolder(float posX, float posY, float posZ, float sizeX, float sizeY, bool cancelButton, CGuiViewSelectFolderCallback *callback);
	~CGuiViewSelectFolder();

	void Render();
	void Render(float posX, float posY, float posZ, float sizeX, float sizeY);

	void LockRenderMutex();
	void UnlockRenderMutex();

	void Init();
	void Init(CSlrString *directoryPath);
	void Init(CSlrString *startPath, CSlrString *currentPath);
	
	void InitFavorites(std::list<CGuiFolderFavorite *> favorites);
	std::vector<CGuiFolderFavorite *> favorites;
	std::vector<CGuiButton *> buttonsFavorites;
	
	CGuiViewSelectFolderCallback *callback;
	
	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);

	CSlrFont *font;

	CSlrString *startDirectoryPath;
	CSlrString *currentDirectoryPath;
	char displayDirectoryPath[4096];
	
	void UpdateDisplayDirectoryPath();

	CGuiList *listBoxFiles;

	std::vector<CFileItem *> *files;

	void ListElementSelected(CGuiList *listBox);

	void SetCallback(CGuiViewSelectFolderCallback *callback);

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

	CGuiButton *btnDone;
	CGuiButton *btnCancel;
//	bool ButtonClicked(CGuiButton *button);
	bool ButtonPressed(CGuiButton *button);

	void HttpFileUploadedCallback();

	void SetFont(CSlrFont *font, float fontScale);

private:
	void DeleteItems();
	void UpdatePath();
};

class CGuiViewSelectFolderCallback
{
public:
	virtual void FolderSelected(CSlrString *fullFolderPath, CSlrString *folderPath);
	virtual void FolderSelectionCancelled();
};

#endif
//_GUI_VIEW_SELECT_FOLDER_

