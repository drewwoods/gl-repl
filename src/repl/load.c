/*
 * src/repl/load.c -- Apply orchestration for non-editor source loading.
 *
 * Drives compile (via repl_compile_*) → predef apply → editor-buffer
 * apply → command-store apply, mirroring the REPL halves of
 * editor_commit_apply_plan but without editor effects (cursor target,
 * insert-mode toggle, input buffer writes), implemented as step 5b of
 * the decouple plan.
 *
 * Lives in its own module so src/repl/compile.c can stay a pure validator
 * (per the file's contract: never writes the editor buffer / command
 * store / predef-var registrations).
 */
#include "repl/load.h"

#include "repl/apply.h"
#include "repl/command_store.h"
#include "repl/compile.h"
#include "repl/core.h"           /* repl_dispatch_edit_line_get / _set */
#include "repl/core_internal.h"  /* collect_visible_vars, repl_format_fits, etc. */
#include "repl/eval.h"
#include "source_document.h"     /* source_document_apply_change, _insert_line */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int repl_load_apply_line(const char *line, char *err, int err_size,
                         int *edit_line_inout) {
    if (!line) return 0;
    if (err && err_size > 0) err[0] = '\0';

    /* Don't pre-skip empty/comment lines: editor_feed_line preserves them as
     * CMD_EMPTY / CMD_COMMENT source rows so editor/export round-trips
     * preserve line count. Flatten drops both from the executable stream. */

    /* Caller-owned cursor. edit_line_inout supplies
     * the insertion position; the per-line apply advances it as
     * the document grows. A NULL pointer falls back to operating
     * on the ambient host cursor through repl_dispatch_edit_line_get
     * / _set: the loader reads the
     * current value at entry, advances it across the per-line
     * apply, and writes the post-load value back on success. This
     * preserves the pre-migration behavior where ad-hoc loader
     * callers (the editor's load-file path, the tutorial runner,
     * mid-test commit chains) saw the cursor advance as a side
     * effect of repl_load_apply_line (implemented in phase 3.6.5 and
     * phase 4 of plans/done/edit-line-ownership.md). */
    int local_edit_line;
    int wrote_local = 0;
    if (edit_line_inout == NULL) {
        local_edit_line = repl_dispatch_edit_line_get();
        edit_line_inout = &local_edit_line;
        wrote_local = 1;
    }
    ReplCompileContext ctx = repl_compile_context_from_live(*edit_line_inout);
    ReplCompiledChange change;
    repl_compiled_change_init(&change);

    /* All six block/decl compile validators run in canonical order
     * via repl_compile_dispatch (#16): float_decl → var_assign →
     * close_brace → for_loop → func_def → if_block. The first
     * matching grammar wins. */
    {
        ReplCompileResult r = repl_compile_dispatch(line, &ctx, &change,
                                                    err, err_size);
        if (r == REPL_COMPILE_ERROR) return 0;
    }

    /* Apply the structured/decl/assign change if any was produced.
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
        /* Apply mutates *edit_line_inout in place via the store.
         * The cursor advance is what
         * makes the next call see insert_idx = document_count for
         * the append-at-end semantics (implemented in phase 1 +
         * phase 3.6.5). */
        int ok = repl_apply_compiled_change(&change, edit_line_inout);
        if (ok && wrote_local)
            repl_dispatch_edit_line_set(*edit_line_inout);
        return ok ? 1 : 0;
    }

    /* (4) Plain GL command path — parse + insert via command store.
     * Mirrors editor_feed_line's plain-command tail but without writing the
     * editor input buffer. */
    int insert_idx = ctx.edit_line < ctx.document_count
                         ? ctx.edit_line : ctx.document_count;

    ExprVar vis_vars[MAX_EXPR_VARS];
    int num_vis_vars = collect_visible_vars(insert_idx, vis_vars,
                                            MAX_EXPR_VARS, NULL);

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
            snprintf(err, (size_t)err_size, "parse error");
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
    /* Plain-command tail: thread the caller's cursor pointer
     * directly so the post-insert cursor lands on
     * `*edit_line_inout`. Mirrors the structured-change path. */
    ReplStoreMutOpts opts = {
        .flags        = REPL_COMMAND_STORE_ADJUST_EDIT_LINE,
        .cursor_inout = edit_line_inout,
    };
    if (!repl_command_store_insert_one(&store, insert_idx, &cmd, &opts)) {
        SourceTextChange rollback = {
            .kind         = SOURCE_TEXT_DELETE_RANGE,
            .pos          = insert_idx,
            .count        = 1,
            .delete_pos   = -1,
            .delete_count = 0,
        };
        source_document_apply_change(&rollback);
        /* Surface the capacity failure through `err` so the importer
         * can include the reason in its "could not parse line" warning
         * (#8). The structured-change path above (REPL_COMPILED_*)
         * already routes through this same error; the plain-command
         * tail was missing it. */
        if (err && err_size > 0 && err[0] == '\0')
            snprintf(err, (size_t)err_size,
                     "command store at capacity (max %d)",
                     MAX_COMMANDS);
        return 0;
    }
    if (wrote_local)
        repl_dispatch_edit_line_set(*edit_line_inout);
    return 1;
}
