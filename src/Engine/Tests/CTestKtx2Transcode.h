#pragma once

#include "CTest.h"

// ENGINE-owned KTX2/UASTC transcoder test.
//
// Distinct from an app's own KTX2 test (one host has a CTestKtx2Decode that
// exercises CImageData::LoadKTX2 end to end on its fixture). This one
// tests the layer BELOW that -- the vendored Basis Universal transcoder itself,
// and the engine's own container preflight -- so every app that links the engine
// gets the coverage, not only the app that happens to ship a .ktx2 file.
//
// Its core assertions need NO fixture: the transcoder must be compiled with KTX2
// support and must REJECT malformed containers rather than trusting them. When
// an app does provide a fixture at the conventional path, the test additionally
// transcodes it. When it does not, that part reports as SKIPPED, never as a pass.
class CTestKtx2Transcode : public CTest
{
public:
	CTestKtx2Transcode();
	virtual ~CTestKtx2Transcode();

	virtual const char *GetName() override { return "Ktx2Transcode"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
