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

	void Open(const char *fileName);
	virtual void OpenForWrite(const char *fileName);
	virtual u32 Write(u8 *data, u32 numBytes);
	virtual void WriteByte(u8 data);
	void ReopenForWrite();
	void Reopen();
	bool Exists();
	u32 GetFileSize();
	u32 Read(u8 *data, u32 numBytes);
	virtual u8 ReadByte();
	int Seek(u32 newFilePos);
	int Seek(long int offset, int origin);
	u32 Tell();
	bool Eof();
	void Close();
	~CSlrFileFromDocuments();

private:
	FILE *fp;
};

#endif
//_CFILEFROMDOCUMENTS_H_

