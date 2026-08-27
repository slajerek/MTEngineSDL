# FTXUI Integration

FTXUI is a C++ terminal UI library used by the admin dashboard (`--admin-dashboard` mode).

- **Source**: Git submodule at `other/lib/ftxui`
- **Upstream**: https://github.com/ArthurSonzogni/FTXUI
- **Components**: screen, dom, component (merged into single static archive)

## Building

### macOS (Xcode)

Built automatically by Xcode via the "Build ftxui" build phase in `MTEngineSDL.xcodeproj`.

To build manually:

```bash
./platform/MacOS/build-ftxui.sh
```

Output: `$MT_CAPS_LIBS_DIR/libftxui.a` (universal arm64+x86_64). That directory
is outside every checkout -- `~/.cache/mtengine/_deps/<caps-hash>/libs` by
default; see `platform/caps-lib.sh`.

### Linux

Option A — standalone script:

```bash
./platform/Linux/build-ftxui.sh
```

Option B — master build script (builds all dependencies + MTEngineSDL):

```bash
./build-linux.sh
```

Output: `platform/Linux/libs/libftxui.a`

FTXUI is also built automatically by CMake (`add_subdirectory`) when `MT_ENABLE_FTXUI=ON` (default).

### Windows

Run from **Developer PowerShell for VS 2022**:

```powershell
# x64 Release (default)
.\platform\Windows\build-ftxui.ps1

# ARM64 Release
.\platform\Windows\build-ftxui.ps1 -Platform ARM64

# x64 Debug
.\platform\Windows\build-ftxui.ps1 -Config Debug

# ARM64 Debug
.\platform\Windows\build-ftxui.ps1 -Platform ARM64 -Config Debug
```

Output: `platform/Windows/libs/{Platform}/{Config}/ftxui.lib`

Build all 4 variants for distribution:

```powershell
foreach ($p in 'x64','ARM64') {
  foreach ($c in 'Debug','Release') {
    .\platform\Windows\build-ftxui.ps1 -Platform $p -Config $c
  }
}
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `MT_ENABLE_FTXUI` | `ON` | Enable FTXUI terminal UI library |

When disabled, the FTXUI library is not built or linked. Code using FTXUI should be guarded with `#if MT_ENABLE_FTXUI`.

## Include Path

```cpp
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
```
