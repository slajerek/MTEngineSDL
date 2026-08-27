#ifndef _CRenderShaderFlatColorOpenGL4_h_
#define _CRenderShaderFlatColorOpenGL4_h_

#include "CRenderShaderOpenGL4.h"

// Fills its quad with a constant colour, ignoring the texture entirely.
//
// The OpenGL twin of CRenderShaderFlatColorMetal, and it exists for the twin's
// sake. The Metal shader family needs its plumbing -- compile, pipeline, ImGui
// draw callback, encoder access -- proven independently of any real shader's
// maths, and the honest way to prove it is a test that runs on BOTH backends and
// gets the same answer. A Metal-only test could not do that: imgui_test_engine
// has no Skipped status (ImGuiTestStatus is Unknown/Success/Queued/Running/
// Error/Suspended), so a test that returns early under OpenGL is counted as
// PASSED on the default backend -- a permanent false green in the headline
// number.
class CRenderShaderFlatColorOpenGL4 : public CRenderShaderOpenGL4
{
public:
	CRenderShaderFlatColorOpenGL4(CRenderBackendOpenGL4 *renderBackend, float r, float g, float b, float a);

	virtual const char *GetFragmentShaderSource() override;
	virtual void GetUniformsLocations() override;
	virtual void SetShaderVars() override;

private:
	float color[4];
	GLint attribLocationFlatColor;
};

#endif
