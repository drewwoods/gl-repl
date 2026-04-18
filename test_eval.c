/*
 * test_eval.c — Standalone test harness for the REPL expression evaluator
 *
 * Build:
 *   gcc -Wall -std=c2x -o test_eval test_eval.c repl_eval.c -lm
 *
 * Usage:
 *   ./test_eval                    # interactive REPL
 *   ./test_eval --run-tests        # run built-in test suite
 *
 * Interactive commands:
 *   <expr>                         eval expression: 1+2, sin(PI/4), x*2+1
 *   set <var> <value>              set predefined var: set x 1.5
 *   to_c <expr>                    translate REPL->C: to_c sin(TAU/n)
 *   to_repl <expr>                 translate C->REPL: to_repl sinf(M_PI)
 *   for <header>                   parse REPL for:    for for(i, 0, n)
 *   cfor <header>                  parse C for:       cfor for (float i = 0; i < 10; i += 1.0f) {
 *   vars                           show all predefined vars
 *   quit / q                       exit
 */
#include "repl_eval.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* ---- Built-in test suite ---------------------------------------------- */

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define ASSERT_FLOAT(expr_str, expected) do { \
    ExprCtx _ctx = { (expr_str), NULL, 0 }; \
    float _got = eval_expr(&_ctx); \
    float _exp = (expected); \
    g_tests_run++; \
    if (fabsf(_got - _exp) < 1e-4f) { \
        g_tests_passed++; \
    } else { \
        printf("  FAIL: eval(\"%s\") = %g, expected %g\n", expr_str, _got, _exp); \
    } \
} while(0)

#define ASSERT_EXPRS(expr_str, n_expected, ...) do { \
    float _vals[8]; \
    int _n = parse_exprs(expr_str, _vals, 8, NULL, 0); \
    float _exp[] = { __VA_ARGS__ }; \
    g_tests_run++; \
    if (_n != n_expected) { \
        printf("  FAIL: parse_exprs(\"%s\") returned %d args, expected %d\n", \
               expr_str, _n, n_expected); \
    } else { \
        int _ok = 1; \
        for (int _i = 0; _i < _n; _i++) \
            if (fabsf(_vals[_i] - _exp[_i]) > 1e-4f) _ok = 0; \
        if (_ok) g_tests_passed++; \
        else { \
            printf("  FAIL: parse_exprs(\"%s\") values:", expr_str); \
            for (int _i = 0; _i < _n; _i++) printf(" %g", _vals[_i]); \
            printf(" (expected"); \
            for (int _i = 0; _i < n_expected; _i++) printf(" %g", _exp[_i]); \
            printf(")\n"); \
        } \
    } \
} while(0)

#define ASSERT_TO_C(in, expected) do { \
    char _buf[512]; \
    repl_expr_to_c(in, _buf, sizeof(_buf)); \
    g_tests_run++; \
    if (strcmp(_buf, expected) == 0) { \
        g_tests_passed++; \
    } else { \
        printf("  FAIL: to_c(\"%s\") = \"%s\", expected \"%s\"\n", in, _buf, expected); \
    } \
} while(0)

#define ASSERT_TO_REPL(in, expected) do { \
    char _buf[512]; \
    c_expr_to_repl(in, _buf, sizeof(_buf)); \
    g_tests_run++; \
    if (strcmp(_buf, expected) == 0) { \
        g_tests_passed++; \
    } else { \
        printf("  FAIL: to_repl(\"%s\") = \"%s\", expected \"%s\"\n", in, _buf, expected); \
    } \
} while(0)

