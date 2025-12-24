#ifndef _CRenderShader_h_
#define _CRenderShader_h_

#include "CRenderBackend.h"

class CRenderShader
{
public:
	CRenderShader();
	virtual ~CRenderShader();
	
	// to be overriden by shader
	virtual const char *GetVertexShaderSource();
	virtual const char *GetFragmentShaderSource();
	
	// create shader program, to be overrided by shader engine (OpenGL, Vulkan, Metal, ...)
	virtual void CompileShaders();
	virtual void UseShaderProgram();
	virtual void ResetState();
	virtual void SetResolution(float screenWidth, float screenHeight);
	float screenWidth, screenHeight;
	
	bool isCompiled;
};

#endif
