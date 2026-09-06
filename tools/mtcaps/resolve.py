"""Manifest parsing, the four resolution rungs, and the canonical resolved form."""

import hashlib
import os
import re
import subprocess

from errors import ManifestError
from vocab import COMMERCIAL_KEY, DEBUG_LOGS_KEY, PLATFORMS, PRIVATE_KEY, SYMBOLS_KEY

# bash `source` is the binding constraint on key spelling and it is stricter than
# PowerShell's ConvertFrom-StringData or CMake's file(STRINGS). A key must be a
# legal shell identifier; anything else is not an assignment, so the shell tries
# to run the line as a command, and every build-*.sh runs `set -e` -- a hard
# build abort, not a warning. Measured: `MT_CAP_HTTPS.linux=0` gives
# "command not found" and exit 127.
SHELL_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

OS_SUFFIXES = {"__LINUX": "linux", "__MACOS": "macos", "__WINDOWS": "windows"}


def parse_manifest(path):
    """KEY=VALUE, one per line. Whole-line `#` comments allowed, inline comments
    forbidden -- ConvertFrom-StringData keeps an inline comment as part of the
    value, so a manifest that reads fine in bash silently means something else on
    Windows. Returns [(key, value, lineno)] in file order."""
    if not os.path.isfile(path):
        raise ManifestError("manifest not found at %s" % path)

    entries = []
    with open(path, "r", encoding="utf-8-sig") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\r\n").strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                raise ManifestError(
                    "%s:%d: not a KEY=VALUE assignment: %r" % (path, lineno, line))
            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip()

            if not SHELL_IDENT_RE.match(key):
                raise ManifestError(
                    "%s:%d: %r is not a legal shell identifier. The manifest is "
                    "read by bash `source`, PowerShell ConvertFrom-StringData and "
                    "CMake file(STRINGS) without a parser, and bash is the "
                    "strictest of the three: a key it cannot assign becomes a "
                    "command, and `set -e` turns that into a build abort"
                    % (path, lineno, key))

            if "#" in value:
                raise ManifestError(
                    "%s:%d: inline comments are forbidden (%r). Whole-line `#` "
                    "comments are fine and are the intended way to document a "
                    "key; an inline one survives into the value under "
                    "ConvertFrom-StringData" % (path, lineno, raw.strip()))

            if value not in ("0", "1"):
                raise ManifestError(
                    "%s:%d: %s=%r -- values are 0 or 1 only, never ON/OFF/true/false"
                    % (path, lineno, key, value))

            entries.append((key, int(value), lineno))
    return entries


def _split_suffix(key):
    for suffix, plat in OS_SUFFIXES.items():
        if key.endswith(suffix):
            return key[: -len(suffix)], plat
    return key, None


