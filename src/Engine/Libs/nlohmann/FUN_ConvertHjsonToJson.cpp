#include "SYS_Main.h"
#include "FUN_ConvertHjsonToJson.h"

nlohmann::json FUN_ConvertHJsonToJson(const Hjson::Value& fromHjson)
{
	nlohmann::json result;
	
	if (fromHjson.type() == Hjson::Type::Map)
	{
		for (auto it = fromHjson.begin(); it != fromHjson.end(); it++)
		{
			const std::string& key = it->first;
			const Hjson::Value& value = it->second;

			result[key] = FUN_ConvertHJsonToJson(value);
		}
	}
	else if (fromHjson.type() == Hjson::Type::Vector)
	{
		for (int i = 0; i < fromHjson.size(); ++i)
		{
			result.push_back(FUN_ConvertHJsonToJson(fromHjson[i]));
		}
	}
	else if (fromHjson.type() == Hjson::Type::Bool)
	{
		return static_cast<const bool>(fromHjson);
	}
	else if (fromHjson.type() == Hjson::Type::Int64)
	{
		return static_cast<const i64>(fromHjson);
	}
	else if (fromHjson.type() == Hjson::Type::Double)
	{
		return static_cast<const double>(fromHjson);
	}
	else if (fromHjson.type() == Hjson::Type::String)
	{
		return static_cast<std::string>(fromHjson);
	}
	else if (fromHjson.type() == Hjson::Type::Undefined
			 || fromHjson.type() == Hjson::Type::Null)
	{
		return nullptr;
	}
	else
	{
		LOGError("FUN_ConvertHJsonToJson: not supported type %d", fromHjson.type());
	}
	
	return result;
}

// test:
//Hjson::Value hjsonRoot;
//hjsonRoot["testInt"] = 3;
//hjsonRoot["testStr"] = "dupa";
//hjsonRoot["testDouble"] = 3.33;
//
//Hjson::Value map2;
//map2["MtestInt"] = 4;
//map2["MtestStr"] = "Mdupa";
//map2["MtestDouble"] = 4.44;
//
//hjsonRoot["testMap"] = map2;
//
//std::cout << hjsonRoot << std::endl;
//
//LOGD("0------");
//
//nlohmann::json res = FUN_ConvertHJsonToJson(hjsonRoot);
//
//std::cout << res << std::endl;
//
//LOGD("1-----");
//
