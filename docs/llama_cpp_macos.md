# macOS: llama.cpp

## Prerequisites

- Xcode (and Command Line Tools)
- CMake (e.g. `brew install cmake`)
- Git (for submodules)

## One-time setup

```bash
git submodule update --init --recursive
```

## Build

The Xcode project runs `platform/MacOS/build-llama_cpp.sh` as a pre-build step.
That script configures and builds llama.cpp in:

- `other/lib/llama.cpp/build-macos/`

and then packages the needed static libraries into:

- `$MT_CAPS_LIBS_DIR/libllama_cpp.a`

To build from the command line:

```bash
xcodebuild -project platform/MacOS/MTEngineSDL.xcodeproj -scheme MTEngineSDL -configuration Debug build
```

If you see errors about missing `cmake`, install it and rebuild.
