#pragma once

#include "CTest.h"

class CTestLlamaAvailability : public CTest {
public:
	const char *GetName() override { return "LlamaAvailability"; }
	void Run(ITestCallback *callback) override;
	void Cancel() override { isRunning = false; }
};
