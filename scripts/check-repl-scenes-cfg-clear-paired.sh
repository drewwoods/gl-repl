#!/bin/bash
# Hard guard: every site in src/repl/scenes.c that flips
# `g_user_scenes[X].used = 0` must also call `scene_cfg_clear(X)`
# in the immediately surrounding block.
#
# Step 7b split the per-slot cfg storage off of UserScene into a
# parallel array (originally in glr_scenes.c, inlined as file-statics
# in src/repl/scenes.c by 7d). The slot lifecycle still drives both
# halves, so the parallel array only stays in sync if every "release
# this slot" path clears both.
#
# The check is a heuristic: look for `used = 0` lines in src/repl/scenes.c
# and require a `scene_cfg_clear` within the next few lines. False
# positives can be silenced by inlining the clear adjacent to the
# used flip.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

file="src/repl/scenes.c"

# Pull the line numbers of every "used = 0" assignment.
used_zero_lines=$(grep -nE '\.used\s*=\s*0\b|->used\s*=\s*0\b' "$file" 2>/dev/null | cut -d: -f1 || true)

if [ -z "$used_zero_lines" ]; then
    # Special case: `repl_scenes_reset` uses memset to clear every
    # slot. That's covered by scene_cfg_reset_all in the same
    # function. The check looks for it explicitly.
    if grep -q 'memset(g_user_scenes, 0' "$file" && \
       grep -q 'scene_cfg_reset_all()' "$file"; then
        echo "repl-scenes-cfg-clear-paired OK (no individual used=0 sites; reset paired)"
        exit 0
    fi
    echo "repl-scenes-cfg-clear-paired OK (no used=0 sites)"
    exit 0
fi

violations=""
for ln in $used_zero_lines; do
    end=$((ln + 5))
    snippet=$(sed -n "${ln},${end}p" "$file")
    if ! echo "$snippet" | grep -q 'scene_cfg_clear'; then
        violations+="${file}:${ln}: used = 0 without paired scene_cfg_clear\n"
    fi
done

# Verify the bulk-reset path too.
if grep -q 'memset(g_user_scenes, 0' "$file"; then
    if ! grep -q 'scene_cfg_reset_all()' "$file"; then
        violations+="${file}: memset(g_user_scenes, 0, ...) without scene_cfg_reset_all()\n"
    fi
fi

if [ -z "$violations" ]; then
    echo "repl-scenes-cfg-clear-paired OK"
    exit 0
fi

echo "ERROR: src/repl/scenes.c has used=0 sites without paired scene_cfg_clear:" >&2
printf "%b" "$violations" >&2
echo "Step 7b requires every slot-release path to clear the parallel cfg slot." >&2
exit 1
