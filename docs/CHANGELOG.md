# Changelog

## Version numbers

**Odd minor versions are development; even minor versions are stable.**

- `3.21`, `3.23`, … live on `devel`. They are where work lands, and they may
  change under you: an interface added in one commit can be reshaped in the
  next.
- `3.22`, `3.24`, … live on `master`. A stable release is made by merging
  `devel` into `master` and bumping to the next even number; nothing goes
  onto `master` that has not been built and tested on macOS, Linux and
  Windows first.

An app built on this engine follows the same pairing: its `devel` tracks the
engine's `devel`, its `master` the engine's `master`.

The convention starts at 3.21. Earlier numbers below predate it and carry no
stability meaning.

**The engine and the app template carry the SAME number.** They are a matched
pair and are read as one release, so a reader who has 3.21.6 of one should not
have to work out which of the other goes with it. 3.21.5 is absent here for that
reason: the two had drifted a patch apart and the engine skips a number to come
back level. A number is cheap; a reader guessing at a pairing is not.

**The engine mints the number; the template only carries it.** A change to the
template alone gets no number of its own — it ships under whatever number the
engine currently has, however many template releases have landed on top of it.
A new number appears here first, and the template follows. This holds for the
template and nothing else: an application built on this engine versions itself
however it likes.

---

## 3.21.14 — development

**A view going fullscreen no longer unbalances ImGui's style stack.**
`CGuiView::PreRenderImGui` pushed two style vars when
`guiMain->viewFullScreen == this`, while `PostRenderImGui` popped two when
`guiMain->viewFullScreen != NULL` -- a different question. Going fullscreen
hides the other views, so nobody was left to mispop except
`guiMain->currentView`, which `CGuiMain::RenderImGui` draws unconditionally:
any host whose main view uses the base Pre/Post pair popped two style vars it
had never pushed, on every frame, and ImGui reported "Calling PopStyleVar()
too many times!". Under a debugger that stops the process, which reads as a
freeze rather than as an error.

`PostRenderImGui` now pops exactly what `PreRenderImGui` pushed, latched in
the view. A latch rather than a matching predicate, because it also holds if
the flag ever changes between the two calls -- it cannot today, since
`SetViewFullScreen` defers the assignment to a UI-thread task, but a context
menu item calling it sits between Pre and Post and nothing else would notice
if that deferral went away.

An app whose main view does not call the base pair never saw this.
MTEngineSDLDummyApp's does, which is what a template app is supposed to
demonstrate, and its Shader Toy output window is the first fullscreen caller
in the tree.

**A custom fragment shader can now sample four textures.**
`CRenderShaderCustomFragment` gains `SetChannelTexture(n, handle)` and
`SetChannelSampler(n, filter, wrap)` — render-thread-only pure stores — and
the uniform block grows from 48 bytes to 240 to carry ShaderToy's full set:
`iChannelResolution`, `iChannelTime`, `iFrameRate`, `iDate`, `iSampleRate`,
plus two of ours, `iChannelUvTransform` and `iChannelWrap`. All three backends
implement it, and each preamble defines `MAIN_IMAGE` so a host writes one word
instead of MSL's eleven-parameter signature.

**The `texChannelN` macros go through `mtChannelUV()`**, which flips, wraps
and scales in that order. The flip is `iChannelUvTransform.z` and defaults to
on: ShaderToy's `fragCoord` has its origin at the bottom left while a
texture's `v = 0` is its top row, so an unflipped channel samples every image
upside down. It is not a workaround for how images are loaded -- they are
stored top-down, the way ImGui wants them -- and shadertoy.com defaults the
same per-channel vflip to on for the same reason. Wrapping happens before the
scale because it has to happen in the image's own 0..1 space, not in the
padded texture's.

**The channels bind at slots 1..4 on every backend, never 0**: ImGui claims
slot 0 for its own draw command, after the callback that installs the shader.
Nothing restores the higher slots afterwards, so `ResetState()` unbinds them —
on D3D11 that needs a callback of its own, since `BACKUP_DX11_STATE` saves
pixel resources for slot 0 only.

