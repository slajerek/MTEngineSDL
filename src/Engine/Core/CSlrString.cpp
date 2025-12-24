#include "CSlrString.h"
#include "SYS_Main.h"
#include "CByteBuffer.h"
#include "SYS_FileSystem.h"
#include "SYS_Threading.h"
#include <iostream>
#include <string>
#include <locale>
#include <codecvt>
#include "utf8.h" // From utf8cpp

std::list< std::vector<u16> * > _CSlrString_charVectorsPool;
CSlrMutex *_CSlrString_mutex;

bool _sysStringsInit = false;

void SYS_InitStrings()
{
	if (_sysStringsInit == false)
	{
		_CSlrString_mutex = new CSlrMutex("_CSlrString_mutex");
		_CSlrString_charVectorsPool.clear();
		
		_sysStringsInit = true;
	}
	
	LOGD("SYS_InitStrings: _CSlrString_charVectorsPool=%x", &_CSlrString_charVectorsPool);
}

std::vector<u16> *_CSlrString_GetCharsVector()
{
	std::vector<u16> *vect;

	_CSlrString_mutex->Lock();
	
	if (_CSlrString_charVectorsPool.empty())
	{
		vect = new std::vector<u16>();
		vect->reserve(1000);
	}
	else
	{
		vect = _CSlrString_charVectorsPool.front();
		_CSlrString_charVectorsPool.pop_front();
	}

	_CSlrString_mutex->Unlock();

	return vect;
}

void _CSlrString_ReleaseCharsVector(std::vector<u16> *vect)
{
	vect->clear();

	_CSlrString_mutex->Lock();

	_CSlrString_charVectorsPool.push_back(vect);

	_CSlrString_mutex->Unlock();
}

CSlrString::CSlrString()
{
	this->chars = _CSlrString_GetCharsVector();
}

#if defined(IOS)
CSlrString::CSlrString(NSString *nsString)
{
	NSData *data = [nsString dataUsingEncoding:NSUTF16StringEncoding];
	
	u8 *buffer = (u8*)[data u8s];
	u32 bufferLen = [data length];
	
	this->chars = _CSlrString_GetCharsVector();
	
	u32 len = bufferLen / 2;
	u16 *buf = (u16*)buffer;
	
	for (u32 i = 0; i < len; i++)
	{
		if (i == 0 && buf[i] == 0xFEFF)
			continue;
		this->chars->push_back(buf[i]);
	}
}
#endif

CSlrString::CSlrString(char *value)
{
	u64 len = strlen(value);
	
	this->chars = _CSlrString_GetCharsVector();
	
	for (u32 i = 0; i < len; i++)
	{
		this->chars->push_back(value[i]);
	}
}

CSlrString::CSlrString(const char *value)
{
	u64 len = strlen(value);
	
	this->chars = _CSlrString_GetCharsVector();
	
	for (u32 i = 0; i < len; i++)
	{
		this->chars->push_back(value[i]);
	}
}

u64 strlen(char16_t *str)
{
	return std::char_traits<char16_t>::length(str);
}

CSlrString::CSlrString(const char16_t *value)
{
	u64 len = std::char_traits<char16_t>::length(value);
	
	this->chars = _CSlrString_GetCharsVector();
	
	for (u32 i = 0; i < len; i++)
	{
		this->chars->push_back(value[i]);
	}
}

CSlrString::CSlrString(CSlrString *copy)
{
	u32 len = copy->GetLength();
	
	this->chars = _CSlrString_GetCharsVector();

	for (u32 i = 0; i < len; i++)
	{
		this->chars->push_back(copy->GetChar(i));
	}
}

CSlrString::CSlrString(std::vector<u16> copy)
{
	u32 len = copy.size();

	this->chars = _CSlrString_GetCharsVector();
	
	for (u32 i = 0; i < len; i++)
	{
		if (i == 0 && copy[i] == 0xFEFF)
			continue;
		
		this->chars->push_back(copy[i]);
	}
}

CSlrString::CSlrString(std::string copy)
{
	std::vector<u16> vect = StringToUtf16(copy);
	u32 len = vect.size();

	this->chars = _CSlrString_GetCharsVector();
	
	for (u32 i = 0; i < len; i++)
	{
		if (i == 0 && vect[i] == 0xFEFF)
			continue;
		
		this->chars->push_back(vect[i]);
	}
}

CSlrString::CSlrString(u8 *buffer, u32 bufferLen)
{
	this->chars = _CSlrString_GetCharsVector();

	u32 len = bufferLen / 2;
	u16 *buf = (u16*)buffer;
	
	for (u32 i = 0; i < len; i++)
	{
		if (i == 0 && buf[i] == 0xFEFF)
			continue;
		this->chars->push_back(buf[i]);
	}
}

CSlrString::CSlrString(CByteBuffer *byteBuffer)
{
	this->chars = _CSlrString_GetCharsVector();
	this->Deserialize(byteBuffer);
}

void CSlrString::Serialize(CByteBuffer *byteBuffer)
{
	u32 len = this->chars->size();
	byteBuffer->PutU32(len);

	for (std::vector<u16>::iterator it = this->chars->begin();
		 it != this->chars->end(); it++)
	{
		u16 chr = *it;
		byteBuffer->PutU16(chr);
	}
}

void CSlrString::Deserialize(CByteBuffer *byteBuffer)
{
	u32 len = byteBuffer->GetU32();
	
	for (u32 i = 0; i < len; i++)
	{
		u16 chr = byteBuffer->GetU16();
		
		if (i == 0 && chr == 0xFEFF)
		{
			continue;
		}
		this->chars->push_back(chr);
	}
}

CSlrString::~CSlrString()
{
	_CSlrString_ReleaseCharsVector(this->chars);
}

void CSlrString::Set(CSlrString *str)
{
	this->Clear();
	this->Concatenate(str);
}

void CSlrString::Set(const char *str)
{
	this->Clear();
	this->Concatenate(str);
}


void CSlrString::Clear()
{
	this->chars->clear();
}

u16 *CSlrString::GetUTF16(u32 *length)
{
	u16 *retBuffer = new u16[this->GetLength()];
	*length = this->GetLength();

	u32 i = 0;
	for (std::vector<u16>::iterator it = this->chars->begin();
		 it != this->chars->end(); it++)
	{
		u16 chr = *it;
		retBuffer[i++] = chr;
	}
		
	return retBuffer;
}

u32 CSlrString::GetLength()
{
	return this->chars->size();
}

bool CSlrString::IsEmpty()
{
	if (this->chars == NULL)
		return true;

	return this->chars->empty();
}

u16 CSlrString::GetChar(u32 pos)
{
	if (pos < GetLength())
	{
		return (*this->chars)[pos];
	}

	SYS_AssertCrash("CSlrString::GetChar: pos=%d length=%d", pos, GetLength());
	this->DebugPrint("CSlrString::GetChar: text=");
	return 0;
}

void CSlrString::SetChar(u32 pos, u16 chr)
{
	if (pos < GetLength())
	{
		(*this->chars)[pos] = chr;
		return;
	}
	
	SYS_AssertCrash("CSlrString::SetChar: pos=%d length=%d", pos, GetLength());
	this->DebugPrint("CSlrString::SetChar: text=");
}

void CSlrString::RemoveCharAt(u32 pos)
{
	if (pos < GetLength())
	{
		this->chars->erase(this->chars->begin() + pos);
		return;
	}
	
	SYS_AssertCrash("CSlrString::RemoveCharAt: pos=%d length=%d", pos, GetLength());
	this->DebugPrint("CSlrString::RemoveCharAt: text=");
}

bool CSlrString::Equals(const char *text)
{
	return this->CompareWith(text);
}

bool CSlrString::Equals(CSlrString *text)
{
	return this->CompareWith(text);
}

bool CSlrString::CompareWith(const char *text)
{
	u32 len = GetLength();
	u32 len2 = strlen(text);

	if (len != len2)
	{
		//LOGD("len=%d len2=%d", len, len2);
		return false;
	}


	for (u32 i = 0; i < len; i++)
	{
		if (text[i] != GetChar(i))
			return false;
	}

	return true;
}

bool CSlrString::CompareWith(CSlrString *text)
{
	u32 len = GetLength();

	if (len != text->GetLength())
		return false;

	for (u32 i = 0; i < len; i++)
	{
		if (text->GetChar(i) != GetChar(i))
			return false;
	}

	return true;
}

