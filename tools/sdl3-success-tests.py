#!/usr/bin/env python3
"""
Find SDL calls whose SUCCESS TEST is written for SDL2's int returns.

WHY THIS EXISTS
---------------
SDL3 changed error returns from `int` (negative on failure) to `bool`. So:

    if (SDL_Function() < 0) { handle_failure(); }     // compiles. NEVER TRUE.
    if (SDL_Function() != 0) { handle_failure(); }    // compiles. TRUE ON SUCCESS.

Neither produces a warning. The first silently stops handling errors; the second
inverts them, which is worse -- it turns every success into an error path. This
is the one class of change in the SDL3 port that no compiler will flag, which is
why it gets its own tool and its own task instead of being folded into the
per-subsystem work.

WHY NOT JUST GREP
-----------------
A raw grep over this codebase is actively misleading, and we learned that
expensively: `grep -c SDL_` over c64d's emulator tree returns 2026 against a
true figure of 12, because it counts commented-out SDL 1.2 code and atari800's
own SDL_-prefixed module globals. So this script:

  * STRIPS COMMENTS before matching (both // and /* */),
  * EXCLUDES vendored and reference code -- src/Emulators/, src/Vendor/,
    src/Engine/Libs/, and anything carrying c64d's [C64D-REFERENCE-ONLY] marker,
  * SEARCHES platform/*/src.* too, not just src/ -- the Windows entry point and
    the macOS platform layer are real engine code and a src/-only sweep misses
    them entirely,
  * CLASSIFIES each hit against the real SDL3 headers rather than guessing from
    the name: still-int (index/count returns are legitimately compared to 0),
    now-bool (must be rewritten), or unknown.

USAGE
    tools/sdl3-success-tests.py <sdl3-include-dir> <repo-root> [<repo-root>...]

e.g.
    tools/sdl3-success-tests.py \
        other/lib/SDL-release-3.4.14-static/include/SDL3 \
        ~/develop/MTEngineSDL ~/develop/PhotoCruise ~/develop/c64d ~/develop/LightHeroes

Exit status is 0 always -- this is a report, not a gate. The gate is a human
reading the NOW-BOOL rows.
"""

import os
import re
import sys

SOURCE_EXT = ('.c', '.cpp', '.h', '.hpp', '.mm', '.m')

EXCLUDE_DIR_PARTS = (
    os.path.join('src', 'Emulators'),
    os.path.join('src', 'Vendor'),
    os.path.join('src', 'Engine', 'Libs'),
    os.path.join('other', 'lib'),
    # Vendored third-party HEADERS live here (SDL2's and SDL3's own), and their
    # internal macros -- SDL_MUSTLOCK, SDL_TICKS_PASSED, SDL_oldnames.h's poison
    # tokens -- are not our code and are not our success tests.
    os.path.join('platform', 'MacOS', 'include'),
    os.path.join('platform', 'Windows', 'include'),
    os.path.join('platform', 'Linux', 'include'),
    'build-macos',
    'DerivedData',
    '.git',
)

SEARCH_ROOTS = ('src', 'platform')

# A success test: an SDL call compared against zero.
TEST_RE = re.compile(
    r'\b(SDL_[A-Za-z0-9_]+)\s*\([^;]*?\)\s*(<\s*0|<=\s*0|>=\s*0|>\s*0|!=\s*0|==\s*0)')

# SDL_TRUE / SDL_FALSE do not exist in SDL3 at all.
TRUEFALSE_RE = re.compile(r'\bSDL_(TRUE|FALSE)\b')


def strip_comments(text):
    """Remove // and /* */ comments. Not a C parser -- string literals holding
    comment markers are rare enough in this codebase that the false-positive
    cost is lower than the false-negative cost of not stripping at all."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    text = re.sub(r'//[^\n]*', '', text)
    return text


def load_sdl3_return_types(include_dir):
    """Map SDL function name -> declared return type, read from the headers."""
    types = {}
    decl = re.compile(
        r'extern\s+SDL_DECLSPEC\s+(.+?)\s+SDLCALL\s+(SDL_[A-Za-z0-9_]+)\s*\(')
    for fn in sorted(os.listdir(include_dir)):
        if not fn.endswith('.h'):
            continue
        with open(os.path.join(include_dir, fn), 'r',
                  encoding='utf-8', errors='replace') as f:
            body = f.read()
        for m in decl.finditer(body):
            types[m.group(2)] = ' '.join(m.group(1).split())
    return types


def is_excluded(path):
    for part in EXCLUDE_DIR_PARTS:
        if part in path:
            return True
    return False


def walk_sources(repo_root):
    for root_name in SEARCH_ROOTS:
        base = os.path.join(repo_root, root_name)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            if is_excluded(dirpath):
                dirnames[:] = []
                continue
            for fn in sorted(filenames):
                if fn.endswith(SOURCE_EXT):
                    path = os.path.join(dirpath, fn)
                    if not is_excluded(path):
                        yield path


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    include_dir = sys.argv[1]
    repos = sys.argv[2:]

    return_types = load_sdl3_return_types(include_dir)
    print("SDL3 functions declared in %s: %d\n" % (include_dir, len(return_types)))

    rows = []
    truefalse = []

    for repo in repos:
        for path in walk_sources(repo):
            try:
                with open(path, 'r', encoding='utf-8', errors='replace') as f:
                    raw = f.read()
            except OSError:
                continue

            # c64d's reference-only marker: those files are inert by
            # construction and are checked separately by
            # tests/test-reference-files.sh.
            if '[C64D-REFERENCE-ONLY]' in raw[:2000]:
                continue

            stripped = strip_comments(raw)
            for lineno, line in enumerate(stripped.split('\n'), 1):
                for m in TEST_RE.finditer(line):
                    name, op = m.group(1), ' '.join(m.group(2).split())
                    rettype = return_types.get(name)
                    if rettype is None:
                        verdict = 'UNKNOWN  '
                    elif rettype == 'bool':
                        verdict = 'NOW-BOOL '
                    elif rettype in ('int', 'Sint32'):
                        verdict = 'still-int'
                    else:
                        verdict = 'other    '
                    rows.append((verdict, os.path.relpath(path, os.path.dirname(repo.rstrip('/'))),
                                 lineno, name, op, rettype or '?'))
                for m in TRUEFALSE_RE.finditer(line):
                    truefalse.append((os.path.relpath(path, os.path.dirname(repo.rstrip('/'))),
                                      lineno, m.group(0)))

    order = {'NOW-BOOL ': 0, 'UNKNOWN  ': 1, 'other    ': 2, 'still-int': 3}
    rows.sort(key=lambda r: (order.get(r[0], 9), r[1], r[2]))

    print("%-9s  %-62s %-6s %-34s %-8s %s" %
          ('VERDICT', 'FILE', 'LINE', 'FUNCTION', 'TEST', 'SDL3 RETURN'))
    print('-' * 150)
    for verdict, path, lineno, name, op, rettype in rows:
        print("%-9s  %-62s %-6d %-34s %-8s %s" %
              (verdict, path, lineno, name, op, rettype))

    counts = {}
    for r in rows:
        counts[r[0]] = counts.get(r[0], 0) + 1
    print("\nsuccess tests: %d total  %s" %
          (len(rows), '  '.join('%s=%d' % (k.strip(), v) for k, v in sorted(counts.items()))))

    print("\nSDL_TRUE / SDL_FALSE (do not exist in SDL3): %d" % len(truefalse))
    for path, lineno, tok in truefalse:
        print("  %s:%d  %s" % (path, lineno, tok))

    return 0


if __name__ == '__main__':
    sys.exit(main())
