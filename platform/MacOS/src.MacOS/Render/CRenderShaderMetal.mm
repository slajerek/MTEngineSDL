#include "Generated/MTDefaultMetallib.h"
#include "Generated/MTFlatColorMetallib.h"
#include "CRenderShaderMetal.h"
#include "CRenderBackendMetal.h"
#include "DBG_Log.h"
#include "VID_Main.h"
#include "imgui.h"

#import <Metal/Metal.h>

CRenderShaderMetal::CRenderShaderMetal(CRenderBackendMetal *renderBackend, const char *shaderName)
: name(shaderName)
{
	LOGD("CRenderShaderMetal: create shader %s", shaderName);
	this->renderBackend = renderBackend;
	this->libraryPtr = NULL;
	this->pipelinePtr = NULL;
	this->compileFailed = false;
	this->isCompiled = false;
}

CRenderShaderMetal::~CRenderShaderMetal()
{
	if (pipelinePtr != NULL)
	{
		id<MTLRenderPipelineState> pipeline = (__bridge_transfer id<MTLRenderPipelineState>)pipelinePtr;
		pipeline = nil;
		pipelinePtr = NULL;
	}
	if (libraryPtr != NULL)
	{
		id<MTLLibrary> library = (__bridge_transfer id<MTLLibrary>)libraryPtr;
		library = nil;
		libraryPtr = NULL;
	}
}

const char *CRenderShaderMetal::GetVertexFunctionName()
{
	return "mtVertexMain";
}

const char *CRenderShaderMetal::GetFragmentFunctionName()
{
	return "mtFragmentMain";
}

const void *CRenderShaderMetal::GetEmbeddedLibraryData(unsigned long *outLength)
{
	if (outLength != NULL)
		*outLength = kMTDefaultMetallibLength;
	return kMTDefaultMetallibData;
}

// The default MSL: the exact equivalent of CRenderShaderOpenGL4's default pair
// -- pass UV and colour through, project by the ortho matrix ImGui's Metal
// backend already puts in vertex buffer slot 1, modulate the texture in slot 0.
//
// The binding indices are NOT free choices. They mirror imgui_impl_metal.mm:
// vertex buffer 0 is the ImDrawVert stream, vertex buffer 1 is the projection,
// fragment texture 0 is the atlas. Buffer bindings persist across a pipeline
// change on the same encoder, so a shader that keeps these slots inherits
// ImGui's own state and only has to bind what it adds.
const char *CRenderShaderMetal::GetMetalShaderSource()
{
	return kMTDefaultMetalSource;
}

bool CRenderShaderMetal::LoadLibrary()
{
	id<MTLDevice> device = (__bridge id<MTLDevice>)renderBackend->GetMetalDevice();
	if (device == nil)
	{
		LOGError("CRenderShaderMetal('%s'): no Metal device yet", name);
		return false;
	}

	NSError *error = nil;
	id<MTLLibrary> library = nil;

	unsigned long embeddedLength = 0;
	const void *embedded = GetEmbeddedLibraryData(&embeddedLength);
	if (embedded != NULL && embeddedLength > 0)
	{
		// SHIPPING PATH. dispatch_data_create with DISPATCH_DATA_DESTRUCTOR_DEFAULT
		// would copy; the blob is a const array in the binary that outlives
		// everything, so the no-op destructor avoids the copy and is safe here
		// precisely because the storage is static.
		dispatch_data_t blob = dispatch_data_create(embedded, (size_t)embeddedLength,
													dispatch_get_main_queue(),
													^{ /* static storage, nothing to free */ });
		library = [device newLibraryWithData:blob error:&error];
		if (library == nil)
		{
			LOGError("CRenderShaderMetal('%s'): embedded .metallib failed to load: %s", name,
					 error ? [[error localizedDescription] UTF8String] : "(no error object)");
			lastCompileLog = error ? [[error localizedDescription] UTF8String]
								   : "embedded .metallib failed to load";
			return false;
		}
	}
	else
	{
		// DEVELOPMENT PATH.
		NSString *source = [NSString stringWithUTF8String:GetMetalShaderSource()];
		library = [device newLibraryWithSource:source options:nil error:&error];
		if (library == nil)
		{
			// Print the compiler diagnostic. A Metal shader that fails to compile
			// otherwise renders nothing with no explanation at all, which is the
			// single most expensive way to debug this.
			LOGError("CRenderShaderMetal('%s'): MSL compile failed: %s", name,
					 error ? [[error localizedDescription] UTF8String] : "(no error object)");
			// RETURNED, not only logged -- a host's error panel shows this.
			lastCompileLog = error ? [[error localizedDescription] UTF8String]
								   : "MSL compile failed (no error object)";
			return false;
		}
	}

	libraryPtr = (__bridge_retained void *)library;
	return true;
}