def resolve(vocab, manifest_path, platform, overrides=None):
    """The four rungs, highest wins:

      1. an override (`--set KEY=VALUE`), persisted so `check` re-resolves identically
      2. an explicit manifest key, OS-suffixed beating unsuffixed
      3. an implication from another capability
      4. the engine default for an unlisted key

    Returns (values, provenance) where values maps every KNOWN capability plus
    MT_COMMERCIAL_BUILD to 0/1, and provenance maps each to the rung that set it.
    """
    if platform not in PLATFORMS:
        raise ManifestError("unknown platform %r; expected one of %s" % (platform, PLATFORMS))

    overrides = dict(overrides or {})
    entries = parse_manifest(manifest_path)

    known = set(vocab.keys) | {COMMERCIAL_KEY, PRIVATE_KEY, SYMBOLS_KEY, DEBUG_LOGS_KEY}

    # --- rung 2: explicit manifest keys, suffixed beating unsuffixed -----------
    explicit = {}
    explicit_suffixed = {}
    for key, value, lineno in entries:
        base, plat = _split_suffix(key)
        if base not in known:
            raise ManifestError(
                "%s:%d: unknown capability %s. The engine owns the vocabulary "
                "(%s); an app cannot invent a key, because a key nothing reads is "
                "configuration that cannot change the build"
                % (manifest_path, lineno, base, vocab.path))
        if plat is None:
            explicit[base] = value
        elif plat == platform:
            explicit_suffixed[base] = value

    resolved_explicit = dict(explicit)
    resolved_explicit.update(explicit_suffixed)

    # --- rung 4 first, then 2, so explicit keys overwrite defaults -------------
    values = {key: vocab.default(key) for key in vocab.keys}
    values[COMMERCIAL_KEY] = 0  # off means non-commercial; there is no third state
    # Default 0 = "assume this gets distributed". The safe direction: an app
    # that never mentions the key gets the RESTRICTED treatment, so forgetting
    # to declare a tier can never widen what ships.
    values[PRIVATE_KEY] = 0
    # Debug symbols in Release: default ON (open-source and dev builds want
    # them; decision 0.5) -- the COMMERCIAL forcing below is what protects the
    # store artifact, not this default.
    values[SYMBOLS_KEY] = 1
    # Debug logs: default ON -- a development build wants them, and "on" is the
    # safe direction (a forgotten switch shows as verbose output, never as a
    # silent binary). The build driver passes --set MT_DEBUG_LOGS=0 for a
    # --prod package; nothing here forces it.
    values[DEBUG_LOGS_KEY] = 1
    provenance = {key: "default" for key in values}

    for key, value in resolved_explicit.items():
        values[key] = value
        provenance[key] = "manifest"

    # --- rung 1: overrides ----------------------------------------------------
    for key, value in overrides.items():
        base, plat = _split_suffix(key)
        if base not in known:
            raise ManifestError("--set names unknown capability %s" % base)
        if plat is not None and plat != platform:
            continue
        values[base] = value
        provenance[base] = "override"

    # --- rung 3: implication, downward only, to a fixed point ------------------
    #
    # A genuine conflict is a configure-time error and an explicit `=0` never
    # silently overrides an implication. If X=1 implies Y=1 and the manifest also
    # says Y=0, the app has asked for something incoherent and only its author can
    # say which half was meant. Rung 2 beating rung 3 applies to an implication the
    # manifest is SILENT about, not to one it contradicts.
    changed = True
    while changed:
        changed = False
        for key in vocab.keys:
            if values[key] != 1:
                continue
            for target in vocab.implies(key):
                if values[target] == 1:
                    continue
                if provenance[target] in ("manifest", "override"):
                    raise ManifestError(
                        "conflict: %s=1 implies %s=1, but %s is explicitly 0 "
                        "(%s). Both keys are named because only the manifest's "
                        "author can say which half was meant -- turn %s off, or "
                        "turn %s on."
                        % (key, target, target,
                           "an override" if provenance[target] == "override" else manifest_path,
                           key, target))
                values[target] = 1
                provenance[target] = "implied by %s" % key
                changed = True

    # --- the two mode keys are mutually exclusive ------------------------------
    #
    # "Sold through an app store" and "never handed to anyone" cannot both be
    # true of one artifact. Setting both is not a preference to be silently
    # reconciled -- it says the author has not decided what this build IS, and
    # the two answers have opposite eligibility rules. Same shape, and the same
    # reasoning, as the implication conflict above: name both keys, refuse to
    # choose.
    if values[COMMERCIAL_KEY] == 1 and values[PRIVATE_KEY] == 1:
        raise ManifestError(
            "%s: %s=1 and %s=1 together. A commercial build is distributed and "
            "a private build is not, so this build is one or the other. Set "
            "%s=0 for the app-store artifact, or %s=0 for the local-only one."
            % (manifest_path, COMMERCIAL_KEY, PRIVATE_KEY,
               PRIVATE_KEY, COMMERCIAL_KEY))

    # --- symbols forcing (decision 0.5) ----------------------------------------
    #
    # A store artifact must never carry debug symbols by accident. At
    # MT_COMMERCIAL_BUILD=1 the key is FORCED to 0 -- overriding even an
    # explicit manifest line, because a manifest default is exactly the
    # accident this exists to prevent. Only rung 1 (--set on THIS invocation,
    # the deliberate UAT build) escapes, and the escape is visible in the
    # resolved string.
    if values[COMMERCIAL_KEY] == 1 and values[SYMBOLS_KEY] == 1 \
            and provenance.get(SYMBOLS_KEY) != "override":
        values[SYMBOLS_KEY] = 0
        provenance[SYMBOLS_KEY] = "forced by %s=1" % COMMERCIAL_KEY

    # --- commercial effects, applied AT RESOLVE (decision 0.1) -----------------
    #
    # Precedence, fixed by the unification plan: (1) the rungs above resolve
    # the manifest; (2) at MT_COMMERCIAL_BUILD=1 each capability's
    # `commercial` effect is applied -- `capability-off` turns the capability
    # off HERE, so fragments, LICENSES.txt and the out_dir hash all see the
    # post-effect set and cannot disagree; (3) only then does the
    # commercial_safe deny-list run (check_commercial_safe below), so a dep
    # an effect just turned off never errors. `variant` stays a build-time
    # concern of the codec scripts (forbidden decoder subsets), not a flag
    # change. Historically resolve deliberately did NOT do this ("MT_COMMERCIAL_BUILD
    # is deliberately NOT here"); that note described the flag SET, and the
    # deliberate part -- no =0 emission for the mode keys -- still holds.
    if values[COMMERCIAL_KEY] == 1:
        for key in vocab.keys:
            if values[key] == 1 and vocab.get(key)["commercial"]["effect"] == "capability-off":
                values[key] = 0
                provenance[key] = "commercial: capability-off"

    return values, provenance


