/*
 * test_expr_program.c - Compiled-expression parity and cache unit tests.
 *
 * The compiled evaluator (src/repl/expr_program.c) claims bit-identical
 * results with the text evaluator (repl_eval_expr) for every expression it
 * accepts, and clean fallback (-1 from compile) for everything else. This
 * suite drives both claims:
 *
 *   - a differential corpus of expressions evaluated under several binding
 *     sets, compared bit-exactly (memcmp on the float, so NaN==NaN is the
 *     NaN parity case);
 *   - targeted unit checks for the dependency-mask algebra (constants,
 *     predef bits, threaded predef masks, local masks, unions);
 *   - compile-time rejection of constructs the text evaluator resolves with
 *     an error (arity mismatch, unknown call target, missing ']', shadowed
 *     builtin call names);
 *   - list compilation mirroring parse_expr_list_exact (strict) and
 *     repl_eval_parse_exprs (lenient);
 *   - the per-line entry lifecycle (begin/add/finish/find, failure rollback,
 *     invalidation).
 */

#include <stdio.h>
#include <string.h>

#include "repl/eval.h"
#include "repl/expr_program.h"
#include "repl/text_helpers.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;

/* Bit-exact float comparison: NaN patterns count as equal when identical. */
static int float_bits_equal(float a, float b) {
    return memcmp(&a, &b, sizeof(float)) == 0 ||
           (isnan(a) && isnan(b));
}

static ReplExprCompileEnv compile_env_with_locals(const ExprVar *locals,
                                                  int num_locals) {
    ReplExprCompileEnv env;
    memset(&env, 0, sizeof(env));
    env.predef = repl_eval_predef_view();
    env.locals = locals;
    env.num_locals = num_locals;
    return env;
}

static ReplExprEvalEnv eval_env_with_locals(const ExprVar *locals,
                                            int num_locals) {
    ReplExprEvalEnv env;
    ReplPredefView predef = repl_eval_predef_view();
    memset(&env, 0, sizeof(env));
    env.locals = locals;
    env.num_locals = num_locals;
    env.predef_vars = predef.vars;
    env.predef_count = predef.count;
    return env;
}

/* Compile `src` and compare the compiled value bit-exactly against
 * repl_eval_expr under the same bindings. Returns the program handle so
 * callers can chain further checks; asserts compile success. */
static int check_parity(ReplExprCache *cache, const char *src,
                        const ExprVar *locals, int num_locals) {
    ReplExprCompileEnv cenv = compile_env_with_locals(locals, num_locals);
    ReplExprEvalEnv eenv = eval_env_with_locals(locals, num_locals);
    int prog = repl_expr_cache_compile(cache, src, &cenv);
    char label[160];

    snprintf(label, sizeof(label), "compiles: %s", src);
    TEST_ASSERT_TRUE(&g_harness, label, prog >= 0);
    if (prog < 0)
        return -1;

    {
        ExprCtx ctx = { src, locals, num_locals, NULL, 0, NULL, 0 };
        float expected = repl_eval_expr(&ctx);
        ReplExprValue got = repl_expr_program_eval(cache, prog, &eenv);
        snprintf(label, sizeof(label), "parity: %s", src);
        TEST_ASSERT_TRUE(&g_harness, label,
                         float_bits_equal(got.value, expected));
        if (!float_bits_equal(got.value, expected))
            printf("       compiled=%g text=%g\n",
                   (double)got.value, (double)expected);
    }
    return prog;
}

static void check_compile_fails(ReplExprCache *cache, const char *src,
                                const ExprVar *locals, int num_locals) {
    ReplExprCompileEnv cenv = compile_env_with_locals(locals, num_locals);
    int prog = repl_expr_cache_compile(cache, src, &cenv);
    char label[160];
    snprintf(label, sizeof(label), "compile fails: %s", src);
    TEST_ASSERT_TRUE(&g_harness, label, prog < 0);
}

/* ---- Differential corpus ------------------------------------------------- */

