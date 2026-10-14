# Building, running and testing an MTEngineSDL application

**This is the one procedure for every application built on this engine.**
Each application's own guidance points here and does not restate it. If a
command below is wrong, fix it here; if it is missing, add it here. It is
written so that an agent with no other context does the right thing on the
first try -- every command is copy-pasteable from the repository root on the
platform it names.

Contents: 1 the two builds -- 2 the four switches -- 3 where the binary runs
from -- 4 running tests -- 5 fixtures -- 6 logging -- 7 the final-build
procedure -- 8 CI -- 9 what to do when -- 10 never do this.

---

## 1. Two builds, and only two

| | development build | final build |
|---|---|---|
| how | your IDE, or `./build-<os>.sh` | the command line only: `./build-<os>.sh --prod` (`.\build-windows.ps1 -Prod`) |
| output | the binary, wherever the toolchain puts it | `platform/<Platform>/prod/<arch>/` -- binary, `assets/`, every `MT_APP_PAYLOAD` directory, `LICENSES.txt`; wiped and rebuilt every time |
| logs / symbols | on / on | **off / off** unless you say otherwise (#2) |
| runs from | **the git root** | the package |
| tested by | `tests/run_test.sh` | `tests/run_test.sh --package` |
| how often | all the time | before a release, once every few days or weeks |

A development build copies nothing and packages nothing. There is no
`--no-prod`: not packaging *is* the default, and the driver rejects the old
flag rather than silently reinterpreting it.

## 2. The four switches

Spelled the same on every platform (`-Config`, `-Logs`, `-Symbols`, `-Tier`
in PowerShell):

| switch | values | default | under `--prod` |
|---|---|---|---|
| `--config` | `debug` \| `release` -- optimisation (`--debug`, `--release` are aliases) | release | unchanged |
| `--logs` | `on` \| `off` -- `MT_DEBUG_LOGS`: are the verbose log macros compiled | on | **off** |
| `--symbols` | `on` \| `off` -- `MT_RELEASE_SYMBOLS`: does the shipped binary keep debug info | on | **off** |
| `--tier` | `dev` \| `commercial` -- `MT_COMMERCIAL_BUILD`: the licence tier | dev | unchanged |

**An explicit switch always beats what `--prod` implies.** A commercial tier
is always stripped: `--symbols on --tier commercial` is refused. Every driver
prints one line at the start so there is no guessing:

```
Build: config=release logs=off symbols=off tier=dev prod=yes
```

`--logs` and `--symbols` are `--set` overrides underneath, so they key the
build tree: a `--prod` build and a development build never share objects, and
`mtcaps check` re-verifies them. Debug info is *always generated* -- the
dSYM/PDB/`.debug` lands in `$MT_OUT/symbols/` -- and `--symbols` decides only
what the binary you ship carries.

```sh
./build-macos.sh                            # development: logs on, symbols on
./build-macos.sh --config debug             # same, unoptimised
./build-macos.sh --prod                     # final: package, logs off, symbols off
./build-macos.sh --prod --logs on           # a DIAGNOSTIC package: deployed shape, verbose logs
./build-macos.sh --prod --tier commercial   # a store build
./build-linux.sh --prod                     # identical on Linux
.\build-windows.ps1 -Prod -Logs on          # identical on Windows
```

Windows additionally has `-Platform x64|ARM64`, `-Compiler Clang|MSVC` and
`-SkipCuda`, because those are Windows-only facts. `--clean`, `--skip-deps`
and `--set KEY=VALUE` exist everywhere.

## 3. The working directory is the git root

The engine resolves `assets/` -- and everything else an application opens by a
relative path -- through the process working directory and nothing else. The
executable's own location is never consulted. So a development build runs from
the directory the repository is checked out in, on every platform:

| launcher | how the root is set |
|---|---|
| `tests/run_test.sh` | does it for you, and exports `MT_TEST_PROJECT_DIR` |
| Xcode | the shared scheme's custom working directory, `$(SRCROOT)/../..`, tracked |
| Visual Studio | `LocalDebuggerWorkingDirectory = $(ProjectDir)..\..\..`, in the tracked `.vcxproj` |
| by hand | `cd <root> && <path-to-binary> --headless ...` |

If ⌘R or F5 starts the application somewhere else -- DerivedData, the
solution directory, `bin/` -- **the project file is broken; fix the project
file.** On Windows the usual cause is a stale `<App>.vcxproj.user`: MSBuild
imports it *after* the project, so a working directory set there wins. Delete
it; it is not tracked. To run one test under F5, set *Debugging > Command
Arguments* to `--headless --run-test AppStartup --exit-after-tests` -- Visual
Studio writes that into a fresh `.vcxproj.user`, which is fine as long as you
leave *Working Directory* at its inherited value. In Xcode the same lives in
the scheme's *Arguments* tab.

**"Assets not found" is a working-directory problem, never an asset
problem.** Do not copy `assets/`. Do not add a fallback search path. Do not
stage anything into `prod/`. Do not add a per-repository workaround. Fix the
working directory. Every one of those "fixes" has been made before, once per
repository, and each left a different mess.

## 4. Running tests

Two suites: **CTestSuite** (`--run-suite`, integration tests on the render
thread) and **imgui_test_engine** (`--run-tests`, UI automation). Always
`--headless` for an automated run.

```sh
tests/run_test.sh                      # development build, both suites, from the git root
tests/run_test.sh AppStartup           # one CTestSuite test by name
tests/run_test.sh --skip-build         # do not rebuild first
tests/run_test.sh --imgui              # the UI suite only (c64d: --imgui-tests)
tests/run_test.sh --package            # FINAL build only: from platform/<P>/prod/<arch>/
```

The UI suite flag is `--imgui` everywhere (c64d also accepts its older
`--imgui-tests` and `--imgui-test <filter>`). On Windows the same script runs
under Git Bash (`bash tests/run_test.sh --binary <exe>`, which is what CI
does); an application may additionally provide `tests\run_test.ps1` with
the same contract (`-Package`).

`MT_IMGUI_TEST_FILTER=<names>` narrows the UI suite to the named tests
(imgui_test_engine filter syntax, comma-separated) for a red/green loop on one
test; the others are reported as not run. The whole suite is still the proof.

By hand, from the root. Where a development binary is:

| platform | development binary |
|---|---|
| macOS | `build-macos/Build/Products/<Config>/<App>.app/Contents/MacOS/<App>` after `./build-macos.sh`; `~/Library/Developer/Xcode/DerivedData/<App>-*/Build/Products/<Config>/...` after an Xcode build (the runner should pick the newest across both; `tests/run_test.sh` prints `Using binary:`) |
| Linux | `build/<target>` (the CMake target name from `mtengine-app.conf`, e.g. `build/retrodebugger`) |
| Windows | `platform\Windows\bin\<Platform>\<Configuration>\<App>.exe` |

```sh
<binary> --headless --log-dir /tmp --run-suite --exit-after-tests          # CTestSuite
<binary> --headless --log-dir /tmp --run-test AppStartup --exit-after-tests # one test
<binary> --headless --log-dir /tmp --run-tests --exit-after-tests          # UI suite
```

(`--log-dir` takes any directory; on Windows use e.g. `%TEMP%\mt-tests`.)

**Which binary ran.** Every app-build driver ends with
`=== Built binary: <absolute path> (<Config>, <mtime>) ===` (plus
`=== Deployed binary: ... ===` under `--prod`/`-Prod`), printed after the strip
and re-sign so the mtime is final. Compare it with the runner's
`Using binary:` line and with the app's own startup line where the app prints
one (for example `MyApp: running <path>`). Two different paths mean a
stale binary was run; an app's runner can also print a `WARNING:` when a newer
binary sits at another known location.

* **Results** always land in `<root>/tests/results/last_run.txt`; the runner
  passes that path absolutely in both modes. A `RESULT: n/m passed` line ends
  it; `SKIPPED` tests did not run and are counted separately -- a skip is
  never a pass, and a missing fixture is a failure, never a skip.
* **Logs** go to `--log-dir` (the runner uses `/tmp`), one file per run --
  macOS `<app>-YYMMDD-HHMM.txt`, Linux `MTEngine-YYMMDD-HHMMSS-<pid>.txt`,
  Windows `MTEngine-YYYYMMDD-HHMMSS-<pid>.txt`. `LOG_GetLogFilePath()` returns
  the file from inside the process.
* **Timeouts** are per suite (`--timeout N`); a run that dies without a
  results file is reported as such, never as the previous run's verdict.
* `--package` runs exactly what the default run runs (both suites, or the
  one you selected) -- it changes only the working directory. It is an
  **error, not a fallback**, when there is no package: build one with
  `--prod` first.
* `MT_TEST_RUN_DIR` pins the working directory outright and exists for tests
  *of* the runner.

### 4a. A UI test runs on the coroutine thread, not the main thread

A `TestFunc` executes on imgui_test_engine's coroutine thread. The **main**
thread dispatches it from inside `ImGui::EndFrame()` -- i.e. from the middle of
`VID_Render()` -- and then blocks until the test yields. So a UI test is not
running "on the main thread", however much it looks like it.

`VID_Render()` holds the render mutex (which *is* `guiMain`'s mutex) across the
whole frame. Calling app code from a test that locks `guiMain` therefore parks
on a mutex only the blocked main thread can release: the app hangs with no
output and the runner reports "results file not found". The mutex being
recursive does not save it -- recursion is per-thread, and this is a different
thread.

`CImGuiTestEngine.cpp` wraps the coroutine interface so the main thread hands
that lock back for the duration of each dispatch and takes it again afterwards.
The test then locks and unlocks normally, which keeps real mutual exclusion
against the emulation threads rather than papering over it. This is test-build
only; nothing changes in a production build.

Two things still follow for test authors:

* **Set up preconditions by calling the app directly; drive the UI for the
  thing under test.** `ctx->MenuClick(...)` exercises the real user path, but
  it only works if the item is reachable in whatever layout the run loaded, so
  it is a poor way to get an emulator started before the actual assertions. A
  direct call is deterministic and, since the bridge above, safe.
* **A hang with no output is this bug until proven otherwise.** Diagnose it with
  a stack sample (`sample <pid>`), not by guessing: the main thread will be in
  `Coroutine_ImplStdThread_Run` and the "Dear ImGui Test Thread" in
  `__psynch_mutexwait`.

## 5. Fixtures: `CTest::ResolveProjectPath()`

A test opens every fixture through the resolver and never by a literal path:

```cpp
std::string d64 = CTest::ResolveProjectPath("tests/data/bitbreaker.d64");
ASSERT_TRUE(!d64.empty() && SYS_FileExists(d64.c_str()), "the fixture is present");
```

It returns `<project root>/<relative>`. The root is `MT_TEST_PROJECT_DIR` when
set, otherwise the nearest ancestor of the directory the process *started* in
that holds `mtengine.caps` or `.git`. From the git root it is the identity;
from `prod/` it walks up four levels. Resolved once, eagerly, in
`CTestSuite`'s constructor, so a host that changes its working directory later
cannot make it latch the wrong place. `""` means no root: **fail the test,
never skip.** Anything a test *writes* goes under `tests/results/`, through the
same call, so a `--package` run leaves `prod/` untouched.

## 6. Logging

One header, `src/Engine/Core/DBG_Log.h`, one level map on every platform.

| macro | level | in a `--logs off` build |
|---|---|---|
| `LOGFatal`, `LOGError` | always on | **written**, to the log file on every platform, and also to stderr (macOS, Linux) or `OutputDebugString` (Windows) |
| `LOGWarning`, `LOGM`, `LOGD`, `LOGD2`, `LOGG`, `LOGF` (paint), the rest | gated by the level mask | compiled to nothing |

* `MT_DEBUG_LOGS` is the compiler define behind `--logs`. Never edit
  `DBG_Log.h` to get output; build with `--logs on`.
* The level mask is a runtime bit set (the Debug Log view, persisted as
  `LogLevel2`); `DBGLVL_DEFAULT_MASK` turns everything on except paint,
  memory, VICE-verbose and debug2.
* `--log-dir <dir>` is read before anything else runs, on all three platforms.
  Without it: macOS `~/Library/Caches`, Linux `${XDG_CACHE_HOME:-~/.cache}/MTEngine`,
  Windows `%TEMP%\MTEngine` -- never the working directory.
* Windows also mirrors the log to `LogConsole.exe`, found beside the
  executable (or the legacy `platform/Windows/_RUNTIME_/`), never in the
  working directory.
* **The test harness's own progress** (`[TEST] CTestSuite: Running test
  n/m: Name`, `Completed OK/FAILED`, `SUITE RESULTS`) is not a log level:
  with logs off it goes to stderr directly, so a run that died still names
  its test.
* **A headless run that crashed**: read the newest file in `--log-dir`; with
  `--logs off` it holds errors, fatals and the `[TEST]` lines only, which is
  still the whole story. A signal (SIGSEGV, SIGABRT) does not go through
  `LOGFatal`: the crash reporter installed at startup writes a report with the
  backtrace to `<settings dir>/crash-reports/` (`/tmp/crash-reports/` when
  there is no settings dir yet), and `SYS_FatalExit` writes its message
  through `LOGFatal` before aborting.

## 7. The final-build procedure

1. `./build-<os>.sh --prod` -- a full build plus the package, `logs=off
   symbols=off`. For a diagnostic package add `--logs on`; for a store build
   add `--tier commercial`.
2. `tests/run_test.sh --package` -- the whole suite, from the package. This is
   the only run that verifies the packaging: assets present, payload present,
   `LICENSES.txt` present, nothing reached by a path that exists only in a
   checkout.
3. `prod/` must contain nothing the deploy stage did not put there -- no
   `tests/`, no agent workspace, no ops scripts, no project sources. An
   application that
   writes runtime state under its asset path (a game's `cache/` or save
   directory, say) says so in its own guidance; that is design, not a leak.
4. Debug symbols for the shipped build are in `$MT_OUT/symbols/` (the driver
   prints the path). Keep them with the release.
5. Package `prod/` -- zip, notarise, upload -- by that application's own
   release procedure. Whether that procedure consumes `prod/` directly or
   builds through the same drivers (c64d's `tools/make-release/` does the
   latter, and `MT_LOGS=on|off` reaches them), it never assembles a package
   by hand from the build tree.

## 8. CI

Every application's three workflows (`build-macos`, `build-linux`,
`build-windows`) run on every push with a `logs: ["on", "off"]` matrix. Both
legs run the same steps -- the development build, then the suites from the
git root -- so the `off` leg is the shipping shape (`MT_DEBUG_LOGS=0`) built
*and tested*, which used to be discovered at release time. (c64d's Linux and
Windows workflows build through `tools/make-release/` and run no suite; its
macOS one does.) Artifacts carry `-logs-<on|off>`. Nothing in CI touches
`prod/`. The uploaded artifact is the binary; a failing run's log is in the
job's own output, since the harness writes `[TEST]` lines to stderr.

## 9. What to do when

| symptom | cause | the one fix |
|---|---|---|
| "assets not found", black window, `RES_ResolveResourceDir ... not found` | the working directory is not the git root (IDE started in DerivedData / the solution dir) | fix the scheme / `.vcxproj` working directory (#3); delete a stale `.vcxproj.user` |
| no log output at all | you are reading stdout of a `--logs off` build, or looking in the wrong directory | `--logs on` for verbose; errors are already in `--log-dir` (#6) |
| a test passes alone and fails in the suite | order-dependent state left by an earlier test | fix the earlier test's teardown; never reorder or skip |
| `--package` run fails, root run passes | a fixture opened by a literal path | `CTest::ResolveProjectPath()` (#5) |
| root run fails, `--package` run passes | production code depends on something only the package has | name it in `MT_APP_PAYLOAD` and reach it through `gPathToResources` |
| Windows F5 starts in the wrong directory | `.vcxproj.user` overrides the tracked setting | delete it |
| `ERROR: --no-prod is gone` | an old script or habit | drop the flag; a build makes no package unless `--prod` |
| `ERROR: --package given but no release package` | there is no `prod/` | `./build-<os>.sh --prod` first |
| `--symbols on` refused | `--tier commercial` | a store build is always stripped; use `--symbols on` on a dev-tier build |
| the build says `logs=off` but you wanted output | `--prod` defaulted it | `--prod --logs on` |
| CI green locally red, or vice versa | different `logs`/`config` leg | reproduce with the same switches; the driver prints them |

## 9a. White-box tests reach internals through `friend`, never `#define private public`

A test that has to see a class's internals declares itself a friend of that
class. It does **not** wrap the include in
`#define protected public` / `#define private public`.

That trick is not portable, and the way it fails is expensive: the Itanium C++
ABI (macOS, Linux) does not encode member access in a mangled name, so the trick
links there and looks correct. The MSVC ABI *does* encode it -- `Q` for public,
`I` for protected, `A` for private -- so the mutated header makes the test
reference `public:` manglings of every out-of-line member it reaches, including
the ones it reaches indirectly through the class's own public inline wrappers.
Nothing defines those symbols. The result is a link error in the *app*, naming
functions that plainly exist:

```
lld-link : error : undefined symbol: public: void __cdecl TextEditor::selectAll(void)
```

`dumpbin /SYMBOLS` on the defining object settles it in one line: the definition
is `?selectAll@TextEditor@@IEAAXXZ` (`I`), the reference wants `Q`.

`CTestTextEditorMutation` is the worked example -- it needs four protected data
members of `TextEditor` and nothing else, so one `friend class` declaration in
the header replaces the macros. Found 2026-09-09, the first time that test was
compiled on Windows.

## 10. Never do this

* Never edit `DBG_Log.h` (or any source) to turn logging on or off.
* Never copy `assets/`, add a search path, or stage files into `prod/` to
  make a binary find something. Fix the working directory.
* Never open a fixture by a literal `"tests/..."` path or a `../../..` list.
* Never write test output anywhere but under `tests/results/` via the
  resolver.
* Never run the binary from DerivedData or a solution directory and then
  "fix" what breaks.
* Never pass `--no-prod` or `-NoProd`; never zip a `prod/` that was not
  produced by the build you are shipping.
* Never treat a `SKIPPED` test as a pass, or a missing fixture as a skip.
* Never reach a class's internals with `#define private public` at the include
  site -- see #9a. Declare the test a `friend`.
