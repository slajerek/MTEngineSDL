#include "GUI_Main.h"
#include "CLayoutParameter.h"
#include "CByteBuffer.h"
#include "CLayoutManager.h"

// to properly store layouts on parameter change we need to do this async way after frame is rendered in CGuiMain.
// see guiMain->StoreLayoutInSettingsAtEndOfThisFrame()

CLayoutParameter::CLayoutParameter(const char *name)
{
	this->name = name;
	this->isHidden = false;
}

CLayoutParameter::CLayoutParameter(const char *name, bool isHidden)
{
	this->name = name;
	this->isHidden = isHidden;
}

CLayoutParameter::~CLayoutParameter()
{
}
	
bool CLayoutParameter::RenderImGui()
{
	return false;
}

void CLayoutParameter::Serialize(CByteBuffer *byteBuffer)
{
}

bool CLayoutParameter::Deserialize(CByteBuffer *byteBuffer)
{
	return true;
}

//
CLayoutParameterBool::CLayoutParameterBool(const char *name, bool *value)
: CLayoutParameter(name)
{
	this->value = value;
	this->flags = 0;
}

CLayoutParameterBool::CLayoutParameterBool(const char *name, bool isHidden, bool *value)
: CLayoutParameter(name, isHidden)
{
	this->value = value;
	this->flags = 0;
}

CLayoutParameterBool::CLayoutParameterBool(const char *name, bool *value, ImGuiInputTextFlags flags)
: CLayoutParameter(name)
{
	this->value = value;
	this->flags = flags;
}

// returns if parameter has changed
bool CLayoutParameterBool::RenderImGui()
{
	if (ImGui::Checkbox(name, value))
	{
		guiMain->StoreLayoutInSettingsAtEndOfThisFrame();
		return true;
	}
	
	return false;
}

void CLayoutParameterBool::Serialize(CByteBuffer *byteBuffer)
{
	byteBuffer->PutBool(*value);
}

bool CLayoutParameterBool::Deserialize(CByteBuffer *byteBuffer)
{
	*value = byteBuffer->GetBool();
	return true;
}

//

CLayoutParameterInt::CLayoutParameterInt(const char *name, int *value)
: CLayoutParameter(name)
{
	this->value = value;
	this->step = 1;
	this->step_fast = 100;
	this->flags = 0;
}

CLayoutParameterInt::CLayoutParameterInt(const char *name, bool isHidden, int *value)
: CLayoutParameter(name, isHidden)
{
	this->value = value;
	this->step = 1;
	this->step_fast = 100;
	this->flags = 0;
}

CLayoutParameterInt::CLayoutParameterInt(const char *name, int *value, int step, int step_fast, ImGuiInputTextFlags flags)
: CLayoutParameter(name)
{
	this->value = value;
	this->step = step;
	this->step_fast = step_fast;
	this->flags = flags;
}

// returns if parameter has changed
bool CLayoutParameterInt::RenderImGui()
{
	if (ImGui::InputInt(name, value, step, step_fast, flags))
	{
		guiMain->StoreLayoutInSettingsAtEndOfThisFrame();
		return true;
	}
	
	return false;
}

void CLayoutParameterInt::Serialize(CByteBuffer *byteBuffer)
{
	byteBuffer->putInt(*value);
}

bool CLayoutParameterInt::Deserialize(CByteBuffer *byteBuffer)
{
	*value = byteBuffer->getInt();
	return true;
}

//

CLayoutParameterFloat::CLayoutParameterFloat(const char *name, float *value)
: CLayoutParameter(name)
{
	this->value = value;
	this->minValue = 0.1f;
	this->maxValue = 50.0f;
	this->step = 0.0f;
	this->step_fast = 0.0f;
	this->format = "%.2f";
	this->flags = 0;
}

