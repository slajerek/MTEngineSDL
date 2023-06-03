#ifndef CBYTEBUFFER_H_
#define CBYTEBUFFER_H_

#define USE_CBYTEBUFFER_POOL

#ifdef USE_CBYTEBUFFER_POOL
#include "CPool.h"
#endif

#include "CSlrFile.h"

#ifndef uint8 
#define uint8  unsigned char
#endif

#include <vector>
#define FALSE 0
#define TRUE 1

class CSlrString;
class CSlrDate;

class EBadPacket
{
public:
	EBadPacket();
};

class CByteBuffer
{
public:
    uint8  tmp[4];
	uint8  *data;
	int wholeDataBufferSize;
	int index;
	int length;
	
	bool crashWhenEOF;

	CByteBuffer();
	CByteBuffer(CByteBuffer *byteBuffer);
	CByteBuffer(uint8  *buffer, int size);
	CByteBuffer(int size);
	CByteBuffer(const char *fileName);
	CByteBuffer(const char *fileName, bool hasHeader);
	CByteBuffer(CSlrString *filePath);
	CByteBuffer(CSlrFile *file);
	CByteBuffer(CSlrFile *file, bool readHeader);
	CByteBuffer(const char *filePath, uint8 fileType);
	CByteBuffer(bool fromResources, const char *filePath, uint8 fileType);
	CByteBuffer(bool fromResources, const char *filePath, uint8 fileType, bool readHeader);
	virtual ~CByteBuffer();

	bool error;

	void Clear();
	void Rewind();
	void Reset();
	void SetData(uint8 *s, u32 len);
	void EnsureDataBufferSize(u32 len);
	void ReserveDataForInsert(u32 len);
	uint8 *GetDataPointerAtIndex();
	void InsertBytes(CByteBuffer *byteBuffer);
	bool IsEof();
	bool IsEmpty();
	void ForwardToEnd();
	void putByte(uint8  b);
	uint8  getByte();
	void PutU8(uint8  b);
	uint8  GetU8();
	void PutByte(uint8  b);
	uint8  GetByte();
	void putBytes(uint8  *b, unsigned int len);
	void PutBytes(uint8  *b, unsigned int len);
	void putBytes(uint8  *b, unsigned int begin, unsigned int len);
	uint8  *getBytes(unsigned int len);
	uint8  *GetBytes(unsigned int len);
	void getBytes(uint8  *b, unsigned int len);
	void GetBytes(uint8  *b, unsigned int len);
	void putByteBuffer(CByteBuffer *byteBuffer);
	CByteBuffer *getByteBuffer();
	void putString(const char *str);
	void PutString(const char *str);
	char *GetString();
	void putString(const char *str, int begin, int len);
	char *getString();
	void PutStdString(std::string str);
	std::string GetStdString();
	
	void PutSlrString(CSlrString *str);
	CSlrString *GetSlrString();
	
	void putStringVector(std::vector<char *> strVect);
	std::vector <char *> *getStringVector();

	void PutByteBuffer(CByteBuffer *byteBuffer);
	CByteBuffer *GetByteBuffer();


	void putShort(short int val);
	short int getShort();
	void putUnsignedShort(short unsigned int val);
	short unsigned int getUnsignedShort();
	void putU16(short unsigned int val);
	short unsigned int getU16();
	void PutU16(short unsigned int val);
	short unsigned int GetU16();
	void putInt(int val);
	int getInt();
	void putI32(int val);
	void PutI32(int val);
	int getI32();
	int GetI32();
	void putUnsignedInt(unsigned int val);
	unsigned int getUnsignedInt();
	void putU32(unsigned int val);
	void PutU32(unsigned int val);
	unsigned int getU32();
	unsigned int GetU32();
	void putU64(long long val);
	long long getU64();
	void PutU64(long long val);
	long long GetU64();
	void PutI16(i16 val);
	i16 GetI16();	
	void putLong(long long val);
	long long getLong();
	void putBool(bool val);
	bool getBool();
	void PutBool(bool val);
	bool GetBool();
	void putFloat(float val);
	float getFloat();
	void PutFloat(float val);
	float GetFloat();
	void PutDouble(double b);
	double GetDouble();
	void putDouble(double val);
	double getDouble();
	void PutDate(CSlrDate *date);
	CSlrDate *GetDate();

	
	char *toHexString();
	char *toHexString(u32 startIndex);
	char *bytesToHexString(uint8  *in, int size);
	
