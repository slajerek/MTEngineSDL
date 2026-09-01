#include "CVideoYUVShader.h"
#include "DBG_Log.h"

// ---------------------------------------------------------------------------
// Embedded GLSL shader sources
// ---------------------------------------------------------------------------

static const char *kVertexShaderSource = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

uniform vec4 uTransform; // x, y, w, h in NDC space
uniform int uRotation;   // 0/90/180/270 -- display rotation, applied as a UV
                         // transform (Task 10's RenderToTarget path only;
                         // Render() always passes 0). Reproduces the exact
                         // same orientation as VideoFrameTransform::RotateRGBA
                         // (CPU path, Task 9) in continuous UV space instead
                         // of a discrete pixel remap -- see that file for the
                         // sign-convention rationale (rot90 is a
                         // *counter*-clockwise turn of the coded pixels).
                         // Derivation: RotateRGBA's rot90 pixel map is
                         // sx=W-1-oy, sy=ox; normalizing to UV (u=sx/W,
                         // v=sy/H, against output uv (aTexCoord)) gives
                         // u_src=1-v_out, v_src=u_out -- and symmetrically
                         // for 270/180 below.

out vec2 vTexCoord;

void main() {
    vec2 pos = aPos * uTransform.zw + uTransform.xy;
    gl_Position = vec4(pos, 0.0, 1.0);

    vec2 uv = aTexCoord;
    if (uRotation == 90) {
        vTexCoord = vec2(1.0 - uv.y, uv.x);
    } else if (uRotation == 180) {
        vTexCoord = vec2(1.0 - uv.x, 1.0 - uv.y);
    } else if (uRotation == 270) {
        vTexCoord = vec2(uv.y, 1.0 - uv.x);
    } else {
        vTexCoord = uv;
    }
}
)";

static const char *kFragmentShaderSource = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D texY;
uniform sampler2D texU;
uniform sampler2D texV;
uniform sampler2D texA;
uniform bool hasAlpha;
uniform float alpha;
uniform int colorSpace;  // NORMALIZED VPX_CS_* value: 1 = BT.601, 2 = BT.709,
                         // 5 = BT.2020 ncl (anything else falls to 601).
uniform bool fullRange;
uniform sampler3D texLut; // CM-E: display colour LUT (unit 4), sampled after
                          // the matrix on encoded R'G'B'. See useLut.
uniform bool useLut;

// --- S-5 Phase 5: HDR transfer ---------------------------------------------
uniform int   uColorTrc;        // 16 = PQ, 18 = HLG, anything else = SDR
uniform bool  uFloatTarget;     // true: keep above-white. false: tone-map to 0..1
uniform bool  uSurfaceLinear;
uniform bool  uSurfaceP3;
uniform float uToneMapHeadroom;

// GENERATED FROM CVideoTransferFunctions.h -- every constant is copied from
// that header and MUST be updated with it. A shader cannot include a C++
// header, which is exactly why the ImGui test `hdr_shader_agrees_with_transfer_header` runs a ramp through THIS
// compiled shader on the GPU and compares against that header on the CPU. These must also stay identical to the MSL copy in
// CVideoYUVShaderMetal.mm, or the two backends draw different pictures.

float PqEotf(float e) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float p = pow(max(e, 0.0), 1.0 / m2);
    float num = p - c1;
    float den = c2 - c3 * p;
    if (num <= 0.0 || den <= 0.0) return 0.0;
    return pow(num / den, 1.0 / m1);
}

float HlgInverseOetf(float e) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    e = max(e, 0.0);
    return (e <= 0.5) ? (e * e) / 3.0 : (exp((e - c) / a) + b) / 12.0;
}

// IEC sRGB, sign-symmetric and CONTINUED past 1.0 rather than clamped.
float SrgbExtendedEncode(float v) {
    float a = abs(v);
    float e = (a <= 0.0031308) ? (a * 12.92) : (1.055 * pow(a, 1.0 / 2.4) - 0.055);
    return (v < 0.0) ? -e : e;
}

