#ifndef _MT_CaptureHelpers_h_
#define _MT_CaptureHelpers_h_

// Backend- and app-independent pixel-capture helpers for imgui_test_engine
// tests.
//
// The engine already registers MT_ScreenCaptureFunc for every host, so the
// plumbing was always shared -- but PhotoCruise grew its analysis helpers inline
// in its own test file while c64d and LightHeroes had no capture sites at all.
// These live here so the other two apps get the same measurements without a
// third and fourth copy, and so the S-4 shader ports can assert on PIXELS rather
// than settling for "it compiled".

#if MT_ENABLE_IMGUI_TEST_ENGINE

#include <vector>

struct ImGuiTestContext;

// Capture a named ImGui window into an owned RGBA8 buffer.
//
// Returns false when the capture came back empty -- the window was not found,
// the frame was aborted, or the backend cannot read its framebuffer back. The
// distinction matters: callers must report a capture failure as a capture
// failure rather than assert against an empty buffer and blame the shader.
bool MT_CaptureWindowRGBA(ImGuiTestContext *ctx, const char *windowName,
						  std::vector<unsigned int> *outPixels, int *outW, int *outH);

// Fraction of pixels differing from `bgColor` by more than `tolerance` per
// channel, and (optionally) the number of DISTINCT colours present.
//
// Both measures are load-bearing and neither replaces the other. A window that
// renders nothing has a fraction near zero; a window that renders the WRONG
// uniform colour has a fraction near one but only a single distinct colour.
// PhotoCruise's all-grey-filmstrip bug was exactly the second kind.
float MT_NonBackgroundFraction(const std::vector<unsigned int> &pixels, unsigned int bgColor,
							   int tolerance, int *outDistinctColors);

// Fraction of pixels within `tolerance` per channel of `color`. The direct form
// of "did this shader paint what it claimed".
float MT_ColorFraction(const std::vector<unsigned int> &pixels, unsigned int color, int tolerance);

// Compare two captures within a per-channel tolerance, reporting the first
// differing index. This is how a Metal port is judged against its OpenGL
// original: one process only ever sees one backend, so the two captures are
// written to disk by separate runs and compared as artifacts.
bool MT_CapturesMatch(const std::vector<unsigned int> &a, const std::vector<unsigned int> &b,
					  int tolerancePerChannel, int *outFirstDifferingIndex);

// Write / read a capture as a flat artifact file, for the cross-backend
// comparison above. Format is a tiny header (magic, w, h) followed by raw RGBA8;
// no PNG dependency, and it is a test artifact rather than anything a user sees.
bool MT_WriteCaptureArtifact(const char *path, const std::vector<unsigned int> &pixels, int w, int h);
bool MT_ReadCaptureArtifact(const char *path, std::vector<unsigned int> *outPixels, int *outW, int *outH);

#endif

#endif
