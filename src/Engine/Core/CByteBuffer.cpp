#include "SYS_Main.h"
#include "CByteBuffer.h"
#include "SYS_Defs.h"
#include "RES_ResourceManager.h"
#include "CSlrFileFromResources.h"
#include "CSlrFileFromDocuments.h"
#include "CSlrFileFromOS.h"
#include "CSlrFileMemory.h"
#include "CSlrFileZlib.h"
#include "SYS_FileSystem.h"
#include "CSlrString.h"
#include "CSlrDate.h"
#include "zlib.h"

#define BYTEBUFFER_MAX_STRING_LENGTH	1*1024*1024

#ifdef USE_CBYTEBUFFER_POOL
CPool CByteBuffer::poolByteBuffer(1000, sizeof(CByteBuffer));
#endif

//#define PRINT_BUFFER_OPS

EBadPacket::EBadPacket()
{
}

CByteBuffer::CByteBuffer(CSlrString *filePath)
{
	CSlrFile *file = new CSlrFileFromOS(filePath, SLR_FILE_MODE_READ);
	
	error = false;
	
	this->data = NULL;
	this->wholeDataBufferSize = 0;
	this->index = 0;
	this->length = 0;
	
	if (file->Exists() == false)
	{
		this->error = true;
		LOGError("CByteBuffer: file does not exist");
	}
	else if (this->readFromFile(file, false) == false)
	{
		this->error = true;
		LOGError("CByteBuffer: file read failed");
	}
	
	delete file;
}

CByteBuffer::CByteBuffer(const char *filePath, uint8 fileType)
{
 
		/// TODO: copy pasted for simplicty in that timed coding by the coder
	
	error = false;
	
	CSlrFile *file = NULL;
	file = RES_OpenFile(true, filePath, fileType);
	
	this->data = NULL;
	this->wholeDataBufferSize = 0;
	this->index = 0;
	this->length = 0;
	
	if (file->Exists() == false)
	{
		this->error = true;
		LOGError("CByteBuffer: file does not exist");
	}
	else if (this->readFromFile(file, false) == false)
	{
		this->error = true;
		LOGError("CByteBuffer: file read failed");
	}
	
	delete file;

	
	
}


CByteBuffer::CByteBuffer(bool fromResources, const char *filePath, uint8 fileType)
{
	error = false;

	CSlrFile *file = NULL;
	file = RES_OpenFile(fromResources, filePath, fileType);

	this->data = NULL;
	this->wholeDataBufferSize = 0;
	this->index = 0;
	this->length = 0;

	if (file->Exists() == false)
	{
		this->error = true;
		LOGError("CByteBuffer: file does not exist");
	}
	else if (this->readFromFileWithHeader(file) == false)
	{
		this->error = true;
		LOGError("CByteBuffer: file read failed");
	}

	delete file;
}

CByteBuffer::CByteBuffer(bool fromResources, const char *filePath, uint8 fileType, bool readHeader)
{
	error = false;
	
	CSlrFile *file = NULL;
	file = RES_OpenFile(fromResources, filePath, fileType);
	
	this->data = NULL;
	this->wholeDataBufferSize = 0;
	this->index = 0;
	this->length = 0;
	
	if (file->Exists() == false)
	{
		this->error = true;
		LOGError("CByteBuffer: file does not exist");
	}
	else if (this->readFromFile(file, readHeader) == false)
	{
		this->error = true;
		LOGError("CByteBuffer: file read failed");
	}
	
	delete file;
}

CByteBuffer::CByteBuffer(CSlrFile *file, bool readHeader)
{
//	LOGR("CByteBuffer::CByteBuffer: CSlrFile fileSize=%d", file->fileSize);
	
	error = false;
	
	this->data = NULL;
	this->wholeDataBufferSize = 0;
	this->index = 0;
	this->length = 0;
	if (this->readFromFile(file, readHeader) == false)
	{
		LOGError("CByteBuffer: file");
		this->data = NULL;
	}
}


CByteBuffer::CByteBuffer(CSlrFile *file)
{
//	LOGR("CByteBuffer::CByteBuffer: CSlrFile fileSize=%d", file->fileSize);
	
	error = false;

	this->data = NULL;
	this->wholeDataBufferSize = 0;
	this->index = 0;
	this->length = 0;
	if (this->readFromFile(file, false) == false)
	{
		LOGError("CByteBuffer: file");
		this->data = NULL;
	}
}

CByteBuffer::CByteBuffer(int size)
{
	error = false;

	data = new uint8[size];
	this->wholeDataBufferSize = size;
	this->index = 0;
	this->length = 0;
	//this->logBytes = false;
}

CByteBuffer::CByteBuffer(uint8 *buffer, int size)
{
	error = false;

	if (buffer == NULL)
#if !defined(EXCEPTIONS_NOT_AVAILABLE)
		throw new EBadPacket();
#else
		SYS_FatalExit("CByteBuffer: buffer NULL");
#endif

	this->data = buffer;
	this->length = size;
	this->wholeDataBufferSize = size;
	this->index = 0;
	//this->logBytes = false;

	/*logger->debug("CByteBuffer():");
	for (int i = 0; i < ((size < 10) ? size : 10); i++)
	{
		logger->debug("buffer[%d] = %2.2x", i, this->data[i]);
	}*/
}

CByteBuffer::CByteBuffer()
{
	error = false;

	this->data = new uint8[1024];
	this->wholeDataBufferSize = 1024;
	this->index = 0;
	this->length = 0;
	//this->logBytes = false;
}

CByteBuffer::CByteBuffer(CByteBuffer *byteBuffer)
{
	error = false;
	
	this->data = new uint8[byteBuffer->length];
	this->wholeDataBufferSize = byteBuffer->wholeDataBufferSize;
	this->index = byteBuffer->index;
	this->length = byteBuffer->length;
	
	memcpy(this->data, byteBuffer->data, byteBuffer->length);
}

CByteBuffer::CByteBuffer(const char *fileName)
{
	error = false;

	this->data = NULL;
	this->wholeDataBufferSize = 0;
	this->index = 0;
	this->length = 0;
	if (this->readFromFileNoHeader(fileName) == false)
	{
		LOGError("CByteBuffer: file not found '%s'", fileName);
		this->data = NULL;
	}
}

