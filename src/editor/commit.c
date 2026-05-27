/*
 * editor_commit.c -- Editor-side orchestration for compile/apply commits.
 *
 * The orchestration shape is the dual of repl_compile():
 *
 *   editor_commit_apply_external_change(change, capture_undo, publish_status)
 *       preflight repl_apply_can_apply_compiled_change(change)
 *       repl_apply_predef_ops(change)                // predef-var cascade
 *       repl_apply_scratch_ops(change)               // scratch-array cascade
 *       editor_buffer_apply_compiled_change(change)  // editor text buffer
 *       repl_apply_compiled_change(change, &cursor)  // ReplState only
 *
 * Commit code now calls the repl_apply_* entry points directly.
 * Typed input runs through the editor_try_commit_* chain; each
 * handler owns its own compile/preflight/apply transaction and
 * surfaces status.
 *
 * Preflight gives the helper an all-or-nothing atomicity guarantee:
 * if the cmd-store can't accept the change, none of the four
 * halves run, so predef-vars, editor buffer, and cmd-store stay in
 * sync. Without the preflight a capacity failure would leave
 * predef-vars declared and (potentially) editor text written but
 * no cmd-store entry — exactly the partial-commit state this
 * transaction shape exists to prevent.
 *
 * Undo capture is owned by the dispatch sites in src/editor/input.c
 * (the ;-key / Enter routes push one snapshot before invoking the
 * try_commit_* chain). The bulk loader editor_feed_line() in the
 * same file deliberately skips per-line undo and brackets the whole
 * load with a single snapshot instead.
 */

#include "commit.h"
#include "completion.h"
#include "input.h"
#include "state.h"
#include "undo.h"

#include "repl/apply.h"
#include "repl/compile.h"
#include "repl/core_internal.h"
#include "repl/parser.h"
#include "repl/core.h"
#include "repl/state_owners.h"
#include "repl/source_scope.h"
#include "repl/eval.h"

#include "config.h"           /* REPL_INDENT_TEXT_MAX */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* File-local forward decls for the func-decl-resume bookkeeping
 * helpers, both of which are now `static` (definitions further
 * below). The publish writer is called from apply-plan and the
 * read-and-clear consumer from the close-brace compile path,
 * both of which sit above the definitions; the forward decls
 * keep `make check-c99` green (under -std=c99, implicit
 * declarations are a hard error). `_peek` stays public for the
 * test harness — see commit.h. */
static void editor_commit_func_decl_resume_set(int delta);
static int  editor_commit_func_decl_resume_take(void);

static void editor_compile_clear_err(char *err, int err_size) {
    if (err && err_size > 0)
        err[0] = '\0';
}

static ReplCompileResult editor_compile_error(char *err, int err_size,
                                              const char *fmt, ...) {
    if (err && err_size > 0) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(err, (size_t)err_size, fmt, args);
        va_end(args);
    }
    return REPL_COMPILE_ERROR;
}

/* Apply the predef-ops + scratch-ops + editor-buffer + cmd-store
 * steps of a compiled change. Preflight + undo capture are the
 * caller's job — this is the shared mutation sequence; each caller
 * decides its own preflight-failure policy and undo policy. The
 * edit-line is read into a local, threaded through apply, and
 * written back on success (matches the apply API's cursor-inout
 * contract; cursor ownership moved editor-side in Phase 1 of
 * plans/in-review/edit-line-ownership.md). */
static void apply_compiled_change_full(const ReplCompiledChange *change) {
    if (!repl_apply_can_apply_compiled_change(change))
        return;
    repl_apply_predef_ops(change);
    repl_apply_scratch_ops(change);
    editor_buffer_apply_compiled_change(change);
    int edit_line = editor_state_edit_line();
    if (repl_apply_compiled_change(change, &edit_line))
        editor_state_edit_line_set(edit_line);
}

static ReplCompileContext editor_compile_context_live(void) {
    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ctx.insert_mode = editor_insert_mode();
    return ctx;
}

int editor_commit_apply_external_change(const struct ReplCompiledChange_s *change,
                                        int capture_undo,
                                        int publish_status) {
    if (!change)
        return 0;
    if (!repl_apply_can_apply_compiled_change(change)) {
        /* Mirror editor_commit_apply_plan: roll back any speculative
         * alias registration when preflight fails. [P1] regression. */
        repl_compiled_change_rollback_alias(change);
        return 0;
    }

    if (capture_undo)
        editor_undo_push_snapshot();

    apply_compiled_change_full(change);

    if (publish_status && change->commit_message[0])
        repl_set_status(change->commit_message);

    return 1;
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
        editor_state_edit_line_set(target);
    }

    /* Func-decl resume advance. The compile step captured the
     * delta; consume it here. The matching read+clear of the
     * live global also happens at compile time so the global
     * stays consistent (commit 26d eliminates the global by
     * folding the read into compile-time entirely). */
    if (effects->end_type == (int)CMD_FUNC_END &&
        effects->func_decl_resume_advance > 0) {
        editor_state_edit_line_set(editor_state_edit_line() +
                                 effects->func_decl_resume_advance);
        if (editor_state_edit_line() > repl_state_document_count())
            editor_state_edit_line_set(repl_state_document_count());
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
        editor_load_line_to_input(editor_state_edit_line());

    if (effects->clear_autocomplete)
        editor_completion_clear();

    if (effects->func_decl_resume_publish)
        editor_commit_func_decl_resume_set(effects->func_decl_resume_publish_value);
}