CLayoutParameterFloat::CLayoutParameterFloat(const char *name, bool isHidden, float *value)
: CLayoutParameter(name, isHidden)
{
	this->value = value;
	this->minValue = 0.1f;
	this->maxValue = 50.0f;
	this->step = 0.0f;
	this->step_fast = 0.0f;
	this->format = "%.2f";
	this->flags = 0;
}

CLayoutParameterFloat::CLayoutParameterFloat(const char *name, float *value, float minValue, float maxValue)
: CLayoutParameter(name)
{
	this->value = value;
	this->minValue = minValue;
	this->maxValue = maxValue;
	this->step = 0.0f;
	this->step_fast = 0.0f;
	this->format = "%.2f";
	this->flags = 0;
}

CLayoutParameterFloat::CLayoutParameterFloat(const char *name, float *value, float minValue, float maxValue, float step, float step_fast, const char* format, ImGuiInputTextFlags flags)
: CLayoutParameter(name)
{
	this->value = value;
	this->minValue = minValue;
	this->maxValue = maxValue;
	this->step = step;
	this->step_fast = step_fast;
	this->format = format;
	this->flags = flags;
}

// returns if parameter has changed
bool CLayoutParameterFloat::RenderImGui()
{
//	ImGui::Text(name);
//	ImGui::SameLine();
	
	char *buf = SYS_GetCharBuf();
	sprintf(buf, "%s##float2", name);
	if (ImGui::SliderFloat(buf, value, minValue, maxValue, format, flags))
	{
		guiMain->StoreLayoutInSettingsAtEndOfThisFrame();
		return true;
	}

	SYS_ReleaseCharBuf(buf);
	return false;
}

void CLayoutParameterFloat::Serialize(CByteBuffer *byteBuffer)
{
	byteBuffer->PutFloat(*value);
}

bool CLayoutParameterFloat::Deserialize(CByteBuffer *byteBuffer)
{
	*value = byteBuffer->GetFloat();
	return true;
}

//
CLayoutParameterDouble::CLayoutParameterDouble(const char *name, double *value)
: CLayoutParameter(name)
{
	this->value = value;
	this->minValue = 0.1f;
	this->maxValue = 50.0f;
	this->step = 0.0f;
	this->step_fast = 0.0f;
	this->format = "%.2f";
	this->flags = 0;
}

CLayoutParameterDouble::CLayoutParameterDouble(const char *name, bool isHidden, double *value)
: CLayoutParameter(name, isHidden)
{
	this->value = value;
	this->minValue = 0.1f;
	this->maxValue = 50.0f;
	this->step = 0.0f;
	this->step_fast = 0.0f;
	this->format = "%.2f";
	this->flags = 0;
}

CLayoutParameterDouble::CLayoutParameterDouble(const char *name, double *value, float minValue, float maxValue)
: CLayoutParameter(name)
{
	this->value = value;
	this->minValue = minValue;
	this->maxValue = maxValue;
	this->step = 0.0f;
	this->step_fast = 0.0f;
	this->format = "%.2f";
	this->flags = 0;
}

CLayoutParameterDouble::CLayoutParameterDouble(const char *name, double *value, float minValue, float maxValue, float step, float step_fast, const char* format, ImGuiInputTextFlags flags)
: CLayoutParameter(name)
{
	this->value = value;
	this->minValue = minValue;
	this->maxValue = maxValue;
	this->step = step;
	this->step_fast = step_fast;
	this->format = format;
	this->flags = flags;
}

// returns if parameter has changed
bool CLayoutParameterDouble::RenderImGui()
{
//	ImGui::Text(name);
//	ImGui::SameLine();
	
	char *buf = SYS_GetCharBuf();
	sprintf(buf, "%s##float2", name);
	float v = (float)*value;
	if (ImGui::SliderFloat(buf, &v, minValue, maxValue, format, flags))
	{
		*value = (double)v;
		guiMain->StoreLayoutInSettingsAtEndOfThisFrame();
		return true;
	}
	*value = (double)v;

	SYS_ReleaseCharBuf(buf);
	return false;
}