#define ASSERT_FOR(input, expect_ok, e_var, e_start, e_end, e_step) do { \
    char _vn[16]; float _s, _e, _st; const char *_b; \
    int _ok = parse_for_header(input, _vn, sizeof(_vn), &_s, &_e, &_st, &_b); \
    g_tests_run++; \
    if (_ok != expect_ok) { \
        printf("  FAIL: for(\"%s\") returned %d, expected %d\n", input, _ok, expect_ok); \
    } else if (_ok && (strcmp(_vn, e_var) != 0 || \
                       fabsf(_s - (e_start)) > 1e-4f || \
                       fabsf(_e - (e_end)) > 1e-4f || \
                       fabsf(_st - (e_step)) > 1e-4f)) { \
        printf("  FAIL: for(\"%s\") -> var=%s s=%g e=%g st=%g" \
               " (expected %s %g %g %g)\n", \
               input, _vn, _s, _e, _st, e_var, (float)(e_start), (float)(e_end), (float)(e_step)); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

#define ASSERT_CFOR(input, expect_ok, e_var, e_start, e_end, e_step) do { \
    char _vn[16]; float _s, _e, _st; \
    int _ok = parse_c_for_header(input, _vn, sizeof(_vn), &_s, &_e, &_st); \
    g_tests_run++; \
    if (_ok != expect_ok) { \
        printf("  FAIL: cfor(\"%s\") returned %d, expected %d\n", input, _ok, expect_ok); \
    } else if (_ok && (strcmp(_vn, e_var) != 0 || \
                       fabsf(_s - (e_start)) > 1e-4f || \
                       fabsf(_e - (e_end)) > 1e-4f || \
                       fabsf(_st - (e_step)) > 1e-4f)) { \
        printf("  FAIL: cfor(\"%s\") -> var=%s s=%g e=%g st=%g" \
               " (expected %s %g %g %g)\n", \
               input, _vn, _s, _e, _st, e_var, (float)(e_start), (float)(e_end), (float)(e_step)); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

#define ASSERT_HAS_VARS(input, expected) do { \
    int _got = input_has_predef_vars(input); \
    g_tests_run++; \
    if (_got == expected) { g_tests_passed++; } \
    else { printf("  FAIL: has_vars(\"%s\") = %d, expected %d\n", input, _got, expected); } \
} while(0)

#define ASSERT_DECLARE_OK(name) do { \
    char _err[128] = {0}; \
    int _ok = declare_predef_var(name, _err, sizeof(_err)); \
    g_tests_run++; \
    if (_ok) { g_tests_passed++; } \
    else { printf("  FAIL: declare_predef_var(\"%s\") should succeed, got: %s\n", name, _err); } \
} while(0)

#define ASSERT_DECLARE_FAIL(name) do { \
    char _err[128] = {0}; \
    int _ok = declare_predef_var(name, _err, sizeof(_err)); \
    g_tests_run++; \
    if (!_ok) { g_tests_passed++; } \
    else { printf("  FAIL: declare_predef_var(\"%s\") should fail\n", name); } \
} while(0)

#define ASSERT_VALIDATE_OK(src, vars, nv) do { \
    char _err[128] = {0}; \
    int _ok = validate_expression_idents(src, vars, nv, _err, sizeof(_err)); \
    g_tests_run++; \
    if (_ok) { g_tests_passed++; } \
    else { printf("  FAIL: validate_expr(\"%s\") should pass, got: %s\n", src, _err); } \
} while(0)

#define ASSERT_VALIDATE_FAIL(src, vars, nv) do { \
    char _err[128] = {0}; \
    int _ok = validate_expression_idents(src, vars, nv, _err, sizeof(_err)); \
    g_tests_run++; \
    if (!_ok) { g_tests_passed++; } \
    else { printf("  FAIL: validate_expr(\"%s\") should fail\n", src); } \
} while(0)

