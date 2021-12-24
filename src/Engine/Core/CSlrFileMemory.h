#ifndef _CSLRFILEMEMORY_H_
#define _CSLRFILEMEMORY_H_

#include "CSlrFile.h"

class CByteBuffer;

// file preloaded to memory
class CSlrFileMemory : public CSlrFile
{
public:
	CSlrFileMemory();
	CSlrFileMemory(bool fromResources, const char *fileName, u8 fileType);
	CSlrFileMemory(CByteBuffer *byteBuffer);
	CSlrFileMemory(u8 *data, u32 size);
	CSlrFileMemory(CSlrFileMemory *cloneFile);

	virtual void Open(bool fromResources, const char *fileName, u8 fileType);
	virtual void Open(u8 *data, u32 size);
	virtual void Open(CSlrFileMemory *cloneFile);
	virtual void Reopen();
	virtual bool Exists();
	virtual u32 GetFileSize();
	virtual u32 Read(u8 *data, u32 numBytes);
	virtual int Seek(u32 newFilePos);
	virtual int Seek(long int offset, int origin);
	virtual u32 Tell();
	virtual bool Eof();
	virtual void Close();
	virtual ~CSlrFileMemory();

	u8 *memFileData;
	u8 memFileType;
	bool fromResources;
	u8 *GetFileMemoryData(u32 *fileSize);
	void ReloadFileMemory();
	bool memFileIsOpened;

	bool dataIsCloned;
};

#endif
//_CSLRFILEMEMORY_H_
