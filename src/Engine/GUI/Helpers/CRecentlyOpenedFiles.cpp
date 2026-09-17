#include "GUI_Main.h"
#include "CByteBuffer.h"
#include "CRecentlyOpenedFiles.h"
#include "CSlrString.h"
#include "SYS_FileSystem.h"

#define FILES_REFRESH_NUM_FRAMES 50

CRecentlyOpenedFiles::CRecentlyOpenedFiles(CSlrString *settingsFileName, CRecentlyOpenedFilesCallback *callback)
{
	this->settingsFileName = settingsFileName;
	this->callback = callback;
	filesListMutex = new CSlrMutex("CRecentlyOpenedFiles::filesListMutex");

	maxNumberOfFiles = 40;
	countFilesAvailableRefresh = FILES_REFRESH_NUM_FRAMES-1;
	RestoreFromSettings();

	sprintf(threadName, "CRecentlyOpenedFiles");
}

void CRecentlyOpenedFiles::Clear()
{
	filesListMutex->Lock();
	while(!listOfFiles.empty())
	{
		CRecentFile *file = listOfFiles.back();
		listOfFiles.pop_back();
		delete file;
	}
	filesListMutex->Unlock();
}

void CRecentlyOpenedFiles::Add(CSlrString *filePath)
{
	LOGD("CRecentlyOpenedFiles::Add:");
	filePath->DebugPrint("filePath=");
	
	filesListMutex->Lock();

	// check if filePath was already added, if yes then move to beginning
	for (std::list<CRecentFile *>::iterator it = listOfFiles.begin(); it != listOfFiles.end(); it++)
	{
		CRecentFile *file = *it;
		if (file->filePath->CompareWith(filePath))
		{
			listOfFiles.remove(file);
			listOfFiles.push_front(file);

			StoreToSettings();
			filesListMutex->Unlock();
			return;
		}
	}
	
	CRecentFile *file = new CRecentFile(filePath);
	listOfFiles.push_front(file);
	
	if (listOfFiles.size() > maxNumberOfFiles)
	{
		CRecentFile *fileToRemove = listOfFiles.back();
		listOfFiles.pop_back();
		delete fileToRemove;
	}
	
	StoreToSettings();
	filesListMutex->Unlock();
}

void CRecentlyOpenedFiles::Remove(CSlrString *filePath)
{
	LOGD("CRecentlyOpenedFiles::Remove:");
	filePath->DebugPrint("filePath=");
	
	filesListMutex->Lock();

	for (std::list<CRecentFile *>::iterator it = listOfFiles.begin(); it != listOfFiles.end(); it++)
	{
		CRecentFile *file = *it;
		if (file->filePath->CompareWith(filePath))
		{
			listOfFiles.remove(file);

			StoreToSettings();
			filesListMutex->Unlock();
			return;
		}
	}

	filesListMutex->Unlock();
}

bool CRecentlyOpenedFiles::Exists(CSlrString *filePath)
{
	filesListMutex->Lock();

	for (std::list<CRecentFile *>::iterator it = listOfFiles.begin(); it != listOfFiles.end(); it++)
	{
		CRecentFile *file = *it;
		if (file->filePath->CompareWith(filePath))
		{
			filesListMutex->Unlock();
			return true;
		}
	}

	filesListMutex->Unlock();
	return false;
}

bool CRecentlyOpenedFiles::IsMostRecentFilePathAvailable()
{
	filesListMutex->Lock();

	if (listOfFiles.empty())
	{
		filesListMutex->Unlock();
		return false;
	}

	CRecentFile *file = listOfFiles.front();
	if (file->isAvailable)
	{
		filesListMutex->Unlock();
		return true;
	}
	
	filesListMutex->Unlock();
	return false;
}

CSlrString *CRecentlyOpenedFiles::GetMostRecentFilePath()
{
	filesListMutex->Lock();

	if (listOfFiles.empty())
	{
		filesListMutex->Unlock();
		return NULL;
	}
	
	CRecentFile *file = listOfFiles.front();
	
	CSlrString *path = new CSlrString(file->filePath);
	filesListMutex->Unlock();

	return path;
}

