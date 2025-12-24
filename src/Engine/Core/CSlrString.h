/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#ifndef _CSLRSTRING_H_
#define _CSLRSTRING_H_

#include "SYS_Defs.h"
#include <vector>
#include <list>
#include "CPool.h"

#define USE_STRINGS_POOL
#define POOL_SIZE_STRINGS	65535

void SYS_InitStrings();

class CByteBuffer;

class CSlrString
{
public:
	CSlrString();
	CSlrString(char *value);
	CSlrString(const char *value);
	CSlrString(const char16_t *value);
	CSlrString(CSlrString *copy);
	CSlrString(std::vector<u16> copy);
	CSlrString(std::string copy);
	CSlrString(u8 *buffer, u32 bufferLen);
	CSlrString(CByteBuffer *byteBuffer);
#if defined(IOS)
	CSlrString(NSString *nsString);
#endif
	virtual ~CSlrString();

	void Serialize(CByteBuffer *byteBuffer);
	void Deserialize(CByteBuffer *byteBuffer);

	u32 GetLength();
	u16 GetChar(u32 pos);
	void SetChar(u32 pos, u16 chr);
	void RemoveCharAt(u32 pos);

	void Clear();
	
	void Set(CSlrString *str);
	void Set(const char *str);
	
	void Concatenate(char chr);
	void Concatenate(u16 chr);
	void Concatenate(const char *str);
	void Concatenate(CSlrString *str);
	
	void RemoveLastCharacter();
	void RemoveEndLineCharacter();
	void RemoveFromBeginningSelectedCharacter(u16 chr);
	
	// will remove stringToRemove from the beginning, stops when characters do not match
	void RemoveFromBeginningSlrString(CSlrString *stringToRemove);
	
	void AddPathSeparatorAtEnd();
	
	bool CompareWith(u32 pos, char chr);
	bool CompareWith(const char *text);
	bool CompareWith(CSlrString *text);
	bool Equals(const char *text);
	bool Equals(CSlrString *text);

	bool Contains(char chr);
	bool Contains(u16 chr);
	
	void RemoveCharacter(u16 chr);
	void RemoveCharacter(char chr);

	// replace character with string (insert chars)
	void ReplaceCharacter(u16 characterToReplace, CSlrString *strToReplaceWith);
	
	bool IsEmpty();

	// gets one word characters till char in stopChars occurs
	CSlrString *GetWord(u32 startPos, u32 *retPos, std::list<u16> stopChars);

	// skip chars in skipChars, return pos of char which is not in skipChars
	u32 SkipChars(u32 startPos, std::list<u16> skipChars);

	std::vector<CSlrString *> *Split(std::list<u16> splitChars);
	std::vector<CSlrString *> *SplitWithChars(std::list<u16> splitChars);
	std::vector<CSlrString *> *Split(u16 splitChar);
	std::vector<CSlrString *> *Split(char splitChar);

	u16 PopCharFront();

	char *GetStdASCII();
	char *GetUTF8();
	char16_t *GetChar16Str();
	std::string GetStdStringUTF8();

	int ToInt();
	int ToIntFromHex();
	float ToFloat();
	bool ToBool();
	
	void ConvertToLowerCase();

	void DebugPrint();
	void DebugPrint(const char *name);
	void DebugPrintVector();
	void DebugPrintVector(const char *name);
	void DebugPrint(const char *name, u32 pos);
	void DebugPrint(FILE *fp);
	void PrintToLog(const char *prefix);
	
	bool IsHexValue();

	static void DeleteVector(std::vector<CSlrString *> *vect);
	static void DeleteVectorElements(std::vector<CSlrString *> *vect);
	static void DeleteList(std::list<CSlrString *> *list);
	static void DeleteListElements(std::list<CSlrString *> *list);
	
	u16 *GetUTF16(u32 *length);
	
	CSlrString *GetFileNameComponentFromPath();
	CSlrString *GetFileExtensionComponentFromPath();
	CSlrString *GetFilePathWithoutFileNameComponentFromPath();
	CSlrString *GetFilePathWithoutExtension();
	CSlrString *GetFileNameWithoutExtensionAndPath();
	
private:
	std::vector<u16> *chars;

#ifdef USE_STRINGS_POOL
private:
	static CPool poolStrings;
public:
	static void* operator new(const size_t size) { return poolStrings.New(size); }
	static void operator delete(void* pObject) { poolStrings.Delete(pObject); }
#endif
	
};

class CSlrStringIterator
{
public:
	CSlrStringIterator(CSlrString *str);
	
	CSlrString *str;
	u32 pos;
	
	bool IsEnd();
	u16 GetChar();
};

// C++ strings
#define STRALLOC(point)		cppstrdup((point))
//str_dup((point))
#define QUICKLINK(point)	str_dup((point))
#define QUICKMATCH(p1, p2)	strcmp((p1), (p2)) == 0
#define STRFREE(point)				\
do									\
{									\
	if (!(point))					\
	{								\
		LOGError("STRFREEing NULL in %s, line %d\n", __FILE__, __LINE__ ); \
	}								\
	else 							\
	{								\
		cppstrfree((point));		\
		(point) = NULL;				\
	}								\
} while(0)

char *cppstrdup(const char *str);
void cppstrfree(const char *str);

u64 strlen(char16_t *str);

unsigned char* NOT_TESTED_Utf8StringToLower(unsigned char* pString);
int NOT_TESTED_Utf8StrnCiCmp(const char* s1, const char* s2, size_t ztCount);
int NOT_TESTED_Utf8StrCiCmp(const char* s1, const char* s2);
char* NOT_TESTED_Utf8StrCiStr(const char* s1, const char* s2);

std::string Utf8StringToLowercase(const std::string& input);
std::string Utf8StringToUppercase(const std::string& input);

std::vector<u16> StringToUtf16(const std::string& utf8String);

#endif
//_CSLRSTRING_H_

