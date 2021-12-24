#include "CSlrString.h"
#include "SYS_Main.h"
#include "CByteBuffer.h"
#include "SYS_FileSystem.h"
#include "SYS_Threading.h"
#include <string>

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
	
	/*
	char *v = GetStdASCII();

	int val = atoi(v);
	delete v;

	return val;*/
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
	
	/*
	char *v = GetStdASCII();

	float val = atof(v);
	delete v;

	return val;*/
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
	delete hexStr;

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
	char buf[1024];
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
}

void CSlrString::DebugPrint(const char *name)
{
	char buf[1024];
	sprintf(buf, "%s", name);

	u16 l = strlen(buf);

	//LOGD("len=%d", GetLength());
	for (u32 i = 0; i < GetLength(); i++)
	{
		char c = (char)GetChar(i);
		//LOGD("i=%d l=%d c=%2.2x '%c'", i, l, c, c);
		buf[l++] = c;
	}

	buf[l] = 0x00;

	LOGD(buf);
}

void CSlrString::DebugPrint(const char *name, u32 pos)
{
	char buf[1024];
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
	LOGD("GetFilePathWithoutFileNameComponentFromPath");
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

#ifdef USE_STRINGS_POOL
CPool CSlrString::poolStrings(POOL_SIZE_STRINGS, sizeof(CSlrString));
#endif


