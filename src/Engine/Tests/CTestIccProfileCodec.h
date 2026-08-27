#pragma once

#include "CTest.h"

// Byte-level ICC plumbing: header validation, the profile-ID/content-digest
// trust boundary, JPEG APP2 split/join and PNG iCCP inflate.
//
// The digest rows are the load-bearing ones. GetProfileId returns the profile's
// own header ID field verbatim when set, and that field is attacker-controlled;
// GetContentDigest hashes the bytes instead. Everything that decides identity
// or keys a cache must use the latter, so these tests pin the difference.
class CTestIccProfileCodec : public CTest
{
public:
	CTestIccProfileCodec();
	virtual ~CTestIccProfileCodec();

	virtual const char *GetName() override { return "IccProfileCodec"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
