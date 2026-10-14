// Option C (gain-map resident HDR): an RGBA8 base drawn with its RGBA8 companion
// gain map, applied PER PIXEL. The maths is MT_GainMapEvaluate's (MT_UltraHdr.cpp),
// the CPU twin that a host's tests prove against libultrahdr and whose output
// a GPU parity test compares this shader with.
//
// Compiled WITH the Metal compiler's default fast math (Task 3c, 2026-10-08). Task 3
// had turned it off (-fno-fast-math through an embed-metal-shaders-flags line);
// Task 8 measured fast math on every parity case -- 1:1, magnified, minified, three
// map shapes -- with no ratio worse than the precise build's (worst 0.102 of the
// bound either way), while the precise transcendentals cost 3.5 ms per frame for a
// 12-cell grid against 0.75 ms. The flag line mechanism stays in the embed script.
//
// Two users of mtFragmentMain: the per-frame draw (grid and filmstrip cells, through
// the ImGui callback) and the ONE-TIME COMPOSITE (flags.w == 2: one apply per base
// texel into an RGBA16F texture at the base's full resolution, which the main pane
// then draws like a float-arm texture). mtFragmentPlain is ImGui's own fragment
// function (imgui_impl_metal.mm), for the test draw of a composite through the
// float arm's path.
//
// Bindings: vertex buffer 0 = ImGui's ImDrawVert stream, vertex buffer 1 = its
// projection (imgui_impl_metal.mm), fragment texture 0 + sampler 0 = the DRAW's own
// base texture and sampler (ImGui / ScopedImageSampler bind them), fragment
// texture 1 = the map, fragment buffer 0 = SMTGainMapUniforms (MT_GainMapUniforms.h),
// all carried by the per-draw callback.
#include <metal_stdlib>
using namespace metal;

struct MTVertexIn
{
	float2 position [[attribute(0)]];
	float2 uv       [[attribute(1)]];
	uchar4 color    [[attribute(2)]];
};

struct MTVertexOut
{
	float4 position [[position]];
	float2 uv;
	float4 color;
};

struct MTUniforms
{
	float4x4 projectionMatrix;
};

vertex MTVertexOut mtVertexMain(MTVertexIn in [[stage_in]],
								constant MTUniforms &uniforms [[buffer(1)]])
{
	MTVertexOut out;
	out.position = uniforms.projectionMatrix * float4(in.position, 0, 1);
	out.uv = in.uv;
	out.color = float4(in.color) / float4(255.0);
	return out;
}

// MT_GainMapUniforms.h, byte for byte (12 float4 = 192 bytes).
struct MTGainMapUniforms
{
	float4 logMin, logMax, invGamma, offSdr, offHdr;
	float4 m0, m1, m2;
	float4 baseRect, mapRect;
	float4 flags;     // x: encodeSrgb, y: mapChannels, z: hdrMax, w: 0 nearest mag, 1 linear mag, 2 composite
	float4 mapAffine; // t * xy + zw
};

static float3 srgbDecode(float3 v)   // MT_SrgbCurve.h SrgbExtendedDecode, sign-symmetric
{
	float3 a = abs(v);
	return sign(v) * select(pow((a + 0.055) / 1.055, 2.4), a / 12.92, a <= 0.04045);
}

static float3 srgbEncode(float3 v)   // MT_SrgbCurve.h SrgbExtendedEncode, sign-symmetric
{
	float3 a = abs(v);
	return sign(v) * select(1.055 * pow(a, 1.0 / 2.4) - 0.055, a * 12.92, a <= 0.0031308);
}

// Steps 2-6 for one base texel value `b` at base-texture coordinate `uv` (a base
// TEXEL CENTRE -- the twin applies one gain per base pixel): the map is read at uv's
// own position (the position rule, mapAffine), result surface-encoded.
static float3 applyAt(constant MTGainMapUniforms &u, texture2d<float, access::sample> mapTexture,
					  float3 b, float2 uv)
{
	float2 t = (uv - u.baseRect.xy) / (u.baseRect.zw - u.baseRect.xy);
	// texel i at base pixel offset + i * s
	float2 muv = u.mapRect.xy + (t * u.mapAffine.xy + u.mapAffine.zw) * (u.mapRect.zw - u.mapRect.xy);
	// The map's bilinear blend done HERE, in float, not by the sampler: the hardware's
	// filter weights carry ~8 fractional bits, and the map gamma's pow (steep near 0)
	// and the sRGB encode's linear toe (x 12.92 near black) amplify that past the
	// parity bound (review round 1: gamma 2.2 and 3-channel maps, 1.1-1.6x the bound).
	// Clamp-to-edge, as the twin's clamped indices.
	// The clamp limits are the map IMAGE's texels inside its texture (mapRect), not
	// the texture's: a texture padded to a power of two holds zeros past the image.
	float2 msz = float2(mapTexture.get_width(), mapTexture.get_height());
	int2 mlo = int2(round(u.mapRect.xy * msz));
	int2 mhi = int2(round(u.mapRect.zw * msz)) - 1;
	float2 mq = muv * msz - 0.5;
	float2 mf = mq - floor(mq);
	int2 m0 = int2(floor(mq));
	uint2 a00 = uint2(clamp(m0, mlo, mhi));
	uint2 a11 = uint2(clamp(m0 + int2(1), mlo, mhi));
	float3 g = (mapTexture.read(uint2(a00.x, a00.y)).rgb * (1.0 - mf.x) + mapTexture.read(uint2(a11.x, a00.y)).rgb * mf.x) * (1.0 - mf.y)
			 + (mapTexture.read(uint2(a00.x, a11.y)).rgb * (1.0 - mf.x) + mapTexture.read(uint2(a11.x, a11.y)).rgb * mf.x) * mf.y;
	g = select(pow(g, u.invGamma.xyz), g, u.invGamma.xyz == 1.0);
	float3 logBoost = u.logMin.xyz * (1.0 - g) + u.logMax.xyz * g;
	float3 hdr = clamp((srgbDecode(b) + u.offSdr.xyz) * exp2(logBoost) - u.offHdr.xyz, 0.0, u.flags.z);
	float3 o = float3(dot(u.m0.xyz, hdr), dot(u.m1.xyz, hdr), dot(u.m2.xyz, hdr));
	return (u.flags.x > 0.5) ? srgbEncode(o) : o;
}