Three things in the OpenGL path are not interchangeable with the obvious
alternative, and each is commented where it lives. The sampler units are
assigned in `SetShaderVars()` rather than after `glLinkProgram`, because the
base class treats `iChannel0` as its own texture uniform and re-points it at
unit 0 every frame. Filtering comes from cached sampler objects rather than
`glTexParameteri`, which would permanently change how that image is filtered
everywhere else in the application. And the active texture unit is restored to
0 — removing that breaks nothing visible, which is what makes it dangerous:
`imgui_impl_opengl3` sets the active unit once per frame, not per draw
command, so the failure appears later as the wrong texture in an unrelated
window.

An empty channel gets a 1x1 black texture rather than a null binding: sampling
an unset slot reads zero in any case, but Metal's validation layer and D3D11's
debug layer both object on every draw. The MSL preamble repeats the C++
header's 240-byte `static_assert`, so the two layouts cannot drift apart
without one of them refusing to build.

## 3.21.13 — development

**`ImGuiColorTextEdit` is vendored** at `src/Engine/Libs/imgui_textedit/` —
goossens' ground-up rewrite, MIT, unmodified, with the TextDiff widget and its
BSD-3 `dtl.h` alongside. C++17, STL only, no regex, no boost; GLSL and HLSL
language definitions built in. It compiles clean against our ImGui 1.93.0 WIP
in about a second. The engine has never had a syntax-highlighting editor; the
only one in any host was a dead 2017 copy whose selection was broken.

**`CGuiViewCodeEditor`** — a thin `CGuiView` over it, for a host that wants a
standalone editor window. Forwards text, language, read-only, palette and
font; one extension point, `RenderToolbar()`. Its default font is resolved at
render time, because a host's views are constructed before its fonts load.

**An application can now declare its own licences.** `LICENSES.txt` was
generated from the engine's vocabulary alone, so a host that embedded a font
shipped a binary containing it and a licence file omitting it.
`mtengine-app-licenses.json` beside `mtengine.caps` is read if present,
emitted as its own section, and gated exactly as a capability dependency is:
fields validated always, the commercial deny-list under
`MT_COMMERCIAL_BUILD=1`. Four dependencies that were already shipped and
undeclared are declared at the same time: the vendored editor, its `dtl`, and
the **Inter and JetBrains Mono** fonts compiled into every binary — both
OFL-1.1, which requires attribution on redistribution.

## 3.21.12 — development

**`CRenderBackend::CreateCustomFragmentShader()` — a fragment shader whose
source arrives at runtime.** A host can now compile and replace shader text
while it runs, which is what an in-app shader editor needs and what nothing
here previously offered.

It is implemented on **all three backends**. Until now the engine had a
ShaderToy-style shader for OpenGL and Metal that no application could reach —
`CRenderBackend` exposed no factory for it — and none at all for D3D11, which
is the default backend on Windows. The Metal header had said as much for some
time: *"an engine facility that works on one backend and silently not the other
is a trap for whoever picks it up next."*

`CRenderShaderCustomFragment` is a pure interface rather than a `CRenderShader`
subclass. The OpenGL and Metal implementations derive from their own shader
base as well, and a shared `CRenderShader` base would have given them two
copies of it.

- **The compiler's diagnostics are returned, not merely logged.** `LOGError`
  compiles to nothing under `GLOBAL_DEBUG_OFF`, which is set on Linux, so a
  host that displayed the log would display an empty panel exactly where a
  headless CI run is the only way anyone sees the failure.
- **A failed rebuild keeps the previous program bound**, on every backend, so a
  host's preview does not go black on a typo.
- **`GetPreambleLineCount()`** lets a host rebase the compiler's line numbers
  onto the text its user actually typed. It counts the string rather than
  stating a number, and the OpenGL count includes the `#version` line the base
  class prepends.
- **The uniform block is 48 bytes with one trap**: the MSL struct must say
  `packed_float3`, because a plain MSL `float3` is 16 bytes and would shift
  every field after it, silently.

Runtime HLSL compilation here does not contradict the bytecode-only policy in
`tools/embed-hlsl-shaders.ps1`: that rule guards a shader which *also* has
committed bytecode, and text typed at runtime has none. The compiler is already
linked and already used every launch by `imgui_impl_dx11`'s own two shaders.
That script's header now records the exception.

