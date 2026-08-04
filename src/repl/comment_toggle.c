/*
 * src/repl/comment_toggle.c - Range resolution + execution for Ctrl+/.
 *
 * See comment_toggle.h for why the two directions do not share a shape.
 * The split here: the plan half is pure and reads only the compile
 * context; the apply half drives repl/compile.c's two direction-specific
 * validators into the loader's apply transaction, bracketing the
 * multi-row uncomment with a SceneSnapshot so a rejected row leaves
 * nothing behind.
 */
#include "repl/comment_toggle.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "repl/apply.h"          /* repl_apply_can_apply_compiled_change */
#include "repl/command.h"
#include "repl/compile.h"
#include "repl/eval.h"          /* repl_line_trailing_comment */
#include "repl/load.h"
#include "repl/scene_snapshot.h"
#include "repl/state_notify.h"
#include "repl/state_owners.h"
#include "source_document.h"

/* ---- plan ------------------------------------------------------- */

static int toggle_set_err(char *err, int err_size, const char *fmt, ...) {
    va_list ap;
    if (err && err_size > 0) {
        va_start(ap, fmt);
        vsnprintf(err, (size_t)err_size, fmt, ap);
        va_end(ap);
    }
    return 0;
}

/* Is the row at `idx` a comment this toggle put there (or could have)? */
static int row_is_strippable_comment(const ReplCompileContext *ctx,
                                     int idx, const char *prefix) {
    char stripped[MAX_LINE_LEN];

    if (idx < 0 || idx >= ctx->document_count)
        return 0;
    if (ctx->document_cmds[idx].type != CMD_COMMENT)
        return 0;
    return repl_compile_comment_prefix_strip(source_text_line(ctx->text, idx),
                                             prefix, stripped,
                                             (int)sizeof(stripped));
}

/* Brace shape of a commented row, read back out of its text. The
 * command model says CMD_COMMENT and nothing more, so the only surviving
 * record of the structure is the text the prefix was prepended to.
 * `} else {` legitimately both closes and opens.
 *
 * Only the *code* half of the stripped line counts. Braces past a `//`
 * are prose, not structure, and a row that is still a comment after one
 * strip (`// // note {`) uncomments back to a comment - it can never
 * produce a block head or end, so it has no shape at all. Reading them
 * as structure made an ordinary `// note {` refuse its own reverse
 * toggle. repl_line_trailing_comment skips string literals, so
 * `label("a {")` is code the whole way. */
static void row_brace_shape(const ReplCompileContext *ctx, int idx,
                            const char *prefix, int *closes, int *opens) {
    char stripped[MAX_LINE_LEN];
    const char *comment;
    const char *p;
    int len;

    *closes = 0;
    *opens = 0;
    if (!repl_compile_comment_prefix_strip(source_text_line(ctx->text, idx),
                                           prefix, stripped,
                                           (int)sizeof(stripped)))
        return;

    comment = repl_line_trailing_comment(stripped);
    len = comment ? (int)(comment - stripped) : (int)strlen(stripped);

    p = stripped;
    while (len > 0 && (*p == ' ' || *p == '\t')) { p++; len--; }
    if (len > 0 && *p == '}')
        *closes = 1;

    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
        len--;
    if (len > 0 && p[len - 1] == '{')
        *opens = 1;
}

/* Expand `cursor_row` to the commented block it belongs to by matching
 * braces across the run of commented rows around it. A row that neither
 * opens nor closes stays a range of one, which is what makes Ctrl+/ on a
 * lone commented command still a one-row operation. */
static int plan_commented_block(const ReplCompileContext *ctx, int cursor_row,
                                const char *prefix, int *out_first,
                                int *out_last, char *err, int err_size) {
    int run_lo = cursor_row;
    int run_hi = cursor_row;
    int closes;
    int opens;
    int head;
    int tail = -1;
    int depth;

    while (run_lo > 0 && row_is_strippable_comment(ctx, run_lo - 1, prefix))
        run_lo--;
    while (run_hi + 1 < ctx->document_count &&
           row_is_strippable_comment(ctx, run_hi + 1, prefix))
        run_hi++;

    row_brace_shape(ctx, cursor_row, prefix, &closes, &opens);

    head = cursor_row;
    if (closes) {
        /* Walk back for the row that opened this one. A `} else {` on
         * the way closes and opens in the same step, so it nets out as
         * the separator it is rather than terminating the scan. */
        depth = 1;
        head = -1;
        for (int j = cursor_row - 1; j >= run_lo; j--) {
            int c;
            int o;
            row_brace_shape(ctx, j, prefix, &c, &o);
            if (c) depth++;
            if (o && --depth == 0) { head = j; break; }
        }
        if (head < 0)
            return toggle_set_err(err, err_size,
                                  "Unmatched } in the commented block");
    }

    depth = 0;
    for (int j = head; j <= run_hi; j++) {
        int c;
        int o;
        row_brace_shape(ctx, j, prefix, &c, &o);
        if (c) depth--;
        if (o) depth++;
        if (depth == 0) { tail = j; break; }
        if (depth < 0)
            break;
    }
    if (tail < 0)
        return toggle_set_err(err, err_size,
                              "Unmatched { in the commented block");

    *out_first = head;
    *out_last = tail;
    return 1;
}

