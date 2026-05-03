/*
 * editor_commit.c -- Editor-side orchestration for compile/apply commits.
 *
 * The orchestration shape is the dual of repl_compile():
 *
 *   editor_commit_apply_compiled_change(change)
 *       preflight repl_apply_can_apply_compiled_change(change)
 *       services.apply_predef_ops(change)         // predef-var cascade
 *       editor_buffer_apply_compiled_change(change)  // EditorState only
 *       services.apply_repl_change(change)        // ReplState only
 *
 * The mutating halves go through the EditorServices table so the
 * editor doesn't reach into `repl_apply_*` directly. Phase D commit
 * 25 grows this into the full `editor_commit_current_input` shape
 * (compile, undo, preflight, apply); for now this helper just
 * threads services through the apply path that already exists.
 *
 * Preflight gives the helper an all-or-nothing atomicity guarantee:
 * if the cmd-store can't accept the change, none of the three
 * halves run, so predef-vars, editor buffer, and cmd-store stay in
 * sync. Without the preflight a capacity failure would leave
 * predef-vars declared and (potentially) editor text written but
 * no cmd-store entry — exactly the partial-commit state the Phase C
 * transaction shape exists to prevent.
 *
 * The undo capture for migrated handlers still rides on
 * repl_undo_push_snapshot() pushed at the dispatch sites
 * (;-key, Enter, feed_line) in repl_editor.c. Phase D commit 25
 * replaces that with a per-commit transaction wrapping this
 * helper.
 */

#include "editor_commit.h"

#include "editor_services.h"
#include "editor_state.h"
#include "repl_apply.h"
#include "repl_compile.h"
#include "repl_command_store.h"
#include "repl_core_internal.h"
#include "repl_source_scope.h"
#include "repl_state.h"
#include "repl_undo.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void result_set_diagnostic(EditorCommitResult *result, const char *msg) {
    if (!result || !msg) return;
    strncpy(result->diagnostic, msg, sizeof(result->diagnostic) - 1);
    result->diagnostic[sizeof(result->diagnostic) - 1] = '\0';
    result->diagnostic_valid = 1;
}

static void result_set_commit_message(EditorCommitResult *result,
                                      const char *msg) {
    if (!result || !msg) return;
    strncpy(result->commit_message, msg, sizeof(result->commit_message) - 1);
    result->commit_message[sizeof(result->commit_message) - 1] = '\0';
    result->commit_message_valid = 1;
}

EditorCommitResult editor_commit_current_input(const struct EditorServices_s *services) {
    EditorCommitResult result = {0};
    if (!services) return result;

    const char *text = editor_input_text();

    ReplCompileContext ctx = services->context(services->user);
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    err[0] = '\0';

    ReplCompileResult cr = services->compile(text, &ctx, &change,
                                             err, sizeof(err),
                                             services->user);

    if (cr == REPL_COMPILE_ERROR) {
        /* Diagnostic flows through the result; no mutation. */
        result.consumed = 1;
        result_set_diagnostic(&result, err);
        return result;
    }

    if (change.kind == REPL_COMPILED_NO_CHANGE) {
        /* Caller falls through to the legacy try_commit_* chain
         * (or treats as unrecognized). */
        result.consumed = 0;
        return result;
    }

    /* Preflight before mutation. */
    if (!repl_apply_can_apply_compiled_change(&change)) {
        result.consumed = 1;
        result.capacity_failed = 1;
        return result;
    }

    /* Transaction boundary: compile + preflight succeeded, no
     * mutation has run yet. Capture undo here. */
    repl_undo_push_snapshot();

    /* Past the preflight every apply call below succeeds. */
    services->apply_predef_ops(&change, services->user);
    editor_buffer_apply_compiled_change(&change);
    services->apply_repl_change(&change, services->user);

    result.consumed = 1;
    result.mutated = 1;
    if (change.commit_message[0])
        result_set_commit_message(&result, change.commit_message);
    return result;
}