int editor_commit_apply_plan(const EditorCommitPlan *plan) {
    if (!plan) return 0;

    /* Preflight before any mutation. */
    if (!repl_apply_can_apply_compiled_change(&plan->change)) {
        /* Roll back any speculative alias registration. [P1]
         * regression: see editor_commit_apply_external_change. */
        repl_compiled_change_rollback_alias(&plan->change);
        return 0;
    }

    /* NOTE: undo capture is the caller's responsibility. The ;-key /
     * Enter / editor_feed_line dispatch sites in src/editor/input.c
     * push a snapshot before invoking the try_commit_* chain; this
     * helper deliberately does NOT push a second snapshot, to avoid
     * double-capture. */

    apply_compiled_change_full(&plan->change);

    /* Editor post-effects. */
    apply_post_effects(&plan->effects);

    if (plan->change.commit_message[0])
        repl_set_status(plan->change.commit_message);

    return 1;
}

/* ---- editor_compile_close_brace --------------------------------- */

static void close_brace_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_cmd_indent(pos, buf, buf_sz);
    int len = (int)strlen(buf);
    /* Dedent one level (2 spaces) so the closing `}` lines up under its
     * block opener rather than under the block body. */
    if (len >= 2)
        len -= 2;
    else
        len = 0;
    if (len > buf_sz - 1)
        len = buf_sz - 1;
    memset(buf, ' ', (size_t)len);
    buf[len] = '\0';
}

/* CONTRACT (audit #11): context-pure for document data, live-state-
 * coupled for scope queries. The ReplCompileContext snapshot is
 * authoritative for ctx->document_cmds / _count / edit_line, but the
 * `repl_source_scope_*` calls below read from the live g_repl_state
 * document. Callers must apply each change to the live document
 * before the next scope-dependent compile call, or the scope queries
 * will misbehave. */
ReplCompileResult editor_compile_close_brace(const char *input,
                                             const ReplCompileContext *ctx,
                                             EditorCommitPlan *out,
                                             char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;

    editor_commit_plan_init(out);
    editor_compile_clear_err(err, err_size);

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
        return editor_compile_error(err, err_size, "unmatched '}'");
    }

    /* Read the func-decl resume delta. The delta is one-shot and only
     * a CMD_FUNC_END close-brace consumes it; peek + conditional take
     * lets it survive across nested non-func close-braces. */
    int resume_delta = editor_commit_func_decl_resume_peek();

    int keep_inserting = (resume_delta > 0 && end_type != CMD_FUNC_END);

    /* Only CMD_FUNC_END consumes the delta: take it (clearing the
     * file-private store) so the apply path sees the one-shot value.
     * For other end_types we leave the store alone and forget the
     * value locally. */
    if (end_type == CMD_FUNC_END)
        (void)editor_commit_func_decl_resume_take();
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
        char indent[REPL_INDENT_TEXT_MAX];
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

    snprintf(out->change.commit_message, sizeof(out->change.commit_message),
             "%s block closed", label);

    return REPL_COMPILE_OK;
}

/* ---- editor_compile_if_block ------------------------------------ */

/* CONTRACT (audit #11): context-pure for document data, live-state-
 * coupled for both scope queries (repl_source_scope_*) and visible-
 * var collection (collect_visible_vars in src/repl/core.c). The
 * ReplCompileContext snapshot is authoritative for document_cmds /
 * _count / edit_line, but the helpers above read from the live
 * g_repl_state document. Callers must apply each change to the live
 * document before the next scope-dependent or visible-vars compile
 * call. */
