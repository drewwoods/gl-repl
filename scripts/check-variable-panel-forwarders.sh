#!/bin/bash
# Ratchet against the variable_panel peer's legacy forwarder API
# becoming permanent. Phase F commits 31–32 moved the visibility flag
# and drag-state bytes into the variable_panel peer, but the legacy
# accessors stayed in place so existing test fixtures keep compiling.
#
# Tracked patterns (call shape only, with `(` to skip declarations
# and comments):
#
#   editor_state_variable_drag*(   — legacy editor accessors
#   ui_state_variable_panel*(      — legacy UI accessors
#   repl_var_drag_*(               — legacy drag-transaction symbols
#
# Sites inside the owner / wrapper translation units are ignored:
#   - editor_state.c / ui_state.c host the forwarder definitions.
#   - repl_var_drag.c is the implementation behind the peer's
#     variable_panel_handle_drag_* surface.
#   - variable_panel.c wraps repl_var_drag_* into the peer API.
#
# All remaining sites today are in tests/ and rewriting them goes
# alongside the eventual repl_var_drag.c rename / further peer
# encapsulation. The check fails if the count rises past the
# baseline. Reductions are always allowed; ratchet by hand by
# lowering scripts/baselines/variable-panel-forwarders.txt.

set -euo pipefail

baseline_file="${1:-scripts/baselines/variable-panel-forwarders.txt}"

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
    --include='*.c' \
    --exclude-dir='.claude' \
    --exclude-dir='build' \
    'editor_state_variable_drag[a-z_]*\(|ui_state_variable_panel[a-z_]*\(|repl_var_drag_[a-z_]*\(' \
    . 2>/dev/null \
    | grep -vE '^(\./)?(repl_var_drag|variable_panel|variable_panel_drag|editor_state|ui_state)\.c:' || true)

if [ -z "$violations" ]; then
    count=0
else
    count=$(printf '%s\n' "$violations" | wc -l | awk '{print $1}')
fi

if [ "$count" -gt "$baseline" ]; then
    echo "ERROR: variable_panel legacy-forwarder count grew past baseline (${baseline} -> ${count})." >&2
    echo "New callers should use variable_panel_handle_* / variable_panel_view*" >&2
    echo "/ variable_panel_drag* directly. See scripts/baselines/variable-panel-forwarders.txt" >&2
    echo "for context. Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "variable-panel-forwarders OK (count=${count}/${baseline})"