CByteBuffer::CByteBuffer(const char *fileName, bool hasHeader)
{
	error = false;
	
	this->data = NULL;
	this->wholeDataBufferSize = 0;
	this->index = 0;
	this->length = 0;
	
	if (hasHeader)
	{
		if (this->readFromFileWithHeader(fileName) == false)
		{
			LOGError("CByteBuffer: file not found '%s'", fileName);
			this->data = NULL;
		}
	}
	else
	{
		if (this->readFromFileNoHeader(fileName) == false)
		{
			LOGError("CByteBuffer: file not found '%s'", fileName);
			this->data = NULL;
		}
	}
}


CByteBuffer::~CByteBuffer()
{
	if (this->data)
		delete [] this->data;
}

void CByteBuffer::SetData(uint8 *bytes, u32 len)
{
//	LOGD("CByteBuffer::SetData: this->data=%x bytes=%x", this->data, bytes);
	error = false;

	if (this->data)
	{
//		LOGD("CByteBuffer::SetData: delete this->data=%x", this->data);
		delete [] this->data;
	}

	this->data = bytes;
	this->wholeDataBufferSize = len;
	this->length = len;
	this->index = 0;	
}

void CByteBuffer::InsertBytes(CByteBuffer *byteBuffer)
{
	for (u32 i = 0; i < byteBuffer->length; i++)
	{
		this->putByte(byteBuffer->data[i]);
	}
}

void CByteBuffer::Rewind()
{
	error = false;

	this->index = 0;
}

void CByteBuffer::Clear()
{
	this->Reset();
}

void CByteBuffer::Reset()
{
	error = false;

	this->index = 0;
	this->length  = 0;
}

bool CByteBuffer::IsEof()
{
	return (index == this->length);
}

bool CByteBuffer::IsEmpty()
{
	return (this->length == 0);
}

void CByteBuffer::ForwardToEnd()
{
	index = this->length;
}

// 32kB memory chunks
#define MEM_CHUNK	1024*32

void CByteBuffer::EnsureDataBufferSize(u32 len)
{
	if (this->wholeDataBufferSize < len)
	{
		uint8 *newData = new uint8[len];
		memcpy(newData, this->data, this->wholeDataBufferSize);
		delete [] this->data;
		this->data = newData;
		this->wholeDataBufferSize = len;
	}
}

void CByteBuffer::ReserveDataForInsert(u32 len)
{
	int newLen =  this->index + len;
	if (this->wholeDataBufferSize < newLen)
	{
		uint8 *newData = new uint8[newLen];
		memcpy(newData, this->data, this->wholeDataBufferSize);
		delete [] this->data;
		this->data = newData;
		this->wholeDataBufferSize = newLen;
	}
}

uint8 *CByteBuffer::GetDataPointerAtIndex()
{
	return this->data + this->index;
}

void CByteBuffer::putByte(uint8 b)
{
#ifdef PRINT_BUFFER_OPS
	LOGD(">>>>>>>>>>>>>> putByte data[%d]=%2.2x", index, b);
#endif

	if (this->index >= this->wholeDataBufferSize)
	{
		EnsureDataBufferSize(this->wholeDataBufferSize + MEM_CHUNK);
	}
	this->data[this->index++] = b;
	this->length++;
}

uint8 CByteBuffer::getByte()
{
	if (index >= length)
	{
		//LOGError("CByteBuffer::getByte: end of stream");
		char *hexStr = this->toHexString();
		if (crashWhenEOF)
		{
			SYS_FatalExit("CByteBuffer::getByte: end of stream: index=%d length=%d\n%s-----\n", index, length, hexStr);
		}
		else
		{
			LOGError("CByteBuffer::getByte: end of stream: index=%d length=%d\n%s-----\n", index, length, hexStr);
		}

		error = true;
		return 0x00;
	}
	//if (this->logBytes)
		//logger->debug("CByteBuffer::getByte,

#ifdef PRINT_BUFFER_OPS
	LOGD("<<<<<<<<<<< getByte: data[%d]=%2.2x length=%d", index, data[index], length);
#endif

	return data[index++];
}

void CByteBuffer::putBytes(uint8 *b, unsigned int len)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("putBytes: len=%d", len);
#endif

	for (int i = 0; i < len; i++)
	{
		putByte(b[i]);
	}
}

void CByteBuffer::putBytes(uint8 *b, unsigned int begin, unsigned int len)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("putBytes: begin=%d len=%d", begin, len);
#endif

	for (int i = 0; i < len; i++)
	{
		putByte(b[begin + i]);
	}
}

uint8 *CByteBuffer::getBytes(unsigned int len)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("getBytes: len=%d", len);
#endif

	uint8 *b = new uint8[len];

	for (int i = 0; i < len; i++)
	{
		b[i] = getByte();
	}

	return b;
}

void CByteBuffer::getBytes(uint8 *b, unsigned int len)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("getBytes: len=%d", len);
#endif

	for (int i = 0; i < len; i++)
	{
		b[i] = getByte();
	}
}

void CByteBuffer::GetBytes(uint8 *b, unsigned int len)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("getBytes: len=%d", len);
#endif
	
	for (int i = 0; i < len; i++)
	{
		b[i] = getByte();
	}
}

void CByteBuffer::putByteBuffer(CByteBuffer *byteBuffer)
{
	putInt(byteBuffer->length);
	putBytes(byteBuffer->data, byteBuffer->length);
}

CByteBuffer *CByteBuffer::getByteBuffer()
{
	int size = getInt();
	uint8 *bytes = getBytes(size);

	return new CByteBuffer(bytes, size);
}

void CByteBuffer::putString(const char *str)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("putString: str='%s'", str);
#endif

	int len = strlen(str);
	this->putString(str, 0, len);
}

void CByteBuffer::putStringZeroEnded(const char *str)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("putStringZeroEnded: str='%s'", str);
#endif

	int len = strlen(str);
	putBytes((uint8 *)str, 0, len);
	putByte(0);
}