	void DebugPrint();
		
	void scramble(uint8  sxor[4]);
	void scramble8(uint8  sxor[8]);

	bool storeToFileWithHeader(const char *fileName);
	bool storeToFileWithHeader(CSlrString *filePath);
	bool storeToFileWithHeader(const char *fileName, uint8  sxor[4]);
	bool storeToFileWithHeader(CSlrFile *file);
	bool storeToFileNoHeader(const char *fileName);
	bool storeToFileNoHeader(CSlrFile *file);
	bool storeToFileNoHeader(CSlrString *filePath);
	
	bool readFromFile(const char *filePath);
	bool readFromFile(CSlrString *filePath);
	bool readFromFile(CSlrFile *file);
	bool storeToFile(const char *filePath);
	bool storeToFile(CSlrString *filePath);
	bool storeToFile(CSlrFile *file);

	bool readFromFile(CSlrFile *file, bool readHeader);
	bool readFromFileWithHeader(const char *fileName);
	bool readFromFileWithHeader(CSlrString *filePath);
	bool readFromFileWithHeader(const char *fileName, uint8  sxor[4]);
	bool readFromFileWithHeader(CSlrFile *file);
	bool readFromFileNoHeader(const char *fileName);
	bool readFromFileNoHeader(CSlrString *filePath);
	bool readFromFileNoHeader(CSlrFile *file);

	char nameBuf[1024];
	bool storeToDocuments(const char *fileName);
	bool loadFromDocuments(const char *fileName);
	bool storeToHiddenDocuments(const char *fileName);
	bool loadFromHiddenDocuments(const char *fileName);

	bool storeToDocuments(const char *fileName, uint8  sxor[4]);
	bool loadFromDocuments(const char *fileName, uint8  sxor[4]);
	bool storeToHiddenDocuments(const char *fileName, uint8  sxor[4]);
	bool loadFromHiddenDocuments(const char *fileName, uint8  sxor[4]);

	bool storeToDocumentsScrambled(const char *fileName);
	bool loadFromDocumentsScrambled(const char *fileName);
	bool storeToHiddenDocumentsScrambled(const char *fileName);
	bool loadFromHiddenDocumentsScrambled(const char *fileName);

	bool storeToTemp(CSlrString *fileName);
	bool loadFromTemp(CSlrString *fileName);
	bool storeToSettings(CSlrString *fileName);
	bool loadFromSettings(CSlrString *fileName);

	bool storeToCompressedFile(const char *filePath);
	bool readFromCompressedFile(const char *filePath);

	bool CompressWholeBuffer();
	bool DecompressWholeBuffer();

	void removeCRLF();
	void removeCRLFinQuotations();
	
	static bool ReadBufferFromFile(CSlrString *filePath, unsigned char *buffer, size_t length);
	static bool ReadBufferFromFile(const char *filePath, unsigned char *buffer, size_t length);
	static bool ReadFromFileOrClearBuffer(const char *filePath, unsigned char *buffer, size_t length);
	static bool ReadFromFileOrClearBuffer(CSlrString *filePath, unsigned char *buffer, size_t length);
	static u8 *ReadFromFileOrAllocEmptyBuffer(const char *filePath, size_t length);
	static bool WriteBufferToFile(const char *filePath, unsigned char *buffer, size_t length);
	
	int GetNumberOfLines();

	bool IsOpened();
		
	//bool logBytes;
	
#ifdef USE_CBYTEBUFFER_POOL
private:
	static CPool poolByteBuffer;
public:
	static void* operator new(const size_t size) { return poolByteBuffer.New(size); }
	static void operator delete(void* pObject) { poolByteBuffer.Delete(pObject); }
#endif
	
};

char *BytesToHexString(uint8  *in, int begin, int size, const char *separator);

#endif /*CBYTEBUFFER_H_*/
