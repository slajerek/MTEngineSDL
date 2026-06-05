#pragma once

struct ImFont;

// Central font manager for engine GUI.
// Replaces VID_Fonts globals and adds optional markdown font variants.
//
// Markdown fonts (Inter Regular/Bold/Italic/BoldItalic + JetBrains Mono) are
// embedded as compressed C arrays in src/Embedded/Fonts/ and loaded on demand.
// Call LoadMarkdownFonts() before ImGui font atlas build if you want markdown
// rendering with proper typography. Apps that don't need it pay zero cost.
//
// Usage:
//   // In engine init (before atlas build):
//   gGuiFontManager.defaultFontPath = "path/to/font.ttf";
//   gGuiFontManager.defaultFontSize = 15.0f;
//   gGuiFontManager.LoadMarkdownFonts();  // optional
//
//   // After atlas build, access fonts:
//   ImFont* bold = gGuiFontManager.fontBold;

class CGuiFontManager
{
public:
    // Default font settings (replaces VID_Fonts globals)
    const char* defaultFontPath       = nullptr;
    float       defaultFontSize       = 13.0f;
    int         defaultFontOversampling = 4;

    // Markdown font variants — null until LoadMarkdownFonts() is called
    ImFont* fontRegular    = nullptr;
    ImFont* fontBold       = nullptr;
    ImFont* fontItalic     = nullptr;
    ImFont* fontBoldItalic = nullptr;
    ImFont* fontMono       = nullptr;

    bool markdownFontsLoaded = false;

    // Call before ImGui font atlas build to load embedded markdown fonts.
    // Safe to call multiple times — subsequent calls are no-ops.
    // monoSize: size for the mono font; 0 = same as size
    void LoadMarkdownFonts(float size = 15.0f, float monoSize = 0.0f);
};

extern CGuiFontManager gGuiFontManager;

// Backward-compatible aliases for code that still uses VID_Fonts globals.
// Prefer accessing gGuiFontManager directly in new code.
#define gDefaultFontPath         gGuiFontManager.defaultFontPath
#define gDefaultFontSize         gGuiFontManager.defaultFontSize
#define gDefaultFontOversampling gGuiFontManager.defaultFontOversampling
