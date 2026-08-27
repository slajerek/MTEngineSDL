#pragma once

#include "CTest.h"

// Malformed profiles must be refused without crashing. A CMM handed a
// truncated profile crashes rather than returning bad colour, and profiles
// arrive from untrusted files.
class CTestCmsMalformedProfile : public CTest
{
public:
	virtual const char *GetName() override { return "CmsMalformedProfile"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Actual colour conversion. Note the load-bearing row is the green-bearing
// one: sRGB and Adobe RGB share near-identical red and blue primaries and the
// same TRC, so neutral and pure-red probes pass unchanged even for a backend
// that copies input to output -- which is exactly the fail-soft path the
// Windows backend builds.
class CTestCmsKnownValues : public CTest
{
public:
	virtual const char *GetName() override { return "CmsKnownValues"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

class CTestCmsIdentity : public CTest
{
public:
	virtual const char *GetName() override { return "CmsIdentity"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

class CTestCmsBuiltinProfiles : public CTest
{
public:
	virtual const char *GetName() override { return "CmsBuiltinProfiles"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// Sharing, eviction, lease survival and backend retirement.
class CTestCmsTransformCache : public CTest
{
public:
	virtual const char *GetName() override { return "CmsTransformCache"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// The WCS variant is Windows-only, so this skips elsewhere. Without it nothing
// ever exercises the second Windows engine.
class CTestCmsWindowsEngines : public CTest
{
public:
	virtual const char *GetName() override { return "CmsWindowsEngines"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// CMS_VARIANT_LCMS2 is what CColorManager actually defaults to on Windows
// (see CColorManager.cpp) -- without this, nothing ever exercises that
// specific variant selection on Windows; CmsKnownValues/CmsBuiltinProfiles
// etc. only ever construct the raw platform default. Also runs on Linux,
// where lcms2 already is the (only) backend, as an ordinary sanity check.
class CTestCmsLcms2Engine : public CTest
{
public:
	virtual const char *GetName() override { return "CmsLcms2Engine"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