void CLayoutParameterDouble::Serialize(CByteBuffer *byteBuffer)
{
	byteBuffer->PutFloat(*value);
}

bool CLayoutParameterDouble::Deserialize(CByteBuffer *byteBuffer)
{
	*value = byteBuffer->GetFloat();
	return true;
}

//
CLayoutParameterCombo::CLayoutParameterCombo(const char *name, int *selectedItem, const char **items, int itemsCount)
: CLayoutParameter(name)
{
	this->selectedItem = selectedItem;
	this->items = items;
	this->itemsCount = itemsCount;
	this->flags = ImGuiComboFlags_None;
}

CLayoutParameterCombo::CLayoutParameterCombo(const char *name, int *selectedItem, const char **items, int itemsCount, ImGuiComboFlags flags)
: CLayoutParameter(name)
{
	this->selectedItem = selectedItem;
	this->items = items;
	this->itemsCount = itemsCount;
	this->flags = flags;
}

bool CLayoutParameterCombo::RenderImGui()
{
	bool parameterChanged = false;
	if (ImGui::BeginCombo(name, items[*selectedItem], flags))
	{
		for (int n = 0; n < itemsCount; n++)
		{
			const bool isSelected = (*selectedItem == n);
			if (ImGui::Selectable(items[n], isSelected))
			{
				*selectedItem = n;
				parameterChanged = true;
				guiMain->StoreLayoutInSettingsAtEndOfThisFrame();
			}

			// Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	return parameterChanged;
}

void CLayoutParameterCombo::Serialize(CByteBuffer *byteBuffer)
{
	byteBuffer->PutU32(*selectedItem);
}

bool CLayoutParameterCombo::Deserialize(CByteBuffer *byteBuffer)
{
	*selectedItem = byteBuffer->GetU32();
	return true;
}

//

CLayoutParameterPath::CLayoutParameterPath(const char *name,
										   LayoutParameterPathMode layoutParameterPathMode,
										   char **pathStrPointer)
: CLayoutParameter(name)
{
	this->layoutParameterPathMode = layoutParameterPathMode;
	this->pathStrPointer = pathStrPointer;
}

bool CLayoutParameterPath::RenderImGui()
{
	if (layoutParameterPathMode == LayoutParameterPathMode::LayoutParameterPathModeOpen)
	{
		char *buf = SYS_GetCharBuf();
		sprintf(buf, "%s##CLayoutParameterPath", name);
//		LOGD("name=%s %x", name, name);
		if (ImGui::Button(buf))
		{
//			SYS_DialogOpenFile(this, <#std::list<CSlrString *> *extensions#>, <#CSlrString *defaultFolder#>, <#CSlrString *windowTitle#>)
			SYS_DialogOpenFile(this, NULL, NULL, NULL);
		}

		if (*pathStrPointer != NULL)
		{
			ImGui::SameLine();
			ImGui::Text(*pathStrPointer);
		}

	}
	return false;
}

void CLayoutParameterPath::SystemDialogFileOpenSelected(CSlrString *path)
{
	// TODO: CLayoutParameterPath
	LOGD("SystemDialogFileOpenSelected");
	path->DebugPrint();
}

void CLayoutParameterPath::SystemDialogFileOpenCancelled()
{
}

void CLayoutParameterPath::SystemDialogFileSaveSelected(CSlrString *path)
{
}

void CLayoutParameterPath::SystemDialogFileSaveCancelled()
{
}

void CLayoutParameterPath::SystemDialogPickFolderSelected(CSlrString *path)
{
}

void CLayoutParameterPath::SystemDialogPickFolderCancelled()
{
}

void CLayoutParameterPath::Serialize(CByteBuffer *byteBuffer)
{
}

bool CLayoutParameterPath::Deserialize(CByteBuffer *byteBuffer)
{
	return true;
}

