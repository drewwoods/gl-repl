/*
 * src/repl/comment_toggle.h - The Ctrl+/ operation, REPL-owned.
 *
 * The editor passes intent (cursor row, selection, comment prefix); this
 * module decides which rows are affected, which direction the toggle
 * runs, and executes it atomically. The editor keeps only what is its
 * own: the undo snapshot, the status line, and the input row.
 *
 * Why this is not a single pure compile call. Commenting is a text
 * rewrite - prepend a prefix, emit CMD_COMMENT - and the whole range
 * fits one ReplCompiledChange. Uncommenting is not its mirror: once a
 * block is commented out, its structure is gone from the command model,
 * so each row has to be re-parsed against a document where the rows
 * above it are *already* restored (a body line needs its function's
 * parameters, a `}` needs the head it closes, an indent needs the depth
 * those two establish). That is a sequence of mutations, so it is run as
 * a transaction: a SceneSnapshot is restored wholesale if any row in the
 * range is rejected, exactly as repl/replace.c does for find/replace.
 *
 * Both directions leave the document unchanged when they refuse.
 */
#ifndef REPL_COMMENT_TOGGLE_H
#define REPL_COMMENT_TOGGLE_H

#include "config.h"          /* REPL_DIAG_TEXT_MAX, REPL_STATUS_TEXT_MAX */
#include "repl/compile.h"    /* ReplCompileContext */

typedef struct {
    int first;      /* first document row the toggle covers */
    int last;       /* last document row, inclusive */
    int uncomment;  /* 1 = restore code, 0 = comment out */
} ReplCommentTogglePlan;

typedef struct {
    int  line_count;                     /* rows the toggle touched */
    int  uncommented;                    /* 1 when code was restored */
    int  touched_declarations;           /* 1 when predef ops were applied */
    int  failed_row;                     /* -1, or the row that rejected */
    char err[REPL_DIAG_TEXT_MAX];        /* failure diagnostic */
    char message[REPL_STATUS_TEXT_MAX];  /* success diagnostic */
} ReplCommentToggleResult;

/* Resolve the rows Ctrl+/ acts on and which way it runs.
 *
 * `sel_first`/`sel_last` describe an active line-range selection; pass -1
 * for either when there is none. With a selection the range *is* the
 * selection, and the direction is decided by the whole range: uncomment
 * only when every row carries `prefix`, otherwise comment. A mixed range
 * comments the rest rather than toggling each row on its own, so a second
 * press is an exact undo of the first.
 *
 * Without a selection the range grows from `cursor_row`:
 *   - a block head / end / if-branch separator expands to its whole
 *     `[head..end]` block (Ctrl+/ on `triangle() {` takes the function);
 *   - a commented row expands the same way, by matching braces across the
 *     run of commented rows around it - the structure is not in the
 *     command model any more, so it is read back out of the text;
 *   - anything else is just that row.
 *
 * Returns 1 with `out` filled, or 0 with a diagnostic in `err`. Pure. */
int repl_comment_toggle_plan(const ReplCompileContext *ctx,
                             int cursor_row, int sel_first, int sel_last,
                             const char *prefix,
                             ReplCommentTogglePlan *out,
                             char *err, int err_size);

typedef enum {
    /* Answer "would this land?" and leave no trace. For a caller whose
     * own pre-mutation bookkeeping is not reversible - the editor's undo
     * push, which is also the transient-scene promotion hook - so that
     * bookkeeping can wait until the answer is yes.
     *
     * Commenting needs no rehearsal: the compile is pure and the
     * command-store preflight settles the rest, so nothing is touched.
     * Uncommenting has no pure form - each row only parses once the rows
     * above it are restored - so the rehearsal *is* the transaction,
     * rolled back on the way out instead of only on failure. */
    REPL_COMMENT_TOGGLE_REHEARSE = 0,
    /* Execute for real, keeping the result. */
    REPL_COMMENT_TOGGLE_COMMIT,
} ReplCommentToggleMode;

/* Run a resolved plan against live REPL state: source document, command
 * store, predefined variables, function aliases.
 *
 * Atomic in both modes. Returns 1 on success with `out->message` set, or
 * 0 having restored the pre-call scene, with `out->err` set and
 * `out->failed_row` naming the offending document row when there is one.
 * Never touches undo or status - those belong to the caller. */
int repl_comment_toggle_run(const ReplCommentTogglePlan *plan,
                            const char *prefix,
                            ReplCommentToggleMode mode,
                            ReplCommentToggleResult *out);

#endif /* REPL_COMMENT_TOGGLE_H */
