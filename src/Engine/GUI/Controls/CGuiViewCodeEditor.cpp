#include "CGuiViewCodeEditor.h"
#include "CGuiFontManager.h"
#include "imgui.h"

CGuiViewCodeEditor::CGuiViewCodeEditor(const char *name, float posX, float posY,
									   float posZ, float sizeX, float sizeY)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	imGuiNoWindowPadding = false;
	imGuiNoScrollbar = true;   // the editor scrolls itself

	// Defaults a host overrides right after construction. C++ rather than no
	// language: plain grey text reads as "highlighting is broken", not as "no
	// language was chosen".
	editor.SetLanguage(TextEditor::Language::Cpp());
	editor.SetPalette(TextEditor::GetDarkPalette());
	editor.SetTabSize(4);
	editor.SetShowLineNumbersEnabled(true);

	// NO font lookup here. gGuiFontManager.fontMono is nullptr until
	// LoadMarkdownFonts() runs, and that runs AFTER views are constructed. A
	// constructor-time default would resolve to "current font" and no test
	// that runs after fonts exist would ever notice. See GetFont().
}

ImFont *CGuiViewCodeEditor::GetFont()
{
	// Resolved on every call, deliberately: the answer changes once between
	// construction and the first frame, when the atlas is built.
	if (fontExplicitlySet)
		return font;
	return gGuiFontManager.fontMono;   // may still be nullptr = current font
}

void CGuiViewCodeEditor::RenderImGui()
{
	PreRenderImGui();
	RenderToolbar();
	ImFont *f = GetFont();
	if (f != nullptr) ImGui::PushFont(f);
	editor.Render("##codeeditor", ImVec2());   // ImVec2() fills the available region
	if (f != nullptr) ImGui::PopFont();
	PostRenderImGui();
}

void CGuiViewCodeEditor::SetText(const char *text)                    { editor.SetText(text != NULL ? text : ""); }
std::string CGuiViewCodeEditor::GetText()                             { return editor.GetText(); }
void CGuiViewCodeEditor::SetLanguage(const TextEditor::Language *l)   { editor.SetLanguage(l); }
const TextEditor::Language *CGuiViewCodeEditor::GetLanguage()         { return editor.GetLanguage(); }
void CGuiViewCodeEditor::SetReadOnly(bool readOnly)                   { editor.SetReadOnlyEnabled(readOnly); }
void CGuiViewCodeEditor::SetTabSize(int tabSize)                      { editor.SetTabSize((size_t)tabSize); }
void CGuiViewCodeEditor::SetShowLineNumbers(bool show)                { editor.SetShowLineNumbersEnabled(show); }
void CGuiViewCodeEditor::SetPalette(const TextEditor::Palette &p)     { editor.SetPalette(p); }
void CGuiViewCodeEditor::SetFont(ImFont *f)                           { font = f; fontExplicitlySet = true; }
