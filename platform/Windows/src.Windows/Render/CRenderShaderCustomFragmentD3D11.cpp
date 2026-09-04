#include "CRenderShaderCustomFragmentD3D11.h"

#if defined(MT_RENDER_BACKEND_D3D11)

#include "CRenderBackendD3D11.h"
#include "DBG_Log.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>

// imgui_impl_dx11.cpp already carries this pragma, so the library is linked
// into every D3D11 build. Repeating it here is deliberate: relying on another
// translation unit's pragma is a link that breaks silently the day that file
// changes.
#pragma comment(lib, "d3dcompiler")

// The preamble. GetPreambleLineCount() COUNTS this string.
//
// THE CBUFFER LAYOUT MIRRORS SShaderToyUniforms EXACTLY, all 240 bytes of it,
// and two members are spelled unlike their GLSL counterparts for that reason:
//
//   * iChannelTime is a float4, not float[4]. HLSL pads EVERY array element to
//     a full 16-byte register, so float iChannelTime[4] would occupy 64 bytes
//     and push everything after it. A float4 indexes identically -- iChannelTime[2]
//     compiles in both languages -- and occupies the 16 bytes the C++ struct has.
//
//   * iChannelResolution is float4[4], because that same padding rule is what
//     forced the C++ array to a 16-byte stride in the first place. Reading
//     .xyz gives the vec3 a shader expects.
//
// The two explicit pads are load-bearing: drop _pad0 and iMouse moves to 24 in
// C++ while HLSL still reads it at 32; drop _pad1 and every array after it
// shifts by 12 bytes. The static_asserts in CRenderShaderCustomFragment.h pin
// the C++ side; this comment is the other half of that pin.
//
// PS_INPUT MUST MATCH imgui_impl_dx11's own vertex-shader output signature
// exactly, semantics and order included: float4 SV_POSITION, float4 COLOR0,
// float2 TEXCOORD0. If an ImGui upgrade changes it, this changes with it, and
// nothing will tell you -- a mismatched PS input is only a compile error when
// the shader is compiled against the same signature, which it is not.
//
// t1..t4 AND s1..s4, never 0: imgui_impl_dx11 binds the draw command's texture
// at t0 and its own sampler at s0, after the callback that installs this
// shader, so slot 0 is not ours.
static const char *kPreamble =
	"cbuffer ShaderToyConstants : register(b0)\n"
	"{\n"
	"    float3 iResolution; float iTime;\n"
	"    float iTimeDelta; float iFrameRate; int iFrame; float _pad0;\n"
	"    float4 iMouse;\n"
	"    float4 iDate;\n"
	"    float4 iChannelTime;\n"
	"    float4 iChannelResolution[4];\n"
	"    float iSampleRate; float3 _pad1;\n"
	"    float4 iChannelUvTransform[4];\n"
	"    float4 iChannelWrap;\n"
	"};\n"
	"struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
	"Texture2D iChannel0 : register(t1);\n"
	"Texture2D iChannel1 : register(t2);\n"
	"Texture2D iChannel2 : register(t3);\n"
	"Texture2D iChannel3 : register(t4);\n"
	"SamplerState iChannel0Sampler : register(s1);\n"
	"SamplerState iChannel1Sampler : register(s2);\n"
	"SamplerState iChannel2Sampler : register(s3);\n"
	"SamplerState iChannel3Sampler : register(s4);\n"
	"float2 mtChannelUV(int n, float2 uv)\n"
	"{\n"
	// VFLIP FIRST -- see the GLSL copy for why: ShaderToy's fragCoord is
	// bottom-left, a texture's v=0 is its top row, and shadertoy.com defaults
	// the same per-channel vflip to on.
	"    if (iChannelUvTransform[n].z > 0.5) uv.y = 1.0 - uv.y;\n"
	"    uv = (iChannelWrap[n] > 0.5) ? frac(uv) : clamp(uv, 0.0, 1.0);\n"
	"    return uv * iChannelUvTransform[n].xy;\n"
	"}\n"
	"#define texChannel0(uv) iChannel0.Sample(iChannel0Sampler, mtChannelUV(0, uv))\n"
	"#define texChannel1(uv) iChannel1.Sample(iChannel1Sampler, mtChannelUV(1, uv))\n"
	"#define texChannel2(uv) iChannel2.Sample(iChannel2Sampler, mtChannelUV(2, uv))\n"
	"#define texChannel3(uv) iChannel3.Sample(iChannel3Sampler, mtChannelUV(3, uv))\n"
	"#define MAIN_IMAGE void mainImage(out float4 fragColor, float2 fragCoord)\n"
	"void mainImage(out float4 fragColor, float2 fragCoord);\n";

