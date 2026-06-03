#include "CTestLlamaAvailability.h"

#include "Sci/Llama/CLlamaService.h"

#include <string>

void CTestLlamaAvailability::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	currentStep = 0;

	CLlamaService llama;

	bool compiledIn = CLlamaService::IsCompiledIn();
	bool available = llama.IsAvailable();

	if (!compiledIn)
	{
		if (available)
		{
			TestCompleted(false, "Expected llama to be unavailable when MT_ENABLE_LLAMA_CPP=0");
			return;
		}
		TestCompleted(true, "llama.cpp disabled and unavailable as expected");
		return;
	}

	if (!available)
	{
		TestCompleted(false, "Expected llama to be available when compiled in");
		return;
	}

	std::string backend = llama.GetBackendName();
	if (backend.empty())
	{
		TestCompleted(false, "Backend name is empty");
		return;
	}

	TestCompleted(true, backend.c_str());
}
