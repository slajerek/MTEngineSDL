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

struct MTShaderToyUniforms
{
	float2 resolution;
	float  time;
	float  padding;
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

static float3 mandelbrot(float2 z, float2 c)
{
	float l = 0.0;
	for (l = 0.0; l < 100.0; l += 1.0)
	{
		z = float2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
		if (dot(z, z) > 65536.0) break;
	}
	l = l - log2(log2(dot(z, z))) + 4.0;
	return float3(l, z);
}

fragment float4 mtFragmentMain(MTVertexOut in [[stage_in]],
							   constant MTShaderToyUniforms &st [[buffer(0)]])
{
	// ShaderToy is Y-flipped relative to the UVs, exactly as the GLSL
	// notes at the same spot.
	float2 fragCoord = float2(in.uv.x * st.resolution.x,
							  (1.0 - in.uv.y) * st.resolution.y);

	float2 uv = ((2.0 * fragCoord - st.resolution) / st.resolution.y) * 1.2;
	float2 ouv = uv - float2(0.5, 0);
	float2 nuv = fragCoord / st.resolution;
	uv.x -= 0.5;
	float res = sin(st.time / 2.0) * 8.0 + 12.0;
	uv.y += 1.0 / res / 2.0;
	float2 puv = floor(uv * res) / res;
	float ref = pow(clamp(-sqrt(mandelbrot(float2(0.0), puv).x / 100.0) + 0.3, 0.0, 8.0) * 16.0, 4.0);
	float scale = 2.6 + ref;
	float2 muv = fmod(uv, 1.0 / res) * res * scale - (scale / 2.0);

	float3 f = float3(0);
	if (nuv.x + nuv.y > 1.0)
		f = mandelbrot(muv, puv);
	else
		f = mandelbrot(float2(0), ouv);

	float l = f.x;
	float2 z = f.yz;
	if (dot(z, z) < 65536.0) l = 0.0;

	float4 outColor;
	if (nuv.x + nuv.y > 1.0)
		outColor = float4(float3(sqrt(l / 100.0)) * float3(0.4, 1.0, 1.3) * 2.0, 1.0);
	else
		outColor = float4(float3(l / 100.0) * float3(0.4, 1.0, 1.3) * 4.0, 1.0);

	return outColor * in.color;
}
