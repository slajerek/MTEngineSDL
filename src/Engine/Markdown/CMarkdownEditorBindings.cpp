#include "CMarkdownEditorBindings.h"
#include "CSlrKeyboardShortcuts.h"
#include "SYS_KeyCodes.h"
#include "CGuiMain.h"

namespace
{
	struct DefaultBinding
	{
		EMarkdownEditorCommand command;
		const char *key;        // translation key
		const char *label;      // English label
		const char *name;       // CSlrKeyboardShortcut name: unique, stable (its hash keys the stored binding)
		int32_t keyCode;
		bool shift, alt, control, super;
	};

	const DefaultBinding kDefaults[] = {
		{EMarkdownEditorCommand::Bold, "markdown.editor.command.bold", "Bold", "Markdown editor: Bold", 'b', false, false, true, false},
		{EMarkdownEditorCommand::Italic, "markdown.editor.command.italic", "Italic", "Markdown editor: Italic", 'i', false, false, true, false},
		{EMarkdownEditorCommand::InsertLink, "markdown.editor.command.insert_link", "Insert link", "Markdown editor: Insert link", 'k', false, false, true, false},
		{EMarkdownEditorCommand::Heading1, "markdown.editor.command.heading1", "Heading 1", "Markdown editor: Heading 1", '1', false, false, true, false},
		{EMarkdownEditorCommand::Heading2, "markdown.editor.command.heading2", "Heading 2", "Markdown editor: Heading 2", '2', false, false, true, false},
		{EMarkdownEditorCommand::Heading3, "markdown.editor.command.heading3", "Heading 3", "Markdown editor: Heading 3", '3', false, false, true, false},
		{EMarkdownEditorCommand::Heading4, "markdown.editor.command.heading4", "Heading 4", "Markdown editor: Heading 4", '4', false, false, true, false},
		{EMarkdownEditorCommand::Heading5, "markdown.editor.command.heading5", "Heading 5", "Markdown editor: Heading 5", '5', false, false, true, false},
		{EMarkdownEditorCommand::Heading6, "markdown.editor.command.heading6", "Heading 6", "Markdown editor: Heading 6", '6', false, false, true, false},
		{EMarkdownEditorCommand::Paragraph, "markdown.editor.command.paragraph", "Normal paragraph", "Markdown editor: Normal paragraph", '0', true, false, true, false},
		{EMarkdownEditorCommand::Undo, "markdown.editor.command.undo", "Undo", "Markdown editor: Undo", 'z', false, false, true, false},
		{EMarkdownEditorCommand::Redo, "markdown.editor.command.redo", "Redo", "Markdown editor: Redo", 'z', true, false, true, false},
		{EMarkdownEditorCommand::RedoAlternate, "markdown.editor.command.redo_alternate", "Redo (alternate)", "Markdown editor: Redo (alternate)", 'y', false, false, true, false},
		{EMarkdownEditorCommand::ToggleTaskOrInsertRow, "markdown.editor.command.toggle_task_or_insert_row", "Toggle task / insert table row", "Markdown editor: Toggle task or insert row", MTKEY_ENTER, false, false, true, false},
		{EMarkdownEditorCommand::Find, "markdown.editor.command.find", "Find", "Markdown editor: Find", 'f', false, false, true, false},
		{EMarkdownEditorCommand::FindNext, "markdown.editor.command.find_next", "Find next", "Markdown editor: Find next", MTKEY_F3, false, false, false, false},
		{EMarkdownEditorCommand::FindPrevious, "markdown.editor.command.find_previous", "Find previous", "Markdown editor: Find previous", MTKEY_F3, true, false, false, false},
		{EMarkdownEditorCommand::TableMoveRowUp, "markdown.editor.command.table_move_row_up", "Move table row up", "Markdown editor: Move table row up", MTKEY_ARROW_UP, true, true, false, false},
		{EMarkdownEditorCommand::TableMoveRowDown, "markdown.editor.command.table_move_row_down", "Move table row down", "Markdown editor: Move table row down", MTKEY_ARROW_DOWN, true, true, false, false},
		{EMarkdownEditorCommand::TableMoveColumnLeft, "markdown.editor.command.table_move_column_left", "Move table column left", "Markdown editor: Move table column left", MTKEY_ARROW_LEFT, true, true, false, false},
		{EMarkdownEditorCommand::TableMoveColumnRight, "markdown.editor.command.table_move_column_right", "Move table column right", "Markdown editor: Move table column right", MTKEY_ARROW_RIGHT, true, true, false, false},
	};
	static_assert(sizeof(kDefaults) / sizeof(kDefaults[0]) == (size_t)EMarkdownEditorCommand::Count, "one default per command");

	const DefaultBinding &DefaultFor(EMarkdownEditorCommand command)
	{
		return kDefaults[(int)command];
	}
}

CMarkdownEditorBindings *CMarkdownEditorBindings::GetDefault()
{
	// Never destroyed: guiMain's registry holds pointers to its shortcuts for
	// the life of the process.
	static CMarkdownEditorBindings *instance = new CMarkdownEditorBindings();
	return instance;
}

CMarkdownEditorBindings::CMarkdownEditorBindings()
{
	for (int i = 0; i < (int)EMarkdownEditorCommand::Count; i++)
	{
		const DefaultBinding &d = kDefaults[i];
		shortcuts[i] = new CSlrKeyboardShortcut(kZone, d.name, d.keyCode, d.shift, d.alt, d.control, d.super, nullptr);
	}
}

