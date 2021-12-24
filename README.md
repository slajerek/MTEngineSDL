# Hello and welcome to the MTEngineSDL!

This is an application host framework for starting custom apps created using
SDL2, ImGui and OpenGL.

# How to compile

Engine compiles SDL2 with ImGui and app as static binary.
You need to compile the MTEngineSDL first.

MTEngineSDL: https://github.com/slajerek/MTEngineSDL
SDL2: https://github.com/libsdl-org/SDL
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

Static SDL2 libraries are in `./platform/Windows/libs` folder.

Windows version uses LogConsole.exe app to display verbose log when compiled
without `GLOBAL_DEBUG_OFF` in `DBG_Log.h`

Fast Log Console C# project: https://sourceforge.net/projects/fastlogconsole/

# Linux

```
mkdir build
cd build
cmake ./../
make
```

Remember to have SDL2 library installed.


# Thanks

This product would not have been created without the help of alpha testers:
Euan Gamble, Robert Troughton, Jesper Rune Larsen, Steve West, Lukhash, 
Markus Dano Burgstaller, Brush/Elysium, Alex Goldblat, Cescom,
Isildur/Samar, Dkt/Samar, Mojzesh/Samar, Pajero/MadTeam^Samar, Yugorin/Samar

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
UI assets licenses are provided by the ImGui, SDL2 and all referenced licenses.


CIAO!
