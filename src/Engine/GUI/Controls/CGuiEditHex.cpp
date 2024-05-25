#include "CGuiEditHex.h"
#include "CSlrString.h"
#include "SYS_KeyCodes.h"

CGuiEditHex::CGuiEditHex(CGuiEditHexCallback *callback)
{
	this->callback = callback;
	this->text = NULL;
	this->textWithCursor = new CSlrString();
	this->isCapitalLetters = true;
}

void CGuiEditHex::SetValue(int value, int numDigits)
{
	originalValueBeforeEsc = value;
	
	char *buf = SYS_GetCharBuf();
	char *bufFormat = SYS_GetCharBuf();
	
	if (isCapitalLetters)
	{
		sprintf(bufFormat, "%%0%dX", numDigits);
	}
	else
	{
		sprintf(bufFormat, "%%0%dx", numDigits);
	}
	
	sprintf(buf, bufFormat, value);
	
	CSlrString *str = new CSlrString(buf);
	SetText(str);
	
	SYS_ReleaseCharBuf(bufFormat);
	SYS_ReleaseCharBuf(buf);
}

void CGuiEditHex::SetText(CSlrString *str)
{
	if (this->text != NULL)
	{
		delete this->text;
	}
	this->text = str;
	
	cursorPos = 0;
	UpdateCursor();
}

void CGuiEditHex::UpdateValue()
{
	if (text == NULL)
		return;
	
	for (int i = 0; i < text->GetLength(); i++)
	{
		u16 chr = this->text->GetChar(i);
		if (chr == '.')
		{
			this->text->SetChar(i, ' ');
		}
	}
	
	char *hexStr = text->GetStdASCII();
	sscanf(hexStr, "%x", &value);
	
	//LOGD("hexStr=%s value=%4.4x", hexStr, value);
	
	delete hexStr;
}

void CGuiEditHex::CancelEntering()
{
	FinalizeEntering(MTKEY_NOTHING, true);
}

void CGuiEditHex::FinalizeEntering(u32 keyCode, bool isCancelled)
{
	if (isCancelled == false)
	{
		UpdateValue();
	}
	else
	{
		value = originalValueBeforeEsc;
	}
	this->callback->GuiEditHexEnteredValue(this, keyCode, isCancelled);
}

void CGuiEditHex::KeyDown(u32 keyCode)
{
	if (keyCode == MTKEY_ENTER)
	{
		FinalizeEntering(keyCode, false);
	}
	else if (keyCode >= '0' && keyCode <= '9')
	{
		this->text->SetChar(cursorPos, keyCode);
		if (cursorPos == text->GetLength()-1)
		{
			FinalizeEntering(keyCode, false);
		}
		else
		{
			cursorPos++;
		}
	}
	else if (keyCode >= 'a' && keyCode <= 'f')
	{
		if (isCapitalLetters)
		{
			this->text->SetChar(cursorPos, keyCode - 0x20);
		}
		else
		{
			this->text->SetChar(cursorPos, keyCode);
		}
		
		if (cursorPos == text->GetLength()-1)
		{
			FinalizeEntering(keyCode, false);
		}
		else
		{
			cursorPos++;
		}
	}
	else if (keyCode == MTKEY_BACKSPACE)
	{
		if (cursorPos > 0)
			cursorPos--;
	}
	else if (keyCode == MTKEY_ARROW_LEFT)
	{
		if (cursorPos > 0)
		{
			cursorPos--;
		}
		else
		{
			FinalizeEntering(keyCode, false);
		}
	}
	else if (keyCode == MTKEY_ARROW_RIGHT)
	{
		if (cursorPos == text->GetLength()-1)
		{
			FinalizeEntering(keyCode, false);
		}
		else
		{
			cursorPos++;
		}
	}
	else if (keyCode == MTKEY_ESC)
	{
		FinalizeEntering(keyCode, true);
	}
	
	UpdateCursor();
	return;
}

void CGuiEditHex::SetCursorPos(int newPos)
{
	LOGD("CGuiEditHex::SetCursorPos=%d", newPos);
	this->cursorPos = newPos;
	UpdateCursor();
}

// TODO: #define CBMSHIFTEDFONT_INVERT	0x80 should be defined elsewhere
#define CBMSHIFTEDFONT_INVERT	0x80

void CGuiEditHex::UpdateCursor()
{
	textWithCursor->Clear();
	for (int i = 0; i < text->GetLength(); i++)
	{
		u16 chr = text->GetChar(i);
		if (cursorPos == i)
		{
			chr += CBMSHIFTEDFONT_INVERT;
		}
		textWithCursor->Concatenate(chr);
	}
}
