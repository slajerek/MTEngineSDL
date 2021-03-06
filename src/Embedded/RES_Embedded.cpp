#include "RES_Embedded.h"
#include "RES_ResourceManager.h"
#include "RES_DeployFile.h"

// default embedded data
#include "console_plain_gfx.h"
#include "console_inverted_plain_gfx.h"
#include "default_font_fnt.h"
#include "default_font_gfx.h"

void RES_InitEmbeddedData()
{
	LOGM("RES_InitEmbeddedData");
	
	RES_AddEmbeddedDataToDeploy("/Engine/console-plain", DEPLOY_FILE_TYPE_GFX, console_plain_gfx, console_plain_gfx_length);
	RES_AddEmbeddedDataToDeploy("/Engine/console-inverted-plain", DEPLOY_FILE_TYPE_GFX, console_inverted_plain_gfx, console_inverted_plain_gfx_length);
	RES_AddEmbeddedDataToDeploy("/Engine/default-font", DEPLOY_FILE_TYPE_FONT, default_font_fnt, default_font_fnt_length);
	RES_AddEmbeddedDataToDeploy("/Engine/default-font", DEPLOY_FILE_TYPE_GFX, default_font_gfx, default_font_gfx_length);
}

