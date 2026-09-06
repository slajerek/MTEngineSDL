#ifndef _CRenderShaderOpenGL4_h_
#define _CRenderShaderOpenGL4_h_

#include "CRenderBackendOpenGL4.h"
#include "CRenderShader.h"
#include <GL/gl3w.h>
#include <string>

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
	GLuint shaderHandle = 0;

	// Set when a compile/link has already failed, so UseShaderProgram() stops
	// retrying it every frame. See CompileShaders() for why the isCompiled fix
	// alone would have been a regression.
	bool compileAttemptedAndFailed = false;
	
	GLint  attribLocationTex;       // Uniforms location
	GLint  attribLocationProjMtx;
	GLuint attribLocationVtxPos;    // Vertex attributes location
	GLuint attribLocationVtxUV;
	GLuint attribLocationVtxColor;

	ImVec2 windowPos;
	ImVec2 windowSize;
	
	bool CheckShader(GLuint handle, const char* desc);
	bool CheckProgram(GLuint handle, const char* desc);

	// The driver's diagnostics from the most recent CompileShaders(), for a
	// subclass that must RETURN them rather than log them. Cleared at the top
	// of every compile, appended to by CheckShader/CheckProgram.
	//
	// It exists because LOGError used to be a no-op under GLOBAL_DEBUG_OFF (since 2026-09-05
	// errors are always on; the returned log stays, because a host must SHOW
	// the text, not only have it printed), which was
	// set on Linux -- so a subclass that showed the log instead would show an
	// empty panel precisely where a headless CI run is the only way anyone
	// sees the failure. See CRenderShaderCustomFragment.h.
	std::string lastCompileLog;
	
	CRenderBackendOpenGL4 *renderBackend;
};

#endif