static const char *kEntry =
	"\n"
	"float4 CustomFragmentPS(PS_INPUT input) : SV_TARGET\n"
	"{\n"
	"    // ShaderToy's origin is bottom-left and its coordinate is in pixels;\n"
	"    // ImGui's UV is top-left and normalised.\n"
	"    float2 fragCoord = float2(input.uv.x * iResolution.x,\n"
	"                              (1.0 - input.uv.y) * iResolution.y);\n"
	"    float4 outColor = float4(0, 0, 0, 0);\n"
	"    mainImage(outColor, fragCoord);\n"
	"    // Modulate by the vertex colour, as the GLSL and MSL entries do.\n"
	"    return outColor * input.col;\n"
	"}\n";

CRenderShaderCustomFragmentD3D11::CRenderShaderCustomFragmentD3D11(
		CRenderBackendD3D11 *renderBackend, const char *name)
: renderBackend(renderBackend), name(name != NULL ? name : ""),
  pixelShaderPtr(NULL), constantBufferPtr(NULL), uniforms()
{
	isCompiled = false;
}

CRenderShaderCustomFragmentD3D11::~CRenderShaderCustomFragmentD3D11()
{
	// Both guarded: the seam test creates one of these and deletes it without
	// ever compiling, so neither pointer is guaranteed to exist.
	if (constantBufferPtr != NULL) { ((ID3D11Buffer *)constantBufferPtr)->Release(); constantBufferPtr = NULL; }
	if (pixelShaderPtr != NULL)    { ((ID3D11PixelShader *)pixelShaderPtr)->Release(); pixelShaderPtr = NULL; }
	// The channel objects are ours and are created lazily, so each is guarded
	// for exactly the same reason.
	if (samplerNearestPtr != NULL) { ((ID3D11SamplerState *)samplerNearestPtr)->Release(); samplerNearestPtr = NULL; }
	if (samplerLinearPtr != NULL)  { ((ID3D11SamplerState *)samplerLinearPtr)->Release(); samplerLinearPtr = NULL; }
	if (blackTextureViewPtr != NULL) { ((ID3D11ShaderResourceView *)blackTextureViewPtr)->Release(); blackTextureViewPtr = NULL; }
}

int CRenderShaderCustomFragmentD3D11::GetPreambleLineCount()
{
	int lines = 0;
	for (const char *p = kPreamble; *p != '\0'; p++)
	{
		if (*p == '\n')
			lines++;
	}
	return lines;
}

