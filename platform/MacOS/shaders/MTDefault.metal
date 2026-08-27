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

fragment half4 mtFragmentMain(MTVertexOut in [[stage_in]],
							  texture2d<half, access::sample> texture [[texture(0)]])
{
	constexpr sampler linearSampler(coord::normalized, min_filter::linear,
									mag_filter::linear, mip_filter::linear);
	half4 texel = texture.sample(linearSampler, in.uv);
	return half4(in.color) * texel;
}