CMarkdownEditorBindings::~CMarkdownEditorBindings()
{
	for (int i = 0; i < (int)EMarkdownEditorCommand::Count; i++)
	{
		if (registered && guiMain != nullptr) guiMain->RemoveKeyboardShortcut(shortcuts[i]);
		delete shortcuts[i];
	}
}

CSlrKeyboardShortcut *CMarkdownEditorBindings::GetShortcut(EMarkdownEditorCommand command) const
{
	if ((int)command < 0 || command >= EMarkdownEditorCommand::Count) return nullptr;
	return shortcuts[(int)command];
}

ImGuiKeyChord CMarkdownEditorBindings::GetChord(EMarkdownEditorCommand command) const
{
	const CSlrKeyboardShortcut *s = GetShortcut(command);
	if (s == nullptr || s->keyCode <= 0) return 0;
	const ImGuiKey key = KeyCodeToImGuiKey(s->keyCode);
	if (key == ImGuiKey_None) return 0;
	ImGuiKeyChord chord = key;
	if (s->isControl) chord |= ImGuiMod_Ctrl;
	if (s->isShift) chord |= ImGuiMod_Shift;
	if (s->isAlt) chord |= ImGuiMod_Alt;
	if (s->isSuper) chord |= ImGuiMod_Super;
	return chord;
}

void CMarkdownEditorBindings::ResetToDefaults()
{
	for (int i = 0; i < (int)EMarkdownEditorCommand::Count; i++)
	{
		const DefaultBinding &d = kDefaults[i];
		const bool wasRegistered = registered && guiMain != nullptr;
		// The registry indexes by key: re-add so its index follows the binding.
		if (wasRegistered) guiMain->RemoveKeyboardShortcut(shortcuts[i]);
		shortcuts[i]->SetKeyCode(d.keyCode, d.shift, d.alt, d.control, d.super);
		if (wasRegistered) guiMain->AddKeyboardShortcut(shortcuts[i]);
	}
}

void CMarkdownEditorBindings::RegisterWithGuiMain()
{
	if (registered || guiMain == nullptr) return;
	for (int i = 0; i < (int)EMarkdownEditorCommand::Count; i++) guiMain->AddKeyboardShortcut(shortcuts[i]);
	registered = true;
}

const char *CMarkdownEditorBindings::GetCommandKey(EMarkdownEditorCommand command)
{
	if (command >= EMarkdownEditorCommand::Count) return "";
	return DefaultFor(command).key;
}

const char *CMarkdownEditorBindings::GetCommandFallbackLabel(EMarkdownEditorCommand command)
{
	if (command >= EMarkdownEditorCommand::Count) return "";
	return DefaultFor(command).label;
}

ImGuiKey CMarkdownEditorBindings::KeyCodeToImGuiKey(int32_t keyCode)
{
	if (keyCode >= 'a' && keyCode <= 'z') return (ImGuiKey)(ImGuiKey_A + (keyCode - 'a'));
	if (keyCode >= 'A' && keyCode <= 'Z') return (ImGuiKey)(ImGuiKey_A + (keyCode - 'A'));
	if (keyCode >= '0' && keyCode <= '9') return (ImGuiKey)(ImGuiKey_0 + (keyCode - '0'));
	switch (keyCode)
	{
		case MTKEY_ENTER: return ImGuiKey_Enter;
		case MTKEY_TAB: return ImGuiKey_Tab;
		case MTKEY_BACKSPACE: return ImGuiKey_Backspace;
		case MTKEY_SPACEBAR: return ImGuiKey_Space;
		case MTKEY_ESC: return ImGuiKey_Escape;
		case MTKEY_DELETE: return ImGuiKey_Delete;
		case MTKEY_INSERT: return ImGuiKey_Insert;
		case MTKEY_HOME: return ImGuiKey_Home;
		case MTKEY_END: return ImGuiKey_End;
		case MTKEY_PAGE_UP: return ImGuiKey_PageUp;
		case MTKEY_PAGE_DOWN: return ImGuiKey_PageDown;
		case MTKEY_ARROW_LEFT: return ImGuiKey_LeftArrow;
		case MTKEY_ARROW_RIGHT: return ImGuiKey_RightArrow;
		case MTKEY_ARROW_UP: return ImGuiKey_UpArrow;
		case MTKEY_ARROW_DOWN: return ImGuiKey_DownArrow;
		case MTKEY_F1: return ImGuiKey_F1;
		case MTKEY_F2: return ImGuiKey_F2;
		case MTKEY_F3: return ImGuiKey_F3;
		case MTKEY_F4: return ImGuiKey_F4;
		case MTKEY_F5: return ImGuiKey_F5;
		case MTKEY_F6: return ImGuiKey_F6;
		case MTKEY_F7: return ImGuiKey_F7;
		case MTKEY_F8: return ImGuiKey_F8;
		case MTKEY_F9: return ImGuiKey_F9;
		case MTKEY_F10: return ImGuiKey_F10;
		case MTKEY_F11: return ImGuiKey_F11;
		case MTKEY_F12: return ImGuiKey_F12;
		case '-': return ImGuiKey_Minus;
		case '=': return ImGuiKey_Equal;
		case '[': return ImGuiKey_LeftBracket;
		case ']': return ImGuiKey_RightBracket;
		case ';': return ImGuiKey_Semicolon;
		case '\'': return ImGuiKey_Apostrophe;
		case ',': return ImGuiKey_Comma;
		case '.': return ImGuiKey_Period;
		case '/': return ImGuiKey_Slash;
		case '\\': return ImGuiKey_Backslash;
		case '`': return ImGuiKey_GraveAccent;
		default: break;
	}
	return ImGuiKey_None;
}
