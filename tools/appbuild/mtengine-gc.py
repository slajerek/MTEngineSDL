#!/usr/bin/env python3
"""mtengine-gc -- report on and prune the mtengine build cache.

One implementation for all three platforms (python3 is already a hard
requirement of the capability system). The cache root layout it knows:

    <root>/<app>/<engine-rev>/...          rev-keyed out dirs (fragments,
                                           stamps, symbols; grows per commit
                                           AND per dirty edit)
    <root>/<app>/_build/...                rev-FREE build dirs (L9: objects,
                                           generated include; bounded)
    <root>/_deps/<plat>/<arch>/<cfg>/<backend>/<hash>/libs   dependency buckets
    <root>/_deps/work/...                  shared dependency build trees
    <root>/_depstore/<plat>/<arch>/<cfg>/<unit>[/<backend>]/<hash>
                                           per-unit dependency build stores (L16)
    <root>/_standalone/, <root>/_deps/standalone/            caps-less builds

Default action is a REPORT. --prune deletes, with liveness computed from the
REAL tooling: the current engine HEAD (and, for deps/_build, an actual
`mtcaps resolve` per sibling app per config) -- never from a typed list.

    mtengine-gc.py                       report
    mtengine-gc.py --prune [--dry-run]   prune with the default retention
        --keep N        rev dirs to keep per app besides the live one(s) (2)
        --keep-dirty N  -dirty- rev dirs to keep per app (1)
        --keep-deps N   non-live deps buckets to keep (2)
        --min-age-hours H   never touch anything modified within H hours (6)
        --days N        ALSO prune kept dirs older than N days (0 = off)
        --deep          also prune _deps/work trees (full rebuild next time)

RETENTION IS GENERATIONAL, NOT AGE-BASED, and that is a correction of the
first design. A rev dir is superseded the moment you build a newer engine
revision; it does not become garbage on a birthday. The first version kept
anything younger than 14 days (3 for dirty), which on a machine doing a dozen
engine commits in one afternoon meant `--prune` reported "nothing eligible"
while sitting on 2.3 GB of dead directories -- the exact case that prompted
this rewrite. Age survives only as `--min-age-hours`, whose job is different:
never delete something a concurrent build might be writing.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ENGINE, "tools", "mtcaps"))
import resolve as R  # noqa: E402


def du(path):
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(root, f)).st_size
            except OSError:
                pass
    return total


def human(n):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return "%.1f %s" % (n, unit)
        n /= 1024.0


def newest_mtime(path):
    """The newest mtime one level down, not the directory's own: a rev dir's own
    mtime stops moving once its subdirectories exist, so it under-reports how
    recently the tree was actually written."""
    try:
        newest = os.path.getmtime(path)
        for entry in os.scandir(path):
            newest = max(newest, entry.stat().st_mtime)
        return newest
    except OSError:
        return 0.0


def age_days(path):
    return (time.time() - newest_mtime(path)) / 86400.0


def age_hours(path):
    return (time.time() - newest_mtime(path)) / 3600.0


def sibling_apps():
    """App checkouts next to the engine: a dir with mtengine.caps + conf."""
    parent = os.path.dirname(ENGINE)
    out = []
    for d in sorted(os.listdir(parent)):
        p = os.path.join(parent, d)
        if os.path.isfile(os.path.join(p, "mtengine.caps")) \
           and os.path.isfile(os.path.join(p, "mtengine-app.conf")):
            out.append((d, p))
    return out


def live_engine_revs():
    """The rev dirs a build STARTED RIGHT NOW would write to: the clean HEAD
    short rev, and -- when the checkout is dirty -- the dirty-suffixed rev the
    resolver actually computes.

    This replaces guessing by mtime. A build in flight writes to exactly one of
    these two names, so protecting them is a fact rather than a heuristic; the
    --min-age-hours window shrank to a safety net for the odd case of a second
    checkout state being built concurrently."""
    clean = subprocess.run(["git", "-C", ENGINE, "rev-parse", "--short", "HEAD"],
                           capture_output=True, text=True).stdout.strip()
    revs = {clean} if clean else set()
    try:
        revs.add(R.engine_rev(ENGINE))
    except Exception:
        pass
    return revs


def live_resolve_paths(platform):
    """Every directory the CURRENT manifests resolve to, per sibling app per
    config: the deps view, the rev-free build dir, and each per-unit store.
    A path the tooling would use today is never pruned.

    One resolve call per (app, config) parses all of them from the pinned
    lines. The earlier form called --print twice and so could not have seen
    the stores at all: --print answers with one path, and there are now as
    many live paths as there are build units."""
    live = set()
    mtcaps = os.path.join(ENGINE, "tools", "mtcaps", "mtcaps.py")
    arch = os.uname().machine if hasattr(os, "uname") else os.environ.get("PROCESSOR_ARCHITECTURE", "x64")
    # The drivers pass these; omitting them puts every resolved path under the
    # "default" backend segment, which on an ARM machine is a segment nothing
    # builds into -- liveness would then protect directories that do not exist
    # and leave the real ones unprotected.
    engine_opts = []
    for k, v in sorted(R.default_engine_options(arch).items()):
        engine_opts += ["--engine-option", "%s=%s" % (k, v)]
    for name, path in sibling_apps():
        app = name
        conf = open(os.path.join(path, "mtengine-app.conf"), encoding="utf-8").read()
        for line in conf.splitlines():
            if line.startswith("MT_APP_NAME="):
                app = line.split("=", 1)[1].strip().strip('"')
        for config in ("Debug", "Release"):
            r = subprocess.run(
                [sys.executable, "-B", mtcaps, "resolve",
                 "--manifest", os.path.join(path, "mtengine.caps"),
                 "--app", app, "--platform", platform,
                 "--arch", arch, "--config", config,
                 "--engine-dir", ENGINE] + engine_opts,
                capture_output=True, text=True)
            if r.returncode != 0:
                continue
            for line in r.stdout.splitlines():
                k, _, v = line.partition("=")
                if k in ("deps_dir", "build_dir") or k.startswith("store."):
                    if v.strip():
                        live.add(os.path.normpath(v.strip()))
    return live


def store_buckets(root):
    """Yield (platform, unit, path) for every bucket under _depstore.

    Where a bucket STARTS is read from the build-unit registry, not guessed
    from the path: the backend segment exists only for units that read the
    backend, so `llama_cpp` is one level deeper than `sdl3`. A unit dir the
    registry no longer knows is yielded with unit=None -- nothing can resolve
    to it again, so it is reported and prunable as a whole."""
    base = os.path.join(root, "_depstore")
    if not os.path.isdir(base):
        return
    for platform in sorted(os.listdir(base)):
        pdir = os.path.join(base, platform)
        if not os.path.isdir(pdir):
            continue
        try:
            units = R.build_units(platform)
        except Exception:
            units = {}
        for arch in sorted(os.listdir(pdir)):
            adir = os.path.join(pdir, arch)
            if not os.path.isdir(adir):
                continue
            for cfg in sorted(os.listdir(adir)):
                cdir = os.path.join(adir, cfg)
                if not os.path.isdir(cdir):
                    continue
                for unit in sorted(os.listdir(cdir)):
                    udir = os.path.join(cdir, unit)
                    if not os.path.isdir(udir):
                        continue
                    if unit not in units:
                        yield platform, None, udir
                        continue
                    depth = 2 if units[unit].get("backend") else 1
                    stack = [(udir, 0)]
                    while stack:
                        d, lvl = stack.pop()
                        if lvl == depth:
                            yield platform, unit, d
                            continue
                        for sub in sorted(os.listdir(d)):
                            sp = os.path.join(d, sub)
                            if os.path.isdir(sp):
                                stack.append((sp, lvl + 1))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prune", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--keep", type=int, default=2,
                    help="rev dirs to keep per app besides the live one")
    ap.add_argument("--keep-dirty", type=int, default=1,
                    help="-dirty- rev dirs to keep per app")
    ap.add_argument("--keep-deps", type=int, default=2,
                    help="non-live deps buckets to keep")
    ap.add_argument("--min-age-hours", type=float, default=1.0,
                    help="never touch anything modified within this many hours "
                         "(a safety net; the rev a build would write is protected by name)")
    ap.add_argument("--days", type=float, default=0,
                    help="also prune KEPT dirs older than this many days (0 = off)")
    ap.add_argument("--deep", action="store_true")
    args = ap.parse_args()

    root = R.default_build_root()
    if os.path.basename(os.path.normpath(root)) != "mtengine":
        print("ERROR: refusing to operate on %r -- not an mtengine cache root" % root)
        return 2
    if not os.path.isdir(root):
        print("nothing at %s" % root)
        return 0

    platform = {"darwin": "macos", "win32": "windows"}.get(sys.platform, "linux")
    heads = live_engine_revs()
    live = live_resolve_paths(platform) if args.prune else set()
    if args.prune:
        print("live engine rev(s): %s; %d live resolve paths"
              % (", ".join(sorted(heads)), len(live)))

    victims = []      # (path, size, reason)
    report = []       # (label, size)
    candidates = {}   # (app, dirty) -> [(path, rev, size)]
    dead_deps = []    # (path, rel, size) -- non-live buckets, newest kept
    kept_recent = 0   # protected by --min-age-hours

    for app in sorted(os.listdir(root)):
        app_dir = os.path.join(root, app)
        if not os.path.isdir(app_dir) or app.startswith("_"):
            continue
        for rev in sorted(os.listdir(app_dir)):
            p = os.path.join(app_dir, rev)
            if not os.path.isdir(p):
                continue
            if rev == "_build":
                for sub, size in [(p, du(p))]:
                    report.append(("%s/_build" % app, size))
                continue
            size = du(p)
            dirty = "-dirty-" in rev
            label = "%s/%s%s" % (app, rev, " (dirty)" if dirty else "")
            report.append((label, size))
            if args.prune and rev not in heads:  # what a build now would write
                candidates.setdefault((app, dirty), []).append((p, rev, size))

    # Generational selection: newest-first, keep N, the rest are dead. A dir
    # modified within --min-age-hours is never a candidate -- a build running
    # right now is the one thing age is genuinely good at detecting.
    for (app, dirty), entries in sorted(candidates.items()):
        entries.sort(key=lambda e: -newest_mtime(e[0]))
        keep_n = args.keep_dirty if dirty else args.keep
        for i, (path, rev, size) in enumerate(entries):
            if age_hours(path) < args.min_age_hours:
                kept_recent += 1
                continue
            if i < keep_n and not (args.days and age_days(path) > args.days):
                continue
            why = "rev %s, #%d newest%s" % (rev, i + 1, ", dirty" if dirty else "")
            victims.append((path, size, why))

    deps_root = os.path.join(root, "_deps")
    if os.path.isdir(deps_root):
        for dirpath, dirnames, _ in os.walk(deps_root):
            if os.path.basename(dirpath) == "work":
                dirnames[:] = []
                size = du(dirpath)
                report.append(("_deps/work", size))
                if args.prune and args.deep:
                    for t in sorted(os.listdir(dirpath)):
                        tp = os.path.join(dirpath, t)
                        if age_hours(tp) >= args.min_age_hours:
                            victims.append((tp, du(tp), "work tree (--deep)"))
                continue
            if "libs" in dirnames:
                dirnames[:] = []
                size = du(dirpath)
                rel = os.path.relpath(dirpath, root)
                report.append((rel, size))
                if args.prune:
                    lp = os.path.normpath(os.path.join(dirpath, "libs"))
                    if lp in live:
                        continue
                    dead_deps.append((dirpath, rel, size))

    # Per-unit stores (L16). Retention is per unit rather than global: three
    # newest buckets across the whole store would happily evict SDL3 entirely
    # while keeping three flavours of video_codecs, and the point of the split
    # was that a unit's buckets are independent of every other unit's.
    dead_stores = {}   # (platform, unit) -> [(path, rel, size)]
    for platform_name, unit, path in store_buckets(root):
        size = du(path)
        rel = os.path.relpath(path, root)
        report.append((rel, size))
        if not args.prune:
            continue
        if unit is None:
            if age_hours(path) >= args.min_age_hours:
                victims.append((path, size, "store unit not in the registry"))
            continue
        if os.path.normpath(path) in live:
            continue
        dead_stores.setdefault((platform_name, unit), []).append((path, rel, size))

    for (platform_name, unit), entries in sorted(dead_stores.items()):
        entries.sort(key=lambda e: -newest_mtime(e[0]))
        for i, (path, rel, size) in enumerate(entries):
            if age_hours(path) < args.min_age_hours:
                kept_recent += 1
                continue
            if i < args.keep_deps and not (args.days and age_days(path) > args.days):
                continue
            victims.append((path, size,
                            "%s store, not live, #%d newest" % (unit, i + 1)))

    dead_deps.sort(key=lambda e: -newest_mtime(e[0]))
    for i, (path, rel, size) in enumerate(dead_deps):
        if age_hours(path) < args.min_age_hours:
            kept_recent += 1
            continue
        if i < args.keep_deps and not (args.days and age_days(path) > args.days):
            continue
        victims.append((path, size, "deps bucket, not live, #%d newest" % (i + 1)))

    report.sort(key=lambda x: -x[1])
    total = sum(s for _, s in report)
    print("\nmtengine cache at %s -- %s total\n" % (root, human(total)))
    for label, size in report[:40]:
        print("  %10s  %s" % (human(size), label))
    if len(report) > 40:
        print("  ... and %d more entries" % (len(report) - 40))

    if not args.prune:
        print("\n(report only; --prune deletes with the retention in --help)")
        return 0

    if not victims:
        print("\nnothing eligible to prune: the live rev, the %d newest rev dirs "
              "per app (%d dirty) and the %d newest deps buckets are kept%s."
              % (args.keep, args.keep_dirty, args.keep_deps,
                 ", and %d entries were modified within %.0fh" % (kept_recent, args.min_age_hours)
                 if kept_recent else ""))
        print("Keep fewer with --keep 0 --keep-dirty 0 --keep-deps 0.")
        return 0
    freed = sum(s for _, s, _ in victims)
    print("\npruning %d entries, %s:" % (len(victims), human(freed)))
    for p, s, why in victims:
        print("  %10s  %s  (%s)" % (human(s), os.path.relpath(p, root), why))
        if not args.dry_run:
            shutil.rmtree(p, ignore_errors=True)
    print("\n%s: freed %s" % ("DRY RUN, deleted nothing" if args.dry_run else "done", human(freed)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
