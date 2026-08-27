#include "MT_CaptureHelpers.h"

#if MT_ENABLE_IMGUI_TEST_ENGINE

#include "imgui.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#include "imgui_capture_tool.h"
#include "DBG_Log.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>

bool MT_CaptureWindowRGBA(ImGuiTestContext *ctx, const char *windowName,
						  std::vector<unsigned int> *outPixels, int *outW, int *outH)
{
	if (outPixels) outPixels->clear();
	if (outW) *outW = 0;
	if (outH) *outH = 0;
	if (ctx == NULL || windowName == NULL)
		return false;

	ImGuiCaptureImageBuf buf;
	ctx->CaptureReset();
	ctx->CaptureAddWindow(windowName);
	ctx->CaptureArgs->InOutputImageBuf = &buf;
	// NO PADDING. imgui_capture_tool defaults to 16px around the window, which
	// is right for a screenshot a human looks at and wrong for every measurement
	// in this file: a 200x120 window captures as 232x152, so a window filled
	// edge to edge tops out at 68% of the pixels and any "> 0.9" assertion is
	// unreachable for a reason that has nothing to do with what is being tested.
	ctx->CaptureArgs->InPadding = 0.0f;
	ctx->CaptureScreenshot(0);

	if (buf.Data == NULL || buf.Width <= 0 || buf.Height <= 0)
		return false;

	size_t total = (size_t)buf.Width * (size_t)buf.Height;
	if (outPixels)
		outPixels->assign(buf.Data, buf.Data + total);
	if (outW) *outW = buf.Width;
	if (outH) *outH = buf.Height;
	return true;
}

// RGB only. The capture path forces alpha to opaque, so comparing it would test
// the capture path rather than the pixel.
static bool ColorsWithin(unsigned int a, unsigned int b, int tolerance)
{
	for (int shift = 0; shift <= 16; shift += 8)
	{
		int d = (int)((a >> shift) & 0xFF) - (int)((b >> shift) & 0xFF);
		if (d < 0) d = -d;
		if (d > tolerance)
			return false;
	}
	return true;
}

float MT_NonBackgroundFraction(const std::vector<unsigned int> &pixels, unsigned int bgColor,
							   int tolerance, int *outDistinctColors)
{
	if (outDistinctColors) *outDistinctColors = 0;
	if (pixels.empty())
		return -1.f;

	size_t nonBg = 0;
	std::vector<unsigned int> distinct;
	distinct.reserve(pixels.size());
	for (size_t i = 0; i < pixels.size(); i++)
	{
		if (!ColorsWithin(pixels[i], bgColor, tolerance))
			nonBg++;
		distinct.push_back(pixels[i] & 0x00FFFFFFu);
	}

	if (outDistinctColors)
	{
		std::sort(distinct.begin(), distinct.end());
		distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
		*outDistinctColors = (int)distinct.size();
	}

	return (float)nonBg / (float)pixels.size();
}

float MT_ColorFraction(const std::vector<unsigned int> &pixels, unsigned int color, int tolerance)
{
	if (pixels.empty())
		return -1.f;

	size_t hits = 0;
	for (size_t i = 0; i < pixels.size(); i++)
	{
		if (ColorsWithin(pixels[i], color, tolerance))
			hits++;
	}
	return (float)hits / (float)pixels.size();
}

bool MT_CapturesMatch(const std::vector<unsigned int> &a, const std::vector<unsigned int> &b,
					  int tolerancePerChannel, int *outFirstDifferingIndex)
{
	if (outFirstDifferingIndex) *outFirstDifferingIndex = -1;
	if (a.size() != b.size() || a.empty())
	{
		if (outFirstDifferingIndex) *outFirstDifferingIndex = 0;
		return false;
	}
	for (size_t i = 0; i < a.size(); i++)
	{
		if (!ColorsWithin(a[i], b[i], tolerancePerChannel))
		{
			if (outFirstDifferingIndex) *outFirstDifferingIndex = (int)i;
			return false;
		}
	}
	return true;
}

static const unsigned int kCaptureArtifactMagic = 0x4D544341u;   // 'MTCA'

bool MT_WriteCaptureArtifact(const char *path, const std::vector<unsigned int> &pixels, int w, int h)
{
	if (path == NULL || pixels.empty() || (size_t)w * (size_t)h != pixels.size())
		return false;

	FILE *fp = fopen(path, "wb");
	if (fp == NULL)
	{
		LOGError("MT_WriteCaptureArtifact: cannot open %s", path);
		return false;
	}
	unsigned int header[3] = { kCaptureArtifactMagic, (unsigned int)w, (unsigned int)h };
	bool ok = fwrite(header, sizeof(header), 1, fp) == 1
		   && fwrite(pixels.data(), sizeof(unsigned int), pixels.size(), fp) == pixels.size();
	fclose(fp);
	return ok;
}

bool MT_ReadCaptureArtifact(const char *path, std::vector<unsigned int> *outPixels, int *outW, int *outH)
{
	if (outPixels) outPixels->clear();
	if (outW) *outW = 0;
	if (outH) *outH = 0;
	if (path == NULL)
		return false;

	FILE *fp = fopen(path, "rb");
	if (fp == NULL)
		return false;

	unsigned int header[3] = { 0, 0, 0 };
	bool ok = fread(header, sizeof(header), 1, fp) == 1 && header[0] == kCaptureArtifactMagic;
	if (ok)
	{
		size_t total = (size_t)header[1] * (size_t)header[2];
		// Sanity bound. A truncated or foreign file would otherwise ask for an
		// arbitrary allocation off a corrupted size field.
		if (total == 0 || total > 64u * 1024u * 1024u)
		{
			ok = false;
		}
		else
		{
			std::vector<unsigned int> px(total);
			ok = fread(px.data(), sizeof(unsigned int), total, fp) == total;
			if (ok && outPixels)
				outPixels->swap(px);
			if (ok && outW) *outW = (int)header[1];
			if (ok && outH) *outH = (int)header[2];
		}
	}
	fclose(fp);
	return ok;
}

#endif
