#pragma once

struct ImGuiTestEngine;

// imgui_test_engine tests for CMarkdownLiveEditor through CGuiViewMarkdownEditor.
// A host registers them next to its own: RegisterMarkdownEditorTests(engine).
// The tests create one editor view on first use and add it to guiMain; they
// drive it with real mouse and keyboard events, so what passes here is what a
// user gets.
void RegisterMarkdownEditorTests(ImGuiTestEngine *engine);
