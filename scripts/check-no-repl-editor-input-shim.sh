#!/bin/bash
# Hard guard: editor_input.c must not delegate dispatch to the legacy
# repl_*_func entry points or include any vestigial repl_editor.h
# header. The bodies migrated into editor_input.c during Phase J1
# (commits 44–46), the entry points were deleted in 49a, and
# repl_editor.{c,h} were deleted in 49b. This guard makes sure they
# can't reappear as new shims.
#
# Forbidden in editor_input.c:
#   - #include "repl_editor"          ← the deleted header
#   - repl_keyboard_func / repl_special_func / repl_mouse_func /
#     repl_motion_func / repl_passive_motion_func /
#     repl_mousewheel_func / repl_timer_func   ← deleted entry points

set -euo pipefail

violations=$(grep -nE \
    '#include[[:space:]]+"repl_editor|repl_keyboard_func|repl_special_func|repl_mouse_func|repl_motion_func|repl_passive_motion_func|repl_mousewheel_func|repl_timer_func' \
    src/editor/input.c 2>/dev/null \
    | grep -vE '^[0-9]+:[[:space:]]*(/\*|\*|//)' \
    || true)

if [ -n "$violations" ]; then
    echo "ERROR: editor_input.c references the deleted repl_editor shim layer." >&2
    echo "editor_input.c owns the dispatch boundary; it must not call legacy" >&2
    echo "repl_*_func entry points or include repl_editor.h." >&2
    echo "Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "no-repl-editor-input-shim OK"
