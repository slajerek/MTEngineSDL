#ifndef _CSLRFILE_H_
#define _CSLRFILE_H_

#include "SYS_Defs.h"

class CSlrString;
class CByteBuffer;

#define SLR_FILE_MODE_NOT_OPENED	0
#define SLR_FILE_MODE_ERROR			1
#define SLR_FILE_MODE_READ			2
#define SLR_FILE_MODE_WRITE			3

// this is abstract class
class CSlrFile
{
public:
	char fileName[512];

	CSlrFile();
	virtual void OpenSlrStr(CSlrString *str);
	virtual void Open(const char *fileName);
	virtual void OpenForWrite(const char *fileName);
	virtual void Reopen();
	virtual bool Exists();
	virtual bool IsOpened();
	virtual u32 GetFileSize();
	virtual u32 Read(u8 *data, u32 numBytes);
	virtual u32 Write(u8 *data, u32 numBytes);
	virtual void WriteByte(u8 data);
	virtual int Seek(u32 newFilePos);
	virtual int Seek(long int offset, int origin);
	virtual u32 Tell();
	virtual bool Eof();
	virtual bool ReadLine(char *buf, u32 bufLen);
	virtual bool ReadLineNoComment(char *buf, u32 bufLen);
	virtual u8 ReadByte();
	virtual u16 ReadUnsignedShort();
	virtual u32 ReadUnsignedInt();
	virtual CByteBuffer *GetByteBuffer();
	virtual void Close();
	virtual ~CSlrFile();

	u32 fileSize;
	u32 filePos;
	
	bool isFromResources;
	u8 fileMode;
};

#endif
//_CSLRFILE_H_

