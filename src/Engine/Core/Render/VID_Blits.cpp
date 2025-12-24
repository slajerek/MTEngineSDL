#include "VID_Blits.h"
#include "GUI_Main.h"
#include "CSlrImage.h"
#include "CContinuousParamSin.h"
#include "CContinuousParamBezier.h"
#include "MTH_FastMath.h"

unsigned int texture[1];

typedef struct {
	float x;
	float y;
	float z;
} Vertex3D;

static float vertices[] = {
	-1.0,  1.0, -3.0,
	1.0,  1.0, -3.0,
	-1.0, -1.0, -3.0,
	1.0, -1.0, -3.0
};


static Vertex3D normals[] = {
	{0.0, 0.0, 1.0},
	{0.0, 0.0, 1.0},
	{0.0, 0.0, 1.0},
	{0.0, 0.0, 1.0}
};

static float texCoords[] = {
	0.0, 1.0,
	1.0, 1.0,
	0.0, 0.0,
	1.0, 0.0
};

static float vertsColors[] = {
	1.0,  1.0, 1.0, 1.0,
	1.0,  1.0, 1.0, 1.0,
	1.0,  1.0, 1.0, 1.0,
	1.0,  1.0, 1.0, 1.0
};

static float colorsOne[] = {
	1.0,  1.0, 1.0, 1.0,
	1.0,  1.0, 1.0, 1.0,
	1.0,  1.0, 1.0, 1.0,
	1.0,  1.0, 1.0, 1.0
};

