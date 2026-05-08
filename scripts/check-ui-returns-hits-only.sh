#!/bin/bash
# Hard guard with ratchet: ui_*.c input helpers must compute UiHits
# and let `imrepl_ctrl` route to the owning subsystem; they may not
# call REPL or editor mutators directly.
#
# The corrected M/V/C+compiler+router contract has UI report neutral
# `UiHit` values from ui_panels_hit_test / ui_menu_bar_hit_test /
# ui_color_picker_hit_test / ui_variable_panel_hit_test. Mutation
# (cursor moves, command-store edits, menu-action dispatch, undo
# pushes, peer-state writes) belongs in editor / imrepl_ctrl /
# peer-subsystem handlers, not inside ui_*.c.
#
# Tracked patterns (in `ui_*.c`, excluding `ui_state.c` which is the
# legitimate owner of `ui_state_*_mut`):
#
#   - `repl_action_*(`               — REPL action dispatch
#   - `repl_command_store_*(`        — direct document mutation
#   - `repl_state_*_mut(`            — REPL state mut accessors
#   - `editor_state_*_mut(`          — editor state mut accessors
#   - `editor_buffer_replace_line(`  — text-buffer mutators
#   - `editor_buffer_insert_line(`
#   - `editor_buffer_delete_range(`
#   - `editor_buffer_set_input(`
#   - `editor_buffer_apply_compiled_change(`
#   - `editor_undo_push_snapshot(`     — undo ring writes
#   - `editor_undo_pop_snapshot(`
#   - `editor_undo_do_redo(`
#
# The check counts violations and fails if the count rises past the
# baseline in `scripts/baselines/ui-returns-hits-only.txt`. Reductions
# are always allowed; ratchet by hand by lowering the baseline. The
# remaining violations are scheduled for Phase F+ cleanup (color
# picker writeback through the editor commit pipeline; menu/cursor
# action dispatch via imrepl_ctrl on UiHit.kind).

set -euo pipefail

baseline_file="${1:-scripts/baselines/ui-returns-hits-only.txt}"

if [ ! -f "$baseline_file" ]; then
    echo "ERROR: missing baseline file $baseline_file" >&2
    exit 1
fi

baseline=$(awk -F: '/^count/{gsub(/[ \t]/,"",$2); print $2}' "$baseline_file")
if [ -z "${baseline:-}" ]; then
    echo "ERROR: baseline file $baseline_file missing 'count: N' entry" >&2
    exit 1
fi

violations=$(grep -REn \
    --include='ui_*.c' \
    --exclude='ui_state.c' \
    --exclude-dir='.claude' \
    'repl_action_[a-zA-Z_]+\(|repl_command_store_[a-zA-Z_]+\(|repl_state_[a-zA-Z_]+_mut\(|editor_state_[a-zA-Z_]+_mut\(|editor_buffer_(replace_line|insert_line|delete_range|set_input|apply_compiled_change)\(|editor_undo_(push_snapshot|pop_snapshot|do_redo)\(' \
    . 2>/dev/null || true)

if [ -z "$violations" ]; then
    count=0
else
    count=$(printf '%s\n' "$violations" | wc -l | awk '{print $1}')
fi

if [ "$count" -gt "$baseline" ]; then
    echo "ERROR: ui_*.c mutator-call count grew past baseline (${baseline} -> ${count})." >&2
    echo "UI files compute UiHits; mutation belongs in editor / imrepl_ctrl / peers." >&2
    echo "See scripts/baselines/ui-returns-hits-only.txt for context." >&2
    echo "Violations:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "ui-returns-hits-only OK (count=${count}/${baseline})"
