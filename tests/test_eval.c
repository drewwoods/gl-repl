/*
 * test_eval.c - Standalone test harness for the REPL expression evaluator
 *
 * Build:
 *   gcc -Wall -std=c2x -I. -o test_eval tests/test_eval.c src/repl/eval.c -lm
 *
 * Usage:
 *   ./test_eval                    # interactive REPL
 *   ./test_eval --run-tests        # run built-in test suite
 *   ./test_eval --rand-dist [N]    # print the rand() uniformity table (N samples)
 *
 * Interactive commands:
 *   <expr>                         eval expression: 1+2, sin(PI/4), x*2+1
 *   set <var> <value>              set predefined var: set x 1.5
 *   to_c <expr>                    translate REPL->C: to_c sin(TAU/n)
 *   to_repl <expr>                 translate C->REPL: to_repl sinf(M_PI)
 *   for <header>                   parse REPL for:    for for(i, 0, n)
 *   cfor <header>                  parse C for:       cfor for (float i = 0; i < 10; i += 1.0f) {
 *   vars                           show all predefined vars
 *   randdist [N]                   rand() uniformity table (N samples, default 100000)
 *   quit / q                       exit
 */
#include "repl/eval.h"

#include "support/test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* ---- Built-in test suite ---------------------------------------------- */

static TestHarness g_harness = TEST_HARNESS_INIT;

/* Defined alongside the rand() distribution table below; run_tests() uses
 * it for the uniformity regression check. */
static double rand_dist_chisq(int seed_is_i, int C, long n);

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, (label), (cond))

#define ASSERT_FLOAT(expr_str, expected) do { \
    ExprCtx _ctx = { (expr_str), NULL, 0 }; \
    TEST_ASSERT_FLOAT(&g_harness, (expr_str), repl_eval_expr(&_ctx), (expected), 1e-4f); \
} while(0)

#define ASSERT_EXPRS(expr_str, n_expected, ...) do { \
    char _label[128]; \
    float _vals[8]; \
    int _n = repl_eval_parse_exprs(expr_str, _vals, 8, NULL, 0); \
    float _exp[] = { __VA_ARGS__ }; \
    int _ok = (_n == (n_expected)); \
    if (_ok) { \
        for (int _i = 0; _i < _n; _i++) { \
            if (fabsf(_vals[_i] - _exp[_i]) > 1e-4f) { \
                _ok = 0; \
                break; \
            } \
        } \
    } \
    snprintf(_label, sizeof(_label), "parse_exprs: %s", (expr_str)); \
    ASSERT_TRUE(_label, _ok); \
} while(0)

#define ASSERT_TO_C(in, expected) do { \
    char _label[128]; \
    char _buf[512]; \
    repl_eval_expr_to_c(in, _buf, sizeof(_buf)); \
    snprintf(_label, sizeof(_label), "to_c: %s", (in)); \
    TEST_ASSERT_STR(&g_harness, _label, _buf, (expected)); \
} while(0)

#define ASSERT_TO_REPL(in, expected) do { \
    char _label[128]; \
    char _buf[512]; \
    repl_eval_c_expr_to_repl(in, _buf, sizeof(_buf)); \
    snprintf(_label, sizeof(_label), "to_repl: %s", (in)); \
    TEST_ASSERT_STR(&g_harness, _label, _buf, (expected)); \
} while(0)

#define ASSERT_FOR(for_input, expect_ok, e_var, e_start, e_end, e_step) do { \
    char _label[128]; \
    char _vn[16]; float _s, _e, _st; const char *_b; \
    int _ok = repl_eval_parse_for_header(&(ReplForHeaderParseConfig){ \
        .input = (for_input), \
        .var_name = _vn, \
        .var_sz = (int)sizeof(_vn), \
        .start = &_s, \
        .end = &_e, \
        .step = &_st, \
        .body_start = &_b, \
    }); \
    int _match = (_ok == (expect_ok)); \
    if (_match && _ok) { \
        _match = (strcmp(_vn, (e_var)) == 0 && \
                  fabsf(_s - (e_start)) <= 1e-4f && \
                  fabsf(_e - (e_end)) <= 1e-4f && \
                  fabsf(_st - (e_step)) <= 1e-4f); \
    } \
    snprintf(_label, sizeof(_label), "for header: %s", (for_input)); \
    ASSERT_TRUE(_label, _match); \
} while(0)

#define ASSERT_CFOR(input, expect_ok, e_var, e_start, e_end, e_step) do { \
    char _label[128]; \
    char _vn[16]; float _s, _e, _st; \
    int _ok = repl_eval_parse_c_for_header(input, _vn, sizeof(_vn), &_s, &_e, &_st); \
    int _match = (_ok == (expect_ok)); \
    if (_match && _ok) { \
        _match = (strcmp(_vn, (e_var)) == 0 && \
                  fabsf(_s - (e_start)) <= 1e-4f && \
                  fabsf(_e - (e_end)) <= 1e-4f && \
                  fabsf(_st - (e_step)) <= 1e-4f); \
    } \
    snprintf(_label, sizeof(_label), "cfor header: %s", (input)); \
    ASSERT_TRUE(_label, _match); \
} while(0)

#define ASSERT_HAS_VARS(input, expected) do { \
    char _label[128]; \
    int _actual = repl_eval_input_has_predef_vars(input); \
    snprintf(_label, sizeof(_label), "has_vars: %s", (input)); \
    TEST_ASSERT_INT(&g_harness, _label, _actual, (expected)); \
} while(0)

#define ASSERT_DECLARE_OK(name) do { \
    char _label[96]; \
    char _err[128] = {0}; \
    int _ok = repl_eval_declare_predef_var(name, _err, sizeof(_err)); \
    (void)_err; \
    snprintf(_label, sizeof(_label), "declare ok: %s", (name)); \
    ASSERT_TRUE(_label, _ok); \
} while(0)

#define ASSERT_DECLARE_FAIL(name) do { \
    char _label[96]; \
    char _err[128] = {0}; \
    int _ok = repl_eval_declare_predef_var(name, _err, sizeof(_err)); \
    snprintf(_label, sizeof(_label), "declare fail: %s", (name)); \
    ASSERT_TRUE(_label, !_ok); \
} while(0)

#define ASSERT_VALIDATE_OK(expr_src, vars_arg, nv_arg) do { \
    char _label[128]; \
    char _err[128] = {0}; \
    int _ok = repl_eval_validate_expression_idents( \
        &(ReplExprIdentValidationConfig){ \
            .src = (expr_src), \
            .vars = (vars_arg), \
            .num_vars = (nv_arg), \
            .predef = repl_eval_predef_view(), \
            .err = _err, \
            .errsz = (int)sizeof(_err), \
        }); \
    (void)_err; \
    snprintf(_label, sizeof(_label), "validate ok: %s", (expr_src)); \
    ASSERT_TRUE(_label, _ok); \
} while(0)

#define ASSERT_VALIDATE_FAIL(expr_src, vars_arg, nv_arg) do { \
    char _label[128]; \
    char _err[128] = {0}; \
    int _ok = repl_eval_validate_expression_idents( \
        &(ReplExprIdentValidationConfig){ \
            .src = (expr_src), \
            .vars = (vars_arg), \
            .num_vars = (nv_arg), \
            .predef = repl_eval_predef_view(), \
            .err = _err, \
            .errsz = (int)sizeof(_err), \
        }); \
    snprintf(_label, sizeof(_label), "validate fail: %s", (expr_src)); \
    ASSERT_TRUE(_label, !_ok); \
} while(0)

#define ASSERT_SOURCE_USES(src, name, expected) do { \
    char _label[160]; \
    int _actual = repl_eval_source_uses_ident(src, name); \
    snprintf(_label, sizeof(_label), "source_uses(%s,%s)", (src), (name)); \
    TEST_ASSERT_INT(&g_harness, _label, _actual, (expected)); \
} while(0)