bool CRenderShaderCustomFragmentD3D11::SetFragmentSource(const char *mainImageSource)
{
	if (mainImageSource == NULL)
		mainImageSource = "";

	lastCompileLog.clear();

	fullSource  = kPreamble;
	fullSource += mainImageSource;
	fullSource += "\n";
	fullSource += kEntry;

	ID3D11Device *device = (ID3D11Device *)(renderBackend ? renderBackend->GetD3DDevice() : NULL);
	if (device == NULL)
	{
		lastCompileLog = "no D3D device yet";
		LOGError("CRenderShaderCustomFragmentD3D11('%s'): no D3D device yet", name.c_str());
		return false;
	}

	// ps_4_0, NOT ps_5_0 -- the floor tools/embed-hlsl-shaders.ps1 pins for the
	// engine's own shaders, so a host's snippet cannot accidentally require a
	// profile the rest of the tree has decided not to depend on.
	ID3DBlob *blob = NULL;
	ID3DBlob *errorBlob = NULL;
	HRESULT hr = D3DCompile(fullSource.c_str(), fullSource.size(), NULL, NULL, NULL,
							"CustomFragmentPS", "ps_4_0", 0, 0, &blob, &errorBlob);
	if (FAILED(hr))
	{
		// RETURNED, not only logged. The blob is not NUL-terminated by contract,
		// so bound the copy by its size.
		if (errorBlob != NULL)
		{
			lastCompileLog.assign((const char *)errorBlob->GetBufferPointer(),
								  (size_t)errorBlob->GetBufferSize());
			errorBlob->Release();
		}
		else
		{
			lastCompileLog = "D3DCompile failed with no diagnostic";
		}
		if (blob != NULL)
			blob->Release();
		LOGError("CRenderShaderCustomFragmentD3D11('%s'): compile failed: %s",
				 name.c_str(), lastCompileLog.c_str());
		// The previous pixel shader stays bound -- nothing above touched it.
		return false;
	}
	if (errorBlob != NULL)
	{
		// A SUCCESSFUL compile can still emit warnings. Keep them: a host that
		// shows them is showing the compiler's own words.
		lastCompileLog.assign((const char *)errorBlob->GetBufferPointer(),
							  (size_t)errorBlob->GetBufferSize());
		errorBlob->Release();
	}

	ID3D11PixelShader *ps = NULL;
	hr = device->CreatePixelShader(blob->GetBufferPointer(), (SIZE_T)blob->GetBufferSize(), NULL, &ps);
	blob->Release();
	if (FAILED(hr))
	{
		lastCompileLog += "CreatePixelShader failed";
		LOGError("CRenderShaderCustomFragmentD3D11('%s'): CreatePixelShader failed (hr=0x%08x)",
				 name.c_str(), (unsigned)hr);
		return false;
	}

	// The constant buffer is created ONCE and reused. DYNAMIC with CPU write
	// access, unlike CRenderShaderFlatColorD3D11's IMMUTABLE one: its colour
	// never changes, and iTime changes every frame.
	if (constantBufferPtr == NULL)
	{
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = (UINT)sizeof(SShaderToyUniforms);   // 240, a multiple of 16
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		ID3D11Buffer *cb = NULL;
		hr = device->CreateBuffer(&bd, NULL, &cb);
		if (FAILED(hr))
		{
			lastCompileLog += "constant buffer creation failed";
			LOGError("CRenderShaderCustomFragmentD3D11('%s'): constant buffer creation failed (hr=0x%08x)",
					 name.c_str(), (unsigned)hr);
			ps->Release();
			return false;
		}
		constantBufferPtr = cb;
	}

	// SWAP ON SUCCESS: only now is the old shader released.
	if (pixelShaderPtr != NULL)
		((ID3D11PixelShader *)pixelShaderPtr)->Release();
	pixelShaderPtr = ps;
	isCompiled = true;
	return true;
}