ReplCompileResult editor_compile_if_block(const char *input,
                                          const ReplCompileContext *ctx,
                                          EditorCommitPlan *out,
                                          char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;

    editor_commit_plan_init(out);
    editor_compile_clear_err(err, err_size);

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "if(", 3) != 0 && strncmp(p, "if (", 4) != 0) {
        out->change.kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    int pos = ctx->insert_mode ? ctx->edit_line :
              (ctx->edit_line < ctx->document_count
                   ? ctx->edit_line : ctx->document_count);

    ExprVar visible_vars[MAX_EXPR_VARS];
    int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS, NULL);

    /* Skip past `if` to the opening `(`. */
    while (*p && *p != '(') p++;
    if (!*p)
        return editor_compile_error(err, err_size, "if syntax: if(expr) {");
    p++;

    char cond_text[MAX_LINE_LEN];
    int  clen;
    {
        int paren = 1;
        const char *expr_start = p;
        while (*p && paren > 0) {
            if (*p == '(')      paren++;
            else if (*p == ')') paren--;
            if (paren > 0) p++;
        }
        if (paren != 0)
            return editor_compile_error(err, err_size, "if syntax: if(expr) {");
        clen = (int)(p - expr_start);
        if (clen > (int)sizeof(cond_text) - 1)
            clen = (int)sizeof(cond_text) - 1;
        memcpy(cond_text, expr_start, (size_t)clen);
        cond_text[clen] = '\0';
    }

    {
        char verr[REPL_DIAG_TEXT_MAX];
        if (!repl_eval_validate_expression_idents(cond_text,
                                                  visible_nv > 0 ? visible_vars : NULL,
                                                  visible_nv,
                                                  verr, sizeof(verr)))
            return editor_compile_error(err, err_size, "%s", verr);
    }

    float cond_val = 0.0f;
    {
        float cond_args[1];
        int neval = repl_eval_parse_exprs(cond_text, cond_args, 1,
                                          visible_nv > 0 ? visible_vars : NULL,
                                          visible_nv);
        cond_val = (neval >= 1) ? cond_args[0] : 0.0f;
    }

    /* Skip past `)`. */
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' && *p != '\0')
        return editor_compile_error(err, err_size, "if syntax: if(expr) {");

    char indent[REPL_INDENT_TEXT_MAX];
    repl_source_scope_cmd_indent(pos, indent, sizeof(indent));

    /* Build CMD_IF_BEGIN. */
    GLCmd ib;
    memset(&ib, 0, sizeof(ib));
    ib.type = CMD_IF_BEGIN;
    ib.args[0] = cond_val;
    ib.valid = 1;
    ib.has_vars = input_has_any_visible_vars(cond_text, visible_vars, visible_nv);

    /* Format the begin line text. cond_text gets trimmed in place. */
    char ib_text[MAX_LINE_LEN];
    {
        char *ct = cond_text;
        int ctlen;
        while (*ct && isspace((unsigned char)*ct)) ct++;
        ctlen = (int)strlen(ct);
        while (ctlen > 0 && isspace((unsigned char)ct[ctlen - 1]))
            ct[--ctlen] = '\0';
        snprintf(ib_text, sizeof(ib_text), "%sif(%s) {", indent, ct);
    }

    /* Header-replace branch: cursor sits on an existing CMD_IF_BEGIN
     * in non-insert mode → REPLACE_ONE. */
    if (!ctx->insert_mode &&
        ctx->edit_line < ctx->document_count &&
        ctx->document_cmds[ctx->edit_line].type == CMD_IF_BEGIN) {
        out->change.kind  = REPL_COMPILED_REPLACE_ONE;
        out->change.pos   = ctx->edit_line;
        out->change.count = 1;
        out->change.cmds[0] = ib;
        snprintf(out->change.text[0], sizeof(out->change.text[0]),
                 "%s", ib_text);

        out->effects.cursor_target       = ctx->edit_line + 1;
        out->effects.insert_mode_target  = 1;
        out->effects.clear_input         = 1;
        out->effects.clear_autocomplete  = 1;

        snprintf(out->change.commit_message, sizeof(out->change.commit_message),
                 "if condition updated");
        return REPL_COMPILE_OK;
    }

    /* Insert-new-block branch: INSERT_MANY (begin + end). */
    GLCmd ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = CMD_IF_END;
    ie.valid = 1;

    char ie_text[MAX_LINE_LEN];
    snprintf(ie_text, sizeof(ie_text), "%s}", indent);

    out->change.kind  = REPL_COMPILED_INSERT_MANY;
    out->change.pos   = pos;
    out->change.count = 2;
    out->change.cmds[0] = ib;
    out->change.cmds[1] = ie;
    snprintf(out->change.text[0], sizeof(out->change.text[0]),
             "%s", ib_text);
    snprintf(out->change.text[1], sizeof(out->change.text[1]),
             "%s", ie_text);

    out->effects.cursor_target      = pos + 1;
    out->effects.insert_mode_target = 1;
    out->effects.clear_input        = 1;

    snprintf(out->change.commit_message, sizeof(out->change.commit_message),
             "if-block: type body lines, press Esc when done");
    return REPL_COMPILE_OK;
}

/* ---- editor_compile_func_def ------------------------------------ */

/* Walk backward from `pos` collecting depth-0 comment/blank rows.
 * Compile-time variant: reads from the ReplCompileContext document
 * snapshot instead of live REPL state. */
static int compile_func_leading_comment_start(const ReplCompileContext *ctx,
                                              int pos) {
    int start = pos;
    while (start > 0 &&
           ctx->document_cmds[start - 1].valid &&
           (ctx->document_cmds[start - 1].type == CMD_COMMENT ||
            ctx->document_cmds[start - 1].type == CMD_EMPTY) &&
           repl_source_scope_block_depth_at(start - 1) == 0)
        start--;
    return start;
}

/* Walk function_decl_insert_pos's logic against a virtually-deleted
 * document where indices [delete_pos, delete_pos+delete_count) are
 * gone. Returns the insert position in post-delete coordinates.
 *
 * In the func_def relocation case, the deleted range is
 * always contiguous depth-0 CMD_COMMENT/CMD_EMPTY rows that
 * function_decl_insert_pos's walk would skip; the deleted indices
 * therefore lie strictly outside the walk's stopping position.
 * That keeps the math simple: walk normally, then translate by
 * subtracting delete_count if the result was past the deleted
 * range. */
static int compile_function_decl_insert_pos_after_delete(
        const ReplCompileContext *ctx, int delete_pos, int delete_count) {
    const GLCmd *cmds = ctx->document_cmds;
    int doc_count = ctx->document_count;

    int pos = 0;
    while (pos < doc_count && cmds[pos].type == CMD_VAR_DECLARE)
        pos++;

    while (pos < doc_count) {
        if (cmds[pos].type == CMD_COMMENT || cmds[pos].type == CMD_EMPTY) {
            pos++;
            continue;
        }
        if (cmds[pos].type != CMD_FUNC_DEF)
            break;

        int end = repl_source_scope_find_block_end(pos);
        if (end >= doc_count)
            return doc_count - delete_count;
        pos = end + 1;
    }

    /* Translate to post-delete coordinates. By construction the
     * deleted range never straddles `pos` (see comment above). */
    if (delete_count > 0 && pos >= delete_pos + delete_count)
        pos -= delete_count;
    return pos;
}

/* CONTRACT (audit #11): context-pure for document data, live-state-
 * coupled for scope queries (repl_source_scope_*). The
 * ReplCompileContext snapshot is authoritative for document_cmds /
 * _count / edit_line, but the helpers above read from the live
 * g_repl_state document. Callers must apply each change to the live
 * document before the next scope-dependent compile call. */
