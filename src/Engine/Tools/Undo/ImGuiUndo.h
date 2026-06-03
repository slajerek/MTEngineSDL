#pragma once

#include "CUndoManager.h"
#include "CUndoFieldChange.h"
#include "imgui.h"
#include <string>
#include <memory>

namespace ImGuiUndo
{
    // Continuous-edit widgets (track activate/deactivate)
    bool InputInt(CUndoManager* mgr, const char* label, int* value);
    bool InputFloat(CUndoManager* mgr, const char* label, float* value, float step = 0.0f, float step_fast = 0.0f, const char* format = "%.3f");
    bool InputText(CUndoManager* mgr, const char* label, std::string* str, ImGuiInputTextFlags flags = 0);
    bool InputTextMultiline(CUndoManager* mgr, const char* label, std::string* str, const ImVec2& size = ImVec2(0, 0), ImGuiInputTextFlags flags = 0);

    // Instant-commit widgets (undo pushed immediately on change)
    bool Checkbox(CUndoManager* mgr, const char* label, bool* value);
    bool CheckboxFlags(CUndoManager* mgr, const char* label, int* flags, int flagValue);
    bool Combo(CUndoManager* mgr, const char* label, int* currentItem, const char* const items[], int itemsCount);

    // Generic helper for manual undo push (used with BeginCombo/Selectable pattern)
    template<typename T>
    inline void PushFieldChange(CUndoManager* mgr, T* field, T oldVal, T newVal)
    {
        if (oldVal != newVal)
        {
            mgr->Push(std::make_unique<CUndoFieldChange<T>>(field, std::move(oldVal), std::move(newVal)));
        }
    }

    // Color — for contiguous float[4] arrays
    bool ColorPicker4(CUndoManager* mgr, const char* label, float col[4], ImGuiColorEditFlags flags = 0);
}