static void test_parity_corpus(void) {
    ReplExprCache *cache = repl_expr_cache_create();
    ExprVar locals[3];

    repl_eval_init_predef_vars();
    repl_eval_declare_predef_var("radius", NULL, 0);
    repl_eval_declare_predef_var("n", NULL, 0);
    repl_eval_predef_vars_mut()[0].value = 2.5f;      /* t */
    repl_eval_predef_vars_mut()[1].value = 1.75f;     /* radius */
    repl_eval_predef_vars_mut()[2].value = -3.0f;     /* n */

    repl_eval_reset_scratch_arrays();
    repl_eval_scratch_set(0, 0, 4.5f);    /* A[0] */
    repl_eval_scratch_set(0, 3, -1.5f);   /* A[3] */
    repl_eval_scratch_set(1, 2, 7.0f);    /* B[2] */
    repl_eval_scratch_set(2, 15, 0.25f);  /* C[15] */

    strcpy(locals[0].name, "i");
    locals[0].value = 3.0f;
    strcpy(locals[1].name, "phase");
    locals[1].value = 0.6f;
    strcpy(locals[2].name, "t");          /* shadows the predef t */
    locals[2].value = 100.0f;

    static const char *const k_exprs[] = {
        /* literals + precedence */
        "1", "1.5", "1.5f", ".5", "0.0", "42",
        "1 + 2 * 3", "(1 + 2) * 3", "2 * 3 + 4 * 5",
        "10 - 4 - 3", "100 / 5 / 2", "7 % 3", "7.5 % 2",
        "1 + 2 - 3 * 4 / 5 % 6",
        /* unary */
        "-1", "+1", "- -2", "-(3 + 4)", "!0", "!1", "!!5", "-!0",
        /* division/modulo guards */
        "1 / 0", "5 % 0", "1 / 0.0000000000001", "1 / (2 - 2)",
        /* comparisons + epsilon equality */
        "1 < 2", "2 < 1", "2 <= 2", "3 >= 4", "1 > 0", "0 > 1",
        "1 == 1", "1 == 1.0000005", "1 != 1.0000005", "1 != 2",
        "1 < 2 < 3", "5 > 4 > 0",
        /* logicals (non-short-circuit) */
        "1 && 1", "1 && 0", "0 || 1", "0 || 0", "1 && 2 || 0",
        "1 < 2 && 3 > 2", "1 / 0 || 1",
        /* constants */
        "PI", "TAU", "e", "2 * PI", "PI / 2", "NAN", "INFINITY",
        "nan", "inf", "infinity", "-INFINITY", "NAN + 1", "0 * INFINITY",
        /* builtins, all of them */
        "sin(1)", "cos(1)", "tan(0.5)", "sqrt(2)", "sqrt(-4)", "abs(-3)",
        "asin(0.5)", "asin(2)", "acos(0.5)", "acos(-2)",
        "atan(0.5)", "atan(-3)",
        "atan2(1, 2)", "atan2(-1, -2)", "atan2(0, 0)",
        "pow(2, 10)", "log(100)", "ln(e)", "min(3, 4)", "max(3, 4)",
        "clamp(5, 0, 1)", "clamp(-5, 0, 1)", "clamp(0.5, 0, 1)",
        "lerp(0, 10, 0.25)", "lerp(0, 10, 2)", "lerp(t, radius, phase)",
        "smoothstep(0, 1, 0.25)", "smoothstep(1, 1, 2)",
        "smoothstep(2, 4, t)",
        "sign(3)", "sign(-3)", "sign(0)",
        "clamp(sin(t), -0.5, 0.5)", "lerp(A[0], B[2], smoothstep(0, 1, t))",
        "floor(2.7)", "ceil(2.1)", "round(2.5)", "fmod(7, 3)", "rem(7, 3)",
        "rand(1)", "rand(1, 2)", "rand2(1)", "rand2(1, 2)",
        "rand(0, 0)", "rand2(7.5, 3)",
        /* nesting */
        "sin(cos(1))", "pow(sin(1), 2) + pow(cos(1), 2)",
        "max(min(1, 2), min(3, 4))", "sqrt(abs(-16))",
        "sin(1 + 2 * 3)", "min(1 + 1, 2 + 2)",
        /* predef vars */
        "t", "radius", "n", "t * 2", "sin(t)", "radius * cos(t) + n",
        "t + radius + n",
        /* locals (i, phase visible; local t shadows predef t) */
        "i", "phase", "i * 2 + phase", "sin(i + phase)", "cos(i / phase)",
        "t",   /* -> 100 under locals */
        "rand(i, phase)",
        /* scratch arrays */
        "A[0]", "A[3]", "B[2]", "C[15]", "A[0] + B[2]",
        "A[i]",              /* i == 3 */
        "A[0 + 0]", "A[1 - 1]", "B[i - 1]",
        "A[20]", "A[-1]",    /* out of range -> 0, evaluation continues */
        "A[A[1]]",           /* nested subscript (A[1] == 0 -> A[0]) */
        "A[0] * 2 + 1",
        /* whitespace forms */
        "  1+2 ", "sin( t )", "max( 1 , 2 )", "A[ 0 ]",
        /* mixed */
        "sin(t) * radius + A[0] / max(1, n + 4)",
        "(t > 1) && (radius < 2)",
        "floor(t) % 2 == 0",
    };

    for (size_t i = 0; i < sizeof(k_exprs) / sizeof(k_exprs[0]); i++) {
        /* Each expression twice: no locals, then with the local scope
         * (shadow case included). */
        check_parity(cache, k_exprs[i], NULL, 0);
        check_parity(cache, k_exprs[i], locals, 3);
    }

    /* Re-evaluating the same program after a predef value change tracks the
     * live table (values are read at eval time, not baked). */
    {
        ReplExprCompileEnv cenv = compile_env_with_locals(NULL, 0);
        ReplExprEvalEnv eenv = eval_env_with_locals(NULL, 0);
        int prog = repl_expr_cache_compile(cache, "t * 2 + radius", &cenv);
        ReplExprValue v;
        TEST_ASSERT_TRUE(&g_harness, "revalue compiles", prog >= 0);
        repl_eval_predef_vars_mut()[0].value = 4.0f;
        repl_eval_predef_vars_mut()[1].value = 0.5f;
        v = repl_expr_program_eval(cache, prog, &eenv);
        TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "revalue tracks live table",
                                  v.value, 8.5f);
    }

    repl_expr_cache_destroy(cache);
    repl_eval_undeclare_predef_var("radius");
    repl_eval_undeclare_predef_var("n");
}