ReplCompileResult editor_compile_func_def(const char *input,
                                          const ReplCompileContext *ctx,
                                          EditorCommitPlan *out,
                                          char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;

    editor_commit_plan_init(out);
    editor_compile_clear_err(err, err_size);

    int fn = -1;
    int param_count = 0;
    char param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];

    /* Quick-reject inputs that look like func *calls* (have `(` and
     * no `{`). The live guard returns 0 from editor_try_commit_func_def
     * for those so the dispatch chain falls through to
     * parse_command, which classifies them as CMD_CALL. */
    const char *trimmed = input ? input : "";
    while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
    if (strchr(trimmed, '(') != NULL && strchr(trimmed, '{') == NULL) {
        out->change.kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    /* Alias-aware pre-step: parse_repl_func_signature only knows the
     * bare `funcN` form plus already-registered aliases. New user
     * names (`drawCube { ... }`) are unregistered the first time the
     * user types them, so we register them up front before invoking
     * the parser.
     *
     * Slot picking: if the cursor is on an existing CMD_FUNC_DEF and
     * we're not in insert mode, the rename targets that line's slot
     * (so `drawCube` -> `drawSphere` reuses slot N). Otherwise pick
     * the next free slot. The bare-funcN branch is a no-op fast path.
     *
     * The pre-step mutates the global alias table; the rollback at
     * each downstream failure path (parse, dup, format) makes that
     * speculative. The success path publishes the touched slot and its
     * previous contents via out->change so editor_commit_apply_plan can
     * roll it back if its own preflight fails. [P2 editor] regression. */
    int rejected_keyword = 0;
    ReplCompileResult alias_res = repl_compile_func_def_resolve_alias(ctx, trimmed, &out->change, &rejected_keyword, err, err_size);
    if (alias_res != REPL_COMPILE_OK)
        return alias_res;
    if (rejected_keyword)
        return REPL_COMPILE_OK;

    if (!parse_repl_func_signature(input ? input : "", &fn,
                                   param_names, MAX_EXPR_VARS,
                                   &param_count)) {
        repl_compiled_change_rollback_alias(&out->change);
        out->change.kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    int edit_pos = ctx->insert_mode ? ctx->edit_line :
                   (ctx->edit_line < ctx->document_count
                        ? ctx->edit_line : ctx->document_count);
    int overwriting_func = (!ctx->insert_mode &&
                            edit_pos < ctx->document_count &&
                            ctx->document_cmds[edit_pos].type == CMD_FUNC_DEF);

    /* Reject duplicate-funcN definitions; overwriting the existing
     * line is allowed. */
    for (int ei = 0; ei < ctx->document_count; ei++) {
        const GLCmd *c = &ctx->document_cmds[ei];
        if (!c->valid) continue;
        if (c->type != CMD_FUNC_DEF) continue;
        if ((int)c->args[0] != fn) continue;
        if (overwriting_func && ei == edit_pos) continue;
        repl_compiled_change_rollback_alias(&out->change);
        return editor_compile_error(err, err_size,
                                    "func%d already defined (line %d)", fn, ei + 1);
    }

    /* Overwrite-header branch: REPLACE_ONE at the cursor line. */
    if (overwriting_func) {
        char indent[REPL_INDENT_TEXT_MAX];
        repl_source_scope_cmd_indent(edit_pos, indent, sizeof(indent));

        GLCmd updated = ctx->document_cmds[edit_pos];
        updated.args[0] = (float)fn;
        updated.num_args = param_count;

        char fd_text[MAX_LINE_LEN];
        format_func_header(fd_text, (int)sizeof(fd_text),
                           indent, fn, param_names, param_count);

        out->change.kind  = REPL_COMPILED_REPLACE_ONE;
        out->change.pos   = edit_pos;
        out->change.count = 1;
        out->change.cmds[0] = updated;
        snprintf(out->change.text[0], sizeof(out->change.text[0]),
                 "%s", fd_text);

        out->effects.cursor_target      = edit_pos + 1;
        out->effects.insert_mode_target = 1;
        out->effects.clear_input        = 1;
        out->effects.clear_autocomplete = 1;

        snprintf(out->change.commit_message, sizeof(out->change.commit_message),
                 "func def header updated");
        return REPL_COMPILE_OK;
    }

    /* New-func-def branch: insert + comment-relocation.
     *
     * The live code:
     *   1. delete leading comments at [comment_start, edit_pos)
     *   2. recompute function_decl_insert_pos against the
     *      post-delete document
     *   3. insert comments + fd + fe at the new position
     *   4. publish g_func_decl_resume_delta = resume_pos - pos
     *
     * The compile path produces an EditorCommitPlan that expresses
     * the same shape via the new delete_pos/delete_count fields and
     * the func_decl_resume_publish post-effect. */
    int comment_start = compile_func_leading_comment_start(ctx, edit_pos);
    int comment_count = edit_pos - comment_start;

    int insert_count = comment_count + 2;  /* comments + fd + fe */
    if (insert_count > MAX_COMMIT_CMDS)
        return editor_compile_error(err, err_size,
                                    "too many leading comments (%d > %d); split or shorten",
                                    comment_count, MAX_COMMIT_CMDS - 2);

    /* A func decl always lives at depth 0, so its indent is the
     * depth-0 indent regardless of the (post-delete) insert position
     * computed below — query the source-scope helper at pos 0. */
    char indent[REPL_INDENT_TEXT_MAX];
    repl_source_scope_cmd_indent(0, indent, sizeof(indent));

    GLCmd fd;
    memset(&fd, 0, sizeof(fd));
    fd.type = CMD_FUNC_DEF;
    fd.args[0] = (float)fn;
    fd.num_args = param_count;
    fd.valid = 1;

    char fd_text[MAX_LINE_LEN];
    format_func_header(fd_text, (int)sizeof(fd_text),
                       indent, fn, param_names, param_count);

    GLCmd fe;
    memset(&fe, 0, sizeof(fe));
    fe.type = CMD_FUNC_END;
    fe.valid = 1;

    char fe_text[MAX_LINE_LEN];
    snprintf(fe_text, sizeof(fe_text), "%s}", indent);

    /* Resolve the post-delete insert position. */
    int insert_pos = compile_function_decl_insert_pos_after_delete(
        ctx, comment_start, comment_count);

    /* Fill the change's cmds[] / text[] with comments + fd + fe.
     * Comment text comes from the source-text view; the cmds
     * themselves come from the document. */
    SourceTextView text_view = ctx->text;
    for (int i = 0; i < comment_count; i++) {
        out->change.cmds[i] = ctx->document_cmds[comment_start + i];
        const char *line = source_text_line(text_view,
                                            comment_start + i);
        if (line)
            repl_copy_string_fits(out->change.text[i],
                                  sizeof(out->change.text[i]), line);
    }
    out->change.cmds[comment_count]     = fd;
    out->change.cmds[comment_count + 1] = fe;
    snprintf(out->change.text[comment_count],
             sizeof(out->change.text[comment_count]), "%s", fd_text);
    snprintf(out->change.text[comment_count + 1],
             sizeof(out->change.text[comment_count + 1]), "%s", fe_text);

    out->change.kind  = REPL_COMPILED_INSERT_MANY;
    out->change.pos   = insert_pos;
    out->change.count = insert_count;
    out->change.delete_pos   = comment_count > 0 ? comment_start : -1;
    out->change.delete_count = comment_count;

    /* Resume bookkeeping. resume_pos is where the cursor would be in
     * the post-delete document. Publish the delta so a subsequent
     * close-brace's compile reads it. */
    int resume_pos = edit_pos - comment_count;
    int resume_delta = (resume_pos > insert_pos) ? (resume_pos - insert_pos) : 0;
    out->effects.func_decl_resume_publish       = 1;
    out->effects.func_decl_resume_publish_value = resume_delta;

    /* Cursor lands on the first body line of the new func — that's
     * insert_pos + comment_count + 1 in post-delete coordinates. */
    out->effects.cursor_target      = insert_pos + comment_count + 1;
    out->effects.insert_mode_target = 1;
    out->effects.clear_input        = 1;

    snprintf(out->change.commit_message, sizeof(out->change.commit_message),
             "func def: type body lines, press Esc when done");

    return REPL_COMPILE_OK;
}

/* ---- editor_compile_for_loop ------------------------------------ */

/* CONTRACT (audit #11): context-pure for document data, live-state-
 * coupled for both scope queries (repl_source_scope_*) and visible-
 * var collection (collect_visible_vars in src/repl/core.c). The
 * ReplCompileContext snapshot is authoritative for document_cmds /
 * _count / edit_line, but the helpers above read from the live
 * g_repl_state document. Callers must apply each change to the live
 * document before the next scope-dependent or visible-vars compile
 * call. */
ReplCompileResult editor_compile_for_loop(const char *input,
                                          const ReplCompileContext *ctx,
                                          EditorCommitPlan *out,
                                          char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;

    editor_commit_plan_init(out);
    editor_compile_clear_err(err, err_size);

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "for(", 4) != 0 && strncmp(p, "for (", 5) != 0) {
        out->change.kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    int pos = ctx->insert_mode ? ctx->edit_line :
              (ctx->edit_line < ctx->document_count
                   ? ctx->edit_line : ctx->document_count);

    ExprVar visible_vars[MAX_EXPR_VARS];
    int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS, NULL);

    char var_name[REPL_PREDEF_NAME_MAX];
    float start, end, step;
    const char *body_start;

    if (!repl_eval_parse_for_header_with_vars(p, var_name, sizeof(var_name),
                                              &start, &end, &step,
                                              visible_vars, visible_nv,
                                              &body_start))
        return editor_compile_error(err, err_size,
                                    "for syntax: for(var, start, end[, step]) body;");

    while (*body_start && isspace((unsigned char)*body_start))
        body_start++;

    char indent[REPL_INDENT_TEXT_MAX];
    repl_source_scope_cmd_indent(pos, indent, sizeof(indent));
    int ind = (int)strlen(indent);

    /* Build CMD_FOR_BEGIN. */
    GLCmd fb;
    memset(&fb, 0, sizeof(fb));
    fb.type = CMD_FOR_BEGIN;
    fb.args[0] = start;
    fb.args[1] = end;
    fb.args[2] = step;
    fb.valid = 1;

    char fb_text[MAX_LINE_LEN];
    {
        /* Re-walk the input to extract the args between the outer
         * `(` and `)` so the formatted line preserves the user's
         * symbolic form when args contain visible vars. */
        const char *raw = p;
        while (*raw && *raw != '(') raw++;
        if (*raw) raw++;
        while (*raw && isspace((unsigned char)*raw)) raw++;
        while (*raw && repl_eval_is_ident_continue((unsigned char)*raw)) raw++;
        while (*raw && isspace((unsigned char)*raw)) raw++;
        if (*raw == ',') raw++;

        const char *args_start = raw;
        int paren = 1;
        const char *ap = args_start;
        char raw_args[MAX_LINE_LEN];
        int rlen;

        while (*ap && paren > 0) {
            if (*ap == '(')      paren++;
            else if (*ap == ')') paren--;
            if (paren > 0) ap++;
        }
        rlen = (int)(ap - args_start);
        if (rlen > (int)sizeof(raw_args) - 1)
            rlen = (int)sizeof(raw_args) - 1;
        memcpy(raw_args, args_start, (size_t)rlen);
        raw_args[rlen] = '\0';
        while (rlen > 0 && isspace((unsigned char)raw_args[rlen - 1]))
            raw_args[--rlen] = '\0';

        char *ra = raw_args;
        while (*ra && isspace((unsigned char)*ra)) ra++;

        char verr[REPL_DIAG_TEXT_MAX];
        if (!repl_eval_validate_expression_idents(ra, visible_vars, visible_nv,
                                                  verr, sizeof(verr)))
            return editor_compile_error(err, err_size, "%s", verr);

        if (input_has_any_visible_vars(ra, visible_vars, visible_nv)) {
            fb.has_vars = 1;
            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                  "%sfor(%s, %s) {",
                                  indent, var_name, ra))
                return editor_compile_error(err, err_size, "Command too long");
        } else if (step != 1.0f) {
            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                  "%sfor(%s, %.9g, %.9g, %.9g) {",
                                  indent, var_name, start, end, step))
                return editor_compile_error(err, err_size, "Command too long");
        } else {
            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                  "%sfor(%s, %.9g, %.9g) {",
                                  indent, var_name, start, end))
                return editor_compile_error(err, err_size, "Command too long");
        }
    }

    /* Build CMD_FOR_END. */
    GLCmd fe;
    memset(&fe, 0, sizeof(fe));
    fe.type = CMD_FOR_END;
    fe.valid = 1;
    char fe_text[MAX_LINE_LEN];
    snprintf(fe_text, sizeof(fe_text), "%s}", indent);

    /* Empty-body branch: `{` or end-of-input. */
    if (*body_start == '{' || *body_start == '\0') {
        /* Header replace branch (cursor on existing CMD_FOR_BEGIN
         * in non-insert mode): REPLACE_ONE. */
        if (!ctx->insert_mode &&
            ctx->edit_line < ctx->document_count &&
            ctx->document_cmds[ctx->edit_line].type == CMD_FOR_BEGIN) {
            out->change.kind  = REPL_COMPILED_REPLACE_ONE;
            out->change.pos   = ctx->edit_line;
            out->change.count = 1;
            out->change.cmds[0] = fb;
            snprintf(out->change.text[0], sizeof(out->change.text[0]),
                     "%s", fb_text);

            out->effects.cursor_target      = ctx->edit_line + 1;
            out->effects.insert_mode_target = 1;
            out->effects.clear_input        = 1;
            out->effects.clear_autocomplete = 1;

            snprintf(out->change.commit_message, sizeof(out->change.commit_message),
                     "for-loop header updated");
            return REPL_COMPILE_OK;
        }

        out->change.kind  = REPL_COMPILED_INSERT_MANY;
        out->change.pos   = pos;
        out->change.count = 2;
        out->change.cmds[0] = fb;
        out->change.cmds[1] = fe;
        snprintf(out->change.text[0], sizeof(out->change.text[0]),
                 "%s", fb_text);
        snprintf(out->change.text[1], sizeof(out->change.text[1]),
                 "%s", fe_text);

        out->effects.cursor_target      = pos + 1;
        out->effects.insert_mode_target = 1;
        out->effects.clear_input        = 1;

        snprintf(out->change.commit_message, sizeof(out->change.commit_message),
                 "for-loop: type body lines, press Esc when done");
        return REPL_COMPILE_OK;
    }

    /* One-liner body branch: INSERT_MANY count=3 (begin + body + end). */
    char body[MAX_LINE_LEN];
    int blen;
    strncpy(body, body_start, MAX_LINE_LEN - 1);
    body[MAX_LINE_LEN - 1] = '\0';
    blen = (int)strlen(body);
    while (blen > 0 &&
           (body[blen - 1] == ';' || isspace((unsigned char)body[blen - 1])))
        body[--blen] = '\0';
    if (blen == 0)
        return editor_compile_error(err, err_size, "for-loop needs a body");

    /* Loop scope: var_name + start, plus the visible vars. */
    ExprVar dv[MAX_EXPR_VARS];
    int dvn = 0;
    repl_copy_string_fits(dv[dvn].name, sizeof(dv[dvn].name), var_name);
    dv[dvn].value = start;
    dvn++;
    for (int var_idx = 0; var_idx < visible_nv && dvn < MAX_EXPR_VARS; var_idx++)
        dv[dvn++] = visible_vars[var_idx];

    /* Feed the parser an err buffer so its specific diagnostic
     * propagates to the caller rather than being lost behind a generic
     * "Invalid for-loop body command" string. */
    char body_err[REPL_STATUS_TEXT_MAX];
    body_err[0] = '\0';
    ReplParseContext parse_ctx = {
        .source_line_idx = pos,
        .vars            = dv,
        .num_vars        = dvn,
        .strict_refs     = 1,
        .err_buf         = body_err,
        .err_sz          = (int)sizeof(body_err),
    };
    ReplParsedLine body_pl;
    if (!repl_parser_parse_command_ctx(body, &body_pl, &parse_ctx)) {
        if (body_err[0])
            return editor_compile_error(err, err_size,
                                        "for-loop body: %s", body_err);
        else
            return editor_compile_error(err, err_size,
                                        "Invalid for-loop body command");
    }
    GLCmd body_cmd = body_pl.cmd;

    char body_text[MAX_LINE_LEN];
    {
        char bind[32];
        int bi = ind + 2;
        if (bi > (int)sizeof(bind) - 1) bi = (int)sizeof(bind) - 1;
        memset(bind, ' ', (size_t)bi);
        bind[bi] = '\0';
        snprintf(body_text, sizeof(body_text), "%s%s;", bind, body);
    }

    out->change.kind  = REPL_COMPILED_INSERT_MANY;
    out->change.pos   = pos;
    out->change.count = 3;
    out->change.cmds[0] = fb;
    out->change.cmds[1] = body_cmd;
    out->change.cmds[2] = fe;
    snprintf(out->change.text[0], sizeof(out->change.text[0]),
             "%s", fb_text);
    snprintf(out->change.text[1], sizeof(out->change.text[1]),
             "%s", body_text);
    snprintf(out->change.text[2], sizeof(out->change.text[2]),
             "%s", fe_text);

    out->effects.cursor_target         = pos + 3;
    out->effects.insert_mode_target    = 0;  /* exit insert mode */
    out->effects.clear_input           = 1;
    out->effects.clear_pending_newline = 1;

    snprintf(out->change.commit_message, sizeof(out->change.commit_message),
             "for-loop: %s from %g to %g", var_name, start, end);
    return REPL_COMPILE_OK;
}

