/*
 * src/repl/eval.h - Expression evaluator, translators, and for-loop parsers.
 *
 * Recursive-descent expression evaluator for the REPL math layer. Supports
 * binary operators (+, -, *, /, %), unary minus, parentheses, function
 * calls (sin, cos, tan, sqrt, abs, pow, min, max, floor, ceil, fmod, rem,
 * rand, rand2), and constants (PI, TAU, e, NAN, INFINITY). All values are floats.
 *
 * Predefined variables (float x, y, z, t, etc.) are declared via
 * repl_eval_declare_predef_var(). The time variable 't' is special: it
 * increments each frame when animation is enabled (Ctrl+T). User variables
 * default to 0.0; expressions can reference them after declaration.
 *
 * Two expression syntaxes:
 *   REPL:  sin(t), x + y, pow(2, 3)
 *   C:     sinf(t), x + y, powf(2.0, 3.0)  [auto-translated to REPL on parse]
 *
 * For-loop syntax:
 *   REPL:  for(i, 0, 10, 0.5)  { ... }    [var, start, end, step]
 *   C:     for (float i = 0; i < 10; i += 0.5) { ... }
 *
 * This header parses expressions and loop headers, and translates REPL <-> C
 * math syntax. Loop expansion and unrolling happen in src/repl/flatten.c.
 *
 * This module is extracted for independent testing. Include this header, then:
 *   - #include "repl/eval.c" for single-TU builds, or
 *   - link with src/repl/eval.c compiled separately
 */
#ifndef REPL_EVAL_H
#define REPL_EVAL_H

#include "config.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 *  MAX_PREDEF_VARS — global user-declared variables
 *  -------------
 *  This sizes the predefined-variable table populated by float t;, float radius;, etc. It's the global identifier table the user sees.
 *
 *  - Storage: g_predef_vars[MAX_PREDEF_VARS] (in src/repl/eval.c).
 *  - One slot is reserved for the built-in t (animation time).
 *  - MAX_PREDEF_VARS - 1 slots remain for user float name; declarations.
 *  - The float-decl compile path rejects new declarations once full with
 *    "variable table full (max <MAX_PREDEF_VARS>)" — the macro value is
 *    formatted into the message.
 *  The REPL UI shows all declared variables in the "Predefined Variables" section of the code panel, and they are available for use in expressions.
 *  Used in:
 *  - src/repl/eval.c — the table itself.
 *  - src/editor/undo.c — undo snapshots capture all MAX_PREDEF_VARS slots' names + values.
 *  - replay and controller baselines — mode transitions copy/restore all MAX_PREDEF_VARS values.
 *  - src/repl/state_views.h — the by-value snapshot the UI reads from.
 *
 *
 *  MAX_EXPR_VARS — local lexical scope at parse time
 *  -------------
 *  This sizes the per-expression visible-variable list built when compiling a
 *  single expression. It's a lexical scope handed to repl_eval so it can
 *  resolve identifiers like i, j, function parameters, and predef vars at
 *  evaluation time without taking out a global lock.
 *
 *  - Storage: per-call stack arrays — ExprVar vis[MAX_EXPR_VARS] in src/repl/compile.c,
 *    ExprVar vars[MAX_EXPR_VARS] in src/repl/flatten.c, and params[MAX_EXPR_VARS]
 *    in autocomplete hint generation.
 *  - Populated by collect_visible_vars() walking the active for-loop / function-def scope.
 *  - Includes function-call parameters, for-loop iterator vars, and visible predef vars.
 *  - Acts as a name-resolution snapshot during one parse/eval call; not persistent state.
 *
 *  Why two
 *  -------
 *
 *  - MAX_PREDEF_VARS caps how many global identifiers can exist at once.
 *  - MAX_EXPR_VARS caps how many identifiers a single expression's lexical scope can hold while it's being parsed.
 *
 *  They are independent limits (MAX_PREDEF_VARS and MAX_EXPR_VARS need not
 *  be equal — see config.h) and not the same constraint:
 *
 *  - MAX_EXPR_VARS actually needs to be MAX_PREDEF_VARS + worst-case nested
 *    locals to be fully correct. With deeply nested for-loops each declaring
 *    a fresh iterator name, the visible-name count can exceed MAX_EXPR_VARS;
 *    the parser then truncates the visible-list silently. (Not a great
 *    property, but the headroom between the two values hides it in practice.)
 *  - MAX_PREDEF_VARS is the user-facing "how many float x; can I have" limit.
 */

#ifndef MAX_NAMES_PER_DECL
#define MAX_NAMES_PER_DECL 8
#endif

#ifndef REPL_SCRATCH_ARRAY_COUNT
#define REPL_SCRATCH_ARRAY_COUNT 3
#endif

