# Embedded Fonts

Compressed C arrays for embedding font data into the binary.
Used by `CGuiFontManager` for markdown rendering in ImGui.

## Fonts Included

| File | Font | Variant | Source |
|------|------|---------|--------|
| `font_inter_regular.cpp`    | Inter | Regular     | https://fonts.google.com/specimen/Inter |
| `font_inter_bold.cpp`       | Inter | Bold        | https://fonts.google.com/specimen/Inter |
| `font_inter_italic.cpp`     | Inter | Italic      | https://fonts.google.com/specimen/Inter |
| `font_inter_bold_italic.cpp`| Inter | Bold Italic | https://fonts.google.com/specimen/Inter |
| `font_jetbrains_mono.cpp`   | JetBrains Mono | Regular | https://fonts.google.com/specimen/JetBrains+Mono |

## License

Both fonts are licensed under the **SIL Open Font License 1.1 (OFL-1.1)**.

**Commercial use: YES.** Embedding in binaries: YES. Redistribution: YES.
Only restriction: you may not sell the font files alone.

Full license text: https://scripts.sil.org/OFL

## Usage

Set `loadMarkdownFonts = true` in engine init (via `CGuiFontManager::LoadMarkdownFonts()`),
then access font pointers via `gGuiFontManager`. See `docs/embedded-fonts.md` for details.

## Regenerating

If you need to update the fonts, run:
```bash
cd Embedded/Fonts && ./generate_fonts.sh
```
This downloads the latest TTFs from Google Fonts and re-converts them.