void CByteBuffer::putString(const char *str, int begin, int len)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("putString: str='%s' begin=%d len=%d", str, begin, len);
#endif

	putInt(len);
	putBytes((uint8 *)str, begin, len);
}

char *CByteBuffer::getString()
{
#ifdef PRINT_BUFFER_OPS
	LOGD("getString()");
#endif

	int len = getInt();
	if (len > BYTEBUFFER_MAX_STRING_LENGTH || len < 0)
	{
		LOGError("CByteBuffer::getString: len=%d, max=%d", len, BYTEBUFFER_MAX_STRING_LENGTH);
		this->error = true;
		return STRALLOC("");
	}

	if (len == 0)
	{
		return STRALLOC("");
    }

	char *strBytes = new char[len+1];

	getBytes((uint8*)strBytes, len);
	strBytes[len] = 0x00;

#ifdef PRINT_BUFFER_OPS
	LOGD("getString: str='%s' len=%d", strBytes, len);
#endif

	return strBytes;
}

void CByteBuffer::PutStdString(std::string str)
{
	LOGD("CByteBuffer::PutStdString");
	cout << str << endl;

	const uint8_t* data = reinterpret_cast<const uint8_t*>(str.data());
	int len = static_cast<int>(str.size());
	CByteBuffer *byteBuffer = new CByteBuffer((u8*)data, len);
	this->PutByteBuffer(byteBuffer);
	
//	const char *cStr = str.c_str();
//	this->putString(cStr);
}

std::string CByteBuffer::GetStdString()
{
	CByteBuffer *byteBuffer = this->GetByteBuffer();
	std::string str(reinterpret_cast<const char*>(byteBuffer->data), byteBuffer->length);
	
	cout << str << endl;
	
	return str;
	
//	char *cStr = this->getString();
//	std::string ret(cStr);
//	free(cStr);
//	return ret;
}


void CByteBuffer::putStringVector(std::vector<char *> strVect)
{
	putInt(strVect.size());
	for (unsigned int i = 0; i < strVect.size(); i++)
	{
		putString(strVect[i]);
	}
}

std::vector <char *> *CByteBuffer::getStringVector()
{
	std::vector<char *> *strVect = new std::vector<char *>;
	int size = getInt();
	for (int i = 0; i < size; i++)
	{
		strVect->push_back(this->getString());
	}
	return strVect;
}

void CByteBuffer::putShort(short int val)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("putShort: %d", val);
#endif
	this->putByte((uint8)((val >> 8) & 0x00FF));
	this->putByte((uint8)(val & 0x00FF));
}

short int CByteBuffer::getShort()
{
	short int s = getByte();
	s = ((s << 8) & 0xFF00) | (getByte() & 0xFF);

#ifdef PRINT_BUFFER_OPS
	LOGD("getShort: %d", s);
#endif
	return s;
}

void CByteBuffer::putUnsignedShort(short unsigned int val)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("putUnsignedShort: %d", val);
#endif
	this->putByte((uint8)((val >> 8) & 0x00FF));
	this->putByte((uint8)(val & 0x00FF));
}

short unsigned int CByteBuffer::getUnsignedShort()
{
#ifdef PRINT_BUFFER_OPS
	LOGD("getUnsignedShort()");
#endif

	uint8 b1 = getByte();
	uint8 b2 = getByte();
	
	short unsigned int s = 
	s = ((b1 << 8) & 0xFF00) | (b2 & 0xFF);

#ifdef PRINT_BUFFER_OPS
	LOGD("getUnsignedShort: %d", s);
#endif

	return s;
}

void CByteBuffer::putInt(int val)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("putInt: %d", val);
#endif
	putByte((uint8)((val >> 24) & 0x000000FF));
	putByte((uint8)((val >> 16) & 0x000000FF));
	putByte((uint8)((val >> 8)  & 0x000000FF));
	putByte((uint8)(val & 0x000000FF));
}

int CByteBuffer::getInt()
{
#ifdef PRINT_BUFFER_OPS
	LOGD("getInt()");
#endif

	unsigned int i = getUnsignedShort();
	int ret = (int)((i << 16) & 0xFFFF0000) | ((unsigned int)getUnsignedShort() & 0x0000FFFF);

#ifdef PRINT_BUFFER_OPS
	LOGD("getInt: ret=%d", ret);
#endif

	return ret;
}

void CByteBuffer::putUnsignedInt(unsigned int val)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("putUnsignedInt %d", val);
#endif
	//SYS_FatalExit("putUnsignedInt????");
	putByte((uint8)((val >> 24) & 0x000000FF));
	putByte((uint8)((val >> 16) & 0x000000FF));
	putByte((uint8)((val >> 8)  & 0x000000FF));
	putByte((uint8)(val & 0x000000FF));
}

unsigned int CByteBuffer::getUnsignedInt()
{
#ifdef PRINT_BUFFER_OPS
	LOGD("getUnsignedInt()");
#endif
	//SYS_FatalExit("getUnsignedInt????");
	unsigned int i = getUnsignedShort();
	unsigned int ret = ((i << 16) & 0xFFFF0000) | (getUnsignedShort() & 0x0000FFFF);

#ifdef PRINT_BUFFER_OPS
	LOGD("getUnsignedInt: ret=%d", ret);
#endif
	return ret;
}



void CByteBuffer::putLong(long long val)
{
	putByte((uint8)((val >> 56) & 0x00000000000000FFL));
	putByte((uint8)((val >> 48) & 0x00000000000000FFL));
	putByte((uint8)((val >> 40) & 0x00000000000000FFL));
	putByte((uint8)((val >> 32) & 0x00000000000000FFL));
	putByte((uint8)((val >> 24) & 0x00000000000000FFL));
	putByte((uint8)((val >> 16) & 0x00000000000000FFL));
	putByte((uint8)((val >> 8 ) & 0x00000000000000FFL));
	putByte((uint8)((val      ) & 0x00000000000000FFL));
}

long long CByteBuffer::getLong()
{
	long long l = this->getInt() & 0x00000000FFFFFFFFL;
	l = ((l << 32)) | (this->getInt() & 0x00000000FFFFFFFFL);
	return l;
}