#ifndef REPL_SCRATCH_ARRAY_LEN
#define REPL_SCRATCH_ARRAY_LEN 8
#endif

/* Function slots: the REPL has a fixed `funcN` table (0..9). Each slot
 * may carry an optional user-chosen alias name, e.g. so `drawCube` can
 * call into slot 4. The slot is still the load-bearing identity stored
 * in CMD_FUNC_DEF / CMD_CALL via GLCmd.args[0]; the alias is purely a
 * display + parser-recognition layer. */
#ifndef REPL_FUNC_SLOT_COUNT
#define REPL_FUNC_SLOT_COUNT 10
#endif

/* Buffer size for a func-slot alias name: 24 usable chars + NUL. Used as
 * both the storage width (g_active_func_aliases, UserScene snapshots) and
 * the parse cap in repl_parse_func_name_token / parse_func_alias, so all
 * three stay in lock-step off this one macro. */
#ifndef REPL_FUNC_NAME_MAX
#define REPL_FUNC_NAME_MAX 25
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Types ------------------------------------------------------------ */

static inline int repl_eval_is_ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static inline int repl_eval_is_ident_continue(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* A single variable name/value pair in the expression context. */
typedef struct {
    char  name[REPL_PREDEF_NAME_MAX];
    float value;
} ExprVar;

/* Parser context: tracks current position in input string and available
 * variables. Used internally by repl_eval_expr() during recursive descent. */
typedef struct {
    const char *p;          /* current position in string */
    const ExprVar *vars;
    int         num_vars;
    char       *err;
    int         err_sz;
} ExprCtx;

/* ---- Predefined variables (global scope) ------------------------------- */

/* Predefined-variable table access. In the REPL binary these names
 * resolve to ReplRuntimeState.variables; standalone evaluator tests
 * use src/repl/eval.c's fallback storage. Reads go through the const
 * accessors below; the `_mut` siblings stay reserved for the writer
 * helpers in eval.c / state.c / executor.c / apply.c / flatten.c /
 * import.c (the only places that legitimately mutate the table). */
const ExprVar *repl_eval_predef_vars(void);
int            repl_eval_predef_count(void);
ExprVar       *repl_eval_predef_vars_mut(void);
int           *repl_eval_predef_count_mut(void);
void           repl_eval_bind_predef_storage(ExprVar *vars, int *count_ptr);

#define g_predef_vars         (repl_eval_predef_vars())
#define g_num_predef_vars     (repl_eval_predef_count())
#define g_predef_vars_mut     (repl_eval_predef_vars_mut())
#define g_num_predef_vars_mut (*repl_eval_predef_count_mut())

typedef struct {
    const ExprVar *vars;
    int            count;
} ReplPredefView;

typedef struct {
    float vals[MAX_PREDEF_VARS];
    char  names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX];
    int   count;
} ReplPredefSnapshot;

/* Read-only snapshot of the current predefined-variable table. The pointer is
 * stable until the next declare/undeclare/restore call; do not cache it across
 * frames. */
ReplPredefView repl_eval_predef_view(void);

typedef struct {
    const char (*names)[REPL_FUNC_NAME_MAX];
    int count;
} ReplFuncAliasView;

/* Function-alias table. Slot index 0..REPL_FUNC_SLOT_COUNT-1; an empty
 * string at a slot means "no alias, use bare funcN". Lookups never
 * match an empty alias. The table is REPL state and round-trips
 * through capture/restore + workspace import/export. */
void repl_func_alias_bind_storage(
    char arrays[REPL_FUNC_SLOT_COUNT][REPL_FUNC_NAME_MAX]);
void repl_func_alias_clear_all(void);
const char *repl_func_alias_get(int slot);
int  repl_func_alias_lookup_slot(const char *name);
ReplFuncAliasView repl_func_alias_view(void);
/* Register `name` for `slot`. Returns 1 on success, 0 if the name is
 * empty / too long / reserved / already taken by a different slot, or
 * if slot is out of range. Pass NULL or "" to clear the slot. */
int  repl_func_alias_set(int slot, const char *name);
/* Shape: returns 1 if `name` is a syntactically valid alias and is
 * not reserved. Does not check whether it's already registered. */
int  repl_func_alias_name_is_valid(const char *name);
/* Find a free slot (alias[]==""), return its index or -1 if all 10
 * slots are taken. The caller still has to call repl_func_alias_set
 * to actually register a name. */
int  repl_func_alias_first_free_slot(void);

void repl_eval_bind_scratch_storage(
    float arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]);