**Also fixed**: `CRenderShaderOpenGL4::CheckShader` logged with a `%s` and no
argument — undefined behaviour printing whatever the stack held — while the
`desc` parameter passed in for exactly that had never been used.

## 3.21.11 — development

**`MT_VERSION_STRING` says 3.21, and had said 3.19 since that release.** It is
the one place a user can read a version — the startup banner, the UI debug
view, and the `appVersion` field of every crash report — and all three had been
quietly a release and a half out of date, which is worst in the crash report,
where the number is the first thing anyone diagnosing a fault reads.

It now carries the **minor only**. Patch numbers move faster than a constant is
worth editing, and what actually separates two builds of the same minor is
already recorded beside it in every one of those places: `__DATE__` and
`__TIME__` appear in the banner, in the UI debug view, in the resource manager
overlay, and in the crash report's `buildInfo` alongside platform and
architecture. One caveat worth knowing when reading a Linux log: the banner is
written with `LOGM`, and `GLOBAL_DEBUG_OFF` is set on that platform, so it does
not appear there at all. The other three places do not go through the log
macros and are unaffected.

The app template moves to this number with the engine, as it always does.

---

## 3.21.10 — development

**The Linux diagnostic logging from 3.21.8 is off again**, its job done: it
found the MIDI sequencer crash and nothing else needs it. The comment left in
its place says what it costs to have it off — with `GLOBAL_DEBUG_OFF` set, a
crash on this platform prints no diagnostic at all — so the next person
debugging something silent knows which line to comment out first.

**The manifest's bash test finds a real bash before measuring with it.** Windows
ships `System32\bash.exe`, the WSL launcher, and on a machine with no
distribution installed it exits 1 having written its complaint to *stdout* — so
the test saw a failed run with an empty stderr and reported it as a
manifest-format failure, which it is not. The candidate is now asked what it is
(`--version` must say GNU bash), Git for Windows' own bash is looked for outside
PATH, and the test skips honestly if no such bash exists rather than blaming the
format it is measuring.

---

## 3.21.9 — development

**MIDI input asks whether there is a sequencer before opening one, and never
leaves a garbage pointer behind.** RtMidi's ALSA backend opens `/dev/snd/seq` in
its constructor, and when that device does not exist — a container, a CI runner,
any machine without `snd-seq` loaded — it does not fail cleanly: it continues
with a null sequencer handle and faults inside libasound. That is not an
`RtMidiError`, so the `try`/`catch` around it never had anything to catch.

The device is now probed first and its absence means no MIDI input and a
running application, which is how every other missing device here already
behaves — audio has asked before opening since it was written.

Two latent bugs in the same function are fixed with it: `midiIn` was never
initialised and the `catch` left it untouched (the two lines that would have
cleared it were commented out), so a construction that threw left a dangling
pointer for every later call to find; and the member is now assigned only after
the port is open and named, because `openPort()` and `getPortName()` can throw
too and a half-open device must not be reachable.

---

## 3.21.8 — development

**Temporary: Linux debug logging is on, to find a crash that leaves no trace.**
Linux is the only platform where `GLOBAL_DEBUG_OFF` is set by default, and with
it set every log macro compiles to nothing — including the `LOGError` inside
`SYS_FatalExit`. So when the app died during automated testing it printed
absolutely nothing: not a hidden diagnostic in a log file, but no diagnostic
written anywhere at all. Reading the code could not narrow it either; every
audio path is correctly guarded against having no device.

With the define commented out, logging goes to stdout and a run prints the
engine's startup trace up to the last thing it managed before dying. **This is
a diagnostic build and the define comes back as soon as the crash is found.**

---

## 3.21.7 — development

**The Windows app follows the engine's own toolset choice, instead of quietly
picking its own.** `app-build-windows.ps1`'s `-Compiler` switch only stated an
explicit `/p:PlatformToolset=` override for `-Compiler MSVC`; for the default
`Clang` it passed nothing, so each `.vcxproj` fell back to whatever it hardcodes
for itself — ClangCL for the engine, v143 for the app — never actually telling
the app to follow `-Compiler` at all. That split reached CI for the first time
on 2026-09-03 as 16 unresolved `__std_*` STL symbols at the app's final link:
object code the ClangCL engine build compiled against one STL/SDK vintage,
linked by the app's v143 build against another's import libraries. A
command-line `/p:` value is a global property a `.vcxproj`'s own unconditioned
`<PlatformToolset>` assignment cannot override, so stating the toolset
explicitly for BOTH `-Compiler` values settles it for both projects the same
way, regardless of what each `.vcxproj` says on its own. `-Compiler` stays the
one command-line switch that decides it; a `.vcxproj`'s own hardcoded default
is for an IDE build only, which this script's `/p:` arguments never reach.