def check_commercial_safe(vocab, values, platform=None, manifest_path=None):
    """The deny-list (decision 0.2): a MT_COMMERCIAL_BUILD=1 resolve must make
    it impossible to compile in a dependency that is not commercial-safe.

    Runs over the FINAL state -- after commercial effects and after the
    PLATFORM_UNAVAILABLE/PRIVATE_ONLY forcing inside enabled_flags() -- so a
    dependency whose capability or gating flag ended 0 is not "enabled" and
    must not error (libheif under MT_CAP_PHOTO_CODECS=1 is the canonical
    case). A dep row may name its gating MT_ENABLE_* in `flag`; absent means
    the capability itself is the gate."""
    if values[COMMERCIAL_KEY] != 1:
        return
    flags = enabled_flags(vocab, values, platform)
    for key in vocab.keys:
        if values[key] != 1:
            continue
        for dep in vocab.get(key)["dependencies"]:
            if dep["commercial_safe"]:
                continue
            gate = dep.get("flag")
            if gate is not None and flags.get(gate, 0) != 1:
                continue
            raise ManifestError(
                "MT_COMMERCIAL_BUILD=1 would enable %r (licence: %s), which is "
                "marked commercial_safe: false in the vocabulary. It is pulled "
                "in by %s%s. Turn that capability off for the store build, or "
                "-- after settling the licence -- flip the vocabulary field."
                % (dep["name"], dep["licence"], key,
                   "" if gate is None else " via %s" % gate))

    # The APPLICATION's own rows (mtengine-app-licenses.json beside the
    # manifest) sit under the same guard. They have no capability to turn off:
    # the app either stops embedding the thing or settles its licence.
    if manifest_path is not None:
        from vocab import load_app_licences, APP_LICENCES_FILE
        for dep in load_app_licences(manifest_path):
            if dep["commercial_safe"]:
                continue
            raise ManifestError(
                "MT_COMMERCIAL_BUILD=1 would ship %r (licence: %s), which the "
                "application declares in %s with commercial_safe: false. Stop "
                "embedding it for the store build, or -- after settling the "
                "licence -- flip the field."
                % (dep["name"], dep["licence"], APP_LICENCES_FILE))


def canonical_form(vocab, values):
    """#4.2, pinned byte-for-byte because two implementers reading "keys sorted,
    joined by `;`" produce two different strings and the check is a comparison.

      * every KNOWN capability, not only those the manifest listed, so rung 4 is
        materialised;
      * MT_ENABLE_* excluded, MT_COMMERCIAL_BUILD and MT_PRIVATE_BUILD included
        -- as 0 or 1, because the check has to tell "off" from "unset", which is
        the one distinction that matters for a mode flag. MT_PRIVATE_BUILD is
        here for the same reason MT_COMMERCIAL_BUILD is, and additionally
        because it CHANGES WHICH LIBRARIES ARE LINKED: a private and a public
        build of an otherwise identical manifest must not share one $MT_OUT, or
        one would silently reuse the other's archives;
      * KEY=VALUE, the same spelling as the manifest;
      * values 1 or 0 only, OS suffixes already resolved away;
      * sorted by byte value (C locale), joined by `;`, NO trailing separator;
      * MT_RELEASE_SYMBOLS included since 2026-08-31 (decision 0.5) -- the
        agreement check must cover it, and a UAT and a store artifact must
        never share one $MT_OUT.
    """
    keys = sorted(list(vocab.keys) + [COMMERCIAL_KEY, PRIVATE_KEY, SYMBOLS_KEY, DEBUG_LOGS_KEY])
    return ";".join("%s=%d" % (k, values[k]) for k in keys)


def deps_key_capabilities(vocab):
    """The capabilities that can change a third-party archive: exactly those
    with an `acquisition` entry in the vocabulary (L16, 2026-09-03).

    Declared intent, not inference. A capability with an acquisition script
    OWNS an archive in the deps bucket, so its value must key that bucket;
    one without has nothing there to key. The set is a deliberate SUPERSET of
    what the scripts are measured to read -- uSockets is acquired
    unconditionally today and MT_CAP_WEBSOCKETS still keys the bucket -- and
    superset is the safe direction: a key that carries more than it needs
    wastes a bucket, a key that carries less REUSES A WRONG ARCHIVE.

    test_deps_key_covers_every_capability_the_scripts_read is the gate on the
    unsafe direction: it strips comments from every platform/*/build-*
    script, collects the vocabulary flags each one actually reads, and fails
    if any of them belongs to a capability outside this set."""
    return sorted(k for k in vocab.keys if vocab.acquisition(k))