void repl_eval_reset_scratch_arrays(void);
int  repl_eval_scratch_array_index(const char *name);
int  repl_eval_scratch_get(int array_idx, int elem_idx, float *out);
int  repl_eval_scratch_set(int array_idx, int elem_idx, float value);
void repl_eval_copy_scratch_arrays(
    float dst[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]);
void repl_eval_restore_scratch_arrays(
    const float src[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]);

/* Copy / restore the predef-variable table (names + values + live
 * count) into / from caller-owned arrays sized to MAX_PREDEF_VARS.
 * Mirrors the scratch-array helpers above so undo / replay can
 * snapshot the predef storage without naming g_predef_vars /
 * g_num_predef_vars directly. */
void repl_eval_copy_predef_vars(float dst_vals[MAX_PREDEF_VARS],
                                char dst_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX],
                                int *dst_count);
void repl_eval_restore_predef_vars(const float src_vals[MAX_PREDEF_VARS],
                                   const char src_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX],
                                   int src_count);
void repl_eval_capture_predef_snapshot(ReplPredefSnapshot *dst);

/* Non-destructive value-only restore that pairs saved values with the
 * live predef slots whose names match the snapshot. The live table's
 * count / names / slot order stay UNCHANGED — only values for names
 * present in both snapshot and live table are updated. Saved names
 * no longer in the live table are silently dropped.
 *
 * The fade-batch render path uses this to revert values to the
 * replay-start baseline before each batch without reshaping the live
 * table mid-frame (which would break the surrounding frame-level
 * values-only save/restore). The full-replacement restore above is
 * for callers that legitimately want to clobber the live table
 * shape (e.g. an undo). */
void repl_eval_restore_predef_values_by_name(
    const float src_vals[MAX_PREDEF_VARS],
    const char src_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX],
    int src_count);
void repl_eval_restore_predef_values_by_snapshot(const ReplPredefSnapshot *src);

/* Values-only snapshot of the live predef table (no names, no count).
 * Used by the controller per-frame baseline-save and by the replay peer
 * for start/stop bracketing. For the full name+value+count snapshot used
 * by undo/scene-switch, see repl_eval_copy_predef_vars above. */
void repl_copy_predef_values(float *dst, int max_vals);
void repl_restore_predef_values(const float *src, int max_vals);

void repl_eval_init_predef_vars(void);
const char *repl_eval_eat_identifier(const char *p, const char **out_start);
int  repl_eval_is_builtin_function(const char *name);
/* Query: does input contain references to runtime-bound values (predef vars or
 * scratch arrays), not just literals? */
int  repl_eval_input_has_predef_vars(const char *s);
/* Look up a variable by name; returns index in g_predef_vars, or -1 if not found. */
int  repl_eval_find_predef_var_idx(const char *name);
/* Declare a new predefined variable; returns 1 on success, 0 on
 * duplicate/error. Errors are written to the err buffer (errsz bytes).
 * Callers that need the slot index should use the _with_value variant
 * below — this one drops it. */
int  repl_eval_declare_predef_var(const char *name, char *err, int errsz);
/* Declare a new predefined variable with a starting value; returns the
 * new slot index, or -1 on duplicate/error. Errors are written to the
 * err buffer (errsz bytes). */
int  repl_eval_declare_predef_var_with_value(const char *name, float value, char *err, int errsz);
/* Unregister a predefined variable by name (safe no-op if not found). */
void repl_eval_undeclare_predef_var(const char *name);
/* Check whether a name is reserved (e.g. a function name or keyword). */
int  repl_eval_is_reserved_ident(const char *name);
/* Scan source for references to a specific identifier. */
int  repl_eval_source_uses_ident(const char *src, const char *name);
/* Validate that all identifiers in source are either known variables or reserved
 * functions. Errors written to err buffer (errsz bytes). Returns 1 on success. */
int  repl_eval_validate_expression_idents(const char *src, const ExprVar *vars,
                                          int num_vars, char *err, int errsz);

/* ---- Expression evaluator (recursive descent) -------------------------- */

/* Parse and evaluate a single expression from ctx->p. Advances ctx->p past
 * the expression; returns the computed float value. Unknown identifiers
 * evaluate to 0.0f; diagnostics are written to ctx->err when set, not
 * to stderr. */
float repl_eval_expr(ExprCtx *ctx);

/* Parse a comma-separated list of expressions from string s. Evaluates each
 * using the provided variables. Returns the number of expressions parsed
 * (up to max). */
int   repl_eval_parse_exprs(const char *s, float *out, int max,
                            const ExprVar *vars, int num_vars);

