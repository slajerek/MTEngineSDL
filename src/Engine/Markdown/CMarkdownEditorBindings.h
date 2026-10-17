#pragma once

// CMarkdownEditorBindings: the live Markdown editor's remappable commands as
// CSlrKeyboardShortcut objects, the engine's keyboard-shortcut currency.
//
// Each command is one shortcut in its own zone (kZone), registered with
// guiMain->keyboardShortcuts the first time an editor renders, so a host's
// shortcut screen lists, remaps and stores them with the rest -- through
// CSlrKeyboardShortcutsZone::StoreToByteBuffer / LoadFromByteBuffer, keyed by
// the shortcut's name hash. CGuiMain dispatches only the GLOBAL zone itself,
// so these never fire outside a focused editor: the editor polls them each
// frame, turning the current binding into an ImGui key chord. A rebinding
// (SetKeyCode) takes effect on the next frame.
//
// Defaults are the product spec's (Ctrl is Cmd on macOS, as in ImGui and in
// CSlrKeyboardShortcut::isControl). Caret movement, deletion, Enter, Tab and
// the clipboard keys are not commands here: they follow the platform and
// stay fixed.

#include "imgui.h"
#include <cstdint>

class CSlrKeyboardShortcut;

enum class EMarkdownEditorCommand : uint8_t
{
	Bold,
	Italic,
	InsertLink,
	Heading1,
	Heading2,
	Heading3,
	Heading4,
	Heading5,
	Heading6,
	Paragraph,
	Undo,
	Redo,
	RedoAlternate,
	ToggleTaskOrInsertRow,   // a task item: flip its box; a table cell: add a row below
	Find,
	FindNext,
	FindPrevious,
	TableMoveRowUp,
	TableMoveRowDown,
	TableMoveColumnLeft,
	TableMoveColumnRight,
	Strikethrough,
	InlineCode,
	BulletList,
	NumberedList,
	TaskList,
	Quote,
	Replace,
	Underline,
	Count
};

class CMarkdownEditorBindings
{
public:
	// The zone the editor's shortcuts live in ('MDED').
	static constexpr uint32_t kZone = 0x4D444544u;

	// The process-wide set every editor uses unless given another.
	static CMarkdownEditorBindings *GetDefault();

	CMarkdownEditorBindings();
	~CMarkdownEditorBindings();
	CMarkdownEditorBindings(const CMarkdownEditorBindings &) = delete;
	CMarkdownEditorBindings &operator=(const CMarkdownEditorBindings &) = delete;

	CSlrKeyboardShortcut *GetShortcut(EMarkdownEditorCommand command) const;
	// The command's binding as an ImGui chord; 0 when unbound or the key has
	// no ImGui equivalent.
	ImGuiKeyChord GetChord(EMarkdownEditorCommand command) const;
	// Back to the defaults.
	void ResetToDefaults();

	// Add every shortcut to guiMain's keyboard-shortcut registry (once; does
	// nothing without a guiMain). Editors call it when they first render.
	void RegisterWithGuiMain();
	bool IsRegistered() const { return registered; }

	// A stable id for translation tables and the English label, e.g.
	// "markdown.editor.command.bold" / "Bold".
	static const char *GetCommandKey(EMarkdownEditorCommand command);
	static const char *GetCommandFallbackLabel(EMarkdownEditorCommand command);

	// An engine key code (SYS_KeyCodes.h: ASCII for printable keys, SDLK_*
	// for the rest) as an ImGuiKey; ImGuiKey_None when there is none.
	static ImGuiKey KeyCodeToImGuiKey(int32_t keyCode);

private:
	CSlrKeyboardShortcut *shortcuts[(int)EMarkdownEditorCommand::Count];
	bool registered = false;
};
