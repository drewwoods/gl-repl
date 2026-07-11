/*
 * src/repl/compile.h - Pure validators that compile proposed source text
 * into ReplCompiledChange.
 *
 * Contract:
 *   ReplCompileResult repl_compile_*(...);
 *       Pure. No editor mutation. No command-store mutation. No status
 *       mutation. No undo entry. Returns ReplCompiledChange on success
 *       or fills `err` with a diagnostic on failure.
 *
 *   void repl_apply_compiled_change(const ReplCompiledChange *change);
 *       Mutates ReplState command arrays only.
 *       Does not touch editor text. Does not touch status.
 *
 *   void editor_buffer_apply_compiled_change(const ReplCompiledChange *change);
 *       Mutates EditorState text only. Does not
 *       touch ReplState. Does not touch status.
 *
 * The editor commit orchestration drives all three inside one undo
 * transaction.
 *
 * ReplCompiledChange is a *source-command* description, not a flat
 * program. Flattening still happens downstream in src/repl/flatten.c.
 */
#ifndef REPL_COMPILE_H
#define REPL_COMPILE_H

#include "config.h"          /* MAX_COMMIT_CMDS, MAX_LINE_LEN */
#include "repl/command.h"    /* GLCmd */
#include "repl/eval.h"       /* MAX_NAMES_PER_DECL, ExprVar */
#include "repl/source_scope.h" /* ReplSourceScopeView */
#include "source_document.h" /* SourceTextView */

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
} ReplCompiledChangeKind;

/* The maximum number of cmds a single compile call can describe.
 * Float decls and assignments produce a single cmd; for-loop
 * one-liners produce 3 (begin + body + end); func_def with leading
 * comment relocation produces N comments + begin + end where N is
 * the count of contiguous depth-0 comments above the cursor.
 *
 * MAX_COMMIT_CMDS lives in config.h (the neutral limits home) so
 * source_document.h can size SourceTextChange.text[] with the same
 * bound. This header pulls it through config.h (already included
 * transitively via src/repl/command.h) — keep the comment block above
 * for context, but no duplicate define. */

/* Predef-variable side-effects produced by compile. The apply step
 * replays these atomically:
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
    char             name[REPL_PREDEF_NAME_MAX];
    float            value;       /* used by SET_VALUE / DECLARE-with-init */
    int              has_value;   /* DECLARE: 1 = with init expression */
} ReplPredefOp;

typedef struct {
    int   array_idx;
    int   elem_idx;
    float value;
} ReplScratchOp;

typedef struct {
    int  slot;                         /* -1 = no alias update */
    char name[REPL_FUNC_NAME_MAX];     /* empty clears the slot */
} ReplFuncAliasOp;

/* Bound: covers the two worst cases.
 *   - Float-decl overwrite emits DECLARE + UNDECLARE per delta name
 *     (MAX_NAMES_PER_DECL * 2 + 1).
 *   - Delete-range can UNDECLARE every live predef in one transaction
 *     (MAX_PREDEF_VARS).
 * With the default config, MAX_PREDEF_VARS dominates MAX_NAMES_PER_DECL * 2 + 1
 * (32 vs. 17), so we pick that. */
#ifndef MAX_PREDEF_OPS_PER_COMMIT
#define MAX_PREDEF_OPS_PER_COMMIT MAX_PREDEF_VARS
#endif

#ifndef MAX_SCRATCH_OPS_PER_COMMIT
#define MAX_SCRATCH_OPS_PER_COMMIT 1
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

    /* Func-alias side-effect emitted by compile and committed by apply.
     * Compile never mutates the alias table; new names are parsed through
     * this pending op, then `repl_apply_alias_ops()` publishes the alias
     * after the source-command mutation succeeds. */
    ReplFuncAliasOp        alias_op;

    /* Success message (used by callers; not mutated by compile). */
    char                   commit_message[REPL_STATUS_TEXT_MAX];
} ReplCompiledChange;

/* Read-only context passed to compile functions. The compile function
 * takes whatever it needs from here; it never reaches back into REPL
 * globals — document, source-scope, function-alias, and predefined-variable
 * reads all come from this snapshot, so compile is driven entirely by the
 * context.
 */
typedef struct {
    int               edit_line;        /* current cursor source-cmd idx */
    int               document_count;   /* current cmd count */
    int               insert_mode;      /* 1 if editor is in insert mode */
    SourceTextView    text;             /* source-text view for ident-usage checks */
    const GLCmd      *document_cmds;    /* source-command array snapshot */
    ReplFuncAliasView func_aliases;     /* function aliases visible to parser calls */
    ReplSourceScopeView source_scope;   /* scope/depth view over document_cmds */
    ReplPredefView    predef;           /* predefined-variable table snapshot */
} ReplCompileContext;

