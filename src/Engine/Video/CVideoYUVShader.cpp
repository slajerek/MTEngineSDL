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

out vec2 vTexCoord;

void main() {
    vec2 pos = aPos * uTransform.zw + uTransform.xy;
    gl_Position = vec4(pos, 0.0, 1.0);
    vTexCoord = aTexCoord;
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
uniform int colorSpace;  // 0 = BT.601, 1 = BT.709
uniform bool fullRange;

void main() {
    float y = texture(texY, vTexCoord).r;
    float u = texture(texU, vTexCoord).r;
    float v = texture(texV, vTexCoord).r;

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
    if (colorSpace == 2) {
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
    rgb = clamp(rgb, 0.0, 1.0);

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

void CVideoYUVShader::Render(GLuint texY, GLuint texU, GLuint texV, GLuint texA,
							 bool hasAlpha, float alpha,
							 int colorSpace, bool fullRange,
							 float x, float y, float w, float h,
							 float screenW, float screenH)
{
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

	glActiveTexture(GL_TEXTURE0);
	glUseProgram(0);
}
