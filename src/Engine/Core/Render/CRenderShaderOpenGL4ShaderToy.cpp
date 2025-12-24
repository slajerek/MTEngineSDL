#include "CRenderShaderOpenGL4ShaderToy.h"
#include "VID_Main.h"

CRenderShaderOpenGL4ShaderToy::CRenderShaderOpenGL4ShaderToy(CRenderBackendOpenGL4 *renderBackend, const char *shaderName, float screenWidth, float screenHeight)
: CRenderShaderOpenGL4(renderBackend, shaderName)
{
	this->screenWidth = screenWidth;
	this->screenHeight = screenHeight;
	
	startTime = SYS_GetCurrentTimeInMillis();
}

const char *CRenderShaderOpenGL4ShaderToy::GetVertexShaderSource()
{
	return R"(
 
	layout (location = 0) in vec2 Position;
	layout (location = 1) in vec2 UV;
	layout (location = 2) in vec4 Color;
	uniform mat4 ProjMtx;
	out vec2 Frag_UV;
	out vec4 Frag_Color;
	void main()
	{
		Frag_UV = UV;
		Frag_Color = Color;
		gl_Position = ProjMtx * vec4(Position.xy,0,1);
	}

	)";
}

const char *CRenderShaderOpenGL4ShaderToy::GetFragmentShaderSource()
{
	return R"(
 
	in vec2 Frag_UV;
	in vec4 Frag_Color;

	uniform sampler2D iChannel0;
	uniform vec2 iResolution;
	uniform float iTime;
	uniform vec2 iWindowPos;
 
	layout (location = 0) out vec4 Out_Color;

	void mainImage(out vec4 fragColor, in vec2 fragCoord);
	void mainImageTexture( out vec4 fragColor, in vec2 fragCoord );
	void mainImageMandelbrot(out vec4 fragColor, in vec2 fragCoord);
	void mainImageTest( out vec4 fragColor, in vec2 fragCoord );

 
	void main() {
		
//		vec2 uv = Frag_UV.st * iResolution.xy;
 
 // note, shader toy if Y-flipped
		vec2 uv = vec2(Frag_UV.s * iResolution.x, (1.0 - Frag_UV.t) * iResolution.y);
 
//		mainImageTexture(Out_Color, uv);
		mainImageMandelbrot(Out_Color, uv);
//		mainImageTest(Out_Color, uv);
//		mainImage(Out_Color, uv);
		Out_Color *= Frag_Color;

	}
 

  void mainImageTexture( out vec4 fragColor, in vec2 fragCoord )
  {
	 // Normalized pixel coordinates (from 0 to 1)
	 vec2 uv = fragCoord/iResolution.xy;

	 // sample texture and output to screen
	 fragColor = texture(iChannel0, uv);
 
//	fragColor = texture(iChannel0, fragCoord);

  }
 
 
 vec3 mandelbrot(vec2 z, vec2 c) {
	 float l = 0.0;
	 for (l = 0.0; l < 100.0; l += 1.0) {
		 z = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
		 if(dot(z, z) > 65536.0) break;
	 }
	 l = l - log2(log2(dot(z,z))) + 4.0;
	 return vec3(l, z);
 }

 void mainImageMandelbrot(out vec4 fragColor, in vec2 fragCoord) {
	 vec2 uv = ((2.0 * fragCoord - iResolution.xy) / iResolution.y) * 1.2;
	 vec2 ouv = uv - vec2(0.5, 0);
	 vec2 nuv = fragCoord / iResolution.xy;
	 uv.x -= 0.5;
	 float res = sin(iTime / 2.0) * 8.0 + 12.0;
	 uv.y += 1.0 / res / 2.0;
	 vec2 puv = floor(uv * res) / res;
	 float ref = pow(clamp(-sqrt(mandelbrot(vec2(0.0), puv).x / 100.0) + 0.3, 0.0, 8.0) * 16.0, 4.0);
	 float scale = 2.6 + ref;
	 vec2 muv = mod(uv, 1.0 / res) * res * scale - (scale / 2.0);
	 
	 vec3 f = vec3(0);
	 if (nuv.x + nuv.y > 1.0)
		 f = mandelbrot(muv, puv);
	 else
		 f = mandelbrot(vec2(0), ouv);
		 
	 float l = f.x;
	 vec2 z = f.yz;
	 if(dot(z, z) < 65536.0) l = 0.0;
	 
	 if (nuv.x + nuv.y > 1.0)
		 fragColor = vec4(vec3(sqrt(l / 100.0)) * vec3(0.4, 1.0, 1.3) * 2.0, 1.0);
	 else
		 fragColor = vec4(vec3(l / 100.0) * vec3(0.4, 1.0, 1.3) * 4.0, 1.0);
 }
 
 /*
 void mainImage( out vec4 fragColor, in vec2 fragCoord )
 {
	 vec2 p = (fragCoord.xy * 2.0 - iResolution.xy) / iResolution.y;
	 vec3 col = vec3(0.0);
	 float t = iTime;
	 vec4 o = vec4(0.0);

	 for (float i = -1.0, N = 400.0; i < 1.0; i += 2.0 / N) {
		 vec3 v = vec3(cos(N * i * 2.4 + sin(i * N + t) + t + vec2(0,11)) * sqrt(1.0 - i * i), i);
		 o += (sin(i * 4.0 + vec4(6,1,2,3)) + 1.0) * (v.y + 1.0) / N / length(p - v.xz / (1.6 - v.y));
	 }

	 o = tanh(0.2 * o * o);

	 // Webcam feed from iChannel0 (should be set automatically)
	 vec2 uv = fragCoord.xy / iResolution.xy;
	 vec4 webcam = texture(iChannel0, uv);

	 // Circular mask for webcam in center of globe
	 float mask = smoothstep(0.5, 0.48, length(p));

	 // Neon effect colors
	 vec3 neon = mix(vec3(0.0, 1.0, 0.4), vec3(0.2, 0.8, 1.0), o.x);
	 neon = mix(neon, vec3(0.0, 1.2, 0.6), o.y);
	 neon *= o.z * 1.5;

	 // Overlay neon fractal slightly over webcam
	 float overlay = smoothstep(0.7, 0.5, length(p)); // Makes neon partially overlay
	 vec3 finalColor = mix(webcam.rgb, neon, overlay * 0.6); // Adjust overlay strength

	 fragColor = vec4(finalColor, 1.0);
 }*/

 void mainImageTest( out vec4 fragColor, in vec2 fragCoord )
 {
	 fragColor = vec4(1.0, 0.0, 0.0, 1.0);
	 vec2 uv = fragCoord / iResolution.xy;
 
	if (uv.x < 0.5)
	{
		fragColor = vec4(0.0, 1.0, 0.0, 1.0);
	}
 }
	
//	void mainImage(out vec4 fragColor, in vec2 fragCoord)
//	{
//		vec2 uv = Frag_UV;
//
//		vec4 color = texture(iChannel0, uv.st);
//
//		float scanline = sin(uv.y * iResolution.y * 3.1415 * 3.5) * 0.1;
//
//		color.r = color.r - scanline;
//		color.g = color.g - scanline;
//		color.b = color.b - scanline;
//
//		float grille = step(0.66, mod(uv.x * iResolution.x, 1.2));
//		color *= vec4(1.0 - grille * 0.3, 1.0 - grille * 0.1, 1.0, 1.0);
//
//		float chromaDistort = 0.0015;
//		vec4 chromaColor;
//		chromaColor.r = texture(iChannel0, uv + vec2(chromaDistort, 0.0)).r;
//		chromaColor.g = texture(iChannel0, uv).g;
//		chromaColor.b = texture(iChannel0, uv - vec2(chromaDistort, 0.0)).b;
//		chromaColor.a = 1.0;
//
//		color = mix(color, chromaColor, 0.5);
// 
////		color.r *= iTime;
//
//		fragColor = Frag_Color * color;
//	}

 
 
 
 
 
	)";
}

