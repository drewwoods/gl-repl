/*
 * repl_compile.h - Pure validators that compile proposed source text
 * into ReplCompiledChange.
 *
 * Phase C contract:
 *   ReplCompileResult repl_compile_*(...);
 *       Pure. No editor mutation. No command-store mutation. No status
 *       mutation. No undo entry. Returns ReplCompiledChange on success
 *       or fills `err` with a diagnostic on failure.
 *
 *   void repl_apply_compiled_change(const ReplCompiledChange *change);
 *       (Phase C commit 20.) Mutates ReplState command arrays only.
 *       Does not touch editor text. Does not touch status.
 *
 *   void editor_buffer_apply_compiled_change(const ReplCompiledChange *change);
 *       (Phase C commit 20.) Mutates EditorState text only. Does not
 *       touch ReplState. Does not touch status.
 *
 * The editor commit orchestration (Phase C commit 21) drives all three
 * inside one undo transaction.
 *
 * ReplCompiledChange is a *source-command* description, not a flat
 * program. Flattening still happens downstream in repl_flatten.c.
 */
#ifndef REPL_COMPILE_H
#define REPL_COMPILE_H

#include "config.h"
#include "editor/state.h" /* EditorBufferView */
#include "repl_command.h" /* GLCmd, MAX_LINE_LEN */
#include "repl_eval.h"    /* MAX_NAMES_PER_DECL, ExprVar */

/* Source-command level shape of a compiled change. The `kind` field
 * picks which fields are meaningful:
 *
 *   REPL_COMPILED_NO_CHANGE
 *       The compile decided nothing needed to mutate. cmds/text are
 *       unused. predef_ops may still carry side-effects (e.g. a var
 *       assignment that updates a predef value but doesn't change the
 *       source).
 *   REPL_COMPILED_INSERT_ONE
 *       Insert cmds[0] / text[0] at `pos`. count==1.
 *   REPL_COMPILED_REPLACE_ONE
 *       Replace cmd at `pos` with cmds[0] / text[0]. count==1.
 *   REPL_COMPILED_INSERT_MANY
 *       Insert cmds[0..count) / text[0..count) at `pos`.
 *   REPL_COMPILED_DELETE_RANGE
 *       Delete `count` commands starting at `pos`. cmds/text unused.
 *   REPL_COMPILED_LOAD_ALL
 *       Bulk replace: clear and load cmds[0..count) / text[0..count).
 *
 * `adjust_edit_line` controls whether insert ops shift the cursor
 * forward when the cursor was at/before `pos` (mirrors
 * REPL_COMMAND_STORE_ADJUST_EDIT_LINE).
 *
 * `commit_message` carries the success diagnostic the controller /
 * status layer can display after the transaction lands. Compile
 * functions must NOT call set_status() themselves.
 */
typedef enum {
    REPL_COMPILED_NO_CHANGE = 0,
    REPL_COMPILED_INSERT_ONE,
    REPL_COMPILED_REPLACE_ONE,
    REPL_COMPILED_INSERT_MANY,
    REPL_COMPILED_DELETE_RANGE,
    REPL_COMPILED_LOAD_ALL,
} ReplCompiledChangeKind;

/* The maximum number of cmds a single compile call can describe.
 * Float decls and assignments produce a single cmd; for-loop
 * one-liners produce 3 (begin + body + end); func_def with leading
 * comment relocation produces N comments + begin + end where N is
 * the count of contiguous depth-0 comments above the cursor.
 *
 * Set to 16 so practical func_def relocation cases (typical 0-4
 * leading comments) fit comfortably; deeper comment blocks are
 * rejected with a diagnostic in editor_compile_func_def. */
#ifndef MAX_COMMIT_CMDS
#define MAX_COMMIT_CMDS 16
#endif

/* Predef-variable side-effects produced by compile. The apply step
 * (Phase C commit 20) replays these atomically:
 *
 *   REPL_PREDEF_OP_DECLARE   register a new name (or no-op if already
 *                            registered with same slot).
 *   REPL_PREDEF_OP_UNDECLARE remove a name (and cascade num_args
 *                            adjustment in apply).
 *   REPL_PREDEF_OP_SET_VALUE write the live value of an existing
 *                            slot (assignments). `value` is set;
 *                            `name` identifies the slot.
 */
typedef enum {
    REPL_PREDEF_OP_NOOP = 0,
    REPL_PREDEF_OP_DECLARE,
    REPL_PREDEF_OP_UNDECLARE,
    REPL_PREDEF_OP_SET_VALUE,
} ReplPredefOpKind;

typedef struct {
    ReplPredefOpKind kind;
    char             name[16];
    float            value;       /* used by SET_VALUE / DECLARE-with-init */
    int              has_value;   /* DECLARE: 1 = with init expression */
} ReplPredefOp;

