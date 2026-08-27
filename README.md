# Hello and welcome to the MTEngineSDL!

This is an application host framework for starting custom apps created using
SDL3, ImGui and OpenGL.

# How to compile

Engine compiles SDL3 with ImGui and app as static binary. SDL3 is built from
the vendored source tree in `other/lib/SDL-release-3.4.14-static/` by the
platform build scripts -- no system or package-manager SDL is used or needed.
You need to compile the MTEngineSDL first.

MTEngineSDL: https://github.com/slajerek/MTEngineSDL
SDL3: https://github.com/libsdl-org/SDL
ImGui: https://github.com/imgui

Verbose log can be switched on by commenting out `#define GLOBAL_DEBUG_OFF`
in `DBG_Log.h` file.

## macOS

cd `./platform/MacOS`
I normally put files into `~/develop/MTEngineSDL` folder. 
Project should compile as is in Xcode, remember to reference SDL library.
The precompled library is put in `~/develop/MTEngineSDL/MacOS/libs` folder.

## Windows

Check VS2019 project in `./platform/Windows`. This should work when put into
`C:\develop\MTEngineSDL`

Static SDL3 libraries are in `./platform/Windows/libs` folder (x64 and ARM64,
Debug and Release -- 32-bit Windows was dropped in 2026-08).

Windows version uses LogConsole.exe app to display verbose log when compiled
without `GLOBAL_DEBUG_OFF` in `DBG_Log.h`

Fast Log Console C# project: https://sourceforge.net/projects/fastlogconsole/

# Linux

```
sudo apt-get install build-essential libgtk-3-dev libsdl2-dev libglew-dev
mkdir build
cd build
cmake ./../
make
```

No SDL package needs to be installed: SDL3 is built from the vendored source
tree by `platform/Linux/build-sdl3.sh`, which `build-linux.sh` runs for you.


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
