#include "CSystemFileDialogCallback.h"

void CSystemFileDialogCallback::SystemDialogFileOpenSelected(CSlrString *path)
{
}

void CSystemFileDialogCallback::SystemDialogFilesOpenSelected(std::vector<CSlrString *> *paths)
{
	if (paths == NULL || paths->empty())
	{
		SystemDialogFileOpenCancelled();
		return;
	}

	for (std::vector<CSlrString *>::iterator it = paths->begin(); it != paths->end(); it++)
	{
		CSlrString *path = *it;
		if (path != NULL)
			SystemDialogFileOpenSelected(path);
	}
}

void CSystemFileDialogCallback::SystemDialogFileOpenCancelled()
{
}

void CSystemFileDialogCallback::SystemDialogFileSaveSelected(CSlrString *path)
{
}

void CSystemFileDialogCallback::SystemDialogFileSaveCancelled()
{
}

void CSystemFileDialogCallback::SystemDialogPickFolderSelected(CSlrString *path)
{
}

void CSystemFileDialogCallback::SystemDialogPickFolderCancelled()
{
}