void CByteBuffer::putBool(bool val)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("putBool");
#endif

	if (val)
		putByte(TRUE);
	else
		putByte(FALSE);
}

bool CByteBuffer::getBool()
{
#ifdef PRINT_BUFFER_OPS
	LOGD("getBool()");
#endif

	if (this->getByte() == TRUE)
		return true;

	return false;
}

void CByteBuffer::putDouble(double val)
{
	long long *valLong = (long long *) &val;
	this->putLong(*valLong);
}

double CByteBuffer::getDouble()
{
	long long valLong = this->getLong();
	double *valDouble = (double *) &valLong;

	return *valDouble;
}

void CByteBuffer::putFloat(float val)
{
#ifdef PRINT_BUFFER_OPS
	LOGD("putFloat: %f", val);
#endif

	double val2 = (double)val;
	long long *valLong = (long long *) &val2;
	this->putLong(*valLong);
}

float CByteBuffer::getFloat()
{
#ifdef PRINT_BUFFER_OPS
	LOGD("getFloat()");
#endif

	long long valLong = this->getLong();
	double *valDouble = (double *) &valLong;

	float valFloat = (float)*valDouble;
#ifdef PRINT_BUFFER_OPS
	LOGD("getFloat: ret=%f", valFloat);
#endif

	return valFloat;
}

void CByteBuffer::printf(const char *format, ...)
{
	char *buffer = SYS_GetCharBuf();
	va_list args;
	
	va_start(args, format);
	vsnprintf(buffer, MAX_STRING_LENGTH-1, format, args);
	va_end(args);

//	LOGD("CByteBuffer::printf %s", buffer);
	putBytes((uint8 *)buffer, 0, strlen(buffer));

	SYS_ReleaseCharBuf(buffer);
}

void CByteBuffer::DebugPrint()
{
	LOGD("CByteBuffer::DebugPrint: %s", this->toHexString());
}

char *CByteBuffer::toHexString()
{
	return BytesToHexString(this->data, 0, this->length, " ");
}

char *CByteBuffer::toHexString(u32 startIndex)
{
	if (startIndex >= this->length)
	{
		LOGError("CByteBuffer::toHexString: startIndex=%d >= length=%d", startIndex, length);
		return strdup("");
	}

	return BytesToHexString(data, startIndex, this->length-startIndex, " ");
}

char *CByteBuffer::bytesToHexString(uint8 *in, int size)
{
	return BytesToHexString(in, 0, size, " ");
}

char *BytesToHexString(uint8 *in, int begin, int size, const char *separator)
{
	u64 sepLen = strlen(separator);
	char *result = new char[size * 2 + size * sepLen + 2];
	uint8 ch = 0x00;
	const char *hexTable = "0123456789ABCDEF";

	int i = 0;
	int pos = 0;
	while (i < size)
	{
		ch = (uint8) (in[begin + i] & 0xF0);
		ch = (uint8) (ch >> 4);
		ch = (uint8) (ch & 0x0F);
		result[pos++] = hexTable[ch];
		ch = (uint8) (in[begin + i] & 0x0F);
		result[pos++] = hexTable[ch];

		for (int j = 0; j < sepLen; j++)
		{
			result[pos++] = separator[j];
		}
		i++;
	}
	result[pos++] = 0x00;
	return result;
}

bool CByteBuffer::storeToFileWithHeader(const char *fileNameConst)
{
	LOGD("CByteBuffer::storeToFile: '%s'", fileNameConst);
	
	char *fileName = SYS_GetCharBuf();
	strcpy(fileName, fileNameConst);
	SYS_FixFileNameSlashes(fileName);
	
	FILE *fp = fopen(fileName, "wb");
	if (fp == NULL)
	{
		return false;
	}
	uint8 tmp[4];

	tmp[0] = (uint8) (((this->length) >> 24) & 0x00FF);
	tmp[1] = (uint8) (((this->length) >> 16) & 0x00FF);
	tmp[2] = (uint8) (((this->length) >> 8) & 0x00FF);
	tmp[3] = (uint8) ((this->length) & 0x00FF);

	fwrite(tmp, 1, 4, fp);
	fwrite(this->data, 1, this->length, fp);
	fclose(fp);

	SYS_ReleaseCharBuf(fileName);
	return true;
}

bool CByteBuffer::storeToFileWithHeader(CSlrString *filePath)
{
	LOGD("CByteBuffer::storeToFile");
	filePath->DebugPrint("filePath=");
		
	char *f = filePath->GetUTF8();
	SYS_FixFileNameSlashes(f);

	FILE *fp = fopen(f, "wb");
	free(f);

	if (fp == NULL)
	{
		return false;
	}
	uint8 tmp[4];
	
	tmp[0] = (uint8) (((this->length) >> 24) & 0x00FF);
	tmp[1] = (uint8) (((this->length) >> 16) & 0x00FF);
	tmp[2] = (uint8) (((this->length) >> 8) & 0x00FF);
	tmp[3] = (uint8) ((this->length) & 0x00FF);
	
	fwrite(tmp, 1, 4, fp);
	fwrite(this->data, 1, this->length, fp);
	fclose(fp);
	
	return true;
}

void CByteBuffer::scramble(uint8 sxor[4])
{
	uint8 s = 0;
	for (u32 i = 0; i < this->length; i++)
	{
		switch(s)
		{
			case 0: this->data[i] ^= sxor[0]; s = 1; break;
			case 1: this->data[i] ^= sxor[1]; s = 2; break;
			case 2: this->data[i] ^= sxor[2]; s = 3; break;
			case 3: this->data[i] ^= sxor[3]; s = 0; break;
		}
	}
}

void CByteBuffer::scramble8(uint8 sxor[8])
{
	uint8 s = 0;
	for (u32 i = 0; i < this->length; i++)
	{
		switch(s)
		{
			case 0: this->data[i] ^= sxor[0]; s = 1; break;
			case 1: this->data[i] ^= sxor[1]; s = 2; break;
			case 2: this->data[i] ^= sxor[2]; s = 3; break;
			case 3: this->data[i] ^= sxor[3]; s = 4; break;
			case 4: this->data[i] ^= sxor[4]; s = 5; break;
			case 5: this->data[i] ^= sxor[5]; s = 6; break;
			case 6: this->data[i] ^= sxor[6]; s = 7; break;
			case 7: this->data[i] ^= sxor[7]; s = 0; break;
		}
	}
}

