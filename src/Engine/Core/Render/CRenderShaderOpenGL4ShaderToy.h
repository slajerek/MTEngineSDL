#ifndef _CRenderShaderOpenGL4ShaderToy_h_
#define _CRenderShaderOpenGL4ShaderToy_h_

#include "CRenderShaderOpenGL4.h"

class CRenderShaderOpenGL4ShaderToy : public CRenderShaderOpenGL4
{
public:
	CRenderShaderOpenGL4ShaderToy(CRenderBackendOpenGL4 *renderBackend, const char *shaderName, float screenWidth, float screenHeight);
	virtual ~CRenderShaderOpenGL4ShaderToy();
	
	// to be overriden by shader
	virtual const char *GetVertexShaderSource();
	virtual const char *GetFragmentShaderSource();
	virtual void GetUniformsLocations();

	virtual void SetShaderVars();
	
	GLint attribLocationResolution;
	GLint attribLocationTime;
	GLint attribLocationWindowPos;
	
	u64 startTime;
};

#endif
