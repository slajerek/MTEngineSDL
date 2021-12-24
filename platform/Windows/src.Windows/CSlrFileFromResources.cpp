#include "CSlrFileFromResources.h"
#include "SYS_Main.h"
#include "SYS_FileSystem.h"
#include "SYS_DocsVsRes.h"

#if defined(USE_DOCS_INSTEAD_OF_RESOURCES)
#define RESOURCES_DIR_NAME gPathToDocuments
#else
#define RESOURCES_DIR_NAME gPathToResources
#endif

CSlrFileFromResources::CSlrFileFromResources(const char *fileName)
{
	this->fp = NULL;
	this->Open(fileName);
}

void CSlrFileFromResources::Open(const char *fileName)
{
	LOGR("CSlrFileFromResources: opening %s", fileName);
	strcpy(this->fileName, fileName);
	sprintf(this->osFileName, "%s%s", RESOURCES_DIR_NAME, fileName);

	this->fileSize = 0;
	this->Reopen();
}

void CSlrFileFromResources::Reopen()
{
	LOGR("CSlrFileFromResources: opening %s size=%d", this->osFileName, this->fileSize);

	if (this->fp != NULL)
		fclose(fp);

	this->filePos = 0;
	this->fp = fopen(this->osFileName, "rb");

	if (this->fp == NULL)
	{
		LOGError("CSlrFileFromResources: failed to open %s", this->osFileName);
		return;
	}

	fseek(fp, 0L, SEEK_END);
	this->fileSize = ftell(fp);
	fseek(fp, 0L, SEEK_SET);

	LOGR("CSlrFileFromResources: %s opened fileSize=%d", this->osFileName, this->fileSize);
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
	LOGR("CSlrFileFromResources::Seek: to %d", newFilePos);
	return fseek(fp, newFilePos, SEEK_SET);
}

int CSlrFileFromResources::Seek(long int offset, int origin)
{
	LOGR("CSlrFileFromResources::Seek: offset %d origin %d", offset, origin);
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
	//LOGD("CSlrFileFromResources::Close()");
	if (fp != NULL)
		fclose(fp);

	fp = NULL;
}

CSlrFileFromResources::~CSlrFileFromResources()
{
	this->Close();
}