void CRenderShaderCustomFragmentD3D11::BindTo(void *deviceContext)
{
	ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)deviceContext;
	if (ctx == NULL || pixelShaderPtr == NULL || constantBufferPtr == NULL)
		return;

	ID3D11Buffer *cb = (ID3D11Buffer *)constantBufferPtr;

	D3D11_MAPPED_SUBRESOURCE mapped;
	if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		memcpy(mapped.pData, &uniforms, sizeof(SShaderToyUniforms));
		ctx->Unmap(cb, 0);
	}

	ctx->PSSetShader((ID3D11PixelShader *)pixelShaderPtr, NULL, 0);
	// b0 OF THE PIXEL STAGE. ImGui's b0 is on the VERTEX stage (its projection
	// matrix) and its own pixel shader reads no constant buffer at all, so the
	// two do not collide -- PSSetConstantBuffers and VSSetConstantBuffers are
	// separate binding tables.
	ctx->PSSetConstantBuffers(0, 1, &cb);

	// SLOTS 1..4. A channel the host left empty gets the 1x1 black texture
	// rather than NULL: a shader sampling an unbound SRV reads zero anyway, but
	// the debug layer says so loudly on every draw, and black is what an unset
	// ShaderToy channel shows.
	ID3D11ShaderResourceView *views[kShaderChannelCount] = {};
	ID3D11SamplerState *samplers[kShaderChannelCount] = {};
	for (int i = 0; i < kShaderChannelCount; i++)
	{
		views[i] = (channelTexture[i] != NULL) ? (ID3D11ShaderResourceView *)channelTexture[i]
											   : (ID3D11ShaderResourceView *)BlackTextureView();
		samplers[i] = (ID3D11SamplerState *)SamplerFor(channelFilter[i]);
	}
	ctx->PSSetShaderResources(1, kShaderChannelCount, views);
	ctx->PSSetSamplers(1, kShaderChannelCount, samplers);
}

void *CRenderShaderCustomFragmentD3D11::SamplerFor(EShaderChannelFilter filter)
{
	void **slot = (filter == SHADER_CHANNEL_LINEAR) ? &samplerLinearPtr : &samplerNearestPtr;
	if (*slot != NULL)
		return *slot;

	ID3D11Device *device = (ID3D11Device *)(renderBackend ? renderBackend->GetD3DDevice() : NULL);
	if (device == NULL)
		return NULL;

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = (filter == SHADER_CHANNEL_LINEAR) ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
												  : D3D11_FILTER_MIN_MAG_MIP_POINT;
	// CLAMP, always -- MT_CHUV in the preamble does the wrapping, because
	// CSlrImage pads every texture to a power of two and hardware WRAP would
	// tile the padding rather than the image.
	sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
	sd.MaxLOD = D3D11_FLOAT32_MAX;

	ID3D11SamplerState *sampler = NULL;
	if (FAILED(device->CreateSamplerState(&sd, &sampler)))
		return NULL;
	*slot = sampler;
	return *slot;
}

void *CRenderShaderCustomFragmentD3D11::BlackTextureView()
{
	if (blackTextureViewPtr != NULL)
		return blackTextureViewPtr;

	ID3D11Device *device = (ID3D11Device *)(renderBackend ? renderBackend->GetD3DDevice() : NULL);
	if (device == NULL)
		return NULL;

	const unsigned char black[4] = { 0, 0, 0, 255 };

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = 1;
	td.Height = 1;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA initial = {};
	initial.pSysMem = black;
	initial.SysMemPitch = 4;

	ID3D11Texture2D *texture = NULL;
	if (FAILED(device->CreateTexture2D(&td, &initial, &texture)))
		return NULL;

	D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
	vd.Format = td.Format;
	vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	vd.Texture2D.MipLevels = 1;

	ID3D11ShaderResourceView *view = NULL;
	HRESULT hr = device->CreateShaderResourceView(texture, &vd, &view);
	// The view holds its own reference to the texture, so the texture is
	// released either way -- on failure to avoid a leak, on success because
	// nothing here needs a second handle to it.
	texture->Release();
	if (FAILED(hr))
		return NULL;

	blackTextureViewPtr = view;
	return blackTextureViewPtr;
}

