#ifndef _CGuiViewMarkdownEditor_h_
#define _CGuiViewMarkdownEditor_h_

#include "CGuiView.h"
#include "CMarkdownLiveEditor.h"
#include <string>

// A live Markdown editor in a window: a thin CGuiView over CMarkdownLiveEditor,
// in the shape of CGuiViewCodeEditor. It owns an editor, fills its window with
// it, and resolves fonts at RENDER time from CGuiFontManager (views are
// constructed before fonts exist), unless the host set them explicitly.
// Everything else is reached through GetEditor().
class CGuiViewMarkdownEditor : public CGuiView
{
public:
	CGuiViewMarkdownEditor(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual void RenderImGui() override;

	// A row above the editor; empty by default.
	virtual void RenderToolbar() {}

	void SetSource(const char *markdown);
	std::string GetSource() const;
	void SetReadOnly(bool readOnly);

	// Explicit fonts; until called, the engine's Markdown fonts are used when
	// loaded (gGuiFontManager.LoadMarkdownFonts()), else the current font.
	void SetFonts(const CMarkdownEditorFonts &fonts);

	CMarkdownLiveEditor &GetEditor() { return editor; }

private:
	CMarkdownLiveEditor editor;
	bool fontsExplicitlySet = false;
};

#endif
