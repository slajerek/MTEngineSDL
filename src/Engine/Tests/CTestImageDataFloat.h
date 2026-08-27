#pragma once

#include "CTest.h"

// S-5 Task 1: the float decode-time image type, IMG_TYPE_RGBA_16F.
//
// Pure data -- no GPU, no backend -- so it is a CTest rather than an ImGui
// test. Anything that needs a live texture upload belongs in the ImGui suite
// (S-5 Task 2's rule); anything that is maths stays here.
//
// Registered in PhotoCruise's suite: an unregistered CTest compiles and never
// runs, which looks exactly like a pass.
class CTestImageDataFloat : public CTest
{
public:
	virtual const char *GetName() override { return "ImageDataFloat"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