// Extended Reinhard; at headroom 1.0 it is EXACTLY the identity on 0..1.
float ToneMapReinhard(float v, float headroom) {
    float h = max(headroom, 1.0);
    v = max(v, 0.0);
    float t = v * (1.0 + v / (h * h)) / (1.0 + v);
    return min(t, 1.0);
}
uniform float lutScale;   // (N-1)/N -- half-texel correction so lattice
uniform float lutOffset;  // 1/(2N)     endpoints land on texel centres.
uniform int uMode; // EYUVShaderMode ordinal: 0 = YUV420_3Plane, 1 = NV12,
                    // 2 = YUV420P10. Render() always passes 0 (its only
                    // supported layout); RenderToTarget() passes the mode
                    // the caller asked for.

void main() {
    float y, u, v;

    if (uMode == 2) {
        // YUV420P10: texY/texU/texV are GL_R16 planar textures holding the
        // decoder's raw 10-bit sample (0..1023) LSB-aligned in the 16-bit
        // texel -- CONFIRMED against CVideoPlayer::ConvertYUV420ToRGBA's CPU
        // fallback, which does `u16Sample >> 2` to downconvert to 8-bit (i.e.
        // the stored value is the unshifted 0..1023 code word, NOT
        // pre-shifted into the top 10 bits of the 16-bit word). Sampling a
        // GL_R16 texture normalizes by /65535.0, so scale back up by
        // 65535/1023 to recover the correct 10-bit-normalized [0,1] sample
        // (equivalent to the ~64x factor a pre-shifted/MSB-aligned encoding
        // would need, but exact for this LSB-aligned layout).
        const float k10 = 65535.0 / 1023.0;
        y = texture(texY, vTexCoord).r * k10;
        u = texture(texU, vTexCoord).r * k10;
        v = texture(texV, vTexCoord).r * k10;
    } else if (uMode == 1) {
        // NV12: texU is the interleaved U,V plane, uploaded as GL_RG8 (R=U, G=V).
        y = texture(texY, vTexCoord).r;
        vec2 uv2 = texture(texU, vTexCoord).rg;
        u = uv2.r;
        v = uv2.g;
    } else {
        // YUV420_3Plane (default; the only layout Render() ever used).
        y = texture(texY, vTexCoord).r;
        u = texture(texU, vTexCoord).r;
        v = texture(texV, vTexCoord).r;
    }

    float yNorm, uNorm, vNorm;
    if (fullRange) {
        yNorm = y;
        uNorm = u - 0.5;
        vNorm = v - 0.5;
    } else {
        yNorm = (y - 16.0/255.0) * (255.0/219.0);
        uNorm = (u - 128.0/255.0) * (255.0/224.0);
        vNorm = (v - 128.0/255.0) * (255.0/224.0);
    }

    vec3 rgb;
    if (colorSpace == 5) {
        // BT.2020 non-constant luminance (VPX_CS_BT_2020 = 5)
        rgb.r = yNorm + 1.4746 * vNorm;
        rgb.g = yNorm - 0.16455 * uNorm - 0.57135 * vNorm;
        rgb.b = yNorm + 1.8814 * uNorm;
    } else if (colorSpace == 2) {
        // BT.709 (VPX_CS_BT_709 = 2)
        rgb.r = yNorm + 1.5748 * vNorm;
        rgb.g = yNorm - 0.1873 * uNorm - 0.4681 * vNorm;
        rgb.b = yNorm + 1.8556 * uNorm;
    } else {
        // BT.601
        rgb.r = yNorm + 1.402 * vNorm;
        rgb.g = yNorm - 0.344136 * uNorm - 0.714136 * vNorm;
        rgb.b = yNorm + 1.772 * uNorm;
    }
    // --- S-5 Phase 5: the HDR transfer ---------------------------------
    // Skipped entirely for SDR clips, which keeps every existing clip
    // byte-identical. Must stay in lockstep with the MSL copy.
    if (uColorTrc == 16 || uColorTrc == 18) {
        // 1. EOTF -> LINEAR, 1.0 == SDR reference white (203 nit, BT.2408).
        vec3 lin;
        if (uColorTrc == 16) {
            const float kPqScale = 10000.0 / 203.0;
            lin = vec3(PqEotf(rgb.r), PqEotf(rgb.g), PqEotf(rgb.b)) * kPqScale;
        } else {
            const float kHlgScale = 1000.0 / 203.0;
            lin = vec3(HlgInverseOetf(rgb.r), HlgInverseOetf(rgb.g), HlgInverseOetf(rgb.b));
            // HLG's display OOTF, driven by BT.2020 luma. PQ has none, which
            // is why one transfer can be right while the other is wrong.
            float ys = 0.2627 * lin.r + 0.6780 * lin.g + 0.0593 * lin.b;
            float gain = (ys > 0.0) ? pow(ys, 0.2) : 0.0;
            lin = lin * gain * kHlgScale;
        }

        // 2. BT.2020 -> sRGB primaries (rows sum to 1.0: neutral stays neutral)
        vec3 srgbLin;
        srgbLin.r =  1.6605 * lin.r - 0.5876 * lin.g - 0.0728 * lin.b;
        srgbLin.g = -0.1246 * lin.r + 1.1329 * lin.g - 0.0083 * lin.b;
        srgbLin.b = -0.0182 * lin.r - 0.1006 * lin.g + 1.1187 * lin.b;

        // 3. PRIMARIES, for BOTH arms -- see the MSL copy for the full
        //    reasoning. The poster applies this stage whatever its resident
        //    format is, and its 8-bit conversion undoes only the TRANSFER, so
        //    applying P3 on the float arm alone would leave playback in sRGB
        //    primaries while the poster sat in P3 on the gate-closed path.
        //    Primaries first, then the tone-map, matching the poster's order.
        //    Full precision, from PC_kLinearSrgbToLinearP3.
        if (uSurfaceP3) {
            vec3 p3;
            p3.r = 0.8224621 * srgbLin.r + 0.1775380 * srgbLin.g + 0.0000000 * srgbLin.b;
            p3.g = 0.0331941 * srgbLin.r + 0.9668058 * srgbLin.g + 0.0000000 * srgbLin.b;
            p3.b = 0.0170827 * srgbLin.r + 0.0723974 * srgbLin.g + 0.9105199 * srgbLin.b;
            srgbLin = p3;
        }

        if (uFloatTarget) {
            // 4a. Gate open: finish in the surface's space, keep above-white.
            if (uSurfaceLinear) {
                rgb = srgbLin;
            } else {
                rgb = vec3(SrgbExtendedEncode(srgbLin.r),
                           SrgbExtendedEncode(srgbLin.g),
                           SrgbExtendedEncode(srgbLin.b));
            }
        } else {
            // 4b. Gate closed -- the arm every user without headroom sees,
            //     which is most users most of the time. Tone-map in LINEAR.
            rgb = vec3(SrgbExtendedEncode(ToneMapReinhard(srgbLin.r, uToneMapHeadroom)),
                       SrgbExtendedEncode(ToneMapReinhard(srgbLin.g, uToneMapHeadroom)),
                       SrgbExtendedEncode(ToneMapReinhard(srgbLin.b, uToneMapHeadroom)));
        }
    } else {
        // THE SDR PATH, UNCHANGED. The clamp stays here and only here.
        rgb = clamp(rgb, 0.0, 1.0);
    }

    // CM-E: source -> display colour transform, baked by the host app's CMM
    // into a 3D LUT over encoded R'G'B'. No LUT bound => bit-identical to the
    // pre-CM-E path. For HDR clips useLut is already 0 (CM-E installs no LUT
    // for trc 16/18) and is left that way deliberately.
    if (useLut) {
        rgb = texture(texLut, rgb * lutScale + lutOffset).rgb;
    }

    float a = alpha;
    if (hasAlpha) {
        a *= texture(texA, vTexCoord).r;
    }
    FragColor = vec4(rgb, a);
}
)";