#define ASSERT_SOURCE_USES(src, name, expected) do { \
    int _got = source_uses_ident(src, name); \
    g_tests_run++; \
    if (_got == expected) { g_tests_passed++; } \
    else { printf("  FAIL: source_uses_ident(\"%s\", \"%s\") = %d, expected %d\n", \
                  src, name, _got, expected); } \
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
        printf("  BUG: set_predef(\"%s\") — var not declared\n", name);
        return;
    }
    g_predef_vars[idx].value = val;
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
    char _cbuf[512], _rbuf[512], _n_in[512], _n_out[512]; \
    repl_expr_to_c((in), _cbuf, sizeof(_cbuf)); \
    c_expr_to_repl(_cbuf, _rbuf, sizeof(_rbuf)); \
    strip_ws((in), _n_in, sizeof(_n_in)); \
    strip_ws(_rbuf, _n_out, sizeof(_n_out)); \
    g_tests_run++; \
    if (strcmp(_n_in, _n_out) == 0) { \
        g_tests_passed++; \
    } else { \
        printf("  FAIL: roundtrip_ws \"%s\" -> \"%s\" -> \"%s\" (norm \"%s\" != \"%s\")\n", \
               (in), _cbuf, _rbuf, _n_in, _n_out); \
    } \
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
    ASSERT_FLOAT("sin(0)", 0.0f);
    ASSERT_FLOAT("cos(0)", 1.0f);
    ASSERT_FLOAT("tan(0)", 0.0f);
    ASSERT_FLOAT("sqrt(4)", 2.0f);
    ASSERT_FLOAT("abs(-3)", 3.0f);
    ASSERT_FLOAT("pow(2,3)", 8.0f);
    ASSERT_FLOAT("min(3,5)", 3.0f);
    ASSERT_FLOAT("max(3,5)", 5.0f);
    ASSERT_FLOAT("floor(2.7)", 2.0f);
    ASSERT_FLOAT("ceil(2.3)", 3.0f);
    ASSERT_FLOAT("fmod(7,3)", 1.0f);
    ASSERT_FLOAT("rand(7,11)", 0.564453f);
    ASSERT_FLOAT("rand(7,11)", 0.564453f); /* deterministic */
    ASSERT_FLOAT("rand(3)", 0.589844f);    /* implicit iter=0 */
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
        init_predef_vars();

        t_idx = predef_idx("t");
        g_tests_run++;
        if (t_idx >= 0) g_tests_passed++;
        else printf("  FAIL: predefined var t missing\n");

        g_tests_run++;
        if (t_idx >= 0 && fabsf(g_predef_vars[t_idx].value - 0.0f) < 1e-6f) g_tests_passed++;
        else printf("  FAIL: predefined var t initial value not zero\n");

        g_tests_run++;
        if (g_num_predef_vars == 1) g_tests_passed++;
        else printf("  FAIL: init_predef_vars should register only t, got %d\n", g_num_predef_vars);

        g_tests_run++;
        if (predef_idx("not_a_predef") == -1) g_tests_passed++;
        else printf("  FAIL: unknown predefined var lookup did not return -1\n");

        declare_predef_var("x", err, sizeof(err));
        declare_predef_var("y", err, sizeof(err));
        declare_predef_var("z", err, sizeof(err));
        declare_predef_var("n", err, sizeof(err));
        declare_predef_var("i", err, sizeof(err));
        declare_predef_var("j", err, sizeof(err));
        declare_predef_var("k", err, sizeof(err));

        g_tests_run++;
        if (predef_idx("x") >= 0) g_tests_passed++;
        else printf("  FAIL: declared var x missing\n");
        g_tests_run++;
        if (predef_idx("y") >= 0) g_tests_passed++;
        else printf("  FAIL: declared var y missing\n");
        g_tests_run++;
        if (predef_idx("z") >= 0) g_tests_passed++;
        else printf("  FAIL: declared var z missing\n");
        g_tests_run++;
        if (predef_idx("n") >= 0) g_tests_passed++;
        else printf("  FAIL: declared var n missing\n");
    }

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

    /* Loop vars override predefined */
    {
        ExprVar lv[1] = { { "x", 99.0f } };
        ExprCtx ctx = { "x", lv, 1 };
        float v = eval_expr(&ctx);
        g_tests_run++;
        if (fabsf(v - 99.0f) < 1e-4f) g_tests_passed++;
        else printf("  FAIL: loop var override: got %g, expected 99\n", v);
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
        int n = parse_exprs("radius + height, 0", vals, 4, vars, 2);
        g_tests_run++;
        if (n == 2 && fabsf(vals[0] - 7.0f) < 1e-4f && fabsf(vals[1]) < 1e-4f) {
            g_tests_passed++;
        } else {
            printf("  FAIL: parse_exprs with vars returned n=%d vals=(%g,%g)\n",
                   n, vals[0], vals[1]);
        }
    }

    /* ---- Expression translation ---- */
    printf("repl_expr_to_c:\n");
    ASSERT_TO_C("sin(x)", "sinf(x)");
    ASSERT_TO_C("cos(PI/4)", "cosf(M_PI/4)");
    ASSERT_TO_C("TAU/n", "(2*M_PI)/n");
    ASSERT_TO_C("sqrt(x*x+y*y)", "sqrtf(x*x+y*y)");
    ASSERT_TO_C("abs(-1)", "fabsf(-1)");
    ASSERT_TO_C("glVertex3f(1,2,3)", "glVertex3f(1,2,3)");  /* unchanged */
    ASSERT_TO_C("pow(x,2)", "powf(x,2)");
    ASSERT_TO_C("min(x,y)", "fminf(x,y)");
    ASSERT_TO_C("rand(i,3)", "repl_randf(i,3)");
    ASSERT_TO_C("x % 2", "fmodf(x, 2)");
    ASSERT_TO_C("10 % 3", "fmodf(10, 3)");
    ASSERT_TO_C("(x+y) % (z*2)", "fmodf((x+y), (z*2))");
    ASSERT_TO_C("sin(x) % 1", "fmodf(sinf(x), 1)");

    printf("c_expr_to_repl:\n");
    ASSERT_TO_REPL("sinf(x)", "sin(x)");
    ASSERT_TO_REPL("cosf(M_PI/4)", "cos(PI/4)");
    ASSERT_TO_REPL("(2*M_PI)/n", "TAU/n");
    ASSERT_TO_REPL("sqrtf(x*x+y*y)", "sqrt(x*x+y*y)");
    ASSERT_TO_REPL("fabsf(-1)", "abs(-1)");
    ASSERT_TO_REPL("glVertex3f(1,2,3)", "glVertex3f(1,2,3)");
    ASSERT_TO_REPL("powf(x,2)", "pow(x,2)");
    ASSERT_TO_REPL("powf(1.0f,2.0f)", "pow(1.0f,2.0f)");
    ASSERT_TO_REPL("repl_randf(i,3)", "rand(i,3)");
    ASSERT_FLOAT("pow(1.0f,2.0f)", 1.0f);
    {
        char tiny[5];
        c_expr_to_repl("sinf(x)", tiny, sizeof(tiny));
        g_tests_run++;
        if (strcmp(tiny, "sin(") == 0) g_tests_passed++;
        else printf("  FAIL: c_expr_to_repl truncation -> \"%s\"\n", tiny);

        strcpy(tiny, "keep");
        c_expr_to_repl("sinf(x)", tiny, 0);
        g_tests_run++;
        if (strcmp(tiny, "keep") == 0) g_tests_passed++;
        else printf("  FAIL: c_expr_to_repl out_sz=0 changed output -> \"%s\"\n", tiny);
    }
    {
        char buf[8];
        strip_ws(" \t a b \n", buf, sizeof(buf));
        g_tests_run++;
        if (strcmp(buf, "ab") == 0) g_tests_passed++;
        else printf("  FAIL: strip_ws mixed whitespace -> \"%s\"\n", buf);

        strip_ws("   \n\t", buf, sizeof(buf));
        g_tests_run++;
        if (strcmp(buf, "") == 0) g_tests_passed++;
        else printf("  FAIL: strip_ws whitespace-only -> \"%s\"\n", buf);

        strip_ws("a b c d", buf, 4);
        g_tests_run++;
        if (strcmp(buf, "abc") == 0) g_tests_passed++;
        else printf("  FAIL: strip_ws truncation -> \"%s\"\n", buf);

        strcpy(buf, "keep");
        strip_ws(NULL, buf, sizeof(buf));
        g_tests_run++;
        if (strcmp(buf, "keep") == 0) g_tests_passed++;
        else printf("  FAIL: strip_ws null input changed output -> \"%s\"\n", buf);
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
            char c_buf[512], repl_buf[512];
            repl_expr_to_c(cases[ci], c_buf, sizeof(c_buf));
            c_expr_to_repl(c_buf, repl_buf, sizeof(repl_buf));
            g_tests_run++;
            if (strcmp(cases[ci], repl_buf) == 0) {
                g_tests_passed++;
            } else {
                printf("  FAIL: roundtrip \"%s\" -> \"%s\" -> \"%s\"\n",
                       cases[ci], c_buf, repl_buf);
            }
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
        int ok = parse_for_header_with_vars(
            "for(i, 0, radius, stepv) glVertex3f(i, 0, 0);",
            vn, sizeof(vn), &s, &e, &st, vars, 2, &body);
        bp = body;
        while (bp && *bp && isspace((unsigned char)*bp))
            bp++;
        g_tests_run++;
        if (ok == 1 &&
            strcmp(vn, "i") == 0 &&
            fabsf(s - 0.0f) < 1e-4f &&
            fabsf(e - 7.5f) < 1e-4f &&
            fabsf(st - 0.5f) < 1e-4f &&
            bp != NULL &&
            strncmp(bp, "glVertex3f(", 11) == 0) {
            g_tests_passed++;
        } else {
            printf("  FAIL: parse_for_header_with_vars did not resolve local vars\n");
        }
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

    /* ---- Expression evaluator — additional coverage ---- */
    printf("Expression evaluator (additional):\n");

    /* Math functions with less obvious inputs */
    ASSERT_FLOAT("sin(PI)", 0.0f);                     /* sin(π) ≈ 0 */
    ASSERT_FLOAT("sqrt(9)", 3.0f);
    ASSERT_FLOAT("sqrt(0)", 0.0f);
    ASSERT_FLOAT("abs(-3.5)", 3.5f);
    ASSERT_FLOAT("ceil(-2.3)", -2.0f);
    ASSERT_FLOAT("floor(-2.3)", -3.0f);
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

    /* Functions with wrong arg count — fall through to 0.0 */
    ASSERT_FLOAT("pow(2)", 0.0f);                      /* needs 2 args */
    ASSERT_FLOAT("min(3)", 0.0f);
    ASSERT_FLOAT("max(7)", 0.0f);

    /* ---- declare_predef_var error paths ---- */
    printf("declare_predef_var (error paths):\n");
    {
        char err[128];
        int ok;

        /* Empty name */
        ok = declare_predef_var("", err, sizeof(err));
        g_tests_run++;
        if (!ok) g_tests_passed++;
        else printf("  FAIL: empty name should fail\n");

        /* Name starts with a digit */
        ok = declare_predef_var("3x", err, sizeof(err));
        g_tests_run++;
        if (!ok) g_tests_passed++;
        else printf("  FAIL: digit-start name should fail\n");

        /* Name contains invalid character */
        ok = declare_predef_var("x@y", err, sizeof(err));
        g_tests_run++;
        if (!ok) g_tests_passed++;
        else printf("  FAIL: invalid char name should fail\n");

        /* Reserved built-in name */
        ok = declare_predef_var("sin", err, sizeof(err));
        g_tests_run++;
        if (!ok) g_tests_passed++;
        else printf("  FAIL: reserved name 'sin' should fail\n");

        ok = declare_predef_var("PI", err, sizeof(err));
        g_tests_run++;
        if (!ok) g_tests_passed++;
        else printf("  FAIL: reserved name 'PI' should fail\n");

        ok = declare_predef_var("t", err, sizeof(err));
        g_tests_run++;
        if (!ok) g_tests_passed++;
        else printf("  FAIL: reserved name 't' should fail (already declared)\n");

        /* Already declared */
        ok = declare_predef_var("x", err, sizeof(err));
        g_tests_run++;
        if (!ok) g_tests_passed++;
        else printf("  FAIL: re-declaring 'x' should fail\n");

        /* Name too long (>= 16 chars) */
        ok = declare_predef_var("averylongvarname1", err, sizeof(err));
        g_tests_run++;
        if (!ok) g_tests_passed++;
        else printf("  FAIL: too-long name should fail\n");

        /* Valid new declaration */
        ASSERT_DECLARE_OK("alpha");
        ASSERT_DECLARE_OK("_beta");

        /* Undeclare and re-declare */
        undeclare_predef_var("alpha");
        g_tests_run++;
        if (find_predef_var_idx("alpha") == -1) g_tests_passed++;
        else printf("  FAIL: 'alpha' should be gone after undeclare\n");

        ASSERT_DECLARE_OK("alpha");   /* re-declare after undeclare */

        /* Undeclare non-existent — no-op, count unchanged */
        int count_before = g_num_predef_vars;
        undeclare_predef_var("nosuchvar");
        g_tests_run++;
        if (g_num_predef_vars == count_before) g_tests_passed++;
        else printf("  FAIL: undeclare non-existent changed count\n");

        /* Clean up extras so later tests aren't affected */
        undeclare_predef_var("alpha");
        undeclare_predef_var("_beta");
    }

    /* ---- validate_expression_idents ---- */
    printf("validate_expression_idents:\n");
    {
        ExprVar lv[2] = { { "r", 1.0f }, { "h", 2.0f } };

        /* Pure number — no identifiers at all */
        ASSERT_VALIDATE_OK("3.14", NULL, 0);
        ASSERT_VALIDATE_OK("42 + 1", NULL, 0);

        /* Constants PI and TAU are always allowed */
        ASSERT_VALIDATE_OK("PI * 2", NULL, 0);
        ASSERT_VALIDATE_OK("TAU / 4", NULL, 0);

        /* Function calls are allowed (identifier followed by '(') */
        ASSERT_VALIDATE_OK("sin(x)", lv, 2);   /* sin is a function call */
        ASSERT_VALIDATE_OK("sqrt(r*r + h*h)", lv, 2);

        /* Declared predefined var */
        ASSERT_VALIDATE_OK("x + 1", NULL, 0);  /* x is a predef var */

        /* Loop-local var provided in vars array */
        ASSERT_VALIDATE_OK("r + h", lv, 2);

        /* Undeclared variable — should fail */
        ASSERT_VALIDATE_FAIL("undefined_var", NULL, 0);
        ASSERT_VALIDATE_FAIL("r + unknown", lv, 2);

        /* Inline comment — scanner stops before it */
        ASSERT_VALIDATE_OK("x + 1 // comment with undefined_var", NULL, 0);

        /* Identifier too long (>= 16 chars) */
        ASSERT_VALIDATE_FAIL("averylongnamethatoverflows", NULL, 0);
    }

    /* ---- source_uses_ident ---- */
    printf("source_uses_ident:\n");
    ASSERT_SOURCE_USES("sin(x) + y", "x", 1);
    ASSERT_SOURCE_USES("sin(x) + y", "y", 1);
    ASSERT_SOURCE_USES("sin(x) + y", "z", 0);
    ASSERT_SOURCE_USES("glVertex3f(1,2,3)", "x", 0);
    /* Substring — "foo" must not match inside "foobar" */
    ASSERT_SOURCE_USES("foobar + 1", "foo", 0);
    ASSERT_SOURCE_USES("foo + foobar", "foo", 1);
    /* Numeric literal that starts with digits — "3" in "3.14" */
    ASSERT_SOURCE_USES("3.14 + x", "x", 1);
    ASSERT_SOURCE_USES("3.14", "x", 0);
    /* Empty source */
    ASSERT_SOURCE_USES("", "x", 0);

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
    ASSERT_TO_C("PI", "M_PI");
    ASSERT_TO_C("", "");
    ASSERT_TO_C("x + y", "x + y");                    /* passthrough, no substitution */
    ASSERT_TO_C("fmod(x, 3)", "fmodf(x, 3)");   /* fmod keyword -> fmodf */

    /* ---- c_expr_to_repl additional translations ---- */
    printf("c_expr_to_repl (additional):\n");
    ASSERT_TO_REPL("fmaxf(x,y)", "max(x,y)");
    ASSERT_TO_REPL("floorf(x/2)", "floor(x/2)");
    ASSERT_TO_REPL("ceilf(x+1)", "ceil(x+1)");
    ASSERT_TO_REPL("M_PI", "PI");
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
    /* Missing end (only one value after var) — should fail */
    ASSERT_FOR("for(i, 0)", 0, "", 0, 0, 0);
    /* Empty var name */
    ASSERT_FOR("for(, 0, 10)", 0, "", 0, 0, 0);
    /* Nested expression in end */
    ASSERT_FOR("for(i, 0, sin(0)+5)", 1, "i", 0.0f, 5.0f, 1.0f);

    /* body_start points right after the closing ')' */
    {
        char vn[16]; float s, e, st;
        const char *body = NULL;
        int ok = parse_for_header("for(i, 0, 5) glVertex3f(0,0,0);",
                                  vn, sizeof(vn), &s, &e, &st, &body);
        g_tests_run++;
        if (ok && body != NULL && strncmp(body, " glVertex3f", 11) == 0)
            g_tests_passed++;
        else
            printf("  FAIL: for body_start: ok=%d body=\"%s\"\n",
                   ok, body ? body : "(null)");
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
        /* 'x' is currently declared — should be found */
        ASSERT_HAS_VARS("x + 1", 1);

        /* undeclare 'x', then it should no longer match */
        undeclare_predef_var("x");
        ASSERT_HAS_VARS("x + 1", 0);

        /* restore 'x' for any follow-on code */
        declare_predef_var("x", NULL, 0);
        set_predef("x", 0.0f);
    }

    /* ---- Summary ---- */
    printf("\n%d / %d tests passed", g_tests_passed, g_tests_run);
    if (g_tests_passed == g_tests_run)
        printf(" — all OK!\n");
    else
        printf(" — %d FAILED\n", g_tests_run - g_tests_passed);
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
            printf("  quit / q         exit\n");
            continue;
        }

        if (strcmp(line, "vars") == 0) {
            for (int i = 0; i < g_num_predef_vars; i++)
                printf("  %s = %g\n", g_predef_vars[i].name, g_predef_vars[i].value);
            continue;
        }

        if (strncmp(line, "set ", 4) == 0) {
            char vname[16]; float val;
            if (sscanf(line + 4, "%15s %f", vname, &val) == 2) {
                int found = 0;
                for (int i = 0; i < g_num_predef_vars; i++) {
                    if (strcmp(g_predef_vars[i].name, vname) == 0) {
                        g_predef_vars[i].value = val;
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
            repl_expr_to_c(line + 5, buf, sizeof(buf));
            printf("  %s\n", buf);
            continue;
        }

        if (strncmp(line, "to_repl ", 8) == 0) {
            char buf[512];
            c_expr_to_repl(line + 8, buf, sizeof(buf));
            printf("  %s\n", buf);
            continue;
        }

        if (strncmp(line, "for ", 4) == 0) {
            char vn[16]; float s, e, st; const char *body;
            if (parse_for_header(line + 4, vn, sizeof(vn), &s, &e, &st, &body))
                printf("  var=%s  start=%g  end=%g  step=%g  body=\"%s\"\n",
                       vn, s, e, st, body);
            else
                printf("  parse failed\n");
            continue;
        }

        if (strncmp(line, "cfor ", 5) == 0) {
            char vn[16]; float s, e, st;
            if (parse_c_for_header(line + 5, vn, sizeof(vn), &s, &e, &st))
                printf("  var=%s  start=%g  end=%g  step=%g\n", vn, s, e, st);
            else
                printf("  parse failed\n");
            continue;
        }

        /* Default: evaluate as expression */
        ExprCtx ctx = { line, NULL, 0 };
        float val = eval_expr(&ctx);
        printf("  = %g", val);
        if (*ctx.p) printf("  (stopped at: \"%s\")", ctx.p);
        printf("\n");
    }
}

int main(int argc, char *argv[]) {
    init_predef_vars();

    if (argc > 1 && strcmp(argv[1], "--run-tests") == 0) {
        run_tests();
        return (g_tests_passed == g_tests_run) ? 0 : 1;
    }

    interactive();
    return 0;
}
