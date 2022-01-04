#ifndef _CProgramLoadableConfig_h_
#define _CProgramLoadableConfig_h_

// this class is to be derived by a feature that allows loading and saving data (or a configuration)
// for example, a plugin that has some data or plugin settings that user can store and restore from file
// feature global settings (featureSettings) are used to restore default data on init.
// feature / derived class should call InitConfig and then implement InitFromHjson and StoreToHjson
#include "hjson.h"

class CSlrString;
class CConfigStorageHjson;

class CFeatureConfig
{
public:
	CFeatureConfig();
	virtual ~CFeatureConfig();
	
	virtual void SetFeatureRootFolderPath(CSlrString *path);
	virtual void InitConfig(const char *featureName);
	virtual void LoadConfig(CSlrString *path);
	virtual void SaveConfig(CSlrString *path);
	virtual void InitFromHjson(Hjson::Value hjsonRoot);
	virtual void StoreToHjson(Hjson::Value hjsonRoot);

	CConfigStorageHjson *featureSettings;
	CSlrString *featureDefaultFolder;
	CSlrString *featureDefaultConfigPath;
	char *featureConfigPath;
	char *featureRootFolderPath;
	char featureErrorText[256];
};

#endif

