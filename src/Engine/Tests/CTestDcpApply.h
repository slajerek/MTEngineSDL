#pragma once
#include "CTest.h"

// RD-D #8.2: the seam arithmetic against writer-built profiles --
// mired-linear interpolation pinned at both calibration temperatures AND
// two intermediates (one point cannot distinguish mired from Kelvin), the
// signature gate both ways, the NeutralToXY fixed point (the resolved
// reference neutral must reproduce the as-shot neutral), the HueSatMap
// sampler against hand-computed triples (hue wrap, sat scale, the V>1
// deviation, the encoding tag), RGBTone vs per-channel on a saturated
// triple, MapWhiteMatrix's identity case, and the tone-curve seam's
// nullptr rule (F13). Registered in the the photo app suite (A0 rule).
class CTestDcpApply : public CTest
{
public:
	CTestDcpApply() = default;
	~CTestDcpApply() = default;
	virtual const char *GetName() override { return "DcpApply"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
