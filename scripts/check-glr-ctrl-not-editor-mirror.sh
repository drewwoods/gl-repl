#!/bin/bash
# Hard guard: imrepl_ctrl must not mirror the editor's per-field
# API. The corrected M/V/C+compiler+router contract has imrepl_ctrl
# act as a thin router — it dispatches raw input to the owning
# subsystem (editor / variable_panel / replay / scene) and builds
# per-frame snapshots. It does NOT accumulate one wrapper per
# editor operation:
#
#     glr_ctrl_editor_search_next();        // NO
#     glr_ctrl_editor_scroll(int delta);    // NO
#     glr_ctrl_editor_move_cursor(...);     // NO
#     glr_ctrl_editor_accept_autocomplete();// NO
#     glr_ctrl_editor_cut();                // NO
#     glr_ctrl_editor_paste();              // NO
#
# Editor behavior lives behind the editor's coarse API
# (editor_handle_key / editor_handle_mouse / editor_handle_scroll /
#  editor_commit_current_input) accessed via the EditorServices
# table. New editor behavior should add an editor API, not an
# imrepl_ctrl wrapper.
#
# This guard catches symbols that look like per-field editor
# mirrors. The pattern is `glr_ctrl_editor_*` — anything matching
# that name shape is considered a violation. Coarse routing
# helpers (`glr_ctrl_keyboard` / `_special` / `_mouse` etc.)
# pass because they are GLUT-callback-name-shaped, not editor-API-
# shaped.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

violations=$(grep -RInE \
    --include='*.c' --include='*.h' \
    'glr_ctrl_editor_' \
    . 2>/dev/null \
    | grep -vE '\./\.claude/worktrees/' || true)

if [ -n "$violations" ]; then
    echo "ERROR: imrepl_ctrl is growing per-field editor wrappers." >&2
    echo "The editor owns its own dispatch (editor_handle_*,"        >&2
    echo "editor_commit_current_input). imrepl_ctrl is a router,"     >&2
    echo "not an editor API mirror. See feature/editor-text-model-"   >&2
    echo "controller.md \"imrepl_ctrl Non-Goals\"."                   >&2
    echo "Hits:"                                                      >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "imrepl-not-editor-mirror OK"