/* ---- EditorCommitPlan ------------------------------------------- */

void editor_commit_plan_init(EditorCommitPlan *plan) {
    if (!plan) return;
    memset(plan, 0, sizeof(*plan));
    repl_compiled_change_init(&plan->change);
    plan->effects.cursor_target            = EDITOR_COMMIT_NO_CURSOR_CHANGE;
    plan->effects.insert_mode_target       = EDITOR_COMMIT_NO_INSERT_MODE_CHANGE;
    plan->effects.clear_input              = 0;
    plan->effects.clear_pending_newline    = 0;
    plan->effects.load_line_after_apply    = 0;
    plan->effects.func_decl_resume_advance = 0;
    plan->effects.end_type                 = (int)CMD_TYPE_COUNT;
}

static void apply_post_effects(const EditorCommitPostEffects *effects) {
    if (!effects) return;

    /* Cursor target lands first so func-decl-resume + load_line
     * observe the post-cursor edit_line. */
    if (effects->cursor_target != EDITOR_COMMIT_NO_CURSOR_CHANGE) {
        int target = effects->cursor_target;
        if (target < 0) target = 0;
        if (target > repl_state_document_count())
            target = repl_state_document_count();
        repl_state_edit_line_set(target);
    }

    /* Func-decl resume advance. The compile step captured the
     * delta; consume it here. The matching read+clear of the
     * legacy global also happens at compile time so the global
     * stays consistent (commit 26d eliminates the global by
     * folding the read into compile-time entirely). */
    if (effects->end_type == (int)CMD_FUNC_END &&
        effects->func_decl_resume_advance > 0) {
        repl_state_edit_line_set(repl_state_edit_line() +
                                 effects->func_decl_resume_advance);
        if (repl_state_edit_line() > repl_state_document_count())
            repl_state_edit_line_set(repl_state_document_count());
    }

    if (effects->insert_mode_target != EDITOR_COMMIT_NO_INSERT_MODE_CHANGE)
        editor_insert_mode_set(effects->insert_mode_target ? 1 : 0);

    if (effects->clear_input) {
        editor_input_clear();
        editor_cursor_pos_set(0);
    }

    if (effects->clear_pending_newline)
        editor_pending_newline_clear();

    if (effects->load_line_after_apply)
        load_line_to_input(repl_state_edit_line());
}

int editor_commit_apply_plan(const EditorCommitPlan *plan) {
    if (!plan) return 0;

    /* Preflight before any mutation. */
    if (!repl_apply_can_apply_compiled_change(&plan->change))
        return 0;

    /* NOTE: undo capture is the caller's responsibility during the
     * Phase D transition. The legacy ;-key / Enter / feed_line
     * dispatch sites in repl_editor.c push a snapshot before
     * invoking try_commit_*; this helper deliberately does NOT
     * push a second snapshot to avoid double-capture. Once the
     * dispatch sites route through editor_commit_current_input
     * (which owns the transaction boundary) we can drop the
     * outer push and add it here. */

    /* REPL halves. */
    repl_apply_predef_ops(&plan->change);
    editor_buffer_apply_compiled_change(&plan->change);
    repl_apply_compiled_change(&plan->change);

    /* Editor post-effects. */
    apply_post_effects(&plan->effects);

    if (plan->commit_message_valid && plan->commit_message[0])
        set_status(plan->commit_message);

    return 1;
}

/* ---- editor_compile_close_brace --------------------------------- */

static void close_brace_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_cmd_indent(pos, buf, buf_sz);
    int len = (int)strlen(buf);
    if (len >= 2)
        len -= 2;
    else
        len = 0;
    if (len > buf_sz - 1)
        len = buf_sz - 1;
    memset(buf, ' ', (size_t)len);
    buf[len] = '\0';
}