void CSlrString::Concatenate(u16 chr)
{
	this->chars->push_back(chr);
}

void CSlrString::Concatenate(CSlrString *str)
{
	for (u32 i = 0; i < str->GetLength(); i++)
	{
		this->chars->push_back(str->GetChar(i));
	}
}

void CSlrString::Concatenate(char chr)
{
	this->Concatenate((u16)chr);
}

void CSlrString::Concatenate(const char *str)
{
	u32 len = strlen(str);
	for(u32 i = 0; i < len; i++)
	{
		this->Concatenate(str[i]);
	}
}

void CSlrString::RemoveLastCharacter()
{
	if (!this->chars->empty())
	{
		this->chars->pop_back();
	}
}

void CSlrString::RemoveEndLineCharacter()
{
	while (GetLength() > 0)
	{
		u16 chr = GetChar(GetLength()-1);
		if (chr == '\n' || chr == '\r')
		{
			RemoveLastCharacter();
			continue;
		}
		
		return;
	}
}

CSlrString *CSlrString::GetWord(u32 startPos, u32 *retPos, std::list<u16> stopChars)
{
	CSlrString *ret = new CSlrString();

	*retPos = startPos;
	while(*retPos < GetLength())
	{
		u16 chr = GetChar(*retPos);

		//LOGD("chr=%c pos=%d", chr, *retPos);

		bool found = false;
		for (std::list<u16>::iterator it = stopChars.begin(); it != stopChars.end(); it++)
		{
			u16 stopChr = *it;

			//LOGD("chr=%d stopChr=%d", chr, stopChr);

			if (chr == stopChr)
			{
				found = true;
				break;
			}
		}

		if (found)
			break;

		ret->Concatenate(chr);

		*retPos = *retPos + 1;

	}

	return ret;
}

u32 CSlrString::SkipChars(u32 startPos, std::list<u16> skipChars)
{
	u32 pos = startPos;
	while(pos < GetLength())
	{
		u16 chr = GetChar(pos);

		bool found = false;
		for (std::list<u16>::iterator it = skipChars.begin(); it != skipChars.end(); it++)
		{
			u16 skipChr = *it;

			//LOGD("chr=%d stopChr=%d", chr, stopChr);

			if (chr == skipChr)
			{
				continue;
			}

			found = true;
			break;
		}

		if (found)
			break;

		pos++;

	}

	return pos;
}

bool CSlrString::CompareWith(u32 pos, char chr)
{
	return ((char)this->GetChar(pos) == chr);
}

std::vector<CSlrString *> *CSlrString::Split(std::list<u16> splitChars)
{
	std::vector<CSlrString *> *words = new std::vector<CSlrString *>();
	u32 pos = 0;

	while(pos < GetLength())
	{
		CSlrString *oneWord = this->GetWord(pos, &pos, splitChars);
		
		if (oneWord->GetLength() == 0)
		{
			delete oneWord;
			pos++;
			continue;
		}
		
		words->push_back(oneWord);

		pos++; // = this->SkipChars(pos, splitChars);
	}

	return words;
}

std::vector<CSlrString *> *CSlrString::Split(u16 splitChar)
{
	std::list<u16> splitChars;
	splitChars.push_back(splitChar);

	return this->Split(splitChars);
}

std::vector<CSlrString *> *CSlrString::Split(char splitChar)
{
	return this->Split((u16)splitChar);
}

std::vector<CSlrString *> *CSlrString::SplitWithChars(std::list<u16> splitChars)
{
	std::vector<CSlrString *> *words = new std::vector<CSlrString *>();
	u32 pos = 0;

	u16 repeatedSplitChar = 0;

	while(pos < GetLength())
	{
		u16 chr = this->GetChar(pos);
		
		bool found = false;
		
		for (std::list<u16>::iterator it = splitChars.begin(); it != splitChars.end(); it++)
		{
			u16 chrSplit = *it;
		
			//LOGD("chrSplit=%d (%c) chr=%d (%c) repeatedSplitChar=%d", chrSplit, chrSplit, chr, chr, repeatedSplitChar);
			
			if (chrSplit == chr)
			{
				std::vector<u16> chrs;
				u16 chr = this->GetChar(pos);
				chrs.push_back(chr);

				if (repeatedSplitChar != chr)
				{
					repeatedSplitChar = chr;
					CSlrString *oneSplitChar = new CSlrString(chrs);
					words->push_back(oneSplitChar);
				}
				
				found = true;
				
				pos++;
				break;
			}
		}
		
		if (found)
			continue;
		
		repeatedSplitChar = 0x00;
		
		CSlrString *oneWord = this->GetWord(pos, &pos, splitChars);
		
		if (oneWord->GetLength() == 0)
		{
			delete oneWord;
			continue;
		}

		words->push_back(oneWord);
	}
	
	return words;
}

void CSlrString::RemoveFromBeginningSelectedCharacter(u16 chr)
{
	std::vector<u16> newChars;
	
	bool skip = true;
	for (std::vector<u16>::iterator it = this->chars->begin(); it != this->chars->end(); it++)
	{
		u16 c = *it;
		if (skip)
		{
			if (c == chr)
			{
				continue;
			}
			
			skip = false;
		}
		
		newChars.push_back(c);
	}
	
	// copy newChars to this
	this->Clear();
	for (std::vector<u16>::iterator it = newChars.begin(); it != newChars.end(); it++)
	{
		u16 c = *it;
		this->chars->push_back(c);
	}
}

void CSlrString::RemoveFromBeginningSlrString(CSlrString *stringToRemove)
{
	std::vector<u16> newChars;
	std::vector<u16>::iterator itThis = this->chars->begin();
	std::vector<u16>::iterator itToRemove = stringToRemove->chars->begin();
	
	while (true)
	{
		if (itThis == this->chars->end())
			break;
		
		if (itToRemove == stringToRemove->chars->end())
			break;
		
		u16 chThis = *itThis;
		u16 chToRemove = *itToRemove;
		
		if (chThis != chToRemove)
			break;
		
		itThis++;
		itToRemove++;
	}

	// copy rest of the string
	while (itThis != this->chars->end())
	{
		u16 chThis = *itThis;
		newChars.push_back(chThis);
		itThis++;
	}
	
	// copy newChars to this
	this->Clear();
	for (std::vector<u16>::iterator it = newChars.begin(); it != newChars.end(); it++)
	{
		u16 c = *it;
		this->chars->push_back(c);
	}
}

void CSlrString::AddPathSeparatorAtEnd()
{
	// check if there is a path separator at end already
	if (GetChar(this->GetLength()-1) == '/'
		|| GetChar(this->GetLength()-1) == '\\')
	{
		SetChar(this->GetLength()-1, SYS_FILE_SYSTEM_PATH_SEPARATOR);
	}
	else
	{
		Concatenate(SYS_FILE_SYSTEM_PATH_SEPARATOR);
	}
}

bool CSlrString::Contains(char chr)
{
	return this->Contains((u16)chr);
}

bool CSlrString::Contains(u16 chr)
{
	u64 len = GetLength();

	for (u64 i = 0; i < len; i++)
	{
		if (GetChar(i) == chr)
			return true;
	}

	return false;
}

void CSlrString::RemoveCharacter(char chr)
{
	this->RemoveCharacter((u16)chr);
}

void CSlrString::RemoveCharacter(u16 chr)
{
	u64 len = GetLength();
	
	for (u64 i = 0; i < len; i++)
	{
		if (GetChar(i) == chr)
		{
			this->RemoveCharAt(i);
			len = GetLength();
		}
	}
}

// replace character with string (insert chars)
void CSlrString::ReplaceCharacter(u16 characterToReplace, CSlrString *strToReplaceWith)
{
	std::vector<u16> replacedChars;
	
	u32 len = GetLength();
	
	for (u32 i = 0; i < len; i++)
	{
		u16 c = GetChar(i);
		if (c == characterToReplace)
		{
			for (u32 j = 0; j < strToReplaceWith->GetLength(); j++)
			{
				u16 chr2 = strToReplaceWith->GetChar(j);
				replacedChars.push_back(chr2);
			}
		}
		else
		{
			replacedChars.push_back(c);
		}
	}
	
	this->chars->clear();
	for (std::vector<u16>::iterator it = replacedChars.begin(); it != replacedChars.end(); it++)
	{
		u16 c = *it;
		this->chars->push_back(c);
	}
}

