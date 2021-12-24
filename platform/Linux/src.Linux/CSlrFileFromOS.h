#ifndef _CFILEFROMOS_H_
#define _CFILEFROMOS_H_

#include "SYS_Defs.h"
#include "CSlrFile.h"

class CSlrFileFromOS : public CSlrFile
{
public:
	char osFilePath[768];
	
	CSlrFileFromOS(CSlrString *filePath);
	CSlrFileFromOS(CSlrString *filePath, u8 fileMode);
	CSlrFileFromOS(const char *filePath);
	CSlrFileFromOS(const char *filePath, u8 fileMode);
	virtual void Open(const char *filePath);
	virtual void OpenForWrite(const char *filePath);
	virtual u32 Write(u8 *data, u32 numBytes);
	virtual void WriteByte(u8 data);
	virtual void Reopen();
	virtual void ReopenForWrite();
	virtual bool Exists();
	virtual u32 GetFileSize();
	virtual u32 Read(u8 *data, u32 numBytes);
	virtual u8 ReadByte();
	virtual int Seek(u32 newFilePos);
	virtual int Seek(long int offset, int origin);
	virtual u32 Tell();
	virtual bool Eof();
	virtual void Close();
	virtual ~CSlrFileFromOS();

private:
	FILE *fp;
};

#endif
//_CFILEFROMOS_H_

