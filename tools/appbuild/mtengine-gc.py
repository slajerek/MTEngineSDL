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
    <root>/_standalone/, <root>/_deps/standalone/            caps-less builds

Default action is a REPORT. --prune deletes, with liveness computed from the
REAL tooling: the current engine HEAD (and, for deps/_build, an actual
`mtcaps resolve` per sibling app per config) -- never from a typed list.

    mtengine-gc.py                       report
    mtengine-gc.py --prune [--dry-run]   prune with the default retention
        --days N        keep non-live rev dirs younger than N days (14)
        --dirty-days N  keep non-live -dirty- rev dirs younger than N (3)
        --deps-days N   keep non-live deps buckets younger than N (30)
        --deep          also prune _deps/work trees older than --days
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


def age_days(path):
    try:
        newest = os.path.getmtime(path)
        for entry in os.scandir(path):
            newest = max(newest, entry.stat().st_mtime)
        return (time.time() - newest) / 86400.0
    except OSError:
        return 0.0


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


def live_engine_rev():
    """Current HEAD's short rev -- WITHOUT any dirty suffix. A dirty digest
    changes on every edit, so dirty dirs are protected only by --dirty-days."""
    out = subprocess.run(["git", "-C", ENGINE, "rev-parse", "--short", "HEAD"],
                         capture_output=True, text=True).stdout.strip()
    return out


def live_resolve_paths(platform):
    """deps and _build dirs the CURRENT manifests resolve to, per sibling app
    per config. A path the tooling would use today is never pruned."""
    live = set()
    mtcaps = os.path.join(ENGINE, "tools", "mtcaps", "mtcaps.py")
    arch = os.uname().machine if hasattr(os, "uname") else os.environ.get("PROCESSOR_ARCHITECTURE", "x64")
    for name, path in sibling_apps():
        app = name
        conf = open(os.path.join(path, "mtengine-app.conf"), encoding="utf-8").read()
        for line in conf.splitlines():
            if line.startswith("MT_APP_NAME="):
                app = line.split("=", 1)[1].strip().strip('"')
        for config in ("Debug", "Release"):
            for what in ("deps-dir", "build-dir"):
                r = subprocess.run(
                    [sys.executable, "-B", mtcaps, "resolve",
                     "--manifest", os.path.join(path, "mtengine.caps"),
                     "--app", app, "--platform", platform,
                     "--arch", arch, "--config", config,
                     "--engine-dir", ENGINE, "--print", what],
                    capture_output=True, text=True)
                p = r.stdout.strip()
                if r.returncode == 0 and p:
                    live.add(os.path.normpath(p))
    return live


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prune", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--days", type=float, default=14)
    ap.add_argument("--dirty-days", type=float, default=3)
    ap.add_argument("--deps-days", type=float, default=30)
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
    head = live_engine_rev()
    live = live_resolve_paths(platform) if args.prune else set()
    if args.prune:
        print("live engine rev: %s; %d live resolve paths" % (head, len(live)))

    victims = []   # (path, size, reason)
    report = []    # (label, size)

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
            if args.prune:
                if rev == head:
                    continue  # the clean current rev is always live
                a = age_days(p)
                limit = args.dirty_days if dirty else args.days
                if a > limit:
                    victims.append((p, size, "rev %s, %.0fd old" % (rev, a)))

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
                        a = age_days(tp)
                        if a > args.days:
                            victims.append((tp, du(tp), "work tree, %.0fd old" % a))
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
                    a = age_days(dirpath)
                    if a > args.deps_days:
                        victims.append((dirpath, size, "deps bucket, %.0fd old, not live" % a))

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
        print("\nnothing eligible to prune.")
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
