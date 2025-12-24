#include "CSlrFile.h"
#include "SYS_Main.h"
#include "CSlrString.h"
#include "CByteBuffer.h"

CSlrFile::CSlrFile()
{
	this->fileName[0] = '\0';
	this->fileMode = SLR_FILE_MODE_NOT_OPENED;
}

void CSlrFile::OpenSlrStr(CSlrString *str)
{
	char *path = str->GetUTF8();
	
	this->Open(path);
	
	delete []path;
}

void CSlrFile::Open(const char *fileName)
{
	SYS_FatalExit("abstract CSlrFile::Open");
}

void CSlrFile::OpenForWrite(const char *fileName)
{
	SYS_FatalExit("abstract CSlrFile::OpenForWrite");
}

u32 CSlrFile::Write(u8 *data, u32 numBytes)
{
	SYS_FatalExit("abstract CSlrFile::Write");
	return 0;
}

void CSlrFile::WriteByte(u8 data)
{
	this->Write(&data, 1);
}

void CSlrFile::Reopen()
{
	SYS_FatalExit("abstract CSlrFile::Reopen");
}

bool CSlrFile::Exists()
{
	SYS_FatalExit("abstract CSlrFile::Exists");
	return false;
}

bool CSlrFile::IsOpened()
{
	if (this->fileMode == SLR_FILE_MODE_READ || this->fileMode == SLR_FILE_MODE_WRITE)
	{
		return true;
	}
	return false;
}

u32 CSlrFile::GetFileSize()
{
	return this->fileSize;
}

u32 CSlrFile::Read(u8 *data, u32 numBytes)
{
	SYS_FatalExit("abstract CSlrFile::Read");
	return 0;
}

int CSlrFile::Seek(u32 newFilePos)
{
	SYS_FatalExit("abstract CSlrFile::Seek");
	return -1;
}

int CSlrFile::Seek(long int offset, int origin)
{
	SYS_FatalExit("abstract CSlrFile::Seek2");
	return -1;
}

u32 CSlrFile::Tell()
{
	return this->filePos;
}

bool CSlrFile::Eof()
{
	SYS_FatalExit("abstract CSlrFile::Eof");
	return false;
}

bool CSlrFile::ReadLine(char *buf, u32 bufLen)
{
	bool eof = false;
	u32 i = 0;
	while(i < bufLen-1)
	{
		if(Eof())
		{
			eof = true;
			break;
		}

		char c = 0x00;
		Read((u8*)&c, 1);
		if (c == 0x0A)
			break;
		if (c == 0x0D)
			continue;

		buf[i++] = c;
	}

	buf[i] = 0x00;
	return eof;
}

bool CSlrFile::ReadLineNoComment(char *buf, u32 bufLen)
{
	bool eof = false;
	while(!eof)
	{
		eof = this->ReadLine(buf, bufLen);
		if (buf[0] == '#')
		{
			buf[0] = '\0';
			continue;
		}

		break;
	}

	return eof;
}

u8 CSlrFile::ReadByte()
{
	u8 data;
	this->Read(&data, 1);
	return data;
}

void CSlrFile::WriteBool(bool b)
{
	WriteByte(b ? 1 : 0);
}

bool CSlrFile::ReadBool()
{
	return (ReadByte() == 0) ? false : true;
}

u16 CSlrFile::ReadUnsignedShort()
{
	short unsigned int s = ReadByte();
	s = ((s << 8) & 0xFF00) | (ReadByte() & 0xFF);

	return s;
}

void CSlrFile::WriteUnsignedShort(u16 val)
{
	u8 v1 = (val & 0xFF00) >> 8;
	u8 v2 =  val & 0x00FF;
	
	WriteByte(v1);
	WriteByte(v2);
}

u32 CSlrFile::ReadUnsignedInt()
{
	unsigned int i = ReadUnsignedShort();
	unsigned int ret = ((i << 16) & 0xFFFF0000) | (ReadUnsignedShort() & 0x0000FFFF);
	return ret;
}

void CSlrFile::WriteUnsignedInt(u32 val)
{
	u16 v1 = (val & 0xFFFF0000) >> 16;
	u16 v2 =  val & 0x0000FFFF;
	
	WriteUnsignedShort(v1);
	WriteUnsignedShort(v2);
}

void CSlrFile::WriteUnsignedLong(u64 val)
{
	u32 v1 = (val & 0xFFFFFFFF00000000) >> 32;
	u32 v2 =  val & 0x00000000FFFFFFFF;
	
	WriteUnsignedInt(v1);
	WriteUnsignedInt(v2);
}

u64 CSlrFile::ReadUnsignedLong()
{
	u32 i = ReadUnsignedInt();
	u64 ret = (( ((u64)i) << 32) & 0xFFFFFFFF00000000) | (ReadUnsignedInt() & 0x00000000FFFFFFFF);
	return ret;
}

void CSlrFile::WriteU8(u8 data)
{
	WriteByte(data);
}

u8 CSlrFile::ReadU8()
{
	return ReadByte();
}

void CSlrFile::WriteU16(u16 val)
{
	WriteUnsignedShort(val);
}

u16 CSlrFile::ReadU16()
{
	return ReadUnsignedShort();
}

void CSlrFile::WriteU32(u32 val)
{
	WriteUnsignedInt(val);
}

u32 CSlrFile::ReadU32()
{
	return ReadUnsignedInt();
}

void CSlrFile::WriteU64(u64 val)
{
	WriteUnsignedLong(val);
}

u64 CSlrFile::ReadU64()
{
	return ReadUnsignedLong();
}

void CSlrFile::WriteByteBuffer(CByteBuffer *byteBuffer)
{
	WriteUnsignedInt(byteBuffer->length);
	if (byteBuffer->length != 0)
	{
		Write(byteBuffer->data, byteBuffer->length);
	}
}

void CSlrFile::ReadByteBuffer(CByteBuffer *byteBuffer)
{
	unsigned int len = ReadUnsignedInt();
	if (len != 0)
	{
		byteBuffer->EnsureDataBufferSize(len);
		byteBuffer->Rewind();
		Read(byteBuffer->data, len);
	}
	byteBuffer->length = len;
}

CByteBuffer *CSlrFile::GetWholeFileAsByteBuffer()
{
	CByteBuffer *byteBuffer = new CByteBuffer(this, false);
	return byteBuffer;
}


void CSlrFile::Close()
{
//	SYS_FatalExit("abstract CSlrFile::Close");
}

CSlrFile::~CSlrFile()
{
	if (fileMode != SLR_FILE_MODE_NOT_OPENED)
		Close();
}