def deps_form(vocab, values):
    """The key for the deps-dir hash: the ACQUISITION capabilities plus the two
    licence keys, and nothing else.

    It excludes MT_RELEASE_SYMBOLS because a third-party archive is
    byte-identical whatever it says (the key drives APP build settings), and
    since L16 it also excludes every capability that owns no dependency at
    all -- MIDI, GAMEPADS, UNDO, TERMINAL, the rest.

    WHY THAT CHANGE. The old form was the whole vocabulary, so flipping a
    capability no dependency has ever heard of moved the bucket, and a moved
    bucket is an EMPTY directory: the stamp check finds nothing and every
    dependency rebuilds from scratch to produce the same bytes. Measured on
    13 macOS buckets before this change: libSDL3.a had ONE distinct content
    across all of them (248 members, built 13 times), uSockets.a likewise,
    and 454 MB of 571 MB was duplicate. The cost was never mainly the disk;
    it was rebuilding FFmpeg and llama.cpp for a MIDI flag.

    The two licence keys stay: build-video_codecs.* reads
    MT_FFMPEG_BUILD_MODE, which derives from MT_PRIVATE_BUILD, and the
    commercial gate narrows the decoder set. They change what the archive
    CONTAINS, which is exactly what a key is for.

    NOT byte-identical to the pre-L16 form, so every existing bucket is
    orphaned once -- harmless (they are a cache), and mtengine-gc.py reclaims
    them on its normal retention."""
    keys = sorted(deps_key_capabilities(vocab) + [COMMERCIAL_KEY, PRIVATE_KEY])
    return ";".join("%s=%d" % (k, values[k]) for k in keys)


def ffmpeg_mode(values):
    """Which FFmpeg decoder set this build gets (decision 0.2, hardened).

    `full` ONLY for a private, never-distributed build. The withheld decoders
    (HEVC, AAC, the WMV/WMA family, VC-1, EAC3) are PATENT-encumbered, and
    patents attach to distribution, not payment -- so the public/free tier
    (COMMERCIAL=0, PRIVATE=0) gets the same restricted set the store build
    does. Before this, COMMERCIAL=0 alone selected `full`, which handed the
    encumbered set to every publicly distributed free build."""
    return "full" if values.get(PRIVATE_KEY, 0) == 1 else "commercial"


def caps_hash(canonical):
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:12]


# TWO SEPARATE REASONS an MT_ENABLE_* flag can be forced off even though the
# capability enabling it is on. They are kept apart deliberately: one is a
# fact about this machine that will change when someone does the work, the
# other is a rule about what may be shipped. Conflating them is how a
# temporary porting gap turns into a permanent policy nobody can find.

# 1. NOT OBTAINABLE ON THIS PLATFORM, YET. Purely an engineering gap.
#    libheif has never been vendored for Windows -- there is no
#    build-libheif.ps1 in any tier, commercial or not -- so there is nothing
#    to link and the flag must be off regardless of policy. macOS does not
#    vendor it either but does not need to (HEIF arrives via ImageIO, a
#    system framework, on a different code path this does not touch); Linux
#    takes it from a system package. Delete the entry here when the Windows
#    vendoring lands; nothing else needs to change.
#
#    Keyed by MT_ENABLE_* flag, NOT by MT_CAP_PHOTO_CODECS: TIFF, WebP and
#    AVIF are BSD-family and unrestricted, and forcing the whole capability
#    off to route around libheif alone would take all three down with it.
PLATFORM_UNAVAILABLE_FLAGS = {
    "windows": frozenset({"MT_ENABLE_LIBHEIF"}),
}

# 1b. THE PLATFORM ALREADY PROVIDES IT, permanently. Not a gap and not a
#     policy -- a fact about the operating system that no amount of work here
#     will change, and that we do not WANT to change.
#
#     macOS decodes HEIF through ImageIO. CImageDataHEIF.cpp dispatches to it
#     unconditionally on Apple platforms and never reaches the libheif path, so
#     compiling that translation unit asks the machine for a header it will not
#     use -- and, worse, would link an HEVC decoder into a binary whose HEVC
#     licence is otherwise Apple's, already paid for on the machine the code
#     runs on. Windows sits in the table above instead, because there the
#     reason really is a missing port; if that port ever lands, WIC still wins
#     in the dispatch and the entry moves here.
#
#     Kept apart from the table above on purpose. One says "nobody has done
#     the work yet" and invites someone to do it; this one says "do not". A
#     single table would lose that difference the first time somebody read it.
PLATFORM_PROVIDES_FLAGS = {
    "macos": frozenset({"MT_ENABLE_LIBHEIF"}),
}

# 2. DISTRIBUTION-RESTRICTED. A library whose terms are not settled for any
#    artifact that leaves the building. Allowed ONLY when MT_PRIVATE_BUILD=1.
#
#    libheif is here because of HEVC PATENT licensing, which is a different
#    question from its LGPL-3.0 copyright licence and from the app's own
#    licence. Patent pools attach to DISTRIBUTION, so an MIT, fully-public,
#    give-it-away-free project is NOT automatically clear -- being open
#    source settles the copyright question and says nothing about the patent
#    one. That is why this gate is MT_PRIVATE_BUILD and not "is this app
#    open source", and why it applies on top of MT_COMMERCIAL_BUILD rather
#    than instead of it: free public distribution sits between the two.
#
#    A capability's `commercial.forbidden_decoders_commercial` in
#    vocabulary.json is the OTHER shape -- one library, many codecs, withhold
#    some at runtime (FFmpeg/HEVC). This is the whole-library shape.
PRIVATE_ONLY_FLAGS = frozenset({"MT_ENABLE_LIBHEIF"})


