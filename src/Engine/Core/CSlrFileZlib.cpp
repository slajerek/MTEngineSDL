#include "CSlrFileZlib.h"
#include "DBG_Log.h"
#include "SYS_Main.h"
#include "MTH_Random.h"
#include <assert.h>

#include "CSlrFileFromOS.h"

#define ZLIB_CHUNK_SIZE 1024*1024

CSlrFileZlib::CSlrFileZlib()
{
	this->fileMode = SLR_FILE_MODE_NOT_OPENED;
	this->compressionLevel = Z_BEST_COMPRESSION;
	this->shouldDeleteFilePointer = false;
}

CSlrFileZlib::CSlrFileZlib(CSlrFile *file)
{
	LOGR("CSlrFileZlib::CSlrFileZlib");
	this->shouldDeleteFilePointer = false;
	this->compressionLevel = Z_BEST_COMPRESSION;
	this->Open(file);
}

CSlrFileZlib::CSlrFileZlib(CSlrFile *file, int compressionLevel)
{
	LOGR("CSlrFileZlib::CSlrFileZlib");
	this->shouldDeleteFilePointer = false;
	this->compressionLevel = compressionLevel;
	this->Open(file);
}

CSlrFileZlib::CSlrFileZlib(CSlrFile *file, bool shouldDeleteFilePointer)
{
	LOGR("CSlrFileZlib::CSlrFileZlib");
	this->shouldDeleteFilePointer = shouldDeleteFilePointer;
	this->compressionLevel = Z_BEST_COMPRESSION;
	this->Open(file);
}

CSlrFileZlib::CSlrFileZlib(CSlrFile *file, int fileSize, bool shouldDeleteFilePointer)
{
	LOGR("CSlrFileZlib::CSlrFileZlib");
	this->shouldDeleteFilePointer = shouldDeleteFilePointer;
	this->compressionLevel = Z_BEST_COMPRESSION;
	this->Open(file);
	this->fileSize = fileSize;
}

void CSlrFileZlib::Open(CSlrFile *file)
{
	LOGR("CSlrFileZlib::Open");

	this->file = file;
	this->fileMode = file->fileMode;
	this->filePos = 0;
	
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	chunkBuf = new u8[ZLIB_CHUNK_SIZE];

	if (fileMode == SLR_FILE_MODE_READ)
	{
		int ret = inflateInit(&strm);
		if (ret != Z_OK)
		{
			SYS_FatalExit("inflateInit failed");
		}
	}
	else if (fileMode == SLR_FILE_MODE_WRITE)
	{
		int ret = deflateInit(&strm, compressionLevel);
		if (ret != Z_OK)
		{
			SYS_FatalExit("deflateInit2 failed");
		}
	}
}

void CSlrFileZlib::Reopen()
{
	this->filePos = 0;
}

bool CSlrFileZlib::Exists()
{
	return true;
}

u32 CSlrFileZlib::GetFileSize()
{
	return this->fileSize;
}

u32 CSlrFileZlib::Read(u8 *dataBuf, u32 numBytes)
{
	//LOGD("CSlrFileZlib::Read: numBytes=%d", numBytes);
	
	u8 *data = dataBuf;
	
	int ret;
	
	u32 numBytesInflated = 0;
	
	do
	{
//		LOGD("... numBytes=%d", numBytes);
//		LOGD("...strm.avail_in=%d", strm.avail_in);
		
		if (strm.avail_in == 0)
		{
			u32 numBytesToRead = file->GetFileSize() - file->Tell();
			if (numBytesToRead == 0)
			{
				LOGError("CSlrFile stream ended before Z_STREAM_END");
				break;
			}
			
			if (numBytesToRead > ZLIB_CHUNK_SIZE)
			{
				numBytesToRead = ZLIB_CHUNK_SIZE;
			}
			
			//LOGD("...file->Read: numBytesRead=%d", numBytesToRead);
			
			file->Read(chunkBuf, numBytesToRead);
			
			strm.avail_in = numBytesToRead;
			strm.next_in = chunkBuf;
		}
		
		strm.avail_out = numBytes;
		strm.next_out = data;
		
		//LOGD("... inflate");
		ret = inflate(&strm, Z_FULL_FLUSH); //Z_NO_FLUSH);
		assert(ret != Z_STREAM_ERROR);  // state not clobbered
		
		if (ret == Z_NEED_DICT)
		{
			LOGError("zlib: Z_NEED_DICT");
			break;
		}
		else if (ret == Z_DATA_ERROR)
		{
			LOGError("zlib: Z_DATA_ERROR");
			break;
		}
		else if (ret == Z_MEM_ERROR)
		{
			LOGError("zlib: Z_MEM_ERROR");
			break;
		}
		
		//LOGD("... | strm.avail_out=%d", strm.avail_out);
		
		u32 have = numBytes - strm.avail_out;
		data += have;
		numBytes -= have;
		
		//LOGD("... | have=%d", have);
		
		numBytesInflated += have;
		filePos += have;
		
		if (have == 0)
		{
			//LOGD("... have=0");
			break;
		}
		
		if (ret == Z_STREAM_END)
		{
			//LOGD("... Z_STREAM_END");
			break;
		}
		
		if (ret != Z_OK)
		{
			LOGError("ret=%d", ret);
			break;
		}
	}
	while (numBytes != 0);
	
	//LOGD("CSlrFileZlib::Read: numBytesInflated=%d", numBytesInflated);
	return numBytesInflated;
}