typedef enum {
    REPL_COMPILE_OK = 0,
    REPL_COMPILE_ERROR,
} ReplCompileResult;

/* Initialize a ReplCompiledChange to its zero state. */
void repl_compiled_change_init(ReplCompiledChange *out);

/* Translate a ReplCompiledChange into the neutral SourceTextChange
 * shape consumed by source_document_apply_change(). Copies kind, pos,
 * count, delete_pos, delete_count, and up to SOURCE_TEXT_CHANGE_MAX_LINES
 * text rows. Used by the load/apply path so the REPL pipeline no
 * longer calls editor_buffer_apply_compiled_change directly. */
void repl_compiled_change_to_text_change(const ReplCompiledChange *in,
                                         SourceTextChange *out);

/* Build a compile context from the live REPL state + a
 * caller-supplied edit-line index. The caller passes the value
 * because REPL pipeline code does not call editor_state_edit_line();
 * callers above that boundary (controllers, editor commit code,
 * tests) read the cursor and pass it in. */
ReplCompileContext repl_compile_context_from_live(int edit_line_idx);

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

/* Compile a "split this declaration" request for the CMD_VAR_DECLARE at
 * `line_idx`: replace it in place with one single-name CMD_VAR_DECLARE
 * per declared name (`float a, b, c;` -> three lines). Purely a source-
 * representation change — the variables are already declared with their
 * current values, so no predef ops are emitted. The original line's
 * trailing comment rides the first emitted line. Same purity contract as
 * repl_compile_float_decl. Returns REPL_COMPILED_NO_CHANGE when line_idx
 * is not a CMD_VAR_DECLARE with at least two names. */