/* ---- Dependency masks ---------------------------------------------------- */

static void test_dependency_masks(void) {
    ReplExprCache *cache = repl_expr_cache_create();
    ExprVar locals[2];
    ReplExprDepMask local_deps[2];

    repl_eval_init_predef_vars();
    repl_eval_declare_predef_var("a", NULL, 0);   /* slot 1 */
    repl_eval_declare_predef_var("b", NULL, 0);   /* slot 2 */
    repl_eval_predef_vars_mut()[0].value = 1.0f;
    repl_eval_predef_vars_mut()[1].value = 2.0f;
    repl_eval_predef_vars_mut()[2].value = 3.0f;

    strcpy(locals[0].name, "i");
    locals[0].value = 5.0f;
    strcpy(locals[1].name, "p");
    locals[1].value = 6.0f;
    local_deps[0] = 0x0;          /* constant-bound iterator */
    local_deps[1] = (1u << 2);    /* call arg that depended on b */

    {
        ReplExprCompileEnv cenv = compile_env_with_locals(locals, 2);
        ReplExprEvalEnv eenv = eval_env_with_locals(locals, 2);
        eenv.local_deps = local_deps;

        /* Constant: no deps. */
        int prog = repl_expr_cache_compile(cache, "1 + PI", &cenv);
        ReplExprValue v = repl_expr_program_eval(cache, prog, &eenv);
        TEST_ASSERT_INT(&g_harness, "const deps", (int)v.deps, 0);

        /* Predef reads contribute their slot bit by default. */
        prog = repl_expr_cache_compile(cache, "t + a", &cenv);
        v = repl_expr_program_eval(cache, prog, &eenv);
        TEST_ASSERT_INT(&g_harness, "predef slot bits", (int)v.deps,
                        (int)((1u << 0) | (1u << 1)));

        /* Builtins union their argument masks; rand adds nothing extra. */
        prog = repl_expr_cache_compile(cache, "rand(a, b)", &cenv);
        v = repl_expr_program_eval(cache, prog, &eenv);
        TEST_ASSERT_INT(&g_harness, "rand unions arg deps", (int)v.deps,
                        (int)((1u << 1) | (1u << 2)));

        /* Locals contribute their binding's mask. */
        prog = repl_expr_cache_compile(cache, "i + 1", &cenv);
        v = repl_expr_program_eval(cache, prog, &eenv);
        TEST_ASSERT_INT(&g_harness, "const-bound local deps", (int)v.deps, 0);

        prog = repl_expr_cache_compile(cache, "p * 2", &cenv);
        v = repl_expr_program_eval(cache, prog, &eenv);
        TEST_ASSERT_INT(&g_harness, "local mask pass-through", (int)v.deps,
                        (int)(1u << 2));

        /* used_local reports local resolution, not predef reads. */
        eenv.used_local = 0;
        prog = repl_expr_cache_compile(cache, "a + b", &cenv);
        (void)repl_expr_program_eval(cache, prog, &eenv);
        TEST_ASSERT_INT(&g_harness, "predef read is not used_local",
                        eenv.used_local, 0);
        prog = repl_expr_cache_compile(cache, "i", &cenv);
        (void)repl_expr_program_eval(cache, prog, &eenv);
        TEST_ASSERT_INT(&g_harness, "local read sets used_local",
                        eenv.used_local, 1);

        /* Threaded predef masks: with predef_deps supplied, a read of slot
         * 1 contributes the current mask (the assignment-dataflow case:
         * after `a = t * 2`, a's mask is {t}). */
        {
            ReplExprDepMask predef_deps[MAX_PREDEF_VARS] = { 0 };
            predef_deps[0] = (1u << 0);
            predef_deps[1] = (1u << 0);   /* a currently depends on t */
            predef_deps[2] = (1u << 2);
            eenv.predef_deps = predef_deps;
            prog = repl_expr_cache_compile(cache, "a + 1", &cenv);
            v = repl_expr_program_eval(cache, prog, &eenv);
            TEST_ASSERT_INT(&g_harness, "threaded predef mask", (int)v.deps,
                            (int)(1u << 0));
            eenv.predef_deps = NULL;
        }

        /* Scratch read: index deps only, plus the program-level flag. */
        repl_eval_reset_scratch_arrays();
        prog = repl_expr_cache_compile(cache, "A[0]", &cenv);
        v = repl_expr_program_eval(cache, prog, &eenv);
        TEST_ASSERT_INT(&g_harness, "scratch const index deps", (int)v.deps, 0);
        TEST_ASSERT_INT(&g_harness, "scratch read flagged",
                        repl_expr_program_has_scratch_read(cache, prog), 1);
        prog = repl_expr_cache_compile(cache, "A[t]", &cenv);
        v = repl_expr_program_eval(cache, prog, &eenv);
        TEST_ASSERT_INT(&g_harness, "scratch var index deps", (int)v.deps,
                        (int)(1u << 0));
        prog = repl_expr_cache_compile(cache, "sin(t)", &cenv);
        TEST_ASSERT_INT(&g_harness, "no scratch read not flagged",
                        repl_expr_program_has_scratch_read(cache, prog), 0);
    }

    repl_expr_cache_destroy(cache);
    repl_eval_undeclare_predef_var("a");
    repl_eval_undeclare_predef_var("b");
}