u32 CSlrFileZlib::Write(u8 *data, u32 numBytes)
{
	strm.total_out = 0;

	strm.next_in = data;
	strm.avail_in = numBytes;

	do
	{
		strm.avail_out = ZLIB_CHUNK_SIZE;
		strm.next_out = chunkBuf;

		int ret = deflate(&strm, Z_NO_FLUSH);
		if (ret != Z_OK)
		{
			LOGError("CSlrFileZlib::Write deflate error=%d", ret);
			SYS_FatalExit("deflate failed");
		}
		
		int len = ZLIB_CHUNK_SIZE - strm.avail_out;
		if (len > 0)
		{
			file->Write(chunkBuf, len);
		}
	}
	while (strm.avail_out == 0);
	
	return numBytes;
}

void CSlrFileZlib::Close()
{
	if (fileMode == SLR_FILE_MODE_READ)
	{
		inflateEnd(&strm);
		fileMode = SLR_FILE_MODE_NOT_OPENED;
	}
	else if (fileMode == SLR_FILE_MODE_WRITE)
	{
		int ret;
		
		do
		{
			strm.avail_out = ZLIB_CHUNK_SIZE;
			strm.next_out = chunkBuf;

			ret = deflate(&strm, Z_FINISH);
			int len = ZLIB_CHUNK_SIZE - strm.avail_out;
			file->Write(chunkBuf, len);
			
			if (ret != Z_STREAM_END)
			{
				LOGError("CSlrFileZlib::Close: deflate returned %d", ret);
				SYS_FatalExit("CSlrFileZlib::Close: deflate returned %d", ret);
			}
		}
		while (ret != Z_STREAM_END);

		deflateEnd(&strm);
		fileMode = SLR_FILE_MODE_NOT_OPENED;
	}

//	LOGD("CSlrFileZlib::Close: done");
}

CSlrFileZlib::~CSlrFileZlib()
{
	if (fileMode != SLR_FILE_MODE_NOT_OPENED)
		Close();
	
	delete [] chunkBuf;
	if (shouldDeleteFilePointer)
		delete file;
}

int CSlrFileZlib::Seek(u32 newFilePos)
{
	SYS_FatalExit("CSlrFileZlib::Seek: not implemented");
	return 0;
}

int CSlrFileZlib::Seek(long int offset, int origin)
{
	SYS_FatalExit("CSlrFileZlib::Seek: not implemented");

	return -1;
}

u32 CSlrFileZlib::Tell()
{
	return filePos;
}

bool CSlrFileZlib::Eof()
{
	// TODO: CSlrFileZlib, EOF not supported
	return false;
}

void UNITTEST_TestZlib()
{
	LOGM("UNITTEST_TestZlib");

	const char *fileName = "/Users/mars/Downloads2/test";
	const int numTries = 4;
	
	int size = 1024*1024*16;
	u8 *dataSave = new u8[size];
	u8 *dataRead = new u8[size];
	for (int i = 0; i < size; i++)
	{
		dataSave[i] = (u8)Uniform(0, 255) & 0xFF;
	}
	
	LOGD("UNITTEST_TestZlib: store");
	CSlrFileFromOS *file = new CSlrFileFromOS(fileName, SLR_FILE_MODE_WRITE);
	CSlrFileZlib *zlib = new CSlrFileZlib(file);
	
	for (int t = 0; t < numTries; t++)
	{
		zlib->Write(dataSave, size);
	}
	delete zlib;
	delete file;
	
	LOGD("UNITTEST_TestZlib: read");
	
	file = new CSlrFileFromOS(fileName, SLR_FILE_MODE_READ);
	zlib = new CSlrFileZlib(file);

	for (int t = 0; t < numTries; t++)
	{
		zlib->Read(dataRead, size);
		for (int i = 0; i < size; i++)
		{
			if (dataRead[i] != dataSave[i])
			{
				LOGError("UNITTEST_TestZlib: data at %d should be 0x%02x but is 0x%02x", i, dataRead[i], dataSave[i]);
			}
		}
	}

	delete zlib;
	delete file;
	
	LOGM("UNITTEST_TestZlib: finished, test correct");
}
