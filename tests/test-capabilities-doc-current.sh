#!/usr/bin/env bash
# Checks that docs/CAPABILITIES.md is what `mtcaps emit-docs` produces today.
#
# The file's own banner has always said "do not edit by hand" and "CI regenerates
# this to a temp path and fails on any diff". The first line was violated by
# d75df6b3 and 44c0dab9, which pasted 79 lines of prose straight into the file;
# the second was never true -- there was no such job anywhere in .github/
# workflows, which is exactly why the drift survived two commits unnoticed and
# was found only by someone running the generator by hand on 2026-08-28.
#
# The prose now lives in _render_docs (mechanism-level) and vocabulary.json
# (capability-level), so regeneration is lossless and this check can exist.
#
# If this fails, DO NOT hand-edit the tracked file back into agreement -- that is
# the failure mode this test exists to catch. Put the text in the generator or
# the vocabulary, then run:
#
#     python3 -B tools/mtcaps/mtcaps.py emit-docs --out docs/CAPABILITIES.md

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOC="$REPO_ROOT/docs/CAPABILITIES.md"

if [ ! -f "$DOC" ]; then
    echo "FAIL: no capability reference at $DOC"
    exit 1
fi

PY="${PYTHON:-python3}"
if ! command -v "$PY" >/dev/null 2>&1; then
    echo "SKIP: no $PY on PATH; mtcaps needs Python 3"
    exit 0
fi

# -B: THE RULE -- a check must not leave bytecode inside the engine checkout.
# mktemp, not a path under the repo, for the same reason.
tmp="$(mktemp -t mtcapsdoc.XXXXXX)"
trap 'rm -f "$tmp"' EXIT

if ! "$PY" -B "$REPO_ROOT/tools/mtcaps/mtcaps.py" emit-docs --out "$tmp" >/dev/null; then
    echo "FAIL: mtcaps emit-docs did not run"
    exit 1
fi

if diff -u --label "generated" --label "tracked" "$tmp" "$DOC"; then
    echo "PASS: docs/CAPABILITIES.md matches mtcaps emit-docs"
    exit 0
fi

echo ""
echo "FAIL: docs/CAPABILITIES.md is not what the generator produces."
echo "      Above, '-' is what the generator emits and '+' is what is tracked."
echo "      Fix the SOURCE (tools/mtcaps/vocabulary.json for capability prose,"
echo "      _render_docs in tools/mtcaps/mtcaps.py for mechanism prose), then:"
echo "        python3 -B tools/mtcaps/mtcaps.py emit-docs --out docs/CAPABILITIES.md"
exit 1