/* ===========================================================================
 * Commit dispatcher chain.
 *
 * The M/V/C + compiler + router contract has the editor attempt a
 * commit and the REPL act as a pure parser/validator. These
 * `editor_try_commit_*` dispatchers decide which compile entry to
 * call, apply the resulting plan, and own the editor post-effect /
 * status fan-out — that is editor-side orchestration, not REPL
 * grammar. Parse failures return data rather than calling set_status
 * from inside the REPL path; callers surface the diagnostic.
 * ===========================================================================
 */

/* Editor-orchestration scratch: the func-decl-resume delta is
 * tracked between a CMD_FUNC_DEF commit (which publishes the delta
 * via editor_commit_func_decl_resume_set) and the matching close-
 * brace / Enter-out-of-func that consumes it. The state is
 * file-private now that all readers/writers live in this TU. */
static int g_func_decl_resume_delta = 0;

int editor_commit_func_decl_resume_peek(void) {
    return g_func_decl_resume_delta;
}

static int editor_commit_func_decl_resume_take(void) {
    int delta = g_func_decl_resume_delta;
    g_func_decl_resume_delta = 0;
    return delta;
}

static void editor_commit_func_decl_resume_set(int delta) {
    g_func_decl_resume_delta = delta;
}

/* Side effect: consumes (zeroes) g_func_decl_resume_delta — it is a
 * one-shot, applied to at most one exit-target resolution. */
