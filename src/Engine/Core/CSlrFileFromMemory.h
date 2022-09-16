#ifndef _CSLR_FILE_FROM_MEMORY_H_
#define _CSLR_FILE_FROM_MEMORY_H_

#include "CSlrFile.h"

class CByteBuffer;

class CSlrFileFromMemory : public CSlrFile
{
public:
	CSlrFileFromMemory(uint8 *data, int dataLength);
	CSlrFileFromMemory(CByteBuffer *byteBuffer);
	virtual void Open(const char *fileName);
	virtual void OpenForWrite(const char *fileName);
	virtual void Reopen();
	virtual bool Exists();
	virtual u32 GetFileSize();
	virtual u32 Read(u8 *data, u32 numBytes);
	virtual u32 Write(u8 *data, u32 numBytes);
	virtual int Seek(u32 newFilePos);
	virtual int Seek(long int offset, int origin);
	virtual u32 Tell();
	virtual bool Eof();
	virtual void Close();
	virtual ~CSlrFileFromMemory();
	
	uint8 *fileData;
};


#endif
