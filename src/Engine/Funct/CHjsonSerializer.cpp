#include "DBG_Log.h"
#include "SYS_FileSystem.h"
#include "CHjsonSerializer.h"
#include "CConfigStorageHjson.h"
#include "CSlrString.h"
#include "CByteBuffer.h"
#include "SYS_Funct.h"
#include <sstream>

CHjsonSerializer::CHjsonSerializer()
{
	InitSerializer();
}

CHjsonSerializer::~CHjsonSerializer()
{
	
}

void CHjsonSerializer::InitSerializer()
{
	LOGD("CHjsonSerializer::InitSerializer");
	serializerErrorText[0] = 0;
	
	EMPTY_PATH_ALLOC(serializerPathToHjson);
}

bool CHjsonSerializer::LoadHjson(const char *path)
{
	LOGD("CHjsonSerializer::LoadHjson: %s", path);
	strcpy(serializerPathToHjson, path);

	CByteBuffer *byteBuffer = new CByteBuffer(serializerPathToHjson);
	if (!byteBuffer->IsEmpty())
	{
		char *jsonText = new char[byteBuffer->length+2];
		memcpy(jsonText, byteBuffer->data, byteBuffer->length);
		jsonText[byteBuffer->length] = 0;
		delete byteBuffer;
		
		Hjson::Value hjsonRoot;
		std::stringstream ss;
		ss.str(jsonText);

		try
		{
			ss >> hjsonRoot;
			this->LoadFromHjson(hjsonRoot);
		}
		catch(const std::exception& e)
		{
			LOGError("CHjsonSerializer::LoadHjson error: %s", e.what());
			sprintf(serializerErrorText, "File is not in correct format, %s", e.what());
			hjsonRoot.clear();
			delete [] jsonText;
			return false;
		}
		delete [] jsonText;
	}
	else
	{
		LOGError("CHjsonSerializer::LoadHjson: empty CByteBuffer, failed to init");
		sprintf(serializerErrorText, "File is not in correct format, empty CByteBuffer, failed to init");
		return false;
	}

	serializerErrorText[0] = 0;
	
	return true;
}

void CHjsonSerializer::SaveHjson(const char *path)
{
	LOGD("CHjsonSerializer::SaveHjson");
	if (path != NULL)
	{
		strcpy(serializerPathToHjson, path);
	}

	serializerErrorText[0] = 0;

	FILE *fp = fopen(serializerPathToHjson, "wb");
	
	if (!fp)
	{
		LOGError("CFeatureConfig::SaveHjson: can't write to %s", serializerPathToHjson);
		sprintf(serializerErrorText, "Can't write to file %s", serializerPathToHjson);
		return;
	}
	
	Hjson::Value hjsonRoot;
	StoreToHjson(hjsonRoot);
			
	// store config
	std::stringstream ss;
	ss << Hjson::Marshal(hjsonRoot);
	
	std::string s = ss.str();
	const char *cstrHjson = s.c_str();

	fprintf(fp, "%s", cstrHjson);
	
	fclose(fp);
}

void CHjsonSerializer::SaveHjson()
{
	SaveHjson(NULL);
}

void CHjsonSerializer::LoadFromHjson(Hjson::Value hjsonRoot)
{
	// Example:
//	settingExportToPrg 	= static_cast<const bool>(hjsonRoot["ExportToPrg"]);
//	strcpy(settingPrgOutputPath, static_cast<const char *>(hjsonRoot["PrgOutputPath"]));
//
//	const char *hexValueStr;
//	hexValueStr = static_cast<const char *>(hjsonRoot["PackedTexturesAddr"]);
//	PACKED_TEXTURES_ADDR = strtoul( hexValueStr, NULL, 16 );

}

void CHjsonSerializer::StoreToHjson(Hjson::Value hjsonRoot)
{
	// Example:
//	hjsonRoot["ExportToPrg"] = settingExportToPrg;
//	hjsonRoot["PrgOutputPath"] = settingPrgOutputPath;

//	char hexStr[16];
//	sprintf(hexStr, "%04x", PACKED_TEXTURES_ADDR);
//	hjsonRoot["PackedTexturesAddr"] = hexStr;
}