/* The uncomment counterpart of compile's balanced-range gate: same
 * question, asked of the stripped text because the command kinds are all
 * CMD_COMMENT by now. */
static int plan_uncomment_range_is_balanced(const ReplCompileContext *ctx,
                                            int first, int last,
                                            const char *prefix,
                                            char *err, int err_size) {
    int depth = 0;

    for (int i = first; i <= last; i++) {
        int closes;
        int opens;
        row_brace_shape(ctx, i, prefix, &closes, &opens);
        if (closes && --depth < 0)
            return toggle_set_err(err, err_size,
                                  "Line %d closes a block that starts "
                                  "outside the selection", i + 1);
        if (opens)
            depth++;
    }
    if (depth != 0)
        return toggle_set_err(err, err_size,
                              "Selection opens a block it does not close");
    return 1;
}

int repl_comment_toggle_plan(const ReplCompileContext *ctx,
                             int cursor_row, int sel_first, int sel_last,
                             const char *prefix,
                             ReplCommentTogglePlan *out,
                             char *err, int err_size) {
    int first;
    int last;
    int uncomment;

    if (err && err_size > 0)
        err[0] = '\0';
    if (!ctx || !out || !prefix || !prefix[0])
        return 0;

    memset(out, 0, sizeof(*out));

    if (sel_first >= 0 && sel_last >= 0) {
        first = sel_first < sel_last ? sel_first : sel_last;
        last  = sel_first < sel_last ? sel_last : sel_first;
        if (first < 0) first = 0;
        if (last >= ctx->document_count) last = ctx->document_count - 1;
        if (last < first)
            return toggle_set_err(err, err_size, "Nothing selected");

        /* Direction is a property of the whole range, never of each row:
         * toggling row by row would make a second press restore only the
         * rows that had been code, which is not an undo of anything. */
        uncomment = 1;
        for (int i = first; i <= last && uncomment; i++) {
            if (!row_is_strippable_comment(ctx, i, prefix))
                uncomment = 0;
        }
    } else {
        if (cursor_row < 0 || cursor_row >= ctx->document_count)
            return toggle_set_err(err, err_size, "No line to toggle");

        uncomment = row_is_strippable_comment(ctx, cursor_row, prefix);
        if (uncomment) {
            if (!plan_commented_block(ctx, cursor_row, prefix,
                                      &first, &last, err, err_size))
                return 0;
        } else if (!repl_compile_block_extent_at(ctx, cursor_row,
                                                 &first, &last)) {
            first = cursor_row;
            last = cursor_row;
        }
    }

    if (uncomment &&
        !plan_uncomment_range_is_balanced(ctx, first, last, prefix,
                                          err, err_size))
        return 0;

    if (last - first + 1 > MAX_COMMIT_CMDS)
        return toggle_set_err(err, err_size,
                              "Block too large to toggle (max %d lines)",
                              MAX_COMMIT_CMDS);

    out->first = first;
    out->last = last;
    out->uncomment = uncomment;
    return 1;
}

/* ---- apply ------------------------------------------------------ */

static void result_init(ReplCommentToggleResult *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->failed_row = -1;
}

static int result_fail(ReplCommentToggleResult *out, int row, const char *err) {
    if (out) {
        out->failed_row = row;
        snprintf(out->err, sizeof(out->err), "%s",
                 err && err[0] ? err : "toggle rejected");
    }
    return 0;
}

/* Compile the comment direction. `out_change` receives the one change
 * that describes the whole range; every way a comment can be refused is
 * a property of the range (unbalanced, over capacity, a declaration
 * still read from outside), so no row is named - those diagnostics
 * already spell out the line they mean. */
static int compile_comment(const ReplCommentTogglePlan *plan,
                           const char *prefix,
                           ReplCompiledChange *out_change,
                           ReplCommentToggleResult *out) {
    ReplCompileContext ctx = repl_compile_context_from_live(plan->first);
    char err[REPL_DIAG_TEXT_MAX] = "";

    if (repl_compile_comment_range(plan->first, plan->last, prefix, &ctx,
                                   out_change, err, sizeof(err))
            != REPL_COMPILE_OK)
        return result_fail(out, -1, err);
    if (out_change->kind == REPL_COMPILED_NO_CHANGE)
        return result_fail(out, -1, "Nothing to comment");
    if (!repl_apply_can_apply_compiled_change(out_change))
        return result_fail(out, -1, "command store at capacity");
    return 1;
}