char *CSlrString::GetStdASCII()
{
	u32 len = this->GetLength();

	char *retBuf = new char[len+1];

	for (u32 i = 0; i < len; i++)
	{
		retBuf[i] = (char)GetChar(i);
	}
	retBuf[len] = 0x00;

	return retBuf;
}

char16_t *CSlrString::GetChar16Str()
{
	u32 len = this->GetLength();

	char16_t *retBuf = new char16_t[len+1];

	for (u32 i = 0; i < len; i++)
	{
		retBuf[i] = (char16_t)GetChar(i);
	}
	retBuf[len] = 0x00;

	return retBuf;
}

// convert utf16 to utf8
char *CSlrString::GetUTF8()
{
	if (GetLength() == 0)
		return STRALLOC("");
	
	int utf16_len = GetLength();

	// Calculate the maximum possible length of the UTF-8 string
	// UTF-8 can take up to 4 bytes per character
	int max_utf8_len = utf16_len * 4;

	// Allocate memory for the UTF-8 string
	char* utf8 = (char*)malloc(max_utf8_len + 1);
	if (utf8 == NULL)
		return NULL;

	char* utf8_ptr = utf8;

	for (int i = 0; i < utf16_len; i++)
	{
		u16 c = GetChar(i);
		if (c <= 0x7F)
		{
			// 1-byte UTF-8 character
			*utf8_ptr++ = (char)c;
		}
		else if (c <= 0x7FF)
		{
			// 2-byte UTF-8 character
			*utf8_ptr++ = 0xC0 | (char)((c >> 6) & 0x1F);
			*utf8_ptr++ = 0x80 | (char)(c & 0x3F);
		}
		else
		{
			// 3-byte UTF-8 character
			*utf8_ptr++ = 0xE0 | (char)((c >> 12) & 0x0F);
			*utf8_ptr++ = 0x80 | (char)((c >> 6) & 0x3F);
			*utf8_ptr++ = 0x80 | (char)(c & 0x3F);
		}
	}

	*utf8_ptr = '\0'; // Null-terminate the UTF-8 string

	return utf8;
}

int CSlrString::ToInt()
{
	char buf[32];
	u32 len = this->GetLength();
	
	if (len > 30)
		len = 30;

	u32 j = 0;
	for (u32 i = 0; i < len; i++)
	{
		u8 chr = (u8)GetChar(i);
		if (chr > 0x40)
			continue;
		
		buf[j++] = chr;
	}
	buf[j] = 0x00;
	
	return atoi(buf);
}

float CSlrString::ToFloat()
{
	char buf[32];
	u32 len = this->GetLength();
	
	if (len == 0)
		return 0.0f;
	
	if (len > 30)
		len = 30;
	
	u32 j = 0;
	for (u32 i = 0; i < len; i++)
	{
		u8 chr = (u8)GetChar(i);
		if (chr > 0x40)
			continue;
		
		buf[j++] = chr;
	}
	buf[j] = 0x00;
	
	return atof(buf);
}

int CSlrString::ToIntFromHex()
{
	int value;
	char *hexStr = this->GetStdASCII();
	
	char *hexStrParse = hexStr;
	
	if (hexStr[0] == '$')
	{
		hexStrParse = hexStr+1;
	}
	
	sscanf(hexStrParse, "%x", &value);
	STRFREE(hexStr);

	return value;
}

bool CSlrString::ToBool()
{
	if (this->Equals((char*)"yes")
		|| this->Equals((char*)"YES")
		|| this->Equals((char*)"true")
		|| this->Equals((char*)"TRUE"))
	{
		return true;
	}

	if (this->Equals((char*)"no")
		|| this->Equals((char*)"NO")
		|| this->Equals((char*)"false")
		|| this->Equals((char*)"FALSE"))
	{
		return false;
	}

	LOGError("CSlrString::ToBool: unknown value");
	this->DebugPrint("CSlrString::ToBool: value=");
	
	return false;
}

void CSlrString::ConvertToLowerCase()
{
	for (int i = 0; i < this->GetLength(); i++)
	{
		u16 chr = GetChar(i);
		if (chr >= 0x41 && chr <= 0x5A)
		{
			SetChar(i, chr + 0x20);
		}
	}
}


u16 CSlrString::PopCharFront()
{
	if (this->chars->empty())
		return 0;

	u16 chr = this->chars->front();
	this->chars->erase(this->chars->begin());
	return chr;
}

bool CSlrString::IsHexValue()
{
	for (u32 i = 0; i < GetLength(); i++)
	{
		u16 chr = GetChar(i);
		
		if ((chr >= 0x30 && chr <= 0x39)
			|| (chr >= 0x41 && chr <= 0x46)
			|| (chr >= 0x61 && chr <= 0x66)
			|| (chr == '$'))
		{
			continue;
		}
		
		return false;
	}
	
	return true;
}

void CSlrString::DebugPrint()
{
	char *buf = SYS_GetCharBuf();
	u16 l = 0;
	
	//LOGD("len=%d", GetLength());
	for (u32 i = 0; i < GetLength(); i++)
	{
		char c = (char)GetChar(i);
		//LOGD("i=%d l=%d c=%2.2x '%c'", i, l, c, c);
		buf[l++] = c;
	}
	
	buf[l] = 0x00;
	
	LOGD(buf);
	SYS_ReleaseCharBuf(buf);
}

void CSlrString::DebugPrint(const char *name)
{
	char *buf = SYS_GetCharBuf();
	sprintf(buf, "%s", name);

	u16 l = strlen(buf);

	//LOGD("len=%d", GetLength());
	for (u32 i = 0; i < GetLength(); i++)
	{
		char c = (char)GetChar(i);
//		LOGD("i=%d l=%d c=%2.2x '%c'", i, l, c, c);
		buf[l++] = c;
	}

	buf[l] = 0x00;

	LOGD(buf);
	SYS_ReleaseCharBuf(buf);
}

void CSlrString::DebugPrint(const char *name, u32 pos)
{
	char *buf = SYS_GetCharBuf();
	sprintf(buf, "pos=%d %s", pos, name);

	u16 l = strlen(buf);
	for (u32 i = 0; i < GetLength(); i++)
	{
		if (i == pos)
		{
			buf[l++] = '!';
			buf[l++] = '{';
		}

		buf[l++] = (char)GetChar(i);

		if (i == pos)
		{
			buf[l++] = '}';
			buf[l++] = '!';
		}
	}

	buf[l] = 0x00;

	LOGD(buf);
	SYS_ReleaseCharBuf(buf);
}

void CSlrString::DebugPrint(FILE *fp)
{
	for (u32 i = 0; i < GetLength(); i++)
	{
		u16 chr = GetChar(i);
		fwrite(&chr, 1, 2, fp);
	}
}

void CSlrString::DebugPrintVector()
{
	LOGD("CSlrString::DebugPrintVector: len=%d", GetLength());
	
	for (u32 i = 0; i < GetLength(); i++)
	{
		u16 c = GetChar(i);
		LOGD("  -> i=%d c=%x '%c'", i, c, c);
	}
}

void CSlrString::DebugPrintVector(const char *name)
{
	LOGD("CSlrString::DebugPrintVector: %s len=%d", name, GetLength());
	
	for (u32 i = 0; i < GetLength(); i++)
	{
		u16 c = GetChar(i);
		LOGD("  -> i=%d c=%x '%c'", i, c, c);
	}
}


void CSlrString::DeleteVector(std::vector<CSlrString *> *vect)
{
	while(!vect->empty())
	{
		CSlrString *val = vect->back();
		vect->pop_back();

		delete val;
	}

	delete vect;
}

void CSlrString::DeleteVectorElements(std::vector<CSlrString *> *vect)
{
	LOGD("CSlrString::DeleteVectorElements: vect=%x", vect);
	LOGD("                                  size=%d", vect->size());
	
	while(!vect->empty())
	{
		CSlrString *val = vect->back();
		vect->pop_back();
		
		delete val;
	}
}


void CSlrString::DeleteList(std::list<CSlrString *> *list)
{
	while(!list->empty())
	{
		CSlrString *val = list->back();
		list->pop_back();
		
		delete val;
	}
	
	delete list;
}

