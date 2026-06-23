#!/bin/bash
# Hard guard: stale cross-module symbol prefixes do not reappear.
#
# The module-naming cleanup (docs/plans/in-review/module-naming-convention.md)
# renamed every exported symbol so its prefix follows the owning
# directory (src/repl -> repl_/Repl, src/editor -> editor_/Editor,
# src/ui -> ui_/Ui, src/app -> glr_/Glr, src/scene -> scene_/Scene,
# src/subsystems/replay -> replay_/Replay). This guard is a denylist of the
# exact stale names that were eliminated: if any reappears anywhere
# under src/ it is a regression (a refactor that moved code without
# re-prefixing, or a revert).
#
# This is deliberately a denylist of removed names, NOT a blanket
# "no foreign prefix" sweep. Borrowed cross-module API types are
# correct C design and must keep passing: ReplCompileContext /
# ReplCompiledChange in src/editor/commit.h, the Repl* snapshot
# fields in src/ui/app/snapshot.h, the export/replay-annotation bridge
# types in src/app/glr_ctrl.h, and VariablePanelViewState surfaced by
# src/subsystems/variable_panel/variable_panel_state.h. None of those are in the list
# below, so they are not flagged.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# Whole-word stale names (types, enums, functions) eliminated by the
# cleanup. Word boundaries keep the new names safe: \bfeed_line\b does
# not match editor_feed_line, \btry_commit_any\b does not match
# editor_try_commit_any, etc.
word_deny='ReplEditorBuffer|ReplEditorInputState|ReplEditorInputView|ReplSelectionState|ReplClipboardState|ReplSearchState|ReplAutocompleteState|ReplVariableDragState|ReplModifierProvider|ReplInputDispatchEffects|ReplUndoSnapshot|ReplUndoRingState|ReplCodePanelRuntimeState|ReplHelpState|ReplProfilePanelState|ReplStatusState|ReplPointerState|ReplViewportState|ReplVariablePanelState|ReplSyntaxKind|ReplSyntaxSpan|ReplMenuId|ReplCameraState|ReplVertexWalkState|ReplVertexWalkContext|ReplVertexWalkCallbacks|ReplTessPreviewCallbacks|EditorTransformer|EditorTransformerList|EditorHighlight|EditorHighlightList|EditorVirtualLine|EditorVirtualLineList|EditorLineOverride|EditorLineOverrideList|TransformerKind|HighlightKind|VirtualLineStyle|CodePanelLayout|ProfilePanelMode|GridTheme|AxesTheme|GridMajorIdx|GridExtentIdx|apply_state_cmd|accept_autocomplete|search_clear_all|handle_search_key|handle_search_special|try_commit_float_decl|try_assign_variable|try_commit_for_loop|try_commit_func_def|try_commit_if_block|try_commit_close_brace|try_commit_var_statements|try_commit_var_statements_then_insert|try_commit_block_structs|try_commit_any|delete_cmd_range|load_line_to_input|feed_line|navigate_to_line|REPL_SCENE_FIXED_COUNT'

# Prefix families (no trailing word boundary — the suffix varies).
prefix_deny='REPL_FILE_ITEM|REPL_MENU_BAR_PIN'

violations=$(grep -rnwE "($word_deny)" --include='*.c' --include='*.h' src 2>/dev/null || true)
violations="$violations
$(grep -rnE "\\b($prefix_deny)" --include='*.c' --include='*.h' src 2>/dev/null || true)"

violations=$(printf '%s\n' "$violations" | grep -vE '^[[:space:]]*$' || true)

if [ -n "$violations" ]; then
    echo "ERROR: a stale pre-cleanup symbol prefix reappeared under src/." >&2
    echo "Prefix must follow the owning directory; see" >&2
    echo "docs/plans/in-review/module-naming-convention.md and the" >&2
    echo "Sanctioned Exceptions / Conventions in docs/MODULES.md." >&2
    echo "Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "module-prefixes OK"
