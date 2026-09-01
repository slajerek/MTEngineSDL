#!/usr/bin/env python3
"""mtcaps -- the MTEngineSDL capability generator.

Python 3, stdlib only, no pip. Invoke it through a RESOLVED interpreter --
CMake's ${Python3_EXECUTABLE}, an MSBuild $(PythonExe), the macOS wrapper's own
python3 lookup -- never a bare `python`/`python3` on PATH.

    mtcaps resolve   --manifest <path> --app <name>
                     --platform linux|macos|windows --arch <arch> --config <cfg>
                     --engine-dir <path> [--engine-option KEY=VALUE ...]
                     [--set KEY=VALUE ...] [--out-dir <root>]

    mtcaps check     --manifest <path> --app <name>
                     --platform <p> --arch <a> --config <c>
                     --engine-dir <path> --resolved <string> [--overrides <path>]
    mtcaps check     --stamp <path> --resolved <string>     # engine-side backstops

    mtcaps emit-docs [--vocabulary <path>] --out <path>

`check` takes the same inputs as `resolve` EXCEPT --set and --engine-option.
--set is absent because overrides reach check through --overrides, the persisted
form of exactly those args. --engine-option is absent because it keys <backend>
in the PATH and never enters the canonical resolved form, so it changes nothing
check computes; when a backstop needs the root it reads resolved.stamp, which
records the engine-options verbatim.

There is no `out-dir` verb and no `--commercial` flag. The licence mode lives in
the manifest and is overridden like everything else, with
`--set MT_COMMERCIAL_BUILD=...`, which persists to overrides.caps so check
re-resolves identically. A dedicated flag would be a third channel the agreement
check CANNOT catch: pass --commercial 1 against a manifest saying 0 and both the
build and the check use the flag, so a commercial binary silently ignores its
manifest and passes.

Exit codes: 0 agree/ok, 1 disagree, 2 manifest/vocabulary error, 3 usage error.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emit  # noqa: E402
import resolve as R  # noqa: E402
import vocab as V  # noqa: E402
from errors import (DisagreeError, ManifestError, MtCapsError,  # noqa: E402
                    EXIT_OK, EXIT_USAGE, UsageError)


def _kv(pairs, what):
    out = {}
    for item in pairs or []:
        if "=" not in item:
            raise UsageError("%s expects KEY=VALUE, got %r" % (what, item))
        k, v = item.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def _int_kv(pairs, what):
    out = {}
    for k, v in _kv(pairs, what).items():
        if v not in ("0", "1"):
            raise UsageError("%s %s=%r -- values are 0 or 1 only" % (what, k, v))
        out[k] = int(v)
    return out


def _resolve_common(args, overrides):
    vocab = V.load(args.vocabulary)
    values, provenance = R.resolve(vocab, args.manifest, args.platform, overrides)
    # The commercial_safe deny-list (decision 0.2) runs on every resolve AND
    # every check -- same code path, so the agreement check cannot pass a
    # configuration the resolve would refuse.
    R.check_commercial_safe(vocab, values, args.platform)
    canonical = R.canonical_form(vocab, values)
    return vocab, values, provenance, canonical


def _compute_out(args, vocab, values, canonical, engine_options):
    root = args.out_dir or R.default_build_root()
    rev = R.engine_rev(args.engine_dir)
    mode = "commercial" if values[V.COMMERCIAL_KEY] == 1 else "full"
    backend = R.backend_hash(engine_options)
    chash = R.caps_hash(canonical)
    out = R.out_dir(root, args.app, rev, args.platform, args.arch, args.config,
                    mode, backend, chash)
    build = R.build_dir(root, args.app, args.platform, args.arch, args.config,
                        mode, backend, chash)
    # Third-party archives are keyed differently and deliberately: no app, no
    # engine revision, no licence mode of its own (MT_COMMERCIAL_BUILD is already
    # inside the capability hash) -- and no MT_RELEASE_SYMBOLS either, which is
    # why the deps hash comes from deps_form(): the archives are byte-identical
    # whatever the symbols key says, and deps_form is byte-identical to the
    # pre-0.5 canonical, so existing buckets stay valid. See R.deps_dir.
    deps = R.deps_dir(root, args.platform, args.arch,
                      R.deps_config(args.platform, args.config), backend,
                      R.caps_hash(R.deps_form(vocab, values)))
    return out, dict(rev=rev, mode=mode, backend=backend, caps_hash=chash,
                     root=root, deps_dir=deps, build_dir=build,
                     ffmpeg_mode=R.ffmpeg_mode(values))


# ---------------------------------------------------------------------------

def cmd_resolve(args):
    overrides = _int_kv(args.set, "--set")
    engine_options = _kv(args.engine_option, "--engine-option")

    vocab, values, provenance, canonical = _resolve_common(args, overrides)
    out, key = _compute_out(args, vocab, values, canonical, engine_options)
    # L9: the generated header lives under the REV-FREE build dir -- a stable
    # path, so a new engine rev does not change every consumer's command line.
    include_dir = os.path.join(key["build_dir"], "include")

    meta = {
        "manifest": os.path.abspath(args.manifest),
        "app": args.app,
        "platform": args.platform,
        "arch": args.arch,
        "config": args.config,
        "engine_dir": os.path.abspath(args.engine_dir),
        "engine_options": engine_options,
        "vocabulary": vocab.path,
        "overrides": os.path.join(out, "overrides.caps") if overrides else None,
        "out_dir": out,
        "deps_dir": key["deps_dir"],
        "include_dir": include_dir,
    }
    meta.update(key)

    emit.emit_all(vocab, values, canonical, out, include_dir, meta)
    if overrides:
        emit.emit_overrides(out, overrides, meta)

    # --print gives ONE bare value and nothing else. It exists for MSBuild, which
    # captures a task's whole stdout into a single property joined by ';' -- so
    # the multi-line form below would arrive as "resolved=...;out_dir=..." and have
    # to be re-split by a regex inside a .targets file. One caller, one line.
    if getattr(args, "print_", None):
        if args.print_ == "resolved":
            print(canonical)
        elif args.print_ == "deps-dir":
            print(key["deps_dir"])
        elif args.print_ == "build-dir":
            print(key["build_dir"])
        else:
            print(out)
        return EXIT_OK

    # Three pinned lines. The PREFIXES are part of the contract: #4.2 pins the
    # canonical string byte-for-byte precisely because two implementers would
    # differ, and leaving the envelope unspecified reintroduces the ambiguity one
    # layer out.
    print("resolved=%s" % canonical)
    print("out_dir=%s" % out)
    print("deps_dir=%s" % key["deps_dir"])
    # Fourth pinned line (decision 0.2, hardened): which FFmpeg decoder set
    # this build gets. The codec scripts and wrappers consume the fragment's
    # MT_FFMPEG_BUILD_MODE instead of deriving a mode from COMMERCIAL alone.
    print("ffmpeg_mode=%s" % key["ffmpeg_mode"])
    # Fifth pinned line (L9): the rev-free build root for compiled objects.
    print("build_dir=%s" % key["build_dir"])
    return EXIT_OK


def cmd_check(args):
    if args.stamp:
        # The engine-side backstop form. The stamp supplies the INPUTS to
        # re-resolve from; --resolved supplies what THIS BUILD actually got, and
        # the comparison is between those two. With the stamp alone there is
        # nothing to compare against and the check is vacuous.
        with open(args.stamp, "r", encoding="utf-8") as f:
            meta = json.load(f)
        overrides = {}
        if meta.get("overrides") and os.path.isfile(meta["overrides"]):
            overrides = emit.parse_overrides(meta["overrides"])
        vocab = V.load(meta.get("vocabulary"))
        values, _ = R.resolve(vocab, meta["manifest"], meta["platform"], overrides)
        fresh = R.canonical_form(vocab, values)
        source = "%s (via %s)" % (meta["manifest"], args.stamp)
    else:
        for required in ("manifest", "app", "platform", "arch", "config", "engine_dir"):
            if not getattr(args, required, None):
                raise UsageError("check without --stamp needs --%s"
                                 % required.replace("_", "-"))
        overrides = emit.parse_overrides(args.overrides) if args.overrides else {}
        _, _, _, fresh = _resolve_common(args, overrides)
        source = args.manifest

    # Re-resolving AT CHECK TIME is what makes one side fresh. Comparing the
    # build's fragment against an artefact the same generator run emitted lets
    # both go stale together: in the xcodebuild -target case -- the entire reason
    # this check exists -- the pre-action did not run, both artefacts are stale
    # from the previous run, they agree, and the silent wrong build returns.
    if fresh == args.resolved:
        return EXIT_OK

    sys.stderr.write("error: STALE capability set\n")
    sys.stderr.write("  built with : %s\n" % args.resolved)
    sys.stderr.write("  %s says: %s\n" % (source, fresh))
    _print_diff(args.resolved, fresh)
    raise DisagreeError("resolved set does not agree with %s" % source)


def _print_diff(built, fresh):
    def as_map(s):
        out = {}
        for item in s.split(";"):
            if "=" in item:
                k, v = item.split("=", 1)
                out[k] = v
        return out

    a, b = as_map(built), as_map(fresh)
    for key in sorted(set(a) | set(b)):
        if a.get(key) != b.get(key):
            sys.stderr.write("  %-32s built=%s manifest=%s\n"
                             % (key, a.get(key, "<absent>"), b.get(key, "<absent>")))


def cmd_emit_docs(args):
    """A SEPARATE verb from resolve, deliberately. docs/CAPABILITIES.md is tracked
    in the engine, and if resolve wrote it then every app build would write a
    tracked file inside the engine checkout -- the thing the rule forbids outright.
    Run by a human or an engine-repo CI job; CI checks currency by regenerating to
    a temp path and diffing."""
    vocab = V.load(args.vocabulary)
    text = _render_docs(vocab)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s" % args.out)
    return EXIT_OK


def _render_docs(vocab):
    L = []
    A = L.append
    A("<!-- generated by `mtcaps emit-docs` -- do not edit by hand. -->")
    A("<!-- CI regenerates this to a temp path and fails on any diff. -->")
    A("")
    A("# MTEngineSDL capability reference")
    A("")
    A("Generated from `tools/mtcaps/vocabulary.json`. A hand-written table is wrong")
    A("within two capabilities, which is why this one is not hand-written.")
    A("")
    A("## Capabilities")
    A("")
    A("| Capability | Default | Kind | Implies | Turns on | App-visible |")
    A("|---|---|---|---|---|---|")
    for key in vocab.keys:
        cap = vocab.get(key)
        A("| `%s` | %d | %s | %s | %s | %s |" % (
            key, cap["default"], cap["kind"],
            ", ".join("`%s`" % i for i in cap["implies"]) or "—",
            ", ".join("`%s`" % e for e in cap["enables"]) or "—",
            "yes" if cap["app_visible"] else "no"))
    A("")
    A("## What each capability costs")
    A("")
    A("| Capability | Dependency | Licence | Version | Provenance |")
    A("|---|---|---|---|---|")
    for key in vocab.keys:
        for dep in vocab.get(key)["dependencies"]:
            A("| `%s` | %s | %s | %s | %s |" % (
                key, dep["name"], dep["licence"], dep["version"], dep["provenance"]))
    A("")
    # ------------------------------------------------------------------
    # Hand-written prose, emitted from HERE rather than pasted into the file.
    #
    # It WAS pasted straight into docs/CAPABILITIES.md, by d75df6b3 and
    # 44c0dab9, under a banner reading "do not edit by hand" -- so the next
    # `emit-docs` would have silently deleted all 79 lines. Found 2026-08-28 by
    # running the generator and diffing its output against the tracked file.
    #
    # The banner's second line -- "CI regenerates this to a temp path and fails
    # on any diff" -- was aspirational: no such job exists in .github/workflows,
    # which is why the drift survived two commits unnoticed.
    #
    # Prose tied to a capability belongs in vocabulary.json. This describes the
    # MECHANISM rather than any one capability, so it has nowhere else to live,
    # and it goes here so the generated file stays genuinely generated.
    # ------------------------------------------------------------------
    A('## How a switched-off dependency is actually skipped')
    A('')
    A('A capability set to 0 does two things: it defines `MT_ENABLE_<DEP>=0` for the')
    A('compiler, which compiles the calling code out, and it must stop the dependency')
    A('from being **built** at all. The second half is the expensive one -- the image')
    A('codec bundle alone is 541 MB of source and build tree -- and it works differently')
    A('on each platform.')
    A('')
    A('Every dependency script gates itself on `$MT_ENABLE_<DEP>` / `$env:MT_ENABLE_<DEP>`')
    A('and, when off, writes a **stub archive** instead of building. The stub carries one')
    A('dummy symbol and no library symbols, because the engine and app projects name')
    A('these archives on the link line unconditionally -- an absent file is a link error,')
    A('not a saving. Anything that still calls into the dependency fails at link time,')
    A('which is the intended outcome: that code should have been compiled out by the')
    A('matching `MT_ENABLE_*` define.')
    A('')
    A('**The gate must come before the submodule check.** With the capability off, an')
    A("app's build script never fetches the submodule -- that is the point of selective")
    A('acquisition -- so a missing-submodule error ahead of the gate turns the saving')
    A('into a build failure on any clean clone.')
    A('')
    A('**Absent means ON.** A flag missing from the resolved fragment is treated as 1')
    A('(`${MT_ENABLE_FTXUI:-1}` on macOS, the same rule in `Import-MTCapsEnvironment` on')
    A('Windows), so a bare engine build with no manifest still builds everything.')
    A('')
    A('**Both paths must stamp.** A script that stamps only the stub leaves that stamp')
    A('beside a real archive after the capability is switched back on; the next off build')
    A('then matches the stamp, skips, and keeps the real archive while believing it wrote')
    A('a stub.')
    A('')
    A('### Where the flags come from')
    A('')
    A('| platform | mechanism |')
    A('|---|---|')
    A('| macOS | Dependency builds are Xcode **script phases**. Xcode exports every build setting from the resolved `MTEngineCaps.xcconfig` into the phase environment automatically, so the scripts just read `$MT_ENABLE_*`. |')
    A("| Windows | Nothing does this for free. `build-deps.ps1` takes `-CapsFile` (the same `MTEngineCaps.xcconfig`) and calls `Import-MTCapsEnvironment`, which publishes the `MT_ENABLE_*` lines into the process environment before any library script runs. An app's build script must pass it. |")
    A('')
    A('Linux publishes them the same way Windows now does, from `build-linux.sh` via')
    A('`mt_caps_read_flags`; that half was always there.')
    A('')
    A('### Coverage')
    A('')
    A('Every acquisition script gates itself. As of 2026-08-27:')
    A('')
    A('| dependency | macOS | Linux | Windows |')
    A('|---|---|---|---|')
    A('| mbedTLS | gate | gate | gate |')
    A('| FTXUI | gate | gate | gate |')
    A('| llama.cpp | gate | n/a (not built) | gate |')
    A('| image codecs | gate | gate | gate |')
    A('| video codecs | gate | gate | gate |')
    A('| SDL2/SDL3, uSockets | core -- no capability, never gated | | |')
    A('')
    A('Before that date the picture was far patchier, and it is worth recording what was')
    A('wrong because the failure was silent in every case -- a dependency that builds')
    A('when it should not produces a working binary, just a much more expensive one.')
    A('')
    A('**Windows could not read the flags at all.** MSBuild properties are not')
    A('environment variables, and nothing converted the resolved fragment into either.')
    A('The one gate that existed (`build-mbedtls.ps1`) therefore could never fire, and')
    A('`build-deps.ps1` built llama.cpp, FTXUI, mbedTLS and the image codecs on every')
    A('build whatever the manifest said. `build-ftxui.ps1`, `build-llama-cpp.ps1` and')
    A('`build-image_codecs.ps1` had no gate at all.')
    A('')
    A('**Linux had the plumbing but two missing gates**: `build-ftxui.sh` and')
    A('`build-image_codecs.sh`.')
    A('')
    A('**Two bundles had no gate on any platform.** The image codecs -- TIFF, WebP, AVIF,')
    A('libgav1, LibRaw, libjxl, lcms2 -- which is why a C64 debugger with `MT_CAP_RAW=0`')
    A('still compiled LibRaw; and the video codecs -- FFmpeg, libvpx, opus -- built even')
    A('with `MT_CAP_VIDEO_PLAYBACK=0`.')
    A('')
    A('Because that bundle is one archive serving six flags, it is skipped only when')
    A('`MT_ENABLE_LIBTIFF`, `MT_ENABLE_LIBWEBP`, `MT_ENABLE_LIBAVIF`, `MT_ENABLE_LIBHEIF`,')
    A('`MT_ENABLE_LIBRAW` and `MT_ENABLE_LCMS2` are all 0 -- any one of them still wanted')
    A('means building it, as there is no per-codec archive to stub. `libjxl` has no')
    A('capability of its own: it decodes JPEG XL DNGs, a RAW concern, so it travels with')
    A('`MT_ENABLE_LIBRAW`.')
    A('')
    A("## Core dependencies")
    A("")
    A("Always-on, belonging to no capability. **A `LICENSES.txt` derived from the")
    A("resolved capability set alone would omit every one of these** and ship a")
    A("legally incomplete SBOM while looking complete.")
    A("")
    A("| Dependency | Licence | Version | Provenance |")
    A("|---|---|---|---|")
    for dep in vocab.core["dependencies"]:
        A("| %s | %s | %s | %s |" % (dep["name"], dep["licence"], dep["version"],
                                     dep["provenance"]))
    A("")
    A("## Commercial-mode effects")
    A("")
    A("`MT_COMMERCIAL_BUILD` is **not** a whole-capability veto. Three values:")
    A("`none`, `capability-off` (the whole capability is withheld) and `variant`")
    A("(the capability stays **on** and a subset of its libraries or decoders is")
    A("withheld) — `variant` is the common case.")
    A("")
    A("Since 2026-08-31 (unification plan, decisions 0.1/0.2/0.5) the mode is")
    A("**applied at resolve time**, in a fixed order: (1) the manifest resolves;")
    A("(2) at `MT_COMMERCIAL_BUILD=1` every `capability-off` effect turns its")
    A("capability off **in the resolved set itself**, so the fragments, the")
    A("out-dir hash and `LICENSES.txt` all see the post-effect state and cannot")
    A("disagree; (3) only then does the `commercial_safe` deny-list run — a")
    A("commercial resolve that would still enable a dependency marked")
    A("`commercial_safe: false` in the vocabulary is a hard error naming the")
    A("dependency, its licence and the capability that pulled it in. A")
    A("dependency whose gating flag ended 0 (libheif under")
    A("`MT_CAP_PHOTO_CODECS=1`) is not enabled and never errors.")
    A("")
    A("Two further mode rules ride along:")
    A("")
    A("- **`MT_RELEASE_SYMBOLS`** (debug symbols in Release builds) is a")
    A("  build-settings mode key, never a C define. Default 1;")
    A("  `MT_COMMERCIAL_BUILD=1` forces it to 0 unless the invocation")
    A("  explicitly `--set`s it back — the deliberate UAT case. It joins the")
    A("  canonical form and the out-dir hash (a UAT and a store artifact never")
    A("  share one `$MT_OUT`) but not the deps hash (archives are")
    A("  byte-identical either way).")
    A("- **The FFmpeg decoder set follows distribution, not payment**: the")
    A("  resolve emits `ffmpeg_mode=` (and `MT_FFMPEG_BUILD_MODE` in every")
    A("  fragment) as `full` only when `MT_PRIVATE_BUILD=1`. The public/free")
    A("  tier (`0,0`) gets the same restricted set the store tier does,")
    A("  because the withheld decoders are patent-encumbered and patents")
    A("  attach to distribution.")
    A("")
    A("| Capability | Effect | Withheld in a commercial build |")
    A("|---|---|---|")
    for key in vocab.keys:
        comm = vocab.get(key)["commercial"]
        if comm["effect"] == "none":
            continue
        A("| `%s` | %s | %s |" % (key, comm["effect"],
                                  ", ".join(comm.get("forbidden_decoders_commercial", [])) or "—"))
    A("")
    A("## Distribution tiers")
    A("")
    A("`MT_COMMERCIAL_BUILD` alone describes two tiers, and there are **three**.")
    A("`MT_PRIVATE_BUILD` supplies the missing distinction.")
    A("")
    A("| Tier | `MT_PRIVATE_BUILD` | `MT_COMMERCIAL_BUILD` | May link distribution-restricted deps |")
    A("|---|---|---|---|")
    A("| Private — built and kept, never handed to anyone | `1` | `0` | **yes** |")
    A("| Public / free — distributed at no cost, e.g. a GitHub release | `0` | `0` | no |")
    A("| Commercial — sold or app-store distributed | `0` | `1` | no |")
    A("")
    A("Setting both to `1` is a configure-time error: an artifact is either sold")
    A("or never distributed, and only its author can say which.")
    A("")
    A("**Open source is not the same question.** A permissive or copyleft source")
    A("licence settles *copyright*. A dependency can additionally carry *patent*")
    A("obligations that attach to **distribution itself**, whether or not money")
    A("changes hands — HEVC, reached through libheif, is the case in this tree.")
    A("So a fully public MIT project is *not* automatically eligible; the tier")
    A("above is what decides, not the repository's licence. This is also why the")
    A("gate is a separate key rather than something derived from")
    A("`MT_COMMERCIAL_BUILD`: free public distribution sits between the two.")
    A("")
    A("Distribution-restricted today: `MT_ENABLE_LIBHEIF`")
    A("(`PRIVATE_ONLY_FLAGS` in `tools/mtcaps/resolve.py`). Kept per **library**,")
    A("not per capability, so the BSD-family siblings under the same")
    A("`MT_CAP_PHOTO_CODECS` key — TIFF, WebP, AVIF — are never collateral.")
    A("")
    A("Separately and for an unrelated reason, `PLATFORM_UNAVAILABLE_FLAGS` in the")
    A("same file forces a flag off where the library simply has not been vendored")
    A("yet — libheif on Windows today. That is an engineering gap, not a policy:")
    A("it applies in **every** tier, private included, and the entry disappears")
    A("when someone does the porting work.")
    A("")
    A("## Descriptions")
    A("")
    for key in vocab.keys:
        cap = vocab.get(key)
        A("### `%s`" % key)
        A("")
        A(cap["description"])
        A("")
        for field in ("implies_note", "app_visible_note", "naming_note", "note",
                      "ownership_note", "off_path_exists", "linux_extra",
                      "needs_guard_first", "phase_note"):
            if cap.get(field):
                A("*%s*" % cap[field])
                A("")
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(prog="mtcaps", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="verb", required=True)

    def common(sp):
        sp.add_argument("--manifest")
        sp.add_argument("--app")
        sp.add_argument("--platform", choices=V.PLATFORMS)
        sp.add_argument("--arch")
        sp.add_argument("--config")
        sp.add_argument("--engine-dir", dest="engine_dir")
        sp.add_argument("--vocabulary", default=None)

    r = sub.add_parser("resolve")
    common(r)
    r.add_argument("--engine-option", action="append", dest="engine_option",
                   help="engine build options that change the artefact but are not "
                        "capabilities (MT_LLAMA_CUDA, ...). They key <backend> in the "
                        "path and never enter the canonical resolved form.")
    r.add_argument("--set", action="append", dest="set",
                   help="override a capability. Persisted to $MT_OUT/overrides.caps so "
                        "`check` re-resolves from the same inputs.")
    r.add_argument("--out-dir", dest="out_dir",
                   help="the output ROOT the key hangs under, not the leaf.")
    r.add_argument("--print", dest="print_",
                   choices=("resolved", "out-dir", "deps-dir", "build-dir"),
                   help="print ONE bare value instead of the two pinned lines. For "
                        "MSBuild, which joins a task's whole stdout into one property.")
    r.set_defaults(func=cmd_resolve)

    c = sub.add_parser("check")
    common(c)
    c.add_argument("--resolved", required=True)
    c.add_argument("--overrides")
    c.add_argument("--stamp")
    c.add_argument("--out-dir", dest="out_dir")
    c.set_defaults(func=cmd_check)

    d = sub.add_parser("emit-docs")
    d.add_argument("--vocabulary", default=None)
    d.add_argument("--out", required=True)
    d.set_defaults(func=cmd_emit_docs)

    return p


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except MtCapsError as e:
        sys.stderr.write("mtcaps: %s\n" % e)
        return e.exit_code
    except FileNotFoundError as e:
        sys.stderr.write("mtcaps: %s\n" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