**libvpx's generated `vpx.sln` disables whole-program optimization, so it links
under either toolset.** Unifying the app onto the engine's toolset surfaced a
second, real incompatibility one level down: `vpx.sln` — generated fresh by
libvpx's own build scripts, not something this repo owns the source of —
enables `/GL` on its Release config by default, an MSVC-proprietary
intermediate object format `link.exe` happily consumes (it invokes cl.exe's own
LTCG backend on such an object as part of linking) but `lld-link` cannot read
at all:

    lld-link : error : ARM64\Release\vpx\vp9_vp9_dx_iface.obj: is not a
    native COFF file. Recompile without /GL?

That stayed invisible for as long as whatever linked `MTVideoCodecs.lib` was
also plain MSVC `link.exe`. The first attempt at fixing it forced
`PlatformToolset=ClangCL` on `vpx.sln` to match `-Compiler`, on the theory that
it should follow the same toolset as everything it is linked into — which then
hit a third, unrelated wall: libvpx's ARM64 NEON dotprod path
(`vpx_convolve8_neon_dotprod.c`) needs the `dotprod` target feature enabled at
the translation unit its `always_inline` intrinsic is used in, a strictness
cl.exe's ARM64 front end does not enforce the same way, and libvpx's own build
files carry no Clang-specific flag for it on this generated Windows project.
Fixing that would have meant patching either libvpx's vendored source or the
auto-generated `.vcxproj`'s per-file compiler flags for a feature this repo has
no informed opinion on enabling globally. Disabling `/GL` sidesteps both
problems by never switching `vpx.sln`'s compiler at all: plain COFF, from
either MSVC or Clang, links under either linker, so which compiler produced it
stops mattering, and `vpx.sln` is free to keep resolving whatever
`PlatformToolset` MSBuild resolves for it by default, exactly as it always has.

Verified on Windows 11 ARM64: a full from-scratch MTEngineSDLDummyApp build now
compiles and links the engine and the app both under ClangCL, and the app's
test suites are 4/4 and 12/12 from the release package.

Follows engine 3.21.6.

---

## 3.21.6 — development

**HEIF follows the platform's own decoder, and the capability fragments stop
disagreeing about it.** HEIF's payload is HEVC and HEVC is patent-encumbered, so
calling the platform decoder means the licence is Apple's or Microsoft's,
already paid for on the machine the code runs on. The engine has always
dispatched that way — ImageIO on macOS, WIC on Windows, libheif only on Linux
where no system decoder exists — but the table that switched the library off
named Windows alone. macOS therefore resolved `MT_ENABLE_LIBHEIF=1`, compiled a
translation unit its own dispatch can never reach, and offered to link an HEVC
decoder into a binary whose HEVC licence was Apple's.

`PLATFORM_PROVIDES_FLAGS` is a third category, kept apart from the two that
existed: "not obtainable here yet" invites someone to do the work, while "the
platform already provides it" says do not. Windows stays in the first, because
there the reason really is a missing port. The distribution gate is unchanged
and still overrides both — patent pools attach to distribution, so being free
and open settles the copyright question and not the patent one.

In the same change the emitters stopped guessing which platform they were
writing for. Two hardcoded Windows and two passed nothing, so a single resolve
could write an xcconfig saying `1` beside a props saying `0`. The target
platform now travels with the key.

**Two Windows build fixes.** The new post-extraction check expected a
`CMakeLists.txt` of LibRaw, which the archive does not contain — upstream ships
autotools, and the file this build uses is written by the script afterwards. It
failed on a cold cache and passed on every later run in the same cache, so a
machine that had built once could not reproduce it. And both extractors now ask
`tar --version` once instead of attempting a call that is *expected* to fail:
the resulting "Option --force-local is not supported" was printed before every
archive on the healthy path, and was read as the cause of an unrelated failure
four archives later.