void CRenderShaderOpenGL4ShaderToy::GetUniformsLocations()
{
	LOGD("CRenderShaderOpenGL4ShaderToy::GetUniformsLocations");
	
//	attribLocationResolution = GetUniformLocation("iResolution");
	attribLocationTime = GetUniformLocation("iTime");
	attribLocationWindowPos = GetUniformLocation("iWindowPos");
}

void CRenderShaderOpenGL4ShaderToy::SetShaderVars()
{
//	DebugPrintUniforms();

//	LOGD("window=%f %f  s=%f %f", windowPos.x, windowPos.y, windowSize.x, windowSize.y);
//	
//	static float f = 0.25f;
//	f += 0.1f;
//	
	if (attribLocationResolution != -1)
	{
		float w = windowSize.x;
		float h = windowSize.y;
		LOGD("w=%f h=%f", w, h);
		glUniform2f(GetUniformLocation("iResolution"), w, h);
		ASSERT_OPENGL();
	}
		
//	LOGD("screen=%f %f", screenWidth, screenHeight);
		
	if (attribLocationTime != -1)
	{
		u64 t = SYS_GetCurrentTimeInMillis() - startTime;
		float elapsedSeconds = (float)t / 1000.0f;
		
		glUniform1f(attribLocationTime, elapsedSeconds);
		ASSERT_OPENGL();
	}
	
	if (attribLocationWindowPos != -1)
	{
		glUniform2f(attribLocationWindowPos, windowPos.x, windowPos.y);
	}
}

CRenderShaderOpenGL4ShaderToy::~CRenderShaderOpenGL4ShaderToy()
{
}
	
