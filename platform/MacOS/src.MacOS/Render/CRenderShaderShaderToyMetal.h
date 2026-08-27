#ifndef _CRenderShaderShaderToyMetal_h_
#define _CRenderShaderShaderToyMetal_h_

#include "CRenderShaderMetal.h"
#include "SYS_Types.h"

// Metal port of CRenderShaderOpenGL4ShaderToy.
//
// It has no app caller right now, and that is not a reason to leave it
// OpenGL-only. It is ENGINE API -- c64d's commented-out construction in
// CViewEmulatorScreen shows it was reachable once and is meant to be again --
// and an engine facility that works on one backend and silently not the other
// is a trap for whoever picks it up next, which is the exact situation this
// stage exists to clear up.
//
// The GL original carries several alternative mainImage* bodies with all but
// one commented out at the call site. Only the one actually selected there
// (the Mandelbrot) is ported; porting dead alternatives would mean maintaining
// MSL nobody runs. The GLSL for the others stays where it is.
class CRenderShaderShaderToyMetal : public CRenderShaderMetal
{
public:
	CRenderShaderShaderToyMetal(CRenderBackendMetal *renderBackend, const char *shaderName,
								float screenWidth, float screenHeight);

	virtual const char *GetMetalShaderSource() override;
	virtual const void *GetEmbeddedLibraryData(unsigned long *outLength) override;
	virtual void SetShaderVars(void *encoder) override;

	u64 startTime;
};

#endif