typedef struct {
    int   array_idx;
    int   elem_idx;
    float value;
} ReplScratchOp;

/* Bound: covers the two worst cases.
 *   - Float-decl overwrite emits DECLARE + UNDECLARE per delta name
 *     (MAX_NAMES_PER_DECL * 2 + 1).
 *   - Delete-range can UNDECLARE every live predef in one transaction
 *     (MAX_PREDEF_VARS).
 * MAX_PREDEF_VARS dominates today (24 vs. 17), so we pick that. */
#ifndef MAX_PREDEF_OPS_PER_COMMIT
#define MAX_PREDEF_OPS_PER_COMMIT MAX_PREDEF_VARS
#endif

#ifndef MAX_SCRATCH_OPS_PER_COMMIT
#define MAX_SCRATCH_OPS_PER_COMMIT MAX_COMMIT_CMDS
#endif

typedef struct ReplCompiledChange_s {
    ReplCompiledChangeKind kind;
    int                    pos;
    int                    count;
    int                    adjust_edit_line;
    GLCmd                  cmds[MAX_COMMIT_CMDS];
    char                   text[MAX_COMMIT_CMDS][MAX_LINE_LEN];

    /* Optional pre-insert delete: when delete_count > 0, apply
     * deletes `delete_count` source commands starting at
     * `delete_pos` BEFORE running the main change. The combined
     * shape is "delete a range, then INSERT_MANY at a new
     * position" expressed as one atomic plan.
     *
     * Important convention: `pos` is interpreted in the
     * **post-delete** document. compile is responsible for
     * translating any insert position into post-delete
     * coordinates so apply does not redo the math.
     *
     * Sentinel: delete_pos = -1, delete_count = 0 means no delete.
     */
    int                    delete_pos;
    int                    delete_count;

    /* Predef-variable side-effects, replayed by the apply step. */
    ReplPredefOp           predef_ops[MAX_PREDEF_OPS_PER_COMMIT];
    int                    predef_op_count;

    /* Scratch-array side-effects, replayed by the apply step. */
    ReplScratchOp          scratch_ops[MAX_SCRATCH_OPS_PER_COMMIT];
    int                    scratch_op_count;

    /* Success message (used by callers; not mutated by compile). */
    char                   commit_message[REPL_STATUS_TEXT_MAX];
} ReplCompiledChange;

/* Read-only context passed to compile functions. The compile function
 * takes whatever it needs from here; it never reaches back into REPL
 * globals.
 *
 * Today's transitional state: compile functions still read predef
 * vars through the shared eval table because that table is ReplState-
 * adjacent and not yet threaded through context. Phase D ties the
 * remaining loose ends.
 */
typedef struct {
    int               edit_line;        /* current cursor source-cmd idx */
    int               document_count;   /* current cmd count */
    int               insert_mode;      /* 1 if editor is in insert mode */
    EditorBufferView  text;             /* source-text view for ident-usage checks */
    const GLCmd      *document_cmds;    /* source-command array snapshot */
} ReplCompileContext;

typedef enum {
    REPL_COMPILE_OK = 0,
    REPL_COMPILE_ERROR,
} ReplCompileResult;

/* Initialize a ReplCompiledChange to its zero state. */
void repl_compiled_change_init(ReplCompiledChange *out);

/* Build a compile context from the live REPL state. Convenience
 * helper for callers in transition; once the editor commit
 * orchestration owns this, the caller will assemble the context
 * directly from EditorState + ReplState handles. */
ReplCompileContext repl_compile_context_from_live(void);

/* Compile a `float name[, name2 ...][ = expr];` declaration into a
 * ReplCompiledChange describing the source change + predef ops.
 * Pure: never mutates state, never calls set_status. Returns
 * REPL_COMPILE_OK on success and fills `out`; returns
 * REPL_COMPILE_ERROR on parse / validation failure and writes the
 * diagnostic into `err` (NUL-terminated, bounded by `err_size`).
 *
 * The `input` argument is the user's input buffer; the `;` is
 * optional (interactive commits may omit it). Returns
 * REPL_COMPILED_NO_CHANGE if `input` doesn't look like a float decl
 * (the dispatcher should fall through to the next handler). */
ReplCompileResult repl_compile_float_decl(const char *input,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size);

/* Compile a `name = expr;` assignment into a ReplCompiledChange.
 * Same purity contract as `repl_compile_float_decl`. Returns
 * REPL_COMPILED_NO_CHANGE if `input` doesn't look like an
 * assignment. */
ReplCompileResult repl_compile_var_assign(const char *input,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size);

