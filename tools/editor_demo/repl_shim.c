/*
 * tools/editor_demo/repl_shim.c - Demo-local stubs for the residual
 * REPL coupling in the editor's data model.
 *
 * Phase 8.7 dropped the REPL-flavored editor controller files
 * (input.c, commit.c, clipboard.c, undo.c, reformat.c, search.c,
 * completion.c, plus the inline_* / help_session) from the demo's
 * link set. The remaining link set is the editor data model
 * (state.c), the generic primitives (edit_ops.c), and the generic
 * UI render layer (text_panel.c, text_layout.c, text_search.c) --
 * none of which reach into REPL except for one site:
 *
 *   src/editor/state.c's EditorInputView builder calls
 *   repl_state_edit_line() to populate the view's edit_line_idx
 *   field. The editor doesn't own its own edit-line cursor today;
 *   a follow-up phase moves edit-line ownership into EditorState
 *   so this stub goes away too.
 *
 * Until that cleanup lands, the demo provides a single one-line
 * stub. This file is the visible record of "what generic editor
 * code still calls into REPL by name" -- currently a one-symbol
 * ledger. If a future change adds a second symbol, that's a
 * signal to consider whether the new dependency is a layering
 * regression to fix at the source, or whether it warrants its
 * own follow-up cleanup phase like the edit-line one.
 */

/* ---- Editor data-model leak: state.c reads the edit-line cursor
 * from REPL state today (rather than owning its own). Demo's
 * "current edit line" is always 0 -- we don't navigate between
 * buffer lines yet (out of v1 scope). ---- */

int repl_state_edit_line(void) {
    return 0;
}
