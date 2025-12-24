#ifndef _CRenderShaderOpenGL4_h_
#define _CRenderShaderOpenGL4_h_

#include "CRenderBackendOpenGL4.h"
#include "CRenderShader.h"
#include <GL/gl3w.h>

class CRenderShaderOpenGL4 : public CRenderShader
{
public:
	CRenderShaderOpenGL4(CRenderBackendOpenGL4 *renderBackend, const char *shaderName);
	virtual ~CRenderShaderOpenGL4();
	
	const char *name;
	
	// to be overriden by shader
	virtual const char *GetVertexShaderSource();
	virtual const char *GetFragmentShaderSource();
	
	// create shader program
	virtual void CompileShaders();
	virtual void GetUniformsLocations();
	virtual void UseShaderProgram();
	virtual void SetShaderVars();
	
	virtual GLint GetUniformLocation(const char *attribName);

	virtual void ResetState();
	
	void DebugPrintUniforms();
	
protected:
	GLuint shaderHandle;
	
	GLint  attribLocationTex;       // Uniforms location
	GLint  attribLocationProjMtx;
	GLuint attribLocationVtxPos;    // Vertex attributes location
	GLuint attribLocationVtxUV;
	GLuint attribLocationVtxColor;

	ImVec2 windowPos;
	ImVec2 windowSize;
	
	bool CheckShader(GLuint handle, const char* desc);
	bool CheckProgram(GLuint handle, const char* desc);
	
	CRenderBackendOpenGL4 *renderBackend;
};

#endif
