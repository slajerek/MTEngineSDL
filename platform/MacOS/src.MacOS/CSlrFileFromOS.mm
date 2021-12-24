#import <Foundation/Foundation.h>

#include "CSlrFileFromOS.h"
#include "SYS_Main.h"
#include "SYS_FileSystem.h"
#include "SYS_Funct.h"
#include "SYS_MacOS.h"

CSlrFileFromOS::CSlrFileFromOS(CSlrString *str)
{
	this->fp = NULL;
	this->isFromResources = false;
	
	this->OpenSlrStr(str);
}

CSlrFileFromOS::CSlrFileFromOS(const char *filePath)
{
	this->fp = NULL;
	this->isFromResources = false;
	
	NSString *str = [NSString stringWithUTF8String:filePath];
	NSString *fullStr = [str stringByStandardizingPath];
	const char *path = [fullStr cStringUsingEncoding:NSUTF8StringEncoding];

	this->Open((char*)path);
}

CSlrFileFromOS::CSlrFileFromOS(CSlrString *filePath, u8 fileMode)
{
	this->fp = NULL;
	this->isFromResources = false;
	
	NSString *str = FUN_ConvertCSlrStringToNSString(filePath);
	NSString *fullStr = [str stringByStandardizingPath];
	const char *path = [fullStr cStringUsingEncoding:NSUTF8StringEncoding];
	
	if (fileMode == SLR_FILE_MODE_READ)
	{
		this->Open((char*)path);
	}
	else if (fileMode == SLR_FILE_MODE_WRITE)
	{
		this->OpenForWrite((char*)path);
	}
	else SYS_FatalExit("unknown file mode %d", fileMode);
}

CSlrFileFromOS::CSlrFileFromOS(const char *filePath, u8 fileMode)
{
	this->fp = NULL;
	this->isFromResources = false;
	
	NSString *str = [NSString stringWithUTF8String:filePath];
	NSString *fullStr = [str stringByStandardizingPath];
	const char *path = [fullStr cStringUsingEncoding:NSUTF8StringEncoding];
	
	if (fileMode == SLR_FILE_MODE_READ)
	{
		this->Open((char*)path);
	}
	else if (fileMode == SLR_FILE_MODE_WRITE)
	{
		this->OpenForWrite((char*)path);
	}
	else SYS_FatalExit("unknown file mode %d", fileMode);
}

//void CSlrFileFromOS::Open(CSlrString *str)
//{
//}
//
void CSlrFileFromOS::Open(const char *filePath)
{
	LOGR("CSlrFileFromOS: opening %s", filePath);
	strcpy(this->fileName, filePath);
	strcpy(this->osFilePath, filePath);

	this->fileSize = 0;
	this->Reopen();
}

void CSlrFileFromOS::OpenForWrite(const char *filePath)
{
	LOGR("CSlrFileFromOS: opening %s for write", filePath);
	strcpy(this->fileName, filePath);
	strcpy(this->osFilePath, filePath);
	
	this->fileSize = 0;
	this->ReopenForWrite();
}

void CSlrFileFromOS::ReopenForWrite()
{
	SYS_FixFileNameSlashes(this->osFilePath);
	LOGR("CSlrFileFromOS: opening %s size=%d", this->osFilePath, this->fileSize);
	
	if (this->fp != NULL)
		fclose(fp);
	
	this->filePos = 0;
	this->fp = fopen(this->osFilePath, "wb");
	
	if (this->fp == NULL)
	{
		LOGError("CSlrFileFromOS: failed to open %s for write", this->osFilePath);
		this->fileMode = SLR_FILE_MODE_ERROR;
		return;
	}
	
	this->fileSize = 0;
	
	this->fileMode = SLR_FILE_MODE_WRITE;
	
	LOGR("CSlrFileFromOS: %s opened, size=%d", osFilePath, this->fileSize);
}

void CSlrFileFromOS::Reopen()
{
	SYS_FixFileNameSlashes(this->osFilePath);
	LOGR("CSlrFileFromOS: opening %s size=%d", this->osFilePath, this->fileSize);

	if (this->fp != NULL)
		fclose(fp);

	this->filePos = 0;
	this->fp = fopen(this->osFilePath, "rb");

	if (this->fp == NULL)
	{
		LOGError("CSlrFileFromOS: failed to open %s", this->osFilePath);
		this->fileMode = SLR_FILE_MODE_ERROR;
		return;
	}

	fseek(fp, 0L, SEEK_END);
	this->fileSize = ftell(fp);
	fseek(fp, 0L, SEEK_SET);
	
	this->fileMode = SLR_FILE_MODE_READ;

	LOGR("CSlrFileFromOS: %s opened, size=%d", osFilePath, this->fileSize);
}

bool CSlrFileFromOS::Exists()
{
	return (fp != NULL);
}

u32 CSlrFileFromOS::GetFileSize()
{
	return this->fileSize;
}

u32 CSlrFileFromOS::Read(u8 *data, u32 numBytes)
{
	//LOGD("CSlrFileFromOS::Read: %d", numBytes);
	return fread(data, 1, numBytes, fp);
}

u8 CSlrFileFromOS::ReadByte()
{
	u8 b;
	fread(&b, 1, 1, fp);
	return b;
}

u32 CSlrFileFromOS::Write(u8 *data, u32 numBytes)
{
	return fwrite(data, 1, numBytes, fp);
}

void CSlrFileFromOS::WriteByte(u8 data)
{
	fwrite(&data, 1, 1, fp);
}

int CSlrFileFromOS::Seek(u32 newFilePos)
{
	//LOGD("CSlrFileFromOS::Seek: to %d", newFilePos);
	return fseek(fp, newFilePos, SEEK_SET);
}

int CSlrFileFromOS::Seek(long int offset, int origin)
{
	//LOGD("CSlrFileFromOS::Seek: offset %d origin %d", offset, origin);
	return fseek(fp, offset, origin);
}

u32 CSlrFileFromOS::Tell()
{
	return ftell(fp);
}

bool CSlrFileFromOS::Eof()
{
	if (fp == NULL)
		return true;
	
	return feof(fp);
}

void CSlrFileFromOS::Close()
{
	//LOGR("CSlrFileFromOS::Close()");
	if (fp != NULL)
		fclose(fp);

	fp = NULL;
}

CSlrFileFromOS::~CSlrFileFromOS()
{
	this->Close();
}

