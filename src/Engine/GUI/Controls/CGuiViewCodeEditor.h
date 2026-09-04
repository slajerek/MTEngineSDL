#ifndef _CGuiViewCodeEditor_h_
#define _CGuiViewCodeEditor_h_

#include "CGuiView.h"
#include "TextEditor.h"
#include <string>

struct ImFont;

// A code editor in a window: a thin CGuiView over the vendored TextEditor
// widget (src/Engine/Libs/imgui_textedit), for a host that wants a standalone
// editor rather than one embedded in its own layout.
//
// THIN IS THE WHOLE SPECIFICATION. It owns a TextEditor, fills its window with
// it, forwards configuration, and adds no policy of its own. Anything it does
// not forward is reached through GetEditor() rather than by growing this
// class. It has exactly one extension point, RenderToolbar(): a subclass draws
// a row above the editor without this class knowing what a toolbar is.
//
// A host that wants the editor INSIDE its own window uses TextEditor directly
// -- DummyApp's Shader Toy does, which is what keeps this wrapper honest about
// being optional.
class CGuiViewCodeEditor : public CGuiView
{
public:
	CGuiViewCodeEditor(const char *name, float posX, float posY, float posZ,
					   float sizeX, float sizeY);
	virtual void RenderImGui() override;

	// THE ONE EXTENSION POINT. Called between PreRenderImGui() and the editor,
	// empty by default.
	virtual void RenderToolbar() {}

	void SetText(const char *text);
	std::string GetText();
	void SetLanguage(const TextEditor::Language *language);
	const TextEditor::Language *GetLanguage();
	void SetReadOnly(bool readOnly);
	void SetTabSize(int tabSize);
	void SetShowLineNumbers(bool show);
	void SetPalette(const TextEditor::Palette &palette);

	// nullptr = render in whatever font is current. Until SetFont() is called
	// the wrapper uses the engine's JetBrains Mono IF it is loaded -- resolved
	// at RENDER time, not construction time, because views are constructed
	// before fonts exist: an application builds its views and only then calls
	// LoadMarkdownFonts(), so a constructor-time lookup would find nullptr and
	// silently mean "current font" forever.
	void SetFont(ImFont *font);
	ImFont *GetFont();   // the EFFECTIVE font: explicit, else fontMono, else nullptr

	TextEditor &GetEditor() { return editor; }

private:
	TextEditor editor;
	ImFont *font = nullptr;
	bool fontExplicitlySet = false;
};

#endif
