#include "CRenderShaderFlatColorOpenGL4.h"
#include "DBG_Log.h"

CRenderShaderFlatColorOpenGL4::CRenderShaderFlatColorOpenGL4(CRenderBackendOpenGL4 *renderBackend,
															 float r, float g, float b, float a)
: CRenderShaderOpenGL4(renderBackend, "FlatColor")
{
	color[0] = r;
	color[1] = g;
	color[2] = b;
	color[3] = a;
	attribLocationFlatColor = -1;
}

const char *CRenderShaderFlatColorOpenGL4::GetFragmentShaderSource()
{
	return R"(
		in vec2 Frag_UV;
		in vec4 Frag_Color;
		uniform vec4 iFlatColor;
		layout (location = 0) out vec4 Out_Color;
		void main()
		{
			Out_Color = iFlatColor;
		}
	)";
}

void CRenderShaderFlatColorOpenGL4::GetUniformsLocations()
{
	attribLocationFlatColor = GetUniformLocation("iFlatColor");
}

void CRenderShaderFlatColorOpenGL4::SetShaderVars()
{
	glUniform4f(attribLocationFlatColor, color[0], color[1], color[2], color[3]);
}
