#ifndef _SYS_DefaultConfig_h_
#define _SYS_DefaultConfig_h_

#include "CConfigStorageHjson.h"

#define APPLICATION_DEFAULT_CONFIG_HJSON_FILE_PATH "settings.hjson"

extern CConfigStorageHjson *gApplicationDefaultConfig;

void SYS_InitApplicationDefaultConfig();

#endif