bool CByteBuffer::readFromFile(const char *filePath)
{
	return readFromFileNoHeader(filePath);
}

bool CByteBuffer::readFromFile(CSlrString *filePath)
{
	return readFromFileNoHeader(filePath);
}

bool CByteBuffer::readFromFile(CSlrFile *file)
{
	return readFromFileNoHeader(file);
}

bool CByteBuffer::storeToFile(const char *filePath)
{
	return storeToFileNoHeader(filePath);
}

bool CByteBuffer::storeToFile(CSlrString *filePath)
{
	return storeToFileNoHeader(filePath);
}

bool CByteBuffer::storeToFile(CSlrFile *file)
{
	return storeToFileNoHeader(file);
}

bool CByteBuffer::storeToCompressedFile(const char *filePath)
{
	this->CompressWholeBuffer();
	return storeToFileNoHeader(filePath);
}

bool CByteBuffer::readFromCompressedFile(const char *filePath)
{
	bool ret = readFromFileNoHeader(filePath);
	if (ret == false)
		return false;
	
	if (this->DecompressWholeBuffer() == false)
		return false;
	
	this->Rewind();
	return true;
}



bool CByteBuffer::storeToFileWithHeader(const char *fileName, uint8 sxor[4])
{
	LOGD("CByteBuffer::storeToFile: '%s'", fileName);
	
	FILE *fp = fopen(fileName, "wb");
	if (fp == NULL)
	{
		return false;
	}
	uint8 tmp[4];
	
	tmp[0] = (uint8) (((this->length) >> 24) & 0x00FF);
	tmp[0] ^= sxor[0];
	
	tmp[1] = (uint8) (((this->length) >> 16) & 0x00FF);
	tmp[1] ^= sxor[1];
	
	tmp[2] = (uint8) (((this->length) >> 8) & 0x00FF);
	tmp[2] ^= sxor[2];
	
	tmp[3] = (uint8) ((this->length) & 0x00FF);
	tmp[3] ^= sxor[3];
	
	fwrite(tmp, 1, 4, fp);

	scramble(sxor);
	
	fwrite(this->data, 1, this->length, fp);
	fclose(fp);

	scramble(sxor);

	return true;
}

bool CByteBuffer::storeToFileWithHeader(CSlrFile *file)
{
	LOGD("CByteBuffer::storeToFile");
	
	uint8 tmp[4];
	
	tmp[0] = (uint8) (((this->length) >> 24) & 0x00FF);
	tmp[1] = (uint8) (((this->length) >> 16) & 0x00FF);
	tmp[2] = (uint8) (((this->length) >> 8) & 0x00FF);
	tmp[3] = (uint8) ((this->length) & 0x00FF);
	
	file->Write(tmp, 4);
	file->Write(this->data, this->length);
	
	return true;
}

bool CByteBuffer::readFromFileWithHeader(const char *fileNameConst)
{
	LOGD("CByteBuffer::readFromFile: '%s'", fileNameConst);

	char *fileName = SYS_GetCharBuf();
	strcpy(fileName, fileNameConst);
	SYS_FixFileNameSlashes(fileName);
	
	FILE *fp = fopen(fileName, "rb");
	if (fp == NULL)
	{
		LOGD("CByteBuffer::readFromFile: %s", fileName);
		return false;
	}
	uint8 tmp[4];

	fread(tmp, 1, 4, fp);
	this->length = tmp[3] | (tmp[2] << 8) | (tmp[1] << 16) | (tmp[0] << 24);
	if (this->data != NULL)
		delete [] this->data;
	this->data = new uint8[this->length];
	fread(this->data, 1, this->length, fp);
	this->index = 0;
	this->wholeDataBufferSize = this->length;
	fclose(fp);

	SYS_ReleaseCharBuf(fileName);
	return true;
}

bool CByteBuffer::readFromFileWithHeader(CSlrString *filePath)
{
	LOGD("CByteBuffer::readFromFile");
	filePath->DebugPrint("filePath=");
	
//#if defined(IOS) || defined(MACOS)
//	NSString *strFilePath = FUN_ConvertCSlrStringToNSString(filePath);
//	FILE *fp = fopen([strFilePath fileSystemRepresentation], "rb");
//	
//#else
	char *f = filePath->GetUTF8();
	
	SYS_FixFileNameSlashes(f);
	
	FILE *fp = fopen(f, "rb");
	free(f);
//#endif
	
	if (fp == NULL)
	{
		LOGD("CByteBuffer::readFromFile failed");
		filePath->DebugPrint("filePath=");
		return false;
	}
	uint8 tmp[4];
	
	fread(tmp, 1, 4, fp);
	this->length = tmp[3] | (tmp[2] << 8) | (tmp[1] << 16) | (tmp[0] << 24);
	if (this->data != NULL)
		delete [] this->data;
	this->data = new uint8[this->length];
	int numRead = fread(this->data, 1, this->length, fp);
	this->index = 0;
	this->wholeDataBufferSize = this->length;
	fclose(fp);
	
	LOGD("CByteBuffer::readFromFile: length=%d read=%d", this->length, numRead);
	
	if (this->length != numRead)
	{
		LOGError("CByteBuffer::readFromFile failed: length=%d read=%d", this->length, numRead);
		delete [] this->data;
		this->length = 0;
		this->wholeDataBufferSize = 0;
		this->data = NULL;
		return false;
	}
	
	return true;
}

