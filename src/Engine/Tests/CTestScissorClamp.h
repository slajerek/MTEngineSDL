#pragma once

#include "CTest.h"

// A Metal render pass ABORTS THE PROCESS when a scissor rect leaves the render
// target, and the abort lands inside vendored ImGui code carrying no numbers.
// It reproduces only while dragging a window corner, because the trigger is
// VID_Render being re-entered from inside AppKit's windowDidResize: while the
// window is mid-change.
//
// That is untestable as a crash and perfectly testable as arithmetic, which is
// why VID_ScissorForClipRect exists: the same projection, the same clamp and
// the same truncating conversion the backend performs, in a pure function this
// can feed the exact conditions a resize creates.
//
// It also separates the two failures that share one call site -- a rect that
// EXCEEDS the attachment and a rect that TRUNCATES to zero -- because a fix for
// one does nothing for the other, and telling them apart from a stack trace is
// impossible.
class CTestScissorClamp : public CTest
{
public:
	virtual const char *GetName() override { return "ScissorClamp"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
