#include "CSlrFileFromTemp.h"
#include "SYS_FileSystem.h"

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
	strcpy(this->fileName, fileName);
	sprintf(this->osFileName, "%s%s", gCPathToTemp, fileName);
	
	this->fileSize = 0;
	this->Reopen();
}

void CSlrFileFromTemp::OpenForWrite(const char *fileName)
{
	LOGR("CSlrFileFromTemp: opening %s for write", fileName);
	strcpy(this->fileName, fileName);
	sprintf(this->osFileName, "%s%s", gCPathToTemp, fileName);
	
	this->fileSize = 0;
	this->ReopenForWrite();
}

