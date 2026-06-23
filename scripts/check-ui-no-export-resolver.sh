#!/bin/bash
# Hard guard: UI source must not resolve dynamic export-derived text
# live. The code panel's row-count/follow-scroll pass and its render
# pass straddle render3d_draw_scene() in glr_ctrl_display_frame(), so
# a value re-resolved in both passes can diverge across a transition
# (see docs/ARCHITECTURE.md, "Rule — where a per-frame dynamic value is
# resolved"). The controller resolves it once into UiRenderSnapshot;
# UI reads the snapshot.
#
# Forbidden in src/ui: a call to repl_export_reshape_projection_lines().
# Referencing the sentinel constant / REPL_EXPORT_PROJ_* dimensions is
# fine — only the live resolver call is the boundary violation.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

files=$(git ls-files 'src/ui/*.c' 'src/ui/*.h')

violations=$(grep -nE '\brepl_export_reshape_projection_lines[[:space:]]*\(' $files 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)' \
    | grep -vE '^[^:]+:[0-9]+:[^"]*"[^"]*repl_export_reshape_projection_lines' \
    || true)

if [ -z "$violations" ]; then
    echo "ui-no-export-resolver OK"
    exit 0
fi

echo "ERROR: src/ui must not call repl_export_reshape_projection_lines()." >&2
echo "Read the controller-frozen UiRenderSnapshot.reshape_proj_lines/_count" >&2
echo "instead. Resolving live in a UI pass races the scene-render boundary." >&2
echo "See docs/ARCHITECTURE.md, \"Rule - where a per-frame dynamic value is resolved\"." >&2
echo "Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
