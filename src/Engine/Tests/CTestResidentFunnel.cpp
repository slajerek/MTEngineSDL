#include "CTestResidentFunnel.h"
#include "CSlrImage.h"
#include "CImageData.h"
#include "DBG_Log.h"
#include <cstdio>

#define RF_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", (msg)); \
			LOGD("CTestResidentFunnel: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, (msg)); \
	} while (0)

void CTestResidentFunnel::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	// ---- the whole decision table, asserted as a table -----------------------
	//
	// Written out row by row rather than as a loop so a future change to one row
	// cannot silently move another: each line is the claim itself.

	// 8-bit in, 8-bit out, whatever the backend can do. Promoting an RGBA8
	// source would gain nothing and only waste memory.
	RF_ASSERT(SlrResidentFormatFor(IMG_TYPE_RGBA, false) == RENDER_TEXTURE_RGBA8,
			  "RGBA8 source -> RGBA8 (backend without float)");
	RF_ASSERT(SlrResidentFormatFor(IMG_TYPE_RGBA, true) == RENDER_TEXTURE_RGBA8,
			  "RGBA8 source -> RGBA8 (backend WITH float: no free promotion)");

	// Integer 16-bit stays collapsed even where float is available. unorm16 is
	// about PRECISION -- it has no values above 1.0 to preserve -- so promoting
	// it would double the memory and buy nothing. This row is the constraint
	// most likely to be "helpfully" broken later.
	RF_ASSERT(SlrResidentFormatFor(IMG_TYPE_RGBA_16BIT, false) == RENDER_TEXTURE_RGBA8,
			  "unorm16 source -> RGBA8 (backend without float)");
	RF_ASSERT(SlrResidentFormatFor(IMG_TYPE_RGBA_16BIT, true) == RENDER_TEXTURE_RGBA8,
			  "unorm16 source -> RGBA8 EVEN WITH float available -- precision, not range");

	// Float is the only row where the backend's answer changes the outcome.
	RF_ASSERT(SlrResidentFormatFor(IMG_TYPE_RGBA_16F, true) == RENDER_TEXTURE_RGBA16F,
			  "float source -> RGBA16F when the backend can upload it");
	RF_ASSERT(SlrResidentFormatFor(IMG_TYPE_RGBA_16F, false) == RENDER_TEXTURE_RGBA8,
			  "float source -> RGBA8 when the backend cannot");

	// Anything the funnel does not know about must still answer, and answer
	// conservatively: the guards downstream are what reject it, and a funnel
	// that returned float here would hand an unknown buffer to an 8-byte
	// upload.
	RF_ASSERT(SlrResidentFormatFor(IMG_TYPE_GRAYSCALE, true) == RENDER_TEXTURE_RGBA8,
			  "unknown/other type -> RGBA8, conservatively");
	RF_ASSERT(SlrResidentFormatFor(IMG_TYPE_CIELAB, true) == RENDER_TEXTURE_RGBA8,
			  "CIELAB -> RGBA8, conservatively");

	// ---- and the collapse actually happens -----------------------------------
	//
	// The table above is the decision; this is the ACT. A funnel that decided
	// correctly and then failed to convert would leave a float buffer typed
	// float on an 8-bit upload path -- which is the bug the funnel exists to
	// prevent, not one it may commit.
	{
		CImageData d(2, 1, IMG_TYPE_RGBA_16F);
		d.AllocImage(false, true);
		d.SetPixelResultFloat(0, 0, 2.0f, 2.0f, 2.0f, 1.0f);
		d.SetPixelResultFloat(1, 0, 0.5f, 0.5f, 0.5f, 1.0f);
		d.floatIsSurfaceEncoded = true;

		// A CSlrImage that never binds: ApplyResidentFormat is what is under
		// test, not the GPU.
		CSlrImage img(true, false);
		img.ApplyResidentFormat(&d);

		// In the headless suite there is a backend, so which answer is correct
		// depends on it -- assert the CONSISTENCY rather than a fixed answer,
		// which is what makes this test mean the same thing on both backends.
		if (img.residentFormat == RENDER_TEXTURE_RGBA8)
		{
			RF_ASSERT(d.getImageType() == IMG_TYPE_RGBA,
					  "resident RGBA8 => the float data was actually collapsed");
		}
		else
		{
			RF_ASSERT(d.getImageType() == IMG_TYPE_RGBA_16F,
					  "resident RGBA16F => the float data was left alone");
		}
	}

	// A 16-bit source is collapsed by the same call, on every backend.
	{
		CImageData d(2, 1, IMG_TYPE_RGBA_16BIT);
		d.AllocImage(false, true);
		d.SetPixelResultRGBA16Bit(0, 0, 65535, 32768, 0, 65535);

		CSlrImage img(true, false);
		img.ApplyResidentFormat(&d);
		RF_ASSERT(img.residentFormat == RENDER_TEXTURE_RGBA8,
				  "unorm16 is resident as RGBA8");
		RF_ASSERT(d.getImageType() == IMG_TYPE_RGBA,
				  "and the data really was collapsed to 8-bit");
	}

	// An 8-bit source passes through untouched -- the SDR path must not change
	// shape just because a float path now exists beside it.
	{
		CImageData d(2, 1, IMG_TYPE_RGBA);
		d.AllocImage(false, true);
		d.SetPixelResultRGBA(0, 0, 10, 20, 30, 255);

		CSlrImage img(true, false);
		img.ApplyResidentFormat(&d);
		RF_ASSERT(img.residentFormat == RENDER_TEXTURE_RGBA8, "RGBA8 stays RGBA8");
		RF_ASSERT(d.getImageType() == IMG_TYPE_RGBA, "and its type is untouched");
		u8 r, g, b, a;
		d.GetPixelResultRGBA(0, 0, &r, &g, &b, &a);
		RF_ASSERT(r == 10 && g == 20 && b == 30 && a == 255, "and its pixels are untouched");
	}

	TestCompleted(true, "resident-format funnel: one decision, two answers");
}