void CRenderShaderMetal::CompileShaders()
{
	if (isCompiled || compileFailed)
		return;

	lastCompileLog.clear();

	@autoreleasepool
	{
		if (!LoadLibrary())
		{
			compileFailed = true;
			return;
		}

		id<MTLLibrary> library = (__bridge id<MTLLibrary>)libraryPtr;
		id<MTLDevice> device = (__bridge id<MTLDevice>)renderBackend->GetMetalDevice();

		id<MTLFunction> vertexFunction = [library newFunctionWithName:
										  [NSString stringWithUTF8String:GetVertexFunctionName()]];
		id<MTLFunction> fragmentFunction = [library newFunctionWithName:
											[NSString stringWithUTF8String:GetFragmentFunctionName()]];
		if (vertexFunction == nil || fragmentFunction == nil)
		{
			LOGError("CRenderShaderMetal('%s'): library has no '%s'/'%s'", name,
					 GetVertexFunctionName(), GetFragmentFunctionName());
			lastCompileLog += "library has no ";
			lastCompileLog += GetVertexFunctionName();
			lastCompileLog += "/";
			lastCompileLog += GetFragmentFunctionName();
			lastCompileLog += "\n";
			compileFailed = true;
			return;
		}

		// The vertex descriptor MUST match imgui_impl_metal.mm's, because the
		// vertex buffer this pipeline draws from is ImGui's own ImDrawVert
		// stream -- the callback only swaps the pipeline, it does not supply
		// geometry.
		MTLVertexDescriptor *vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
		vertexDescriptor.attributes[0].offset = offsetof(ImDrawVert, pos);
		vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2;
		vertexDescriptor.attributes[0].bufferIndex = 0;
		vertexDescriptor.attributes[1].offset = offsetof(ImDrawVert, uv);
		vertexDescriptor.attributes[1].format = MTLVertexFormatFloat2;
		vertexDescriptor.attributes[1].bufferIndex = 0;
		vertexDescriptor.attributes[2].offset = offsetof(ImDrawVert, col);
		vertexDescriptor.attributes[2].format = MTLVertexFormatUChar4;
		vertexDescriptor.attributes[2].bufferIndex = 0;
		vertexDescriptor.layouts[0].stepRate = 1;
		vertexDescriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
		vertexDescriptor.layouts[0].stride = sizeof(ImDrawVert);

		MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
		descriptor.vertexFunction = vertexFunction;
		descriptor.fragmentFunction = fragmentFunction;
		descriptor.vertexDescriptor = vertexDescriptor;
		// Ask the BACKEND for the format rather than hardcoding BGRA8: Task 11
		// switches the surface to RGBA16Float for HDR, and a pipeline whose
		// attachment format disagrees with the render pass is a validation
		// failure at draw time, not at creation.
		descriptor.colorAttachments[0].pixelFormat =
			(MTLPixelFormat)renderBackend->GetColorPixelFormatRaw();
		descriptor.colorAttachments[0].blendingEnabled = YES;
		descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
		descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
		descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
		descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
		descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

		NSError *error = nil;
		id<MTLRenderPipelineState> pipeline =
			[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
		if (pipeline == nil)
		{
			LOGError("CRenderShaderMetal('%s'): pipeline creation failed: %s", name,
					 error ? [[error localizedDescription] UTF8String] : "(no error object)");
			lastCompileLog += error ? [[error localizedDescription] UTF8String]
								   : "pipeline creation failed";
			compileFailed = true;
			return;
		}

		pipelinePtr = (__bridge_retained void *)pipeline;
		isCompiled = true;
		LOGD("CRenderShaderMetal('%s'): compiled", name);
	}
}

void CRenderShaderMetal::UseShaderProgram()
{
	if (!isCompiled)
		return;

	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddCallback([](const ImDrawList *drawList, const ImDrawCmd *cmd)
	{
		CRenderShaderMetal *shader = (CRenderShaderMetal *)cmd->UserCallbackData;

		// Fetch the encoder FRESHLY and tolerate nil: NewFrame() aborts the
		// frame when there is no drawable (a 0x0 or fully occluded window), and
		// the draw lists are still walked afterwards.
		CRenderBackendMetal *backend = (CRenderBackendMetal *)VID_GetRenderBackend();
		if (backend == NULL)
			return;
		id<MTLRenderCommandEncoder> encoder =
			(__bridge id<MTLRenderCommandEncoder>)backend->GetCurrentRenderCommandEncoder();
		if (encoder == nil)
			return;

		id<MTLRenderPipelineState> pipeline = (__bridge id<MTLRenderPipelineState>)shader->pipelinePtr;
		if (pipeline == nil)
			return;

		[encoder setRenderPipelineState:pipeline];
		shader->SetShaderVars((__bridge void *)encoder);
	}, (void *)this);
}

void CRenderShaderMetal::SetShaderVars(void *encoder)
{
}

void CRenderShaderMetal::ResetState()
{
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// --- flat colour proof shader ---------------------------------------------

CRenderShaderFlatColorMetal::CRenderShaderFlatColorMetal(CRenderBackendMetal *renderBackend,
														 float r, float g, float b, float a)
: CRenderShaderMetal(renderBackend, "FlatColor")
{
	color[0] = r;
	color[1] = g;
	color[2] = b;
	color[3] = a;
}

const char *CRenderShaderFlatColorMetal::GetMetalShaderSource()
{
	return kMTFlatColorMetalSource;
}

const void *CRenderShaderFlatColorMetal::GetEmbeddedLibraryData(unsigned long *outLength)
{
	if (outLength != NULL)
		*outLength = kMTFlatColorMetallibLength;
	return kMTFlatColorMetallibData;
}

void CRenderShaderFlatColorMetal::SetShaderVars(void *encoder)
{
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)encoder;
	[enc setFragmentBytes:color length:sizeof(color) atIndex:0];
}
