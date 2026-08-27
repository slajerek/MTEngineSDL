#pragma once
#include "CTest.h"

// S-6 Task A1: VALUE-LEVEL assertions on the Windows surface-encoding maths
// (MT_SurfaceEncoding.h).
//
// WHY THIS EXISTS, and why it is the FIRST task of the stage: S-6 adds a
// Direct3D 11 backend whose present-time resolve pass converts our
// sRGB-encoded frame into scRGB for a DXGI FP16 swapchain. None of that can be
// compiled -- let alone run -- on the machine it is written on, and the HDR
// verdict needs a display nobody here has. The conversion arithmetic is the one
// part that can be PROVEN before any of that, so it was factored into a
// header-only, platform-neutral namespace precisely so this test could exist.
//
// THE ASSERTION THAT MATTERS MOST is that the SDR-white scale is a MULTIPLY:
// EncodedSrgbToScRgb(1.0, 2.5) == 2.5, and it rises with the scale. The first
// draft of the S-6 plan had it inverted, which would have rendered the whole
// app at 32 nits beside 200-nit SDR windows and made it DARKER as the user
// turned the Windows SDR-brightness slider UP. Endpoint-only tests do not see
// that; these do.
//
// THE SECOND is that the resolve is the exact IDENTITY on an SDR swapchain.
// Decoding "at scale 1.0" instead moves mid-grey 0.5 to 0.214 -- ~73 LSB across
// the entire UI, on the DEFAULT Windows path, which is S-4's washed-out bug
// inverted.
//
// Pure maths, no GPU, no D3D: runs headlessly on macOS and Linux, where nothing
// else in S-6 runs at all.
class CTestSurfaceEncoding : public CTest
{
public:
	CTestSurfaceEncoding() = default;
	~CTestSurfaceEncoding() = default;
	virtual const char *GetName() override { return "SurfaceEncoding"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