/* Evaluate the parenthesized condition expression on an `if (...)` source
 * line. Extracts the paren payload via repl_extract_paren_payload, runs
 * it through repl_eval_c_expr_to_repl, then through repl_eval_expr with
 * the provided variable scope. Returns `fallback` when the extract step
 * fails (treat as "no eval performed"). Two callers exercise this with
 * different inputs and at different times — flatten_if_block evaluates
 * at flatten time to decide whether to unroll the body into the flat
 * program; the executor's CMD_IF_BEGIN handler re-evaluates per-iteration
 * so goto loops back into the body see updated vars. The kernel is
 * identical, so a shared helper keeps the two sides from drifting. */
float repl_eval_if_condition(const char *src_text,
                             const ExprVar *vars, int num_vars,
                             float fallback);

/* Advance past one comma-separated argument and return the pointer to the
 * next top-level `,`, `)`, or `\0`. Treats nested `(...)` as a unit so
 * args containing function calls or grouped sub-expressions are spanned
 * whole.
 *
 * Use this anywhere you'd reach for strchr(s, ',') to split a function
 * call's args — strchr is paren-naive and finds inner commas inside
 * `max(a, b)` etc. Caller advances past the returned delimiter to start
 * the next slot. */
const char *repl_scan_next_arg_delim(const char *s);

/* Given p pointing INSIDE an open paren (i.e., one past `(`), advance to
 * the matching `)`. Internal commas at the same paren depth are treated
 * as part of the payload (use repl_scan_next_arg_delim for arg-splitting
 * instead). Returns p at the matching `)` on success, or at the
 * end-of-string when the paren was unmatched — callers test `*p == ')'`
 * to distinguish. */
const char *repl_scan_to_matching_paren(const char *p);

/* Return a pointer to the start of `s`'s trailing line comment (the "//"
 * itself), or NULL if there is none. Skips "//" inside a double-quoted
 * string so a label("a // b") format argument is not mistaken for a
 * comment. Used to preserve a command's trailing `// ...` across the
 * canonical-text regeneration paths (commit / reformat / swatch / export
 * to C). */
const char *repl_line_trailing_comment(const char *s);

/* Append `source`'s trailing line comment (if any) to `dst`, separated by
 * a single space, trimming the comment's own trailing whitespace. No-op
 * when `source` has no trailing comment or `dst` is already full. Safe to
 * call when `dst` already ends in the canonical command text. */
void repl_append_trailing_comment(char *dst, size_t dst_sz, const char *source);

/* 1 if `line`'s trailing comment carries a whole-token `@tune` marker (the
 * tag that promotes a float decl to an exported keyboard knob), else 0.
 * Token-bounded: matches `// @tune` but NOT `// @tuned=5`. Pure string
 * predicate — no command/state access (keeps eval a leaf). */
int repl_eval_line_has_tune_tag(const char *line);

/* ---- Inline numeric swatch helpers ------------------------------------- */

typedef struct {
    int   found;       /* 1 if cursor sits in a pure numeric literal arg */
    int   arg_start;   /* offset of first char (incl. leading sign) */
    int   arg_end;     /* offset one past last digit/decimal/exponent */
    float value;       /* parsed via repl_eval_expr */
} ReplNumericArgAtCursor;

ReplNumericArgAtCursor repl_eval_numeric_arg_at_cursor(const char *src,
                                                       int cursor);
float repl_eval_swatch_step(float value);
void  repl_eval_format_swatch_number(float v, char *out, int out_sz);

/* ---- Expression translation: REPL <-> C syntax ----------------------- */

/* Translate REPL expression (sin, cos, etc.) to C (sinf, cosf, etc.).
 * Used internally when parsing C-style expressions. */
void repl_eval_expr_to_c(const char *in, char *out, int out_sz);

/* Translate C expression (sinf, cosf, etc.) back to REPL (sin, cos, etc.)
 * for display in the code panel. */
void repl_eval_c_expr_to_repl(const char *in, char *out, int out_sz);

/* ---- For-loop header parsers ------------------------------------------ */

/* Parse REPL for-loop header: for(var, start, end[, step]) body
 * All expressions are evaluated using available variables.
 * Returns 1 on success; *body_start points past the closing ')'.
 * Step defaults to 1.0 if omitted. */
int repl_eval_parse_for_header(const char *input, char *var_name, int var_sz,
                                float *start, float *end, float *step,
                                const char **body_start);
int repl_eval_parse_for_header_with_vars(const char *input, char *var_name, int var_sz,
                                          float *start, float *end, float *step,
                                          const ExprVar *vars, int num_vars,
                                          const char **body_start);

/* Parse C for-loop header: for (float var = start; var < end; var += step) {
 * Returns 1 on success. Expressions are constant-folded (vars not supported). */
int repl_eval_parse_c_for_header(const char *input, char *var_name, int var_sz,
                                  float *start, float *end, float *step);

#endif /* REPL_EVAL_H */
