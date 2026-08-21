/*
 * glr_variable_panel_bridge.c - REPL/editor side of the variable panel.
 *
 * The drag *math* belongs to the peer (variable_panel_drag.c: pixels ->
 * requested value); what a requested value means to the document belongs
 * here. Carved out of glr_ctrl_router.c, which owned it only because a drag
 * starts with a mouse press - none of it inspects an input event. It stays on
 * this side of the boundary rather than moving into the subsystem because it
 * compiles REPL statements and commits editor changes, and the peer must keep
 * linking standalone in variable_panel_demo.
 *
 * The split across the two writes below is the load-bearing part: motion
 * updates the live value only, and release rewrites the declaration exactly
 * once. Rewriting the source per pointer event would mark it dirty hundreds
 * of times in one gesture (and, downstream, would make the external-editor
 * watcher emit a file write per event instead of per drag).
 */
#include "app/glr_variable_panel_bridge.h"

#include "editor/commit.h"
#include "editor/input.h"
#include "editor/state.h"
#include "repl/compile.h"
#include "repl/eval.h"
#include "repl/host_effects.h"
#include "repl/state_views.h"
#include <stdio.h>
#include <string.h>

/* Drag-begin reads the declared name + value straight from the REPL eval
 * table. The peer takes it as an installed callback so variable_panel_demo
 * can supply its own in-memory table instead. */
static int glr_variable_panel_read_row(int row, char *name_out, int name_cap,
                                       float *value_out) {
    ReplPredefView predef = repl_eval_predef_view();
    if (row < 0 || row >= predef.count) return 0;
    snprintf(name_out, (size_t)name_cap, "%s", predef.vars[row].name);
    *value_out = predef.vars[row].value;
    return 1;
}

static const VariablePanelValueSource g_glr_var_value_source = {
    glr_variable_panel_read_row,
};

void glr_variable_panel_install_value_source(void) {
    variable_panel_install_value_source(&g_glr_var_value_source);
}


void glr_variable_panel_apply_value_change(
        const VariablePanelValueChange *value_change) {
    ReplCompiledChange compiled;
    ReplCompileContext ctx;
    VariablePanelDragState drag;
    char err[REPL_STATUS_TEXT_MAX] = "";
    int var_idx;
    int capture_undo;

    if (!value_change || !value_change->name[0])
        return;

    drag = variable_panel_drag();
    var_idx = drag.var_idx;
    ReplPredefView predef = repl_eval_predef_view();
    if (var_idx < 0 || var_idx >= predef.count)
        return;
    if (strcmp(predef.vars[var_idx].name, value_change->name) != 0) {
        var_idx = repl_eval_find_predef_var_idx(value_change->name);
        if (var_idx < 0)
            return;
    }
    if (predef.vars[var_idx].value == value_change->value)
        return;

    ctx = repl_compile_context_from_live(editor_state_edit_line());
    /* repl_compile_context_from_live(editor_state_edit_line()) defaults insert_mode to 0
     * (non-editor convention); the editor-side caller knows the live
     * value and overrides. */
    ctx.insert_mode = editor_insert_mode();
    /* Live-only: motion updates the variable's value, never its declaration
     * text. Rewriting the source on every pointer event would mark the source
     * dirty hundreds of times per drag; the release handler persists the
     * settled value once instead. */
    if (repl_compile_set_predef_value_live(value_change->name,
                                           value_change->value,
                                           &ctx, &compiled,
                                           err, sizeof(err)) != REPL_COMPILE_OK) {
        repl_set_status_error(err[0] ? err : "Variable update failed");
        return;
    }

    capture_undo = !variable_panel_drag_undo_snapshot_pushed();
    if (!editor_commit_apply_external_change(&compiled, capture_undo, 0)) {
        repl_set_status_error("Command buffer full!");
        return;
    }
    if (capture_undo)
        variable_panel_drag_mark_undo_snapshot_pushed();
    variable_panel_drag_note_applied_value(value_change->value);
}

/* Mouse-up: write the value the drag settled on back into the variable's
 * declaration, once. Motion applied the live value only (see
 * glr_variable_panel_apply_value_change), so this is the drag's single
 * source mutation - one dirty mark, one re-flatten from cold state, and the
 * saved source matches what the scene renders.
 *
 * No undo snapshot: the first motion already captured one at the drag-start
 * value. No tutorial notify: the change carries no predef op, and the live
 * value it persists was notified when it was applied. A drag with no motion,
 * or a variable with no declaration row, does no work at all. */
void glr_variable_panel_persist_drag_value(
        const VariablePanelDragState *drag) {
    ReplCompiledChange compiled;
    ReplCompileContext ctx;
    char err[REPL_STATUS_TEXT_MAX] = "";

    if (!drag->value_changed || !drag->name[0])
        return;

    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ctx.insert_mode = editor_insert_mode();
    if (repl_compile_persist_predef_value(drag->name, drag->final_value,
                                          &ctx, &compiled,
                                          err, sizeof(err)) != REPL_COMPILE_OK) {
        repl_set_status_error(err[0] ? err : "Variable update failed");
        return;
    }
    if (compiled.kind == REPL_COMPILED_NO_CHANGE)
        return;   /* no declaration row to rewrite */

    /* On failure the already-applied live value stands; Undo still restores
     * the drag-start snapshot. */
    if (!editor_commit_apply_external_change(&compiled, /*capture_undo=*/0,
                                             /*publish_status=*/0)) {
        repl_set_status_error("Command buffer full!");
        return;
    }

    if (compiled.pos == editor_state_edit_line())
        editor_load_line_to_input(compiled.pos);
}