/* ---- Compile rejection --------------------------------------------------- */

static void test_compile_rejection(void) {
    ReplExprCache *cache = repl_expr_cache_create();
    ExprVar locals[1];

    repl_eval_init_predef_vars();

    check_compile_fails(cache, "", NULL, 0);
    check_compile_fails(cache, "sin(1, 2)", NULL, 0);     /* arity */
    check_compile_fails(cache, "pow(1)", NULL, 0);        /* arity */
    check_compile_fails(cache, "rand(1, 2, 3)", NULL, 0); /* arity */
    check_compile_fails(cache, "foo(1)", NULL, 0);        /* unknown call */
    check_compile_fails(cache, "sin(1", NULL, 0);         /* missing ')' */
    check_compile_fails(cache, "A", NULL, 0);             /* scratch, no index */
    check_compile_fails(cache, "A[1", NULL, 0);           /* missing ']' */
    check_compile_fails(cache, "D[0]", NULL, 0);          /* unknown array */
    check_compile_fails(cache,
        "abcdefghijklmnopqrstuvwxyz_abcdefghijklmnop", NULL, 0); /* >=32 chars */

    /* A local binding shadowing a builtin name must not compile to the
     * builtin call - the text evaluator resolves the local first (yielding
     * the local's value and leaving "(1)" unconsumed). The compiler emits
     * the same variable read, so parity holds. */
    strcpy(locals[0].name, "sin");
    locals[0].value = 9.0f;
    check_parity(cache, "sin(1)", locals, 1);
    check_parity(cache, "sin + 1", locals, 1);

    repl_expr_cache_destroy(cache);
}

/* ---- List compilation ---------------------------------------------------- */

