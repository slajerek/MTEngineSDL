# Hello and welcome to the MTEngineSDL!

This is an application host framework for starting custom apps created using
SDL3, ImGui and OpenGL.

# How to compile

One command per platform. It clones what it needs, builds every dependency
from the vendored sources, and links the engine -- no system SDL, no
package-manager dependency, nothing to install first beyond a compiler and
CMake.

```
./build-macos.sh          # macOS (Xcode toolchain)
./build-linux.sh          # Linux
.\build-windows.ps1       # Windows (PowerShell, VS2019/2022)
```

SDL3 is built from `other/lib/SDL-release-3.4.14-static/`; ImGui, FreeType,
mbedTLS, libuv and the rest come from `other/lib/` or from submodules the
build initialises on demand.

Nothing a build produces is written inside this checkout. Object files,
dependency archives, generated headers and final binaries all live under a
cache root outside it -- `${XDG_CACHE_HOME:-~/.cache}/mtengine` on macOS and
Linux, `%USERPROFILE%\.cache\mtengine` on Windows. Override it with
`MTENGINE_BUILD_ROOT`. `tools/appbuild/mtengine-gc.py` reports on that cache
and prunes it.

## Building an app on top of the engine

The engine is a host framework: you write the app, it provides the window,
the ImGui context, the view stack, the file system and the rest. An app is a
sibling checkout of this one that implements the `MT_API.h` lifecycle and
carries three small files -- a capability manifest (`mtengine.caps`), a
parameter file (`mtengine-app.conf`) and a build stub per platform that hands
over to `tools/appbuild/app-build-<platform>`. The build flow itself lives
here, so an app repo holds parameters rather than a copy of the machinery.

`MTEngineSDLDummyApp` is the template to copy:
https://github.com/slajerek/MTEngineSDLDummyApp

## Capabilities

What the engine compiles in is selected per app, not fixed here. The manifest
names capabilities -- video playback, photo codecs, LLM inference, HTTPS,
websockets, MIDI, the terminal, the test engine and more -- and the build
resolves them into compiler defines, the dependency set to acquire, and the
licence documents to ship. A capability that is off costs nothing: its
dependency is never built and its code is never compiled.

`tools/mtcaps/vocabulary.json` is the authoritative list, with each
capability's dependencies, licences and acquisition scripts. Its tests run
standalone:

```
python3 tools/mtcaps/tests/test_mtcaps.py
```

## Verbose logging

Verbose logging is a build switch: a development build (`./build-<os>.sh`)
has it on, a final build (`--prod`) off unless you pass `--logs on`. `LOGError`
and `LOGFatal` are in every build. Never edit `DBG_Log.h` for output; see
`docs/testing.md`. Without `--log-dir`
the log goes to the system temp folder, or to `./log/` when that directory
exists. On Windows, LogConsole.exe displays it live:
https://sourceforge.net/projects/fastlogconsole/

# Thanks

This product would not have been created without the help of alpha testers:
Euan Gamble, Robert Troughton, Jesper Rune Larsen, Steve West, Lukhash, 
Markus Dano Burgstaller, Brush/Elysium, Alex Goldblat, Cescom,
Isildur/Samar, Dkt/Samar, Mojzesh/Samar, Pajero/MadTeam^Samar, Boogaloo/Horizon,
zero211, Mr.Mouse/XeNTaX/Genesis Project, Yugorin/Samar

And everyone who made a donation!

# Beer Donation

If you like this tool and you feel that you would like to share with me
some beers, then you can use this link: https://tinyurl.com/C64Debugger-PayPal

Or send me some Bitcoins using this address:
`1G3ZRT7j27QycHnkoo176t9j5a2J49fsXc`

Donations will help me in development, thanks!

# License

MTEngineSDL source code is licensed under MIT license.

This product uses 1-Writer font: http://home-2002.code-cop.org/c64/font_01.html
UI assets licenses are provided by the ImGui, SDL3 and all referenced licenses.


CIAO!