void CRenderShaderCustomFragmentD3D11::UseShaderProgram()
{
	if (!isCompiled)
		return;

	ImGui::GetWindowDrawList()->AddCallback([](const ImDrawList *, const ImDrawCmd *cmd)
	{
		CRenderShaderCustomFragmentD3D11 *shader = (CRenderShaderCustomFragmentD3D11 *)cmd->UserCallbackData;
		if (shader == NULL)
			return;

		// FETCH THE CONTEXT FRESHLY AND TOLERATE NULL. The callback runs at
		// RENDER time, long after the draw that queued it, and
		// Renderer_RenderState is published only for the duration of
		// ImGui_ImplDX11_RenderDrawData -- so a callback reached outside that
		// walk (a frame NewFrame() aborted, whose draw lists are still walked)
		// must do nothing rather than dereference.
		ImGui_ImplDX11_RenderState *rs =
			(ImGui_ImplDX11_RenderState *)ImGui::GetPlatformIO().Renderer_RenderState;
		if (rs == NULL)
			return;
		shader->BindTo(rs->DeviceContext);
	}, (void *)this);
}

void CRenderShaderCustomFragmentD3D11::ResetState()
{
	// GIVE ImGui ITS PIXEL SHADER BACK, and be precise about why this is not
	// belt-and-braces: imgui_impl_dx11 captures BACKUP_DX11_STATE ONCE before
	// the whole draw walk and restores it ONCE after; inside the walk a user
	// callback gets no save and no restore at all. This reset is the ONLY thing
	// putting ImGui's pipeline back before the next draw.
	//
	// The PIXEL constant buffer is worse than the rest: BindTo() binds one, and
	// BACKUP_DX11_STATE has no PSConstantBuffer field (it backs up the VERTEX
	// one only), nor does ImGui_ImplDX11_SetupRenderState ever set a pixel
	// cbuffer -- so ours outlives even the end of the walk. Harmless only
	// because ImGui's own pixel shader reads no cbuffer, which is luck.
	//
	// The platform_io form, not the obsolete ImDrawCallback_ResetRenderState
	// sentinel: 1.92.8 deprecated the latter in favour of this.
	//
	// SLOTS 1..4 GO FIRST, in a callback of our own, and for the same reason as
	// the cbuffer above: BACKUP_DX11_STATE saves PSShaderResources for slot 0
	// only and ImGui_ImplDX11_SetupRenderState sets only slot 0, so an SRV left
	// at t1 outlives the whole draw walk and keeps its texture alive with it.
	ImGui::GetWindowDrawList()->AddCallback([](const ImDrawList *, const ImDrawCmd *)
	{
		ImGui_ImplDX11_RenderState *rs =
			(ImGui_ImplDX11_RenderState *)ImGui::GetPlatformIO().Renderer_RenderState;
		if (rs == NULL || rs->DeviceContext == NULL)
			return;
		ID3D11ShaderResourceView *none[kShaderChannelCount] = {};
		ID3D11SamplerState *noSamplers[kShaderChannelCount] = {};
		((ID3D11DeviceContext *)rs->DeviceContext)->PSSetShaderResources(1, kShaderChannelCount, none);
		((ID3D11DeviceContext *)rs->DeviceContext)->PSSetSamplers(1, kShaderChannelCount, noSamplers);
	}, NULL);

	ImGui::GetWindowDrawList()->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, NULL);
}



// --- channels -------------------------------------------------------------
//
// Pure stores, RENDER THREAD ONLY. BindTo() does the binding, inside the draw
// callback where a device context exists.

void CRenderShaderCustomFragmentD3D11::SetChannelTexture(int channel, void *texture)
{
	if (channel < 0 || channel >= kShaderChannelCount)
		return;
	channelTexture[channel] = texture;
}

void CRenderShaderCustomFragmentD3D11::SetChannelSampler(int channel, EShaderChannelFilter filter,
							  EShaderChannelWrap wrap)
{
	if (channel < 0 || channel >= kShaderChannelCount)
		return;
	channelFilter[channel] = filter;
	channelWrapMode[channel] = wrap;
}

#endif   // MT_RENDER_BACKEND_D3D11