bool CByteBuffer::readFromFileWithHeader(const char *fileNameConst, uint8 sxor[4])
{
	LOGD("CByteBuffer::readFromFile: '%s'", fileNameConst);
	
	char *fileName = SYS_GetCharBuf();
	strcpy(fileName, fileNameConst);
	SYS_FixFileNameSlashes(fileName);
	
	FILE *fp = fopen(fileName, "rb");
	if (fp == NULL)
	{
		LOGD("CByteBuffer::readFromFile: %s", fileName);
		return false;
	}
	uint8 tmp[4];
	
	fread(tmp, 1, 4, fp);
	
	tmp[0] ^= sxor[0];
	tmp[1] ^= sxor[1];
	tmp[2] ^= sxor[2];
	tmp[3] ^= sxor[3];
	
	this->length = tmp[3] | (tmp[2] << 8) | (tmp[1] << 16) | (tmp[0] << 24);
	if (this->data != NULL)
		delete [] this->data;
	this->data = new uint8[this->length];
	fread(this->data, 1, this->length, fp);
	this->index = 0;
	this->wholeDataBufferSize = this->length;
	fclose(fp);
	
	scramble(sxor);
	
	SYS_ReleaseCharBuf(fileName);
	return true;
}

bool CByteBuffer::readFromFileWithHeader(CSlrFile *file)
{
	uint8 tmp[4];

	file->Read(tmp, 4);
	this->length = tmp[3] | (tmp[2] << 8) | (tmp[1] << 16) | (tmp[0] << 24);
	if (this->data != NULL)
		delete [] this->data;
	this->data = new uint8[this->length];
	file->Read(this->data, this->length);
	this->index = 0;
	this->wholeDataBufferSize = this->length;
	return true;
}

bool CByteBuffer::readFromFile(CSlrFile *file, bool readHeader)
{
	if (readHeader)
		return this->readFromFileWithHeader(file);
	
	if (this->data != NULL)
		delete [] this->data;

	this->length = file->GetFileSize();
	this->data = new uint8[this->length];
	file->Read(this->data, this->length);
	this->index = 0;
	this->wholeDataBufferSize = this->length;
	return true;
}

bool CByteBuffer::readFromFileNoHeader(const char *fileName)
{
	char *buf = SYS_GetCharBuf();
	strcpy(buf, fileName);
	
	SYS_FixFileNameSlashes(buf);

	CSlrFile *file = RES_GetFile(buf, DEPLOY_FILE_TYPE_DATA);
	SYS_ReleaseCharBuf(buf);

	if (!file)
		return false;
	if (!file->Exists())
		return false;
	
	bool ret = this->readFromFileNoHeader(file);
	
	delete file;
	return ret;
}

bool CByteBuffer::readFromFileNoHeader(CSlrString *filePath)
{
	CSlrFile *file = new CSlrFileFromOS(filePath, SLR_FILE_MODE_READ);
	if (!file->Exists())
	{
		delete file;
		return false;
	}
	
	bool ret = this->readFromFile(file, false);
	delete file;
	
	return ret;
}

bool CByteBuffer::storeToFileNoHeader(CSlrString *filePath)
{
	CSlrFile *file = new CSlrFileFromOS(filePath, SLR_FILE_MODE_WRITE);
	if (file->IsOpened() == false)
	{
		delete file;
		return false;
	}
	
	storeToFileNoHeader(file);
	delete file;
	
	return true;
}

bool CByteBuffer::readFromFileNoHeader(CSlrFile *file)
{
	return this->readFromFile(file, false);
}

bool CByteBuffer::storeToFileNoHeader(const char *fileName)
{
	LOGD("CByteBuffer::storeToFileNoHeader: '%s'", fileName);
	
	char *buf = SYS_GetCharBuf();
	strcpy(buf, fileName);
	
	SYS_FixFileNameSlashes(buf);
	FILE *fp = fopen(buf, "wb");
	SYS_ReleaseCharBuf(buf);

	if (fp == NULL)
	{
		LOGError("fp NULL");
		return false;
	}
	
	fwrite(this->data, 1, this->length, fp);
	fclose(fp);

	return true;
}

bool CByteBuffer::storeToFileNoHeader(CSlrFile *file)
{
	file->Write(this->data, this->length);
	return true;
}

bool CByteBuffer::storeToHiddenDocuments(const char *fileName)
{
	sprintf(nameBuf, "%s%s", gCPathToDocuments, fileName);
	SYS_FixFileNameSlashes(nameBuf);
	return this->storeToFileWithHeader(nameBuf);
}

bool CByteBuffer::loadFromHiddenDocuments(const char *fileName)
{
	sprintf(nameBuf, "%s%s", gCPathToDocuments, fileName);
	SYS_FixFileNameSlashes(nameBuf);
	return this->readFromFileWithHeader(nameBuf);
}

bool CByteBuffer::storeToDocuments(const char *fileName)
{
	sprintf(nameBuf, "%s%s", gCPathToDocuments, fileName);
	SYS_FixFileNameSlashes(nameBuf);
	return this->storeToFileWithHeader(nameBuf);
}

bool CByteBuffer::loadFromDocuments(const char *fileName)
{
	sprintf(nameBuf, "%s%s", gCPathToDocuments, fileName);
	SYS_FixFileNameSlashes(nameBuf);
	return this->readFromFileWithHeader(nameBuf);
}

bool CByteBuffer::storeToHiddenDocuments(const char *fileName, uint8 sxor[4])
{
	sprintf(nameBuf, "%s%s", gCPathToDocuments, fileName);
	SYS_FixFileNameSlashes(nameBuf);
	return this->storeToFileWithHeader(nameBuf, sxor);
}

bool CByteBuffer::loadFromHiddenDocuments(const char *fileName, uint8 sxor[4])
{
	sprintf(nameBuf, "%s%s", gCPathToDocuments, fileName);
	SYS_FixFileNameSlashes(nameBuf);
	return this->readFromFileWithHeader(nameBuf, sxor);
}

bool CByteBuffer::storeToDocuments(const char *fileName, uint8 sxor[4])
{
	sprintf(nameBuf, "%s%s", gCPathToDocuments, fileName);
	SYS_FixFileNameSlashes(nameBuf);
	return this->storeToFileWithHeader(nameBuf, sxor);
}

bool CByteBuffer::loadFromDocuments(const char *fileName, uint8 sxor[4])
{
	sprintf(nameBuf, "%s%s", gCPathToDocuments, fileName);
	SYS_FixFileNameSlashes(nameBuf);
	return this->readFromFileWithHeader(nameBuf, sxor);
}

