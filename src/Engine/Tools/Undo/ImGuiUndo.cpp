#include "ImGuiUndo.h"
#include "CUndoFieldChange.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <unordered_map>
#include <array>

static std::unordered_map<ImGuiID, int> s_oldInts;
static std::unordered_map<ImGuiID, float> s_oldFloats;
static std::unordered_map<ImGuiID, std::string> s_oldStrings;
static std::unordered_map<ImGuiID, std::array<float, 4>> s_oldColors;

namespace ImGuiUndo
{

bool InputInt(CUndoManager* mgr, const char* label, int* value)
{
    int current = *value;
    bool changed = ImGui::InputInt(label, value);
    if (ImGui::IsItemActivated())
    {
        s_oldInts[ImGui::GetItemID()] = current;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        ImGuiID id = ImGui::GetItemID();
        auto it = s_oldInts.find(id);
        if (it != s_oldInts.end() && it->second != *value)
        {
            mgr->Push(std::make_unique<CUndoFieldChange<int>>(value, it->second, *value));
        }
        s_oldInts.erase(id);
    }
    return changed;
}

bool InputFloat(CUndoManager* mgr, const char* label, float* value, float step, float step_fast, const char* format)
{
    float current = *value;
    bool changed = ImGui::InputFloat(label, value, step, step_fast, format);
    if (ImGui::IsItemActivated())
    {
        s_oldFloats[ImGui::GetItemID()] = current;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        ImGuiID id = ImGui::GetItemID();
        auto it = s_oldFloats.find(id);
        if (it != s_oldFloats.end() && it->second != *value)
        {
            mgr->Push(std::make_unique<CUndoFieldChange<float>>(value, it->second, *value));
        }
        s_oldFloats.erase(id);
    }
    return changed;
}

bool InputText(CUndoManager* mgr, const char* label, std::string* str, ImGuiInputTextFlags flags)
{
    std::string current = *str;
    bool changed = ImGui::InputText(label, str, flags);
    if (ImGui::IsItemActivated())
    {
        s_oldStrings[ImGui::GetItemID()] = current;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        ImGuiID id = ImGui::GetItemID();
        auto it = s_oldStrings.find(id);
        if (it != s_oldStrings.end() && it->second != *str)
        {
            mgr->Push(std::make_unique<CUndoFieldChange<std::string>>(str, it->second, *str));
        }
        s_oldStrings.erase(id);
    }
    return changed;
}

bool InputTextMultiline(CUndoManager* mgr, const char* label, std::string* str, const ImVec2& size, ImGuiInputTextFlags flags)
{
    std::string current = *str;
    bool changed = ImGui::InputTextMultiline(label, str, size, flags);
    if (ImGui::IsItemActivated())
    {
        s_oldStrings[ImGui::GetItemID()] = current;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        ImGuiID id = ImGui::GetItemID();
        auto it = s_oldStrings.find(id);
        if (it != s_oldStrings.end() && it->second != *str)
        {
            mgr->Push(std::make_unique<CUndoFieldChange<std::string>>(str, it->second, *str));
        }
        s_oldStrings.erase(id);
    }
    return changed;
}

bool Checkbox(CUndoManager* mgr, const char* label, bool* value)
{
    bool oldVal = *value;
    bool changed = ImGui::Checkbox(label, value);
    if (changed && oldVal != *value)
    {
        mgr->Push(std::make_unique<CUndoFieldChange<bool>>(value, oldVal, *value));
    }
    return changed;
}

bool CheckboxFlags(CUndoManager* mgr, const char* label, int* flags, int flagValue)
{
    int oldVal = *flags;
    bool changed = ImGui::CheckboxFlags(label, flags, flagValue);
    if (changed && oldVal != *flags)
    {
        mgr->Push(std::make_unique<CUndoFieldChange<int>>(flags, oldVal, *flags));
    }
    return changed;
}

bool Combo(CUndoManager* mgr, const char* label, int* currentItem, const char* const items[], int itemsCount)
{
    int oldVal = *currentItem;
    bool changed = ImGui::Combo(label, currentItem, items, itemsCount);
    if (changed && oldVal != *currentItem)
    {
        mgr->Push(std::make_unique<CUndoFieldChange<int>>(currentItem, oldVal, *currentItem));
    }
    return changed;
}

bool ColorPicker4(CUndoManager* mgr, const char* label, float col[4], ImGuiColorEditFlags flags)
{
    float current[4] = { col[0], col[1], col[2], col[3] };
    bool changed = ImGui::ColorPicker4(label, col, flags);
    if (ImGui::IsItemActivated())
    {
        ImGuiID id = ImGui::GetItemID();
        s_oldColors[id] = { current[0], current[1], current[2], current[3] };
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        ImGuiID id = ImGui::GetItemID();
        auto it = s_oldColors.find(id);
        if (it != s_oldColors.end())
        {
            auto& old = it->second;
            if (old[0] != col[0] || old[1] != col[1] || old[2] != col[2] || old[3] != col[3])
            {
                ImVec4* colVec = reinterpret_cast<ImVec4*>(col);
                ImVec4 oldVec = { old[0], old[1], old[2], old[3] };
                mgr->Push(std::make_unique<CUndoFieldChange<ImVec4>>(colVec, oldVec, *colVec));
            }
            s_oldColors.erase(id);
        }
    }
    return changed;
}

} // namespace ImGuiUndo