int editor_commit_resolve_insert_exit_target(int target) {
    if (!editor_insert_mode() ||
        g_func_decl_resume_delta <= 0 ||
        editor_state_edit_line() < 0 ||
        editor_state_edit_line() >= repl_state_document_count() ||
        repl_state_document_cmds_mut()[editor_state_edit_line()].type != CMD_FUNC_END)
        return target;

    if (target == editor_state_edit_line()) {
        target += g_func_decl_resume_delta;
        if (target > repl_state_document_count())
            target = repl_state_document_count();
    }

    g_func_decl_resume_delta = 0;
    return target;
}

void editor_commit_reset_transients(void) {
    g_func_decl_resume_delta = 0;
}

/* --- Float-decl commit ---
 *
 * UNDO CONTRACT: this handler (like every editor_try_commit_*) goes
 * through editor_commit_apply_plan, which does NOT capture undo. The
 * caller must push a snapshot first if it wants the commit to be
 * undoable. Current dispatch sites in src/editor/input.c that push:
 *  - ; key (handle_semicolon_commit_key_route, unconditional)
 *  - Enter / navigation (commit_current_input, gated on input_len > 0
 *    or insert_mode — empty input never triggers a mutating handler)
 *  - editor_feed_line callers bracket the whole load with one push
 *    (currently only editor_clipboard_paste_current uses this path)
 * Audit #39 walked these and confirmed no missing-undo bugs. */

