/*
 *  CGuiViewSaveFile.h
 *
 *  Created by Marcin Skoczylas on 10-07-15.
 *  Copyright 2010 Marcin Skoczylas. All rights reserved.
 *
 */

#ifndef _GUI_DIALOG_SAVE_FILE_
#define _GUI_DIALOG_SAVE_FILE_

#include "CSlrImage.h"
#include "CGuiElement.h"
#include "SYS_FileSystem.h"
#include "CGuiView.h"
#include "CGuiButton.h"
#include "VID_Main.h"
#include "CGuiEditBoxText.h"
#include "CSlrFont.h"
#include "CGuiViewSelectFolder.h"

#include <vector>

#ifdef IPHONE
#import <UIKit/UIKit.h>
#endif

#include "GuiConsts.h"


class CGuiViewSaveFileCallback;

class CGuiViewSaveFile : public CGuiView, CGuiButtonCallback, CGuiEditBoxTextCallback, CGuiViewSelectFolderCallback
{
public:
	CGuiViewSaveFile(float posX, float posY, float posZ, float sizeX, float sizeY, CGuiViewSaveFileCallback *callback);

	void Init(CSlrString *defaultFileName, CSlrString *saveExtension);
	void Init(CSlrString *defaultFileName, CSlrString *saveExtension, CSlrString *saveDirectoryPath);
	void InitFavorites(std::list<CGuiFolderFavorite *> favorites);
	void SetFont(CSlrFont *font);
	void Render();

	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);

	CGuiButton *btnCancel;
	CGuiButton *btnSave;

	CGuiButton *btnSelectFolder;

	bool ButtonPressed(CGuiButton *button);

	virtual void ActivateView();
	virtual void DeactivateView();

	bool canceled;

	char *titleText;
	void SetTitleText(char *newTitleText);

	CGuiEditBoxText *editBoxFileName;
	void EditBoxTextFinished(CGuiEditBoxText *editBox, char *text);

	CSlrFont *font;
	float fontScale;
	
	CGuiViewSelectFolder *viewSelectFolder;
	
	CGuiViewSaveFileCallback *callback;

	virtual void FolderSelected(CSlrString *fullFolderPath, CSlrString *folderPath);
	virtual void FolderSelectionCancelled();
	
	void SetFont(CSlrFont *font, float fontScale);
	
	float offsetX, offsetY;
	
private:
	CSlrString *defaultFileName;
	CSlrString *saveDirectoryPath;
	CSlrString *saveExtension;

};

class CGuiViewSaveFileCallback
{
public:
	virtual void SaveFileSelected(CSlrString *fullFilePath, CSlrString *fileName);
	virtual void SaveFileSelectionCancelled();
};

#endif //_GUI_DIALOG_SAVE_FILE_