void Blit(CSlrImage *what, float destX, float destY, float z)
{
	SYS_FatalExit("Blit: not implemented");
//	texCoords[0] = what->defaultTexStartX;
//	texCoords[1] = what->defaultTexStartY;
//	texCoords[2] = what->defaultTexEndX;
//	texCoords[3] = what->defaultTexStartY;
//	texCoords[4] = what->defaultTexStartX;
//	texCoords[5] = what->defaultTexEndY;
//	texCoords[6] = what->defaultTexEndX;
//	texCoords[7] = what->defaultTexEndY;
//
//	vertices[0] = -1.0 + destY ; //+ TOP_OFFSET_Y;
//	vertices[1] = 1.0 + destX ; //+ LEFT_OFFSET_X;
//	vertices[2] = z;
//
//	vertices[3] =  1.0 + destY ; //+ TOP_OFFSET_Y;
//	vertices[4] =  1.0 + destX ; //+ LEFT_OFFSET_X;
//	vertices[5] = z;
//
//	vertices[6] = -1.0 + destY ; //+ TOP_OFFSET_Y;
//	vertices[7] = -1.0 + destX ; //+ LEFT_OFFSET_X;
//	vertices[8] = z;
//
//	vertices[9] =  1.0 + destY ; //+ TOP_OFFSET_Y;
//	vertices[10] = -1.0 + destX ; //+ LEFT_OFFSET_X;
//	vertices[11] = z;
//
//    glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}


void BlitAlpha(CSlrImage *what, float destX, float destY, float z, float alpha)
{
	SYS_FatalExit("Blit: not implemented");
//	texCoords[0] = what->defaultTexStartX;
//	texCoords[1] = what->defaultTexStartY;
//	texCoords[2] = what->defaultTexEndX;
//	texCoords[3] = what->defaultTexStartY;
//	texCoords[4] = what->defaultTexStartX;
//	texCoords[5] = what->defaultTexEndY;
//	texCoords[6] = what->defaultTexEndX;
//	texCoords[7] = what->defaultTexEndY;
//
//	vertices[0] = -1.0 + destY  ;
//	vertices[1] =  1.0 + destX  ;
//	vertices[2] = z;
//
//	vertices[3] =  1.0 + destY  ;
//	vertices[4] =  1.0 + destX  ;
//	vertices[5] = z;
//
//	vertices[6] = -1.0 + destY  ;
//	vertices[7] = -1.0 + destX  ;
//	vertices[8] = z;
//
//	vertices[9] =  1.0 + destY  ;
//	vertices[10] = -1.0 + destX ;
//	vertices[11] = z;
//
//    glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//
//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
//	glColor4f(1.0f, 1.0f, 1.0f, alpha);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
//	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}


void BlitSize(CSlrImage *what, float destX, float destY, float z, float size)
{
	SYS_FatalExit("Blit: not implemented");
//	float sizeX = size;
//	float sizeY = size;
//
//	texCoords[0] = 0.0;
//	texCoords[1] = 0.0;
//	texCoords[2] = 1.0;
//	texCoords[3] = 0.0;
//	texCoords[4] = 0.0;
//	texCoords[5] = 1.0;
//	texCoords[6] = 1.0;
//	texCoords[7] = 1.0;
//
//	vertices[0]  =  destX			; //+ LEFT_OFFSET_X;
//	vertices[1]  =  sizeY + destY	; //+ TOP_OFFSET_Y;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	; //+ LEFT_OFFSET_X;
//	vertices[4]  =  sizeY + destY	; //+ TOP_OFFSET_Y;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			; //+ LEFT_OFFSET_X;
//	vertices[7]  =  destY			; //+ TOP_OFFSET_Y;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	; //+ LEFT_OFFSET_X;
//	vertices[10] =	destY			; //+ TOP_OFFSET_Y;
//	vertices[11] =	z;
//
//    glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Blit(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY)
{
	//LOGD("Blit: x=%f y=%f sx=%f sy=%f", destX, destY, sizeX, sizeY);

	ImVec2 uv0 = ImVec2(image->defaultTexStartX, image->defaultTexStartY);
	ImVec2 uv1 = ImVec2(image->defaultTexEndX, image->defaultTexEndY);
	
	//	ImGuiWindow* window = ImGui::GetCurrentWindow();
	//	ImVec2 pMin(window->ContentRegionRect.Min.x + destX,			window->ContentRegionRect.Min.y + destY);
	//	ImVec2 pMax(window->ContentRegionRect.Min.x + destX + sizeX,	window->ContentRegionRect.Min.y + destY + sizeY);
	
	ImVec2 pMin(destX,			destY);
	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddImage(image->texturePtr, pMin, pMax, uv0, uv1,
					   ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));

	
}

void BlitFlipVertical(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY)
{
	SYS_FatalExit("Blit: not implemented");
//	//LOGD("Blit: x=%f y=%f sx=%f sy=%f", destX, destY, sizeX, sizeY);
//
//	texCoords[0] = what->defaultTexStartX;
//	texCoords[1] = what->defaultTexStartY;
//	texCoords[2] = what->defaultTexEndX;
//	texCoords[3] = what->defaultTexStartY;
//	texCoords[4] = what->defaultTexStartX;
//	texCoords[5] = what->defaultTexEndY;
//	texCoords[6] = what->defaultTexEndX;
//	texCoords[7] = what->defaultTexEndY;
//
//	vertices[0]  =  destX			;
//	vertices[1]  =  destY	;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	;
//	vertices[4]  =  destY	;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			;
//	vertices[7]  =  sizeY + destY	;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	;
//	vertices[10] =	sizeY + destY	;
//	vertices[11] =	z;
//
////	for (int i = 0; i < 12; i++)
////	{
////		LOGD("vertices[%d] = %f", i, vertices[i]);
////		LOGD("  texCoords[%d] = %f", i, texCoords[i]);
////	}
//
//    glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

}

void BlitFlipHorizontal(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY)
{
	SYS_FatalExit("Blit: not implemented");
//	//LOGD("Blit: x=%f y=%f sx=%f sy=%f", destX, destY, sizeX, sizeY);
//
//	texCoords[0] = what->defaultTexEndX;
//	texCoords[1] = what->defaultTexStartY;
//	texCoords[2] = what->defaultTexStartX;
//	texCoords[3] = what->defaultTexStartY;
//	texCoords[4] = what->defaultTexEndX;
//	texCoords[5] = what->defaultTexEndY;
//	texCoords[6] = what->defaultTexStartX;
//	texCoords[7] = what->defaultTexEndY;
//
//	vertices[0]  =  destX	;
//	vertices[1]  =  destY	;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	;
//	vertices[4]  =  destY	;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			;
//	vertices[7]  =  sizeY + destY	;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	;
//	vertices[10] =	sizeY + destY	;
//	vertices[11] =	z;
//
////	for (int i = 0; i < 12; i++)
////	{
////		LOGD("vertices[%d] = %f", i, vertices[i]);
////		LOGD("  texCoords[%d] = %f", i, texCoords[i]);
////	}
//
//    glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	
}

void BlitAtl(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY)
{
	SYS_FatalExit("Blit: not implemented");
//	sizeX -=1;
//	sizeY -=1;
//
//	// plain exchange y
//	texCoords[0] = what->defaultTexStartX;
//	texCoords[1] = 1.0f - what->defaultTexEndY;
//	texCoords[2] = what->defaultTexEndX;
//	texCoords[3] = 1.0f - what->defaultTexEndY;
//	texCoords[4] = what->defaultTexStartX;
//	texCoords[5] = 1.0f - what->defaultTexStartY;
//	texCoords[6] = what->defaultTexEndX;
//	texCoords[7] = 1.0f - what->defaultTexStartY;
//
//	vertices[0]  =  destX			;
//	vertices[1]  =  sizeY + destY	;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	;
//	vertices[4]  =  sizeY + destY	;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			;
//	vertices[7]  =  destY			;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	;
//	vertices[10] =	destY			;
//	vertices[11] =	z;
//
//    glBindTexture(GL_TEXTURE_2D, what->imgAtlas->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void BlitAtlFlipHorizontal(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY)
{
	SYS_FatalExit("Blit: not implemented");
//	sizeX -=1;
//	sizeY -=1;
//
//	// plain exchange y
//	texCoords[0] = what->defaultTexEndX;
//	texCoords[1] = 1.0f - what->defaultTexEndY;
//	texCoords[2] = what->defaultTexStartX;
//	texCoords[3] = 1.0f - what->defaultTexEndY;
//	texCoords[4] = what->defaultTexEndX;
//	texCoords[5] = 1.0f - what->defaultTexStartY;
//	texCoords[6] = what->defaultTexStartX;
//	texCoords[7] = 1.0f - what->defaultTexStartY;
//
//	vertices[0]  =  destX			;
//	vertices[1]  =  sizeY + destY	;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	;
//	vertices[4]  =  sizeY + destY	;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			;
//	vertices[7]  =  destY			;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	;
//	vertices[10] =	destY			;
//	vertices[11] =	z;
//
//    glBindTexture(GL_TEXTURE_2D, what->imgAtlas->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void BlitAtlAlpha(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY, float alpha)
{
	SYS_FatalExit("Blit: not implemented");
//	sizeX -=1;
//	sizeY -=1;
//
//	// plain exchange y
//	texCoords[0] = what->defaultTexStartX;
//	texCoords[1] = 1.0f - what->defaultTexEndY;
//	texCoords[2] = what->defaultTexEndX;
//	texCoords[3] = 1.0f - what->defaultTexEndY;
//	texCoords[4] = what->defaultTexStartX;
//	texCoords[5] = 1.0f - what->defaultTexStartY;
//	texCoords[6] = what->defaultTexEndX;
//	texCoords[7] = 1.0f - what->defaultTexStartY;
//
//	vertices[0]  =  destX			;
//	vertices[1]  =  sizeY + destY	;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	;
//	vertices[4]  =  sizeY + destY	;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			;
//	vertices[7]  =  destY			;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	;
//	vertices[10] =	destY			;
//	vertices[11] =	z;
//
//    glBindTexture(GL_TEXTURE_2D, what->imgAtlas->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//
//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
//	glColor4f(1.0f, 1.0f, 1.0f, alpha);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
//	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void BlitAlpha(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY, float alpha)
{
	SYS_FatalExit("Blit: not implemented");
//	texCoords[0] = what->defaultTexStartX;
//	texCoords[1] = what->defaultTexStartY;
//	texCoords[2] = what->defaultTexEndX;
//	texCoords[3] = what->defaultTexStartY;
//	texCoords[4] = what->defaultTexStartX;
//	texCoords[5] = what->defaultTexEndY;
//	texCoords[6] = what->defaultTexEndX;
//	texCoords[7] = what->defaultTexEndY;
//
//	vertices[0]  =  destX			;
//	vertices[1]  =  sizeY + destY	;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	;
//	vertices[4]  =  sizeY + destY	;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			;
//	vertices[7]  =  destY			;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	;
//	vertices[10] =	destY			;
//	vertices[11] =	z;
//
//    glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//
//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
//	glColor4f(1.0f, 1.0f, 1.0f, alpha);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
//	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void BlitMixColor(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY, float alpha,
				  float mixColorR, float mixColorG, float mixColorB)
{
	SYS_FatalExit("Blit: not implemented");
//	texCoords[0] = what->defaultTexStartX;
//	texCoords[1] = what->defaultTexStartY;
//	texCoords[2] = what->defaultTexEndX;
//	texCoords[3] = what->defaultTexStartY;
//	texCoords[4] = what->defaultTexStartX;
//	texCoords[5] = what->defaultTexEndY;
//	texCoords[6] = what->defaultTexEndX;
//	texCoords[7] = what->defaultTexEndY;
//
//	vertices[0]  =  destX			;
//	vertices[1]  =  sizeY + destY	;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	;
//	vertices[4]  =  sizeY + destY	;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			;
//	vertices[7]  =  destY			;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	;
//	vertices[10] =	destY			;
//	vertices[11] =	z;
//
//	glBindTexture(GL_TEXTURE_2D, what->textureId);
//	glVertexPointer(3, GL_FLOAT, 0, vertices);
//	glNormalPointer(GL_FLOAT, 0, normals);
//	glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//
//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
//	glColor4f(mixColorR, mixColorG, mixColorB, alpha);
//	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
//	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

}

void BlitAlpha_aaaa(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY, float alpha)
{
	SYS_FatalExit("Blit: not implemented");
//	texCoords[0] = what->defaultTexStartX;
//	texCoords[1] = what->defaultTexStartY;
//	texCoords[2] = what->defaultTexEndX;
//	texCoords[3] = what->defaultTexStartY;
//	texCoords[4] = what->defaultTexStartX;
//	texCoords[5] = what->defaultTexEndY;
//	texCoords[6] = what->defaultTexEndX;
//	texCoords[7] = what->defaultTexEndY;
//
//	vertices[0]  =  destX			;
//	vertices[1]  =  sizeY + destY	;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	;
//	vertices[4]  =  sizeY + destY	;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			;
//	vertices[7]  =  destY			;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	;
//	vertices[10] =	destY			;
//	vertices[11] =	z;
//
//    glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//
//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
//	glColor4f(alpha, alpha, alpha, alpha);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
//	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void BlitAlphaColor(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY,
					float texStartX, float texStartY,
					float texEndX, float texEndY,
					float colorR, float colorG, float colorB, float alpha)
{
#ifdef LOG_BLITS  
	LOGG("BlitAlphaColor: %s", what->name);
#endif
	
	ImVec2 uv0 = ImVec2(texStartX, texStartY);
	ImVec2 uv1 = ImVec2(texEndX, texEndY);
	ImVec2 pMin(destX,			destY);
	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddImage(image->texturePtr, pMin, pMax, uv0, uv1,
					   ImGui::ColorConvertFloat4ToU32(ImVec4(colorR, colorG, colorB, alpha)));
	
#ifdef LOG_BLITS
	LOGG("BlitAlphaColor done: %s", what->name);
#endif
	

}


void BlitCheckAtl(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY)
{
	if (what->isFromAtlas)
	{
		BlitAtl(what, destX, destY, z, sizeX, sizeY);
	}
	else
	{
		Blit(what, destX, destY, z, sizeX, sizeY);
	}

}

void BlitCheckAtlAlpha(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY, float alpha)
{
	if (what->isFromAtlas)
	{
		BlitAtlAlpha(what, destX, destY, z, sizeX, sizeY, alpha);
	}
	else
	{
		BlitAlpha(what, destX, destY, z, sizeX, sizeY, alpha);
	}

}

void BlitCheckAtlFlipHorizontal(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY)
{
	if (what->isFromAtlas)
	{
		BlitAtlFlipHorizontal(what, destX, destY, z, sizeX, sizeY);
	}
	else
	{
		SYS_FatalExit("not implemented");
		//..BlitFlipHorizontal(what, destX, destY, z, sizeX, sizeY, alpha);
	}

}

void Blit(CSlrImage *image, float destX, float destY, float z, float size,
		  float texStartX, float texStartY,
		  float texEndX, float texEndY)
{
	float sizeX = size;
	float sizeY = size;

	//	LOGD("destX=%f destY=%f", destX, destY);
	ImVec2 uv0 = ImVec2(texStartX, texStartY);
	ImVec2 uv1 = ImVec2(texEndX, texEndY);
	ImVec2 pMin(destX,			destY);
	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddImage(image->texturePtr, pMin, pMax, uv0, uv1,
					   ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));

}

void BlitAlpha(CSlrImage *image, float destX, float destY, float z, float size,
			   float texStartX, float texStartY,
			   float texEndX, float texEndY, float alpha)
{
	float sizeX = size;
	float sizeY = size;

	//	LOGD("destX=%f destY=%f", destX, destY);
	ImVec2 uv0 = ImVec2(texStartX, texStartY);
	ImVec2 uv1 = ImVec2(texEndX, texEndY);
	ImVec2 pMin(destX,			destY);
	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddImage(image->texturePtr, pMin, pMax, uv0, uv1,
					   ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)));
}

void BlitAlpha_aaaa(CSlrImage *what, float destX, float destY, float z, float size,
			   float texStartX, float texStartY,
			   float texEndX, float texEndY, float alpha)
{
	SYS_FatalExit("Blit: not implemented");
//	float sizeX = size;
//	float sizeY = size;
//
//	// plain exchange y
//	texCoords[0] = texStartX;
//	texCoords[1] = 1.0f - texEndY;
//	texCoords[2] = texEndX;
//	texCoords[3] = 1.0f - texEndY;
//	texCoords[4] = texStartX;
//	texCoords[5] = 1.0f - texStartY;
//	texCoords[6] = texEndX;
//	texCoords[7] = 1.0f - texStartY;
//
//	vertices[0]  =  destX			;
//	vertices[1]  =  sizeY + destY	;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	;
//	vertices[4]  =  sizeY + destY	;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			;
//	vertices[7]  =  destY			;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	;
//	vertices[10] =	destY			;
//	vertices[11] =	z;
//
//	glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//
//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
//	glColor4f(alpha, alpha, alpha, alpha);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
//	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	
}

void Blit(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY,
		  float texStartX, float texStartY,
		  float texEndX, float texEndY)
{
//	LOGD("destX=%f destY=%f", destX, destY);
	ImVec2 uv0 = ImVec2(texStartX, texStartY);
	ImVec2 uv1 = ImVec2(texEndX, texEndY);
	
//	ImGuiWindow* window = ImGui::GetCurrentWindow();
//	ImVec2 pMin(window->ContentRegionRect.Min.x + destX,			window->ContentRegionRect.Min.y + destY);
//	ImVec2 pMax(window->ContentRegionRect.Min.x + destX + sizeX,	window->ContentRegionRect.Min.y + destY + sizeY);

	ImVec2 pMin(destX,			destY);
	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddImage(image->texturePtr, pMin, pMax, uv0, uv1,
					   ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));
}

void BlitFlipVertical(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY,
		  float texStartX, float texStartY,
		  float texEndX, float texEndY)
{
//	LOGD("destX=%f destY=%f", destX, destY);
	ImVec2 uv0 = ImVec2(texStartX, texStartY);
	ImVec2 uv1 = ImVec2(texEndX, texEndY);
	
//	ImGuiWindow* window = ImGui::GetCurrentWindow();
//	ImVec2 pMin(window->ContentRegionRect.Min.x + destX,			window->ContentRegionRect.Min.y + destY);
//	ImVec2 pMax(window->ContentRegionRect.Min.x + destX + sizeX,	window->ContentRegionRect.Min.y + destY + sizeY);

	ImVec2 pMin(destX,			destY);
	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddImage(image->texturePtr, pMin, pMax, uv0, uv1,
					   ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));
}