// ---------------------------------------------------------------------------
// CVideoYUVShader
// ---------------------------------------------------------------------------

CVideoYUVShader::CVideoYUVShader()
{
}

CVideoYUVShader::~CVideoYUVShader()
{
	if (program != 0)
	{
		glDeleteProgram(program);
		program = 0;
	}
	if (vao != 0)
	{
		glDeleteVertexArrays(1, &vao);
		vao = 0;
	}
	if (vbo != 0)
	{
		glDeleteBuffers(1, &vbo);
		vbo = 0;
	}
}

GLuint CVideoYUVShader::CompileShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);

	GLint status = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if ((GLboolean)status == GL_FALSE)
	{
		GLint logLength = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
		if (logLength > 1)
		{
			char *log = new char[logLength + 1];
			glGetShaderInfoLog(shader, logLength, nullptr, log);
			LOGError("CVideoYUVShader::CompileShader: %s shader compile failed:\n%s",
					 (type == GL_VERTEX_SHADER ? "vertex" : "fragment"), log);
			delete[] log;
		}
		else
		{
			LOGError("CVideoYUVShader::CompileShader: %s shader compile failed (no log)",
					 (type == GL_VERTEX_SHADER ? "vertex" : "fragment"));
		}
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

void CVideoYUVShader::CreateQuadVAO()
{
	// Quad vertices: position (0..1) and texcoord (V flipped for video orientation)
	// pos.x, pos.y, tex.x, tex.y
	float vertices[] = {
		0.0f, 0.0f,  0.0f, 1.0f,   // bottom-left  (texcoord Y flipped)
		1.0f, 0.0f,  1.0f, 1.0f,   // bottom-right
		0.0f, 1.0f,  0.0f, 0.0f,   // top-left
		1.0f, 1.0f,  1.0f, 0.0f,   // top-right
	};

	glGenVertexArrays(1, &vao);
	glGenBuffers(1, &vbo);

	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	// location 0: aPos (vec2)
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);

	// location 1: aTexCoord (vec2)
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
}