static void test_compile_lists(void) {
    ReplExprCache *cache = repl_expr_cache_create();
    ReplExprCompileEnv cenv;
    ReplExprEvalEnv eenv;
    int progs[8];
    int n;

    repl_eval_init_predef_vars();
    repl_eval_predef_vars_mut()[0].value = 2.0f;
    cenv = compile_env_with_locals(NULL, 0);
    eenv = eval_env_with_locals(NULL, 0);

    /* Strict: mirrors parse_expr_list_exact. */
    n = repl_expr_cache_compile_list(cache, "1, sin(t), 3 + 4", 1, 8, progs,
                                     &cenv);
    TEST_ASSERT_INT(&g_harness, "strict list count", n, 3);
    if (n == 3) {
        float expected[3];
        int count = 0;
        TEST_ASSERT_TRUE(&g_harness, "text strict list parses",
                         parse_expr_list_exact("1, sin(t), 3 + 4", expected, 3,
                                               NULL, 0, &count) && count == 3);
        for (int i = 0; i < 3; i++) {
            ReplExprValue v = repl_expr_program_eval(cache, progs[i], &eenv);
            TEST_ASSERT_TRUE(&g_harness, "strict list parity",
                             float_bits_equal(v.value, expected[i]));
        }
    }

    n = repl_expr_cache_compile_list(cache, "", 1, 8, progs, &cenv);
    TEST_ASSERT_INT(&g_harness, "strict empty list", n, 0);
    n = repl_expr_cache_compile_list(cache, "1, ", 1, 8, progs, &cenv);
    TEST_ASSERT_INT(&g_harness, "strict trailing comma fails", n, -1);
    n = repl_expr_cache_compile_list(cache, "1 2", 1, 8, progs, &cenv);
    TEST_ASSERT_INT(&g_harness, "strict missing comma fails", n, -1);
    n = repl_expr_cache_compile_list(cache, "1, 2, 3", 1, 2, progs, &cenv);
    TEST_ASSERT_INT(&g_harness, "strict overflow fails", n, -1);

    /* Lenient: mirrors repl_eval_parse_exprs. */
    n = repl_expr_cache_compile_list(cache, " ,, 1, 2)", 0, 8, progs, &cenv);
    TEST_ASSERT_INT(&g_harness, "lenient list count", n, 2);
    if (n == 2) {
        float expected[8];
        int count = repl_eval_parse_exprs(" ,, 1, 2)", expected, 8, NULL, 0);
        TEST_ASSERT_INT(&g_harness, "lenient text count", count, 2);
        for (int i = 0; i < 2; i++) {
            ReplExprValue v = repl_expr_program_eval(cache, progs[i], &eenv);
            TEST_ASSERT_TRUE(&g_harness, "lenient list parity",
                             float_bits_equal(v.value, expected[i]));
        }
    }
    n = repl_expr_cache_compile_list(cache, "1, 2, 3, 4", 0, 2, progs, &cenv);
    TEST_ASSERT_INT(&g_harness, "lenient stops at max", n, 2);

    /* A failing sub-expression rolls the arenas back. */
    {
        int progs_before = 0;
        n = repl_expr_cache_compile_list(cache, "1", 1, 8, &progs_before,
                                         &cenv);
        TEST_ASSERT_INT(&g_harness, "rollback probe", n, 1);
        n = repl_expr_cache_compile_list(cache, "1, 2, foo(3)", 1, 8, progs,
                                         &cenv);
        TEST_ASSERT_INT(&g_harness, "strict bad member fails", n, -1);
        n = repl_expr_cache_compile_list(cache, "5", 1, 8, progs, &cenv);
        TEST_ASSERT_INT(&g_harness, "arena usable after rollback", n, 1);
        TEST_ASSERT_INT(&g_harness, "handles stay dense after rollback",
                        progs[0], progs_before + 1);
    }

    repl_expr_cache_destroy(cache);
}

/* ---- Line-entry lifecycle ------------------------------------------------ */

