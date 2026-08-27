#!/usr/bin/env bash
# Checks that the MTENGINE-PATCH markers in the vendored Dear ImGui tree and
# the rows of MTENGINE_PATCHES.md describe the same set of patches.
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
REGISTRY="$IMGUI_DIR/MTENGINE_PATCHES.md"
CLEAN_CLONE="${IMGUI_CLEAN_CLONE:-$HOME/develop/imgui}"

if [ ! -d "$IMGUI_DIR" ]; then
    echo "FAIL: no imgui directory at $IMGUI_DIR"
    exit 1
fi
if [ ! -f "$REGISTRY" ]; then
    echo "FAIL: no registry at $REGISTRY"
    exit 1
fi

# Every marker id that appears in the sources (open or close), deduplicated.
markers_in_source() {
    grep -rho 'MTENGINE-PATCH: [a-z0-9-]*' "$IMGUI_DIR" \
        --include='*.h' --include='*.cpp' --include='*.mm' --include='*.inl' \
        | sed 's/.*: //' | sort -u
}

# Every id declared by a registry table row: a line starting with "| `<id>` |".
markers_in_registry() {
    grep -oE '^\| `[a-z0-9-]+` \|' "$REGISTRY" \
        | sed -E 's/^\| `([a-z0-9-]+)` \|/\1/' | sort -u
}

case "${1:-}" in
--list)
    grep -rn 'MTENGINE-PATCH' "$IMGUI_DIR" \
        --include='*.h' --include='*.cpp' --include='*.mm' --include='*.inl'
    exit 0
    ;;
--emit-patch)
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

src_ids="$(markers_in_source)"
reg_ids="$(markers_in_registry)"

if [ -z "$reg_ids" ]; then
    echo "FAIL: parsed zero ids out of $REGISTRY -- the table format changed"
    echo "      expected rows starting with: | \`some-id\` |"
    fail=1
fi

undocumented="$(comm -23 <(echo "$src_ids") <(echo "$reg_ids"))"
if [ -n "$undocumented" ]; then
    echo "FAIL: marker(s) in source with no row in MTENGINE_PATCHES.md:"
    echo "$undocumented" | sed 's/^/      /'
    fail=1
fi

missing="$(comm -13 <(echo "$src_ids") <(echo "$reg_ids"))"
if [ -n "$missing" ]; then
    echo "FAIL: registry row(s) with no marker in source:"
    echo "$missing" | sed 's/^/      /'
    echo "      (a patch that was reverted must lose its row too)"
    fail=1
fi

# Every id must have a matching close marker. Unbalanced brackets mean the
# grep-one-command promise is broken for that patch.
for id in $src_ids; do
    opens=$(grep -rho "\[MTENGINE-PATCH: $id\]" "$IMGUI_DIR" \
        --include='*.h' --include='*.cpp' --include='*.mm' --include='*.inl' | wc -l | tr -d ' ')
    closes=$(grep -rho "\[/MTENGINE-PATCH: $id\]" "$IMGUI_DIR" \
        --include='*.h' --include='*.cpp' --include='*.mm' --include='*.inl' | wc -l | tr -d ' ')
    if [ "$opens" != "$closes" ]; then
        echo "FAIL: $id has $opens open marker(s) and $closes close marker(s)"
        fail=1
    fi
done

if [ "$fail" = 0 ]; then
    count=$(echo "$src_ids" | wc -l | tr -d ' ')
    echo "PASS: $count patch id(s), markers and registry agree"
fi
exit $fail