int editor_try_commit_float_decl(void) {
    ReplCompileContext ctx = editor_compile_context_live();
    EditorCommitPlan plan;
    editor_commit_plan_init(&plan);
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult result = repl_compile_float_decl(
        editor_state_input().input, &ctx, &plan.change, err, sizeof(err));
    if (result == REPL_COMPILE_OK && plan.change.kind == REPL_COMPILED_NO_CHANGE)
        return 0;
    if (result != REPL_COMPILE_OK) {
        repl_set_status_error(err);
        return 1;
    }

    plan.effects.clear_input = 1;
    if (plan.change.kind == REPL_COMPILED_REPLACE_ONE) {
        plan.effects.cursor_target = ctx.edit_line + 1;
        plan.effects.load_line_after_apply = 1;
    } else if (plan.change.kind == REPL_COMPILED_INSERT_ONE) {
        /* INSERT_ONE has adjust_edit_line=1 — the REPL apply auto-bumps
         * edit_line when pos <= edit_line. The live post-effect
         * conditionally reloaded the input from the new edit_line when
         * not in insert mode and the cursor still pointed at a real
         * line. Reproduce that here. */
        int new_edit_line = ctx.edit_line +
            (plan.change.pos <= ctx.edit_line ? 1 : 0);
        int doc_count_after = ctx.document_count + 1;
        if (!ctx.insert_mode && new_edit_line < doc_count_after)
            plan.effects.load_line_after_apply = 1;
    }


    if (!editor_commit_apply_plan(&plan)) {
        repl_set_status_error("Command buffer full!");
        return 1;
    }
    repl_mark_source_dirty();
    return 1;
}

/* --- Var-assign commit --- */

int editor_try_commit_assign_variable(void) {
    ReplCompileContext ctx = editor_compile_context_live();
    EditorCommitPlan plan;
    editor_commit_plan_init(&plan);
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult result = repl_compile_var_assign(
        editor_state_input().input, &ctx, &plan.change, err, sizeof(err));
    if (result == REPL_COMPILE_OK && plan.change.kind == REPL_COMPILED_NO_CHANGE)
        return 0;
    if (result != REPL_COMPILE_OK) {
        repl_set_status_error(err);
        return 1;
    }

    plan.effects.clear_input = 1;
    if (plan.change.kind == REPL_COMPILED_REPLACE_ONE) {
        plan.effects.cursor_target = ctx.edit_line + 1;
        plan.effects.load_line_after_apply = 1;
    } else if (plan.change.kind == REPL_COMPILED_INSERT_ONE) {
        if (ctx.insert_mode) {
            plan.effects.cursor_target = ctx.edit_line + 1;
        } else {
            /* Append-at-end branch (insert_idx == ctx.document_count). */
            plan.effects.cursor_target = ctx.document_count + 1;
        }
    }


    if (!editor_commit_apply_plan(&plan)) {
        repl_set_status_error("Command buffer full!");
        return 1;
    }
    repl_mark_source_dirty();
    return 1;
}