/* The comment direction needs no rehearsal: the compile is pure and the
 * store preflight answers the only remaining question. */
static int rehearse_comment(const ReplCommentTogglePlan *plan,
                            const char *prefix,
                            ReplCommentToggleResult *out) {
    ReplCompiledChange change;
    return compile_comment(plan, prefix, &change, out);
}

static int apply_comment(const ReplCommentTogglePlan *plan, const char *prefix,
                         ReplCommentToggleResult *out) {
    ReplCompiledChange change;
    ReplLoadTransactionResult tx;

    if (!compile_comment(plan, prefix, &change, out))
        return 0;

    /* One compiled change, so the transaction's own preflight is the
     * all-or-nothing gate - no snapshot needed on this side. */
    if (!repl_load_apply_compiled_change_transaction(&change, plan->first, &tx))
        return result_fail(out, -1, "command store at capacity");

    if (out) {
        out->touched_declarations = change.predef_op_count > 0;
        snprintf(out->message, sizeof(out->message), "%s",
                 change.commit_message);
    }
    return 1;
}

/* `rehearse` uncomments the range for real - the only way to find out
 * whether it can be, since each row needs the rows above it already
 * restored - and then puts the snapshot back, so the caller learns the
 * answer without the document keeping it. */
static int apply_uncomment(const ReplCommentTogglePlan *plan,
                           const char *prefix,
                           int rehearse,
                           ReplCommentToggleResult *out) {
    SceneSnapshot *before;
    int n = plan->last - plan->first + 1;
    int touched_declarations = 0;
    int failed_row = -1;
    char err[REPL_DIAG_TEXT_MAX] = "";

    /* SceneSnapshot is far too large for the stack (a full command array
     * plus a full text buffer). */
    before = (SceneSnapshot *)malloc(sizeof(*before));
    if (!before)
        return result_fail(out, plan->first, "out of memory");
    scene_snapshot_capture_live(before);

    for (int row = plan->first; row <= plan->last; row++) {
        /* Rebuilt per row on purpose: the rows already restored are what
         * give this one its scope, its visible variables, and its
         * indent. */
        ReplCompileContext ctx = repl_compile_context_from_live(row);
        ReplCompiledChange change;
        ReplLoadTransactionResult tx;

        err[0] = '\0';
        if (repl_compile_uncomment_line(row, prefix, &ctx, &change,
                                        err, sizeof(err)) != REPL_COMPILE_OK ||
            change.kind != REPL_COMPILED_REPLACE_ONE) {
            failed_row = row;
            break;
        }
        if (!repl_load_apply_compiled_change_transaction(&change, row, &tx)) {
            snprintf(err, sizeof(err), "command store rejected the line");
            failed_row = row;
            break;
        }
        touched_declarations |= (change.predef_op_count > 0);
    }

    if (failed_row >= 0 || rehearse) {
        scene_snapshot_apply_live(before, SCENE_SNAPSHOT_CAMERA_SNAP);
        repl_state_mark_flat_dirty();
        repl_mark_source_dirty();
    }
    free(before);

    if (failed_row >= 0)
        return result_fail(out, failed_row,
                           err[0] ? err : "Cannot uncomment: not a valid command");

    if (out) {
        out->touched_declarations = touched_declarations;
        snprintf(out->message, sizeof(out->message),
                 "Uncommented %d line%s", n, n > 1 ? "s" : "");
    }
    return 1;
}

int repl_comment_toggle_run(const ReplCommentTogglePlan *plan,
                            const char *prefix,
                            ReplCommentToggleMode mode,
                            ReplCommentToggleResult *out) {
    int rehearse = (mode == REPL_COMMENT_TOGGLE_REHEARSE);
    int n;
    int ok;

    result_init(out);
    if (!plan || !prefix || !prefix[0])
        return result_fail(out, -1, "Comment toggle is disabled");

    n = plan->last - plan->first + 1;
    if (plan->first < 0 || n <= 0 || plan->last >= repl_state_document_count())
        return result_fail(out, -1, "No line to toggle");

    if (plan->uncomment)
        ok = apply_uncomment(plan, prefix, rehearse, out);
    else if (rehearse)
        ok = rehearse_comment(plan, prefix, out);
    else
        ok = apply_comment(plan, prefix, out);
    if (!ok)
        return 0;

    if (out) {
        out->line_count = n;
        out->uncommented = plan->uncomment;
    }
    if (!rehearse) {
        repl_state_mark_flat_dirty();
        repl_mark_source_dirty();
    }
    return 1;
}