ReplCompileResult editor_compile_close_brace(const char *input,
                                             const ReplCompileContext *ctx,
                                             EditorCommitPlan *out,
                                             char *err, int err_size) {
    (void)err;
    (void)err_size;
    if (!ctx || !out) return REPL_COMPILE_ERROR;

    editor_commit_plan_init(out);

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '}') {
        out->change.kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    int pos = ctx->insert_mode ? ctx->edit_line :
              (ctx->edit_line < ctx->document_count
                   ? ctx->edit_line : ctx->document_count);

    CmdType open_type = repl_source_scope_nearest_open_block_at(pos);
    CmdType end_type;
    const char *label;

    if (open_type == CMD_FOR_BEGIN) {
        end_type = CMD_FOR_END;
        label = "for-loop";
    } else if (open_type == CMD_FUNC_DEF) {
        end_type = CMD_FUNC_END;
        label = "func def";
    } else if (open_type == CMD_IF_BEGIN) {
        end_type = CMD_IF_END;
        label = "if-block";
    } else {
        out->change.kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    /* Read the func-decl resume delta. Important: the legacy
     * close_brace only clears the global on CMD_FUNC_END (via
     * apply_func_decl_resume's early-return for other end_types);
     * we mirror that here with peek + conditional take so the
     * delta survives across nested non-func close-braces.
     *
     * Phase D commit 26d folds the set side of the global into
     * compile too, eliminating the global entirely. */
    int resume_delta = repl_commit_func_decl_resume_delta_peek();

    int keep_inserting = (resume_delta > 0 && end_type != CMD_FUNC_END);

    /* Only CMD_FUNC_END consumes the delta. Take it (clear the
     * global) so the apply path observes the same one-shot
     * semantics as the legacy apply_func_decl_resume. For other
     * end_types we leave the global alone and forget the value. */
    if (end_type == CMD_FUNC_END)
        (void)repl_commit_func_decl_resume_delta_take();
    else
        resume_delta = 0;  /* not consumed by this close-brace */

    out->effects.end_type                 = (int)end_type;
    out->effects.func_decl_resume_advance = resume_delta;
    out->effects.insert_mode_target       = keep_inserting ? 1 : 0;
    out->effects.clear_input              = 1;

    /* Reuse-existing-end branch: cursor target advances past the
     * existing CMD_FOR_END / CMD_FUNC_END / CMD_IF_END and we
     * reload g_input from the new edit-line's text. */
    if (pos < ctx->document_count &&
        ctx->document_cmds[pos].type == end_type) {
        out->change.kind = REPL_COMPILED_NO_CHANGE;
        out->effects.cursor_target         = pos + 1;
        out->effects.load_line_after_apply = 1;
    } else {
        /* Insert-new-end-marker branch. */
        char indent[32];
        close_brace_indent(pos, indent, sizeof(indent));

        GLCmd fe;
        memset(&fe, 0, sizeof(fe));
        fe.type = end_type;
        fe.valid = 1;

        out->change.kind  = REPL_COMPILED_INSERT_ONE;
        out->change.pos   = pos;
        out->change.count = 1;
        out->change.adjust_edit_line = 0;
        out->change.cmds[0] = fe;
        snprintf(out->change.text[0], sizeof(out->change.text[0]),
                 "%s}", indent);

        out->effects.cursor_target         = pos + 1;
        out->effects.clear_pending_newline = 1;
    }

    snprintf(out->commit_message, sizeof(out->commit_message),
             "%s block closed", label);
    out->commit_message_valid = 1;

    return REPL_COMPILE_OK;
}

int editor_commit_apply_compiled_change(const struct ReplCompiledChange_s *change) {
    if (!change) return 0;

    /* Preflight: if the cmd-store can't accept the change, return
     * 0 before mutating anything. Pure read; doesn't go through
     * services. */
    if (!repl_apply_can_apply_compiled_change(change))
        return 0;

    /* Past the preflight every apply call below succeeds. */
    EditorServices svc = editor_services_default();
    svc.apply_predef_ops(change, svc.user);
    editor_buffer_apply_compiled_change(change);
    svc.apply_repl_change(change, svc.user);
    return 1;
}
