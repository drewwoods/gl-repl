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

#include "sample.h"           /* GLCmd, MAX_LINE_LEN, etc. */
#include "editor_state.h"     /* EditorBufferView */
#include "repl_state_views.h" /* REPL_STATUS_TEXT_MAX */
#include "repl_eval.h"        /* MAX_NAMES_PER_DECL, ExprVar */

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
 * Float decls and assignments produce a single cmd; structured-block
 * commits in Phase C commit 20 raise this to cover for-loop bodies
 * (start + body + end). */
#ifndef MAX_COMMIT_CMDS
#define MAX_COMMIT_CMDS 4
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

/* Bound: a single float decl can declare up to MAX_NAMES_PER_DECL
 * names; an assignment writes one. Float-decl overwrite needs DECLARE
 * and UNDECLARE for each delta name. We size for the worst case. */
#ifndef MAX_PREDEF_OPS_PER_COMMIT
#define MAX_PREDEF_OPS_PER_COMMIT (MAX_NAMES_PER_DECL * 2 + 1)
#endif

typedef struct ReplCompiledChange_s {
    ReplCompiledChangeKind kind;
    int                    pos;
    int                    count;
    int                    adjust_edit_line;
    GLCmd                  cmds[MAX_COMMIT_CMDS];
    char                   text[MAX_COMMIT_CMDS][MAX_LINE_LEN];

    /* Predef-variable side-effects, replayed by the apply step. */
    ReplPredefOp           predef_ops[MAX_PREDEF_OPS_PER_COMMIT];
    int                    predef_op_count;

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

#endif /* REPL_COMPILE_H */