CSlrString *CRecentlyOpenedFiles::GetCurrentOpenedFolder()
{
	filesListMutex->Lock();

	if (listOfFiles.empty())
	{
		filesListMutex->Unlock();
		// A COPY, never the global itself. The other branch below returns a
		// freshly allocated string, so every caller deletes what it gets back
		// -- all nine of them do. Returning gUTFPathToDocuments raw meant the
		// first caller on an empty list DELETED THAT GLOBAL, and every later
		// call handed out the dangling pointer.
		//
		// The list is empty exactly on a first run (no recent files yet), which
		// is why this only ever bit with a fresh settings folder. Under
		// USE_STRINGS_POOL the freed CSlrString is recycled by CPool -- which
		// has no double-free guard -- and handed to another string, so two live
		// CSlrStrings ended up sharing one pooled std::vector<u16>. One cleared
		// it while the other filled it, and CSlrString::GetUTF16's
		// `new u16[GetLength()]` then allocated zero elements while the loop
		// after it still wrote the other string's characters: a
		// heap-buffer-overflow that sprayed UTF-16 path text through unrelated
		// live objects and crashed the app far from here. Found with ASan
		// 2026-09-09; see c64d/src/TODO.txt "FIRST RUN".
		return new CSlrString(gUTFPathToDocuments);
	}
	
	CRecentFile *file = listOfFiles.front();
	CSlrString *folderPath = file->filePath->GetFilePathWithoutFileNameComponentFromPath();
	
	filesListMutex->Unlock();
	return folderPath;
}

void CRecentlyOpenedFiles::RenderImGuiMenu(const char *menuItemLabel)
{
	if (++countFilesAvailableRefresh >= FILES_REFRESH_NUM_FRAMES)
	{
		RefreshAreFilesAvailable();
		countFilesAvailableRefresh = 0;
	}
	
	filesListMutex->Lock();

	bool enabled = listOfFiles.size() > 0 ? true : false;
	
	CRecentFile *fileSelected = NULL;

	bool menuStarted = true;
	if (menuItemLabel != NULL)
	{
		menuStarted = ImGui::BeginMenu(menuItemLabel, enabled);
	}
	
	if (menuStarted)
	{
		for (std::list<CRecentFile *>::iterator it = listOfFiles.begin(); it != listOfFiles.end(); it++)
		{
			CRecentFile *file = *it;
			// TODO: replace below with const char *menuLabelText = guiMain->isAltPressed ? file->filePath : file->fileName;
			char *cstr = file->filePath->GetUTF8();
			if (guiMain->isAltPressed)
			{
				if (ImGui::MenuItem(cstr, "", false, file->isAvailable))
				{
					fileSelected = file;
				}
			}
			else
			{
				// Use full path as hidden ImGui ID to avoid duplicate IDs for files with the same name in different folders
				char labelBuf[4096];
				snprintf(labelBuf, sizeof(labelBuf), "%s##%s", file->fileName, cstr);
				if (ImGui::MenuItem(labelBuf, "", false, file->isAvailable))
				{
					fileSelected = file;
				}
			}
			STRFREE(cstr);
		}
		
		if (menuItemLabel != NULL)
			ImGui::EndMenu();
	}
	
	if (fileSelected)
	{
		callback->RecentlyOpenedFilesCallbackSelectedMenuItem(fileSelected->filePath);
	}
	
	filesListMutex->Unlock();
}

#define RECENTLY_OPENED_FILES_MARKER	'R'
#define RECENTLY_OPENED_FILES_VERSION	0x0001
void CRecentlyOpenedFiles::Serialize(CByteBuffer *byteBuffer)
{
	LOGD("CRecentlyOpenedFiles::Serialize");
	byteBuffer->PutU8(RECENTLY_OPENED_FILES_MARKER);
	byteBuffer->PutU16(RECENTLY_OPENED_FILES_VERSION);
	byteBuffer->PutU32((u32)listOfFiles.size());
	for (std::list<CRecentFile *>::iterator it = listOfFiles.begin(); it != listOfFiles.end(); it++)
	{
		CRecentFile *file = *it;
		byteBuffer->PutSlrString(file->filePath);
	}
}