def enabled_flags(vocab, values, platform=None):
    """The MT_ENABLE_* set the engine turns on. Every known flag appears, as
    `NAME=1` or `NAME=0`: emitting only the on ones would leave the off ones
    UNDEFINED, and the `#ifdef` family defaults off while the value style defaults
    on -- so "absent" means different things in different guards. Explicit 0 is
    what makes a guard sweep safe.

    Both override tables are applied AFTER the capability-driven computation, so
    they beat an otherwise-on capability -- the same precedence a rung-3
    implication has over rung 2, but for platform reality and distribution
    policy instead of manifest coherence.

    `platform=None` (the default) skips both platform tables; the distribution
    table always applies, because MT_PRIVATE_BUILD is in `values` and needs no
    caller cooperation to be correct. Passing None is therefore a bug in any
    caller that emits a fragment FOR a platform -- it produced an xcconfig
    saying MT_ENABLE_LIBHEIF=1 beside a props saying 0, from one resolve.

    MT_COMMERCIAL_BUILD is deliberately NOT here; see emit.py."""
    flags = {}
    for key in vocab.keys:
        for flag in vocab.enables(key):
            # A flag serving two capabilities is on if EITHER is on.
            flags[flag] = max(flags.get(flag, 0), values[key])
    for table in (PLATFORM_UNAVAILABLE_FLAGS, PLATFORM_PROVIDES_FLAGS):
        for flag in table.get(platform, ()):
            if flag in flags:
                flags[flag] = 0
    if values.get(PRIVATE_KEY, 0) != 1:
        for flag in PRIVATE_ONLY_FLAGS:
            if flag in flags:
                flags[flag] = 0
    return dict(sorted(flags.items()))


def app_visible_defines(vocab, values, platform=None):
    """What MT_CAPS_DEFINES carries: every MT_CAP_*, plus the MT_ENABLE_* flags of
    capabilities marked app_visible, plus MT_COMMERCIAL_BUILD **only when on**."""
    out = ["%s=%d" % (k, values[k]) for k in vocab.keys]
    flags = enabled_flags(vocab, values, platform)
    for key in vocab.keys:
        if vocab.app_visible(key):
            for flag in vocab.enables(key):
                out.append("%s=%d" % (flag, flags[flag]))
    if values[COMMERCIAL_KEY] == 1:
        out.append("MT_COMMERCIAL_BUILD=1")
    # Presence-style, same rule and same reason as the licence flag above.
    if values[PRIVATE_KEY] == 1:
        out.append("MT_PRIVATE_BUILD=1")
    # VALUE-style, always present: DBG_Log.h reads `#if MT_DEBUG_LOGS`, and a
    # value of 0 has to reach the compiler as 0, not as absence.
    out.append("%s=%d" % (DEBUG_LOGS_KEY, values[DEBUG_LOGS_KEY]))
    return sorted(set(out))


def engine_only_defines(vocab, values, platform=None):
    """MT_CAPS_DEFINES_ENGINE: the flags no app guards on. Measured from the
    vocabulary's app_visible field, which Phase 2 populated by grepping all four
    app src/ trees -- not by judgement. It may legitimately be non-empty or empty;
    the channel exists either way, because spike G question 8 showed a command-line
    setting cannot be scoped by the supplier and the split has to be made by the
    consumer instead."""
    flags = enabled_flags(vocab, values, platform)
    engine_only = set(flags)
    for key in vocab.keys:
        if vocab.app_visible(key):
            engine_only -= set(vocab.enables(key))
    return sorted("%s=%d" % (f, flags[f]) for f in engine_only)


def engine_rev(engine_dir):
    """`<short-head>[-dirty-<hash>]`.

    A short HEAD does not distinguish a dirty tree, and during the modularization
    programme the engine is dirty constantly -- every phase edits it. Two builds of
    "the same" revision with different uncommitted changes would otherwise share an
    output root and silently reuse stale archives. Refusing to build against a dirty
    engine is right for CI and unusable for the people writing the engine, which is
    everyone reading this."""
    def git(*args):
        # BYTES, not text: `git diff HEAD` includes binary file content when a
        # binary is staged (measured 2026-08-31: the staged deletion of 1226
        # vendored SDL2 files crashed the utf-8 decode at byte 0xb5), and the
        # output is only ever hashed, never read.
        return subprocess.run(
            ("git", "-C", engine_dir) + args,
            capture_output=True, check=False).stdout

    head = git("rev-parse", "--short", "HEAD").decode("ascii", "replace").strip()
    if not head:
        raise ManifestError("--engine-dir %s is not a git checkout" % engine_dir)

    status = git("status", "--porcelain", "--untracked-files=no")
    if status.strip():
        diff = git("diff", "HEAD")
        digest = hashlib.sha256(status + diff).hexdigest()[:8]
        return "%s-dirty-%s" % (head, digest)
    return head


