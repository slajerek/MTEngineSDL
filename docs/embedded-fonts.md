# Embedded Fonts

MTEngineSDL ships embedded font data for markdown rendering in ImGui. The fonts
are compiled directly into the binary — no external font files needed at runtime.

## Fonts

| Font | Variants | Use case |
|------|----------|----------|
| **Inter** | Regular, Bold, Italic, Bold Italic | UI text, markdown body |
| **JetBrains Mono** | Regular | Code blocks in markdown |

Both fonts are from [Google Fonts](https://fonts.google.com/).

## License

Both fonts are licensed under the **SIL Open Font License 1.1 (OFL-1.1)**.

- **Commercial use**: YES
- **Embedding in binaries**: YES
- **Redistribution**: YES (with attribution)
- **Selling font files alone**: NO

Full license text: https://scripts.sil.org/OFL

## How It Works

Font TTF files are converted to compressed C arrays using ImGui's
`binary_to_compressed_c` tool and stored in `Embedded/Fonts/*.cpp`.
`CGuiFontManager` `#include`s these files and loads them into the ImGui font
atlas via `AddFontFromMemoryCompressedTTF()`.

## Usage

Markdown font loading is **opt-in**. Call it before the ImGui font atlas is built:

```cpp
// In your application init, before ImGui backend initialises fonts:
gGuiFontManager.LoadMarkdownFonts();  // loads Inter + JetBrains Mono
```

After the atlas is built, font pointers are available:

```cpp
gGuiFontManager.fontRegular     // Inter Regular
gGuiFontManager.fontBold        // Inter Bold
gGuiFontManager.fontItalic      // Inter Italic
gGuiFontManager.fontBoldItalic  // Inter Bold Italic
gGuiFontManager.fontMono        // JetBrains Mono Regular
gGuiFontManager.markdownFontsLoaded  // true if fonts were loaded
```

Applications that don't call `LoadMarkdownFonts()` pay zero cost — the font data
arrays are still compiled in (they live in `CGuiFontManager.cpp` which is always
built), but `AddFontFromMemoryCompressedTTF` is never called so the atlas is not
enlarged.

## CGuiFontManager also replaces VID_Fonts

`CGuiFontManager` subsumes the old `VID_Fonts` globals:

```cpp
gGuiFontManager.defaultFontPath        // was: gDefaultFontPath
gGuiFontManager.defaultFontSize        // was: gDefaultFontSize
gGuiFontManager.defaultFontOversampling // was: gDefaultFontOversampling
```

`VID_Fonts.h` still exists as a deprecated redirect to `CGuiFontManager.h` for
backward compatibility. New code should include `CGuiFontManager.h` directly.

## Regenerating Font Data

If you need to update the fonts to a newer version:

```bash
cd Embedded/Fonts
./generate_fonts.sh
```

This script downloads the latest TTFs from Google Fonts CDN and regenerates
the compressed C arrays. Requires `clang++` and internet access.