void BlitInImGuiWindow(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY,
					   float texStartX, float texStartY,
					   float texEndX, float texEndY, float colorR, float colorG, float colorB, float alpha)
{
	//	LOGD("destX=%f destY=%f", destX, destY);
	ImVec2 uv0 = ImVec2(texStartX, texStartY);
	ImVec2 uv1 = ImVec2(texEndX, texEndY);
	
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	ImVec2 pMin(window->ContentRegionRect.Min.x + destX,			window->ContentRegionRect.Min.y + destY);
	ImVec2 pMax(window->ContentRegionRect.Min.x + destX + sizeX,	window->ContentRegionRect.Min.y + destY + sizeY);
	
//	ImVec2 pMin(destX,			destY);
//	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddImage(image->texturePtr, pMin, pMax, uv0, uv1,
					   ImGui::ColorConvertFloat4ToU32(ImVec4(colorR, colorG, colorB, alpha)));
	
}

void BlitInImGuiWindow(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY,
					   float texStartX, float texStartY,
					   float texEndX, float texEndY)
{
	//	LOGD("destX=%f destY=%f", destX, destY);
	ImVec2 uv0 = ImVec2(texStartX, texStartY);
	ImVec2 uv1 = ImVec2(texEndX, texEndY);
	
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	ImVec2 pMin(window->ContentRegionRect.Min.x + destX,			window->ContentRegionRect.Min.y + destY);
	ImVec2 pMax(window->ContentRegionRect.Min.x + destX + sizeX,	window->ContentRegionRect.Min.y + destY + sizeY);
	
//	ImVec2 pMin(destX,			destY);
//	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddImage(image->texturePtr, pMin, pMax, uv0, uv1,
					   ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));
	
}

