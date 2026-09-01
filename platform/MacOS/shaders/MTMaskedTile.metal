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

struct MTTileUniforms
{
	float2 tilePos;    // tile origin in PHYSICAL pixels, y down
	float2 tileSize;   // tile size in physical pixels
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

fragment half4 mtFragmentMain(MTVertexOut in [[stage_in]],
							  constant MTTileUniforms &tile [[buffer(0)]],
							  texture2d<half, access::sample> pieceTexture [[texture(0)]],
							  texture2d<half, access::sample> maskTexture  [[texture(1)]])
{
	constexpr sampler linearSampler(coord::normalized, address::clamp_to_edge,
									min_filter::linear, mag_filter::linear);

	half4 piece = pieceTexture.sample(linearSampler, in.uv);

	// in.position, NOT a separate `float4 [[position]]` parameter. Declaring
	// [[position]] twice -- once in the stage_in struct and once as a parameter
	// -- is a hard MSL compile error, and it is the reason this shader silently
	// never built before the shaders were compiled at BUILD time: the failure
	// was a log line at first use, and the game app drew the hex grid unmasked
	// instead. After rasterisation in.position holds the framebuffer coordinate,
	// which is what gl_FragCoord gives the GLSL original.
	float2 maskUV = (in.position.xy - tile.tilePos) / tile.tileSize;

	float maskAlpha = (float)maskTexture.sample(linearSampler, maskUV).a;

	if (maskUV.x < 0.0 || maskUV.x > 1.0 ||
		maskUV.y < 0.0 || maskUV.y > 1.0 ||
		maskAlpha < 0.5)
	{
		discard_fragment();
	}

	return half4(in.color) * piece;
}