def backend_hash(engine_options):
    """`<backend>`: engine build options that change the emitted artefact but are
    NOT capabilities -- MT_LLAMA_CUDA, MT_LLAMA_VULKAN, MT_GGML_NATIVE today. They
    select a different llama.cpp backend: a materially different archive under an
    identical capability set.

    They key the PATH and never join the canonical resolved form. Putting them in
    the form breaks the agreement check outright: `check` re-resolves from the
    manifest plus --overrides, and neither the manifest format nor the CLI has an
    input for these keys, so the build's form would contain keys the check cannot
    reproduce -- permanent exit 1. And it would fire deterministically today, since
    build-linux.sh forces MT_GGML_NATIVE=OFF on ARM against the option's ON default,
    failing every ARM64 Linux build."""
    if not engine_options:
        return "default"
    items = ";".join("%s=%s" % (k, v) for k, v in sorted(engine_options.items()))
    return hashlib.sha256(items.encode("utf-8")).hexdigest()[:8]


def default_engine_options(arch):
    """The engine options the BUILD DRIVERS pass for a given arch, as a dict.

    The rule -- MT_GGML_NATIVE follows the machine, OFF on ARM where the option's
    ON default miscompiles -- is implemented in four shell sites (both drivers,
    the Xcode pre-action, the decoder probe) because shell cannot import this
    module. Any PYTHON caller that needs to know which bucket a build actually
    lands in must use this, and not omit engine options: `backend_hash({})` is
    "default", a segment no real build on an ARM machine ever writes to. The GC
    computed liveness that way and would have deleted the live deps bucket the
    moment retention dropped to zero."""
    if str(arch).lower() in ("arm64", "aarch64"):
        return {"MT_GGML_NATIVE": "OFF"}
    return {"MT_GGML_NATIVE": "ON"}


def default_build_root():
    """Outside all five checkouts. `${XDG_CACHE_HOME:-$HOME/.cache}` and not a bare
    `$XDG_CACHE_HOME`: the variable is unset on macOS and on most Linux systems, so
    the bare form expands to `/mtengine`."""
    root = os.environ.get("MTENGINE_BUILD_ROOT")
    if root:
        return root
    if os.name == "nt":
        # %USERPROFILE%\.cache\mtengine, NOT %LOCALAPPDATA%: MSBuild's
        # FileTracker drops read/write tracking for paths under LOCALAPPDATA,
        # which killed incremental builds outright -- 519/519 TUs recompiled on
        # a no-change pass, tlogs with zero object entries (measured on Windows
        # 2026-09-01, HANDOVER item 11; maintainer decided the new default the
        # same day). The .cache form also mirrors the Unix default below.
        home = os.environ.get("USERPROFILE") or os.path.expanduser("~")
        return os.path.join(home, ".cache", "mtengine")
    cache = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(cache, "mtengine")


def out_dir(root, app, rev, platform, arch, config, mode, backend, chash):
    return os.path.join(root, app, rev, platform, arch, config, mode, backend, chash)


def build_dir(root, app, platform, arch, config, mode, backend, chash):
    """The REV-FREE build root (L9, 2026-09-01): compiled objects and the
    generated include/ live here, NOT under the rev-keyed out_dir.

    WHY: the rev key exists for artifacts whose staleness nothing else
    guards. Compiled objects have a guard -- MSBuild's tracker and CMake's
    dependency scan, the same machinery every normal in-checkout build
    trusts -- so keying them by engine rev bought correctness the build
    system already provides, at the price of a FULL engine rebuild per
    commit (and per dirty edit) on Windows and Linux, plus unbounded disk
    growth. The generated include/ moves too, because its CONTENT depends
    only on the resolved set and the vocabulary, while its PATH changing
    per rev invalidated every consumer's command line -- the tracker
    rebuilds on command-line change, so even untouched TUs recompiled.

    Everything that stays under out_dir (fragments, stamps, symbols,
    LICENSES) is either regenerated on every resolve or genuinely
    per-revision."""
    return os.path.join(root, app, "_build", platform, arch, config, mode,
                        backend, chash)


def deps_config(platform, config):
    """`<config>` for the deps path, and it is deliberately NOT just `config`.

    Windows third-party archives are configuration-specific: MSVC links a Debug
    CRT (/MDd) and a Release CRT (/MD) and the two cannot be mixed, which is why
    every Windows acquisition script builds `--config $Configuration` and already
    puts it in its stamp.

    macOS and Linux are the opposite case, and must NOT gain the component: the
    six platform/Linux/build-*.sh take no config argument at all, and a --debug
    and a --release macOS build resolve here identically today. Splitting there
    would double the cache and rebuild SDL3, FFmpeg, libvpx and llama.cpp to
    produce archives a Debug app links unchanged."""
    return config if platform == "windows" else "common"


