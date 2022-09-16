#ifndef _CSLRFILEZLIB_H_
#define _CSLRFILEZLIB_H_

#include "CSlrFile.h"
#include "zlib.h"

// file preloaded to memory
class CSlrFileZlib : public CSlrFile
{
public:
	CSlrFileZlib();
	CSlrFileZlib(CSlrFile *file);
	CSlrFileZlib(CSlrFile *file, int compressionLevel);
	CSlrFileZlib(CSlrFile *file, bool shouldDeleteFilePointer);
	CSlrFileZlib(CSlrFile *file, int fileSize, bool shouldDeleteFilePointer);

	virtual void Open(CSlrFile *file);
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
	virtual ~CSlrFileZlib();

	int compressionLevel;
	u8 *chunkBuf;
	z_stream strm;
	
	bool shouldDeleteFilePointer;
	CSlrFile *file;
	
};

void UNITTEST_TestZlib();

#endif
//_CSLRFILEZLIB_H_
