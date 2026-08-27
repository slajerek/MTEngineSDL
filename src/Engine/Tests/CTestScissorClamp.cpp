#include "CTestScissorClamp.h"
#include "CRenderBackend.h"
#include "DBG_Log.h"
#include <cstdio>

#define SC_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", (msg)); \
			LOGD("CTestScissorClamp: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, (msg)); \
	} while (0)

void CTestScissorClamp::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	const ImVec2 noOffset(0.0f, 0.0f);
	const ImVec2 scale2x(2.0f, 2.0f);

	// ---- 1. the ordinary case: nothing to fix ------------------------------
	//
	// A Retina window where everything agrees. If this ever fails, the helper
	// has stopped describing the backend and every other case here is
	// meaningless.
	{
		const SVidScissor sc = VID_ScissorForClipRect(
			ImVec4(10.0f, 20.0f, 110.0f, 120.0f), noOffset, scale2x,
			/*fb*/ 1600, 1200, /*att*/ 1600, 1200);
		SC_ASSERT(!sc.skipped && !sc.exceeds && !sc.degenerate,
				  "agreeing sizes: an ordinary clip rect is fine");
		SC_ASSERT(sc.x == 20 && sc.y == 40 && sc.w == 200 && sc.h == 200,
				  "and is projected by FramebufferScale, not passed through");
	}

	// ---- 2. THE CRASH: framebuffer larger than the attachment --------------
	//
	// The live-resize condition. ImGui's framebuffer measures 1600x1200 (window
	// points x scale) while the drawable is already 1596x1196, so the backend's
	// clamp -- which uses the FRAMEBUFFER -- happily allows a rect the render
	// target cannot hold. This is the rect Metal aborts on.
	{
		const SVidScissor sc = VID_ScissorForClipRect(
			ImVec4(0.0f, 0.0f, 800.0f, 600.0f), noOffset, scale2x,
			/*fb*/ 1600, 1200, /*att*/ 1596, 1196);
		SC_ASSERT(!sc.skipped, "resize mismatch: the command is not skipped");
		SC_ASSERT(sc.exceeds,
				  "resize mismatch: the scissor EXCEEDS the attachment -- the crash");
		SC_ASSERT(sc.x + sc.w == 1600 && sc.y + sc.h == 1200,
				  "and it exceeds by exactly the size disagreement");
	}

	// ---- 3. and the clamp removes it ---------------------------------------
	//
	// Clamping ImGui's framebuffer down to the attachment must make the SAME
	// clip rect safe, because the backend's internal clamp is computed from it.
	{
		ImDrawData dd;
		dd.DisplaySize = ImVec2(800.0f, 600.0f);      // points
		dd.FramebufferScale = ImVec2(2.0f, 2.0f);      // -> 1600x1200 framebuffer
		dd.DisplayPos = ImVec2(0.0f, 0.0f);
		VID_ClampDrawDataToAttachment(&dd, 1596, 1196);

		const int fbW = (int)(dd.DisplaySize.x * dd.FramebufferScale.x);
		const int fbH = (int)(dd.DisplaySize.y * dd.FramebufferScale.y);
		SC_ASSERT(fbW <= 1596 && fbH <= 1196,
				  "the clamp brings ImGui's framebuffer within the attachment");

		const SVidScissor sc = VID_ScissorForClipRect(
			ImVec4(0.0f, 0.0f, 800.0f, 600.0f), dd.DisplayPos, dd.FramebufferScale,
			fbW, fbH, 1596, 1196);
		SC_ASSERT(!sc.exceeds,
				  "and the same clip rect is then SAFE -- this is the fix, asserted");
	}

	// ---- 4. the clamp must not touch an agreeing frame ---------------------
	//
	// Otherwise it would shrink the UI by a pixel on every ordinary frame,
	// which is a rendering bug traded for a crash.
	{
		ImDrawData dd;
		dd.DisplaySize = ImVec2(800.0f, 600.0f);
		dd.FramebufferScale = ImVec2(2.0f, 2.0f);
		dd.DisplayPos = ImVec2(0.0f, 0.0f);
		VID_ClampDrawDataToAttachment(&dd, 1600, 1200);
		SC_ASSERT(dd.DisplaySize.x == 800.0f && dd.DisplaySize.y == 600.0f,
				  "an agreeing frame is left exactly alone");

		// And a LARGER attachment (window shrank, drawable not yet caught up)
		// must also be left alone -- clamping up would stretch the UI.
		VID_ClampDrawDataToAttachment(&dd, 2400, 1800);
		SC_ASSERT(dd.DisplaySize.x == 800.0f && dd.DisplaySize.y == 600.0f,
				  "a LARGER attachment is left alone too -- the clamp only shrinks");
	}

	// ---- 5. THE OTHER FAILURE at the same call site ------------------------
	//
	// A sub-pixel clip rect. ImGui's own guard is `clip_max <= clip_min`, which
	// 0.5 px passes -- and then the truncating conversion makes the width ZERO.
	// Metal may reject that, and it presents identically to case 2 from a stack
	// trace while being immune to case 3's fix. Naming it is the point: it is
	// why "the clamp did not help" is a legitimate outcome rather than a
	// mystery.
	{
		const SVidScissor sc = VID_ScissorForClipRect(
			ImVec4(10.0f, 10.0f, 10.25f, 40.0f), noOffset, ImVec2(1.0f, 1.0f),
			/*fb*/ 1600, 1200, /*att*/ 1600, 1200);
		SC_ASSERT(!sc.skipped,
				  "a 0.25 px wide clip rect survives ImGui's clip_max<=clip_min guard");
		SC_ASSERT(sc.degenerate,
				  "and truncates to a ZERO-WIDTH scissor -- a different bug, same call site");
		SC_ASSERT(!sc.exceeds,
				  "and it is NOT an out-of-bounds rect, so clamping cannot fix it");
	}

	// ---- 6. rects that ImGui skips entirely --------------------------------
	{
		const SVidScissor off = VID_ScissorForClipRect(
			ImVec4(-100.0f, -100.0f, -50.0f, -50.0f), noOffset, ImVec2(1.0f, 1.0f),
			1600, 1200, 1600, 1200);
		SC_ASSERT(off.skipped, "a fully off-screen rect is skipped, not clamped to nothing");

		const SVidScissor inverted = VID_ScissorForClipRect(
			ImVec4(500.0f, 500.0f, 100.0f, 100.0f), noOffset, ImVec2(1.0f, 1.0f),
			1600, 1200, 1600, 1200);
		SC_ASSERT(inverted.skipped, "an inverted rect is skipped");
	}

	// ---- 7. multi-viewport offsets ------------------------------------------
	//
	// DisplayPos is non-zero for secondary viewport windows, and the projection
	// subtracts it. A clip rect in absolute desktop coordinates must land
	// inside that window's own attachment, not somewhere off it.
	{
		const ImVec2 clipOff(1000.0f, 500.0f);
		const SVidScissor sc = VID_ScissorForClipRect(
			ImVec4(1000.0f, 500.0f, 1400.0f, 800.0f), clipOff, ImVec2(1.0f, 1.0f),
			/*fb*/ 400, 300, /*att*/ 400, 300);
		SC_ASSERT(!sc.exceeds && !sc.skipped,
				  "a secondary viewport's rect is projected by DisplayPos and fits");
		SC_ASSERT(sc.x == 0 && sc.y == 0 && sc.w == 400 && sc.h == 300,
				  "and covers that window exactly");
	}

	// ---- 8. degenerate attachments do not crash the helper ------------------
	{
		ImDrawData dd;
		dd.DisplaySize = ImVec2(800.0f, 600.0f);
		dd.FramebufferScale = ImVec2(2.0f, 2.0f);
		VID_ClampDrawDataToAttachment(&dd, 0, 0);
		SC_ASSERT(dd.DisplaySize.x == 800.0f,
				  "a zero-sized attachment is ignored rather than zeroing the UI");
		VID_ClampDrawDataToAttachment(nullptr, 100, 100);
		SC_ASSERT(true, "a null ImDrawData is tolerated");

		ImDrawData zeroScale;
		zeroScale.DisplaySize = ImVec2(800.0f, 600.0f);
		zeroScale.FramebufferScale = ImVec2(0.0f, 0.0f);
		VID_ClampDrawDataToAttachment(&zeroScale, 100, 100);
		SC_ASSERT(zeroScale.DisplaySize.x == 800.0f,
				  "a zero FramebufferScale does not divide by zero");
	}

	TestCompleted(true, "scissor arithmetic: the resize mismatch, its fix, and the "
						"zero-width case the fix does NOT address");
}