def deps_dir(root, platform, arch, dconfig, backend, chash):
    """Third-party archives, and the ONE thing they are keyed by.

    NOT $MT_OUT, which is what this first used. That root also carries app,
    ENGINE REVISION, config and licence mode. Every one of those but the last is
    irrelevant to a third-party archive -- and engine revision is worse than
    irrelevant, it changes on every commit and on every dirty edit. Keying there
    does not cost "duplicated disk"; it costs a full rebuild of SDL3, FFmpeg,
    libvpx and llama.cpp per engine commit, per app, to produce a byte-identical
    archive.

    <caps-hash> is the discriminator that matters: a capability being OFF is what
    makes two archives differ, because the acquisition scripts write a stub
    archive in that case. <backend> joins it because MT_LLAMA_CUDA / GGML_NATIVE
    select a materially different llama.cpp under an IDENTICAL capability set --
    see backend_hash -- and no stamp carries a backend component, so only the
    path can tell those apart. <arch> joins it because only macOS builds
    universal archives; a Linux x64 and an ARM64 archive share a name and nothing
    else. <config> joins it on Windows only -- see deps_config.

    Correctness does not rest on this path in any case: every acquisition script
    stamps its output with its own full inputs (upstream sha, script sha,
    versions, deployment target, licence mode) and rebuilds on a mismatch. The
    key only decides how OFTEN that mismatch happens -- and four apps whose
    manifests resolve alike get one build instead of four."""
    return os.path.join(root, "_deps", platform, arch, dconfig, backend, chash,
                        "libs")


# ---------------------------------------------------------------------------
# THE BUILD-UNIT REGISTRY (L16)
#
# One row per producing script, per platform, saying WHICH INPUTS ITS OUTPUT
# DEPENDS ON. That is the whole of the per-unit key: a unit is rebuilt when
# something it reads changes, and not when something it has never heard of
# changes.
#
# WHY A REGISTRY AND NOT A DERIVATION. The obvious move is to read the
# vocabulary's `acquisition` rows -- one per capability, naming its script.
# They do not cover the units that matter most: SDL3, FreeType and libuv are
# `core.dependencies` with no capability at all, and uSockets is inline in
# build-macos.sh on macOS, a script on Linux and a core dep of build-deps.ps1
# on Windows. Those four are exactly the units that read NOTHING and would
# otherwise keep forking a bucket per capability flip. A derivation that
# cannot see them is not a derivation.
#
# WHY PER PLATFORM. The same dependency is a different unit on each platform:
# uSockets has a capability row on Linux and none elsewhere, and llama.cpp is
# one script on macOS and two on Windows.
#
# The `reads` lists are MEASURED, not guessed -- from what each script
# actually references, checked by TestDepsKey against the scripts themselves.
# `mode` means the unit reads MT_FFMPEG_BUILD_MODE, which mtcaps derives from
# the licence keys, so those two join its key. `backend` means the unit is
# built differently by MT_LLAMA_CUDA / MT_GGML_NATIVE.
BUILD_UNITS = {
    "macos": {
        "sdl3":          {"reads": [], "mode": False, "backend": False,
                          "scripts": ["platform/MacOS/build-sdl3.sh"]},
        "usockets":      {"reads": [], "mode": False, "backend": False,
                          "scripts": ["platform/MacOS/build-usockets.sh"]},
        "mbedtls":       {"reads": ["MT_CAP_HTTPS"], "mode": False, "backend": False,
                          "scripts": ["platform/MacOS/build-mbedtls.sh"]},
        "ftxui":         {"reads": ["MT_CAP_FTXUI"], "mode": False, "backend": False,
                          "scripts": ["platform/MacOS/build-ftxui.sh"]},
        "llama_cpp":     {"reads": ["MT_CAP_LLM"], "mode": False, "backend": True,
                          "scripts": ["platform/MacOS/build-llama_cpp.sh"]},
        "image_codecs":  {"reads": ["MT_CAP_PHOTO_CODECS", "MT_CAP_RAW",
                                    "MT_CAP_COLOR_MANAGEMENT"], "mode": False, "backend": False,
                          "scripts": ["platform/MacOS/build-image_codecs.sh"]},
        "video_codecs":  {"reads": ["MT_CAP_VIDEO_PLAYBACK"], "mode": True, "backend": False,
                          "scripts": ["platform/MacOS/build-video_codecs.sh"]},
    },
    # No llama_cpp: on Linux llama.cpp is an add_subdirectory of the app's own
    # CMake build, not a staged archive, so there is no producer to give a store.
    "linux": {
        "sdl3":          {"reads": [], "mode": False, "backend": False,
                          "scripts": ["platform/Linux/build-sdl3.sh"]},
        "usockets":      {"reads": [], "mode": False, "backend": False,
                          "scripts": ["platform/Linux/build-usockets.sh"]},
        "mbedtls":       {"reads": ["MT_CAP_HTTPS"], "mode": False, "backend": False,
                          "scripts": ["platform/Linux/build-mbedtls.sh"]},
        "ftxui":         {"reads": ["MT_CAP_FTXUI"], "mode": False, "backend": False,
                          "scripts": ["platform/Linux/build-ftxui.sh"]},
        "image_codecs":  {"reads": ["MT_CAP_PHOTO_CODECS", "MT_CAP_RAW",
                                    "MT_CAP_COLOR_MANAGEMENT"], "mode": False, "backend": False,
                          "scripts": ["platform/Linux/build-image_codecs.sh"]},
        "video_codecs":  {"reads": ["MT_CAP_VIDEO_PLAYBACK"], "mode": True, "backend": False,
                          "scripts": ["platform/Linux/build-video_codecs.sh"]},
    },
    # freetype and libuv are Windows-only units: the other two platforms take
    # them from the system rather than building them.
    "windows": {
        "sdl3":          {"reads": [], "mode": False, "backend": False,
                          "scripts": ["platform/Windows/build-sdl3.ps1"]},
        "freetype":      {"reads": [], "mode": False, "backend": False,
                          "scripts": ["platform/Windows/build-freetype.ps1"]},
        "libuv":         {"reads": [], "mode": False, "backend": False,
                          "scripts": ["platform/Windows/build-libuv.ps1"]},
        "usockets":      {"reads": [], "mode": False, "backend": False,
                          "scripts": ["platform/Windows/build-usockets.ps1"]},
        "mbedtls":       {"reads": ["MT_CAP_HTTPS"], "mode": False, "backend": False,
                          "scripts": ["platform/Windows/build-mbedtls.ps1"]},
        "ftxui":         {"reads": ["MT_CAP_FTXUI"], "mode": False, "backend": False,
                          "scripts": ["platform/Windows/build-ftxui.ps1"]},
        # One unit, two producers: the CPU and CUDA scripts are two spellings of
        # one dependency, told apart by <backend> rather than by unit name.
        "llama_cpp":     {"reads": ["MT_CAP_LLM"], "mode": False, "backend": True,
                          "scripts": ["platform/Windows/build-llama_cpp_cpu.ps1",
                                      "platform/Windows/build-llama_cpp_cuda.ps1"]},
        "image_codecs":  {"reads": ["MT_CAP_PHOTO_CODECS", "MT_CAP_RAW",
                                    "MT_CAP_COLOR_MANAGEMENT"], "mode": False, "backend": False,
                          "scripts": ["platform/Windows/build-image_codecs.ps1"]},
        "video_codecs":  {"reads": ["MT_CAP_VIDEO_PLAYBACK"], "mode": True, "backend": False,
                          "scripts": ["platform/Windows/build-video_codecs.ps1"]},
    },
}


