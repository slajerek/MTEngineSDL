#ifndef _CRenderShaderMetal_h_
#define _CRenderShaderMetal_h_

#include "CRenderShader.h"
#include <string>

class CRenderBackendMetal;

// Metal counterpart of CRenderShaderOpenGL4.
//
// The shape is deliberately the same -- CompileShaders() / UseShaderProgram() /
// SetShaderVars() / ResetState() -- because CRenderShader is already
// backend-neutral and imgui_impl_metal already dispatches ImDrawList user
// callbacks. What does NOT carry over is the source language, and one structural
// difference follows from it: GLSL is two independent stage sources, MSL is ONE
// library containing both entry points, so a subclass overrides
// GetMetalShaderSource() once and names its two functions.
//
// TWO LOADING PATHS, by design:
//
//   * source compilation (newLibraryWithSource:) -- the DEVELOPMENT path. Edit a
//     shader, relaunch, see it. It stays forever; it is the shortest iteration
//     loop there is.
//
//   * an EMBEDDED .metallib (newLibraryWithData:) -- the SHIPPING path. It moves
//     MSL compilation off startup, turns shader syntax errors into build
//     failures instead of first-run surprises, and -- decisively for c64d --
//     keeps the app a single executable, since a loose .metallib beside the
//     binary is exactly what that app exists to avoid.
//
// LoadLibrary() picks between them: embedded blob if the subclass registered
// one, source otherwise. Subclasses need to know about neither.
class CRenderShaderMetal : public CRenderShader
{
public:
	CRenderShaderMetal(CRenderBackendMetal *renderBackend, const char *shaderName);
	virtual ~CRenderShaderMetal();

	const char *name;

	// --- to be overridden by the shader ---------------------------------

	// MSL for BOTH stages in one string.
	virtual const char *GetMetalShaderSource();
	virtual const char *GetVertexFunctionName();
	virtual const char *GetFragmentFunctionName();

	// Task 9b seam. Return the embedded .metallib blob for this shader, or NULL
	// (the default) to compile from source. Implemented by subclasses whose MSL
	// the build pipeline pre-compiles.
	virtual const void *GetEmbeddedLibraryData(unsigned long *outLength);

	// --- lifecycle -------------------------------------------------------

	virtual void CompileShaders() override;
	virtual void UseShaderProgram() override;
	virtual void ResetState() override;

	// Called from inside the ImGui draw callback with the frame's live
	// id<MTLRenderCommandEncoder>, already null-checked. Subclasses bind their
	// own textures and uniform bytes here; the base class has none.
	virtual void SetShaderVars(void *encoder);

	// False when compilation failed. Callers draw their unshaded fallback rather
	// than issuing draws that would silently render nothing -- a Metal shader
	// that fails to build is otherwise completely invisible.
	bool IsUsable() const { return isCompiled; }

protected:
	// Builds the MTLLibrary by whichever path applies. Separated from
	// CompileShaders() so the embedded path is an ADDITION in Task 9b rather
	// than a rewrite of the source path.
	virtual bool LoadLibrary();

	CRenderBackendMetal *renderBackend;

	void *libraryPtr;    // id<MTLLibrary>,             retained
	void *pipelinePtr;   // id<MTLRenderPipelineState>, retained

	// The compiler's diagnostics from the most recent CompileShaders(), for a
	// subclass that must RETURN them rather than log them. Cleared at the top
	// of every compile.
	//
	// It exists because LOGError is a no-op under GLOBAL_DEBUG_OFF, which is
	// set on Linux; the Metal path never runs there, but the seam this serves
	// is backend-neutral and its contract has to be the same everywhere.
	// See CRenderShaderCustomFragment.h.
	std::string lastCompileLog;

	// Latch. Without it a failed compile is retried on every single frame, which
	// turns one diagnostic into a scrolling wall of them and costs real time.
	bool compileFailed;
};

// Fills its quad with a constant colour, ignoring the texture entirely.
//
// This exists to prove the shader PLUMBING -- compile, pipeline creation, ImGui
// draw callback dispatch, encoder access -- independently of any real shader's
// maths, so that a failure in the masked-tile or CRT port is a shader bug and
// not an infrastructure bug. It has a deliberate OpenGL twin
// (CRenderShaderFlatColorOpenGL4) so the test that exercises it runs and asserts
// on BOTH backends rather than being a Metal-only test that silently counts as
// passed on the default backend.
class CRenderShaderFlatColorMetal : public CRenderShaderMetal
{
public:
	CRenderShaderFlatColorMetal(CRenderBackendMetal *renderBackend, float r, float g, float b, float a);

	virtual const char *GetMetalShaderSource() override;
	virtual const void *GetEmbeddedLibraryData(unsigned long *outLength) override;
	virtual void SetShaderVars(void *encoder) override;

private:
	float color[4];
};

#endif
