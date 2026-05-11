/*
 * repl_load.c -- Apply orchestration for non-editor source loading.
 *
 * Drives compile (via repl_compile_*) → predef apply → editor-buffer
 * apply → command-store apply, mirroring the REPL halves of
 * editor_commit_apply_plan but without editor effects (cursor target,
 * insert-mode toggle, input buffer writes). Step 5b of the decouple
 * plan.
 *
 * Lives in its own module so repl_compile.c can stay a pure validator
 * (per the file's contract: never writes the editor buffer / command
 * store / predef-var registrations).
 */
#include "repl_load.h"

#include "repl_apply.h"
#include "repl_command_store.h"
#include "repl_compile.h"
#include "repl_core_internal.h"  /* collect_visible_vars, repl_format_fits, etc. */
#include "repl_eval.h"
#include "repl_parser.h"
#include "repl_state_owners.h"
#include "source_document.h"     /* source_document_apply_change, _insert_line */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Try one of the structured-block validators. Returns 1 if the line
 * was consumed (kind != NO_CHANGE) or if the validator returned an
 * error. Returns 0 if the line wasn't this block's shape (caller
 * should fall through to the next handler). */
static int load_try_block(ReplCompileResult (*compile)(const char *,
                                                       const ReplCompileContext *,
                                                       ReplCompiledChange *,
                                                       char *, int),
                          const char *line,
                          const ReplCompileContext *ctx,
                          ReplCompiledChange *out,
                          char *err, int err_size,
                          int *failed_out) {
    ReplCompileResult r = compile(line, ctx, out, err, err_size);
    if (r == REPL_COMPILE_ERROR) {
        if (failed_out) *failed_out = 1;
        return 1;  /* claimed but failed */
    }
    return out->kind != REPL_COMPILED_NO_CHANGE;
}

