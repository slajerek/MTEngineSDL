#include "CRenderShaderOpenGL4.h"
#include "DBG_Log.h"
#include "VID_Main.h"
#include "CGuiView.h"
#include "SYS_Platform.h"

CRenderShaderOpenGL4::CRenderShaderOpenGL4(CRenderBackendOpenGL4 *renderBackend, const char *shaderName)
: name(shaderName)
{
	LOGD("CRenderShaderOpenGL4: create shader %s", shaderName);
	this->renderBackend = renderBackend;
	this->isCompiled = false;
	this->compileAttemptedAndFailed = false;
}

const char *CRenderShaderOpenGL4::GetVertexShaderSource()
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

const char *CRenderShaderOpenGL4::GetFragmentShaderSource()
{
	return R"(
		in vec2 Frag_UV;
		in vec4 Frag_Color;
		uniform sampler2D iChannel0;
		layout (location = 0) out vec4 Out_Color;
		void main()
		{
			vec4 color = texture(iChannel0, Frag_UV.st);
			Out_Color = Frag_Color * color;
		}
	)";
}
	
void CRenderShaderOpenGL4::CompileShaders()
{
	LOGD("CRenderShaderOpenGL4::CompileShaders");

	if (isCompiled || compileAttemptedAndFailed)
		return;

	lastCompileLog.clear();

	// Create shaders
	const char *glslVersionString = renderBackend->GetGlSlVersion();
	
	char *buf = SYS_GetCharBuf();
	sprintf(buf, "%s\n", glslVersionString);
	
	const char *vertexShaderSource = GetVertexShaderSource();
	const GLchar* vertex_shader_with_version[2] = { buf, vertexShaderSource };
	GLuint vert_handle = glCreateShader(GL_VERTEX_SHADER);
	ASSERT_OPENGL();
	
	glShaderSource(vert_handle, 2, vertex_shader_with_version, nullptr);
	ASSERT_OPENGL();

	glCompileShader(vert_handle);
	ASSERT_OPENGL();

	bool vertexOk = CheckShader(vert_handle, "vertex shader");

	const char *fragmentShaderSource = GetFragmentShaderSource();
	LOGD("fragmentShaderSource=%s", fragmentShaderSource);
	const GLchar* fragment_shader_with_version[2] = { buf, fragmentShaderSource };
	GLuint frag_handle = glCreateShader(GL_FRAGMENT_SHADER);
	ASSERT_OPENGL();

	glShaderSource(frag_handle, 2, fragment_shader_with_version, nullptr);
	ASSERT_OPENGL();

	glCompileShader(frag_handle);
	ASSERT_OPENGL();

	bool fragmentOk = CheckShader(frag_handle, "fragment shader");

	SYS_ReleaseCharBuf(buf);
	
	// Link
	shaderHandle = glCreateProgram();
	ASSERT_OPENGL();
	glAttachShader(shaderHandle, vert_handle);
	ASSERT_OPENGL();
	glAttachShader(shaderHandle, frag_handle);
	ASSERT_OPENGL();
	glLinkProgram(shaderHandle);
	ASSERT_OPENGL();
	bool programOk = CheckProgram(shaderHandle, "shader program");

	// HONOUR the three results. They used to be discarded and isCompiled was
	// set unconditionally at the end of this function, so a shader that failed
	// to compile or link still reported itself compiled -- which made every
	// compile-time assertion in the engine false-green and hid exactly the
	// failure mode a shader port produces.
	if (!vertexOk || !fragmentOk || !programOk)
	{
		LOGError("CRenderShaderOpenGL4('%s'): shader unusable, drawing will be skipped", name);
		glDetachShader(shaderHandle, vert_handle);
		glDetachShader(shaderHandle, frag_handle);
		glDeleteShader(vert_handle);
		glDeleteShader(frag_handle);
		glDeleteProgram(shaderHandle);
		shaderHandle = 0;
		isCompiled = false;
		// LATCH. Gating isCompiled alone would turn one silent failure into a
		// per-frame leak: UseShaderProgram() re-enters CompileShaders() whenever
		// isCompiled is false, and each pass would allocate a fresh program and
		// two shaders without releasing the last -- an unbounded GL object leak
		// plus a compile stall every frame, worse than the bug being fixed.
		compileAttemptedAndFailed = true;
		return;
	}

	glDetachShader(shaderHandle, vert_handle);
	ASSERT_OPENGL();
	glDetachShader(shaderHandle, frag_handle);
	ASSERT_OPENGL();
	glDeleteShader(vert_handle);
	ASSERT_OPENGL();
	glDeleteShader(frag_handle);
	ASSERT_OPENGL();

	attribLocationTex = GetUniformLocation("iChannel0");
	attribLocationProjMtx = GetUniformLocation("ProjMtx");
	
	attribLocationVtxPos = (GLuint)glGetAttribLocation(shaderHandle, "Position");
	ASSERT_OPENGL();
	attribLocationVtxUV = (GLuint)glGetAttribLocation(shaderHandle, "UV");
	ASSERT_OPENGL();
	attribLocationVtxColor = (GLuint)glGetAttribLocation(shaderHandle, "Color");
	ASSERT_OPENGL();

	GetUniformsLocations();
	ASSERT_OPENGL();

	isCompiled = true;
}

