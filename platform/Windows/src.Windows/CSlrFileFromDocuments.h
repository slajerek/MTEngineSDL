#ifndef _CFILEFROMDOCUMENTS_H_
#define _CFILEFROMDOCUMENTS_H_

#include "SYS_Defs.h"
#include "CSlrFile.h"

class CSlrFileFromDocuments : public CSlrFile
{
public:
	char osFileName[768];

	CSlrFileFromDocuments(const char *fileName);
	CSlrFileFromDocuments(const char *fileName, u8 fileMode);
	virtual void Open(const char *fileName);
	virtual void OpenForWrite(const char *fileName);
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
	virtual ~CSlrFileFromDocuments();

private:
	FILE *fp;
};

#endif
//_CFILEFROMDOCUMENTS_H_