uint8 cxor[4] = { 0xBA, 0xCA, 0xFA, 0xCE };

bool CByteBuffer::storeToHiddenDocumentsScrambled(const char *fileName)
{
	return this->storeToHiddenDocuments(fileName, cxor);
}

bool CByteBuffer::loadFromHiddenDocumentsScrambled(const char *fileName)
{
	return loadFromHiddenDocuments(fileName, cxor);
}

bool CByteBuffer::storeToDocumentsScrambled(const char *fileName)
{
	return storeToDocuments(fileName, cxor);
}

bool CByteBuffer::loadFromDocumentsScrambled(const char *fileName)
{
	return loadFromDocuments(fileName, cxor);
}

bool CByteBuffer::storeToTemp(CSlrString *fileName)
{
	CSlrString *str = new CSlrString(gUTFPathToTemp);
	str->Concatenate(fileName);
	
	bool ret = storeToFile(str);
	delete str;
	
	return ret;
}

bool CByteBuffer::loadFromTemp(CSlrString *fileName)
{
	CSlrString *str = new CSlrString(gUTFPathToTemp);
	str->Concatenate(fileName);
	
	bool ret = readFromFile(str);
	delete str;
	
	return ret;
}

bool CByteBuffer::storeToSettings(CSlrString *fileName)
{
//	LOGD("CByteBuffer::storeToSettings");
//	fileName->DebugPrint("CByteBuffer::storeToSettings: fileName=");
//	gUTFPathToSettings->DebugPrint("CByteBuffer::storeToSettings: gUTFPathToSettings=");

	CSlrString *str = new CSlrString(gUTFPathToSettings);

//	str->DebugPrint("CByteBuffer::storeToSettings: str=");
//	fileName->DebugPrint("CByteBuffer::storeToSettings: fileName=");

	str->Concatenate(fileName);
	
//	str->DebugPrint("CByteBuffer::storeToSettings: storeToFile, path str=");
	
	bool ret = storeToFile(str);
	delete str;
	
	return ret;
}

bool CByteBuffer::loadFromSettings(CSlrString *fileName)
{
	CSlrString *str = new CSlrString(gUTFPathToSettings);
	str->Concatenate(fileName);
	
	bool ret = readFromFile(str);
	delete str;
	
	return ret;
}


void CByteBuffer::removeCRLF()
{
	uint8 *parsed = new uint8[this->wholeDataBufferSize];
	
	u32 newLength = 0;
	for (u32 i = 0; i < length; i++)
	{
		uint8 c = this->data[i];
		if (c == 0x0D || c == 0x0A)
		{
			continue;
		}
		
		parsed[newLength++] = c;
	}
	
	delete [] this->data;
	this->data = parsed;
	
	this->length = newLength;
}

void CByteBuffer::removeCRLFinQuotations()
{
	uint8 *parsed = new uint8[this->wholeDataBufferSize];
	
	bool insideQ = false;
	u32 newLength = 0;
	for (u32 i = 0; i < length; i++)
	{
		uint8 c = this->data[i];
		
		if (insideQ)
		{
			if (c == 0x0D || c == 0x0A)
			{
				continue;
			}
		}
		
		if (c == '"')
		{
			if (insideQ == false)
			{
				insideQ = true;
			}
			else
			{
				insideQ = false;
			}
		}
		
		parsed[newLength++] = c;
	}
	
	delete [] this->data;
	this->data = parsed;
	
	//LOGD("old len=%d new len=%d", this->length, newLength);
	
	this->length = newLength;
}

void CByteBuffer::putU16(short unsigned int val)
{
	this->putUnsignedShort(val);
}

short unsigned int CByteBuffer::getU16()
{
	return this->getUnsignedShort();
}

void CByteBuffer::PutI16(i16 val)
{
	this->putShort(val);
}

i16 CByteBuffer::GetI16()
{
	return this->getShort();
}

void CByteBuffer::putU32(unsigned int val)
{
	this->putUnsignedInt(val);
}

unsigned int CByteBuffer::GetU32()
{
	return this->getUnsignedInt();
}

void CByteBuffer::PutU32(unsigned int val)
{
	this->putUnsignedInt(val);
}

unsigned int CByteBuffer::getU32()
{
	return this->getUnsignedInt();
}

void CByteBuffer::putI32(int val)
{
	this->putInt(val);
}

int CByteBuffer::getI32()
{
	return this->getInt();
}

void CByteBuffer::PutI32(int val)
{
	this->putInt(val);
}

int CByteBuffer::GetI32()
{
	return this->getInt();
}

void CByteBuffer::putU64(long long val)
{
	this->putLong(val);
}

long long CByteBuffer::getU64()
{
	return this->getLong();
}

void CByteBuffer::PutU64(long long val)
{
	this->putLong(val);
}

long long CByteBuffer::GetU64()
{
	return this->getLong();
}

void CByteBuffer::PutU16(short unsigned int val)
{
	this->putUnsignedShort(val);
}

short unsigned int CByteBuffer::GetU16()
{
	return this->getUnsignedShort();
}

void CByteBuffer::PutBytes(uint8 *b, unsigned int len)
{
	this->putBytes(b, len);
}

uint8 *CByteBuffer::GetBytes(unsigned int len)
{
	return this->getBytes(len);
}

void CByteBuffer::PutString(const char *str)
{
	this->putString(str);
}

void CByteBuffer::PutStringZeroEnded(const char *str)
{
	this->putStringZeroEnded(str);
}

char *CByteBuffer::GetString()
{
	return this->getString();
}

void CByteBuffer::PutByteBuffer(CByteBuffer *byteBuffer)
{
	this->putByteBuffer(byteBuffer);
}

CByteBuffer *CByteBuffer::GetByteBuffer()
{
	return this->getByteBuffer();
}

void CByteBuffer::PutBool(bool val)
{
	this->putBool(val);
}

bool CByteBuffer::GetBool()
{
	return this->getBool();
}

void CByteBuffer::PutByte(uint8 b)
{
	this->putByte(b);
}

uint8 CByteBuffer::GetByte()
{
	return this->getByte();
}

void CByteBuffer::PutU8(uint8 b)
{
	this->putByte(b);
}

