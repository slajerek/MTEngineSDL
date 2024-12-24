#ifndef _FUN_ConvertHjsonToJson_h_
#define _FUN_ConvertHjsonToJson_h_

#include "hjson.h"
#include "json.hpp"

nlohmann::json FUN_ConvertHJsonToJson(const Hjson::Value& fromHjson);

#endif
