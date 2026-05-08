#!/bin/bash
# Audit script for the editor / REPL / UI three-layer ownership split.
# See feature/editor-owns-text-completion.md (Phase 0) and
# feature/editor-ownership-gap-cleanup.md (§0.1) for the contract this is
# measuring. This is a *report*, not a hard guard. Hard guards land in
# Phase 5 (commit 17) once each phase has driven its signal to its target.
#
# Usage: scripts/audit_editor_ownership.sh
#        make audit-editor-ownership

set -eu

print_section() {
    printf '\n== %s ==\n' "$1"
}

# 1. Editor / UI-shaped slices still exposed through repl_state_*.
#    Phase 1 (commits 3-7) drives this to zero outside repl_state.c +
#    forwarder shims. Tests are excluded so the signal tracks shipping
#    code, not test scaffolding.
print_section "editor/ui-like state still exposed through repl_state"
grep -RInE \
  'repl_state_(editor_|clipboard|selection|autocomplete|search|status|variable_panel|variable_drag|help|code_panel|profile_panel|viewport|pointer|camera)' \
  --include='*.c' --include='*.h' . \
  | grep -v '^\./tests/' \
  | grep -v 'EDITOR_OWNERSHIP_TODO' \
  || true

# 2. repl_command_store_*_with_line[s] usage. Phase 2 (commit 8) drops
#    the text-aware overload; this should land at zero.
print_section "command store text-aware API usage"
grep -RInE \
  'repl_command_store_.*_with_line|repl_command_store_.*_with_lines' \
  --include='*.c' --include='*.h' . \
  | grep -v 'EDITOR_OWNERSHIP_TODO' \
  || true

# 3. REPL modules reading editor-buffer text directly. Phase 2 (commit 9)
#    converts these to EditorBufferView parameters.
print_section "REPL modules reading editor-buffer text directly"
grep -RInE \
  'repl_state_editor_buffer|editor_buffer_line' \
  repl_*.c repl_*.h 2>/dev/null \
  | grep -v '^repl_state' \
  | grep -v 'EDITOR_OWNERSHIP_TODO' \
  || true

# 4. UI files calling state mutators directly. Phase 4 (commits 12-14)
#    routes these through UiAction + glr_ctrl_dispatch_action.
print_section "UI files with live mutation calls"
grep -RInE \
  'repl_(action|command_store|clipboard|undo|search|var_drag|autocomplete)_[a-z_]+\(|repl_state_.*_mut\(' \
  ui_*.c ui_*.h 2>/dev/null \
  | grep -v 'EDITOR_OWNERSHIP_TODO' \
  || true

# 5. repl_editor.c's dependency surface. Phase 5 (commit 15) splits this
#    file into editor_input.c + editor_commit.c + lifted-into-imrepl_ctrl.
#    The include count is a proxy for how broad the responsibility is.
print_section "repl_editor broad dependency inventory"
grep -nE '^#include ' repl_editor.c 2>/dev/null || true