---

## 3.21.4 — development

**macOS staged the FFmpeg headers and nothing else, so the engine compiled
against whichever libvpx the machine happened to have.** It includes
`<vpx/vpx_codec.h>` and `<opus/opus.h>` directly, not through FFmpeg, and only
the five `libav*` directories reached the shared include root. Those headers
were therefore never provided by the build at all. One machine here compiled
against Homebrew's libvpx 1.16.0 while linking the 1.15.2 this build produces,
and it worked only because the two happen to agree; a machine with no system
libvpx cannot compile the engine at all. Linux always staged the whole install
prefix and Windows always merged vpx and opus into one include root — macOS was
the outlier.

**An x86 target says up front that it needs an x86 assembler.** libvpx has no
assembler-less fallback the way FFmpeg's `--disable-x86asm` is, so without nasm
its configure stops deep inside the codec build, naming neither the package to
install nor the build that wanted it — and on macOS it does so from inside an
Xcode script phase, where it is nearly unfindable. The question is now asked
once, before anything is downloaded, and the answer names the package. macOS
always builds universal so it always asks; Linux asks only when the machine's
own architecture is x86.

**The Windows codec build names MSYS2's `make` absolutely, instead of trusting
PATH.** FFmpeg is configured out of tree, and its configure then writes a
one-line Makefile into the build directory:

    include /c/Users/.../video-codecs/src/ffmpeg-7.1.2/Makefile

an absolute path in MSYS2's `/c/...` spelling, because that is what `pwd`
returns in the shell configure ran in. Only an MSYS2 `make` can read it. A
native Windows GNU make reads `/c/Users` as `C:\c\Users` and stops with

    Makefile:1: /c/Users/.../Makefile: No such file or directory
    make: *** No rule to make target '/c/Users/.../Makefile'.  Stop.

naming FFmpeg's own source Makefile — so it reads as a broken extraction, and it
is not. libvpx passes through the same shell untouched, because ITS generated
Makefile says `include config.mk`, relative, which any make resolves; "vpx
built, FFmpeg did not" is the signature of this defect rather than evidence
against it.

`Import-VcVars` splices MSYS2's `usr\bin` into PATH ahead of the pre-existing
entries, which is sufficient *when MSYS2 has make installed*. It is not part of
a base MSYS2 install — it is the documented `pacman -S --needed make`
prerequisite — and when it is absent PATH search does not fail; it falls through
to whatever other make the machine has. `/usr/bin/make` removes the search:
`/usr/bin` inside the bash this script launches is always
`<MSYS2_ROOT>\usr\bin`, whatever `$env:MSYS2_ROOT` says. The prerequisite is now
checked once, before the first archive is configured, and reported as the pacman
line rather than as a missing Makefile an hour later.

Measured on Windows 11 ARM64 2026-09-03, against the real generated Makefile in
an MSYS2 shell with a native make first on PATH: bare `make` reproduces the
runner's message byte for byte, `/usr/bin/make` runs. Worth knowing while
reading this: a plain MSYS2 shell inherits the Windows PATH, so bare `make`
there is the native one on any machine that has one — a Chocolatey install is
enough.

**Both extractors verify what they extracted.** `Extract-Archive` returned early
whenever the destination directory existed, and that early-out is the only thing
making extraction a once-per-cache event: no stamp, no manifest, no size check.
A half-extracted tree was therefore permanent, and every later run failed
somewhere that said nothing about extraction. Both copies now take a list of
files the build immediately needs — each tree's `configure`, and FFmpeg's
top-level `Makefile` — and check it on the cached path as well as on the freshly
extracted one, so a cache poisoned by a failed run is reported where the cause
is still legible. bsdtar's exit code says it stopped without raising an error,
not that the tree is complete; nothing else here checked the difference.

---

## 3.21.3 — development

Two more failures from continuous integration, both of the same shape as the
last: something a development machine happens to have, and a clean runner does
not.

