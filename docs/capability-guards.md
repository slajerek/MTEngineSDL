# Guarding engine code on a capability

A capability that does not remove code is a manifest entry, not a capability.
Two of them were exactly that until 2026-09-06: `MT_ENABLE_HTTPLIB` and
`MT_ENABLE_ENET` appeared in no source file, so turning HTTP or the network
transport off changed the resolved set and the caps-hash and left the binary
unchanged.

This note is how to add a guard so that does not happen again.

## The idiom: guard the body, keep the file

Never remove a file from a build list. An Xcode `PBXBuildFile` cannot be
conditioned on a build setting, the way a CMake or MSBuild item can, so a
build-system exclusion is silently macOS-divergent. Instead the file stays in
all three lists and compiles to nothing:

```cpp
// A .cpp
#if MT_ENABLE_X

... everything ...

#endif // MT_ENABLE_X
```

```cpp
// A .h -- INSIDE the include guard, so the header stays includable and simply
// declares nothing when the capability is off.
#pragma once

#if MT_ENABLE_X
... the class ...
#endif // MT_ENABLE_X
```

`#if`, never `#ifdef`: the generator emits `MT_ENABLE_X=0` rather than absence,
and `#ifdef` is true for a macro defined as 0.

## Anything an un-guarded caller still calls needs an `#else`

`NET_Initialize()` is the worked example. `Core/SYS_Startup.cpp` calls it on
two paths with no guard of its own, so the off branch defines an empty one.
`CImageData::LoadTIFF` does the same and logs an error. A test class needs
every method its vtable references, including the destructor and `Cancel()` --
a guard that swallows one of them fails at link, not at compile.

## Before writing the guard, check three things

1. **Is the capability `app_visible`?** If any app includes a header you are
   about to guard, it must be. `engine_only_defines()` keeps an engine-only
   flag out of `MT_CAPS_DEFINES`, the app xcconfigs read only that set, and
   the Windows IDE define file omits it -- so an app-side `#if` on it
   evaluates to 0 on macOS and Windows whatever the capability resolved to,
   while Linux gets it right, because `mt_apply_capabilities` marks every
   define `PUBLIC`. A guard that disagrees across platforms is worse than no
   guard.

   The criterion is FFmpeg's, not a grep for existing guards: **a flag is
   app-visible if it appears in a header app translation units include.**

2. **Does the capability have a flag at all?** `MT_CAP_NET_GAME` had
   `"enables": []`, so its 17 sources had nothing to be gated on.

3. **Does the flag have an `option()` in `CMakeLists.txt`?** Without one a
   standalone `cmake` leaves it undefined, which a value-style `#if` reads as
   0 -- so the feature silently vanishes from a plain engine build.

## Compute the closure, do not guess it

A class that never names the library still depends on it through a header. The
17 net-game classes guarded on `MT_ENABLE_NET_GAME` were found by taking the
transitive closure of local includes from the four core net headers, and 12
sibling classes in the same directory correctly needed no guard at all.

## Prove it

`tools/appbuild/scan-capability-symbols.sh <binary> <MT_OUT>` reads the
resolved set and the vocabulary's symbol patterns and fails if a capability
that is off has left its symbols in the binary. `capability_matrix.sh` runs it
after every variant. That gate is what found both of these, and it is the
acceptance test for any new guard.