/* Compile an external live-value update for an existing predefined
 * variable.
 *
 * Source policy:
 *   - Prefer the last literal CMD_VAR_ASSIGN for the variable and
 *     replace it with canonical `name = %g;` text.
 *   - Otherwise, if a CMD_VAR_DECLARE line contains the variable,
 *     rewrite just that declarator's initializer while preserving
 *     the other declarators and any trailing comment.
 *   - Otherwise emit REPL_COMPILED_NO_CHANGE and only carry the
 *     REPL_PREDEF_OP_SET_VALUE side effect.
 *
 * This entry is pure like the other compile helpers: it never
 * mutates editor text, command arrays, predef storage, status, or
 * undo history. `name` must already identify a live predefined slot.
 */
ReplCompileResult repl_compile_set_predef_value(const char *name,
                                                float value,
                                                const ReplCompileContext *ctx,
                                                ReplCompiledChange *out,
                                                char *err, int err_size);

/* Compile a delete-range operation: clamp `start`/`count` to the
 * document, validate that no CMD_VAR_DECLARE in the range owns a name
 * still referenced outside the range, and populate UNDECLARE predef
 * ops for every variable declared inside the range. The apply step
 * cascades the predef compaction (CMD_VAR_ASSIGN.num_args adjustment)
 * automatically.
 *
 * Pure: never mutates state, never calls set_status.
 *
 * Returns:
 *   REPL_COMPILE_OK + REPL_COMPILED_DELETE_RANGE  delete plan ready
 *   REPL_COMPILE_OK + REPL_COMPILED_NO_CHANGE     range was empty after
 *                                                 clamping (count<=0)
 *   REPL_COMPILE_ERROR                            still-referenced name
 *                                                 or predef-op overflow;
 *                                                 `err` filled.
 *
 * `commit_message` is set to a neutral default ("Removed N lines");
 * the editor caller may overwrite it with a verb-specific phrasing
 * ("Cut N lines"). */
ReplCompileResult repl_compile_delete_range(int start, int count,
                                            const ReplCompileContext *ctx,
                                            ReplCompiledChange *out,
                                            char *err, int err_size);

/* Compile a blank-line insert at `line_idx`. Produces INSERT_ONE
 * with `cmds[0]` set to CMD_EMPTY and `text[0]` empty. Pure: never
 * mutates state, never calls set_status. Lets the editor's
 * Enter-on-empty-input path go through the compile/apply seam
 * without constructing a GLCmd inline. */
ReplCompileResult repl_compile_empty_line(int line_idx,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size);

/* Compile a comment-toggle on `line_idx` using `prefix` as the
 * line-comment marker (e.g., "// "). The REPL fully owns toggle
 * semantics — the editor passes intent, this entry decides.
 *
 * Behavior by cmd kind at line_idx:
 *
 *   Plain non-comment line: prepend `prefix` after leading whitespace
 *     and produce REPLACE_ONE with cmds[0]=CMD_COMMENT. commit_message
 *     "Commented out 1 line".
 *
 *   CMD_COMMENT line: strip `prefix` (must match the line's prefix
 *     after leading whitespace; otherwise REPL_COMPILE_ERROR), then
 *     re-parse the stripped text via the dispatch handlers + GL
 *     parser fallback. Result is coerced to REPLACE_ONE at line_idx
 *     (override kind/pos/count, preserve cmds[0], text[0],
 *     predef_ops, scratch_ops). Re-parses producing INSERT_MANY /
 *     DELETE_RANGE / LOAD_ALL are rejected as multi-line uncomment.
 *     commit_message "Uncommented 1 line".
 *
 *   Block head (CMD_FOR_BEGIN, CMD_FUNC_DEF, CMD_IF_BEGIN) or end
 *     (CMD_FOR_END, CMD_FUNC_END, CMD_IF_END): walk to the matching
 *     other end; for every line in [head..end] inclusive, build
 *     prefix-prepended text and a CMD_COMMENT cmd. Returns
 *     INSERT_MANY at `head` with `delete_pos=head, delete_count=N`
 *     (combined replace-range plan; uses ReplCompiledChange's
 *     pre-insert delete fields). commit_message "Commented out N
 *     lines". Block size is capped at MAX_COMMIT_CMDS; larger blocks
 *     are rejected with a diagnostic.
 *
 * `prefix` may be NULL or empty — the function returns NO_CHANGE in
 * that case (toggle disabled). `line_idx` out of range also returns
 * NO_CHANGE.
 *
 * Pure: never mutates state, never calls set_status. */
ReplCompileResult repl_compile_toggle_comment(int line_idx,
                                              const char *prefix,
                                              const ReplCompileContext *ctx,
                                              ReplCompiledChange *out,
                                              char *err, int err_size);

#endif /* REPL_COMPILE_H */
