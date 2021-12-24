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
	*value = 0;
}

void CDataAdapter::AdapterWriteByte(int pointer, u8 value)
{
	
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