static int predef_idx(const char *name) {
    for (int i = 0; i < g_num_predef_vars; i++) {
        if (strcmp(g_predef_vars[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void set_predef(const char *name, float val) {
    int idx = predef_idx(name);
    if (idx < 0) {
        printf("  BUG: set_predef(\"%s\") - var not declared\n", name);
        return;
    }
    g_predef_vars_mut[idx].value = val;
}

static void strip_ws(const char *in, char *out, int out_sz) {
    int oi = 0;
    if (!in || !out || out_sz <= 0)
        return;
    for (int i = 0; in[i] && oi < out_sz - 1; i++) {
        if (!isspace((unsigned char)in[i]))
            out[oi++] = in[i];
    }
    out[oi] = '\0';
}

#define ASSERT_ROUNDTRIP_WS(in) do { \
    char _label[128]; \
    char _cbuf[512], _rbuf[512], _n_in[512], _n_out[512]; \
    repl_eval_expr_to_c((in), _cbuf, sizeof(_cbuf)); \
    repl_eval_c_expr_to_repl(_cbuf, _rbuf, sizeof(_rbuf)); \
    strip_ws((in), _n_in, sizeof(_n_in)); \
    strip_ws(_rbuf, _n_out, sizeof(_n_out)); \
    snprintf(_label, sizeof(_label), "roundtrip_ws: %s", (in)); \
    TEST_ASSERT_STR(&g_harness, _label, _n_out, _n_in); \
} while(0)

static void run_tests(void) {
    printf("Running tests...\n\n");

    /* ---- Expression evaluator ---- */
    printf("Expression evaluator:\n");
    ASSERT_FLOAT("42", 42.0f);
    ASSERT_FLOAT("1+2", 3.0f);
    ASSERT_FLOAT("3-1", 2.0f);
    ASSERT_FLOAT("2*3", 6.0f);
    ASSERT_FLOAT("10/4", 2.5f);
    ASSERT_FLOAT("2+3*4", 14.0f);
    ASSERT_FLOAT("(2+3)*4", 20.0f);
    ASSERT_FLOAT("-5", -5.0f);
    ASSERT_FLOAT("--5", 5.0f);
    ASSERT_FLOAT("+5", 5.0f);
    ASSERT_FLOAT("1.5+0.5", 2.0f);
    ASSERT_FLOAT("1.5f+0.5f", 2.0f);
    ASSERT_FLOAT("PI", (float)M_PI);
    ASSERT_FLOAT("TAU", (float)(2.0 * M_PI));
    {
        ExprCtx ctx = { "NAN", NULL, 0 };
        ASSERT_TRUE("NAN constant", isnan(repl_eval_expr(&ctx)));
    }
    {
        ExprCtx ctx = { "nan", NULL, 0 };
        ASSERT_TRUE("nan constant", isnan(repl_eval_expr(&ctx)));
    }
    {
        ExprCtx ctx = { "INFINITY", NULL, 0 };
        float value = repl_eval_expr(&ctx);
        ASSERT_TRUE("INFINITY constant", isinf(value) && value > 0.0f);
    }
    {
        ExprCtx ctx = { "inf", NULL, 0 };
        float value = repl_eval_expr(&ctx);
        ASSERT_TRUE("inf constant", isinf(value) && value > 0.0f);
    }
    ASSERT_FLOAT("sin(0)", 0.0f);
    ASSERT_FLOAT("cos(0)", 1.0f);
    ASSERT_FLOAT("tan(0)", 0.0f);
    ASSERT_FLOAT("asin(0)", 0.0f);
    ASSERT_FLOAT("asin(1)", (float)M_PI / 2.0f);
    ASSERT_FLOAT("asin(-1)", -(float)M_PI / 2.0f);
    ASSERT_FLOAT("asin(2)", (float)M_PI / 2.0f);    /* clamped, never NaN */
    ASSERT_FLOAT("asin(-2)", -(float)M_PI / 2.0f);
    ASSERT_FLOAT("acos(1)", 0.0f);
    ASSERT_FLOAT("acos(0)", (float)M_PI / 2.0f);
    ASSERT_FLOAT("acos(-1)", (float)M_PI);
    ASSERT_FLOAT("acos(5)", 0.0f);                  /* clamped, never NaN */
    ASSERT_FLOAT("acos(-5)", (float)M_PI);
    ASSERT_FLOAT("atan(0)", 0.0f);
    ASSERT_FLOAT("atan(1)", (float)M_PI / 4.0f);
    ASSERT_FLOAT("atan(-1)", -(float)M_PI / 4.0f);
    ASSERT_FLOAT("tan(atan(0.75))", 0.75f);           /* round-trip */
    ASSERT_FLOAT("atan2(0,1)", 0.0f);
    ASSERT_FLOAT("atan2(1,0)", (float)M_PI / 2.0f); /* +Y axis */
    ASSERT_FLOAT("atan2(1,1)", (float)M_PI / 4.0f);
    ASSERT_FLOAT("atan2(-1,-1)", -3.0f * (float)M_PI / 4.0f); /* quadrant from both signs */
    ASSERT_FLOAT("atan2(0,-1)", (float)M_PI);
    ASSERT_FLOAT("atan2(0,0)", 0.0f);               /* degenerate, but total */
    ASSERT_FLOAT("atan2(1)", 0.0f);                 /* arity 2 required */
    ASSERT_FLOAT("sin(asin(0.5))", 0.5f);           /* round-trip */
    ASSERT_FLOAT("cos(acos(0.25))", 0.25f);
    ASSERT_FLOAT("atan2(sin(1), cos(1))", 1.0f);    /* angle recovered from a point */
    ASSERT_FLOAT("sqrt(4)", 2.0f);
    ASSERT_FLOAT("abs(-3)", 3.0f);
    ASSERT_FLOAT("pow(2,3)", 8.0f);
    ASSERT_FLOAT("min(3,5)", 3.0f);
    ASSERT_FLOAT("clamp(5,0,1)", 1.0f);
    ASSERT_FLOAT("clamp(-5,0,1)", 0.0f);
    ASSERT_FLOAT("clamp(0.25,0,1)", 0.25f);
    ASSERT_FLOAT("clamp(3,-2,-1)", -1.0f);
    ASSERT_FLOAT("clamp(0,5,1)", 5.0f);       /* crossed bounds: lo wins */
    ASSERT_FLOAT("clamp(1,2)", 0.0f);         /* arity 3 required */
    ASSERT_FLOAT("lerp(0,10,0)", 0.0f);
    ASSERT_FLOAT("lerp(0,10,1)", 10.0f);
    ASSERT_FLOAT("lerp(0,10,0.25)", 2.5f);
    ASSERT_FLOAT("lerp(0,10,2)", 20.0f);      /* unclamped: extrapolates */
    ASSERT_FLOAT("lerp(0,10,-1)", -10.0f);
    ASSERT_FLOAT("lerp(-4,4,0.5)", 0.0f);
    ASSERT_FLOAT("smoothstep(0,1,0)", 0.0f);
    ASSERT_FLOAT("smoothstep(0,1,1)", 1.0f);
    ASSERT_FLOAT("smoothstep(0,1,0.5)", 0.5f);
    ASSERT_FLOAT("smoothstep(0,1,-3)", 0.0f);  /* clamped below */
    ASSERT_FLOAT("smoothstep(0,1,7)", 1.0f);   /* clamped above */
    ASSERT_FLOAT("smoothstep(2,4,3)", 0.5f);   /* midpoint of any span */
    ASSERT_FLOAT("smoothstep(1,1,0.5)", 0.0f); /* zero span -> step */
    ASSERT_FLOAT("smoothstep(1,1,2)", 1.0f);
    ASSERT_FLOAT("smoothstep(0,1,0.25)", 0.15625f); /* u*u*(3-2u) */
    ASSERT_FLOAT("sign(3.5)", 1.0f);
    ASSERT_FLOAT("sign(-3.5)", -1.0f);
    ASSERT_FLOAT("sign(0)", 0.0f);
    ASSERT_FLOAT("sign(-0)", 0.0f);
    ASSERT_FLOAT("max(3,5)", 5.0f);
    ASSERT_FLOAT("floor(2.7)", 2.0f);
    ASSERT_FLOAT("ceil(2.3)", 3.0f);
    ASSERT_FLOAT("round(2.3)", 2.0f);
    ASSERT_FLOAT("round(2.7)", 3.0f);
    ASSERT_FLOAT("fmod(7,3)", 1.0f);
    ASSERT_FLOAT("rem(7,3)", remainderf(7.0f, 3.0f));   /* IEEE round-to-nearest */
    ASSERT_FLOAT("rem(8,3)", remainderf(8.0f, 3.0f));   /* -1, not 2 */
    ASSERT_FLOAT("rem(-7,3)", remainderf(-7.0f, 3.0f));
    ASSERT_FLOAT("log(100)", 2.0f);          /* base-10 log */
    ASSERT_FLOAT("log(10)", 1.0f);
    ASSERT_FLOAT("log(1)", 0.0f);
    ASSERT_FLOAT("ln(1)", 0.0f);             /* natural log */
    ASSERT_FLOAT("ln(e)", 1.0f);             /* ln(e) = 1 by definition */
    ASSERT_FLOAT("e", (float)M_E);           /* Euler's number */
    ASSERT_FLOAT("rand(7,11)", 0.233398f);
    ASSERT_FLOAT("rand(7,11)", 0.233398f); /* deterministic */
    ASSERT_FLOAT("rand(3)", 0.164062f);    /* implicit iter=0 */
    ASSERT_FLOAT("rand(3,1,2)", 0.0f);     /* extra args rejected */
    ASSERT_FLOAT("rand2(3)", -0.671875f);  /* implicit iter=0 */
    ASSERT_FLOAT("rand2(3,1,2)", 0.0f);    /* extra args rejected */
    ASSERT_FLOAT("rand(0,0)", 0.282227f);  /* seed offset keeps seed 0 off sin()'s zero */
    ASSERT_FLOAT("sin(PI/2)", 1.0f);
    ASSERT_FLOAT("cos(TAU)", 1.0f);
    ASSERT_FLOAT("10/0", 0.0f);  /* div by zero returns 0 */
    ASSERT_FLOAT("10%3", 1.0f);
    ASSERT_FLOAT("10%0", 0.0f);  /* mod by zero returns 0 */

    /* Logical and comparison */
    ASSERT_FLOAT("1 == 1", 1.0f);
    ASSERT_FLOAT("1 == 0", 0.0f);
    ASSERT_FLOAT("1 != 0", 1.0f);
    ASSERT_FLOAT("1 != 1", 0.0f);
    ASSERT_FLOAT("5 > 3", 1.0f);
    ASSERT_FLOAT("3 > 5", 0.0f);
    ASSERT_FLOAT("5 >= 5", 1.0f);
    ASSERT_FLOAT("5 >= 6", 0.0f);
    ASSERT_FLOAT("3 < 5", 1.0f);
    ASSERT_FLOAT("5 < 3", 0.0f);
    ASSERT_FLOAT("5 <= 5", 1.0f);
    ASSERT_FLOAT("4 <= 5", 1.0f);
    ASSERT_FLOAT("6 <= 5", 0.0f);
    ASSERT_FLOAT("!1", 0.0f);
    ASSERT_FLOAT("!0", 1.0f);
    ASSERT_FLOAT("1 && 1", 1.0f);
    ASSERT_FLOAT("1 && 0", 0.0f);
    ASSERT_FLOAT("0 && 1", 0.0f);
    ASSERT_FLOAT("0 && 0", 0.0f);
    ASSERT_FLOAT("1 || 1", 1.0f);
    ASSERT_FLOAT("1 || 0", 1.0f);
    ASSERT_FLOAT("0 || 1", 1.0f);
    ASSERT_FLOAT("0 || 0", 0.0f);
    ASSERT_FLOAT("unknown_var", 0.0f);
    ASSERT_FLOAT("unknown_func(1,2)", 0.0f);

    printf("predefined vars:\n");
    {
        int t_idx;
        char err[128];
        repl_eval_init_predef_vars();

        t_idx = predef_idx("t");
        ASSERT_TRUE("predefined var t exists", t_idx >= 0);
        ASSERT_TRUE("predefined var t starts at zero",
                    t_idx >= 0 && fabsf(g_predef_vars[t_idx].value - 0.0f) < 1e-6f);
        ASSERT_TRUE("init_predef_vars registers only t", g_num_predef_vars == 1);
        ASSERT_TRUE("unknown predefined var lookup returns -1",
                    predef_idx("not_a_predef") == -1);

        repl_eval_declare_predef_var("x", err, sizeof(err));
        repl_eval_declare_predef_var("y", err, sizeof(err));
        repl_eval_declare_predef_var("z", err, sizeof(err));
        repl_eval_declare_predef_var("n", err, sizeof(err));
        repl_eval_declare_predef_var("i", err, sizeof(err));
        repl_eval_declare_predef_var("j", err, sizeof(err));
        repl_eval_declare_predef_var("k", err, sizeof(err));

        ASSERT_TRUE("declared var x exists", predef_idx("x") >= 0);
        ASSERT_TRUE("declared var y exists", predef_idx("y") >= 0);
        ASSERT_TRUE("declared var z exists", predef_idx("z") >= 0);
        ASSERT_TRUE("declared var n exists", predef_idx("n") >= 0);
    }
    ASSERT_DECLARE_FAIL("NAN");
    ASSERT_DECLARE_FAIL("nan");
    ASSERT_DECLARE_FAIL("INFINITY");
    ASSERT_DECLARE_FAIL("inf");

    /* Variables */
    set_predef("x", 1.5f);
    set_predef("y", 2.5f);
    set_predef("z", 3.5f);
    set_predef("n", 24.0f);
    set_predef("t", 4.0f);
    ASSERT_FLOAT("x", 1.5f);
    ASSERT_FLOAT("y", 2.5f);
    ASSERT_FLOAT("z", 3.5f);
    ASSERT_FLOAT("n", 24.0f);
    ASSERT_FLOAT("t", 4.0f);
    ASSERT_FLOAT("x+1", 2.5f);
    ASSERT_FLOAT("x+y+z+t", 11.5f);
    ASSERT_FLOAT("n*2", 48.0f);
    ASSERT_FLOAT("TAU/n", (float)(2.0 * M_PI / 24.0));

    printf("Operator precedence (arithmetic):\n");
    /* '*' '/' '%' bind tighter than '+' '-' */
    ASSERT_FLOAT("2 + 3 * 4", 14.0f);
    ASSERT_FLOAT("3 * 4 + 2", 14.0f);
    ASSERT_FLOAT("20 - 6 / 2", 17.0f);
    ASSERT_FLOAT("20 / 5 - 1", 3.0f);
    ASSERT_FLOAT("10 + 7 % 3", 11.0f);
    ASSERT_FLOAT("7 % 3 + 10", 11.0f);
    ASSERT_FLOAT("2 + 3 * 4 + 5", 19.0f);
    ASSERT_FLOAT("1 * 2 + 3 * 4", 14.0f);
    ASSERT_FLOAT("2 + 3 * 4 - 5 / 5", 13.0f);
    ASSERT_FLOAT("100 - 4 * 5 + 2", 82.0f);
    /* '*' '/' '%' all share one level - strictly left-to-right */
    ASSERT_FLOAT("12 / 3 * 2", 8.0f);   /* not 2 - left-assoc, not right */
    ASSERT_FLOAT("12 * 2 / 3", 8.0f);
    ASSERT_FLOAT("20 % 7 * 2", 12.0f);  /* (20%7)*2 = 6*2 */
    ASSERT_FLOAT("2 * 20 % 7", 5.0f);   /* (2*20)%7 = 40%7 */
    ASSERT_FLOAT("100 / 2 / 5 / 2", 5.0f);
    /* '+' '-' left-to-right */
    ASSERT_FLOAT("10 - 5 - 2", 3.0f);   /* not 7 */
    ASSERT_FLOAT("10 - 5 + 2", 7.0f);
    ASSERT_FLOAT("10 + 5 - 2", 13.0f);
    ASSERT_FLOAT("1 - 2 - 3 - 4", -8.0f);
    ASSERT_FLOAT("1 + 2 - 3 + 4 - 5", -1.0f);

    printf("Operator precedence (unary):\n");
    /* Unary '-' '+' '!' bind tighter than '*' */
    ASSERT_FLOAT("-2 * 3", -6.0f);
    ASSERT_FLOAT("2 * -3", -6.0f);
    ASSERT_FLOAT("-2 * -3", 6.0f);
    ASSERT_FLOAT("-(2 + 3)", -5.0f);
    ASSERT_FLOAT("-(-3)", 3.0f);
    ASSERT_FLOAT("--3", 3.0f);          /* unary chain */
    ASSERT_FLOAT("---3", -3.0f);
    ASSERT_FLOAT("----3", 3.0f);
    ASSERT_FLOAT("-3 - -3", 0.0f);
    ASSERT_FLOAT("+5 + -3", 2.0f);
    ASSERT_FLOAT("!1 + 1", 1.0f);       /* (!1) + 1 = 0 + 1 */
    ASSERT_FLOAT("!0 * 5", 5.0f);       /* (!0) * 5 = 1 * 5 */
    ASSERT_FLOAT("!(1 + 0)", 0.0f);     /* paren forces order */
    ASSERT_FLOAT("!!1", 1.0f);
    ASSERT_FLOAT("!!0", 0.0f);
    ASSERT_FLOAT("!!!1", 0.0f);

    printf("Operator precedence (comparison):\n");
    /* Arithmetic binds tighter than comparison */
    ASSERT_FLOAT("1 + 2 < 5", 1.0f);    /* (1+2) < 5 */
    ASSERT_FLOAT("5 > 1 + 2", 1.0f);
    ASSERT_FLOAT("2 * 3 == 6", 1.0f);
    ASSERT_FLOAT("2 * 3 != 7", 1.0f);
    ASSERT_FLOAT("5 - 2 >= 3", 1.0f);
    ASSERT_FLOAT("5 - 2 <= 3", 1.0f);
    ASSERT_FLOAT("10 / 5 > 1", 1.0f);
    ASSERT_FLOAT("3 * 4 == 2 * 6", 1.0f);
    ASSERT_FLOAT("3 + 4 < 2 * 5", 1.0f);
    /* Chained comparisons evaluate left-to-right and produce 0/1 */
    ASSERT_FLOAT("5 > 3 > 0", 1.0f);    /* (5>3)=1, 1>0=1 */
    ASSERT_FLOAT("1 < 2 == 1", 1.0f);   /* (1<2)=1, 1==1=1 */
    ASSERT_FLOAT("0 == 0 == 1", 1.0f);  /* (0==0)=1, 1==1=1 */

    printf("Operator precedence (logical):\n");
    /* Comparison binds tighter than '&&' '||' */
    ASSERT_FLOAT("1 < 2 && 2 < 3", 1.0f);
    ASSERT_FLOAT("1 > 2 || 3 > 2", 1.0f);
    ASSERT_FLOAT("1 == 1 && 0 != 0", 0.0f);
    ASSERT_FLOAT("5 > 3 && 2 < 5 || 0", 1.0f);
    ASSERT_FLOAT("1 + 1 == 2 && 3 - 1 == 2", 1.0f);
    /* '&&' and '||' share one precedence level in this REPL - strictly
     * left-to-right. This differs from standard C where '&&' binds
     * tighter than '||'. Spot-check both flavors so any future grammar
     * change shows up loudly. */
    ASSERT_FLOAT("1 || 0 && 0", 0.0f);  /* REPL: (1||0)&&0 = 0; C: 1 */
    ASSERT_FLOAT("0 || 1 && 0", 0.0f);  /* REPL: (0||1)&&0 = 0; C: 0 */
    ASSERT_FLOAT("0 || 1 && 1", 1.0f);
    ASSERT_FLOAT("1 && 0 || 1", 1.0f);  /* REPL: (1&&0)||1 = 1; C: 1 */
    ASSERT_FLOAT("1 && 1 || 0", 1.0f);
    ASSERT_FLOAT("0 && 1 || 0", 0.0f);

    printf("Operator precedence (parenthesization):\n");
    ASSERT_FLOAT("(2 + 3) * 4", 20.0f);
    ASSERT_FLOAT("2 * (3 + 4)", 14.0f);
    ASSERT_FLOAT("(1 + 2) * (3 + 4)", 21.0f);
    ASSERT_FLOAT("((1 + 2) * 3) + 4", 13.0f);
    ASSERT_FLOAT("((((1))))", 1.0f);
    ASSERT_FLOAT("(((1 + 2))) * 3", 9.0f);
    ASSERT_FLOAT("(2 + 3) * (4 - 1)", 15.0f);
    ASSERT_FLOAT("-(2 + 3) * 2", -10.0f);
    ASSERT_FLOAT("(1 || 0) && 0", 0.0f);
    ASSERT_FLOAT("1 || (0 && 0)", 1.0f); /* parens recover C semantics */
    ASSERT_FLOAT("(1 + 2) == (4 - 1)", 1.0f);

    printf("Operator precedence (with variables):\n");
    /* x=1.5, y=2.5, z=3.5, n=24, t=4 from the variable setup above */
    ASSERT_FLOAT("x + y * z", 1.5f + 2.5f * 3.5f);     /* 10.25 */
    ASSERT_FLOAT("(x + y) * z", (1.5f + 2.5f) * 3.5f); /* 14.0 */
    ASSERT_FLOAT("n - t * 2", 16.0f);
    ASSERT_FLOAT("-x + y", 1.0f);
    ASSERT_FLOAT("x * -y", -3.75f);
    ASSERT_FLOAT("-x * -y", 3.75f);
    ASSERT_FLOAT("n / 2 + n / 2", 24.0f);
    ASSERT_FLOAT("n / (2 + n / 2)", 24.0f / (2.0f + 24.0f / 2.0f));
    ASSERT_FLOAT("x + y + z + t", 1.5f + 2.5f + 3.5f + 4.0f);
    ASSERT_FLOAT("t - n + n - t", 0.0f);
    ASSERT_FLOAT("n % 5 + t", 8.0f);    /* 24%5=4, +4=8 */
    ASSERT_FLOAT("x < y && y < z", 1.0f);
    ASSERT_FLOAT("x < y && y > z", 0.0f);
    ASSERT_FLOAT("n > t * 5", 1.0f);        /* 24 > 20 */
    ASSERT_FLOAT("n > t * 6", 0.0f);        /* 24 > 24 false */
    ASSERT_FLOAT("n >= t * 6", 1.0f);
    ASSERT_FLOAT("n - 4 == 5 * 4", 1.0f);   /* 20 == 20 */
    ASSERT_FLOAT("x + y == z + 0.5", 1.0f);     /* 4.0 == 4.0 */
    ASSERT_FLOAT("x * 2 + y == 5.5", 1.0f);     /* 3.0 + 2.5 == 5.5 */
    ASSERT_FLOAT("x * 2 + y * 2 == 8", 1.0f);   /* 3.0 + 5.0 == 8.0 */
    /* Unary on a variable, then arithmetic */
    ASSERT_FLOAT("-x * 2 + y", -3.0f + 2.5f);
    ASSERT_FLOAT("!(x > y)", 1.0f);   /* x<y so x>y is 0, !0=1 */
    ASSERT_FLOAT("!(n > 0)", 0.0f);

    printf("Operator precedence (with functions):\n");
    /* Function-call arguments are full expressions; outer expression
     * sees the call result as an atom that binds tighter than '*'. */
    ASSERT_FLOAT("sin(0) + cos(0)", 1.0f);
    ASSERT_FLOAT("cos(0) * 2 + 1", 3.0f);
    ASSERT_FLOAT("sqrt(4) + sqrt(9)", 5.0f);
    ASSERT_FLOAT("-sin(0)", 0.0f);
    ASSERT_FLOAT("-cos(0)", -1.0f);
    ASSERT_FLOAT("2 * sqrt(4)", 4.0f);
    ASSERT_FLOAT("sqrt(2 * 2 + 3 * 3)", sqrtf(13.0f));
    ASSERT_FLOAT("pow(2, 3) + 1", 9.0f);
    ASSERT_FLOAT("pow(2, 1 + 2)", 8.0f);
    ASSERT_FLOAT("pow(1 + 1, 3)", 8.0f);
    ASSERT_FLOAT("pow(2, 3) * 2", 16.0f);
    ASSERT_FLOAT("min(1, 2) + max(3, 4)", 5.0f);
    ASSERT_FLOAT("min(2 * 3, 7)", 6.0f);
    ASSERT_FLOAT("max(1 + 2, 3 - 4)", 3.0f);
    ASSERT_FLOAT("sin(PI / 2)", 1.0f);
    ASSERT_FLOAT("cos(PI) + 1", 0.0f);
    ASSERT_FLOAT("abs(-3 * 2)", 6.0f);
    ASSERT_FLOAT("abs(-3) * 2", 6.0f);
    ASSERT_FLOAT("floor(3.7) + ceil(0.2)", 4.0f);
    ASSERT_FLOAT("floor(3.7 + 0.2)", 3.0f);
    ASSERT_FLOAT("floor(3.7) * 2", 6.0f);
    /* Functions whose args reference variables (x=1.5, y=2.5, z=3.5,
     * n=24, t=4) */
    ASSERT_FLOAT("sin(t * 0)", 0.0f);
    ASSERT_FLOAT("cos(t * 0)", 1.0f);
    ASSERT_FLOAT("pow(x, 2)", 1.5f * 1.5f);     /* 2.25 */
    ASSERT_FLOAT("pow(2, n / 12)", 4.0f);       /* 2^2 */
    ASSERT_FLOAT("min(x, y)", 1.5f);
    ASSERT_FLOAT("max(x, y)", 2.5f);
    ASSERT_FLOAT("max(x + 1, y)", 2.5f);        /* tie, returns y */
    ASSERT_FLOAT("abs(x - y) + abs(y - x)", 2.0f);
    ASSERT_FLOAT("sqrt(x * x + y * y)",
                 sqrtf(1.5f * 1.5f + 2.5f * 2.5f));
    /* Function-of-function (nested calls) */
    ASSERT_FLOAT("sin(cos(0))", sinf(1.0f));
    ASSERT_FLOAT("sqrt(abs(-9))", 3.0f);
    ASSERT_FLOAT("abs(sin(0) - 1)", 1.0f);
    ASSERT_FLOAT("min(max(1, 2), 3)", 2.0f);
    ASSERT_FLOAT("max(min(5, 7), min(3, 4))", 5.0f);
    ASSERT_FLOAT("floor(abs(-2.7))", 2.0f);
    ASSERT_FLOAT("pow(sqrt(4), 3)", 8.0f);
    /* Function calls combined with logical operators */
    ASSERT_FLOAT("sin(0) == 0 && cos(0) == 1", 1.0f);
    ASSERT_FLOAT("pow(2, 3) > 5 || 0", 1.0f);
    ASSERT_FLOAT("abs(x) > 0 && abs(y) > 0", 1.0f);
    ASSERT_FLOAT("sqrt(x * x) == x", 1.0f);
    /* Reset between sections */

    printf("Operator precedence (with scratch arrays):\n");
    {
        repl_eval_reset_scratch_arrays();
        repl_eval_scratch_set(0, 0, 10.0f);    /* A[0] */
        repl_eval_scratch_set(0, 1, 20.0f);    /* A[1] */
        repl_eval_scratch_set(0, 2, 5.0f);     /* A[2] */
        repl_eval_scratch_set(1, 0, 100.0f);   /* B[0] */
        repl_eval_scratch_set(1, 1, 200.0f);   /* B[1] */

        ASSERT_FLOAT("A[0] + A[1]", 30.0f);
        ASSERT_FLOAT("A[0] * 2 + 1", 21.0f);
        ASSERT_FLOAT("A[0] + A[1] * 2", 50.0f);  /* 10 + 40 */
        ASSERT_FLOAT("(A[0] + A[1]) * 2", 60.0f);
        ASSERT_FLOAT("A[0] < A[1]", 1.0f);
        ASSERT_FLOAT("A[0] + A[1] == 30", 1.0f);
        ASSERT_FLOAT("A[0] + A[1] > B[0]", 0.0f);   /* 30 > 100 false */
        ASSERT_FLOAT("A[1] * 5 == B[0]", 1.0f);     /* 100 == 100 */
        ASSERT_FLOAT("-A[0] + A[1]", 10.0f);
        ASSERT_FLOAT("A[1 + 1]", 5.0f);            /* index expression */
        ASSERT_FLOAT("A[0] + B[0]", 110.0f);
        ASSERT_FLOAT("B[1] - B[0]", 100.0f);
        ASSERT_FLOAT("A[0] > 0 && B[0] > 0", 1.0f);
        ASSERT_FLOAT("sqrt(A[0] * A[0] + A[1] * A[1])",
                     sqrtf(10.0f * 10.0f + 20.0f * 20.0f));
        ASSERT_FLOAT("max(A[0], A[1]) - min(A[0], A[1])", 10.0f);

        repl_eval_reset_scratch_arrays();
    }

    printf("scratch arrays:\n");
    {
        float value = -1.0f;
        float snapshot[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];

        repl_eval_reset_scratch_arrays();
        ASSERT_TRUE("scratch array index A", repl_eval_scratch_array_index("A") == 0);
        ASSERT_TRUE("scratch array index B", repl_eval_scratch_array_index("B") == 1);
        ASSERT_TRUE("scratch array index C", repl_eval_scratch_array_index("C") == 2);
        ASSERT_TRUE("scratch array index miss", repl_eval_scratch_array_index("Z") == -1);

        ASSERT_TRUE("scratch arrays reset to zero",
                    repl_eval_scratch_get(0, 0, &value) && fabsf(value) < 1e-6f);
        ASSERT_TRUE("scratch set A[0]",
                    repl_eval_scratch_set(0, 0, 1.25f));
        ASSERT_TRUE("scratch set A[2]",
                    repl_eval_scratch_set(0, 2, 3.5f));
        ASSERT_TRUE("scratch get A[2]",
                    repl_eval_scratch_get(0, 2, &value) && fabsf(value - 3.5f) < 1e-6f);
        ASSERT_TRUE("scratch reject bad element",
                    !repl_eval_scratch_set(0, REPL_SCRATCH_ARRAY_LEN, 0.0f));
        repl_eval_copy_scratch_arrays(snapshot);
        repl_eval_scratch_set(0, 2, 9.0f);
        repl_eval_restore_scratch_arrays(snapshot);
        ASSERT_TRUE("scratch restore A[2]",
                    repl_eval_scratch_get(0, 2, &value) && fabsf(value - 3.5f) < 1e-6f);

        set_predef("i", 2.0f);
        {
            char err[128] = {0};
            ExprCtx ctx = { "A[1 + 1]", NULL, 0, err, sizeof(err) };
            float actual = repl_eval_expr(&ctx);
            ASSERT_TRUE("scratch read A[1+1]",
                        fabsf(actual - 3.5f) < 1e-4f && err[0] == '\0');
        }
        {
            char err[128] = {0};
            ExprCtx ctx = { "A[i]", NULL, 0, err, sizeof(err) };
            float actual = repl_eval_expr(&ctx);
            ASSERT_TRUE("scratch read A[i]",
                        fabsf(actual - 3.5f) < 1e-4f && err[0] == '\0');
        }
        {
            char err[128] = {0};
            ExprCtx ctx = { "A[16]", NULL, 0, err, sizeof(err) };
            (void)repl_eval_expr(&ctx);
            ASSERT_TRUE("scratch read A[16] errors", err[0] != '\0');
        }
        {
            char err[128] = {0};
            ExprCtx ctx = { "A[-1]", NULL, 0, err, sizeof(err) };
            (void)repl_eval_expr(&ctx);
            ASSERT_TRUE("scratch read A[-1] errors", err[0] != '\0');
        }
        {
            char err[128] = {0};
            ExprCtx ctx = { "A", NULL, 0, err, sizeof(err) };
            (void)repl_eval_expr(&ctx);
            ASSERT_TRUE("bare scratch array errors", err[0] != '\0');
        }
    }

    /* Loop vars override predefined */
    {
        ExprVar lv[1] = { { "x", 99.0f } };
        ExprCtx ctx = { "x", lv, 1 };
        float v = repl_eval_expr(&ctx);
        ASSERT_TRUE("loop var override equals 99", fabsf(v - 99.0f) < 1e-4f);
    }

    /* Reset */
    set_predef("x", 0.0f);
    set_predef("y", 0.0f);
    set_predef("z", 0.0f);
    set_predef("n", 0.0f);
    set_predef("t", 0.0f);

    /* ---- parse_exprs ---- */
    printf("parse_exprs:\n");
    ASSERT_EXPRS("1, 2, 3", 3, 1.0f, 2.0f, 3.0f);
    ASSERT_EXPRS("1+2, 3*4", 2, 3.0f, 12.0f);
    ASSERT_EXPRS("sin(0), cos(0)", 2, 0.0f, 1.0f);
    {
        ExprVar vars[2] = { { "radius", 3.0f }, { "height", 4.0f } };
        float vals[4];
        int n = repl_eval_parse_exprs("radius + height, 0", vals, 4, vars, 2);
        ASSERT_TRUE("parse_exprs with vars returns expected values",
                    n == 2 && fabsf(vals[0] - 7.0f) < 1e-4f && fabsf(vals[1]) < 1e-4f);
    }

    /* ---- Expression translation ---- */
    printf("repl_expr_to_c:\n");
    ASSERT_TO_C("sin(x)", "sinf(x)");
    ASSERT_TO_C("cos(PI/4)", "cosf(((float)M_PI)/4)");
    ASSERT_TO_C("TAU/n", "(2.0f*(float)M_PI)/n");
    ASSERT_TO_C("sqrt(x*x+y*y)", "sqrtf(x*x+y*y)");
    ASSERT_TO_C("abs(-1)", "fabsf(-1)");
    ASSERT_TO_C("glVertex3f(1,2,3)", "glVertex3f(1,2,3)");  /* unchanged */
    ASSERT_TO_C("pow(x,2)", "powf(x,2)");
    ASSERT_TO_C("min(x,y)", "fminf(x,y)");
    ASSERT_TO_C("rand(i,3)", "repl_randf(i,3)");
    ASSERT_TO_C("asin(x)", "asinf(x)");
    ASSERT_TO_C("acos(x)", "acosf(x)");
    ASSERT_TO_C("atan(x)", "atanf(x)");
    ASSERT_TO_C("clamp(x,0,1)", "repl_clampf(x,0,1)");
    ASSERT_TO_C("lerp(a,b,s)", "repl_lerpf(a,b,s)");
    ASSERT_TO_C("smoothstep(0,1,x)", "repl_smoothstepf(0,1,x)");
    ASSERT_TO_C("sign(x)", "repl_signf(x)");
    ASSERT_TO_C("atan2(y,x)", "atan2f(y,x)");
    ASSERT_TO_C("x % 2", "fmodf(x, 2)");
    ASSERT_TO_C("10 % 3", "fmodf(10, 3)");
    ASSERT_TO_C("(x+y) % (z*2)", "fmodf((x+y), (z*2))");
    ASSERT_TO_C("sin(x) % 1", "fmodf(sinf(x), 1)");
    ASSERT_TO_C("rem(x,2)", "remainderf(x,2)");
    ASSERT_TO_C("nan", "NAN");
    ASSERT_TO_C("inf", "INFINITY");
    /* Fractional literals lower float-suffixed: an unsuffixed `1.2` is a
     * double in C, and the whole expression would promote with it while
     * the evaluator rounds to float after every operation. Integers need
     * no suffix (C converts an int operand without promoting), which is
     * also what leaves subscripts and hex masks alone. */
    ASSERT_TO_C("t*1.2 + p*0.037", "t*1.2f + p*0.037f");
    ASSERT_TO_C("h*0.5", "h*0.5f");
    ASSERT_TO_C("1.5e3*x", "1.5e3f*x");
    ASSERT_TO_C("x*2", "x*2");
    ASSERT_TO_C("1.2f*x", "1.2f*x");   /* already suffixed, not doubled */
    ASSERT_TO_C("glStencilFunc(GL_EQUAL, 1, 0xFF)",
                "glStencilFunc(GL_EQUAL, 1, 0xFF)");
    ASSERT_TO_C("A[2]", "A[2]");
    ASSERT_TO_C("A[i+1]", "A[(int)(i+1)]");
    ASSERT_TO_C("A[B[0]+1]", "A[(int)(B[0]+1)]");

    printf("c_expr_to_repl:\n");
    ASSERT_TO_REPL("sinf(x)", "sin(x)");
    ASSERT_TO_REPL("cosf(((float)M_PI)/4)", "cos(PI/4)");
    ASSERT_TO_REPL("(2.0f*(float)M_PI)/n", "TAU/n");
    /* Pre-float-cast exports still import. */
    ASSERT_TO_REPL("cosf(M_PI/4)", "cos(PI/4)");
    ASSERT_TO_REPL("(2*M_PI)/n", "TAU/n");
    ASSERT_TO_REPL("sqrtf(x*x+y*y)", "sqrt(x*x+y*y)");
    ASSERT_TO_REPL("fabsf(-1)", "abs(-1)");
    ASSERT_TO_REPL("glVertex3f(1,2,3)", "glVertex3f(1,2,3)");
    ASSERT_TO_REPL("powf(x,2)", "pow(x,2)");
    /* The `f` suffix comes back off: it is what repl_eval_expr_to_c added
     * so the exported C would evaluate in float instead of promoting to
     * double, and canonical REPL text does not carry it. */
    ASSERT_TO_REPL("powf(1.0f,2.0f)", "pow(1.0,2.0)");
    ASSERT_TO_REPL("t*1.2f + p*0.037f", "t*1.2 + p*0.037");
    ASSERT_TO_REPL("1.5e3f*x", "1.5e3*x");
    /* Untouched: integers never grew a suffix, and a hex mask is not a
     * decimal literal at all. */
    ASSERT_TO_REPL("glStencilFunc(GL_EQUAL, 1, 0xFF)",
                   "glStencilFunc(GL_EQUAL, 1, 0xFF)");
    ASSERT_TO_REPL("repl_randf(i,3)", "rand(i,3)");
    ASSERT_TO_REPL("asinf(x)", "asin(x)");
    ASSERT_TO_REPL("acosf(x)", "acos(x)");
    ASSERT_TO_REPL("atanf(x)", "atan(x)");
    ASSERT_TO_REPL("repl_clampf(x,0,1)", "clamp(x,0,1)");
    ASSERT_TO_REPL("repl_lerpf(a,b,s)", "lerp(a,b,s)");
    ASSERT_TO_REPL("repl_smoothstepf(0,1,x)", "smoothstep(0,1,x)");
    ASSERT_TO_REPL("repl_signf(x)", "sign(x)");
    ASSERT_TO_REPL("atan2f(y,x)", "atan2(y,x)");
    ASSERT_TO_REPL("remainderf(x,2)", "rem(x,2)");
    ASSERT_TO_REPL("nan", "NAN");
    ASSERT_TO_REPL("INFINITY", "INFINITY");
    ASSERT_TO_REPL("A[2]", "A[2]");
    ASSERT_TO_REPL("A[(int)(i+1)]", "A[i+1]");
    ASSERT_TO_REPL("A[(int)(B[0] + 1)]", "A[B[0] + 1]");
    ASSERT_FLOAT("pow(1.0f,2.0f)", 1.0f);
    {
        char tiny[5];
        repl_eval_c_expr_to_repl("sinf(x)", tiny, sizeof(tiny));
        TEST_ASSERT_STR(&g_harness, "c_expr_to_repl truncation", tiny, "sin(");

        strcpy(tiny, "keep");
        repl_eval_c_expr_to_repl("sinf(x)", tiny, 0);
        TEST_ASSERT_STR(&g_harness, "c_expr_to_repl out_sz=0 keeps output",
                        tiny, "keep");
    }
    {
        char buf[8];
        strip_ws(" \t a b \n", buf, sizeof(buf));
        TEST_ASSERT_STR(&g_harness, "strip_ws mixed whitespace", buf, "ab");

        strip_ws("   \n\t", buf, sizeof(buf));
        TEST_ASSERT_STR(&g_harness, "strip_ws whitespace-only", buf, "");

        strip_ws("a b c d", buf, 4);
        TEST_ASSERT_STR(&g_harness, "strip_ws truncation", buf, "abc");

        strcpy(buf, "keep");
        strip_ws(NULL, buf, sizeof(buf));
        TEST_ASSERT_STR(&g_harness, "strip_ws null input unchanged", buf, "keep");
    }
    ASSERT_ROUNDTRIP_WS("sin( TAU / n )");
    ASSERT_ROUNDTRIP_WS("pow( radius + 1, height )");
    ASSERT_ROUNDTRIP_WS("min( x , y )");

    /* Roundtrip */
    printf("Roundtrip (repl->c->repl):\n");
    {
        const char *cases[] = {
            "sin(TAU/n)", "cos(PI*x)", "sqrt(x*x+y*y)",
            "pow(2, n)", "glVertex3f(x, y, z)",
            "abs(x-y)", "floor(x/2)", "min(x, y)",
            NULL
        };
        for (int ci = 0; cases[ci]; ci++) {
            char label[128];
            char c_buf[512], repl_buf[512];
            repl_eval_expr_to_c(cases[ci], c_buf, sizeof(c_buf));
            repl_eval_c_expr_to_repl(c_buf, repl_buf, sizeof(repl_buf));
            snprintf(label, sizeof(label), "roundtrip: %s", cases[ci]);
            TEST_ASSERT_STR(&g_harness, label, repl_buf, cases[ci]);
        }
    }

    /* ---- For-loop parsers ---- */
    printf("parse_for_header:\n");
    ASSERT_FOR("for(i, 0, 10)", 1, "i", 0.0f, 10.0f, 1.0f);
    ASSERT_FOR("for(i, 0, 10, 2)", 1, "i", 0.0f, 10.0f, 2.0f);
    ASSERT_FOR("for (j, 1, 5)", 1, "j", 1.0f, 5.0f, 1.0f);
    ASSERT_FOR("for(i, 0, 2*PI)", 1, "i", 0.0f, (float)(2.0*M_PI), 1.0f);
    ASSERT_FOR("not a for loop", 0, "", 0, 0, 0);
    ASSERT_FOR("for(i 0 10)", 0, "", 0, 0, 0);  /* missing commas */

    /* For with variables */
    set_predef("n", 24.0f);
    ASSERT_FOR("for(i, 0, n)", 1, "i", 0.0f, 24.0f, 1.0f);
    set_predef("n", 0.0f);
    {
        ExprVar vars[2] = { { "radius", 7.5f }, { "stepv", 0.5f } };
        char vn[16];
        float s, e, st;
        const char *body = NULL;
        const char *bp = NULL;
        int ok = repl_eval_parse_for_header(
            &(ReplForHeaderParseConfig){
                .input = "for(i, 0, radius, stepv) glVertex3f(i, 0, 0);",
                .var_name = vn,
                .var_sz = (int)sizeof(vn),
                .start = &s,
                .end = &e,
                .step = &st,
                .body_start = &body,
                .vars = vars,
                .num_vars = 2,
            });
        bp = body;
        while (bp && *bp && isspace((unsigned char)*bp))
            bp++;
        ASSERT_TRUE("parse_for_header resolves local vars",
                    ok == 1 &&
                    strcmp(vn, "i") == 0 &&
                    fabsf(s - 0.0f) < 1e-4f &&
                    fabsf(e - 7.5f) < 1e-4f &&
                    fabsf(st - 0.5f) < 1e-4f &&
                    bp != NULL &&
                    strncmp(bp, "glVertex3f(", 11) == 0);
    }

    /* Finding 2 / P2: the evaluator resolves predefined-variable names against
     * ExprCtx.predef_vars when one is supplied (how compile evaluates against
     * its ReplCompileContext snapshot) rather than the live g_predef_vars
     * table. `t` is always live, so a synthetic value distinct from its live
     * value proves the context view wins - both for a direct eval and for a
     * for-header bound, which threads the same view. */
    {
        ExprVar synth[1] = { { "t", 99.0f } };
        ExprCtx ctx = { "t * 2", NULL, 0, NULL, 0, synth, 1 };
        ASSERT_TRUE("eval predef resolves from ctx->predef_vars",
                    fabsf(repl_eval_expr(&ctx) - 198.0f) < 1e-4f);

        char vn[16]; float s, e, st; const char *body = NULL;
        int ok = repl_eval_parse_for_header(
            &(ReplForHeaderParseConfig){
                .input = "for(i, 0, t) glVertex3f(i, 0, 0);",
                .var_name = vn,
                .var_sz = (int)sizeof(vn),
                .start = &s,
                .end = &e,
                .step = &st,
                .body_start = &body,
                .predef_vars = synth,
                .predef_count = 1,
            });
        ASSERT_TRUE("for-bound resolves predef from the supplied view",
                    ok == 1 && fabsf(e - 99.0f) < 1e-4f);
    }

    printf("parse_c_for_header:\n");
    ASSERT_CFOR("  for (float i = 0; i < 10; i += 1.0f) {", 1, "i", 0.0f, 10.0f, 1.0f);
    ASSERT_CFOR("for (float j = 1; j < 5; j++) {", 1, "j", 1.0f, 5.0f, 1.0f);
    ASSERT_CFOR("for (int i = 10; i > 0; i--) {", 1, "i", 10.0f, 0.0f, -1.0f);
    ASSERT_CFOR("for (float i = 0; i < 10; i += 0.5f) {", 1, "i", 0.0f, 10.0f, 0.5f);
    ASSERT_CFOR("not a for loop", 0, "", 0, 0, 0);

    /* ---- input_has_predef_vars ---- */
    printf("input_has_predef_vars:\n");
    ASSERT_HAS_VARS("glVertex3f(1, 2, 3)", 0);
    ASSERT_HAS_VARS("glVertex3f(x, 0, 0)", 1);
    ASSERT_HAS_VARS("glVertex3f(0, y, z)", 1);
    ASSERT_HAS_VARS("sin(PI)", 0);        /* PI is not a predef var */
    ASSERT_HAS_VARS("sin(n*PI)", 1);      /* n is */
    ASSERT_HAS_VARS("glVertex3f(1,2,3)", 0);
    ASSERT_HAS_VARS("i+j*k", 1);
    ASSERT_HAS_VARS("t + 1", 1);
    ASSERT_HAS_VARS("x * 2", 1);
    ASSERT_HAS_VARS("y - 3", 1);
    ASSERT_HAS_VARS("z / 4", 1);
    ASSERT_HAS_VARS("alpha + beta", 0);

    /* ---- Expression evaluator - additional coverage ---- */
    printf("Expression evaluator (additional):\n");

    /* Math functions with less obvious inputs */
    ASSERT_FLOAT("sin(PI)", 0.0f);                     /* sin(pi) ~ 0 */
    ASSERT_FLOAT("sqrt(9)", 3.0f);
    ASSERT_FLOAT("sqrt(0)", 0.0f);
    ASSERT_FLOAT("abs(-3.5)", 3.5f);
    ASSERT_FLOAT("ceil(-2.3)", -2.0f);
    ASSERT_FLOAT("floor(-2.3)", -3.0f);
    ASSERT_FLOAT("round(-2.3)", -2.0f);
    ASSERT_FLOAT("round(-2.7)", -3.0f);
    ASSERT_FLOAT("round(0.5)", 1.0f);
    ASSERT_FLOAT("round(-0.5)", -1.0f);
    ASSERT_FLOAT("min(5,3)", 3.0f);                    /* reversed arg order */
    ASSERT_FLOAT("max(5,3)", 5.0f);
    ASSERT_FLOAT("pow(2,0)", 1.0f);
    ASSERT_FLOAT("fmod(-7,3)", fmodf(-7.0f, 3.0f));   /* sign follows dividend */
    ASSERT_FLOAT("fmod(0,5)", 0.0f);

    /* Operator precedence and nesting */
    ASSERT_FLOAT("1+2*3-4/2", 5.0f);                  /* 1+(2*3)-(4/2) */
    ASSERT_FLOAT("(1+2)*(3+4)", 21.0f);
    ASSERT_FLOAT("((2+3))*((4-1)+2)", 25.0f);          /* deeply nested */
    ASSERT_FLOAT("---5", -5.0f);                       /* triple unary minus */
    ASSERT_FLOAT("--5", 5.0f);                         /* double unary minus (exists) */
    ASSERT_FLOAT("1.5f", 1.5f);                        /* C-style float literal */

    /* Logical operators with non-integer truth values */
    ASSERT_FLOAT("!0.5", 0.0f);                        /* 0.5 is truthy */
    ASSERT_FLOAT("!0.0", 1.0f);
    ASSERT_FLOAT("1==1 && 2>1", 1.0f);
    ASSERT_FLOAT("1==0 || 2>1", 1.0f);
    ASSERT_FLOAT("5>=5 && 5<=5", 1.0f);
    ASSERT_FLOAT("0 || 0 || 1", 1.0f);
    ASSERT_FLOAT("1 && 1 && 0", 0.0f);

    /* Functions with wrong arg count - fall through to 0.0 */
    ASSERT_FLOAT("pow(2)", 0.0f);                      /* needs 2 args */
    ASSERT_FLOAT("min(3)", 0.0f);
    ASSERT_FLOAT("max(7)", 0.0f);
    ASSERT_FLOAT("fmod(1)", 0.0f);                     /* needs 2 args */
    ASSERT_FLOAT("sin(1,2)", 0.0f);                    /* too many args */
    ASSERT_FLOAT("cos(1,2)", 0.0f);
    ASSERT_FLOAT("floor(1,2)", 0.0f);
    ASSERT_FLOAT("abs(1,2)", 0.0f);

    /* ---- declare_predef_var error paths ---- */
    printf("declare_predef_var (error paths):\n");
    {
        char err[128];
        int ok;

        /* Empty name */
        ok = repl_eval_declare_predef_var("", err, sizeof(err));
        ASSERT_TRUE("empty name should fail", !ok);

        /* Name starts with a digit */
        ok = repl_eval_declare_predef_var("3x", err, sizeof(err));
        ASSERT_TRUE("digit-start name should fail", !ok);

        /* Name contains invalid character */
        ok = repl_eval_declare_predef_var("x@y", err, sizeof(err));
        ASSERT_TRUE("invalid char name should fail", !ok);

        /* Reserved built-in name */
        ok = repl_eval_declare_predef_var("sin", err, sizeof(err));
        ASSERT_TRUE("reserved name sin should fail", !ok);

        ok = repl_eval_declare_predef_var("log", err, sizeof(err));
        ASSERT_TRUE("reserved name log should fail", !ok);

        ok = repl_eval_declare_predef_var("ln", err, sizeof(err));
        ASSERT_TRUE("reserved name ln should fail", !ok);

        ok = repl_eval_declare_predef_var("PI", err, sizeof(err));
        ASSERT_TRUE("reserved name PI should fail", !ok);

        ok = repl_eval_declare_predef_var("e", err, sizeof(err));
        ASSERT_TRUE("reserved name e should fail", !ok);

        ok = repl_eval_declare_predef_var("A", err, sizeof(err));
        ASSERT_TRUE("reserved name A should fail", !ok);

        ok = repl_eval_declare_predef_var("B", err, sizeof(err));
        ASSERT_TRUE("reserved name B should fail", !ok);

        ok = repl_eval_declare_predef_var("C", err, sizeof(err));
        ASSERT_TRUE("reserved name C should fail", !ok);

        /* lerp was this case's "not a builtin, so declarable" name until it
         * became one; gain is the stand-in. Both directions still matter:
         * a builtin name is refused, a free name registers. */
        ok = repl_eval_declare_predef_var("lerp", err, sizeof(err));
        ASSERT_TRUE("reserved name lerp should fail", !ok);

        ok = repl_eval_declare_predef_var("clamp", err, sizeof(err));
        ASSERT_TRUE("reserved name clamp should fail", !ok);

        ok = repl_eval_declare_predef_var("smoothstep", err, sizeof(err));
        ASSERT_TRUE("reserved name smoothstep should fail", !ok);

        ok = repl_eval_declare_predef_var("sign", err, sizeof(err));
        ASSERT_TRUE("reserved name sign should fail", !ok);

        ok = repl_eval_declare_predef_var("gain", err, sizeof(err));
        ASSERT_TRUE("name gain now allowed", ok);
        ASSERT_TRUE("name gain registered",
                repl_eval_find_predef_var_idx("gain") >= 0);
        repl_eval_undeclare_predef_var("gain");
        ASSERT_TRUE("name gain removed",
                repl_eval_find_predef_var_idx("gain") < 0);

        /* Jump keywords. Nothing rejected these before: a scene could
         * declare `float break;`, assign to it and read it back, and then
         * export `static float break = 0.0f;` - which is not C. The
         * keyword statements parse ahead of any expression, so the name
         * was only ever reachable as an operand, never as a statement. */
        ok = repl_eval_declare_predef_var("break", err, sizeof(err));
        ASSERT_TRUE("reserved name break should fail", !ok);

        ok = repl_eval_declare_predef_var("continue", err, sizeof(err));
        ASSERT_TRUE("reserved name continue should fail", !ok);

        ok = repl_eval_declare_predef_var("return", err, sizeof(err));
        ASSERT_TRUE("reserved name return should fail", !ok);

        ok = repl_eval_declare_predef_var("t", err, sizeof(err));
        ASSERT_TRUE("reserved name t should fail (already declared)", !ok);

        /* Already declared */
        ok = repl_eval_declare_predef_var("x", err, sizeof(err));
        ASSERT_TRUE("re-declaring x should fail", !ok);

        /* Name too long (>= 16 chars) */
        ok = repl_eval_declare_predef_var("averylongvarname1", err, sizeof(err));
        ASSERT_TRUE("too-long name should fail", !ok);

        /* Valid new declaration */
        ASSERT_DECLARE_OK("alpha");
        ASSERT_DECLARE_OK("_beta");

        /* Undeclare and re-declare */
        repl_eval_undeclare_predef_var("alpha");
        ASSERT_TRUE("alpha removed after undeclare",
                    repl_eval_find_predef_var_idx("alpha") == -1);

        ASSERT_DECLARE_OK("alpha");   /* re-declare after undeclare */

        /* Undeclare non-existent - no-op, count unchanged */
        int count_before = g_num_predef_vars;
        repl_eval_undeclare_predef_var("nosuchvar");
        ASSERT_TRUE("undeclare non-existent keeps count",
                    g_num_predef_vars == count_before);

        /* Clean up extras so later tests aren't affected */
        repl_eval_undeclare_predef_var("alpha");
        repl_eval_undeclare_predef_var("_beta");
    }

    /* ---- validate_expression_idents ---- */
    printf("validate_expression_idents:\n");
    {
        ExprVar lv[2] = { { "r", 1.0f }, { "h", 2.0f } };

        /* Pure number - no identifiers at all */
        ASSERT_VALIDATE_OK("3.14", NULL, 0);
        ASSERT_VALIDATE_OK("42 + 1", NULL, 0);

        /* Constants PI, TAU, and e are always allowed */
        ASSERT_VALIDATE_OK("PI * 2", NULL, 0);
        ASSERT_VALIDATE_OK("TAU / 4", NULL, 0);
        ASSERT_VALIDATE_OK("e * 2", NULL, 0);
        ASSERT_VALIDATE_OK("ln(e)", NULL, 0);
        ASSERT_VALIDATE_OK("log(100)", NULL, 0);

        /* Function calls are allowed (identifier followed by '(') */
        ASSERT_VALIDATE_OK("sin(x)", lv, 2);   /* sin is a function call */
        ASSERT_VALIDATE_OK("sqrt(r*r + h*h)", lv, 2);

        /* Declared predefined var */
        ASSERT_VALIDATE_OK("x + 1", NULL, 0);  /* x is a predef var */
        ASSERT_VALIDATE_OK("A[0] + x", NULL, 0);
        ASSERT_VALIDATE_OK("A[i]", NULL, 0);

        /* Loop-local var provided in vars array */
        ASSERT_VALIDATE_OK("r + h", lv, 2);

        /* Undeclared variable - should fail */
        ASSERT_VALIDATE_FAIL("undefined_var", NULL, 0);
        ASSERT_VALIDATE_FAIL("r + unknown", lv, 2);
        ASSERT_VALIDATE_FAIL("A", NULL, 0);
        ASSERT_VALIDATE_FAIL("x[0]", NULL, 0);
        ASSERT_VALIDATE_FAIL("A[16]", NULL, 0);
        ASSERT_VALIDATE_FAIL("A[-1]", NULL, 0);

        /* Inline comment - scanner stops before it */
        ASSERT_VALIDATE_OK("x + 1 // comment with undefined_var", NULL, 0);

        /* Identifier too long (>= 16 chars) */
        ASSERT_VALIDATE_FAIL("averylongnamethatoverflows", NULL, 0);

        /* Unbalanced parens. The recursive-descent eval silently
         * tolerates a missing ')' on a function call (`max(1, 2`
         * evaluates to 2), so the validator must reject these so a
         * truncated line never reaches the commit pipeline. */
        ASSERT_VALIDATE_FAIL("max(1, 2", NULL, 0);
        ASSERT_VALIDATE_FAIL("floor(2.7", NULL, 0);
        ASSERT_VALIDATE_FAIL("min(3, 4", NULL, 0);
        ASSERT_VALIDATE_FAIL("abs(5", NULL, 0);
        ASSERT_VALIDATE_FAIL("sin(0", NULL, 0);
        ASSERT_VALIDATE_FAIL("max(min(1,2), 3", NULL, 0);
        ASSERT_VALIDATE_FAIL("(1 + 2", NULL, 0);
        /* Stray ')' is also a hard error (would otherwise terminate
         * eval midway and leave trailing junk). */
        ASSERT_VALIDATE_FAIL("1 + 2)", NULL, 0);
        ASSERT_VALIDATE_FAIL("max(1, 2))", NULL, 0);
        /* Balanced cases still validate. */
        ASSERT_VALIDATE_OK("max(1, 2)", NULL, 0);
        ASSERT_VALIDATE_OK("max(min(1,2), 3)", NULL, 0);
        ASSERT_VALIDATE_OK("((1 + 2) * 3)", NULL, 0);
    }

    /* ---- source_uses_ident ---- */
    printf("source_uses_ident:\n");
    ASSERT_SOURCE_USES("sin(x) + y", "x", 1);
    ASSERT_SOURCE_USES("sin(x) + y", "y", 1);
    ASSERT_SOURCE_USES("sin(x) + y", "z", 0);
    ASSERT_SOURCE_USES("glVertex3f(1,2,3)", "x", 0);
    /* Substring - "foo" must not match inside "foobar" */
    ASSERT_SOURCE_USES("foobar + 1", "foo", 0);
    ASSERT_SOURCE_USES("foo + foobar", "foo", 1);
    /* Numeric literal that starts with digits - "3" in "3.14" */
    ASSERT_SOURCE_USES("3.14 + x", "x", 1);
    ASSERT_SOURCE_USES("3.14", "x", 0);
    /* Empty source */
    ASSERT_SOURCE_USES("", "x", 0);
    /* Inline comment - scanner stops before //, mirroring the
     * expression validator above. A trailing comment that mentions
     * a name does not count as a real use; otherwise the
     * delete/comment-toggle reference checks falsely block on
     * remarks like `glVertex3f(0,0,0); // p axis`. */
    ASSERT_SOURCE_USES("glVertex3f(0,0,0) // p axis", "p", 0);
    ASSERT_SOURCE_USES("y = 1 // x lives in a comment now", "x", 0);
    /* Real use BEFORE an inline comment is still detected. */
    ASSERT_SOURCE_USES("y = x + 1 // x lives in a comment now", "x", 1);

    /* ---- parse_exprs edge cases ---- */
    printf("parse_exprs (edge cases):\n");
    ASSERT_EXPRS("", 0);                               /* empty string */
    ASSERT_EXPRS("   ", 0);                            /* whitespace only */
    ASSERT_EXPRS("42", 1, 42.0f);                      /* single value */
    ASSERT_EXPRS("1, 2", 2, 1.0f, 2.0f);              /* basic two-arg */
    ASSERT_EXPRS("sin(0), cos(0), sqrt(4)", 3, 0.0f, 1.0f, 2.0f);

    /* ---- repl_expr_to_c additional translations ---- */
    printf("repl_expr_to_c (additional):\n");
    ASSERT_TO_C("max(x,y)", "fmaxf(x,y)");
    ASSERT_TO_C("floor(x/2)", "floorf(x/2)");
    ASSERT_TO_C("ceil(x+1)", "ceilf(x+1)");
    ASSERT_TO_C("round(x+1)", "roundf(x+1)");
    ASSERT_TO_C("PI", "((float)M_PI)");
    ASSERT_TO_C("", "");
    ASSERT_TO_C("x + y", "x + y");                    /* passthrough, no substitution */
    ASSERT_TO_C("fmod(x, 3)", "fmodf(x, 3)");   /* fmod keyword -> fmodf */
    ASSERT_TO_C("log(x)", "log10f(x)");
    ASSERT_TO_C("ln(x)", "logf(x)");
    ASSERT_TO_C("e", "((float)M_E)");
    ASSERT_TO_C("ln(e)", "logf(((float)M_E))");

    /* ---- Omitted optional arguments are spelled out ----
     * rand/rand2 are the only builtins with arity_min < arity_max. The
     * evaluator reads the missing `iter` out of a zero-filled args[]; the C
     * helper's signature is fixed, so lowering has to write the 0 or the
     * exported translation unit does not compile. The default stays a bare
     * integer: it converts to the float parameter exactly, and an argument
     * is not an operand, so there is nothing here to promote. */
    printf("expr_to_c (optional args):\n");
    ASSERT_TO_C("rand(x)", "repl_randf(x, 0)");
    ASSERT_TO_C("rand2(x)", "repl_rand2f(x, 0)");
    /* A call already at full arity is untouched - no second default. */
    ASSERT_TO_C("rand(x, 3)", "repl_randf(x, 3)");
    /* The argument is still translated, and a nested call that also needs
     * padding closes against its own paren. */
    ASSERT_TO_C("rand(cos(x))", "repl_randf(cosf(x), 0)");
    ASSERT_TO_C("rand(rand(x))", "repl_randf(repl_randf(x, 0), 0)");
    ASSERT_TO_C("rand(max(a, b))", "repl_randf(fmaxf(a, b), 0)");
    /* Two sibling calls each get their own default. */
    ASSERT_TO_C("rand(a) + rand2(b)", "repl_randf(a, 0) + repl_rand2f(b, 0)");
    /* Padding is keyed to the call's own paren, not the nearest one. */
    ASSERT_TO_C("(rand(a))*2", "(repl_randf(a, 0))*2");
    ASSERT_TO_C("sin(rand(a))", "sinf(repl_randf(a, 0))");
    /* A trailing comment is copied raw, so a call named inside it is not
     * padded - it is not code. */
    ASSERT_TO_C("rand(a) // rand(b)", "repl_randf(a, 0) // rand(b)");

    /* Lowering pads and c_expr_to_repl does not un-pad, so `rand(x)` comes
     * back as `rand(x, 0)`: the same call by the evaluator's own rule (a
     * missing iter reads as 0), different characters. Teaching the inverse
     * to strip a trailing `, 0` would also delete one a user wrote on
     * purpose, so the asymmetry is the documented behavior. */
    {
        char c_buf[512], repl_buf[512];
        repl_eval_expr_to_c("rand(x)", c_buf, sizeof(c_buf));
        repl_eval_c_expr_to_repl(c_buf, repl_buf, sizeof(repl_buf));
        TEST_ASSERT_STR(&g_harness, "roundtrip: rand(x) normalizes to 2 args",
                        repl_buf, "rand(x, 0)");
    }

    /* ---- c_expr_to_repl additional translations ---- */
    printf("c_expr_to_repl (additional):\n");
    ASSERT_TO_REPL("fmaxf(x,y)", "max(x,y)");
    ASSERT_TO_REPL("floorf(x/2)", "floor(x/2)");
    ASSERT_TO_REPL("ceilf(x+1)", "ceil(x+1)");
    ASSERT_TO_REPL("roundf(x+1)", "round(x+1)");
    ASSERT_TO_REPL("((float)M_PI)", "PI");
    ASSERT_TO_REPL("M_PI", "PI");
    ASSERT_TO_REPL("((float)M_E)", "e");
    ASSERT_TO_REPL("M_E", "e");
    ASSERT_TO_REPL("log10f(x)", "log(x)");
    ASSERT_TO_REPL("logf(x)", "ln(x)");
    ASSERT_TO_REPL("logf(((float)M_E))", "ln(e)");
    ASSERT_TO_REPL("logf(M_E)", "ln(e)");
    ASSERT_TO_REPL("", "");
    ASSERT_TO_REPL("x + y", "x + y");

    /* ---- parse_for_header additional ---- */
    printf("parse_for_header (additional):\n");
    /* Step expressed as a constant */
    ASSERT_FOR("for(i, 0, 10, PI)", 1, "i", 0.0f, 10.0f, (float)M_PI);
    /* Float start/end */
    ASSERT_FOR("for(i, 0.5, 3.0)", 1, "i", 0.5f, 3.0f, 1.0f);
    /* Negative step */
    ASSERT_FOR("for(i, 10, 0, -1)", 1, "i", 10.0f, 0.0f, -1.0f);
    /* Missing end (only one value after var) - should fail */
    ASSERT_FOR("for(i, 0)", 0, "", 0, 0, 0);
    /* Empty var name */
    ASSERT_FOR("for(, 0, 10)", 0, "", 0, 0, 0);
    /* Nested expression in end */
    ASSERT_FOR("for(i, 0, sin(0)+5)", 1, "i", 0.0f, 5.0f, 1.0f);

    /* body_start points right after the closing ')' */
    {
        char vn[16]; float s, e, st;
        const char *body = NULL;
        int ok = repl_eval_parse_for_header(
            &(ReplForHeaderParseConfig){
                .input = "for(i, 0, 5) glVertex3f(0,0,0);",
                .var_name = vn,
                .var_sz = (int)sizeof(vn),
                .start = &s,
                .end = &e,
                .step = &st,
                .body_start = &body,
            });
        ASSERT_TRUE("for body_start points after header",
                    ok && body != NULL && strncmp(body, " glVertex3f", 11) == 0);
    }

    /* ---- parse_c_for_header additional ---- */
    printf("parse_c_for_header (additional):\n");
    /* double type */
    ASSERT_CFOR("for (double i = 0.5; i < 3.5; i += 1.0f) {", 1, "i", 0.5f, 3.5f, 1.0f);
    /* No space before '(' */
    ASSERT_CFOR("for(float k = 0; k < 8; k++) {", 1, "k", 0.0f, 8.0f, 1.0f);
    /* Inclusive upper bound: <= nudges end up by 1 */
    ASSERT_CFOR("for (float i = 0; i <= 9; i++) {", 1, "i", 0.0f, 10.0f, 1.0f);
    /* Count-down with -= */
    ASSERT_CFOR("for (float i = 5; i > 0; i -= 1.5f) {", 1, "i", 5.0f, 0.0f, -1.5f);
    /* Inclusive lower bound with >=: nudges end down by 1 */
    ASSERT_CFOR("for (float i = 10; i >= 2; i -= 2.0f) {", 1, "i", 10.0f, 1.0f, -2.0f);

    /* ---- input_has_predef_vars after undeclare ---- */
    printf("input_has_predef_vars (after undeclare):\n");
    {
        ASSERT_HAS_VARS("A[0] + 1", 1);

        /* 'x' is currently declared - should be found */
        ASSERT_HAS_VARS("x + 1", 1);

        /* undeclare 'x', then it should no longer match */
        repl_eval_undeclare_predef_var("x");
        ASSERT_HAS_VARS("x + 1", 0);

        /* restore 'x' for any follow-on code */
        repl_eval_declare_predef_var("x", NULL, 0);
        set_predef("x", 0.0f);
    }

    /* ---- repl_scan_next_arg_delim ----
     *
     * Paren-aware delimiter scan: finds the next top-level `,` or `)`,
     * skipping over nested `(...)`. Locks down the helper's contract so
     * any future callers that lift it for new arg-parsing inherit a
     * tested predicate. */
    {
        printf("\n--- repl_scan_next_arg_delim ---\n");

        /* Trivial: empty input -> returns the terminator. */
        {
            const char *s = "";
            const char *r = repl_scan_next_arg_delim(s);
            TEST_ASSERT_TRUE(&g_harness, "scan: empty -> '\\0'", *r == '\0');
        }

        /* Flat args: stops at the first top-level comma. */
        {
            const char *s = "1, 2, 3)";
            const char *r = repl_scan_next_arg_delim(s);
            TEST_ASSERT_TRUE(&g_harness, "scan: flat -> first ','", *r == ',');
            TEST_ASSERT_TRUE(&g_harness, "scan: flat: r points at ','",
                             (r - s) == 1);
        }

        /* Nested call: comma inside (...) is NOT a delimiter. */
        {
            const char *s = "cos(i*TAU + phase)*r, sin(j)*r, 0)";
            const char *r = repl_scan_next_arg_delim(s);
            TEST_ASSERT_TRUE(&g_harness, "scan: nested cos(): stops at top ','",
                             *r == ',');
            /* The top-level comma is right after "*r" - index 20. */
            TEST_ASSERT_TRUE(&g_harness, "scan: nested cos(): correct position",
                             (r - s) == 20);
        }

        /* Multiple nested parens: also handled. */
        {
            const char *s = "func0((a, b), (c, d)), more)";
            const char *r = repl_scan_next_arg_delim(s);
            TEST_ASSERT_TRUE(&g_harness, "scan: deep nesting: stops at top ','",
                             *r == ',');
            TEST_ASSERT_TRUE(&g_harness, "scan: deep nesting: correct position",
                             (r - s) == 21);
        }

        /* Top-level close paren ends the scan. */
        {
            const char *s = "cos(i) + 1)";
            const char *r = repl_scan_next_arg_delim(s);
            TEST_ASSERT_TRUE(&g_harness, "scan: ends at top ')'", *r == ')');
            TEST_ASSERT_TRUE(&g_harness, "scan: ')' at correct position",
                             (r - s) == 10);
        }

        /* No delimiter: walks to end-of-string. */
        {
            const char *s = "0.5 + 1";
            const char *r = repl_scan_next_arg_delim(s);
            TEST_ASSERT_TRUE(&g_harness, "scan: no delim -> '\\0'", *r == '\0');
        }

        /* Unbalanced open paren: walks to end-of-string instead of
         * matching a top-level delim. Defensive: malformed input
         * shouldn't loop forever. */
        {
            const char *s = "max(1, 2";
            const char *r = repl_scan_next_arg_delim(s);
            TEST_ASSERT_TRUE(&g_harness, "scan: unbalanced '(' -> '\\0'",
                             *r == '\0');
        }
    }

    /* ---- numeric_arg_at_cursor ---- */
    printf("numeric_arg_at_cursor:\n");
    {
        ReplNumericArgAtCursor r;

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0, 2.5, 3.0)", 12);
        ASSERT_TRUE("vertex arg0 found", r.found);
        ASSERT_TRUE("vertex arg0 start", r.arg_start == 11);
        ASSERT_TRUE("vertex arg0 end",   r.arg_end   == 14);
        TEST_ASSERT_FLOAT(&g_harness, "vertex arg0 value", r.value, 1.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0, 2.5, 3.0)", 17);
        ASSERT_TRUE("vertex arg1 found", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "vertex arg1 value", r.value, 2.5f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0, 2.5, 3.0)", 23);
        ASSERT_TRUE("vertex arg2 found", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "vertex arg2 value", r.value, 3.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glColor3f(0.8, 0.2, 0.5)", 11);
        ASSERT_TRUE("color arg found", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "color arg value", r.value, 0.8f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1+2, 3.0, 4.0)", 12);
        ASSERT_TRUE("expr arg first literal found", r.found);
        ASSERT_TRUE("expr arg first literal start", r.arg_start == 11);
        ASSERT_TRUE("expr arg first literal end", r.arg_end == 12);
        TEST_ASSERT_FLOAT(&g_harness, "expr arg first literal value", r.value, 1.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1+2, 3.0, 4.0)", 13);
        ASSERT_TRUE("expr arg second literal found", r.found);
        ASSERT_TRUE("expr arg second literal start", r.arg_start == 13);
        ASSERT_TRUE("expr arg second literal end", r.arg_end == 14);
        TEST_ASSERT_FLOAT(&g_harness, "expr arg second literal value", r.value, 2.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(sin(1), 3.0, 4.0)", 14);
        ASSERT_TRUE("func call arg not found", !r.found);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(t, 3.0, 4.0)", 12);
        ASSERT_TRUE("variable arg not found", !r.found);

        r = repl_eval_numeric_arg_at_cursor("glPointSize(5.0)", 13);
        ASSERT_TRUE("single-arg found", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "single-arg value", r.value, 5.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(-2.5, 3.0, 4.0)", 12);
        ASSERT_TRUE("negative arg found", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "negative arg value", r.value, -2.5f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("x = 5", 4);
        ASSERT_TRUE("bare assignment rhs found", r.found);
        ASSERT_TRUE("bare assignment rhs start", r.arg_start == 4);
        ASSERT_TRUE("bare assignment rhs end", r.arg_end == 5);
        TEST_ASSERT_FLOAT(&g_harness, "bare assignment rhs value", r.value, 5.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("x = 3.14", 5);
        ASSERT_TRUE("bare decimal found", r.found);
        ASSERT_TRUE("bare decimal start", r.arg_start == 4);
        ASSERT_TRUE("bare decimal end", r.arg_end == 8);
        TEST_ASSERT_FLOAT(&g_harness, "bare decimal value", r.value, 3.14f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("x = -2.5", 5);
        ASSERT_TRUE("bare negative found", r.found);
        ASSERT_TRUE("bare negative start", r.arg_start == 4);
        ASSERT_TRUE("bare negative end", r.arg_end == 8);
        TEST_ASSERT_FLOAT(&g_harness, "bare negative value", r.value, -2.5f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("x = 1 + 2", 9);
        ASSERT_TRUE("bare second operand found", r.found);
        ASSERT_TRUE("bare second operand start", r.arg_start == 8);
        ASSERT_TRUE("bare second operand end", r.arg_end == 9);
        TEST_ASSERT_FLOAT(&g_harness, "bare second operand value", r.value, 2.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("x = 1 + -3", 10);
        ASSERT_TRUE("bare signed second operand found", r.found);
        ASSERT_TRUE("bare signed second operand start", r.arg_start == 8);
        ASSERT_TRUE("bare signed second operand end", r.arg_end == 10);
        TEST_ASSERT_FLOAT(&g_harness, "bare signed second operand value", r.value, -3.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("a = 2 * sin(t)", 4);
        ASSERT_TRUE("bare coefficient found", r.found);
        ASSERT_TRUE("bare coefficient start", r.arg_start == 4);
        ASSERT_TRUE("bare coefficient end", r.arg_end == 5);
        TEST_ASSERT_FLOAT(&g_harness, "bare coefficient value", r.value, 2.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("hello world", 3);
        ASSERT_TRUE("no parens not found", !r.found);

        r = repl_eval_numeric_arg_at_cursor("x = sin(t)", 8);
        ASSERT_TRUE("bare variable not found", !r.found);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0, 2.0, 3.0)", 0);
        ASSERT_TRUE("cursor before paren not found", !r.found);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0, 2.0, 3.0)", 8);
        ASSERT_TRUE("digit in identifier not found", !r.found);

        {
            const char *line = "x = 5 // offset 9";
            const char *nine = strchr(line, '9');
            r = repl_eval_numeric_arg_at_cursor(line, (int)(nine - line));
            ASSERT_TRUE("trailing comment number not found", !r.found);
        }

        /* Boundary positions inside an arg slot: in front of the digits,
         * in the middle, and after - with and without surrounding
         * whitespace. "glVertex3f(1.0, 2.5, 3.0)" - ( at 10, , at 14, ,
         * at 19, ) at 24. */
        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0, 2.5, 3.0)", 11);
        ASSERT_TRUE("cursor in front of first arg", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "front of first arg value", r.value, 1.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0, 2.5, 3.0)", 13);
        ASSERT_TRUE("cursor on last digit of first arg", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "last digit value", r.value, 1.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0, 2.5, 3.0)", 14);
        ASSERT_TRUE("cursor on trailing comma (no space)", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "trailing comma value", r.value, 1.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0, 2.5, 3.0)", 15);
        ASSERT_TRUE("cursor in leading space of second arg", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "leading space value", r.value, 2.5f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0, 2.5, 3.0)", 24);
        ASSERT_TRUE("cursor on closing paren of last arg", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "closing paren value", r.value, 3.0f, 1e-4f);

        /* Trailing whitespace inside a slot: "glVertex3f(1.0 , 2.5, 3.0)"
         * - ( at 10, ' ' at 14, , at 15. */
        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0 , 2.5, 3.0)", 14);
        ASSERT_TRUE("cursor in trailing space after first arg", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "trailing-space value", r.value, 1.0f, 1e-4f);

        r = repl_eval_numeric_arg_at_cursor("glVertex3f(1.0 , 2.5, 3.0)", 15);
        ASSERT_TRUE("cursor on comma after trailing space", r.found);
        TEST_ASSERT_FLOAT(&g_harness, "comma-after-space value", r.value, 1.0f, 1e-4f);
    }

    /* ---- swatch_step ---- */
    printf("swatch_step:\n");
    {
        TEST_ASSERT_FLOAT(&g_harness, "step(0)", repl_eval_swatch_step(0.0f), 0.05f, 1e-6f);
        TEST_ASSERT_FLOAT(&g_harness, "step(0.3)", repl_eval_swatch_step(0.3f), 0.05f, 1e-6f);
        TEST_ASSERT_FLOAT(&g_harness, "step(1.0)", repl_eval_swatch_step(1.0f), 0.05f, 1e-6f);
        TEST_ASSERT_FLOAT(&g_harness, "step(5.0)", repl_eval_swatch_step(5.0f), 0.05f, 1e-6f);
        TEST_ASSERT_FLOAT(&g_harness, "step(10.0)", repl_eval_swatch_step(10.0f), 0.5f, 1e-6f);
        TEST_ASSERT_FLOAT(&g_harness, "step(50.0)", repl_eval_swatch_step(50.0f), 0.5f, 1e-6f);
        TEST_ASSERT_FLOAT(&g_harness, "step(100.0)", repl_eval_swatch_step(100.0f), 5.0f, 1e-4f);
        TEST_ASSERT_FLOAT(&g_harness, "step(-3.0)", repl_eval_swatch_step(-3.0f), 0.05f, 1e-6f);
        TEST_ASSERT_FLOAT(&g_harness, "step(-25.0)", repl_eval_swatch_step(-25.0f), 0.5f, 1e-6f);
    }

    /* ---- format_swatch_number ---- */
    printf("format_swatch_number:\n");
    {
        char buf[32];

        repl_eval_format_swatch_number(1.0f, buf, sizeof(buf));
        ASSERT_TRUE("fmt 1", strcmp(buf, "1") == 0);

        repl_eval_format_swatch_number(0.5f, buf, sizeof(buf));
        ASSERT_TRUE("fmt 0.5", strcmp(buf, "0.5") == 0);

        repl_eval_format_swatch_number(0.05f, buf, sizeof(buf));
        ASSERT_TRUE("fmt 0.05", strcmp(buf, "0.05") == 0);

        repl_eval_format_swatch_number(-2.5f, buf, sizeof(buf));
        ASSERT_TRUE("fmt -2.5", strcmp(buf, "-2.5") == 0);

        repl_eval_format_swatch_number(10.0f, buf, sizeof(buf));
        ASSERT_TRUE("fmt 10", strcmp(buf, "10") == 0);

        repl_eval_format_swatch_number(0.0f, buf, sizeof(buf));
        ASSERT_TRUE("fmt 0", strcmp(buf, "0") == 0);
    }

    /* ---- rand() distribution uniformity ---- */
    /* Guards expr_rand01()'s seed offset and overall uniformity: each
     * argument pattern must pass a chi-square goodness-of-fit at the
     * p=.01 level (df=19 critical value 36.19). Sample size is kept at
     * 2048 deliberately - the float32 hash has only ~5000 distinct
     * outputs, so far larger samples saturate that grid and inflate
     * chi-square past the threshold (see `randdist 100000`). At 2048 the
     * worst pattern sits near 23, leaving margin for sinf() ULP
     * differences across platforms. Run `--rand-dist` for the full table. */
    {
        const double CHISQ_P01 = 36.19; /* df=19, p=.01 */
        const long n = 2048;
        const int Cs[] = { 0, 1, 7 };
        for (int k = 0; k < 3; k++) {
            char lbl[64];
            double chisq = rand_dist_chisq(1, Cs[k], n);
            snprintf(lbl, sizeof lbl, "rand(i,%d) chi^2 %.1f < %.2f", Cs[k], chisq, CHISQ_P01);
            ASSERT_TRUE(lbl, chisq < CHISQ_P01);
        }
        for (int k = 0; k < 3; k++) {
            char lbl[64];
            double chisq = rand_dist_chisq(0, Cs[k], n);
            snprintf(lbl, sizeof lbl, "rand(%d,i) chi^2 %.1f < %.2f", Cs[k], chisq, CHISQ_P01);
            ASSERT_TRUE(lbl, chisq < CHISQ_P01);
        }
    }

    /* ---- Summary ---- */
    printf("\n%d / %d tests passed", g_harness.passed, g_harness.run);
    if (g_harness.passed == g_harness.run)
        printf(" - all OK!\n");
    else
        printf(" - %d FAILED\n", g_harness.run - g_harness.passed);
}

/* ---- rand() distribution table ---------------------------------------- */
/* Samples rand(...) through the real evaluator (no duplicated hash, so the
 * table can never drift from src/repl/eval.c) and prints per-pattern
 * uniformity stats. Drives the `randdist` interactive command and the
 * `--rand-dist` CLI mode. */

#define RAND_DIST_BINS 20

static float rand_dist_eval(const char *expr) {
    ExprCtx ctx = { expr, NULL, 0 };
    return repl_eval_expr(&ctx);
}

static int rand_dist_cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

/* Chi-square goodness-of-fit of `n` rand() samples against uniform[0,1)
 * over RAND_DIST_BINS equal bins. `seed_is_i` selects rand(i, C) vs
 * rand(C, i). Deterministic for a given (pattern, n); shared by the
 * printed table and the regression assertion in run_tests(). */
static double rand_dist_chisq(int seed_is_i, int C, long n) {
    long bins[RAND_DIST_BINS] = {0};
    char expr[64];
    for (long i = 0; i < n; i++) {
        if (seed_is_i) snprintf(expr, sizeof expr, "rand(%ld,%d)", i, C);
        else           snprintf(expr, sizeof expr, "rand(%d,%ld)", C, i);
        float r = rand_dist_eval(expr);
        int b = (int)(r * RAND_DIST_BINS);
        if (b < 0) b = 0;
        if (b >= RAND_DIST_BINS) b = RAND_DIST_BINS - 1;
        bins[b]++;
    }
    double expv = (double)n / RAND_DIST_BINS, chisq = 0;
    for (int b = 0; b < RAND_DIST_BINS; b++) { double d = bins[b] - expv; chisq += d * d / expv; }
    return chisq;
}

/* One uniformity row. `seed_is_i` selects rand(i, C) [seed = loop index,
 * iter const] vs rand(C, i) [seed const, iter = loop index]. */
static void rand_dist_row(const char *label, int seed_is_i, int C, long n) {
    long bins[RAND_DIST_BINS] = {0};
    double sum = 0, sumsq = 0, mn = 1e9, mx = -1e9;
    float *vals = (float *)malloc((size_t)n * sizeof *vals);
    char expr[64];
    for (long i = 0; i < n; i++) {
        if (seed_is_i) snprintf(expr, sizeof expr, "rand(%ld,%d)", i, C);
        else           snprintf(expr, sizeof expr, "rand(%d,%ld)", C, i);
        float r = rand_dist_eval(expr);
        if (vals) vals[i] = r;
        int b = (int)(r * RAND_DIST_BINS);
        if (b < 0) b = 0;
        if (b >= RAND_DIST_BINS) b = RAND_DIST_BINS - 1;
        bins[b]++;
        sum += r; sumsq += (double)r * r;
        if (r < mn) mn = r;
        if (r > mx) mx = r;
    }
    double mean = sum / n, var = sumsq / n - mean * mean;
    double expv = (double)n / RAND_DIST_BINS, chisq = 0;
    for (int b = 0; b < RAND_DIST_BINS; b++) { double d = bins[b] - expv; chisq += d * d / expv; }
    long distinct = 0;
    if (vals) {
        qsort(vals, (size_t)n, sizeof *vals, rand_dist_cmp_float);
        distinct = n ? 1 : 0;
        for (long i = 1; i < n; i++) if (vals[i] != vals[i - 1]) distinct++;
        free(vals);
    }
    printf("\n=== %s  (n=%ld, %d bins) ===\n", label, n, RAND_DIST_BINS);
    printf("mean     = %.5f   (ideal 0.50000)\n", mean);
    printf("var      = %.5f   (ideal 0.08333)\n", var);
    printf("min/max  = %.5f / %.5f\n", mn, mx);
    printf("distinct = %ld (%.1f%%)\n", distinct, 100.0 * distinct / (double)n);
    printf("chi^2    = %.2f   (df=19; >30.14 fails p=.05, >36.19 fails p=.01)\n", chisq);
    double unit = expv / 25.0;
    for (int b = 0; b < RAND_DIST_BINS; b++) {
        printf("  [%.2f,%.2f) %7ld ", b / (double)RAND_DIST_BINS,
               (b + 1) / (double)RAND_DIST_BINS, bins[b]);
        long bars = unit > 0 ? (long)(bins[b] / unit) : 0;
        for (long k = 0; k < bars && k < 60; k++) putchar('#');
        putchar('\n');
    }
}

static void rand_dist_table(long n) {
    static const int Cs[] = { 0, 1, 7 };
    if (n < 1) n = 100000;
    printf("rand() uniformity table  (%ld samples per pattern, seed offset %.2f)\n",
           n, 0.5);
    for (int k = 0; k < 3; k++) {
        char lbl[64];
        snprintf(lbl, sizeof lbl, "rand(i, %d)  [seed varies, iter const]", Cs[k]);
        rand_dist_row(lbl, 1, Cs[k], n);
    }
    for (int k = 0; k < 3; k++) {
        char lbl[64];
        snprintf(lbl, sizeof lbl, "rand(%d, i)  [seed const, iter varies]", Cs[k]);
        rand_dist_row(lbl, 0, Cs[k], n);
    }
}

/* ---- Interactive REPL ------------------------------------------------- */

static void interactive(void) {
    printf("REPL eval test harness (type 'help' for commands, 'q' to quit)\n\n");
    char line[512];
    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        if (strcmp(line, "q") == 0 || strcmp(line, "quit") == 0) break;

        if (strcmp(line, "help") == 0) {
            printf("  <expr>           evaluate expression\n");
            printf("  set <var> <val>  set predefined variable\n");
            printf("  to_c <expr>      translate REPL -> C\n");
            printf("  to_repl <expr>   translate C -> REPL\n");
            printf("  for <header>     parse REPL for-header\n");
            printf("  cfor <header>    parse C for-header\n");
            printf("  vars             show predefined variables\n");
            printf("  randdist [N]     rand() uniformity table (N samples, default 100000)\n");
            printf("  quit / q         exit\n");
            continue;
        }

        if (strcmp(line, "randdist") == 0 || strncmp(line, "randdist ", 9) == 0) {
            long n = (line[8] == ' ') ? strtol(line + 9, NULL, 10) : 0;
            rand_dist_table(n);
            continue;
        }

        if (strcmp(line, "vars") == 0) {
            for (int i = 0; i < g_num_predef_vars; i++)
                printf("  %s = %g\n", g_predef_vars[i].name, g_predef_vars[i].value);
            continue;
        }

        if (strncmp(line, "set ", 4) == 0) {
            char vname[REPL_PREDEF_NAME_MAX]; float val;
            if (sscanf(line + 4, "%15s %f", vname, &val) == 2) {
                int found = 0;
                for (int i = 0; i < g_num_predef_vars; i++) {
                    if (strcmp(g_predef_vars[i].name, vname) == 0) {
                        g_predef_vars_mut[i].value = val;
                        printf("  %s = %g\n", vname, val);
                        found = 1;
                        break;
                    }
                }
                if (!found) printf("  unknown variable: %s\n", vname);
            } else {
                printf("  usage: set <var> <value>\n");
            }
            continue;
        }

        if (strncmp(line, "to_c ", 5) == 0) {
            char buf[512];
            repl_eval_expr_to_c(line + 5, buf, sizeof(buf));
            printf("  %s\n", buf);
            continue;
        }

        if (strncmp(line, "to_repl ", 8) == 0) {
            char buf[512];
            repl_eval_c_expr_to_repl(line + 8, buf, sizeof(buf));
            printf("  %s\n", buf);
            continue;
        }

        if (strncmp(line, "for ", 4) == 0) {
            char vn[16]; float s, e, st; const char *body;
            if (repl_eval_parse_for_header(&(ReplForHeaderParseConfig){
                    .input = line + 4,
                    .var_name = vn,
                    .var_sz = (int)sizeof(vn),
                    .start = &s,
                    .end = &e,
                    .step = &st,
                    .body_start = &body,
                }))
                printf("  var=%s  start=%g  end=%g  step=%g  body=\"%s\"\n",
                       vn, s, e, st, body);
            else
                printf("  parse failed\n");
            continue;
        }

        if (strncmp(line, "cfor ", 5) == 0) {
            char vn[16]; float s, e, st;
            if (repl_eval_parse_c_for_header(line + 5, vn, sizeof(vn), &s, &e, &st))
                printf("  var=%s  start=%g  end=%g  step=%g\n", vn, s, e, st);
            else
                printf("  parse failed\n");
            continue;
        }

        /* Default: evaluate as expression */
        ExprCtx ctx = { line, NULL, 0 };
        float val = repl_eval_expr(&ctx);
        printf("  = %g", val);
        if (*ctx.p) printf("  (stopped at: \"%s\")", ctx.p);
        printf("\n");
    }
}

int main(int argc, char *argv[]) {
    repl_eval_init_predef_vars();

    if (argc > 1 && strcmp(argv[1], "--run-tests") == 0) {
        run_tests();
        return (g_harness.passed == g_harness.run) ? 0 : 1;
    }

    if (argc > 1 && strcmp(argv[1], "--rand-dist") == 0) {
        rand_dist_table(argc > 2 ? strtol(argv[2], NULL, 10) : 0);
        return 0;
    }

    interactive();
    return 0;
}