static void test_line_entries(void) {
    ReplExprCache *cache = repl_expr_cache_create();
    ReplExprCompileEnv cenv;

    repl_eval_init_predef_vars();
    cenv = compile_env_with_locals(NULL, 0);

    TEST_ASSERT_INT(&g_harness, "untouched line is EMPTY",
                    repl_expr_cache_line_state(cache, 7),
                    REPL_EXPR_LINE_EMPTY);

    {
        int p0 = repl_expr_cache_compile(cache, "1 + t", &cenv);
        int p1 = repl_expr_cache_compile(cache, "2 * t", &cenv);
        TEST_ASSERT_TRUE(&g_harness, "line begin",
                         repl_expr_cache_line_begin(cache, 7));
        TEST_ASSERT_TRUE(&g_harness, "line add arg0",
                         repl_expr_cache_line_add(cache, 7,
                                                  REPL_EXPR_ROLE_CMD_ARG, 0, p0));
        TEST_ASSERT_TRUE(&g_harness, "line add arg1",
                         repl_expr_cache_line_add(cache, 7,
                                                  REPL_EXPR_ROLE_CMD_ARG, 1, p1));
        repl_expr_cache_line_finish(cache, 7, 1);

        TEST_ASSERT_INT(&g_harness, "finished line is READY",
                        repl_expr_cache_line_state(cache, 7),
                        REPL_EXPR_LINE_READY);
        TEST_ASSERT_INT(&g_harness, "find arg0",
                        repl_expr_cache_line_find(cache, 7,
                                                  REPL_EXPR_ROLE_CMD_ARG, 0), p0);
        TEST_ASSERT_INT(&g_harness, "find arg1",
                        repl_expr_cache_line_find(cache, 7,
                                                  REPL_EXPR_ROLE_CMD_ARG, 1), p1);
        TEST_ASSERT_INT(&g_harness, "find missing role",
                        repl_expr_cache_line_find(cache, 7,
                                                  REPL_EXPR_ROLE_CONDITION, 0),
                        -1);
        TEST_ASSERT_INT(&g_harness, "role count",
                        repl_expr_cache_line_role_count(cache, 7,
                                                        REPL_EXPR_ROLE_CMD_ARG),
                        2);
    }

    /* Failed build: refs dropped, state FAILED, no lookups. */
    {
        int p = repl_expr_cache_compile(cache, "t", &cenv);
        TEST_ASSERT_TRUE(&g_harness, "line 9 begin",
                         repl_expr_cache_line_begin(cache, 9));
        repl_expr_cache_line_add(cache, 9, REPL_EXPR_ROLE_ASSIGN_RHS, 0, p);
        repl_expr_cache_line_finish(cache, 9, 0);
        TEST_ASSERT_INT(&g_harness, "failed line is FAILED",
                        repl_expr_cache_line_state(cache, 9),
                        REPL_EXPR_LINE_FAILED);
        TEST_ASSERT_INT(&g_harness, "failed line has no programs",
                        repl_expr_cache_line_find(cache, 9,
                                                  REPL_EXPR_ROLE_ASSIGN_RHS, 0),
                        -1);
        /* Line 7's entries survive line 9's failure. */
        TEST_ASSERT_TRUE(&g_harness, "other line intact",
                         repl_expr_cache_line_find(cache, 7,
                                                   REPL_EXPR_ROLE_CMD_ARG, 0)
                         >= 0);
    }

    /* Invalidation drops everything but keeps the cache usable. */
    repl_expr_cache_invalidate(cache);
    TEST_ASSERT_INT(&g_harness, "line EMPTY after invalidate",
                    repl_expr_cache_line_state(cache, 7),
                    REPL_EXPR_LINE_EMPTY);
    {
        int p = repl_expr_cache_compile(cache, "3 + 4", &cenv);
        ReplExprEvalEnv eenv = eval_env_with_locals(NULL, 0);
        ReplExprValue v;
        TEST_ASSERT_TRUE(&g_harness, "compile after invalidate", p >= 0);
        v = repl_expr_program_eval(cache, p, &eenv);
        TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "eval after invalidate",
                                  v.value, 7.0f);
    }

    /* Stale handles evaluate to the conservative failure value. */
    {
        ReplExprEvalEnv eenv = eval_env_with_locals(NULL, 0);
        ReplExprValue v = repl_expr_program_eval(cache, 999, &eenv);
        TEST_ASSERT_TRUE(&g_harness, "stale handle is conservative",
                         v.value == 0.0f && v.deps == REPL_EXPR_DEP_ALL);
    }

    repl_expr_cache_destroy(cache);
}

int main(void) {
    test_parity_corpus();
    test_dependency_masks();
    test_compile_rejection();
    test_compile_lists();
    test_line_entries();
    return test_harness_report(&g_harness, "test_expr_program");
}