fragment half4 mtFragmentMain(MTVertexOut in [[stage_in]],
							  constant MTGainMapUniforms &u [[buffer(0)]],
							  texture2d<float, access::sample> baseTexture [[texture(0)]],
							  sampler baseSampler [[sampler(0)]],
							  texture2d<float, access::sample> mapTexture  [[texture(1)]])
{
	// Decision R-1. The float arm's texture holds APPLIED values and the draw's sampler
	// filters those: linear 2x2 when minified (no mips: uncompressed textures are created
	// mipmapped:NO), nearest when a cache image magnifies, linear when a linearScaling
	// image does. Filtering base and map first and applying afterwards would not commute
	// with the nonlinear apply, so when the hardware would blend, blend APPLIED texels.
	float2 tsz = float2(baseTexture.get_width(), baseTexture.get_height());
	float2 p = in.uv * tsz;
	// The base IMAGE's texels inside its texture (baseRect): clamp-to-edge at the
	// image's edge, never into power-of-two padding.
	int2 lo = int2(round(u.baseRect.xy * tsz));
	int2 hi = int2(round(u.baseRect.zw * tsz)) - 1;
	float2 dx = dfdx(p), dy = dfdy(p);
	bool minified = max(dot(dx, dx), dot(dy, dy)) > 1.0;   // texel footprint > 1: the min filter
	// The composite (flags.w == 2) is a 1:1 pass, one fragment per base texel: it
	// takes the one-texel branch unconditionally, so a derivative that rounds a hair
	// above 1 can never blend neighbours into the texture the float arm's path reads.
	if (u.flags.w > 1.5 || (!minified && u.flags.w < 0.5))
	{
		// nearest magnification: ONE base texel, and the map applied at THAT TEXEL'S
		// CENTRE -- one applied value per base texel, as the float arm's texture holds
		// (Decision R-1). Applying at in.uv would vary the gain inside one base pixel
		// when zoomed in (review round 1). The texel is READ at floor(p) rather than
		// sampled: the sampler's own sub-texel rounding could pick the neighbour of the
		// texel whose gain is applied, a one-fragment sliver at a steep gain edge at
		// non-integral zooms (review round 2). baseSampler stays bound (the draw's own)
		// but this branch does not need it.
		int2 ib = clamp(int2(floor(p)), lo, hi);
		float4 b = baseTexture.read(uint2(ib));
		float2 tc = (float2(ib) + 0.5) / tsz;
		return half4(in.color) * half4(half3(applyAt(u, mapTexture, b.rgb, tc)), half(b.a));
	}
	float2 q = p - 0.5;
	float2 f = fract(q);
	int2 i0 = int2(floor(q));
	float4 acc = 0.0;
	for (int k = 0; k < 4; k++)
	{
		int2 o = int2(k & 1, k >> 1);
		int2 ik = clamp(i0 + o, lo, hi);                           // clamp-to-edge at the image's edge
		float4 bk = baseTexture.read(uint2(ik));
		float3 ok = applyAt(u, mapTexture, bk.rgb, (float2(ik) + 0.5) / tsz);
		float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
		acc += w * float4(ok, bk.a);
	}
	return half4(in.color) * half4(acc);
}

// ImGui's own fragment function, verbatim (imgui_impl_metal.mm): the vertex colour
// times the texture as the draw's sampler filters it. The test draw of a composite
// (CGainMapShader::DrawTextureToTargetForTest) goes through this, so it sees what a
// composite drawn on screen -- a float-arm image's path -- puts there.
fragment half4 mtFragmentPlain(MTVertexOut in [[stage_in]],
							   texture2d<half, access::sample> texture [[texture(0)]],
							   sampler textureSampler [[sampler(0)]])
{
	half4 texColor = texture.sample(textureSampler, in.uv);
	return half4(in.color) * texColor;
}
