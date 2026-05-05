/*
 * repl_eval.h - Expression evaluator, translators, and for-loop parsers.
 *
 * Recursive-descent expression evaluator for the REPL math layer. Supports
 * binary operators (+, -, *, /, %, ^), unary minus, parentheses, function
 * calls (sin, cos, tan, sqrt, abs, pow, min, max, floor, ceil, fmod, rand),
 * and constants (PI, TAU). All values are floats.
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
 * math syntax. Loop expansion and unrolling happen in repl_flatten.c.
 *
 * This module is extracted for independent testing. Include this header, then:
 *   - #include "repl_eval.c" for single-TU builds, or
 *   - link with repl_eval.c compiled separately
 */
#ifndef REPL_EVAL_H
#define REPL_EVAL_H

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MAX_LINE_LEN
#define MAX_LINE_LEN 256
#endif

/*
 *  MAX_EXPR_VARS - global user-declared variables
 *  -------------
 *  This sizes the predefined-variable table populated by float t;, float radius;, etc. It's the global identifier table the user sees.
 *
 *  - Storage: g_predef_vars[MAX_PREDEF_VARS] (in repl_eval.c).
 *  - One slot is reserved for the built-in t (animation time).
 *  - 15 slots remain for user float name; declarations.
 *  - The try_commit_float_decl handler rejects new declarations once full with "variable table full (max 16)".
 *  The REPL UI shows all declared variables in the "Predefined Variables" section of the code panel, and they are available for use in expressions.
 *  Used in:
 *  - repl_eval.c — the table itself.
 *  - editor_undo.h — undo snapshots capture all 16 slots' names + values.
 *  - replay.c / imrepl_ctrl.c — replay baselines copy/restore all 16 values across mode transitions.
 *  - repl_state_views.h — the by-value snapshot the UI reads from.
 *
 *
 *  MAX_EXPR_VARS — local lexical scope at parse time
 *  -------------
 *  This sizes the per-expression visible-variable list built when compiling a
 *  single expression. It's a lexical scope handed to repl_eval so it can
 *  resolve identifiers like i, j, function parameters, and predef vars at
 *  evaluation time without taking out a global lock.
 *
 *  - Storage: per-call stack arrays — ExprVar vis[MAX_EXPR_VARS] in repl_compile.c, ExprVar vars[MAX_EXPR_VARS] in repl_flatten.h's FlatScope, params[MAX_EXPR_VARS] in autocomplete hint generation.
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
 *  The values are equal today only because the worst-case scope is "all 16
 *  predef vars visible plus zero locals" (the for-loop body inside zero nested
 *  scopes). But they're not the same constraint:
 *
 *  - MAX_EXPR_VARS actually needs to be MAX_PREDEF_VARS + worst-case nested
 *    locals to be fully correct. With 8-deep nested for-loops each declaring a
 *    fresh iterator name, you'd exceed 16 visible names. In
 *    practice the parser truncates the visible-list silently because the limit happens to be 16. (Not a great property, just hidden by the equal value.)
 *  - MAX_PREDEF_VARS is the user-facing "how many float x; can I have" limit.
 */
#ifndef MAX_EXPR_VARS
#define MAX_EXPR_VARS 24
#endif
#ifndef MAX_PREDEF_VARS
#define MAX_PREDEF_VARS 16
#endif

#ifndef MAX_NAMES_PER_DECL
#define MAX_NAMES_PER_DECL 8
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Types ------------------------------------------------------------ */

/* A single variable name/value pair in the expression context. */
typedef struct {
    char  name[16];
    float value;
} ExprVar;

/* Parser context: tracks current position in input string and available
 * variables. Used internally by repl_eval_expr() during recursive descent. */
typedef struct {
    const char *p;          /* current position in string */
    const ExprVar *vars;
    int         num_vars;
} ExprCtx;

/* ---- Predefined variables (global scope) ------------------------------- */

/* Compatibility access to the predefined-variable table. In the REPL binary
 * these names resolve to ReplRuntimeState.variables; standalone evaluator
 * tests use repl_eval.c's fallback storage. */
ExprVar *repl_eval_predef_vars_mut(void);
int     *repl_eval_predef_count_mut(void);
void     repl_eval_bind_predef_storage(ExprVar *vars, int *count_ptr);

#define g_predef_vars     (repl_eval_predef_vars_mut())
#define g_num_predef_vars (*repl_eval_predef_count_mut())

typedef struct {
    const ExprVar *vars;
    int            count;
} ReplPredefView;

/* Read-only snapshot of the current predefined-variable table. The pointer is
 * stable until the next declare/undeclare/restore call; do not cache it across
 * frames. */
ReplPredefView repl_eval_predef_view(void);

void repl_eval_init_predef_vars(void);
/* Query: does input contain references to predefined variables (not just literals)? */
int  repl_eval_input_has_predef_vars(const char *s);
/* Look up a variable by name; returns index in g_predef_vars, or -1 if not found. */
int  repl_eval_find_predef_var_idx(const char *name);
/* Declare a new predefined variable; returns index, or -1 on duplicate/error.
 * Errors are written to the err buffer (errsz bytes). */
int  repl_eval_declare_predef_var(const char *name, char *err, int errsz);
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
 * the expression; returns the computed float value. Errors logged to stderr. */
float repl_eval_expr(ExprCtx *ctx);

/* Parse a comma-separated list of expressions from string s. Evaluates each
 * using the provided variables. Returns the number of expressions parsed
 * (up to max), -1 on error. */
int   repl_eval_parse_exprs(const char *s, float *out, int max,
                            ExprVar *vars, int num_vars);

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
                                          ExprVar *vars, int num_vars,
                                          const char **body_start);

/* Parse C for-loop header: for (float var = start; var < end; var += step) {
 * Returns 1 on success. Expressions are constant-folded (vars not supported). */
int repl_eval_parse_c_for_header(const char *input, char *var_name, int var_sz,
                                  float *start, float *end, float *step);

#endif /* REPL_EVAL_H */
