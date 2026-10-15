#include "CGuiViewMarkdownEditor.h"
#include "CGuiFontManager.h"
#include "imgui.h"

CGuiViewMarkdownEditor::CGuiViewMarkdownEditor(const char *name, float posX, float posY,
											   float posZ, float sizeX, float sizeY)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	imGuiNoWindowPadding = false;
	imGuiNoScrollbar = true;   // the editor scrolls itself
}

void CGuiViewMarkdownEditor::RenderImGui()
{
	PreRenderImGui();
	RenderToolbar();
	if (!fontsExplicitlySet && gGuiFontManager.markdownFontsLoaded)
	{
		CMarkdownEditorFonts f;
		f.regular = gGuiFontManager.fontRegular;
		f.bold = gGuiFontManager.fontBold;
		f.italic = gGuiFontManager.fontItalic;
		f.boldItalic = gGuiFontManager.fontBoldItalic;
		f.mono = gGuiFontManager.fontMono;
		editor.SetFonts(f);
		fontsExplicitlySet = true;   // resolved once; the atlas does not change under us
	}
	editor.Render("##markdowneditor", ImVec2());
	PostRenderImGui();
}

void CGuiViewMarkdownEditor::SetSource(const char *markdown)      { editor.SetSource(markdown != NULL ? markdown : ""); }
std::string CGuiViewMarkdownEditor::GetSource() const             { return editor.GetSource(); }
void CGuiViewMarkdownEditor::SetReadOnly(bool readOnly)           { editor.SetReadOnly(readOnly); }
void CGuiViewMarkdownEditor::SetFonts(const CMarkdownEditorFonts &f) { editor.SetFonts(f); fontsExplicitlySet = true; }
