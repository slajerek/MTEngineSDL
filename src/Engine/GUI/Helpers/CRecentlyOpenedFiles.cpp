#include "GUI_Main.h"
#include "CByteBuffer.h"
#include "CRecentlyOpenedFiles.h"
#include "CSlrString.h"
#include "SYS_FileSystem.h"

CRecentlyOpenedFiles::CRecentlyOpenedFiles(CSlrString *settingsFileName, CRecentlyOpenedFilesCallback *callback)
{
	this->settingsFileName = settingsFileName;
	this->callback = callback;
	
	maxNumberOfFiles = 20;
	RestoreFromSettings();
}

void CRecentlyOpenedFiles::Clear()
{
	while(!listOfFiles.empty())
	{
		CRecentFile *file = listOfFiles.back();
		listOfFiles.pop_back();
		delete file;
	}
}

void CRecentlyOpenedFiles::Add(CSlrString *filePath)
{
	LOGD("CRecentlyOpenedFiles::Add:");
	filePath->DebugPrint("filePath=");
	
	// check if filePath was already added, if yes then move to beginning
	for (std::list<CRecentFile *>::iterator it = listOfFiles.begin(); it != listOfFiles.end(); it++)
	{
		CRecentFile *file = *it;
		if (file->filePath->CompareWith(filePath))
		{
			listOfFiles.remove(file);
			listOfFiles.push_front(file);
			
			StoreToSettings();
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
}

bool CRecentlyOpenedFiles::IsMostRecentFilePathAvailable()
{
	return !listOfFiles.empty();
}

CSlrString *CRecentlyOpenedFiles::GetMostRecentFilePath()
{
	if (listOfFiles.empty())
		return NULL;
	
	CRecentFile *file = listOfFiles.front();
	return new CSlrString(file->filePath);
}

CSlrString *CRecentlyOpenedFiles::GetCurrentOpenedFolder()
{
	if (listOfFiles.empty())
		return gUTFPathToDocuments;
	
	CRecentFile *file = listOfFiles.front();
	CSlrString *folderPath = file->filePath->GetFilePathWithoutFileNameComponentFromPath();
	return folderPath;
}

void CRecentlyOpenedFiles::RenderImGuiMenu(char *menuItemLabel)
{
	bool enabled = listOfFiles.size() > 0 ? true : false;
	
	CRecentFile *fileSelected = NULL;
	if (ImGui::BeginMenu(menuItemLabel, enabled))
	{
		for (std::list<CRecentFile *>::iterator it = listOfFiles.begin(); it != listOfFiles.end(); it++)
		{
			CRecentFile *file = *it;
			if (ImGui::MenuItem(file->fileName))
			{
				fileSelected = file;
			}
		}
		ImGui::EndMenu();
	}
	
	if (fileSelected)
	{
		callback->RecentlyOpenedFilesCallbackSelectedMenuItem(fileSelected->filePath);
	}
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
	CByteBuffer *b = new CByteBuffer();
	Serialize(b);
	b->storeToSettings(settingsFileName);
	delete b;
}

void CRecentlyOpenedFiles::RestoreFromSettings()
{
	Clear();
	CByteBuffer *b = new CByteBuffer();
	b->loadFromSettings(settingsFileName);
	if (b->length > 0)
	{
		Deserialize(b);
	}
	delete b;
}

CRecentlyOpenedFiles::~CRecentlyOpenedFiles()
{
	Clear();
}

CRecentFile::CRecentFile(CSlrString *filePath)
{
	this->filePath = new CSlrString(filePath);
	
	CSlrString *fn = this->filePath->GetFileNameComponentFromPath();
	this->fileName = fn->GetStdASCII();
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
