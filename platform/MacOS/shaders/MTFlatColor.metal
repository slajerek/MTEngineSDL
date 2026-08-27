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
	return out;
}

fragment half4 mtFragmentMain(MTVertexOut in [[stage_in]],
							  constant float4 &flatColor [[buffer(0)]])
{
	return half4(flatColor);
}
