#include "CRenderBackend.h"
#include "DBG_Log.h"

CRenderBackend::CRenderBackend(const char *name)
{
	this->name = name;
	this->mainWindow = NULL;
}

SDL_Window *CRenderBackend::CreateSDLWindow(const char *title, int x, int y, int w, int h, bool maximized)
{
	return NULL;
}

void CRenderBackend::CreateRenderContext()
{
}

void CRenderBackend::InitRenderPipeline()
{
}

void CRenderBackend::CreateFontsTexture()
{
}

void CRenderBackend::NewFrame(ImVec4 clearColor)
{
}

void CRenderBackend::PresentFrameBuffer(ImVec4 clearColor)
{
}

void CRenderBackend::ApplyDisplayColorGamut(VID_DisplayColorGamut gamut)
{
}

void CRenderBackend::Shutdown()
{
}

void CRenderBackend::CreateTexture(CSlrImage *image)
{
}

void CRenderBackend::UpdateTextureLinearScaling(CSlrImage *image)
{
}

void CRenderBackend::ReBindTexture(CSlrImage *image)
{
}

void CRenderBackend::DeleteTexture(CSlrImage *image)
{
}

CRenderBackend::~CRenderBackend()
{
}

// ---------------------------------------------------------------------------
// Scissor arithmetic (see the header for why it lives here)
// ---------------------------------------------------------------------------

SVidScissor VID_ScissorForClipRect(const ImVec4 &clipRect,
								   const ImVec2 &clipOff, const ImVec2 &clipScale,
								   int fbW, int fbH, int attW, int attH)
{
	SVidScissor out;

	// Project into framebuffer space, exactly as the Metal backend does.
	float minX = (clipRect.x - clipOff.x) * clipScale.x;
	float minY = (clipRect.y - clipOff.y) * clipScale.y;
	float maxX = (clipRect.z - clipOff.x) * clipScale.x;
	float maxY = (clipRect.w - clipOff.y) * clipScale.y;

	// Clamp to the FRAME BUFFER -- note: not to the attachment. That is the
	// backend's behaviour being reproduced, not an endorsement of it; when the
	// two disagree this is precisely where the fatal rect is born.
	if (minX < 0.0f) minX = 0.0f;
	if (minY < 0.0f) minY = 0.0f;
	if (maxX > (float)fbW) maxX = (float)fbW;
	if (maxY > (float)fbH) maxY = (float)fbH;

	if (maxX <= minX || maxY <= minY)
	{
		out.skipped = true;      // ImGui `continue`s -- nothing is set
		return out;
	}

	// The TRUNCATING conversion the backend performs. A span of 0.5 px passes
	// the guard above and lands here as a width of ZERO, which is a different
	// failure with the same call site -- worth naming separately, because a fix
	// for an oversized rect does nothing for it.
	out.x = (int)minX;
	out.y = (int)minY;
	out.w = (int)(maxX - minX);
	out.h = (int)(maxY - minY);
	out.degenerate = (out.w <= 0 || out.h <= 0);
	out.exceeds = (out.x + out.w > attW) || (out.y + out.h > attH)
			   || (out.x >= attW) || (out.y >= attH);
	return out;
}

void VID_ClampDrawDataToAttachment(ImDrawData *drawData, int attW, int attH)
{
	if (drawData == NULL || attW <= 0 || attH <= 0)
		return;
	if (drawData->FramebufferScale.x > 0.0f &&
		drawData->DisplaySize.x * drawData->FramebufferScale.x > (float)attW)
	{
		drawData->DisplaySize.x = (float)attW / drawData->FramebufferScale.x;
	}
	if (drawData->FramebufferScale.y > 0.0f &&
		drawData->DisplaySize.y * drawData->FramebufferScale.y > (float)attH)
	{
		drawData->DisplaySize.y = (float)attH / drawData->FramebufferScale.y;
	}
}

int VID_ReportBadScissors(const ImDrawData *drawData, int attW, int attH,
						  const char *whereFrom)
{
	if (drawData == NULL)
		return 0;

	const int fbW = (int)(drawData->DisplaySize.x * drawData->FramebufferScale.x);
	const int fbH = (int)(drawData->DisplaySize.y * drawData->FramebufferScale.y);
	int bad = 0;

	for (int n = 0; n < drawData->CmdLists.Size; n++)
	{
		const ImDrawList *cmdList = drawData->CmdLists[n];
		for (int i = 0; i < cmdList->CmdBuffer.Size; i++)
		{
			const ImDrawCmd *pcmd = &cmdList->CmdBuffer[i];
			if (pcmd->UserCallback != NULL)
				continue;
			if (pcmd->ElemCount == 0)
				continue;
			const SVidScissor sc = VID_ScissorForClipRect(
				pcmd->ClipRect, drawData->DisplayPos, drawData->FramebufferScale,
				fbW, fbH, attW, attH);
			if (sc.skipped)
				continue;
			if (sc.exceeds || sc.degenerate)
			{
				bad++;
				if (bad <= 4)   // enough to see the pattern, not a wall of text
				{
					LOGError("VID scissor %s: %s rect=(%d,%d %dx%d) "
							 "attachment=%dx%d framebuffer=%dx%d "
							 "displaySize=%.2fx%.2f scale=%.2fx%.2f "
							 "clip=(%.2f,%.2f..%.2f,%.2f) [%s]",
							 whereFrom,
							 sc.exceeds ? "EXCEEDS ATTACHMENT" : "DEGENERATE (truncates to zero)",
							 sc.x, sc.y, sc.w, sc.h, attW, attH, fbW, fbH,
							 drawData->DisplaySize.x, drawData->DisplaySize.y,
							 drawData->FramebufferScale.x, drawData->FramebufferScale.y,
							 pcmd->ClipRect.x, pcmd->ClipRect.y,
							 pcmd->ClipRect.z, pcmd->ClipRect.w,
							 sc.exceeds ? "Metal WILL abort on this" : "Metal may reject this");
				}
			}
		}
	}
	return bad;
}
