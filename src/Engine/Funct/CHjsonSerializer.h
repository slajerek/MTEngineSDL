#pragma once

#include "hjson.h"

class CSlrString;
class CConfigStorageHjson;

class CHjsonSerializer
{
public:
	CHjsonSerializer();
	virtual ~CHjsonSerializer();
	
	virtual void InitSerializer();
	virtual bool LoadHjson(const char *path);
	virtual void SaveHjson(const char *path);
	virtual void SaveHjson();
	virtual void LoadFromHjson(Hjson::Value hjsonRoot);
	virtual void StoreToHjson(Hjson::Value hjsonRoot);

	char *serializerPathToHjson;
	char serializerErrorText[256];
};
