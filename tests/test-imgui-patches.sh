#!/usr/bin/env bash
# Checks that the MTENGINE-PATCH markers in the vendored trees and the rows of
# their MTENGINE_PATCHES.md registries describe the same set of patches.
#
# TWO trees are vendored and locally patched, each with its own registry:
# src/Engine/Libs/imgui and src/Engine/Libs/imgui_test_engine. The second was
# unchecked until 2026-08-28, when it acquired its first patch -- the
# capability-gate that makes MT_CAP_TEST_ENGINE=0 buildable. An unchecked
# patched tree is exactly what these registries exist to prevent, since the
# upgrade procedure is "copy the new sources over the bundled tree".
#
#   tests/test-imgui-patches.sh              check (exit 0 = agree)
#   tests/test-imgui-patches.sh --list       list every marker with file:line
#   tests/test-imgui-patches.sh --emit-patch unified diff vs the clean clone
#
# Read MTENGINE_PATCHES.md before changing anything here.

set -u

# comm compares bytes; sort collates by locale. The current 10 ids happen to
# order identically under both, but the day an id like "metal-a" sits beside
# "metalx" the set differences go silently wrong -- on a script whose only job
# is set comparison.
export LC_ALL=C

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMGUI_DIR="$REPO_ROOT/src/Engine/Libs/imgui"
TE_DIR="$REPO_ROOT/src/Engine/Libs/imgui_test_engine"
CLEAN_CLONE="${IMGUI_CLEAN_CLONE:-$HOME/develop/imgui}"

# Every vendored tree that carries markers. Each owns its own registry beside
# its sources, so a tree can be upgraded without reading the other's table.
TREES="$IMGUI_DIR $TE_DIR"

for d in $TREES; do
    if [ ! -d "$d" ]; then
        echo "FAIL: no vendored directory at $d"
        exit 1
    fi
    if [ ! -f "$d/MTENGINE_PATCHES.md" ]; then
        echo "FAIL: no registry at $d/MTENGINE_PATCHES.md"
        exit 1
    fi
done

# Every marker id that appears in the sources (open or close), deduplicated.
markers_in_source() {
    grep -rho 'MTENGINE-PATCH: [a-z0-9-]*' "$1" \
        --include='*.h' --include='*.cpp' --include='*.mm' --include='*.inl' \
        | sed 's/.*: //' | sort -u
}

# Every id declared by a registry table row: a line starting with "| `<id>` |".
markers_in_registry() {
    grep -oE '^\| `[a-z0-9-]+` \|' "$1/MTENGINE_PATCHES.md" \
        | sed -E 's/^\| `([a-z0-9-]+)` \|/\1/' | sort -u
}

case "${1:-}" in
--list)
    for d in $TREES; do
        grep -rn 'MTENGINE-PATCH' "$d" \
            --include='*.h' --include='*.cpp' --include='*.mm' --include='*.inl'
    done
    exit 0
    ;;
--emit-patch)
    # IMGUI ONLY. It diffs against $CLEAN_CLONE, and there is no clean clone of
    # the test engine on this machine to diff the second tree against. Say so
    # rather than silently emitting half a patch set.
    echo "# NOTE: --emit-patch covers $IMGUI_DIR only; $TE_DIR is not diffed." >&2
    if [ ! -d "$CLEAN_CLONE" ]; then
        echo "FAIL: no clean clone at $CLEAN_CLONE (set IMGUI_CLEAN_CLONE)" >&2
        exit 1
    fi
    for f in "$IMGUI_DIR"/*.h "$IMGUI_DIR"/*.cpp "$IMGUI_DIR"/*.mm; do
        [ -e "$f" ] || continue
        base="$(basename "$f")"
        # The bundled tree is flat; upstream keeps backends and freetype in
        # subdirectories. Try each location and diff against the first hit.
        for cand in "$CLEAN_CLONE/$base" \
                    "$CLEAN_CLONE/backends/$base" \
                    "$CLEAN_CLONE/misc/freetype/$base"; do
            if [ -f "$cand" ]; then
                # --label so the output is applyable with -p1 rather than
                # carrying absolute, machine-specific paths.
                diff -u --label "a/$base" --label "b/$base" "$cand" "$f"
                found=1
                break
            fi
        done
        if [ "${found:-0}" = 0 ]; then
            # Silence here would be the same class of loss this stage exists to
            # prevent: a future engine-added file in this directory would
            # vanish from the "always current" patch set without a signal.
            echo "# WARNING: no upstream counterpart for $base" >&2
        fi
        found=0
    done
    exit 0
    ;;
-*)
    echo "usage: test-imgui-patches.sh [--list | --emit-patch]" >&2
    exit 2
    ;;
esac

fail=0
total=0

# Per tree, not pooled. Pooling the two sets would let a marker in one tree be
# "documented" by a row in the other's registry -- the exact confusion each
# registry exists to prevent, and it would pass silently.
for dir in $TREES; do
    name="$(basename "$dir")"
    src_ids="$(markers_in_source "$dir")"
    reg_ids="$(markers_in_registry "$dir")"

    if [ -z "$reg_ids" ]; then
        echo "FAIL[$name]: parsed zero ids out of $dir/MTENGINE_PATCHES.md -- the table format changed"
        echo "      expected rows starting with: | \`some-id\` |"
        fail=1
    fi

    undocumented="$(comm -23 <(echo "$src_ids") <(echo "$reg_ids"))"
    if [ -n "$undocumented" ]; then
        echo "FAIL[$name]: marker(s) in source with no row in MTENGINE_PATCHES.md:"
        echo "$undocumented" | sed 's/^/      /'
        fail=1
    fi

    missing="$(comm -13 <(echo "$src_ids") <(echo "$reg_ids"))"
    if [ -n "$missing" ]; then
        echo "FAIL[$name]: registry row(s) with no marker in source:"
        echo "$missing" | sed 's/^/      /'
        echo "      (a patch that was reverted must lose its row too)"
        fail=1
    fi

    # Every id must have a matching close marker. Unbalanced brackets mean the
    # grep-one-command promise is broken for that patch.
    for id in $src_ids; do
        opens=$(grep -rho "\[MTENGINE-PATCH: $id\]" "$dir" \
            --include='*.h' --include='*.cpp' --include='*.mm' --include='*.inl' | wc -l | tr -d ' ')
        closes=$(grep -rho "\[/MTENGINE-PATCH: $id\]" "$dir" \
            --include='*.h' --include='*.cpp' --include='*.mm' --include='*.inl' | wc -l | tr -d ' ')
        if [ "$opens" != "$closes" ]; then
            echo "FAIL[$name]: $id has $opens open marker(s) and $closes close marker(s)"
            fail=1
        fi
    done

    [ -n "$src_ids" ] && total=$((total + $(echo "$src_ids" | wc -l | tr -d ' ')))
done

if [ "$fail" = 0 ]; then
    echo "PASS: $total patch id(s) across 2 vendored tree(s), markers and registries agree"
fi
exit $fail
