# Windows UTF-8 file paths

MTEngineSDL's convention is that **a path in a `char *` or `std::string` is
UTF-8**. That is native on macOS and Linux. On Windows it is not: every
*narrow* CRT/Win32 entry point decodes a `char *` using the **process ANSI code
page**, historically CP1252 / CP1250 / CP932. The affected calls are the ones
everybody reaches for:

| Narrow call | Wide equivalent it really uses |
|---|---|
| `fopen`, `fopen_s` | `_wfopen` after an ANSI to UTF-16 conversion |
| `std::ifstream` / `std::ofstream` (`const char*` / `std::string` ctor) | same |
| `std::filesystem::path(std::string)` and `path::string()` | same, and `.string()` **throws** `std::system_error` when a character has no mapping |
| `CreateFileA`, `FindFirstFileA`, `GetFileAttributesA` | the `-W` counterpart |
| `argv` | `CommandLineToArgvW` + ANSI conversion |

So UTF-8 bytes name a file that does not exist. Symptom: a file is listed
happily through the wide APIs and then will not open — a photo under a folder
such as `Chrząszczożewoszyce`, or with CJK, emoji or macOS NFD-normalized
accents in its name (common for files that originated on another OS: a network
share, a Time Machine or Parallels-mounted volume). It reads as a missing or
corrupt file, not as an encoding bug.

Two independent layers fix this. **A Windows host app wants both.**

## 1. `activeCodePage=UTF-8` in the app manifest (the systemic fix)

`platform/Windows/MTEngineSDL.manifest` sets the process code page to UTF-8, so
`GetACP()` returns 65001 and *every* narrow path API in the process speaks
UTF-8 — the engine's, the app's, and the vendored third-party libraries'.

Host apps get it with one line. In the host `.vcxproj` (an `Application`
project), after `MTEngineSDLDir` is defined:

```xml
<Import Project="$(MTEngineSDLDir)platform\Windows\MTEngineSDLHost.props" />
```

The manifest is *merged* with the one the linker generates from
`AdditionalManifestDependencies`, so an existing host keeps its Common-Controls
dependency, requested execution level and anything else it declared. A host
that needs its own extra settings (DPI awareness, `longPathAware`) can either
add a second `<Manifest Include="..."/>` — `mt.exe` merges all inputs — or point
`$(MTEngineSDLManifest)` at its own copy.

Requires Windows 10 1903 (build 18362) or newer. Older Windows ignores the
setting and behaves exactly as it did before.

Verify it landed:

```
mt.exe -nologo -inputresource:YourApp.exe;#1 -out:embedded.manifest
```

XML gotcha: an XML comment may not contain `--`. `mt.exe` rejects the whole
manifest with `general error c1010070: Failed to load and parse the manifest`
and the link fails with `LNK1327`.

## 2. `SYS_FileUtf8.h` (belt and braces, in engine code)

`src/Engine/Core/SYS_FileUtf8.h` is header-only and works regardless of the
process code page or Windows version:

| Function | Use instead of |
|---|---|
| `SYS_FopenUtf8(path, mode)` | `fopen` / `fopen_s` |
| `SYS_Utf8ToFsPath(utf8)` | `std::filesystem::path(std::string)`, and to open an `ifstream`/`ofstream` |
| `SYS_FsPathToUtf8(path)` | `path.string()` (which throws) |
| `SYS_Utf8ToWide` / `SYS_WideToUtf8` | raw `MultiByteToWideChar` / `WideCharToMultiByte` calls |

`SYS_FopenUtf8` is a drop-in for `fopen`: identical for an ASCII path, correct
for everything else. It deliberately does **not** add the `\\?\`
extended-length prefix — that prefix also disables path normalization, so a
relative path or one containing `..` would break. For MAX_PATH use
`SYS_WindowsNormalizeLongPathUtf8()` from `SYS_WindowsPathUtils.h`, which is a
separate concern.

The header declares `MultiByteToWideChar`/`WideCharToMultiByte` itself rather
than including `windows.h`. That is deliberate: it is included from libjpeg's
translation units, and `windows.h`'s `basetsd.h` typedefs `INT32` as `int`
where libjpeg's `jmorecfg.h` already typedef'd it as `long`, which is a hard
`typedef redefinition with different types` error. The declarations match
`windows.h` exactly, so a TU that includes both is a legal redeclaration.

Engine call sites already converted (the media-loading path, where the failure
is user-visible):

- `stb_image.h` — `stbi__fopen` (upstream's `STBI_WINDOWS_UTF8` branch, made
  unconditional and given a heap buffer instead of upstream's fixed
  `wchar_t[1024]`). Kept in plain C with no engine includes: `stb_image.h` is
  compiled as C from `stb_image.c`.
- `CImageData.cpp` — all PNG/KTX2/raw load and save `fopen`s
- `CImageDataWebP.cpp` — `LoadWebP`
- `JPEGWriter.h` — `beginWrite`
- `JPEGReader.cpp` — `header`
- `CExifReader.cpp` — `ReadFile` / `ReadFileHeader`

Everything else in the engine still uses narrow calls and relies on layer 1.
When touching such code, prefer `SYS_FileUtf8.h`.

## Related

- `SYS_WindowsPathUtils.h` — separator normalization and the `\\?\` long-path
  prefix (a different problem: MAX_PATH, not encoding).
- Windows file dialogs use the wide common-dialog APIs and return UTF-8 through
  `CSlrString`; read them with `CSlrString::GetUTF8()` and `free()` the result.
- PhotoCruise's `src/Core/PC_PathUtf8.h` is the app-side equivalent of
  `SYS_Utf8ToFsPath`/`SYS_FsPathToUtf8`, and `CTestUtf8Paths` there is the
  end-to-end regression guard for both layers.
