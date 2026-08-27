#include "Generated/MTShaderToyMetallib.h"
#include "CRenderShaderShaderToyMetal.h"
#include "CRenderBackendMetal.h"
#include "SYS_Main.h"
#include "DBG_Log.h"

#import <Metal/Metal.h>

CRenderShaderShaderToyMetal::CRenderShaderShaderToyMetal(CRenderBackendMetal *renderBackend,
														 const char *shaderName,
														 float screenWidth, float screenHeight)
: CRenderShaderMetal(renderBackend, shaderName)
{
	this->screenWidth = screenWidth;
	this->screenHeight = screenHeight;
	this->startTime = SYS_GetCurrentTimeInMillis();
}

const char *CRenderShaderShaderToyMetal::GetMetalShaderSource()
{
	return kMTShaderToyMetalSource;
}

const void *CRenderShaderShaderToyMetal::GetEmbeddedLibraryData(unsigned long *outLength)
{
	if (outLength != NULL)
		*outLength = kMTShaderToyMetallibLength;
	return kMTShaderToyMetallibData;
}

void CRenderShaderShaderToyMetal::SetShaderVars(void *encoder)
{
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)encoder;

	u64 t = SYS_GetCurrentTimeInMillis() - startTime;
	float uniforms[4] = { screenWidth, screenHeight, (float)t / 1000.0f, 0.0f };
	[enc setFragmentBytes:uniforms length:sizeof(uniforms) atIndex:0];
}