void BlitInImGuiWindow(CSlrImage *image)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	float w = window->ContentRegionRect.GetWidth();
	float scale = (w / (float)image->width);
	float h = image->height * scale;
	
	BlitInImGuiWindow(image,
					  0, 0, -1, w, h,
					  image->defaultTexStartX,
					  image->defaultTexStartY,
					  image->defaultTexEndX,
					  image->defaultTexEndY);

}

void BlitInImGuiWindow(CSlrImage *image, float posX, float posY)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	float w = window->ContentRegionRect.GetWidth();
	float scale = (w / (float)image->width);
	float h = image->height * scale;
	
	BlitInImGuiWindow(image,
					  posX, posY, -1, w, h,
					  image->defaultTexStartX,
					  image->defaultTexStartY,
					  image->defaultTexEndX,
					  image->defaultTexEndY);
	
}

void BlitInImGuiWindow(CSlrImage *image, float posX, float posY, float alpha)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	float w = window->ContentRegionRect.GetWidth();
	float scale = (w / (float)image->width);
	float h = image->height * scale;
	
	BlitInImGuiWindow(image,
					  posX, posY, -1, w, h,
					  image->defaultTexStartX,
					  image->defaultTexStartY,
					  image->defaultTexEndX,
					  image->defaultTexEndY,
					  1.0f, 1.0f, 1.0f, alpha);
	
}


void BlitAlpha(CSlrImage *image, float destX, float destY, float z, float sizeX, float sizeY,
			   float texStartX, float texStartY,
			   float texEndX, float texEndY,
			   float alpha)
{
	//	LOGD("destX=%f destY=%f", destX, destY);
		ImVec2 uv0 = ImVec2(texStartX, texStartY);
		ImVec2 uv1 = ImVec2(texEndX, texEndY);
		
	//	ImGuiWindow* window = ImGui::GetCurrentWindow();
	//	ImVec2 pMin(window->ContentRegionRect.Min.x + destX,			window->ContentRegionRect.Min.y + destY);
	//	ImVec2 pMax(window->ContentRegionRect.Min.x + destX + sizeX,	window->ContentRegionRect.Min.y + destY + sizeY);

		ImVec2 pMin(destX,			destY);
		ImVec2 pMax(destX + sizeX,	destY + sizeY);
		
		ImDrawList *drawList = ImGui::GetWindowDrawList();
		drawList->AddImage(image->texturePtr, pMin, pMax, uv0, uv1,
						   ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)));
}

void BlitAlpha_aaaa(CSlrImage *what, float destX, float destY, float z, float sizeX, float sizeY,
			   float texStartX, float texStartY,
			   float texEndX, float texEndY,
			   float alpha)
{
	SYS_FatalExit("Blit: not implemented");
//	// plain exchange y
//	texCoords[0] = texStartX;
//	texCoords[1] = 1.0f - texEndY;
//	texCoords[2] = texEndX;
//	texCoords[3] = 1.0f - texEndY;
//	texCoords[4] = texStartX;
//	texCoords[5] = 1.0f - texStartY;
//	texCoords[6] = texEndX;
//	texCoords[7] = 1.0f - texStartY;
//
//	vertices[0]  =  destX			;
//	vertices[1]  =  sizeY + destY	;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	;
//	vertices[4]  =  sizeY + destY	;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			;
//	vertices[7]  =  destY			;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	;
//	vertices[10] =	destY			;
//	vertices[11] =	z;
//
//    glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//
//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
//	glColor4f(alpha, alpha, alpha, alpha);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
//	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	
}

void BlitFilledRectangle(float destX, float destY, float z, float sizeX, float sizeY,
						 float colorR, float colorG, float colorB, float alpha)
{
	
	ImVec2 pMin(destX,			destY);
	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawFlags corners_none = 0;

	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(ImVec4(colorR, colorG, colorB, alpha)), 0.0f, corners_none);

}

void BlitFilledRectangleOnForeground(float destX, float destY, float z, float sizeX, float sizeY,
						 float colorR, float colorG, float colorB, float alpha)
{
	
	ImVec2 pMin(destX,			destY);
	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawFlags corners_none = 0;

	ImDrawList *drawList = ImGui::GetForegroundDrawList();
	drawList->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(ImVec4(colorR, colorG, colorB, alpha)), 0.0f, corners_none);

}

