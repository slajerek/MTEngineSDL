#include "CDataAdapter.h"

//void CDataAdapterCallback::ReadByteCallback(CDataAdapter *adapter, int pointer, byte value, bool isAvailable)
//{
//}
//
//void CDataAdapterCallback::WriteByteCallback(CDataAdapter *adapter, int pointer, byte value, bool isAvailable)
//{
//}

CDataAdapter::CDataAdapter(const char *adapterID)
{
	this->adapterID = adapterID;
}

CDataAdapter::~CDataAdapter()
{
}

int CDataAdapter::AdapterGetDataLength()
{
	return 0;
}

int CDataAdapter::GetDataOffset()
{
	return 0;
}

void CDataAdapter::AdapterReadByte(int pointer, u8 *value)
{
	bool isAvailable;
	AdapterReadByte(pointer, value, &isAvailable);
}

void CDataAdapter::AdapterWriteByte(int pointer, u8 value)
{
	bool isAvailable;
	AdapterWriteByte(pointer, value, &isAvailable);
}

void CDataAdapter::AdapterReadByte(int pointer, u8 *value, bool *isAvailable)
{
//	if (this->callback != NULL)
//	{
//		this->callback->ReadByteCallback(this, pointer, value, isAvailable);
//	}
}

void CDataAdapter::AdapterWriteByte(int pointer, u8 value, bool *isAvailable)
{
//	if (this->callback != NULL)
//	{
//		this->callback->WriteByteCallback(this, pointer, value, isAvailable);
//	}
}

void CDataAdapter::AdapterReadBlockDirect(uint8 *buffer, int pointerStart, int pointerEnd)
{
	for (int i = pointerStart; i < pointerEnd; i++)
	{
		AdapterReadByte(i, &(buffer[i]));
	}
}

uint8 CDataAdapter::AdapterReadByteModulus(int pointer)
{
	uint8 value;
	AdapterReadByte(pointer % AdapterGetDataLength(), &value);
	return value;
}

void CDataAdapter::AdapterReadByteModulus(int pointer, uint8 *value)
{
	AdapterReadByte(pointer % AdapterGetDataLength(), value);
}

void CDataAdapter::AdapterWriteByteModulus(int pointer, uint8 value)
{
	AdapterWriteByte(pointer % AdapterGetDataLength(), value);
}

int countDigits(int num) {
	int count = 0;
	
	// Handle the case of negative numbers
	if (num < 0) {
		count++;  // Account for the negative sign
		num = -num;  // Make the number positive
	}
	
	// Count the number of digits
	do {
		count++;
		num /= 10;
	} while (num != 0);
	
	return count;
}

void CDataAdapter::GetAddressStringForCell(int cell, char *str, int maxLen)
{
	char format[16];
	sprintf(format, "%%%dd", countDigits(AdapterGetDataLength()));
	snprintf(str, maxLen, format, cell);
}
