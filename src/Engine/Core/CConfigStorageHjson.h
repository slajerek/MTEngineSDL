#ifndef _CCONFIGSTORAGEJSON_H_
#define _CCONFIGSTORAGEJSON_H_

#include "SYS_Defs.h"
#include "hjson.h"

class CSlrString;

class CConfigStorageHjson
{
public:
	CConfigStorageHjson(const char *configFileName);
	virtual ~CConfigStorageHjson();
	
	CSlrString *configFileName;
	
	void ReadConfig();
	void SaveConfig();
	
	Hjson::Value hjsonRoot;

	void SetBool(const char *name, bool *value);
	void SetBoolSkipConfigSave(const char *name, bool *value);
	void GetBool(const char *name, bool *value, bool defaultValue);
	bool GetBool(const char *name, bool defaultValue);

	void SetInt(const char *name, int *value);
	void GetInt(const char *name, int *value, int defaultValue);

	void SetFloat(const char *name, float *value);
	void GetFloat(const char *name, float *value, float defaultValue);

	void SetDouble(const char *name, double *value);
	void GetDouble(const char *name, double *value, double defaultValue);

	void SetString(const char *name, const char *value);
	void SetString(const char *name, const char **value);
	void GetString(const char *name, const char **value, const char *defaultValue);
	void GetString(const char *name, char *value, int stringMaxLength, const char *defaultValue);

	void SetSlrString(const char *name, CSlrString **value);
	void GetSlrString(const char *name, CSlrString **value, CSlrString *defaultValue);
	
	// damn fucking ms windows !!!! when below member name is called Exists then vc compiler is stupid as int sts exists
	bool E_x_i_s_t_s(const char *name);
};

#endif