void BlitGradientRectangle(float destX, float destY, float z, float sizeX, float sizeY,
						   float colorR1, float colorG1, float colorB1, float colorA1,
						   float colorR2, float colorG2, float colorB2, float colorA2,
						   float colorR3, float colorG3, float colorB3, float colorA3,
						   float colorR4, float colorG4, float colorB4, float colorA4)
						   
{
	SYS_FatalExit("Blit: not implemented");
//	vertices[0]  =  destX			;
//	vertices[1]  =  sizeY + destY	;
//	vertices[2]  =	z;
//
//	vertices[3]  =  sizeX + destX	;
//	vertices[4]  =  sizeY + destY	;
//	vertices[5]  =	z;
//
//	vertices[6]  =  destX			;
//	vertices[7]  =  destY			;
//	vertices[8]  =	z;
//
//	vertices[9]  =  sizeX + destX	;
//	vertices[10] =	destY			;
//	vertices[11] =	z;
//
//	vertsColors[0]	= colorR1;
//	vertsColors[1]	= colorG1;
//	vertsColors[2]	= colorB1;
//	vertsColors[3]	= colorA1;
//
//	vertsColors[4]	= colorR2;
//	vertsColors[5]	= colorG2;
//	vertsColors[6]	= colorB2;
//	vertsColors[7]	= colorA2;
//
//	vertsColors[8]	= colorR3;
//	vertsColors[9]	= colorG3;
//	vertsColors[10]	= colorB3;
//	vertsColors[11]	= colorA3;
//
//	vertsColors[12]	= colorR4;
//	vertsColors[13]	= colorG4;
//	vertsColors[14]	= colorB4;
//	vertsColors[15]	= colorA4;
//
//	glDisable(GL_TEXTURE_2D);
//
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//	glColorPointer(4, GL_FLOAT, 0, vertsColors);
//
//	glEnableClientState(GL_COLOR_ARRAY);
//
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
//
//	glEnable(GL_TEXTURE_2D);
//
//	glDisableClientState(GL_COLOR_ARRAY);
}

void BlitRectangle(float destX, float destY, float z, float sizeX, float sizeY,
				   float colorR, float colorG, float colorB, float alpha)
{
	BlitRectangle(destX, destY, z, sizeX, sizeY, colorR, colorG, colorB, alpha, 1.0f);
}

void BlitRectangle(float destX, float destY, float z, float sizeX, float sizeY,
				   float colorR, float colorG, float colorB, float alpha, float lineWidth)
{
	ImVec2 pMin(destX,			destY);
	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawFlags corners_none = 0;

	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddRect(pMin, pMax, ImGui::ColorConvertFloat4ToU32(ImVec4(colorR, colorG, colorB, alpha)), 0.0f, corners_none, lineWidth);
}

void VID_EnableSolidsOnly()
{
	SYS_FatalExit("Blit: not implemented");
//    glEnableClientState(GL_VERTEX_ARRAY);
//    glDisableClientState(GL_NORMAL_ARRAY);
//    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
//    glDisableClientState(GL_COLOR_ARRAY);
	
}

void VID_DisableSolidsOnly()
{
	SYS_FatalExit("Blit: not implemented");
//    glEnableClientState(GL_VERTEX_ARRAY);
//    glEnableClientState(GL_NORMAL_ARRAY);
//    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
//    glDisableClientState(GL_COLOR_ARRAY);
}

void VID_DisableTextures()
{
	SYS_FatalExit("Blit: not implemented");
//	glDisable(GL_TEXTURE_2D);
}

