#include "CSlrFileFromResources.h"
#include "SYS_Main.h"
#include "SYS_FileSystem.h"
#include "SYS_MacOS.h"

CSlrFileFromResources::CSlrFileFromResources(const char *fileName)
{
	this->fp = NULL;
	this->Open(fileName);
}

void CSlrFileFromResources::Open(const char *fileName)
{
	LOGR("CSlrFileFromResources: opening %s", fileName);

	this->isFromResources = true;
	
	strcpy(this->fileName, fileName);
	this->fileSize = 0;
	this->Reopen();
}

void CSlrFileFromResources::Reopen()
{
	LOGR("CSlrFileFromResources: opening %s size=%d", this->fileName, this->fileSize);

	if (this->fp != NULL)
		fclose(fp);

	this->filePos = 0;

	NSString *path = MACOS_GetPathForResource(fileName);
	
	if (path == nil)
	{
		LOGR("CSlrFileFromResources: failed to open %s", this->fileName);
		return; 
		//SYS_FatalExit("CSlrFileFromResources: failed to open %s", this->fileName);
	}
	NSLog(@"CSlrFileFromResources: path=%@", path);
	this->fp = fopen([path fileSystemRepresentation], "rb");

	if (this->fp == NULL)
	{
		NSLog(@"CSlrFileFromResources: failed to open %s, path=%@", this->fileName, path);
		this->fileMode = SLR_FILE_MODE_ERROR;
		return;
	}

	fseek(fp, 0L, SEEK_END);
	this->fileSize = ftell(fp);
	fseek(fp, 0L, SEEK_SET);

	this->fileMode = SLR_FILE_MODE_READ;

	LOGR("CSlrFileFromResources: %s opened", fileName);
}

bool CSlrFileFromResources::Exists()
{
	return (fp != NULL);
}

u32 CSlrFileFromResources::GetFileSize()
{
	return this->fileSize;
}

u32 CSlrFileFromResources::Read(u8 *data, u32 numBytes)
{
	//LOGD("CSlrFileFromResources::Read: %d", numBytes);
	return fread(data, 1, numBytes, fp);
}

int CSlrFileFromResources::Seek(u32 newFilePos)
{
	//LOGD("CSlrFileFromResources::Seek: to %d", newFilePos);
	return fseek(fp, newFilePos, SEEK_SET);
}

int CSlrFileFromResources::Seek(long int offset, int origin)
{
	//LOGD("CSlrFileFromResources::Seek: offset %d origin %d", offset, origin);
	return fseek(fp, offset, origin);
}

u32 CSlrFileFromResources::Tell()
{
	return ftell(fp);
}

bool CSlrFileFromResources::Eof()
{
	if (fp == NULL)
		return true;
	
	return feof(fp);
}

void CSlrFileFromResources::Close()
{
	//LOGR("CSlrFileFromResources::Close()");
	if (fp != NULL)
		fclose(fp);

	fp = NULL;
}

CSlrFileFromResources::~CSlrFileFromResources()
{
	this->Close();
}