int repl_load_apply_line(const char *line, char *err, int err_size) {
    if (!line) return 0;
    if (err && err_size > 0) err[0] = '\0';

    /* Don't pre-skip empty/comment lines: feed_line preserves them as
     * CMD_EMPTY / CMD_COMMENT so flatten can pass them through and
     * roundtrip preserves the line count. The parser handles them. */

    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    repl_compiled_change_init(&change);
    int failed = 0;

    /* (1) float decl + var assign via dispatcher. */
    {
        ReplCompileResult r = repl_compile_dispatch(line, &ctx, &change,
                                                    err, err_size);
        if (r == REPL_COMPILE_ERROR) return 0;
    }

    /* (2) structured-block validators in canonical try_commit order:
     * close_brace → for_loop → func_def → if_block. */
    if (change.kind == REPL_COMPILED_NO_CHANGE &&
        load_try_block(repl_compile_close_brace, line, &ctx, &change,
                       err, err_size, &failed)) {
        if (failed) return 0;
    }
    if (change.kind == REPL_COMPILED_NO_CHANGE &&
        load_try_block(repl_compile_for_loop, line, &ctx, &change,
                       err, err_size, &failed)) {
        if (failed) return 0;
    }
    if (change.kind == REPL_COMPILED_NO_CHANGE &&
        load_try_block(repl_compile_func_def, line, &ctx, &change,
                       err, err_size, &failed)) {
        if (failed) return 0;
    }
    if (change.kind == REPL_COMPILED_NO_CHANGE &&
        load_try_block(repl_compile_if_block, line, &ctx, &change,
                       err, err_size, &failed)) {
        if (failed) return 0;
    }

    /* (3) Apply the structured/decl/assign change if any was produced.
     *
     * Force adjust_edit_line=1 so insert ops auto-advance edit_line.
     * The compile validators leave it 0 because the editor wrapper
     * controls cursor target via EditorCommitPlan; for the lean
     * loader's append-at-end semantics, edit_line must auto-advance
     * line-by-line so the next call sees insert_idx = document_count.
     *
     * Preflight FIRST so a capacity overflow doesn't leave half-applied
     * side effects (registered predef vars, modified scratch arrays,
     * editor-buffer mutations) without a matching source command.
     * Mirrors editor_commit_apply_plan's preflight gate. [P1]
     * regression. */
    if (change.kind != REPL_COMPILED_NO_CHANGE) {
        change.adjust_edit_line = 1;

        if (!repl_apply_can_apply_compiled_change(&change)) {
            /* Roll back any alias the func_def pre-step registered
             * speculatively. Predef/scratch/editor-buffer ops haven't
             * run yet (preflight gates them), so nothing else to
             * undo. */
            repl_compiled_change_rollback_alias(&change);
            if (err && err_size > 0 && err[0] == '\0')
                snprintf(err, (size_t)err_size,
                         "command store at capacity (max %d)",
                         MAX_COMMANDS);
            return 0;
        }

        /* Past preflight: apply source text FIRST so a host that can
         * fail the write (a non-editor backend or test fixture) doesn't
         * leave predef-vars registered + scratch arrays mutated with no
         * matching text on the document. predef/scratch/command-store
         * mutations follow only once the text apply has committed. */
        SourceTextChange text_change;
        repl_compiled_change_to_text_change(&change, &text_change);
        if (!source_document_apply_change(&text_change)) {
            repl_compiled_change_rollback_alias(&change);
            if (err && err_size > 0 && err[0] == '\0')
                snprintf(err, (size_t)err_size,
                         "source document apply failed");
            return 0;
        }
        repl_apply_predef_ops(&change);
        repl_apply_scratch_ops(&change);
        return repl_apply_compiled_change(&change) ? 1 : 0;
    }

    /* (4) Plain GL command path — parse + insert via command store.
     * Mirrors feed_line's plain-command tail but without writing the
     * editor input buffer. */
    int insert_idx = ctx.edit_line < ctx.document_count
                         ? ctx.edit_line : ctx.document_count;

    ExprVar vis_vars[MAX_EXPR_VARS];
    int vis_total = 0;
    int num_vis_vars = collect_visible_vars(insert_idx, vis_vars,
                                            MAX_EXPR_VARS, &vis_total);

    GLCmd cmd;
    char cmd_text[MAX_LINE_LEN] = "";
    memset(&cmd, 0, sizeof(cmd));
    int parsed;
    if (num_vis_vars > 0) {
        parsed = repl_parse_and_normalize_strict(line, insert_idx,
                                                 vis_vars, num_vis_vars,
                                                 input_has_any_visible_vars(line, vis_vars, num_vis_vars),
                                                 &cmd, cmd_text, sizeof(cmd_text));
    } else {
        parsed = repl_parse_and_normalize_strict(line, insert_idx,
                                                 NULL, 0,
                                                 repl_eval_input_has_predef_vars(line),
                                                 &cmd, cmd_text, sizeof(cmd_text));
    }
    if (!parsed) {
        if (err && err_size > 0 && err[0] == '\0')
            snprintf(err, (size_t)err_size, "could not parse line: %s", line);
        return 0;
    }

    /* Source text first; command store second. If the cmd-store insert
     * fails (capacity), roll the text back so the document stays in
     * lockstep with the GLCmd array. */
    if (!source_document_insert_line(insert_idx, cmd_text)) {
        if (err && err_size > 0 && err[0] == '\0')
            snprintf(err, (size_t)err_size,
                     "source document insert failed");
        return 0;
    }
    ReplCommandStore store = repl_command_store_live();
    if (!repl_command_store_insert_one(&store, insert_idx, &cmd,
                                       REPL_COMMAND_STORE_ADJUST_EDIT_LINE)) {
        SourceTextChange rollback = {
            .kind         = SOURCE_TEXT_DELETE_RANGE,
            .pos          = insert_idx,
            .count        = 1,
            .delete_pos   = -1,
            .delete_count = 0,
        };
        source_document_apply_change(&rollback);
        return 0;
    }
    return 1;
}