void VID_EnableTextures()
{
	SYS_FatalExit("Blit: not implemented");
//	glEnable(GL_TEXTURE_2D);
//	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}


void BlitLine(float startX, float startY, float endX, float endY, float posZ,
			  float colorR, float colorG, float colorB, float alpha)
{
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddLine(ImVec2(startX, startY), ImVec2(endX, endY), ImGui::ColorConvertFloat4ToU32(ImVec4(colorR, colorG, colorB, alpha)));
}

void BlitLine(float startX, float startY, float endX, float endY, float posZ,
			  float colorR, float colorG, float colorB, float alpha, float thickness)
{
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddLine(ImVec2(startX, startY), ImVec2(endX, endY), ImGui::ColorConvertFloat4ToU32(ImVec4(colorR, colorG, colorB, alpha)), thickness);
}

void BlitCircle(float centerX, float centerY, float radius, float colorR, float colorG, float colorB, float colorA)
{
	SYS_FatalExit("Blit: not implemented");
	return;
	
//	const int numCircleVerts = 48;
//    float glverts[numCircleVerts*2];
//    glVertexPointer(2, GL_FLOAT, 0, glverts);
//    glEnableClientState(GL_VERTEX_ARRAY);
//
//	float angle = 0;
//	for (int i = 0; i < numCircleVerts; i++, angle += DEGTORAD*360.0f/(numCircleVerts-1))
//	{
//		glverts[i*2]   = sinf(angle)*radius;
//		glverts[i*2+1] = cosf(angle)*radius;
//	}
//
//	glPushMatrix();
//	glTranslatef(centerX, centerY, 0);
//
//    //edge lines
//	glDisable(GL_TEXTURE_2D);
//	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
//	glDisableClientState(GL_COLOR_ARRAY);
//
//	glColor4f(colorR, colorG, colorB, colorA);
//	glDrawArrays(GL_LINE_LOOP, 0, numCircleVerts);
//
//	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
//	glEnable(GL_TEXTURE_2D);
//
//	glPopMatrix();
//
//	glVertexPointer(3, GL_FLOAT, 0, vertices);
//	glNormalPointer(GL_FLOAT, 0, normals);
//	glTexCoordPointer(2, GL_FLOAT, 0, texCoords);

}

void BlitFilledCircle(float centerX, float centerY, float radius, float colorR, float colorG, float colorB, float colorA)
{
	SYS_FatalExit("Blit: not implemented");
	
//	const int numCircleVerts = 32;
//    float glverts[numCircleVerts*2];
//    glVertexPointer(2, GL_FLOAT, 0, glverts);
//    glEnableClientState(GL_VERTEX_ARRAY);
//
//	float angle = 0;
//	for (int i = 0; i < numCircleVerts; i++, angle += DEGTORAD*360.0f/(numCircleVerts-1))
//	{
//		glverts[i*2]   = MTH_FastSin(angle)*radius;	//sinf
//		glverts[i*2+1] = MTH_FastCos(angle)*radius;	//cosf
//	}
//
//	glPushMatrix();
//	glTranslatef(centerX, centerY, 0);
//
//	glColor4f(colorR, colorG, colorB, colorA);
//	glDrawArrays(GL_TRIANGLE_FAN, 0, numCircleVerts);
//
//    //edge lines
//	glDisable(GL_TEXTURE_2D);
//	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
//	glDisableClientState(GL_COLOR_ARRAY);
//
//	glColor4f(colorR, colorG, colorB, colorA);
//	glDrawArrays(GL_LINE_LOOP, 0, numCircleVerts);
//
//	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
//	glEnable(GL_TEXTURE_2D);
//
//	glPopMatrix();
//
//	glVertexPointer(3, GL_FLOAT, 0, vertices);
//	glNormalPointer(GL_FLOAT, 0, normals);
//	glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
}

void GenerateLineStripFromCircularBuffer(CGLLineStrip *lineStrip, signed short *data, int length, int pos, float posX, float posY, float posZ, float sizeX, float sizeY)
{
#ifdef LOG_BLITS
	LOGG("GenerateLineStripFromCircularBuffer");
#endif

	int dataLen = (int)((sizeX+1) * 3.0);
	lineStrip->Update(dataLen);
	float *lineStripData = lineStrip->lineStripData;

	float step = length / sizeX;

	float samplePos = pos;
	int c = 0;

	for (float x = 0; x <= sizeX; x += 1.0)
	{
		lineStripData[c] = posX + x;
		c++;
		lineStripData[c] = posY + (((float)data[(int)samplePos] + 32767.0) / 65536.0) * sizeY;
		c++;
		lineStripData[c] = posZ;
		c++;

		samplePos += step;
		if (samplePos >= length)
			samplePos = 0;

		if ((c+3) >= dataLen)
			break;
	}
	lineStrip->numberOfPoints = (int)(c / 3);

#ifdef LOG_BLITS
	LOGG("GenerateLineStripFromCircularBuffer done");
#endif

	return;
}

void GenerateLineStripFromFft(CGLLineStrip *lineStrip, float *data, int start, int count, float multiplier, float posX, float posY, float posZ, float sizeX, float sizeY)
{
#ifdef LOG_BLITS
	LOGG("GenerateLineStripFromFft");
#endif

	int dataLen = (int)((sizeX+1) * 3.0);
	lineStrip->Update(dataLen);
	float *lineStripData = lineStrip->lineStripData;

	float step = count / sizeX;

	float samplePos = start;
	float counter = 0;
	float endY = posY + sizeY;
	int c = 0;

	float maxVal = data[0];

	//if (step <= 1.0)
	{
		for (float x = 0; x <= sizeX; x += 1.0)
		{
			//lineStripData[c] = posY + (((float)data[(int)samplePos] + 32767.0) / 65536.0) * sizeY;
			lineStripData[c] = posX + x;
			c++;
			lineStripData[c] = (endY) - (maxVal*multiplier) * sizeY;
			c++;
			lineStripData[c] = posZ;
			c++;

			int prevSamplePos = (int)samplePos;
			samplePos += step;
			int nextSamplePos = (int)samplePos;

			if (prevSamplePos+1 < nextSamplePos)
			{
				maxVal = 0.0;
				for (int i = prevSamplePos; i <= nextSamplePos; i++)
				{
					float val = data[(int)samplePos];
					if (fabs(val) > fabs(maxVal))
					{
						maxVal = val;
					}
				}
			}
			else
			{
				maxVal = data[(int)samplePos];
			}

			counter += step;
			if (((int)counter) >= (int)count)
				break;
		}
	}

	lineStrip->numberOfPoints = (int)(c / 3);

#ifdef LOG_BLITS
	LOGG("GenerateLineStripFromFft done");
#endif

	return;

}


void GenerateLineStripFromFloat(CGLLineStrip *lineStrip, float *data, int start, int count, float posX, float posY, float posZ, float sizeX, float sizeY)
{
#ifdef LOG_BLITS
	LOGG("GenerateLineStripFromFloat");
#endif

	int dataLen = (int)((sizeX+1) * 3.0);
	lineStrip->Update(dataLen);
	float *lineStripData = lineStrip->lineStripData;

	float step = count / sizeX;

	float samplePos = start;
	float counter = 0;
	int c = 0;

	float maxVal = data[0];

	//if (step <= 1.0)
	{
		for (float x = 0; x <= sizeX; x += 1.0)
		{
			//lineStripData[c] = posY + (((float)data[(int)samplePos] + 32767.0) / 65536.0) * sizeY;
			lineStripData[c] = posX + x;
			c++;
			lineStripData[c] = posY + (maxVal) * sizeY;
			c++;
			lineStripData[c] = posZ;
			c++;

			int prevSamplePos = (int)samplePos;
			samplePos += step;
			int nextSamplePos = (int)samplePos;

			if (prevSamplePos+1 < nextSamplePos)
			{
				maxVal = 0.0;
				for (int i = prevSamplePos; i <= nextSamplePos; i++)
				{
					float val = data[(int)samplePos];
					if (fabs(val) > fabs(maxVal))
					{
						maxVal = val;
					}
				}
			}
			else
			{
				maxVal = data[(int)samplePos];
			}

			counter += step;
			if (((int)counter) >= (int)count)
				break;
		}
	}

	lineStrip->numberOfPoints = (int)(c / 3);

#ifdef LOG_BLITS
	LOGG("GenerateLineStripFromFloat done");
#endif

	return;

}

#ifdef IS_TRACKER
void GenerateLineStripFromEnvelope(CGLLineStrip *lineStrip,
								   envelope_t *envelope,
								   float posX, float posY, float posZ, float sizeX, float sizeY)
{
#ifdef LOG_BLITS
	LOGG("GenerateLineStripFromEnvelope");
#endif

	int dataLen = (int)(envelope->numPoints * 3);
	lineStrip->Update(dataLen);

	float *lineStripData = lineStrip->lineStripData;

	//LOGG("GenerateLineStripFromEnvelope: envelope numPoints=%d", envelope->numPoints);

	// x = 0..324
	// y = 0..64
	int c = 0;
	for (int currentPoint = 0; currentPoint < envelope->numPoints; currentPoint++)
	{
		float x = envelope->points[currentPoint * 2];
		float y = envelope->points[currentPoint * 2 + 1];

		//LOGG("point=%d x=%f y=%f", currentPoint, x, y);
		lineStripData[c] = posX + (x / 324.0) * sizeX;
		c++;
		lineStripData[c] = posY + ((64.0 - y)/64.0) * sizeY;
		c++;
		lineStripData[c] = posZ;
		c++;

	}

	//SYS_FatalExit("exit");
	lineStrip->length = (int)(c / 3);

#ifdef LOG_BLITS
	LOGG("GenerateLineStripFromEnvelope done");
#endif

	return;

	//SYS_Errorf("generatelinestrip");
}
#endif


void GenerateLineStrip(CGLLineStrip *lineStrip,
					   signed short *data,
					   int start, int count,
					   float posX, float posY, float posZ, float sizeX, float sizeY)
{
#ifdef LOG_BLITS
	LOGG("GenerateLineStrip");
#endif

	int dataLen = (int)((sizeX+1) * 3.0);
	lineStrip->Update(dataLen);
	float *lineStripData = lineStrip->lineStripData;

	float step = count / sizeX;

	float samplePos = start;
	float counter = 0;
	int c = 0;
	float end = (float)(start+count);
	signed short maxVal = data[0];

	//if (step <= 1.0)
	{
		for (float x = 0; x <= sizeX; x += 1.0)
		{
			//lineStripData[c] = posY + (((float)data[(int)samplePos] + 32767.0) / 65536.0) * sizeY;
			lineStripData[c] = posX + x;
			c++;
			lineStripData[c] = posY + (((float)maxVal + 32767.0) / 65536.0) * sizeY;
			c++;
			lineStripData[c] = posZ;
			c++;

			//int prevSamplePos = (int)samplePos;
			samplePos += step;
			//int nextSamplePos = (int)samplePos;

			if (samplePos >= end)
				break;

			// debug -> simple:
			/* maxVal
			 if (prevSamplePos+1 < nextSamplePos)
			 {
			 maxVal = 0.0;
			 for (int i = prevSamplePos; i <= nextSamplePos; i++)
			 {
			 signed short val = data[(int)samplePos];
			 if (abs(val) > abs(maxVal))
			 {
			 maxVal = val;
			 }
			 }
			 }
			 else*/
			{
				maxVal = data[(int)samplePos];
			}

			counter += step;
			if (((int)counter) >= (int)count)
				break;
		}
	}

	lineStrip->numberOfPoints = (int)(c / 3);

#ifdef LOG_BLITS
	LOGG("GenerateLineStrip done");
#endif


	return;
}

ImU32 FloatToColorU32(float colorR, float colorG, float colorB, float alpha)
{
	u8 cr = (u8)(colorR * 255.0f);
	u8 cg = (u8)(colorG * 255.0f);
	u8 cb = (u8)(colorB * 255.0f);
	u8 ca = (u8)(alpha * 255.0f);
	
	u32 col = cr << 24 | cg << 16 || cb << 8 | ca;
	
	return col;
}


void BlitLineStrip(CGLLineStrip *glLineStrip, float colorR, float colorG, float colorB, float alpha)
{
//	LOGD("BlitLineStrip");
	
	// B
	ImU32 col = 0xEEFFFFFF; //FloatToColorU32(colorR, colorG, colorB, alpha);
	ImDrawList *drawList = ImGui::GetWindowDrawList();

	float prevpx = 0.0f;
	float prevpy = 0.0f;
	float prevpz = 0.0f;

	u32 c = 0;
	for (int i = 0; i < glLineStrip->numberOfPoints; i++)
	{
		float px = glLineStrip->lineStripData[c++];
		float py = glLineStrip->lineStripData[c++];
		float pz = glLineStrip->lineStripData[c++];
		
		if (i != 0)
		{
			// draw line
			ImVec2 p1(prevpx, prevpy);
			ImVec2 p2(px, py);

			drawList->AddLine(p1, p2, col);
		}
		
		prevpx = px;
		prevpy = py;
		prevpz = pz;
	}
	
	/*
	
	
	ImVec2 pMin(destX,			destY);
	ImVec2 pMax(destX + sizeX,	destY + sizeY);
	
	ImDrawFlags corners_none = 0;

	drawList->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(ImVec4(colorR, colorG, colorB, alpha)), 0.0f, corners_none);

	void ImDrawList::AddLine(const ImVec2& p1, const ImVec2& p2, ImU32 col, float thickness)
	{
		if ((col & IM_COL32_A_MASK) == 0)
			return;
		PathLineTo(p1 + ImVec2(0.5f, 0.5f));
		PathLineTo(p2 + ImVec2(0.5f, 0.5f));
		PathStroke(col, 0, thickness);
	}


	ImGui
	 */
	/*
	glColor4f(colorR, colorG, colorB, alpha);

	glDisable(GL_TEXTURE_2D);
	glVertexPointer(3, GL_FLOAT,  0, glLineStrip->lineStripData);

	//glEnableClientState(GL_VERTEX_ARRAY);
	glDrawArrays(GL_LINE_STRIP, 0, glLineStrip->length);
	//glDisableClientState(GL_VERTEX_ARRAY);

	glEnable(GL_TEXTURE_2D);

	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

	glVertexPointer(3, GL_FLOAT, 0, vertices);
	glNormalPointer(GL_FLOAT, 0, normals);
	glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
	 */
}

#define CENTER_MARKER_SIZE 12.0

void BlitPlus(float posX, float posY, float posZ, float r, float g, float b, float alpha)
{
	BlitLine(posX, posY-CENTER_MARKER_SIZE,
			 posX, posY+CENTER_MARKER_SIZE, posZ,
			 r, g, b, alpha);

	BlitLine(posX-CENTER_MARKER_SIZE, posY,
			 posX+CENTER_MARKER_SIZE, posY, posZ,
			 r, g, b, alpha);
}

void BlitPlus(float posX, float posY, float posZ, float sizeX, float sizeY, float r, float g, float b, float alpha)
{
	BlitLine(posX, posY-sizeY/2.0f,
			 posX, posY+sizeY/2.0f, posZ,
			 r, g, b, alpha);

	BlitLine(posX-sizeX/2.0f, posY,
			 posX+sizeX/2.0f, posY, posZ,
			 r, g, b, alpha);
}

void PushMatrix2D()
{
	SYS_FatalExit("Blit: not implemented");
//	glPushMatrix();
}

void PopMatrix2D()
{
	SYS_FatalExit("Blit: not implemented");
//	glPopMatrix();
}

void Translate2D(float posX, float posY, float posZ)
{
	SYS_FatalExit("Blit: not implemented");
//	glTranslatef(posX, posY, posZ);
}

void Rotate2D(float angle)
{
	SYS_FatalExit("Blit: not implemented");
//	glRotatef( angle , 0, 0, 1 );	//* RADTODEG
}

void Scale2D(float scaleX, float scaleY, float scaleZ)
{
	SYS_FatalExit("Blit: not implemented");
//	glScalef(scaleX, scaleY, scaleZ);
}

void BlitRotatedImage(CSlrImage *image, float pX, float pY, float pZ, float rotationAngle, float alpha)
{
	SYS_FatalExit("Blit: not implemented");
//	float rPosX = pX;
//	float rPosY = pY;
//	float rPosZ = pZ;
//	float rSizeX = image->width;
//	float rSizeY = image->height;
//	float rSizeX2 = rSizeX/2.0f;
//	float rSizeY2 = rSizeY/2.0f;
//	rPosX -= rSizeX2;
//	rPosY -= rSizeY2;
//
//	//LOGD("BLIT: %3.2f %3.2f %3.2f | %3.2f %3.2f", rPosX, rPosY, rPosZ, rSizeX, rSizeY);
//
//	PushMatrix2D();
//
//	Translate2D(rPosX + rSizeX2, rPosY + rSizeY2, rPosZ);
//	Rotate2D(rotationAngle);
//
//	BlitAlpha(image, -rSizeX2, -rSizeY2, 0, rSizeX, rSizeY, alpha);
//	PopMatrix2D();
}

void BlitTriangleAlpha(CSlrImage *what, float z, float alpha,
					   float vert1x, float vert1y, float tex1x, float tex1y,
					   float vert2x, float vert2y, float tex2x, float tex2y,
					   float vert3x, float vert3y, float tex3x, float tex3y)
{
	SYS_FatalExit("Blit: not implemented");

//#ifdef LOG_BLITS
//	LOGG("BlitTriangleAlpha: %s", what->name);
//#endif
//
//	float tx = (what->defaultTexEndX - what->defaultTexStartX);
//	float ty = (what->defaultTexEndY - what->defaultTexStartY);
//
//	float t1x = tx * tex1x + what->defaultTexStartX;
//	float t1y = ty * (1.0f-tex1y) + what->defaultTexStartY;
//	float t2x = tx * tex2x + what->defaultTexStartX;
//	float t2y = ty * (1.0f-tex2y) + what->defaultTexStartY;
//	float t3x = tx * tex3x + what->defaultTexStartX;
//	float t3y = ty * (1.0f-tex3y) + what->defaultTexStartY;
//
//	texCoords[0] = t1x;
//	texCoords[1] = t1y;
//	texCoords[2] = t2x;
//	texCoords[3] = t2y;
//	texCoords[4] = t3x;
//	texCoords[5] = t3y;
//
//	vertices[0]  =  vert1x;
//	vertices[1]  =  vert1y;
//	vertices[2]  =	z;
//
//	vertices[3]  =  vert2x;
//	vertices[4]  =  vert2y;
//	vertices[5]  =	z;
//
//	vertices[6]  =  vert3x;
//	vertices[7]  =  vert3y;
//	vertices[8]  =	z;
//
////	LOGD(" >>>> BlitTriangleAlpha <<<<");
////	for (u32 i = 0; i < 6; i++)
////		LOGD("texCoords[%d]=%3.2f", i, texCoords[i]);
////
////	for (u32 i = 0; i < 9; i++)
////		LOGD("vertices[%d]=%3.2f", i, vertices[i]);
////
////	float *norms = (float *)normals;
////	for (u32 i = 0; i < 9; i++)
////		LOGD("normals[%d]=%3.2f", i, norms[i]);
//
//    glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, vertices);
//    glNormalPointer(GL_FLOAT, 0, normals);
//    glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//
//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
//	glColor4f(1.0f, 1.0f, 1.0f, alpha);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, 3);
//	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
//
//#ifdef LOG_BLITS
//	LOGG("BlitTriangleAlpha done: %s", what->name);
//#endif
	
}

void BlitPolygonAlpha(CSlrImage *what, float alpha, float *verts, float *texs, float *norms, unsigned int numVertices)
{
	SYS_FatalExit("Blit: not implemented");
//#ifdef LOG_BLITS
//	LOGG("BlitPolygonAlpha: %s", what->name);
//#endif
//
////	LOGD("========= BlitPolygonAlpha ========");
////	for (u32 i = 0; i < 6; i++)
////		LOGD("texCoords[%d]=%3.2f", i, texs[i]);
////
////	for (u32 i = 0; i < 9; i++)
////		LOGD("vertices[%d]=%3.2f", i, verts[i]);
////
////	for (u32 i = 0; i < 9; i++)
////		LOGD("normals[%d]=%3.2f", i, norms[i]);
//
//    glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, verts);
//    glNormalPointer(GL_FLOAT, 0, norms);
//    glTexCoordPointer(2, GL_FLOAT, 0, texs);
//
//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
//	glColor4f(1.0f, 1.0f, 1.0f, alpha);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, numVertices);
//	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
//
//	glVertexPointer(3, GL_FLOAT, 0, vertices);
//	glNormalPointer(GL_FLOAT, 0, normals);
//	glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//
//#ifdef LOG_BLITS
//	LOGG("BlitPolygonAlpha done: %s", what->name);
//#endif

}

void BlitPolygonMixColor(CSlrImage *what, float mixColorR, float mixColorG, float mixColorB, float mixColorA, float *verts, float *texs, float *norms, unsigned int numVertices)
{
	SYS_FatalExit("Blit: not implemented");
//#ifdef LOG_BLITS
//	LOGG("BlitPolygonMixColor: %s", what->name);
//#endif
//
//	//	LOGD("========= BlitPolygonAlpha ========");
//	//	for (u32 i = 0; i < 6; i++)
//	//		LOGD("texCoords[%d]=%3.2f", i, texs[i]);
//	//
//	//	for (u32 i = 0; i < 9; i++)
//	//		LOGD("vertices[%d]=%3.2f", i, verts[i]);
//	//
//	//	for (u32 i = 0; i < 9; i++)
//	//		LOGD("normals[%d]=%3.2f", i, norms[i]);
//
//    glBindTexture(GL_TEXTURE_2D, what->textureId);
//    glVertexPointer(3, GL_FLOAT, 0, verts);
//    glNormalPointer(GL_FLOAT, 0, norms);
//    glTexCoordPointer(2, GL_FLOAT, 0, texs);
//
//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
//	glColor4f(mixColorR, mixColorG, mixColorB, mixColorA);
//    glDrawArrays(GL_TRIANGLE_STRIP, 0, numVertices);
//	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
//
//	glVertexPointer(3, GL_FLOAT, 0, vertices);
//	glNormalPointer(GL_FLOAT, 0, normals);
//	glTexCoordPointer(2, GL_FLOAT, 0, texCoords);
//
//#ifdef LOG_BLITS
//	LOGG("BlitPolygonMixColor done: %s", what->name);
//#endif
	
}