ReplCompileResult repl_compile_split_decl(const ReplCompileContext *ctx,
                                          int line_idx,
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

/* Compile dispatcher: walks all six per-kind compile validators in
 * canonical order and returns the first one that produces a
 * non-NO_CHANGE result. NO_CHANGE means the input didn't match any
 * handler — the caller decides whether that is "fall through to a
 * generic-command parser" (e.g. repl_load_apply_line punts back to
 * repl_parser_parse_command) or "no commit happened".
 *
 * Coverage: float_decl → var_assign → if_branch → close_brace →
 * for_loop → func_def → if_block.
 *
 * Handler order is load-bearing: `repl_compile_float_decl` must run
 * before `repl_compile_var_assign`, otherwise `float x;` would parse
 * as an assignment to an identifier named `float`. */
ReplCompileResult repl_compile_dispatch(const char *text,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size);

/* Pure structured-block compile helpers for the line-at-a-time loader.
 *
 * These are the REPL-pipeline-side counterparts to the editor's
 * editor_compile_close_brace / _if_block / _func_def / _for_loop
 * wrappers in src/editor/commit.c. They perform the same parse and
 * validation work, but return a ReplCompiledChange with no editor-side baggage
 * (no cursor target, insert-mode toggle, or clear-input request).
 *
 * Scope: line-by-line load. repl_load_apply_line() uses them for import/example
 * loading, where a single source line either inserts one command or fails.
 * They do NOT handle the editor's edit-time branches (header replace, oneliner
 * body, matched-existing-end close-brace) since those only arise in the live
 * editor commit path. This split became explicit in step 5a/5b of
 * feature/decouple-repl-from-gl-repl-alt.md.
 *
 * Each returns:
 *   REPL_COMPILE_OK + INSERT_ONE         valid block-structure line
 *   REPL_COMPILE_OK + NO_CHANGE          input doesn't match this shape
 *                                        (caller falls through)
 *   REPL_COMPILE_ERROR                   syntax error; `err` filled */
ReplCompileResult repl_compile_close_brace(const char *input,
                                           const ReplCompileContext *ctx,
                                           ReplCompiledChange *out,
                                           char *err, int err_size);

ReplCompileResult repl_compile_if_block(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size);

ReplCompileResult repl_compile_if_branch(const char *input,
                                         const ReplCompileContext *ctx,
                                         ReplCompiledChange *out,
                                         char *err, int err_size);

/* Resolve a custom function name in `trimmed` to a pending alias op.
 * Pure: never writes the alias table. Existing aliases produce no op
 * because the parser can already resolve them from live state. */
ReplCompileResult repl_compile_func_def_resolve_alias(const ReplCompileContext *ctx,
                                                      const char *trimmed,
                                                      ReplCompiledChange *out,
                                                      int *rejected_keyword,
                                                      char *err, int err_size);

ReplCompileResult repl_compile_func_def(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size);

ReplCompileResult repl_compile_for_loop(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size);

/* Shared parse/validate kernel for `for(var, start, end[, step]) body;`.
 *
 * Both repl_compile_for_loop (the lean-loader path) and
 * editor_compile_for_loop (which adds header-replace / one-liner-body /
 * paired-end branches on top) call this kernel. The kernel handles:
 *   - whitespace + `for(` / `for (` prefix detection
 *   - visible-var collection at the insert pos
 *   - for-header parse (var_name, start, end, step, body_start)
 *   - raw-args extraction + identifier validation
 *   - indent resolution
 *   - CMD_FOR_BEGIN assembly (args[0..2] + has_vars)
 *   - fb_text formatting (symbolic when args contain vars; literal
 *     via repl_format_source_float otherwise)
 *
 * Returns:
 *   REPL_COMPILE_OK with out->valid == 0 — input wasn't a for-loop.
 *   REPL_COMPILE_OK with out->valid == 1 — parsed; out is fully populated.
 *   REPL_COMPILE_ERROR                   — syntax / format error; err set.
 *
 * Editor-only concerns (header-replace REPLACE_ONE, paired-end
 * INSERT_MANY count=2, one-liner-body count=3, effects, commit_message)
 * stay in editor_compile_for_loop. */
typedef struct {
    int        valid;
    int        pos;
    char       var_name[REPL_PREDEF_NAME_MAX];
    float      start, end, step;
    const char *body_start;             /* points into the caller's input */
    GLCmd      fb;                      /* CMD_FOR_BEGIN, args + has_vars set */
    char       fb_text[MAX_LINE_LEN];
    char       indent[REPL_INDENT_TEXT_MAX];
    /* Visible-var scope at the insert pos. The editor's one-liner-body
     * branch parses a body command under (var_name + start) prepended to
     * this scope; the loader's wrapper ignores it. */
    ExprVar    visible_vars[MAX_EXPR_VARS];
    int        visible_nv;
} ReplForLoopKernel;

ReplCompileResult repl_compile_for_loop_kernel(const char *input,
                                               const ReplCompileContext *ctx,
                                               ReplForLoopKernel *out,
                                               char *err, int err_size);

/* Shared kernel for `if(expr) {`. Same contract as the for-loop
 * kernel: OK + valid=0 means "input isn't an if-block, fall through";
 * OK + valid=1 means out is populated; ERROR means err filled.
 *
 * Editor wraps with header-replace REPLACE_ONE and INSERT_MANY count=2
 * (paired begin + end); the loader emits INSERT_ONE for just the
 * begin (the `}` end-marker arrives as a separate file line). */
typedef struct {
    int    valid;
    int    pos;
    GLCmd  ib;                          /* CMD_IF_BEGIN, args[0] = cond_val */
    char   ib_text[MAX_LINE_LEN];
    char   indent[REPL_INDENT_TEXT_MAX];
} ReplIfBlockKernel;

ReplCompileResult repl_compile_if_block_kernel(const char *input,
                                               const ReplCompileContext *ctx,
                                               ReplIfBlockKernel *out,
                                               char *err, int err_size);

/* Shared kernel for if-branch separator lines:
 *   } else if(expr) {
 *   } else {
 *
 * These are source-command separators inside an already-open
 * CMD_IF_BEGIN / CMD_IF_END range. The kernel validates that the
 * current insert position is in an if-chain, that CMD_ELSE appears at
 * most once and last, and that an else-if condition is valid in the
 * visible scope. */
typedef struct {
    int      valid;
    int      pos;
    CmdType  branch_type;               /* CMD_ELSE_IF / CMD_ELSE */
    GLCmd    branch;
    char     branch_text[MAX_LINE_LEN];
    char     indent[REPL_INDENT_TEXT_MAX];
} ReplIfBranchKernel;

ReplCompileResult repl_compile_if_branch_kernel(const char *input,
                                                const ReplCompileContext *ctx,
                                                ReplIfBranchKernel *out,
                                                char *err, int err_size);

/* Shared kernel for `}`. No expression parse — just the
 * open-block scope lookup and the matched-existing-end vs insert-
 * new-end-marker decision. Both wrappers consume:
 *   - end_type to thread label strings and (for the editor) the
 *     func-decl-resume one-shot.
 *   - matched_existing == 1 means the close-brace landed on a row
 *     that's already the right end marker; the loader emits
 *     NO_CHANGE; the editor advances the cursor without writing.
 *   - When matched_existing == 0 the kernel populates `fe` + `fe_text`
 *     + `pos` for an INSERT_ONE. */
typedef struct {
    int      valid;
    int      pos;
    CmdType  open_type;                 /* CMD_FOR_BEGIN / FUNC_DEF / IF_BEGIN */
    CmdType  end_type;                  /* CMD_FOR_END / FUNC_END / IF_END */
    int      matched_existing;          /* 1 = NO_CHANGE; 0 = INSERT_ONE */
    GLCmd    fe;                        /* meaningful only when !matched_existing */
    char     fe_text[MAX_LINE_LEN];
} ReplCloseBraceKernel;

ReplCompileResult repl_compile_close_brace_kernel(const char *input,
                                                  const ReplCompileContext *ctx,
                                                  ReplCloseBraceKernel *out,
                                                  char *err, int err_size);

/* Shared kernel for `funcN(params) {` (and alias-named variants like
 * `drawCube { ... }`). Resolves a pending alias without mutating the
 * alias table, parses the signature, runs the duplicate-funcN guard,
 * resolves indent, and emits CMD_FUNC_DEF + fd_text.
 *
 * The duplicate guard is policy-controlled: `allow_overwrite_at_pos`
 * gives the caller's edit-position when an in-place rewrite is OK
 * (editor overwrite-header branch); -1 rejects every duplicate (loader
 * path). On rejection the pending alias is simply dropped; there is no
 * global state to roll back. */
typedef struct {
    int    valid;
    int    rejected_keyword;            /* parser rejected reserved name */
    int    pos;
    int    fn;
    int    param_count;
    char   param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
    GLCmd  fd;
    char   fd_text[MAX_LINE_LEN];
    char   indent[REPL_INDENT_TEXT_MAX];
    ReplFuncAliasOp alias_op;
} ReplFuncDefKernel;

ReplCompileResult repl_compile_func_def_kernel(const char *input,
                                               const ReplCompileContext *ctx,
                                               int allow_overwrite_at_pos,
                                               ReplFuncDefKernel *out,
                                               char *err, int err_size);

/* repl_load_apply_line moved to src/repl/load.h to keep this header pure
 * (compile descriptors only; no apply orchestration). */

/* Compile an external live-value update for an existing predefined
 * variable.
 *
 * Source policy:
 *   - If a CMD_VAR_DECLARE line contains the variable, rewrite just
 *     that declarator's initializer while preserving the other
 *     declarators and any trailing comment.
 *   - Otherwise emit REPL_COMPILED_NO_CHANGE and only carry the
 *     REPL_PREDEF_OP_SET_VALUE side effect. Assignment rows are not
 *     source rewrite targets.
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

/* The two halves of the above, for callers that need to separate the live
 * value from its persistence — the variable-panel drag applies the live half
 * on every pointer motion and the source half once, on mouse-up.
 *
 * `_live` emits only REPL_PREDEF_OP_SET_VALUE with REPL_COMPILED_NO_CHANGE:
 * no editor text is rewritten and no command-store row is touched, so a drag
 * does not mark the source dirty on every motion event.
 *
 * `_persist` emits only the declaration rewrite and no predef op (the live
 * value is already final), or REPL_COMPILED_NO_CHANGE when the variable has
 * no declaration row. Because it carries no predef op, applying it does not
 * fire a second tutorial variable notification.
 *
 * All three share one lookup/rewrite kernel, so the declaration text they
 * emit cannot drift apart. Both are pure, like every other compile helper.
 */
ReplCompileResult repl_compile_set_predef_value_live(const char *name,
                                                     float value,
                                                     const ReplCompileContext *ctx,
                                                     ReplCompiledChange *out,
                                                     char *err, int err_size);

ReplCompileResult repl_compile_persist_predef_value(const char *name,
                                                    float value,
                                                    const ReplCompileContext *ctx,
                                                    ReplCompiledChange *out,
                                                    char *err, int err_size);

/* Compile a delete-range operation: clamp `start`/`count` to the
 * document, validate that no CMD_VAR_DECLARE in the range owns a name
 * still referenced outside the range, and populate UNDECLARE predef
 * ops for every variable declared inside the range. The apply step
 * cascades the predef compaction (CMD_VAR_ASSIGN.var_idx adjustment)
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
 *     DELETE_RANGE are rejected as multi-line uncomment.
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