void CRenderShaderOpenGL4::GetUniformsLocations()
{
}

GLint CRenderShaderOpenGL4::GetUniformLocation(const char *uniformName)
{
	GLint uniformLocation = glGetUniformLocation(shaderHandle, uniformName);
	ASSERT_OPENGL();
	if (uniformLocation == -1)
	{
		LOGError("CRenderShaderOpenGL4::GetUniformLocation: uniform '%s' not found", uniformName);
	}
	return uniformLocation;
}

void CRenderShaderOpenGL4::UseShaderProgram()
{
	if (!isCompiled)
	{
		CompileShaders();
		// Still not compiled: either it just failed, or it failed earlier and
		// the latch stopped it retrying. Either way there is no program to
		// bind, and queuing a draw callback that binds program 0 would render
		// garbage rather than nothing.
		if (!isCompiled)
			return;
	}
	
//	ImVec2 p0 = ImGui::GetItemRectMin();
//	ImVec2 p1 = ImGui::GetItemRectMax();
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	windowPos = ImGui::GetWindowPos();
	windowSize = ImGui::GetWindowSize();
	
	float dpiScale = 1.0f; // Default
	#ifdef __APPLE__
		// Get DPI scale for the current display (works after window moves between screens)
		// SDL3: this used to be MACOS_GetBackingScaleFactor(SDL_GetWindowDisplayIndex(w)),
		// and the rename scripts turned the inner call into SDL_GetDisplayForWindow --
		// which compiles and is WRONG. MACOS_GetBackingScaleFactor indexes
		// [NSScreen screens]; SDL3 display IDs are OPAQUE HANDLES, not indices
		// (they typically start at 1), so on a single-screen Mac this asked for
		// screen 1 of 1, fell off the end, and silently returned 1.0f -- i.e. no
		// Retina scaling, exactly where the shader needs it.
		//
		// SDL_GetWindowPixelDensity is the direct equivalent of NSScreen's
		// backingScaleFactor ("a ratio of pixel size to window size"), it follows
		// the window across screens by itself, and it needs no platform helper.
		//
		// The __APPLE__ guard STAYS. This API works everywhere, but non-Apple
		// platforms have always taken the 1.0f default here, and switching them to
		// a real density is a behaviour change with its own testing -- not
		// something to slip into an SDL port.
		dpiScale = SDL_GetWindowPixelDensity(VID_GetMainSDLWindow());
	#endif

//	LOGD("dpiScale=%f", dpiScale);
	
//	windowSize.x *= dpiScale;
//	windowSize.y *= dpiScale;
	
//	draw_list->PushClipRect(p0, p1);
	draw_list->AddCallback([](const ImDrawList *draw_list, const ImDrawCmd *cmd)
	{
		CRenderShaderOpenGL4 *renderShader = (CRenderShaderOpenGL4*)cmd->UserCallbackData;
		
		ImDrawData* draw_data = ImGui::GetDrawData();

		float L = draw_data->DisplayPos.x;
		float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
		float T = draw_data->DisplayPos.y;
		float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;

		const float ortho_projection[4][4] =
		{
			{ 2.0f/(R-L),   0.0f,         0.0f,   0.0f },
			{ 0.0f,         2.0f/(T-B),   0.0f,   0.0f },
			{ 0.0f,         0.0f,        -1.0f,   0.0f },
			{ (R+L)/(L-R),  (T+B)/(B-T),  0.0f,   1.0f },
		};

		glUseProgram(renderShader->shaderHandle);
		glUniform1i(renderShader->attribLocationTex, 0);
		glUniformMatrix4fv(renderShader->attribLocationProjMtx, 1, GL_FALSE, &ortho_projection[0][0]);

		renderShader->SetShaderVars();
		
	}, (void*)this);
}