# Scripts under platform/*/ that build-* naming makes look like producers but
# that own no store, because they own no output: they forward -OutLibDir to a
# leaf script and the leaf's store is the one that counts. Listed rather than
# pattern-matched so that a NEW dependency script fails the registry test until
# somebody says which of the two it is.
DISPATCH_SCRIPTS = frozenset({
    "platform/Windows/build-deps.ps1",       # runs the whole set in order
    "platform/Windows/build-llama-cpp.ps1",  # picks the CPU or the CUDA leaf
})


def build_units(platform):
    return BUILD_UNITS.get(platform, {})


def unit_form(unit, values):
    """The canonical string a unit's hash is taken over: the capabilities it
    reads, plus the licence keys when it reads the derived FFmpeg mode.

    The licence keys are NOT optional for such a unit. FFmpeg's CONTENT
    depends on them -- that is the whole of decision 0.2 -- so a full and a
    commercial build must never share a store."""
    keys = sorted(unit["reads"])
    if unit.get("mode"):
        keys += [COMMERCIAL_KEY, PRIVATE_KEY]
    return ";".join("%s=%d" % (k, values[k]) for k in sorted(keys))


def unit_store_dir(root, platform, arch, dconfig, unit_name, unit, values, backend):
    """Where a unit BUILDS and stamps. The view it copies into stays the one
    shared `libs` directory every consumer already reads.

    A SEPARATE `_depstore` ROOT, not another segment under `_deps`: the GC
    decides what is a bucket by finding a `libs` directory inside it
    (mtengine-gc.py), and a `<unit>` name sharing the slot a caps-hash uses
    would make the two indistinguishable.

    <backend> appears only for units that read it -- llama.cpp -- rather than
    above every unit as it sits in the view path. SDL3 does not care which
    llama backend an app asked for."""
    parts = [root, "_depstore", platform, arch, dconfig, unit_name]
    if unit.get("backend"):
        parts.append(backend)
    parts.append(caps_hash(unit_form(unit, values)) if unit_form(unit, values) else "common")
    return os.path.join(*parts)


def standalone_out_dir(root, platform, arch, config):
    """The engine's own builds are bound by the same rule as an app's. No app
    identity, no manifest, no caps-hash -- but still outside the checkout."""
    return os.path.join(root, "_standalone", platform, arch, config)
