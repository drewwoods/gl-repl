#!/bin/bash
# Hard guard: src/repl/export.c stays GL-free and pulls all app/scene
# state through controller-installed bridges (ReplExportCameraBridge,
# ReplExportProjectionBridge, ReplConfigBridge, ...), never by reaching
# into the scene or app layers directly. This is the canonical seam for
# keeping the REPL/export pipeline reusable without the GL frontend
# (see ARCHITECTURE.md, "Dynamic Reshape Projection" + decouple step 4).
#
# check-gl-boundaries already forbids GL/GLU/GLUT *calls* here; this
# guard closes the other door: no scene_*/glr_* calls and no scene/app
# header includes, so a future "just call scene_get_active_projection()"
# shortcut fails the build instead of silently bypassing the bridge.
# Comments and string literals (e.g. the "@scene-name" marker, doc
# comments naming glr_*) are not violations.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

file=src/repl/export.c

# /dev/null forces grep to prefix the filename even for a single input,
# so the FILE:LINENO:content shape (and the comment/string filters
# below) is identical to the multi-file guards.
violations=$(grep -nE '#[[:space:]]*include[[:space:]]+"(scene|app)/|\b(scene|glr)_[a-z0-9_]+[[:space:]]*\(' "$file" /dev/null 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)' \
    | grep -vE '^[^:]+:[0-9]+:[^"]*".*"' \
    || true)

if [ -z "$violations" ]; then
    echo "repl-export-via-bridge OK"
    exit 0
fi

echo "ERROR: src/repl/export.c must not reach the scene/app layers directly." >&2
echo "Pull app/scene-derived values through a controller-installed bridge" >&2
echo "(e.g. ReplExportProjectionBridge -> scene_get_active_projection in" >&2
echo "src/app/glr_ctrl.c), not by calling scene_*/glr_* or including their" >&2
echo "headers here. See ARCHITECTURE.md, \"Dynamic Reshape Projection\"." >&2
echo "Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