void CRenderShaderOpenGL4::SetShaderVars()
{
}

void CRenderShaderOpenGL4::ResetState()
{
	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	//	draw_list->PopClipRect();
	draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// ImGui implementation
bool CRenderShaderOpenGL4::CheckShader(GLuint handle, const char* desc)
{
	GLint status = 0, log_length = 0;
	glGetShaderiv(handle, GL_COMPILE_STATUS, &status);
	ASSERT_OPENGL();
	glGetShaderiv(handle, GL_INFO_LOG_LENGTH, &log_length);
	ASSERT_OPENGL();
	if ((GLboolean)status == GL_FALSE)
	{
		// The %s here carried NO ARGUMENT until 2026-09-04 -- undefined
		// behaviour that printed whatever the stack held -- while desc, passed
		// in for exactly this, went unused.
		LOGError("CRenderShaderOpenGL4::CheckShader: failed to compile %s", desc);
	}
	
	if (log_length > 1)
	{
		ImVector<char> buf;
		buf.resize((int)(log_length + 1));
		glGetShaderInfoLog(handle, log_length, nullptr, (GLchar*)buf.begin());
		ASSERT_OPENGL();
		LOGError("%s", buf.begin());
		lastCompileLog += desc;
		lastCompileLog += ": ";
		lastCompileLog += buf.begin();
		lastCompileLog += "\n";
	}
	return (GLboolean)status == GL_TRUE;
}

bool CRenderShaderOpenGL4::CheckProgram(GLuint handle, const char* desc)
{
	GLint status = 0, log_length = 0;
	glGetProgramiv(handle, GL_LINK_STATUS, &status);
	ASSERT_OPENGL();
	glGetProgramiv(handle, GL_INFO_LOG_LENGTH, &log_length);
	ASSERT_OPENGL();
	if ((GLboolean)status == GL_FALSE)
	{
		LOGError("CRenderShaderOpenGL4::CheckProgram: failed to link %s", desc);
	}
	
	if (log_length > 1)
	{
		ImVector<char> buf;
		buf.resize((int)(log_length + 1));
		glGetProgramInfoLog(handle, log_length, nullptr, (GLchar*)buf.begin());
		ASSERT_OPENGL();
		LOGError("%s", buf.begin());
		lastCompileLog += desc;
		lastCompileLog += ": ";
		lastCompileLog += buf.begin();
		lastCompileLog += "\n";
	}
	return (GLboolean)status == GL_TRUE;
}

void CRenderShaderOpenGL4::DebugPrintUniforms()
{
	GLint count;
	glGetProgramiv(shaderHandle, GL_ACTIVE_UNIFORMS, &count);
	ASSERT_OPENGL();
	printf("Active uniforms: %d\n", count);

	for (int i = 0; i < count; i++) {
		char name[256];
		GLsizei length;
		GLint size;
		GLenum type;
		glGetActiveUniform(shaderHandle, i, sizeof(name), &length, &size, &type, name);
		ASSERT_OPENGL();
		LOGD("Uniform #%d Type: %u Name: %s", i, type, name);
	}
}

CRenderShaderOpenGL4::~CRenderShaderOpenGL4()
{
}