bool CVideoYUVShader::Compile()
{
	if (compiled)
		return true;

	LOGD("CVideoYUVShader::Compile");

	// Compile shaders
	GLuint vertShader = CompileShader(GL_VERTEX_SHADER, kVertexShaderSource);
	if (vertShader == 0)
		return false;

	GLuint fragShader = CompileShader(GL_FRAGMENT_SHADER, kFragmentShaderSource);
	if (fragShader == 0)
	{
		glDeleteShader(vertShader);
		return false;
	}

	// Link program
	program = glCreateProgram();
	glAttachShader(program, vertShader);
	glAttachShader(program, fragShader);
	glLinkProgram(program);

	GLint linkStatus = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
	if ((GLboolean)linkStatus == GL_FALSE)
	{
		GLint logLength = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
		if (logLength > 1)
		{
			char *log = new char[logLength + 1];
			glGetProgramInfoLog(program, logLength, nullptr, log);
			LOGError("CVideoYUVShader::Compile: program link failed:\n%s", log);
			delete[] log;
		}
		else
		{
			LOGError("CVideoYUVShader::Compile: program link failed (no log)");
		}
		glDeleteShader(vertShader);
		glDeleteShader(fragShader);
		glDeleteProgram(program);
		program = 0;
		return false;
	}

	// Shaders are linked into the program; no longer needed individually
	glDetachShader(program, vertShader);
	glDetachShader(program, fragShader);
	glDeleteShader(vertShader);
	glDeleteShader(fragShader);

	// Get uniform locations
	locTransform  = glGetUniformLocation(program, "uTransform");
	locTexY       = glGetUniformLocation(program, "texY");
	locTexU       = glGetUniformLocation(program, "texU");
	locTexV       = glGetUniformLocation(program, "texV");
	locTexA       = glGetUniformLocation(program, "texA");
	locHasAlpha   = glGetUniformLocation(program, "hasAlpha");
	locAlpha      = glGetUniformLocation(program, "alpha");
	locColorSpace = glGetUniformLocation(program, "colorSpace");
	locFullRange  = glGetUniformLocation(program, "fullRange");
	locMode       = glGetUniformLocation(program, "uMode");
	locRotation   = glGetUniformLocation(program, "uRotation");
	locTexLut     = glGetUniformLocation(program, "texLut");
	locUseLut     = glGetUniformLocation(program, "useLut");
	locColorTrc        = glGetUniformLocation(program, "uColorTrc");
	locFloatTarget     = glGetUniformLocation(program, "uFloatTarget");
	locSurfaceLinear   = glGetUniformLocation(program, "uSurfaceLinear");
	locSurfaceP3       = glGetUniformLocation(program, "uSurfaceP3");
	locToneMapHeadroom = glGetUniformLocation(program, "uToneMapHeadroom");
	locLutScale   = glGetUniformLocation(program, "lutScale");
	locLutOffset  = glGetUniformLocation(program, "lutOffset");

	// Pin the sampler3D to unit 4 ONCE, at link time (CM-E review High): a
	// sampler uniform defaults to 0, and leaving texLut on unit 0 aliases the
	// sampler2D texY -- two active samplers of DIFFERENT types on one unit is
	// GL_INVALID_OPERATION at every draw on conforming GL, regardless of the
	// useLut branch. RenderToTarget() re-sets it per draw; the screen-space
	// Render() path (engine demo viewer) relies on this link-time default.
	glUseProgram(program);
	glUniform1i(locTexLut, 4);
	glUseProgram(0);

	if (locTransform == -1)
	{
		LOGError("CVideoYUVShader::Compile: uTransform uniform not found");
	}
	if (locTexY == -1)
	{
		LOGError("CVideoYUVShader::Compile: texY uniform not found");
	}

	// Create quad geometry
	CreateQuadVAO();

	compiled = true;
	LOGD("CVideoYUVShader::Compile: shader compiled and linked successfully");
	return true;
}