bool CRecentlyOpenedFiles::Deserialize(CByteBuffer *byteBuffer)
{
	u8 m = byteBuffer->GetU8();
	if (m != RECENTLY_OPENED_FILES_MARKER)
	{
		LOGError("CRecentlyOpenedFiles::Deserialize: bad marker m=%02x", m);
		return false;
	}
	
	u16 v = byteBuffer->GetI16();
	if (v != RECENTLY_OPENED_FILES_VERSION)
	{
		LOGError("CRecentlyOpenedFiles::Deserialize: bad version v=%02x", v);
		return false;
	}
	
	u32 numPaths = byteBuffer->GetU32();
	for (int i = 0; i < numPaths; i++)
	{
		CSlrString *strPath = byteBuffer->GetSlrString();
		CRecentFile *file = new CRecentFile(strPath);
		listOfFiles.push_back(file);
		delete strPath;
	}
	
	return true;
}

void CRecentlyOpenedFiles::StoreToSettings()
{
	LOGD("CRecentlyOpenedFiles::StoreToSettings");
	filesListMutex->Lock();

	CByteBuffer *b = new CByteBuffer();
	Serialize(b);
	settingsFileName->DebugPrint("settingsFileName=");
	if (b->storeToSettings(settingsFileName) == false)
	{
		LOGError("CRecentlyOpenedFiles::StoreToSettings: save failed");
	}
	delete b;

	filesListMutex->Unlock();
}

void CRecentlyOpenedFiles::RestoreFromSettings()
{
	LOGD("CRecentlyOpenedFiles::RestoreFromSettings");
	filesListMutex->Lock();

	Clear();
	CByteBuffer *b = new CByteBuffer();
	settingsFileName->DebugPrint("settingsFileName=");
	b->loadFromSettings(settingsFileName);
	if (b->length > 0)
	{
		Deserialize(b);
	}
	delete b;

	filesListMutex->Unlock();
}

void CRecentlyOpenedFiles::ThreadRun(void *passData)
{
	u64 i = 0;
	
	while (true)
	{
		filesListMutex->Lock();
		u64 s = listOfFiles.size();

		if (i < s)
		{
			std::list<CRecentFile *>::iterator it = listOfFiles.begin();
			std::advance(it, i);
			
			CRecentFile *file = *it;
			CSlrString *filePath = new CSlrString(file->filePath);
			
			filesListMutex->Unlock();
			
			// this is async check with mutex unlocked (as this is blocking operation)
			bool fileExists = SYS_FileExists(filePath);
			
			filesListMutex->Lock();
			
			s = listOfFiles.size();
			if (i < s)
			{
				std::list<CRecentFile *>::iterator it = listOfFiles.begin();
				std::advance(it, i);
				
				CRecentFile *file = *it;
				
				// the same file? note, if we fail as the list changed then that's not bad - subsequent refresh will fix that
				if (file->filePath->CompareWith(filePath))
				{
					file->isAvailable = fileExists;
				}
			}
			
			delete filePath;
		}

		filesListMutex->Unlock();
		SYS_Sleep(10);
		i++;
		
		if (i >= s)
			break;
	}
}

void CRecentlyOpenedFiles::RefreshAreFilesAvailable()
{
	if (this->isRunning)
	{
		return;
	}
	
	SYS_StartThread(this);
}

CRecentlyOpenedFiles::~CRecentlyOpenedFiles()
{
	Clear();
}

CRecentFile::CRecentFile(CSlrString *filePath)
{
	this->filePath = new CSlrString(filePath);
	isAvailable = true;
	
	CSlrString *fn = this->filePath->GetFileNameComponentFromPath();
	this->fileName = fn->GetUTF8();
	delete fn;
}

CRecentFile::~CRecentFile()
{
	delete filePath;
	STRFREE(fileName);
}


void CRecentlyOpenedFilesCallback::RecentlyOpenedFilesCallbackSelectedMenuItem(CSlrString *filePath)
{
}