void CSlrString::DeleteListElements(std::list<CSlrString *> *list)
{
	while(!list->empty())
	{
		CSlrString *val = list->back();
		list->pop_back();
		
		delete val;
	}
}

CSlrStringIterator::CSlrStringIterator(CSlrString *str)
{
	this->str = str;
	this->pos = 0;
}

u16 CSlrStringIterator::GetChar()
{
	if (this->IsEnd())
		return 0;
	
	u16 chr = str->GetChar(pos);
	pos++;
	
	return chr;
}

bool CSlrStringIterator::IsEnd()
{
	if (str->GetLength() == pos)
		return true;
	
	return false;
}

CSlrString *CSlrString::GetFileNameComponentFromPath()
{
	std::vector<CSlrString *> *strs = this->Split(SYS_FILE_SYSTEM_PATH_SEPARATOR);
	if (strs->empty())
		return new CSlrString(this);
	
	CSlrString *fname = new CSlrString(strs->back());
	
	while(!strs->empty())
	{
		CSlrString *t = strs->back();
		strs->pop_back();
		delete t;
	}
	delete strs;

	return fname;
}

CSlrString *CSlrString::GetFileExtensionComponentFromPath()
{
	CSlrString *fname = GetFileNameComponentFromPath();
	std::vector<CSlrString *> *strs = this->Split(SYS_FILE_SYSTEM_EXTENSION_SEPARATOR);
	
	if (strs->empty())
	{
		LOGError("CSlrString::GetFileExtensionComponentFromPath: empty string");
		delete strs;
		delete fname;
		return new CSlrString("");
	}
	
	CSlrString *fext = new CSlrString(strs->back());
	
	while(!strs->empty())
	{
		CSlrString *t = strs->back();
		strs->pop_back();
		delete t;
	}
	delete strs;
	delete fname;
	
	return fext;
}


CSlrString *CSlrString::GetFilePathWithoutFileNameComponentFromPath()
{
//	LOGD("GetFilePathWithoutFileNameComponentFromPath");
//	this->DebugPrint("this=");

	std::vector<CSlrString *> *strs = this->Split(SYS_FILE_SYSTEM_PATH_SEPARATOR);
	CSlrString *fpath = new CSlrString();
	
	if (!strs->empty())
	{
#if !defined(WIN32)
		fpath->Concatenate(SYS_FILE_SYSTEM_PATH_SEPARATOR);
#endif
		
		for (int i = 0; i < strs->size()-1; i++)
		{
			CSlrString *t = (*strs)[i];
			fpath->Concatenate(t);
			fpath->Concatenate(SYS_FILE_SYSTEM_PATH_SEPARATOR);
		}
		
		while(!strs->empty())
		{
			CSlrString *t = strs->back();
			strs->pop_back();
			delete t;
		}
	}
	
	delete strs;
	
//	fpath->DebugPrint("return=");
	return fpath;
}

CSlrString *CSlrString::GetFilePathWithoutExtension()
{
	LOGD("GetFilePathWithoutExtension");
	this->DebugPrint("this=");

	if (!this->Contains(SYS_FILE_SYSTEM_EXTENSION_SEPARATOR))
	{
		CSlrString *fpath = new CSlrString(this);
		return fpath;
	}
	
	std::vector<CSlrString *> *strs = this->Split(SYS_FILE_SYSTEM_EXTENSION_SEPARATOR);
	CSlrString *fpath = new CSlrString();

	if (!strs->empty())
	{
		for (int i = 0; i < strs->size()-1; i++)
		{
			CSlrString *t = (*strs)[i];
			fpath->Concatenate(t);
			
			if (i != strs->size()-2)
			{
				fpath->Concatenate(SYS_FILE_SYSTEM_EXTENSION_SEPARATOR);
			}
		}
		
		while(!strs->empty())
		{
			CSlrString *t = strs->back();
			strs->pop_back();
			delete t;
		}
	}
	
	delete strs;
	
	//	fpath->DebugPrint("return=");
	return fpath;
}

CSlrString *CSlrString::GetFileNameWithoutExtensionAndPath()
{
	CSlrString *fileName = this->GetFileNameComponentFromPath();
	CSlrString *fileNameWithoutExt = fileName->GetFilePathWithoutExtension();
	delete fileName;
	return fileNameWithoutExt;
}

char *cppstrdup(const char *str)
{
	char *copyStr = new char[strlen(str)+1];
	strcpy(copyStr, str);
	return copyStr;
}

void cppstrfree(const char *str)
{
	delete [] str;
}

