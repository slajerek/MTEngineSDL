#include "LLamaBackend_Stub.h"

bool LLamaBackend_Stub::TryLoadModel(const std::string &, const MT_LlamaLoadParams &, std::string *errorOut)
{
	if (errorOut)
		*errorOut = "llama.cpp support is disabled (MT_ENABLE_LLAMA_CPP=0)";
	return false;
}
