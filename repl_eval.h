/*
 * repl_eval.h — Expression evaluator, translators, and for-loop parsers
 *
 * Extracted from the REPL for independent testing.
 * Include this header, then either:
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

#ifndef MAX_EXPR_VARS
#define MAX_EXPR_VARS 16
#endif

#ifndef MAX_PREDEF_VARS
#define MAX_PREDEF_VARS 11
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Types ------------------------------------------------------------ */

typedef struct {
    char  name[16];
    float value;
} ExprVar;

typedef struct {
    const char *p;          /* current position in string */
    ExprVar    *vars;
    int         num_vars;
} ExprCtx;

/* ---- Predefined variables --------------------------------------------- */

extern ExprVar g_predef_vars[MAX_PREDEF_VARS];
extern int     g_num_predef_vars;

void init_predef_vars(void);
int  input_has_predef_vars(const char *s);

/* ---- Expression evaluator --------------------------------------------- */

float eval_expr(ExprCtx *ctx);
int   parse_exprs(const char *s, float *out, int max,
                  ExprVar *vars, int num_vars);

/* ---- Expression translation: REPL <-> C ------------------------------- */

void repl_expr_to_c(const char *in, char *out, int out_sz);
void c_expr_to_repl(const char *in, char *out, int out_sz);

/* ---- For-loop header parsers ------------------------------------------ */

/* REPL form: for(var, start, end[, step]) body
 * Returns 1 on success.  *body_start points past ')'. */
int parse_for_header(const char *input, char *var_name, int var_sz,
                     float *start, float *end, float *step,
                     const char **body_start);
int parse_for_header_with_vars(const char *input, char *var_name, int var_sz,
                               float *start, float *end, float *step,
                               ExprVar *vars, int num_vars,
                               const char **body_start);

/* C form: for (float var = start; var < end; var += step) {
 * Returns 1 on success. */
int parse_c_for_header(const char *input, char *var_name, int var_sz,
                       float *start, float *end, float *step);

#endif /* REPL_EVAL_H */