**lcms2 no longer builds its command-line tools on Linux.** `LCMS2_BUILD_TOOLS`
defaults to ON and builds `tificc`, `tifdiff` and `jpgicc`, which
`find_package(TIFF)` — and that finds the libtiff this same script installed
into the prefix a few steps earlier, whose exported target names
`CMath::CMath` without teaching the consumer to define it. The generate step
then fails. A machine with `libtiff-dev` installed never sees it, because
`find_package` picks the system copy first. Nothing here consumes those tools.
The Windows script has passed these options since it was written; the Linux copy
never got them.

**The Windows tar fallback passed a POSIX path to a tar that cannot read one.**
The video-codec extractor tries GNU tar with `--force-local` and falls back to
the system tar, but both calls gave `-C` the MSYS spelling of the directory. On
a runner where `tar.exe` is the Windows built-in BSD tar, the fallback then
failed for a second, unrelated reason — `could not chdir to '/c/Users/...'`. The
two tars want opposite spellings of the same directory.

---

## 3.21.2 — development

Two build fixes, both found by the first continuous-integration run of a
development branch — the first time any of this was built from a clean clone on
a machine that is not the author's.

**The codec script failed on stock macOS bash.** macOS ships bash 3.2, where
expanding an *empty* array under `set -u` is an unbound-variable error; 4.4 and
later allow it. The 3.21.1 change that moved the decoder policy into the
vocabulary removed the branch that filled one such array and left the array and
its expansion behind — dead code that could only ever run on 3.2, and could only
ever fail there. A machine with a newer bash never saw it. The array is gone,
and the one that is legitimately empty on a native build now uses the
`${a[@]+"${a[@]}"}` form. A test decides this class by reading: an array
declared empty and never filled must not be expanded.

**CMake 4 refuses FreeType.** It removed compatibility with
`cmake_minimum_required` below 3.5 and will not configure such a project at all;
the vendored FreeType declares 2.8.12. `CMAKE_POLICY_VERSION_MINIMUM=3.5` is now
passed to every CMake-configured dependency on Windows, where an unused `-D` on
an older CMake is a warning rather than an error.

---

## 3.21.1 — development

Build-system work on top of 3.21, and the first Windows fixes that came out of
building every host application on that platform. No engine source behaviour
changes on macOS or Linux; the D3D11 addition is new code on a path that
previously had none.

**A build store per dependency.** Dependency archives used to be keyed by the
whole acquisition capability set: switching one capability off moved the bucket
for *every* dependency, so changing a terminal-UI library rebuilt SDL3, mbedTLS,
the image codecs and FFmpeg from scratch to produce the same bytes. Each
dependency now builds and stamps in a store keyed only by the capabilities it
actually reads, then copies its outputs into the same shared `libs` directory
consumers link against — nothing downstream changed. Dependencies that read no
capability at all (SDL3, FreeType, libuv, uSockets) get one bucket per machine,
shared by every application on it. Measured: turning FTXUI off rebuilds FTXUI
and nothing else.

**The FFmpeg decoder policy has one home.** The decoder, parser and demuxer
lists live in `tools/mtcaps/vocabulary.json` and reach the codec scripts as
resolved values; the scripts no longer carry their own copies. In the same
change the built decoder set follows the *resolved build mode* rather than the
licence tier, which are not the same question — a non-commercial tier can
legitimately resolve to a restricted decoder set.

**A release package on every platform.** `platform/<Platform>/prod/<arch>/`
holds the application, its assets and `LICENSES.txt`, on macOS and Linux as it
already did on Windows, and the deploy step fails rather than shipping a package
without its licence document. Applications are tested *from* that package,
because an engine application resolves its assets through the working directory
and never through the executable's own location.

**Windows.** The generated CMake fragment could not carry a Windows path: a
backslash in a quoted value is a character escape, so the line was a parse error
that took the whole fragment down. An IDE build compiled applications with no
capability defines at all. An IDE build had no way to populate its dependency
directory short of building the dependencies inside Visual Studio, and now syncs
them from the stores instead (`platform/Windows/sync-deps-view.ps1`). IDE and
command-line builds no longer write their executable to the same path, where
each could see the other's as up to date. The app-side MSBuild properties moved
into the engine (`platform/Windows/MTEngineApp.props`), leaving an application
with a stub that declares its identity.

