#include "CSlrFileFromTemp.h"
#include "SYS_FileSystem.h"
#include "SYS_WindowsPathUtils.h"
#include <cstring>

CSlrFileFromTemp::CSlrFileFromTemp(const char *fileName)
: CSlrFileFromDocuments(fileName)
{
	
}

CSlrFileFromTemp::CSlrFileFromTemp(const char *fileName, u8 fileMode)
: CSlrFileFromDocuments(fileName, fileMode)
{
	
}

void CSlrFileFromTemp::Open(const char *fileName)
{
	LOGR("CSlrFileFromTemp: opening %s", fileName);
	strncpy(this->fileName, fileName ? fileName : "", 511);
	this->fileName[511] = 0;
	this->osFileName = SYS_WindowsPathBackslashes(std::string(gCPathToTemp ? gCPathToTemp : "") + (fileName ? fileName : ""));
	
	this->fileSize = 0;
	this->Reopen();
}

void CSlrFileFromTemp::OpenForWrite(const char *fileName)
{
	LOGR("CSlrFileFromTemp: opening %s for write", fileName);
	strncpy(this->fileName, fileName ? fileName : "", 511);
	this->fileName[511] = 0;
	this->osFileName = SYS_WindowsPathBackslashes(std::string(gCPathToTemp ? gCPathToTemp : "") + (fileName ? fileName : ""));
	
	this->fileSize = 0;
	this->ReopenForWrite();
}