uint8 CByteBuffer::GetU8()
{
	return this->getByte();
}

void CByteBuffer::PutFloat(float b)
{
	this->putFloat(b);
}

float CByteBuffer::GetFloat()
{
	return this->getFloat();
}

void CByteBuffer::PutDouble(double b)
{
	this->putDouble(b);
}

double CByteBuffer::GetDouble()
{
	return this->getDouble();
}


void CByteBuffer::PutSlrString(CSlrString *str)
{
	if (str == NULL)
	{
		this->PutU32(0xFFFFFFFF);
	}
	else
	{
		str->Serialize(this);		
	}
}

CSlrString *CByteBuffer::GetSlrString()
{
	u32 len = this->GetU32();
	
	if (len == 0xFFFFFFFF)
	{
		return NULL;
	}
	
	this->index -= 4;

	CSlrString *str = new CSlrString(this);
	return str;
}

void CByteBuffer::PutDate(CSlrDate *date)
{
	this->PutByte(date->day);
	this->PutByte(date->month);
	this->PutI16(date->year);
	this->PutByte(date->second);
	this->PutByte(date->minute);
	this->PutByte(date->hour);
}

CSlrDate *CByteBuffer::GetDate()
{
	uint8 day = GetByte();
	uint8 month = GetByte();
	i16 year = GetI16();
	uint8 second = GetByte();
	uint8 minute = GetByte();
	uint8 hour = GetByte();
	CSlrDate *d = new CSlrDate(day, month, year, second, minute, hour);
	return d;
}

int CByteBuffer::GetNumberOfLines()
{
	LOGD("GetNumberOfLines, len=%d", this->length);
	
	int numLines = 0;
	for (int i = 0; i < this->length; i++)
	{
		if (this->data[i] == '\n')
		{
			numLines++;
		}
	}
	
	LOGD("GetNumberOfLines: numLines=%d", numLines);
	return numLines;
}

// this will compress data and replace it with compressed buffer with a dest number of bytes at the beginning
// @returns is correct/no error
bool CByteBuffer::CompressWholeBuffer()
{
	LOGD("CByteBuffer::CompressWholeBuffer");
	
	u32 decompressedSize = this->length;
	uLong outBufferSize = compressBound(decompressedSize);
	u8 *outBuffer = new u8[outBufferSize];

	int result = compress2(outBuffer, &outBufferSize, this->data, this->length, 9);

	if (result != Z_OK)
	{
		LOGError("zlib error: %d", result);
		delete [] outBuffer;
		return false;
	}

	u32 outSize = (u32)outBufferSize;

	LOGD("..original size=%d compressed=%d", this->length, outSize);

	this->Clear();
	this->PutU32(decompressedSize);
	this->PutBytes(outBuffer, outSize);
	
	delete [] outBuffer;
	
	return true;
}

bool CByteBuffer::DecompressWholeBuffer()
{
	LOGD("CByteBuffer::DecompressWholeBuffer");
	
	this->Rewind();
	
	u32 decompressedSize = this->GetU32();
	u8 *decompressedDataBuffer = (u8*)malloc( decompressedSize );

	u32 compressedSize = this->length - 4;
	u8 *compressedData = this->data + 4;	// u32
	
	CSlrFileMemory *memFile = new CSlrFileMemory(compressedData, compressedSize);
	
	CSlrFileZlib *fileZlib = new CSlrFileZlib(memFile);
	int readLen = fileZlib->Read(decompressedDataBuffer, decompressedSize);
	if (readLen != decompressedSize)
	{
		LOGError("CByteBuffer::DecompressWholeBuffer: readLen=%d != decompressedSize=%d", readLen, decompressedSize);
		this->Clear();
		delete [] decompressedDataBuffer;
		delete fileZlib;
		return false;
	}
	
	delete fileZlib;
	
	this->Clear();
	this->PutBytes(decompressedDataBuffer, decompressedSize);
	
	delete [] decompressedDataBuffer;
	
	delete memFile;

	return true;
}

bool CByteBuffer::WriteBufferToFile(const char *filePath, unsigned char *buffer, size_t length)
{
	FILE *fp = fopen(filePath, "wb");
	if (!fp)
		return false;
	
	size_t l = fwrite(buffer, 1, length, fp);
	if (l == length)
	{
		fclose(fp);
		return true;
	}
	
	fclose(fp);
	return false;
}

bool CByteBuffer::ReadBufferFromFile(CSlrString *filePath, unsigned char *buffer, size_t length)
{
	if (!filePath)
		return false;
	
	char *p = filePath->GetUTF8();
	FILE *fp = fopen(p, "rb");
	STRFREE(p);
	if (!fp)
	{
		return false;
	}
	
	size_t l = fread(buffer, 1, length, fp);
	if (l == length)
	{
		fclose(fp);
		return true;
	}
	
	fclose(fp);
	return false;
}


bool CByteBuffer::ReadBufferFromFile(const char *filePath, unsigned char *buffer, size_t length)
{
	FILE *fp = fopen(filePath, "rb");
	if (!fp)
		return false;
	
	size_t l = fread(buffer, 1, length, fp);
	if (l == length)
	{
		fclose(fp);
		return true;
	}
	
	fclose(fp);
	return false;
}

u8 *CByteBuffer::ReadFromFileOrAllocEmptyBuffer(const char *filePath, size_t length)
{
	u8 *buffer = new u8[length];
	memset(buffer, 0, length);
	CByteBuffer::ReadBufferFromFile(filePath, buffer, length);
	return buffer;
}

bool CByteBuffer::ReadFromFileOrClearBuffer(const char *filePath, unsigned char *buffer, size_t length)
{
	if (filePath && CByteBuffer::ReadBufferFromFile(filePath, buffer, length))
	{
		return true;
	}
	
	memset(buffer, 0, length);
	return false;
}

bool CByteBuffer::ReadFromFileOrClearBuffer(CSlrString *filePath, unsigned char *buffer, size_t length)
{
	if (filePath && CByteBuffer::ReadBufferFromFile(filePath, buffer, length))
	{
		return true;
	}
	
	memset(buffer, 0, length);
	return false;
}