// utf8 NOT TESTED
// code taken from: https://stackoverflow.com/questions/36897781/how-to-uppercase-lowercase-utf-8-characters-in-c
unsigned char* NOT_TESTED_Utf8StringToLower(unsigned char* pString)
{
	unsigned char* p = pString;
	unsigned char* pExtChar = 0;

	if (pString && *pString) {
		while (*p) {
			if ((*p >= 0x41) && (*p <= 0x5a)) /* US ASCII */
				(*p) += 0x20;
			else if (*p > 0xc0) {
				pExtChar = p;
				p++;
				switch (*pExtChar) {
				case 0xc3: /* Latin 1 */
					if ((*p >= 0x80)
						&& (*p <= 0x9e)
						&& (*p != 0x97))
						(*p) += 0x20; /* US ASCII shift */
					break;
				case 0xc4: /* Latin ext */
					if (((*p >= 0x80)
						&& (*p <= 0xb7)
						&& (*p != 0xb0))
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					else if ((*p >= 0xb9)
						&& (*p <= 0xbe)
						&& (*p % 2)) /* Odd */
						(*p)++; /* Next char is lwr */
					else if (*p == 0xbf) {
						*pExtChar = 0xc5;
						(*p) = 0x80;
					}
					break;
				case 0xc5: /* Latin ext */
					if ((*p >= 0x81)
						&& (*p <= 0x88)
						&& (*p % 2)) /* Odd */
						(*p)++; /* Next char is lwr */
					else if ((*p >= 0x8a)
						&& (*p <= 0xb7)
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					else if (*p == 0xb8) {
						*pExtChar = 0xc3;
						(*p) = 0xbf;
					}
					else if ((*p >= 0xb9)
						&& (*p <= 0xbe)
						&& (*p % 2)) /* Odd */
						(*p)++; /* Next char is lwr */
					break;
				case 0xc6: /* Latin ext */
					switch (*p) {
					case 0x81:
						*pExtChar = 0xc9;
						(*p) = 0x93;
						break;
					case 0x86:
						*pExtChar = 0xc9;
						(*p) = 0x94;
						break;
					case 0x89:
						*pExtChar = 0xc9;
						(*p) = 0x96;
						break;
					case 0x8a:
						*pExtChar = 0xc9;
						(*p) = 0x97;
						break;
					case 0x8e:
						*pExtChar = 0xc9;
						(*p) = 0x98;
						break;
					case 0x8f:
						*pExtChar = 0xc9;
						(*p) = 0x99;
						break;
					case 0x90:
						*pExtChar = 0xc9;
						(*p) = 0x9b;
						break;
					case 0x93:
						*pExtChar = 0xc9;
						(*p) = 0xa0;
						break;
					case 0x94:
						*pExtChar = 0xc9;
						(*p) = 0xa3;
						break;
					case 0x96:
						*pExtChar = 0xc9;
						(*p) = 0xa9;
						break;
					case 0x97:
						*pExtChar = 0xc9;
						(*p) = 0xa8;
						break;
					case 0x9c:
						*pExtChar = 0xc9;
						(*p) = 0xaf;
						break;
					case 0x9d:
						*pExtChar = 0xc9;
						(*p) = 0xb2;
						break;
					case 0x9f:
						*pExtChar = 0xc9;
						(*p) = 0xb5;
						break;
					case 0xa9:
						*pExtChar = 0xca;
						(*p) = 0x83;
						break;
					case 0xae:
						*pExtChar = 0xca;
						(*p) = 0x88;
						break;
					case 0xb1:
						*pExtChar = 0xca;
						(*p) = 0x8a;
						break;
					case 0xb2:
						*pExtChar = 0xca;
						(*p) = 0x8b;
						break;
					case 0xb7:
						*pExtChar = 0xca;
						(*p) = 0x92;
						break;
					case 0x82:
					case 0x84:
					case 0x87:
					case 0x8b:
					case 0x91:
					case 0x98:
					case 0xa0:
					case 0xa2:
					case 0xa4:
					case 0xa7:
					case 0xac:
					case 0xaf:
					case 0xb3:
					case 0xb5:
					case 0xb8:
					case 0xbc:
						(*p)++; /* Next char is lwr */
						break;
					default:
						break;
					}
					break;
				case 0xc7: /* Latin ext */
					if (*p == 0x84)
						(*p) = 0x86;
					else if (*p == 0x85)
						(*p)++; /* Next char is lwr */
					else if (*p == 0x87)
						(*p) = 0x89;
					else if (*p == 0x88)
						(*p)++; /* Next char is lwr */
					else if (*p == 0x8a)
						(*p) = 0x8c;
					else if (*p == 0x8b)
						(*p)++; /* Next char is lwr */
					else if ((*p >= 0x8d)
						&& (*p <= 0x9c)
						&& (*p % 2)) /* Odd */
						(*p)++; /* Next char is lwr */
					else if ((*p >= 0x9e)
						&& (*p <= 0xaf)
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					else if (*p == 0xb1)
						(*p) = 0xb3;
					else if (*p == 0xb2)
						(*p)++; /* Next char is lwr */
					else if (*p == 0xb4)
						(*p)++; /* Next char is lwr */
					else if (*p == 0xb6) {
						*pExtChar = 0xc6;
						(*p) = 0x95;
					}
					else if (*p == 0xb7) {
						*pExtChar = 0xc6;
						(*p) = 0xbf;
					}
					else if ((*p >= 0xb8)
						&& (*p <= 0xbf)
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					break;
				case 0xc8: /* Latin ext */
					if ((*p >= 0x80)
						&& (*p <= 0x9f)
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					else if (*p == 0xa0) {
						*pExtChar = 0xc6;
						(*p) = 0x9e;
					}
					else if ((*p >= 0xa2)
						&& (*p <= 0xb3)
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					else if (*p == 0xbb)
						(*p)++; /* Next char is lwr */
					else if (*p == 0xbd) {
						*pExtChar = 0xc6;
						(*p) = 0x9a;
					}
					/* 0xba three byte small 0xe2 0xb1 0xa5 */
					/* 0xbe three byte small 0xe2 0xb1 0xa6 */
					break;
				case 0xc9: /* Latin ext */
					if (*p == 0x81)
						(*p)++; /* Next char is lwr */
					else if (*p == 0x83) {
						*pExtChar = 0xc6;
						(*p) = 0x80;
					}
					else if (*p == 0x84) {
						*pExtChar = 0xca;
						(*p) = 0x89;
					}
					else if (*p == 0x85) {
						*pExtChar = 0xca;
						(*p) = 0x8c;
					}
					else if ((*p >= 0x86)
						&& (*p <= 0x8f)
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					break;
				case 0xcd: /* Greek & Coptic */
					switch (*p) {
					case 0xb0:
					case 0xb2:
					case 0xb6:
						(*p)++; /* Next char is lwr */
						break;
					case 0xbf:
						*pExtChar = 0xcf;
						(*p) = 0xb3;
						break;
					default:
						break;
					}
					break;
				case 0xce: /* Greek & Coptic */
					if (*p == 0x86)
						(*p) = 0xac;
					else if (*p == 0x88)
						(*p) = 0xad;
					else if (*p == 0x89)
						(*p) = 0xae;
					else if (*p == 0x8a)
						(*p) = 0xaf;
					else if (*p == 0x8c) {
						*pExtChar = 0xcf;
						(*p) = 0x8c;
					}
					else if (*p == 0x8e) {
						*pExtChar = 0xcf;
						(*p) = 0x8d;
					}
					else if (*p == 0x8f) {
						*pExtChar = 0xcf;
						(*p) = 0x8e;
					}
					else if ((*p >= 0x91)
						&& (*p <= 0x9f))
						(*p) += 0x20; /* US ASCII shift */
					else if ((*p >= 0xa0)
						&& (*p <= 0xab)
						&& (*p != 0xa2)) {
						*pExtChar = 0xcf;
						(*p) -= 0x20;
					}
					break;
				case 0xcf: /* Greek & Coptic */
					if (*p == 0x8f)
						(*p) = 0x97;
					else if ((*p >= 0x98)
						&& (*p <= 0xaf)
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					else if (*p == 0xb4) {
						(*p) = 0x91;
					}
					else if (*p == 0xb7)
						(*p)++; /* Next char is lwr */
					else if (*p == 0xb9)
						(*p) = 0xb2;
					else if (*p == 0xba)
						(*p)++; /* Next char is lwr */
					else if (*p == 0xbd) {
						*pExtChar = 0xcd;
						(*p) = 0xbb;
					}
					else if (*p == 0xbe) {
						*pExtChar = 0xcd;
						(*p) = 0xbc;
					}
					else if (*p == 0xbf) {
						*pExtChar = 0xcd;
						(*p) = 0xbd;
					}
					break;
				case 0xd0: /* Cyrillic */
					if ((*p >= 0x80)
						&& (*p <= 0x8f)) {
						*pExtChar = 0xd1;
						(*p) += 0x10;
					}
					else if ((*p >= 0x90)
						&& (*p <= 0x9f))
						(*p) += 0x20; /* US ASCII shift */
					else if ((*p >= 0xa0)
						&& (*p <= 0xaf)) {
						*pExtChar = 0xd1;
						(*p) -= 0x20;
					}
					break;
				case 0xd1: /* Cyrillic supplement */
					if ((*p >= 0xa0)
						&& (*p <= 0xbf)
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					break;
				case 0xd2: /* Cyrillic supplement */
					if (*p == 0x80)
						(*p)++; /* Next char is lwr */
					else if ((*p >= 0x8a)
						&& (*p <= 0xbf)
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					break;
				case 0xd3: /* Cyrillic supplement */
					if (*p == 0x80)
						(*p) = 0x8f;
					else if ((*p >= 0x81)
						&& (*p <= 0x8e)
						&& (*p % 2)) /* Odd */
						(*p)++; /* Next char is lwr */
					else if ((*p >= 0x90)
						&& (*p <= 0xbf)
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					break;
				case 0xd4: /* Cyrillic supplement & Armenian */
					if ((*p >= 0x80)
						&& (*p <= 0xaf)
						&& (!(*p % 2))) /* Even */
						(*p)++; /* Next char is lwr */
					else if ((*p >= 0xb1)
						&& (*p <= 0xbf)) {
						*pExtChar = 0xd5;
						(*p) -= 0x10;
					}
					break;
				case 0xd5: /* Armenian */
					if ((*p >= 0x80)
						&& (*p <= 0x8f)) {
						(*p) += 0x30;
					}
					else if ((*p >= 0x90)
						&& (*p <= 0x96)) {
						*pExtChar = 0xd6;
						(*p) -= 0x10;
					}
					break;
				case 0xe1: /* Three byte code */
					pExtChar = p;
					p++;
					switch (*pExtChar) {
					case 0x82: /* Georgian asomtavruli */
						if ((*p >= 0xa0)
							&& (*p <= 0xbf)) {
							*pExtChar = 0x83;
							(*p) -= 0x10;
						}
						break;
					case 0x83: /* Georgian asomtavruli */
						if (((*p >= 0x80)
							&& (*p <= 0x85))
							|| (*p == 0x87)
							|| (*p == 0x8d))
							(*p) += 0x30;
						break;
					case 0x8e: /* Cherokee */
						if ((*p >= 0xa0)
							&& (*p <= 0xaf)) {
							*(p - 2) = 0xea;
							*pExtChar = 0xad;
							(*p) += 0x10;
						}
						else if ((*p >= 0xb0)
							&& (*p <= 0xbf)) {
							*(p - 2) = 0xea;
							*pExtChar = 0xae;
							(*p) -= 0x30;
						}
						break;
					case 0x8f: /* Cherokee */
						if ((*p >= 0x80)
							&& (*p <= 0xaf)) {
							*(p - 2) = 0xea;
							*pExtChar = 0xae;
							(*p) += 0x10;
						}
						else if ((*p >= 0xb0)
							&& (*p <= 0xb5)) {
							(*p) += 0x08;
						}
						/* 0xbe three byte small 0xe2 0xb1 0xa6 */
						break;
					case 0xb2: /* Georgian mtavruli */
						if (((*p >= 0x90)
							&& (*p <= 0xba))
							|| (*p == 0xbd)
							|| (*p == 0xbe)
							|| (*p == 0xbf))
							*pExtChar = 0x83;
						break;
					case 0xb8: /* Latin ext */
						if ((*p >= 0x80)
							&& (*p <= 0xbf)
							&& (!(*p % 2))) /* Even */
							(*p)++; /* Next char is lwr */
						break;
					case 0xb9: /* Latin ext */
						if ((*p >= 0x80)
							&& (*p <= 0xbf)
							&& (!(*p % 2))) /* Even */
							(*p)++; /* Next char is lwr */
						break;
					case 0xba: /* Latin ext */
						if ((*p >= 0x80)
							&& (*p <= 0x94)
							&& (!(*p % 2))) /* Even */
							(*p)++; /* Next char is lwr */
						else if ((*p >= 0xa0)
							&& (*p <= 0xbf)
							&& (!(*p % 2))) /* Even */
							(*p)++; /* Next char is lwr */
						/* 0x9e Two byte small 0xc3 0x9f */
						break;
					case 0xbb: /* Latin ext */
						if ((*p >= 0x80)
							&& (*p <= 0xbf)
							&& (!(*p % 2))) /* Even */
							(*p)++; /* Next char is lwr */
						break;
					case 0xbc: /* Greek ex */
						if ((*p >= 0x88)
							&& (*p <= 0x8f))
							(*p) -= 0x08;
						else if ((*p >= 0x98)
							&& (*p <= 0x9d))
							(*p) -= 0x08;
						else if ((*p >= 0xa8)
							&& (*p <= 0xaf))
							(*p) -= 0x08;
						else if ((*p >= 0xb8)
							&& (*p <= 0xbf))
							(*p) -= 0x08;
						break;
					case 0xbd: /* Greek ex */
						if ((*p >= 0x88)
							&& (*p <= 0x8d))
							(*p) -= 0x08;
						else if ((*p == 0x99)
							|| (*p == 0x9b)
							|| (*p == 0x9d)
							|| (*p == 0x9f))
							(*p) -= 0x08;
						else if ((*p >= 0xa8)
							&& (*p <= 0xaf))
							(*p) -= 0x08;
						break;
					case 0xbe: /* Greek ex */
						if ((*p >= 0x88)
							&& (*p <= 0x8f))
							(*p) -= 0x08;
						else if ((*p >= 0x98)
							&& (*p <= 0x9f))
							(*p) -= 0x08;
						else if ((*p >= 0xa8)
							&& (*p <= 0xaf))
							(*p) -= 0x08;
						else if ((*p >= 0xb8)
							&& (*p <= 0xb9))
							(*p) -= 0x08;
						else if ((*p >= 0xba)
							&& (*p <= 0xbb)) {
							*(p - 1) = 0xbd;
							(*p) -= 0x0a;
						}
						else if (*p == 0xbc)
							(*p) -= 0x09;
						break;
					case 0xbf: /* Greek ex */
						if ((*p >= 0x88)
							&& (*p <= 0x8b)) {
							*(p - 1) = 0xbd;
							(*p) += 0x2a;
						}
						else if (*p == 0x8c)
							(*p) -= 0x09;
						else if ((*p >= 0x98)
							&& (*p <= 0x99))
							(*p) -= 0x08;
						else if ((*p >= 0x9a)
							&& (*p <= 0x9b)) {
							*(p - 1) = 0xbd;
							(*p) += 0x1c;
						}
						else if ((*p >= 0xa8)
							&& (*p <= 0xa9))
							(*p) -= 0x08;
						else if ((*p >= 0xaa)
							&& (*p <= 0xab)) {
							*(p - 1) = 0xbd;
							(*p) += 0x10;
						}
						else if (*p == 0xac)
							(*p) -= 0x07;
						else if ((*p >= 0xb8)
							&& (*p <= 0xb9)) {
							*(p - 1) = 0xbd;
						}
						else if ((*p >= 0xba)
							&& (*p <= 0xbb)) {
							*(p - 1) = 0xbd;
							(*p) += 0x02;
						}
						else if (*p == 0xbc)
							(*p) -= 0x09;
						break;
					default:
						break;
					}
					break;
				case 0xe2: /* Three byte code */
					pExtChar = p;
					p++;
					switch (*pExtChar) {
					case 0xb0: /* Glagolitic */
						if ((*p >= 0x80)
							&& (*p <= 0x8f)) {
							(*p) += 0x30;
						}
						else if ((*p >= 0x90)
							&& (*p <= 0xae)) {
							*pExtChar = 0xb1;
							(*p) -= 0x10;
						}
						break;
					case 0xb1: /* Latin ext */
						switch (*p) {
						case 0xa0:
						case 0xa7:
						case 0xa9:
						case 0xab:
						case 0xb2:
						case 0xb5:
							(*p)++; /* Next char is lwr */
							break;
						case 0xa2: /* Two byte small 0xc9 0xab */
						case 0xa4: /* Two byte small 0xc9 0xbd */
						case 0xad: /* Two byte small 0xc9 0x91 */
						case 0xae: /* Two byte small 0xc9 0xb1 */
						case 0xaf: /* Two byte small 0xc9 0x90 */
						case 0xb0: /* Two byte small 0xc9 0x92 */
						case 0xbe: /* Two byte small 0xc8 0xbf */
						case 0xbf: /* Two byte small 0xc9 0x80 */
							break;
						case 0xa3:
							*(p - 2) = 0xe1;
							*(p - 1) = 0xb5;
							*(p) = 0xbd;
							break;
						default:
							break;
						}
						break;
					case 0xb2: /* Coptic */
						if ((*p >= 0x80)
							&& (*p <= 0xbf)
							&& (!(*p % 2))) /* Even */
							(*p)++; /* Next char is lwr */
						break;
					case 0xb3: /* Coptic */
						if (((*p >= 0x80)
							&& (*p <= 0xa3)
							&& (!(*p % 2))) /* Even */
							|| (*p == 0xab)
							|| (*p == 0xad)
							|| (*p == 0xb2))
							(*p)++; /* Next char is lwr */
						break;
					case 0xb4: /* Georgian nuskhuri */
						if (((*p >= 0x80)
							&& (*p <= 0xa5))
							|| (*p == 0xa7)
							|| (*p == 0xad)) {
							*(p - 2) = 0xe1;
							*(p - 1) = 0x83;
							(*p) += 0x10;
						}
						break;
					default:
						break;
					}
					break;
				case 0xea: /* Three byte code */
					pExtChar = p;
					p++;
					switch (*pExtChar) {
					case 0x99: /* Cyrillic */
						if ((*p >= 0x80)
							&& (*p <= 0xad)
							&& (!(*p % 2))) /* Even */
							(*p)++; /* Next char is lwr */
						break;
					case 0x9a: /* Cyrillic */
						if ((*p >= 0x80)
							&& (*p <= 0x9b)
							&& (!(*p % 2))) /* Even */
							(*p)++; /* Next char is lwr */
						break;
					case 0x9c: /* Latin ext */
						if ((((*p >= 0xa2)
							&& (*p <= 0xaf))
							|| ((*p >= 0xb2)
								&& (*p <= 0xbf)))
							&& (!(*p % 2))) /* Even */
							(*p)++; /* Next char is lwr */
						break;
					case 0x9d: /* Latin ext */
						if ((((*p >= 0x80)
							&& (*p <= 0xaf))
							&& (!(*p % 2))) /* Even */
							|| (*p == 0xb9)
							|| (*p == 0xbb)
							|| (*p == 0xbe))
							(*p)++; /* Next char is lwr */
						else if (*p == 0xbd) {
							*(p - 2) = 0xe1;
							*(p - 1) = 0xb5;
							*(p) = 0xb9;
						}
						break;
					case 0x9e: /* Latin ext */
						if (((((*p >= 0x80)
							&& (*p <= 0x87))
							|| ((*p >= 0x96)
								&& (*p <= 0xa9))
							|| ((*p >= 0xb4)
								&& (*p <= 0xbf)))
							&& (!(*p % 2))) /* Even */
							|| (*p == 0x8b)
							|| (*p == 0x90)
							|| (*p == 0x92))
							(*p)++; /* Next char is lwr */
						else if (*p == 0xb3) {
							*(p - 2) = 0xea;
							*(p - 1) = 0xad;
							*(p) = 0x93;
						}
						/* case 0x8d: // Two byte small 0xc9 0xa5 */
						/* case 0xaa: // Two byte small 0xc9 0xa6 */
						/* case 0xab: // Two byte small 0xc9 0x9c */
						/* case 0xac: // Two byte small 0xc9 0xa1 */
						/* case 0xad: // Two byte small 0xc9 0xac */
						/* case 0xae: // Two byte small 0xc9 0xaa */
						/* case 0xb0: // Two byte small 0xca 0x9e */
						/* case 0xb1: // Two byte small 0xca 0x87 */
						/* case 0xb2: // Two byte small 0xca 0x9d */
						break;
					case 0x9f: /* Latin ext */
						if ((*p == 0x82)
							|| (*p == 0x87)
							|| (*p == 0x89)
							|| (*p == 0xb5))
							(*p)++; /* Next char is lwr */
						else if (*p == 0x84) {
							*(p - 2) = 0xea;
							*(p - 1) = 0x9e;
							*(p) = 0x94;
						}
						else if (*p == 0x86) {
							*(p - 2) = 0xe1;
							*(p - 1) = 0xb6;
							*(p) = 0x8e;
						}
						/* case 0x85: // Two byte small 0xca 0x82 */
						break;
					default:
						break;
					}
					break;
				case 0xef: /* Three byte code */
					pExtChar = p;
					p++;
					switch (*pExtChar) {
					case 0xbc: /* Latin fullwidth */
						if ((*p >= 0xa1)
							&& (*p <= 0xba)) {
							*pExtChar = 0xbd;
							(*p) -= 0x20;
						}
						break;
					default:
						break;
					}
					break;
				case 0xf0: /* Four byte code */
					pExtChar = p;
					p++;
					switch (*pExtChar) {
					case 0x90:
						pExtChar = p;
						p++;
						switch (*pExtChar) {
						case 0x90: /* Deseret */
							if ((*p >= 0x80)
								&& (*p <= 0x97)) {
								(*p) += 0x28;
							}
							else if ((*p >= 0x98)
								&& (*p <= 0xa7)) {
								*pExtChar = 0x91;
								(*p) -= 0x18;
							}
							break;
						case 0x92: /* Osage  */
							if ((*p >= 0xb0)
								&& (*p <= 0xbf)) {
								*pExtChar = 0x93;
								(*p) -= 0x18;
							}
							break;
						case 0x93: /* Osage  */
							if ((*p >= 0x80)
								&& (*p <= 0x93))
								(*p) += 0x28;
							break;
						case 0xb2: /* Old hungarian */
							if ((*p >= 0x80)
								&& (*p <= 0xb2))
								*pExtChar = 0xb3;
							break;
						default:
							break;
						}
						break;
					case 0x91:
						pExtChar = p;
						p++;
						switch (*pExtChar) {
						case 0xa2: /* Warang citi */
							if ((*p >= 0xa0)
								&& (*p <= 0xbf)) {
								*pExtChar = 0xa3;
								(*p) -= 0x20;
							}
							break;
						default:
							break;
						}
						break;
					case 0x96:
						pExtChar = p;
						p++;
						switch (*pExtChar) {
						case 0xb9: /* Medefaidrin */
							if ((*p >= 0x80)
								&& (*p <= 0x9f)) {
								(*p) += 0x20;
							}
							break;
						default:
							break;
						}
						break;
					case 0x9E:
						pExtChar = p;
						p++;
						switch (*pExtChar) {
						case 0xA4: /* Adlam */
							if ((*p >= 0x80)
								&& (*p <= 0x9d))
								(*p) += 0x22;
							else if ((*p >= 0x9e)
								&& (*p <= 0xa1)) {
								*(pExtChar) = 0xa5;
								(*p) -= 0x1e;
							}
							break;
						default:
							break;
						}
						break;
					default:
						break;
					}
					break;
				default:
					break;
				}
				pExtChar = 0;
			}
			p++;
		}
	}
	return pString;
}

int NOT_TESTED_Utf8StrnCiCmp(const char* s1, const char* s2, size_t ztCount)
{
	unsigned char* pStr1Low = 0;
	unsigned char* pStr2Low = 0;
	unsigned char* p1 = 0;
	unsigned char* p2 = 0;

	if (s1 && *s1 && s2 && *s2) {
		char cExtChar = 0;
		pStr1Low = (unsigned char*)calloc(strlen(s1) + 1, sizeof(unsigned char));
		if (pStr1Low) {
			pStr2Low = (unsigned char*)calloc(strlen(s2) + 1, sizeof(unsigned char));
			if (pStr2Low) {
				p1 = pStr1Low;
				p2 = pStr2Low;
				strcpy((char*)pStr1Low, s1);
				strcpy((char*)pStr2Low, s2);
				NOT_TESTED_Utf8StringToLower(pStr1Low);
				NOT_TESTED_Utf8StringToLower(pStr2Low);
				for (; ztCount--; p1++, p2++) {
					int iDiff = *p1 - *p2;
					if (iDiff != 0 || !*p1 || !*p2) {
						free(pStr1Low);
						free(pStr2Low);
						return iDiff;
					}
				}
				free(pStr1Low);
				free(pStr2Low);
				return 0;
			}
			free(pStr1Low);
			return (-1);
		}
		return (-1);
	}
	return (-1);
}

int NOT_TESTED_Utf8StrCiCmp(const char* s1, const char* s2)
{
	return NOT_TESTED_Utf8StrnCiCmp(s1, s2, (size_t)(-1));
}

char* NOT_TESTED_Utf8StrCiStr(const char* s1, const char* s2)
{
	char* p = (char*)s1;
	size_t len = 0;

	if (s1 && *s1 && s2 && *s2) {
		len = strlen(s2);
		while (*p) {
			if (NOT_TESTED_Utf8StrnCiCmp(p, s2, len) == 0)
				return (char*)p;
			p++;
		}
	}
	return (0);
}

/// Basic Latin, Latin-1 Supplement, 	Latin Extended-A
std::unordered_map<uint32_t, uint32_t> unicodeLowercaseMap = {
	{0x0041, 0x0061}, {0x0042, 0x0062}, {0x0043, 0x0063}, {0x0044, 0x0064}, {0x0045, 0x0065}, {0x0046, 0x0066}, {0x0047, 0x0067}, {0x0048, 0x0068}, {0x0049, 0x0069}, {0x004A, 0x006A},
	{0x004B, 0x006B}, {0x004C, 0x006C}, {0x004D, 0x006D}, {0x004E, 0x006E}, {0x004F, 0x006F}, {0x0050, 0x0070},	{0x0051, 0x0071}, {0x0052, 0x0072},	{0x0053, 0x0073}, {0x0054, 0x0074},
	{0x0055, 0x0075}, {0x0056, 0x0076}, {0x0057, 0x0077}, {0x0058, 0x0078}, {0x0059, 0x0079}, {0x005A, 0x007A}, {0x00C0, 0x00E0}, {0x00C1, 0x00E1}, {0x00C2, 0x00E2}, {0x00C3, 0x00E3},
	{0x00C4, 0x00E4}, {0x00C5, 0x00E5}, {0x00C6, 0x00E6}, {0x00C7, 0x00E7}, {0x00C8, 0x00E8}, {0x00C9, 0x00E9}, {0x00CA, 0x00EA}, {0x00CB, 0x00EB}, {0x00CC, 0x00EC}, {0x00CD, 0x00ED},
	{0x00CE, 0x00EE}, {0x00CF, 0x00EF}, {0x00D0, 0x00F0}, {0x00D1, 0x00F1}, {0x00D2, 0x00F2}, {0x00D3, 0x00F3}, {0x00D4, 0x00F4}, {0x00D5, 0x00F5}, {0x00D6, 0x00F6}, {0x00D8, 0x00F8},
	{0x00D9, 0x00F9}, {0x00DA, 0x00FA}, {0x00DB, 0x00FB}, {0x00DC, 0x00FC}, {0x00DD, 0x00FD}, {0x00DE, 0x00FE}, {0x0100, 0x0101}, {0x0102, 0x0103}, {0x0104, 0x0105}, {0x0106, 0x0107},
	{0x0108, 0x0109}, {0x010A, 0x010B}, {0x010C, 0x010D}, {0x010E, 0x010F}, {0x0110, 0x0111}, {0x0112, 0x0113}, {0x0114, 0x0115}, {0x0116, 0x0117}, {0x0118, 0x0119}, {0x011A, 0x011B},
	{0x011C, 0x011D}, {0x011E, 0x011F}, {0x0120, 0x0121}, {0x0122, 0x0123}, {0x0124, 0x0125}, {0x0126, 0x0127}, {0x0128, 0x0129}, {0x012A, 0x012B}, {0x012C, 0x012D}, {0x012E, 0x012F},
	{0x0132, 0x0133}, {0x0134, 0x0135}, {0x0136, 0x0137}, {0x0139, 0x013A}, {0x013B, 0x013C}, {0x013D, 0x013E}, {0x013F, 0x0140}, {0x0141, 0x0142}, {0x0143, 0x0144}, {0x0145, 0x0146},
	{0x0147, 0x0148}, {0x014A, 0x014B}, {0x014C, 0x014D}, {0x014E, 0x014F}, {0x0150, 0x0151}, {0x0152, 0x0153}, {0x0154, 0x0155}, {0x0156, 0x0157}, {0x0158, 0x0159}, {0x015A, 0x015B},
	{0x015C, 0x015D}, {0x015E, 0x015F}, {0x0160, 0x0161}, {0x0162, 0x0163}, {0x0164, 0x0165}, {0x0166, 0x0167}, {0x0168, 0x0169}, {0x016A, 0x016B}, {0x016C, 0x016D}, {0x016E, 0x016F},
	{0x0170, 0x0171}, {0x0172, 0x0173}, {0x0174, 0x0175}, {0x0176, 0x0177}, {0x0178, 0x00FF}, {0x0179, 0x017A}, {0x017B, 0x017C}, {0x017D, 0x017E}
};

std::string Utf8StringToLowercase(const std::string& input) {
	std::string result;
	auto it = input.begin();
	while (it != input.end()) {
		uint32_t cp = utf8::next(it, input.end());
		auto found = unicodeLowercaseMap.find(cp);
		if (found != unicodeLowercaseMap.end()) {
			utf8::append(found->second, std::back_inserter(result));
		} else {
			utf8::append(cp, std::back_inserter(result));
		}
	}
	return result;
}

std::unordered_map<uint32_t, uint32_t> unicodeUppercaseMap = {
	{0x0061, 0x0041}, {0x0062, 0x0042}, {0x0063, 0x0043}, {0x0064, 0x0044}, {0x0065, 0x0045}, {0x0066, 0x0046}, {0x0067, 0x0047}, {0x0068, 0x0048}, {0x0069, 0x0049}, {0x006A, 0x004A},
	{0x006B, 0x004B}, {0x006C, 0x004C}, {0x006D, 0x004D}, {0x006E, 0x004E}, {0x006F, 0x004F}, {0x0070, 0x0050}, {0x0071, 0x0051}, {0x0072, 0x0052}, {0x0073, 0x0053}, {0x0074, 0x0054},
	{0x0075, 0x0055}, {0x0076, 0x0056}, {0x0077, 0x0057}, {0x0078, 0x0058}, {0x0079, 0x0059}, {0x007A, 0x005A}, {0x00B5, 0x039C}, {0x00E0, 0x00C0}, {0x00E1, 0x00C1}, {0x00E2, 0x00C2},
	{0x00E3, 0x00C3}, {0x00E4, 0x00C4}, {0x00E5, 0x00C5}, {0x00E6, 0x00C6}, {0x00E7, 0x00C7}, {0x00E8, 0x00C8}, {0x00E9, 0x00C9}, {0x00EA, 0x00CA}, {0x00EB, 0x00CB}, {0x00EC, 0x00CC},
	{0x00ED, 0x00CD}, {0x00EE, 0x00CE}, {0x00EF, 0x00CF}, {0x00F0, 0x00D0}, {0x00F1, 0x00D1}, {0x00F2, 0x00D2}, {0x00F3, 0x00D3}, {0x00F4, 0x00D4}, {0x00F5, 0x00D5}, {0x00F6, 0x00D6},
	{0x00F8, 0x00D8}, {0x00F9, 0x00D9}, {0x00FA, 0x00DA}, {0x00FB, 0x00DB}, {0x00FC, 0x00DC}, {0x00FD, 0x00DD}, {0x00FE, 0x00DE}, {0x00FF, 0x0178}, {0x0101, 0x0100}, {0x0103, 0x0102},
	{0x0105, 0x0104}, {0x0107, 0x0106}, {0x0109, 0x0108}, {0x010B, 0x010A}, {0x010D, 0x010C}, {0x010F, 0x010E},	{0x0111, 0x0110}, {0x0113, 0x0112}, {0x0115, 0x0114}, {0x0117, 0x0116},
	{0x0119, 0x0118}, {0x011B, 0x011A}, {0x011D, 0x011C}, {0x011F, 0x011E}, {0x0121, 0x0120}, {0x0123, 0x0122}, {0x0125, 0x0124}, {0x0127, 0x0126}, {0x0129, 0x0128}, {0x012B, 0x012A},
	{0x012D, 0x012C}, {0x012F, 0x012E}, {0x0131, 0x0049}, {0x0133, 0x0132}, {0x0135, 0x0134}, {0x0137, 0x0136}, {0x013A, 0x0139}, {0x013C, 0x013B}, {0x013E, 0x013D}, {0x0140, 0x013F},
	{0x0142, 0x0141}, {0x0144, 0x0143}, {0x0146, 0x0145}, {0x0148, 0x0147}, {0x014B, 0x014A}, {0x014D, 0x014C}, {0x014F, 0x014E}, {0x0151, 0x0150}, {0x0153, 0x0152}, {0x0155, 0x0154},
	{0x0157, 0x0156}, {0x0159, 0x0158}, {0x015B, 0x015A}, {0x015D, 0x015C}, {0x015F, 0x015E}, {0x0161, 0x0160}, {0x0163, 0x0162}, {0x0165, 0x0164}, {0x0167, 0x0166}, {0x0169, 0x0168},
	{0x016B, 0x016A}, {0x016D, 0x016C}, {0x016F, 0x016E}, {0x0171, 0x0170}, {0x0173, 0x0172}, {0x0175, 0x0174}, {0x0177, 0x0176}, {0x017A, 0x0179}, {0x017C, 0x017B}, {0x017E, 0x017D},
	{0x017F, 0x0053}
};

std::string Utf8StringToUppercase(const std::string& input) {
	std::string result;
	auto it = input.begin();
	while (it != input.end()) {
		uint32_t cp = utf8::next(it, input.end());
		auto found = unicodeUppercaseMap.find(cp);
		if (found != unicodeUppercaseMap.end()) {
			utf8::append(found->second, std::back_inserter(result));
		} else {
			utf8::append(cp, std::back_inserter(result));
		}
	}
	return result;
}

//std::string Utf8StringToLowercase(const std::string& utf8_str)
//{
//	LOGD("Utf8StringToLowercase");
//	LOGD("utf8_str=%s", utf8_str.c_str());
//	std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
//	std::wstring wide_str = conv.from_bytes(utf8_str);
//
//	std::locale loc("");
//	for (wchar_t& c : wide_str) {
//		c = std::tolower(c, loc);
//	}
//
//	return conv.to_bytes(wide_str);
//}

std::vector<u16> StringToUtf16(const std::string& utf8String)
{
	// Create a UTF-8 to UTF-16 converter
	std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;

	// Convert the UTF-8 string to UTF-16 (stored in a u16string)
	std::u16string utf16String = convert.from_bytes(utf8String);

	// Convert the UTF-16 u16string to a vector of uint16_t
	std::vector<uint16_t> utf16Vector(utf16String.begin(), utf16String.end());

	return utf16Vector;
}

std::string CSlrString::GetStdStringUTF8()
{
	std::string utf8result;
	utf8::utf16to8(chars->begin(), chars->end(), back_inserter(utf8result));
	return utf8result;
}

std::string UTF16VectorToUTF8String(const std::vector<char16_t>& utf16vec)
{
	std::u16string utf16str(utf16vec.begin(), utf16vec.end());

	// Convert UTF-16 to UTF-8
	std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
	return convert.to_bytes(utf16str);
}

#ifdef USE_STRINGS_POOL
CPool CSlrString::poolStrings(POOL_SIZE_STRINGS, sizeof(CSlrString));
#endif

