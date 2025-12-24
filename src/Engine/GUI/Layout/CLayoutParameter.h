#ifndef _CLayoutParameter_h_
#define _CLayoutParameter_h_

#include "GUI_Main.h"
#include "SYS_FileSystem.h"

enum LayoutParameterType
{
	LAYOUT_PARAMETER_TYPE_UNKNOWN = 0,
	LAYOUT_PARAMETER_TYPE_BOOL,
	LAYOUT_PARAMETER_TYPE_INT,
	LAYOUT_PARAMETER_TYPE_FLOAT,
	LAYOUT_PARAMETER_TYPE_COMBO
};

class CLayoutParameter
{
public:
	CLayoutParameter(const char *name);
	CLayoutParameter(const char *name, bool isHidden);
	virtual ~CLayoutParameter();
	
	const char *name;
	
	// do not show in view's context menu
	bool isHidden;
	
	// @returns if layout parameter has changed
	virtual bool RenderImGui();
	
	virtual void Serialize(CByteBuffer *byteBuffer);
	virtual bool Deserialize(CByteBuffer *byteBuffer);
};

class CLayoutParameterBool : public CLayoutParameter
{
public:
	CLayoutParameterBool(const char *name, bool *value);
	CLayoutParameterBool(const char *name, bool isHidden, bool *value);
	CLayoutParameterBool(const char *name, bool *value, ImGuiInputTextFlags flags);
	bool *value;

	ImGuiInputTextFlags flags;
	
	virtual bool RenderImGui();
	virtual void Serialize(CByteBuffer *byteBuffer);
	virtual bool Deserialize(CByteBuffer *byteBuffer);
};

class CLayoutParameterInt : public CLayoutParameter
{
public:
	CLayoutParameterInt(const char *name, int *value);
	CLayoutParameterInt(const char *name, bool isHidden, int *value);
	CLayoutParameterInt(const char *name, int *value, int step, int step_fast, ImGuiInputTextFlags flags);
	int *value;

	int step;
	int step_fast;
	ImGuiInputTextFlags flags;
	
	virtual bool RenderImGui();
	virtual void Serialize(CByteBuffer *byteBuffer);
	virtual bool Deserialize(CByteBuffer *byteBuffer);
};

class CLayoutParameterFloat : public CLayoutParameter
{
public:
	CLayoutParameterFloat(const char *name, float *value);
	CLayoutParameterFloat(const char *name, bool isHidden, float *value);
	CLayoutParameterFloat(const char *name, float *value, float minValue, float maxValue);
	CLayoutParameterFloat(const char *name, float *value, float minValue, float maxValue, float step, float step_fast, const char* format, ImGuiInputTextFlags flags);
	float *value;

	float minValue;
	float maxValue;
	float step;
	float step_fast;
	const char* format;
	ImGuiInputTextFlags flags;
	
	virtual bool RenderImGui();
	virtual void Serialize(CByteBuffer *byteBuffer);
	virtual bool Deserialize(CByteBuffer *byteBuffer);
};

class CLayoutParameterDouble : public CLayoutParameter
{
public:
	CLayoutParameterDouble(const char *name, double *value);
	CLayoutParameterDouble(const char *name, bool isHidden, double *value);
	CLayoutParameterDouble(const char *name, double *value, float minValue, float maxValue);
	CLayoutParameterDouble(const char *name, double *value, float minValue, float maxValue, float step, float step_fast, const char* format, ImGuiInputTextFlags flags);
	double *value;

	float minValue;
	float maxValue;
	float step;
	float step_fast;
	const char* format;
	ImGuiInputTextFlags flags;
	
	virtual bool RenderImGui();
	virtual void Serialize(CByteBuffer *byteBuffer);
	virtual bool Deserialize(CByteBuffer *byteBuffer);
};

class CLayoutParameterCombo : public CLayoutParameter
{
public:
	CLayoutParameterCombo(const char *name, int *selectedItem, const char **items, int itemsCount);
	CLayoutParameterCombo(const char *name, int *selectedItem, const char **items, int itemsCount, ImGuiComboFlags flags);
	int *selectedItem;
	
	const char **items;
	int itemsCount;
	ImGuiComboFlags flags;
	
	virtual bool RenderImGui();
	virtual void Serialize(CByteBuffer *byteBuffer);
	virtual bool Deserialize(CByteBuffer *byteBuffer);
};

enum LayoutParameterPathMode : u8
{
	LayoutParameterPathModeOpen,
	LayoutParameterPathModeSave,
	LayoutParameterPathModeSelectFolder
};

class CLayoutParameterPath : public CLayoutParameter, CSystemFileDialogCallback
{
public:
	// note this is async, path will be assigned when open file dialog is closed, imgui style
	CLayoutParameterPath(const char *name, LayoutParameterPathMode layoutParameterPathMode, char **pathStrPointer);
	char **pathStrPointer;
	
	virtual bool RenderImGui();
	virtual void Serialize(CByteBuffer *byteBuffer);
	virtual bool Deserialize(CByteBuffer *byteBuffer);
	
	LayoutParameterPathMode layoutParameterPathMode;
	
	virtual void SystemDialogFileOpenSelected(CSlrString *path);
	virtual void SystemDialogFileOpenCancelled();
	virtual void SystemDialogFileSaveSelected(CSlrString *path);
	virtual void SystemDialogFileSaveCancelled();
	virtual void SystemDialogPickFolderSelected(CSlrString *path);
	virtual void SystemDialogPickFolderCancelled();
};


#endif
