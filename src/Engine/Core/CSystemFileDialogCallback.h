#ifndef _CSystemFileDialogCallback_h_
#define _CSystemFileDialogCallback_h_

#include "CSlrString.h"

class CSystemFileDialogCallback
{
public:
	virtual void SystemDialogFileOpenSelected(CSlrString *path);
	virtual void SystemDialogFileOpenCancelled();
	virtual void SystemDialogFileSaveSelected(CSlrString *path);
	virtual void SystemDialogFileSaveCancelled();
	virtual void SystemDialogPickFolderSelected(CSlrString *path);
	virtual void SystemDialogPickFolderCancelled();
};

#endif

