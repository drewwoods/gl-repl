#!/bin/bash
# Hard guard: src/repl/export*.c and src/repl/import.c stay GL-free and
# pull all app/render3d state through controller-installed bridges
# (ReplExportCameraBridge, ReplExportProjectionBridge, ReplConfigBridge,
# ...), never by reaching into the render3d or app layers directly. This is
# the canonical seam for keeping the REPL/export pipeline reusable
# without the GL frontend (see docs/ARCHITECTURE.md, "Dynamic Reshape
# Projection" + decouple step 4).
#
# check-gl-boundaries already forbids GL/GLU/GLUT *calls* here; this
# guard closes the other door: no render3d_*/glr_* calls and no render3d/app
# header includes, so a future "just call scene_get_active_projection()"
# shortcut fails the build instead of silently bypassing the bridge.
# Comments and string literals (e.g. the "@scene-name" marker, doc
# comments naming glr_*) are not violations.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

files=(
    src/repl/export.c
    src/repl/export_cmd_writer.c
    src/repl/export_display.c
    src/repl/export_prologue.c
    src/repl/export_setup.c
    src/repl/import.c
)

violations=$(grep -nE '#[[:space:]]*include[[:space:]]+"(render3d|app)/|\b(render3d|glr)_[a-z0-9_]+[[:space:]]*\(' "${files[@]}" 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)' \
    | grep -vE '^[^:]+:[0-9]+:[^"]*".*"' \
    || true)

if [ -z "$violations" ]; then
    echo "repl-export-via-bridge OK"
    exit 0
fi

echo "ERROR: src/repl/export*.c / src/repl/import.c must not reach the render3d/app layers directly." >&2
echo "Pull app/render3d-derived values through a controller-installed bridge" >&2
echo "(e.g. ReplExportProjectionBridge -> render3d_get_active_projection in" >&2
echo "src/app/glr_ctrl.c), not by calling render3d_*/glr_* or including their" >&2
echo "headers here. See docs/ARCHITECTURE.md, \"Dynamic Reshape Projection\"." >&2
echo "Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
