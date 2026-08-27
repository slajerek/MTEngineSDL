#pragma once

#include "CTest.h"

// JPEGWriter marker emission, chroma subsampling and error-path hygiene.
//
// Every negative case here reaches libjpeg's error_exit. Until 2026-07-20 the
// writer's error handler asserted is_decompressor (inverted), so in a debug
// build these would have aborted the suite rather than failing -- an aborted
// run being no result at all rather than a failing one.
class CTestJpegMarkers : public CTest
{
public:
	CTestJpegMarkers();
	virtual ~CTestJpegMarkers();

	virtual const char *GetName() override { return "JpegMarkers"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
