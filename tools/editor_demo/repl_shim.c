/*
 * tools/editor_demo/repl_shim.c - Demo-local stubs for the residual
 * REPL coupling in the editor's data model.
 *
 * Phase 8.7 dropped the REPL-flavored editor controller files
 * (input.c, commit.c, clipboard.c, undo.c, reformat.c, search.c,
 * completion.c, plus the inline_* / help_session) from the demo's
 * link set. The remaining link set is the editor data model
 * (state.c), the generic primitives (edit_ops.c), and the generic
 * UI render layer (text_panel.c, text_layout.c, text_search.c).
 *
 * During Phases 2-3 of plans/in-review/edit-line-ownership.md the
 * editor accessors editor_state_edit_line / _set / _clamp forward
 * to repl_state_edit_line / _set / _clamp. The shim implements
 * those REPL-side names against demo-local storage so both reads
 * and writes land on the same int. Phase 4 flips storage into
 * EditorState and these forwarders go away; Phase 5 deletes the
 * shim file.
 *
 * This file remains a small, scoped shim from the linker's
 * perspective; any new `repl_*` entry beyond the edit-line trio
 * is a layering-regression signal.
 */

#include <stddef.h>

/* Demo-local edit-line cursor. The shim's repl_state_edit_line /
 * _set / _clamp read and write this value; the demo's input
 * dispatcher goes through the editor accessor, which forwards to
 * these during the transition phases. */
static int g_demo_edit_line = 0;

int repl_state_edit_line(void) {
    return g_demo_edit_line;
}

void repl_state_edit_line_set(int n) {
    if (n < 0) n = 0;
    g_demo_edit_line = n;
}

void repl_state_edit_line_clamp(void) {
    /* The demo doesn't know the document count without pulling in
     * editor state from this TU, but the editor side already
     * clamps when reading (snapshot builder clamps to >= 0 and
     * <= buffer count). The shim's _clamp is a no-op stub kept
     * here only because the editor's _clamp forwarder calls it. */
    if (g_demo_edit_line < 0)
        g_demo_edit_line = 0;
}