/* --- Structured-block commits: each delegates to its editor_compile_*
 * partner and applies the resulting plan via editor_commit_apply_plan.
 * The four wrappers differ only in which editor_compile_* they call, so
 * the shared body lives in editor_try_commit_block(). --- */

/* Prototyped (not old-style) function-pointer typedef per the C99
 * portability convention in CLAUDE.md. Matches every editor_compile_*
 * structured-block signature in commit.h. */
typedef ReplCompileResult (*EditorBlockCompileFn)(const char *input,
                                                  const ReplCompileContext *ctx,
                                                  EditorCommitPlan *out,
                                                  char *err, int err_size);

static int editor_try_commit_block(EditorBlockCompileFn compile) {
    ReplCompileContext ctx = editor_compile_context_live();
    EditorCommitPlan plan;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = compile(editor_state_input().input,
                                  &ctx, &plan,
                                  err, sizeof(err));
    if (r == REPL_COMPILE_OK &&
        plan.change.kind == REPL_COMPILED_NO_CHANGE &&
        !plan.change.commit_message[0])
        return 0;
    if (r != REPL_COMPILE_OK) {
        repl_set_status_error(err);
        return 1;
    }
    if (!editor_commit_apply_plan(&plan)) {
        repl_set_status_error("Command buffer full!");
        return 1;
    }
    repl_mark_source_dirty();
    return 1;
}

int editor_try_commit_for_loop(void) {
    return editor_try_commit_block(editor_compile_for_loop);
}

int editor_try_commit_func_def(void) {
    return editor_try_commit_block(editor_compile_func_def);
}

int editor_try_commit_if_block(void) {
    return editor_try_commit_block(editor_compile_if_block);
}

int editor_try_commit_close_brace(void) {
    return editor_try_commit_block(editor_compile_close_brace);
}

/* --- Higher-order dispatchers --- */

/* Block-structural commit handlers: `}`, `for(`, `funcN`, `if(`. */
int editor_try_commit_block_structs(void) {
    if (editor_try_commit_close_brace()) return 1;
    if (editor_try_commit_for_loop())    return 1;
    if (editor_try_commit_func_def())    return 1;
    if (editor_try_commit_if_block())    return 1;
    return 0;
}

/* Statement-level commit handlers. float decl MUST precede assign so
 * that `float x` is not misread as an assignment to "float". */
int editor_try_commit_var_statements(void) {
    if (editor_try_commit_float_decl())  return 1;
    if (editor_try_commit_assign_variable())    return 1;
    return 0;
}

int editor_try_commit_any(void) {
    if (editor_try_commit_var_statements()) return 1;
    if (editor_try_commit_block_structs())  return 1;
    return 0;
}

static void enter_insert_mode_after_var_commit(void) {
    editor_insert_mode_set(1);
    editor_input_clear();
    editor_completion_clear();
}

/* Overwrite-mode Enter variant: on successful var-statement commit,
 * enter insert mode and clear the input. Preserve the specific
 * decl/assign status published by the inner commit helper; this
 * wrapper only adds the insert-mode/input-clear post-effect. */
int editor_try_commit_var_statements_then_insert(void) {
    if (editor_try_commit_float_decl()) {
        enter_insert_mode_after_var_commit();
        return 1;
    }
    if (editor_try_commit_assign_variable()) {
        enter_insert_mode_after_var_commit();
        return 1;
    }
    return 0;
}

int editor_commit_apply_swatch_change(int edit_line, int direction) {
    EditorInputView in = editor_state_input();
    ReplNumericArgAtCursor d;
    float step, new_value;
    char buf[32];
    char new_line[MAX_LINE_LEN];
    int n;
    char parse_err[REPL_STATUS_TEXT_MAX] = "";
    ReplParsedLine pl;
    ReplCompiledChange change;
    int text_len;

    d = repl_eval_numeric_arg_at_cursor(in.input, in.cursor_pos);
    if (!d.found) return 0;

    step = repl_eval_swatch_step(d.value);
    new_value = d.value + (direction > 0 ? step : -step);
    repl_eval_format_swatch_number(new_value, buf, sizeof buf);

    n = snprintf(new_line, sizeof new_line, "%.*s%s%s",
                 d.arg_start, in.input, buf, in.input + d.arg_end);
    if (n < 0 || n >= (int)sizeof new_line) return 0;

    {
        ReplParseContext parse_ctx = {
            .source_line_idx = edit_line,
            .err_buf = parse_err,
            .err_sz = (int)sizeof parse_err,
        };
        if (!repl_parser_parse_command_ctx(new_line, &pl, &parse_ctx)) {
            if (parse_err[0]) repl_set_status(parse_err);
            return 0;
        }
    }
    if (pl.cmd.type == CMD_COMMENT) return 0;

    repl_compiled_change_init(&change);
    change.kind = REPL_COMPILED_REPLACE_ONE;
    change.pos = edit_line;
    change.count = 1;
    change.cmds[0] = pl.cmd;
    text_len = (int)strlen(pl.text);
    if (text_len >= MAX_LINE_LEN) text_len = MAX_LINE_LEN - 1;
    memcpy(change.text[0], pl.text, (size_t)text_len);
    change.text[0][text_len] = '\0';

    if (editor_commit_apply_external_change(&change, 1, 0)) {
        editor_load_line_to_input(edit_line);
        {
            EditorInputView reloaded = editor_state_input();
            int pos = d.arg_start < reloaded.input_len
                          ? d.arg_start : reloaded.input_len;
            editor_cursor_pos_set(pos);
        }
        editor_completion_update();
        editor_request_redraw();
        return 1;
    }
    return 0;
}