// Sets every HDR uniform from the caller's description. One place, so the two
// draw paths cannot disagree about them.
//
// CALLED FROM BOTH PATHS, ALWAYS -- including the screen-space Render(), which
// passes a default-constructed SVideoHdrOutput. GL uniforms are PROGRAM STATE
// that survives between draws, and this program is shared with other
// applications (the game app draws its cutscenes through Render()), so a
// uColorTrc left set by the last RenderToTarget() would apply a PQ EOTF to
// somebody else's SDR video. The existing `glUniform1i(locUseLut, 0)` line in
// Render() exists for exactly this reason; these are the same hazard.
void CVideoYUVShader::SetHdrUniforms(const SVideoHdrOutput &hdr)
{
	if (locColorTrc >= 0)
		glUniform1i(locColorTrc, hdr.IsHdr() ? hdr.colorTrc : 2);
	if (locFloatTarget >= 0)
		glUniform1i(locFloatTarget, hdr.floatTarget ? 1 : 0);
	if (locSurfaceLinear >= 0)
		glUniform1i(locSurfaceLinear, hdr.surfaceIsLinear ? 1 : 0);
	if (locSurfaceP3 >= 0)
		glUniform1i(locSurfaceP3, hdr.surfaceIsP3 ? 1 : 0);
	if (locToneMapHeadroom >= 0)
		glUniform1f(locToneMapHeadroom, (hdr.toneMapHeadroom >= 1.0f) ? hdr.toneMapHeadroom : 1.0f);
}

