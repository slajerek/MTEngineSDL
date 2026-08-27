#pragma once
#include "CTest.h"

// RD-D #8.1: the DCP parser against writer-built fixtures -- round-trips,
// every refusal in #3.3's valid-combination table, the DNG 1.6+ detection,
// the malformed set (truncation sweep, wrong magic incl. TIFF's 42, absurd
// dims, offsets past EOF), load-time normalisations (F1/F2), the
// DNG-embedded path + file tags (0xC6F3 route, F12), and fingerprint
// stability/difference pins (F10). Registered in the PhotoCruise suite
// (the A0 rule: engine tests register there or never run).
class CTestDcpParse : public CTest
{
public:
	CTestDcpParse() = default;
	~CTestDcpParse() = default;
	virtual const char *GetName() override { return "DcpParse"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
