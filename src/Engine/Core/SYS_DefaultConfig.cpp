#include "SYS_DefaultConfig.h"

CConfigStorageHjson *gApplicationDefaultConfig = NULL;

void SYS_InitApplicationDefaultConfig()
{
	gApplicationDefaultConfig = new CConfigStorageHjson(APPLICATION_DEFAULT_CONFIG_HJSON_FILE_PATH, true);
}