void CVideoYUVShader::Render(void *texYp, void *texUp, void *texVp, void *texAp,
							 bool hasAlpha, float alpha,
							 int colorSpace, bool fullRange,
							 float x, float y, float w, float h,
							 float screenW, float screenH,
							 const SVideoHdrOutput &hdr)
{
	// The seam carries void*; GL wants GLuint. Cast once, here, at the boundary.
	GLuint texY = (GLuint)(uintptr_t)texYp;
	GLuint texU = (GLuint)(uintptr_t)texUp;
	GLuint texV = (GLuint)(uintptr_t)texVp;
	GLuint texA = (GLuint)(uintptr_t)texAp;
	if (!compiled)
		return;

	glUseProgram(program);

	// Convert pixel coordinates to NDC
	// NDC: x in [-1, 1] left-to-right, y in [-1, 1] bottom-to-top
	// Input: x,y is top-left corner in screen pixels (origin top-left)
	float ndcX = (x / screenW) * 2.0f - 1.0f;
	float ndcY = 1.0f - (y / screenH) * 2.0f;   // Y flipped: screen top = NDC +1
	float ndcW = (w / screenW) * 2.0f;
	float ndcH = (h / screenH) * 2.0f;

	// Adjust Y: input y is the top edge, but our quad starts at bottom-left in NDC
	// ndcY currently points to the top edge; move down by ndcH to get bottom edge
	ndcY -= ndcH;

	glUniform4f(locTransform, ndcX, ndcY, ndcW, ndcH);
	glUniform1i(locMode, (int)EYUVShaderMode::YUV420_3Plane); // Render() only ever handled this layout
	glUniform1i(locRotation, 0); // screen-space Render() never rotates -- callers pre-rotate their layout

	// Bind YUV(A) textures to texture units
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texY);
	glUniform1i(locTexY, 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, texU);
	glUniform1i(locTexU, 1);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, texV);
	glUniform1i(locTexV, 2);

	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, texA);
	glUniform1i(locTexA, 3);

	// Set uniforms
	glUniform1i(locHasAlpha, hasAlpha ? 1 : 0);
	glUniform1f(locAlpha, alpha);
	glUniform1i(locColorSpace, colorSpace);
	glUniform1i(locFullRange, fullRange ? 1 : 0);
	glUniform1i(locUseLut, 0);   // CM-E: the screen-space path never applies a LUT
	SetHdrUniforms(hdr);         // see SetHdrUniforms: uniforms are program state

	// Enable blending if there is alpha
	GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
	if (hasAlpha || alpha < 1.0f)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	// Draw quad
	glBindVertexArray(vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);

	// Restore state
	if (!blendWasEnabled)
	{
		glDisable(GL_BLEND);
	}

	// Unbind the plane textures from their sampler units (see RenderToTarget()
	// for the full rationale): leaving them bound means a later
	// glDeleteTextures() deletes still-bound textures, which arms a latent
	// GL_INVALID_OPERATION on Apple's GL.
	glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
}

