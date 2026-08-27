#pragma once

#include "CTest.h"

// ICC extraction through CExifReader's own container walkers: JPEG APP2
// (single- and multi-segment, out of order, with gaps) and PNG iCCP.
//
// The point of testing here rather than only at CImageData level is that these
// walkers are shared with EXIF, and the changes that let them collect ICC also
// had to stop them abandoning the walk when EXIF is absent or malformed.
class CTestIccExtractJpeg : public CTest
{
public:
	CTestIccExtractJpeg();
	virtual ~CTestIccExtractJpeg();

	virtual const char *GetName() override { return "IccExtractJpeg"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};

class CTestIccExtractPng : public CTest
{
public:
	CTestIccExtractPng();
	virtual ~CTestIccExtractPng();

	virtual const char *GetName() override { return "IccExtractPng"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};

// ICC on CImageData itself: the members, their ownership across reloads and
// copies, and per-format extraction through the real loaders.
class CTestIccExtractFormats : public CTest
{
public:
	CTestIccExtractFormats();
	virtual ~CTestIccExtractFormats();

	virtual const char *GetName() override { return "IccExtractFormats"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
