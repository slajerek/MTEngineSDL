#ifndef _CDataAdapter_H_
#define _CDataAdapter_H_

#include "SYS_Defs.h"

//
// CDataAdapter: abstract/interface class to handle data space with callback
//

class CDataAdapter;

//class CDataAdapterCallback
//{
//public:
//	virtual void ReadByteCallback(CDataAdapter *adapter, int pointer, byte value, bool isAvailable);
//	virtual void WriteByteCallback(CDataAdapter *adapter, int pointer, byte value, bool isAvailable);
//};

class CDataAdapter
{
public:
	CDataAdapter(const char *adapterID);
	virtual ~CDataAdapter();

	// for ImGui id and serialization of states (tables, watches, ...)
	const char *adapterID;
	
	virtual int AdapterGetDataLength();
	
	// renderers should add this offset to the presented address
	virtual int GetDataOffset();
	
	virtual void AdapterReadByte(int pointer, uint8 *value);
	virtual void AdapterWriteByte(int pointer, uint8 value);
	virtual void AdapterReadByte(int pointer, uint8 *value, bool *isAvailable);
	virtual void AdapterWriteByte(int pointer, uint8 value, bool *isAvailable);
	
	virtual void AdapterReadBlockDirect(uint8 *buffer, int pointerStart, int pointerEnd);

	// pointer % AdapterGetDataLength
	virtual uint8 AdapterReadByteModulus(int pointer);
	virtual void AdapterReadByteModulus(int pointer, uint8 *value);
	virtual void AdapterWriteByteModulus(int pointer, uint8 value);

	//CDataAdapterCallback *callback;
};

#endif