// ---------------------------------------------------------------------------
// RenderToTarget (Task 10)
// ---------------------------------------------------------------------------
//
// Design note: this reuses the SAME compiled program/VAO as Render() rather
// than compiling separate per-mode program variants. The existing shader
// already branches on uniforms for colorSpace/fullRange (an int + a bool);
// `uMode`/`uRotation` are just two more uniforms in that same style. This
// keeps exactly one GL program to compile/manage/leak-check, and Render()'s
// screen-space cutscene path is unaffected -- it just always passes
// mode=YUV420_3Plane, rotation=0 (see Render() above), which is bitwise the
// shader's pre-Task-10 behavior.
void CVideoYUVShader::RenderToTarget(EYUVShaderMode mode, void *texYp, void *texUVorUp, void *texVp,
									  bool hasAlpha, void *texAp, int colorSpace, bool fullRange,
									  int rotationDegrees, void *lutTexturep, int lutEdge,
									  CRenderTarget &targetBase,
									  const SVideoHdrOutput &hdr)
{
	// The seam carries void*; GL wants GLuint. Cast once, here, at the boundary.
	// The target is downcast because this is the GL implementation and only ever
	// receives the GL target -- the backend factory that produced both is what
	// keeps them paired.
	GLuint texY = (GLuint)(uintptr_t)texYp;
	GLuint texUVorU = (GLuint)(uintptr_t)texUVorUp;
	GLuint texV = (GLuint)(uintptr_t)texVp;
	GLuint texA = (GLuint)(uintptr_t)texAp;
	GLuint lutTexture = (GLuint)(uintptr_t)lutTexturep;
	CGLRenderTarget &target = static_cast<CGLRenderTarget &>(targetBase);
	if (!compiled)
		return;

	target.BindAsTarget();

	glUseProgram(program);

	// Fullscreen quad: uTransform covers the entire NDC range so the target's
	// color texture receives exactly one full frame (no letterboxing -- unlike
	// Render()'s screen-space x,y,w,h placement, this always fills `target`).
	//
	// NDC height is NEGATIVE on purpose: the shared quad VAO carries
	// V-flipped texcoords sized for Render()'s direct-to-screen path (NDC
	// top = presented top), but an FBO's texture row 0 is NDC *bottom* -- so
	// with a plain (-1,-1,2,2) transform the target texture comes out
	// bottom-up and every CSlrImage consumer (which draws texel row 0 at the
	// top) shows the video upside-down. Inverting the quad's Y here makes
	// the target texture top-down (row 0 = visual top) for ALL uRotation
	// values uniformly, matching VideoFrameTransform::RotateRGBA's CPU
	// output row order (guarded by the photo app's CTestVideoRenderSmoke
	// orientation step against h264_topred.mp4).
	glUniform4f(locTransform, -1.0f, 1.0f, 2.0f, -2.0f);
	glUniform1i(locMode, (int)mode);
	glUniform1i(locRotation, rotationDegrees);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texY);
	glUniform1i(locTexY, 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, texUVorU);
	glUniform1i(locTexU, 1);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, texV);
	glUniform1i(locTexV, 2);

	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, texA);
	glUniform1i(locTexA, 3);

	glUniform1i(locHasAlpha, hasAlpha ? 1 : 0);
	glUniform1f(locAlpha, 1.0f);
	glUniform1i(locColorSpace, colorSpace);
	glUniform1i(locFullRange, fullRange ? 1 : 0);

	// CM-E: display colour LUT on unit 4. 0/0 (or a degenerate edge) means no
	// LUT -- the shader path is then bit-identical to the pre-CM-E behaviour.
	const bool applyLut = (lutTexture != 0 && lutEdge >= 2);
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_3D, applyLut ? lutTexture : 0);
	glUniform1i(locTexLut, 4);
	glUniform1i(locUseLut, applyLut ? 1 : 0);
	SetHdrUniforms(hdr);
	if (applyLut)
	{
		const float edge = (float)lutEdge;
		glUniform1f(locLutScale, (edge - 1.0f) / edge);
		glUniform1f(locLutOffset, 1.0f / (2.0f * edge));
	}

	// Straight overwrite: the fullscreen quad fully covers `target`, so
	// (unlike Render()'s screen-space compositing over whatever's already
	// drawn) blending with the target's previous contents would be wrong --
	// force it off for the duration of this draw.
	GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
	glDisable(GL_BLEND);

	glBindVertexArray(vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);

	if (blendWasEnabled)
	{
		glEnable(GL_BLEND);
	}

	// Unbind the plane textures from their sampler units. Without this the
	// textures stay bound to units 0..3 after we return, so when the player is
	// torn down (CVideoPlayer::FreeResources -> DestroyRGBATextures ->
	// glDeleteTextures) it deletes textures that are STILL bound to active
	// sampler units. On Apple's legacy GL that intermittently arms a latent
	// GL_INVALID_OPERATION which only trips at the next PresentFrameBuffer
	// (navigating away from a playing video -> crash). Leaving unit 0 active
	// with nothing bound matches Render()'s cleanup contract.
	glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_3D, 0);   // CM-E: LUT unit, same rationale
	glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);

	target.Unbind();
}
