"""Manifest parsing, the four resolution rungs, and the canonical resolved form."""

import hashlib
import os
import re
import subprocess

from errors import ManifestError
from vocab import COMMERCIAL_KEY, PLATFORMS, PRIVATE_KEY

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

    known = set(vocab.keys) | {COMMERCIAL_KEY, PRIVATE_KEY}

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

    return values, provenance


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
      * sorted by byte value (C locale), joined by `;`, NO trailing separator.
    """
    keys = sorted(list(vocab.keys) + [COMMERCIAL_KEY, PRIVATE_KEY])
    return ";".join("%s=%d" % (k, values[k]) for k in keys)


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

    `platform=None` (the default) skips only the platform table; the
    distribution table always applies, because MT_PRIVATE_BUILD is in `values`
    and needs no caller cooperation to be correct.

    MT_COMMERCIAL_BUILD is deliberately NOT here; see emit.py."""
    flags = {}
    for key in vocab.keys:
        for flag in vocab.enables(key):
            # A flag serving two capabilities is on if EITHER is on.
            flags[flag] = max(flags.get(flag, 0), values[key])
    for flag in PLATFORM_UNAVAILABLE_FLAGS.get(platform, ()):
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
        return subprocess.run(
            ("git", "-C", engine_dir) + args,
            capture_output=True, text=True, check=False).stdout

    head = git("rev-parse", "--short", "HEAD").strip()
    if not head:
        raise ManifestError("--engine-dir %s is not a git checkout" % engine_dir)

    status = git("status", "--porcelain", "--untracked-files=no")
    if status.strip():
        diff = git("diff", "HEAD")
        digest = hashlib.sha256((status + diff).encode("utf-8")).hexdigest()[:8]
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


def default_build_root():
    """Outside all five checkouts. `${XDG_CACHE_HOME:-$HOME/.cache}` and not a bare
    `$XDG_CACHE_HOME`: the variable is unset on macOS and on most Linux systems, so
    the bare form expands to `/mtengine`."""
    root = os.environ.get("MTENGINE_BUILD_ROOT")
    if root:
        return root
    if os.name == "nt":
        local = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        return os.path.join(local, "mtengine")
    cache = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(cache, "mtengine")


def out_dir(root, app, rev, platform, arch, config, mode, backend, chash):
    return os.path.join(root, app, rev, platform, arch, config, mode, backend, chash)


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


def standalone_out_dir(root, platform, arch, config):
    """The engine's own builds are bound by the same rule as an app's. No app
    identity, no manifest, no caps-hash -- but still outside the checkout."""
    return os.path.join(root, "_standalone", platform, arch, config)