**D3D11: the masked-tile shader.** It had been left unimplemented on this
backend with a note that every caller drew an unshaded fallback; one did not.
Ported from the GLSL original deliberately without its Y flip, since
`SV_Position` is already top-left where `gl_FragCoord` is bottom-left.

**Cache maintenance.** `tools/appbuild/mtengine-gc.py` understands the per-unit
stores and keeps them per dependency rather than globally. Its liveness check
resolved without engine options, which put every path it computed under a
backend segment that no build on an ARM machine ever writes to — it protected
directories that did not exist and could have deleted a live bucket.

---

## 3.21 — development

The capability system, and a build that keeps its output out of your checkout.

**Capabilities.** What the engine compiles in is chosen per app rather than
fixed here. An app declares a manifest naming what it needs — video playback,
photo codecs, LLM inference, HTTPS, websockets, MIDI, the terminal, the test
engine — and the build resolves that into compiler defines, the set of
dependencies to acquire, and the licence documents to ship. A capability that
is off costs nothing: its dependency is never built and its code is never
compiled. `tools/mtcaps/vocabulary.json` is the authoritative list, with each
capability's dependencies, licences and acquisition scripts;
`tools/mtcaps/tests/test_mtcaps.py` is its test suite and runs standalone.

**The engine owns the build flow.** `tools/appbuild/app-build-{macos,linux}.sh`
and `app-build-windows.ps1` carry it — resolve, dependency acquisition, engine
build, app build, licence gate, symbols. An app repository keeps parameters and
a thin stub per platform, not a copy of the machinery. `--set KEY=VALUE`
(`-Set` on Windows) overrides a capability for one build without editing a
tracked manifest.

**Nothing a build produces is written inside the checkout.** Objects,
dependency archives, generated headers, symbols and binaries live under a cache
root outside it: `${XDG_CACHE_HOME:-~/.cache}/mtengine`, or
`%USERPROFILE%\.cache\mtengine` on Windows. `MTENGINE_BUILD_ROOT` overrides it.
`tools/appbuild/mtengine-gc.py` reports on that cache and prunes it, keeping
the revision a build would write now plus the newest few per app.

**Dependencies are cached by what they actually depend on.** The cache key is
the capabilities that own an acquisition script, so flipping a capability no
dependency has ever heard of no longer rebuilds SDL3, FFmpeg and llama.cpp to
produce the same bytes. A test scans every dependency script for the capability
flags it really reads and fails if one falls outside the key.

**Visual Studio builds what the command line builds.** The IDE path reads the
resolved capability set through `platform/Windows/MTEngineApp.targets`, which an
app imports rather than copies.

**Windows dependencies are built from source.** SDL3, FreeType, libuv and
uSockets are compiled by `platform/Windows/build-*.ps1`; the tracked prebuilt
archives are gone, as is SDL2, which nothing linked.

**Fixes.** libuv's `src/{unix,win}/core.c` are in the tree again — an
unanchored `core.*` ignore rule had been matching them, and on a
case-insensitive filesystem `src/Engine/Core/` and SDL's `src/core/` with
them. A log written without `--log-dir` goes to the system temp directory
rather than the current one. Two absolute paths under a developer's home
directory left the sources. Building two checkouts of the engine on one
machine no longer collides over the shared dependency work trees.

## 3.20 — 2026-08-31

Capability programme, first public cut: the engine-owned build drivers,
dependency archives relocated outside every checkout, and the Windows
from-source dependency builds.

## 3.19 — 2026-08-05

`.gitignore`: SDL2's `src/core/` was being swallowed by the legacy `core`
rule. CI on macOS and Windows builds static SDL2 from the vendored source;
clang `_m_prefetch` redefinition fixed.

## 3.18 — 2026-06-04

Windows dependency builds auto-detect the Visual Studio CMake generator
instead of hardcoding one. `SYS_Defs.h` includes `<stdint.h>` so fixed-width
types resolve on GCC 13 and later. `CTestSuite`/`CTest` became the shared base
for app subclasses. ggml native CPU optimisations disabled on ARM.

## 3.16 — 2025-12-24

## 3.15 — 2025-02-22

Event ordering fixed so a key shortcut cannot consume an event before the
focused view sees it.

## 3.14 and earlier

See the commit history.
