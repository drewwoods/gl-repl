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
 *   so this stub goes away too. (Plan options A and B documented
 *   under "What's still open" in plans/active/editor-demo.md.)
 *
 * Phase 8b (multi-line + click): the shim's repl_state_edit_line()
 * returns demo-local backing storage that the demo's input
 * dispatcher writes through demo_edit_line_set(). That makes the
 * demo a real multi-line editor today, while still leaving the
 * underlying coupling (REPL owns edit-line) for the eventual
 * cleanup phase.
 *
 * This file remains a one-symbol shim from the linker's
 * perspective; any second `repl_*` entry is a layering-regression
 * signal.
 */

#include <stddef.h>

/* Demo-local edit-line cursor. The shim's repl_state_edit_line()
 * returns this value; the demo's input dispatcher updates it via
 * demo_edit_line_set() during Enter / up / down / click
 * navigation. */
static int g_demo_edit_line = 0;

int repl_state_edit_line(void) {
    return g_demo_edit_line;
}

/* Demo-internal setter. Declared at the use sites in editor_demo.c
 * and input.c via a `void demo_edit_line_set(int);` extern line
 * (no header -- kept inline to avoid a new file for one symbol).
 * Clamps negatives to zero; otherwise stores verbatim so callers
 * can park the cursor on a "virtual" line past the last committed
 * buffer entry (the snapshot builder handles that case). */
void demo_edit_line_set(int n) {
    if (n < 0) n = 0;
    g_demo_edit_line = n;
}
