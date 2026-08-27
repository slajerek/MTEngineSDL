#pragma once
#include "CTest.h"

// CM-C1: the reach CExifReader gained beyond IFD0 -- SubIFDs (0x014A, both LONG
// and IFD encodings), the IFD1 chain, DNG PreviewColorSpace candidates, vendor
// MakerNote decoding, and the colour hint those produce.
//
// Every fixture is a hand-built byte array. CExifBuilder cannot express any of
// this -- it says so in its own header ("no thumbnail IFD, no IFD1 and no
// MakerNote") -- so these tests carry their own minimal TIFF writer.

// Walking: SubIFD arrays, both pointer encodings, and the IFD1 chain including
// the cycle and bounds shapes that must terminate rather than hang or over-read.
class CTestExifWalk : public CTest
{
public:
    virtual const char *GetName() override { return "ExifWalk"; }
    virtual void Run(ITestCallback *callback) override;
    virtual void Cancel() override { isRunning = false; }
};

// The guarantee the folder scan depends on: with the capture flag off, nothing
// this phase added is reachable and the result is what it was before CM-C1.
class CTestExifLeanPath : public CTest
{
public:
    virtual const char *GetName() override { return "ExifLeanPath"; }
    virtual void Run(ITestCallback *callback) override;
    virtual void Cancel() override { isRunning = false; }
};

// CImageData's preview colour hint must live and die exactly as iccProfile
// does. The bug this guards -- a file inheriting the previous file's colour
// space -- is a confident wrong colour, not a crash, so nothing else would
// surface it.
class CTestExifPreviewHintLifecycle : public CTest
{
public:
    virtual const char *GetName() override { return "ExifPreviewHintLifecycle"; }
    virtual void Run(ITestCallback *callback) override;
    virtual void Cancel() override { isRunning = false; }
};

// Vendor MakerNotes: the three offset shapes, per-vendor colour mappings and
// their sentinels, unknown vendors, and malformed blobs.
class CTestExifMakerNote : public CTest
{
public:
    virtual const char *GetName() override { return "ExifMakerNote"; }
    virtual void Run(ITestCallback *callback) override;
    virtual void Cancel() override { isRunning = false; }
};
