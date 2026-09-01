#pragma once

#include "CTest.h"

// Generic EXIF engine test: CExifReader parsing (ported from the photo app's
// CTestExif) plus CExifBuilder construction and round-trip.
//
// The builder is validated against the reader rather than against expected
// byte blobs, because a builder compared only to its own expectations passes
// every test while being self-consistently wrong.
class CTestExifEngine : public CTest
{
public:
	CTestExifEngine();
	virtual ~CTestExifEngine();

	virtual const char *GetName() override { return "ExifEngine"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
