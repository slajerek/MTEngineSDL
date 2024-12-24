extern "C" {
#include "DBG_LogForCWrapper.h"
}

#include "DBG_Log.h"

void DBG_LOGD(const char *str)
{
	LOGD("%s", str);
}

void DBG_LOGError(const char *str)
{
	LOGError("%s", str);
}


