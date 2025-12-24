#include "CRenderShader.h"

CRenderShader::CRenderShader()
{
	isCompiled = false;
	screenWidth = 1.0f;
	screenHeight = 1.0f;
}

CRenderShader::~CRenderShader()
{
}
	
const char *CRenderShader::GetVertexShaderSource()
{
	return NULL;
}

const char *CRenderShader::GetFragmentShaderSource()
{
	return NULL;
}
	
void CRenderShader::CompileShaders()
{
}

void CRenderShader::UseShaderProgram()
{	
}

void CRenderShader::ResetState()
{
}

void CRenderShader::SetResolution(float screenWidth, float screenHeight)
{
	this->screenWidth = screenWidth;
	this->screenHeight = screenHeight;
}
