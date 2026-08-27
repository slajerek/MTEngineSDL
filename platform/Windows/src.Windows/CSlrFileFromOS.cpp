#include "CSlrFileFromOS.h"
#include "SYS_Main.h"
#include "SYS_FileSystem.h"
#include "SYS_Funct.h"
#include "SYS_WindowsPathUtils.h"
#include <cstdlib>
#include <cstring>

static void MT_CopyFileNameForDebug(char *dst, const char *src)
{
	if (!dst)
		return;
	if (!src)
		src = "";
	strncpy(dst, src, 511);
	dst[511] = 0;
}

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
	this->Open(filePath);
}

CSlrFileFromOS::CSlrFileFromOS(CSlrString* filePath, u8 fileMode)
{
	this->fp = NULL;
	this->isFromResources = false;

	char* cFilePath = filePath->GetUTF8();

	if (fileMode == SLR_FILE_MODE_READ)
	{
		this->Open(cFilePath);
	}
	else if (fileMode == SLR_FILE_MODE_WRITE)
	{
		this->OpenForWrite(cFilePath);
	}
	else SYS_FatalExit("unknown file mode %d", fileMode);

	free(cFilePath);
}

CSlrFileFromOS::CSlrFileFromOS(const char *filePath, u8 fileMode)
{
	this->fp = NULL;
	this->isFromResources = false;
	if (fileMode == SLR_FILE_MODE_READ)
	{
		this->Open(filePath);
	}
	else if (fileMode == SLR_FILE_MODE_WRITE)
	{
		this->OpenForWrite(filePath);
	}
	else SYS_FatalExit("unknown file mode %d", fileMode);
}

void CSlrFileFromOS::Open(const char *filePath)
{
	LOGD("CSlrFileFromOS: opening %s", filePath);

	LOGD("...filePath='%s'", filePath);

	MT_CopyFileNameForDebug(this->fileName, filePath);
	SYS_FixFileNameSlashes(this->fileName);
	this->osFilePath = SYS_WindowsPathBackslashes(filePath ? filePath : "");

	this->fileSize = 0;
	this->Reopen();
}

void CSlrFileFromOS::OpenForWrite(const char *filePath)
{
	LOGR("CSlrFileFromOS: opening %s for write", filePath);
	MT_CopyFileNameForDebug(this->fileName, filePath);
	this->osFilePath = SYS_WindowsPathBackslashes(filePath ? filePath : "");
	
	this->fileSize = 0;
	this->ReopenForWrite();
}

void CSlrFileFromOS::ReopenForWrite()
{
	this->osFilePath = SYS_WindowsPathBackslashes(this->osFilePath);
	LOGR("CSlrFileFromOS: opening %s size=%d", this->osFilePath.c_str(), this->fileSize);
	
	if (this->fp != NULL)
		fclose(fp);
	
	this->filePos = 0;
	this->fp = SYS_OpenFile(this->osFilePath.c_str(), "wb");
	
	if (this->fp == NULL)
	{
		LOGError("CSlrFileFromOS: failed to open %s for write", this->osFilePath.c_str());
		this->fileMode = SLR_FILE_MODE_ERROR;
		return;
	}
	
	this->fileSize = 0;
	
	this->fileMode = SLR_FILE_MODE_WRITE;
	
	LOGR("CSlrFileFromOS: %s opened, size=%d", osFilePath.c_str(), this->fileSize);
}

void CSlrFileFromOS::Reopen()
{
	this->osFilePath = SYS_WindowsPathBackslashes(this->osFilePath);
	LOGR("CSlrFileFromOS: opening %s size=%d", this->osFilePath.c_str(), this->fileSize);

	if (this->fp != NULL)
		fclose(fp);

	this->filePos = 0;
	this->fp = SYS_OpenFile(this->osFilePath.c_str(), "rb");

	if (this->fp == NULL)
	{
		LOGError("CSlrFileFromOS: failed to open %s", this->osFilePath.c_str());
		this->fileMode = SLR_FILE_MODE_ERROR;
		return;
	}

	fseek(fp, 0L, SEEK_END);
	this->fileSize = ftell(fp);
	fseek(fp, 0L, SEEK_SET);
	
	this->fileMode = SLR_FILE_MODE_READ;

	LOGR("CSlrFileFromOS: %s opened, size=%d", osFilePath.c_str(), this->fileSize);
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
