#ifndef _CCONFIG_READER_H_
#define _CCONFIG_READER_H_

// Note, this is old style config storage that is not used anymore. Please use CConfigStorageHjson instead

#include "SYS_Defs.h"
#include "CSlrFile.h"
#include <vector>

#define MAX_NAME_LEN 255
#define MAX_VALUE_LEN 511

class CConfigValue
{
public:
	char name[MAX_NAME_LEN];
	char val[MAX_VALUE_LEN];

	CConfigValue();
};

class CConfigStorage
{
public:
	char fileName[MAX_VALUE_LEN];
	
	CConfigStorage();
	CConfigStorage(bool fromResources, const char *fileName);
	CConfigStorage(CSlrFile *file);
	~CConfigStorage();

	void Save();

	void ReadFromFile(CSlrFile *file);

	std::vector<CConfigValue *> *values;

	CConfigValue *GetConfigValue(const char *name);

	bool GetBoolValue(const char *name, bool def);
	float GetFloatValue(const char *name, float def);
	int GetIntValue(const char *name, int def);
	void GetStringValue(const char *name, char *value, int numChars, const char *def);

	void SetBoolValue(const char *name, bool def);
	void SetFloatValue(const char *name, float def);
	void SetIntValue(const char *name, int def);
	void SetStringValue(const char *name, char *value, int numChars);

	void DumpToFile(const char *filePath);
	void DumpToLog();
};


#endif

//_CCONFIG_READER_H_

