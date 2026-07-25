/*
 * test_repl_flatten_differential.c - Flatten fast-path differential.
 *
 * Phase 1's flatten fast paths reuse committed command structure instead of
 * rediscovering it from source text on every expanded instance:
 *
 *   - has_vars == 0 appends the committed command verbatim (Phase 1A),
 *   - standard numeric commands evaluate only their known argument list,
 *   - scalar assignments evaluate their already-canonical REPL RHS directly.
 *
 * This suite tests that claim over the whole built-in corpus. Each example is
 * flattened twice from an identical baseline — once normally, once with
 * ReplFlattenOptions.force_reparse (the old always-reparse behavior) — and the
 * two runs must agree on:
 *
 *   - the flatten result (ok / flat count / lighting / status text),
 *   - every flat GLCmd field, including provenance and local-var snapshots,
 *   - the post-flatten predef-variable and scratch-array state.
 *
 * Each example is sampled at several `t` values so animated scenes exercise
 * the re-evaluated (has_vars) commands alongside the literal ones, and so
 * dynamic-topology scenes (Whale) are compared at more than one flat count.
 */

#include <stdio.h>
#include <string.h>

#include "editor/input.h"    /* editor_feed_line */
#include "editor/state.h"
#include "app/glr_ctrl.h"
#include "repl/command.h"
#include "repl/color_limits.h"
#include "repl/eval.h"
#include "repl/example_loader.h"
#include "repl/examples.h"
#include "repl/flatten.h"
#include "repl/pipeline.h"
#include "repl/state_owners.h"
#include "repl/time.h"
#include "source_document.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;

/* One flatten's full observable output: the temp flat program plus the
 * runtime state the expansion writes as a side effect. Static so the two
 * MAX_FLAT_COMMANDS-sized buffers stay off the stack. */
typedef struct {
    GLCmd             cmds[MAX_FLAT_COMMANDS];
    FlatCmdLocalVars  locals[MAX_FLAT_COMMANDS];
    ReplFlattenResult result;
    float             predef[MAX_PREDEF_VARS];
    float             scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
} FlattenRun;

static FlattenRun g_fast;
static FlattenRun g_warm;
static FlattenRun g_reparse;

typedef struct {
    float predef[MAX_PREDEF_VARS];
    float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
} Baseline;

static void baseline_capture(Baseline *b) {
    repl_copy_predef_values(b->predef, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(b->scratch);
}

static void baseline_restore(const Baseline *b) {
    repl_restore_predef_values(b->predef, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(b->scratch);
}

/* Flatten the live document into `run`'s private buffers. force_reparse
 * selects the old always-reparse path; use_cache attaches the live
 * compiled-expression cache (invalidated by every source mutation, so
 * example loads reset it exactly as the app's loads do). The live flat
 * program is untouched. */
static void flatten_into(FlattenRun *run, int force_reparse, int use_cache) {
    ReplFlattenOptions opts;

    memset(run->cmds, 0, sizeof(run->cmds));
    memset(run->locals, 0, sizeof(run->locals));
    memset(&run->result, 0, sizeof(run->result));

    opts = (ReplFlattenOptions){
        .source_cmds      = repl_state_document_cmds(),
        .source_cmd_count = repl_state_document_count(),
        .flat_cmds        = run->cmds,
        .flat_local_vars  = run->locals,
        .flat_capacity    = MAX_FLAT_COMMANDS,
        .text             = source_document_view(),
        .func_aliases     = repl_func_alias_view(),
        .max_call_depth   = 0,
        .visit_budget     = 0,
        .force_reparse    = force_reparse,
        .expr_cache       = use_cache ? repl_expr_cache_live() : NULL,
    };
    repl_flatten_program(&opts, &run->result);

    repl_copy_predef_values(run->predef, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(run->scratch);
}

/* Compare one flat command across the two runs. Returns 1 on match.
 * The GLCmd payload union is compared only for the types that own it
 * (the contract in command.h is "zeroed; do not read" elsewhere). */
static int flat_cmd_equal(const GLCmd *a, const GLCmd *b) {
    if (a->type != b->type || a->valid != b->valid ||
        a->has_vars != b->has_vars || a->is_auto != b->is_auto ||
        a->num_args != b->num_args || a->var_idx != b->var_idx)
        return 0;
    for (int i = 0; i < a->num_args; i++) {
        /* Bit-identical, not epsilon: both paths must evaluate the same
         * expression with the same float ops in the same order. */
        if (memcmp(&a->args[i], &b->args[i], sizeof(a->args[i])) != 0)
            return 0;
    }
    if (a->src_cmd_idx != b->src_cmd_idx ||
        a->call_src_cmd_idx != b->call_src_cmd_idx ||
        a->root_call_src_cmd_idx != b->root_call_src_cmd_idx ||
        a->func_scope_mask != b->func_scope_mask ||
        a->call_depth != b->call_depth)
        return 0;
    if (a->type == CMD_LABEL &&
        strcmp(a->payload.label.fmt, b->payload.label.fmt) != 0)
        return 0;
    if (a->type == CMD_VAR_DECLARE &&
        memcmp(&a->payload.decl, &b->payload.decl, sizeof(a->payload.decl)) != 0)
        return 0;
    if (a->type == CMD_MULT_MATRIXF &&
        memcmp(&a->payload.matrix, &b->payload.matrix,
               sizeof(a->payload.matrix)) != 0)
        return 0;
    return 1;
}

static int flat_locals_equal(const FlatCmdLocalVars *a,
                             const FlatCmdLocalVars *b) {
    if (a->num_vars != b->num_vars)
        return 0;
    for (int v = 0; v < a->num_vars; v++) {
        if (strcmp(a->vars[v].name, b->vars[v].name) != 0)
            return 0;
        if (memcmp(&a->vars[v].value, &b->vars[v].value,
                   sizeof(a->vars[v].value)) != 0)
            return 0;
    }
    return 1;
}

/* Compare everything two flatten runs produce. `tag` names the pair (which
 * fast path ran against the forced-reparse reference). */
static void compare_runs(const char *name, float t, const char *tag,
                         const FlattenRun *a, const FlattenRun *b) {
    char label[192];
    int mismatch_cmd = -1;
    int mismatch_locals = -1;

    snprintf(label, sizeof(label), "%s @ t=%g [%s]: result ok",
             name, (double)t, tag);
    TEST_ASSERT_INT(&g_harness, label, a->result.ok, b->result.ok);

    snprintf(label, sizeof(label), "%s @ t=%g [%s]: flat count",
             name, (double)t, tag);
    TEST_ASSERT_INT(&g_harness, label, a->result.flat_cmd_count,
                    b->result.flat_cmd_count);

    snprintf(label, sizeof(label), "%s @ t=%g [%s]: lighting",
             name, (double)t, tag);
    TEST_ASSERT_INT(&g_harness, label, a->result.user_lighting_enabled,
                    b->result.user_lighting_enabled);

    snprintf(label, sizeof(label), "%s @ t=%g [%s]: status",
             name, (double)t, tag);
    TEST_ASSERT_STR(&g_harness, label, a->result.status, b->result.status);

    if (a->result.flat_cmd_count != b->result.flat_cmd_count)
        return;   /* counts already reported; per-command diffs would spam */

    for (int i = 0; i < a->result.flat_cmd_count; i++) {
        if (mismatch_cmd < 0 && !flat_cmd_equal(&a->cmds[i], &b->cmds[i]))
            mismatch_cmd = i;
        if (mismatch_locals < 0 &&
            !flat_locals_equal(&a->locals[i], &b->locals[i]))
            mismatch_locals = i;
    }

    snprintf(label, sizeof(label),
             "%s @ t=%g [%s]: flat cmds identical (first diff)",
             name, (double)t, tag);
    TEST_ASSERT_INT(&g_harness, label, mismatch_cmd, -1);

    snprintf(label, sizeof(label),
             "%s @ t=%g [%s]: local snapshots identical (first diff)",
             name, (double)t, tag);
    TEST_ASSERT_INT(&g_harness, label, mismatch_locals, -1);

    snprintf(label, sizeof(label), "%s @ t=%g [%s]: post-flatten predef state",
             name, (double)t, tag);
    TEST_ASSERT_INT(&g_harness, label,
                    memcmp(a->predef, b->predef, sizeof(a->predef)) == 0, 1);

    snprintf(label, sizeof(label), "%s @ t=%g [%s]: post-flatten scratch state",
             name, (double)t, tag);
    TEST_ASSERT_INT(&g_harness, label,
                    memcmp(a->scratch, b->scratch, sizeof(a->scratch)) == 0, 1);
}

/* Flatten the live document three ways at `t` from an identical baseline
 * and compare against the forced-reparse reference:
 *   - cold: fast paths + an empty expression cache (this run builds it);
 *   - warm: the same cache, now READY — the compiled path proper.
 * The cache carries over between the cold and warm runs only; the example
 * load's source-dirty invalidation cleared it beforehand. */
static void compare_document_at_time(const char *name, float t) {
    Baseline base;

    baseline_capture(&base);

    repl_set_time(t);
    flatten_into(&g_fast, /*force_reparse=*/0, /*use_cache=*/1);   /* cold */

    baseline_restore(&base);
    repl_set_time(t);
    flatten_into(&g_warm, /*force_reparse=*/0, /*use_cache=*/1);   /* warm */

    baseline_restore(&base);
    repl_set_time(t);
    flatten_into(&g_reparse, /*force_reparse=*/1, /*use_cache=*/0);

    baseline_restore(&base);

    compare_runs(name, t, "cold-cache", &g_fast, &g_reparse);
    compare_runs(name, t, "warm-cache", &g_warm, &g_reparse);
}

/* Load `example_idx`, then compare the fast/cached paths against the forced
 * reparse at `t` from the identical post-load baseline. */
static void compare_example_at_time(int example_idx, float t) {
    repl_load_example_lines_for_test(repl_example_lines(example_idx));
    compare_document_at_time(repl_example_name(example_idx), t);
}

/* t=0 is the loaded state; the other samples move animated scenes off their
 * origin and, for Whale, across particle spawn/expiry boundaries so the two
 * paths are compared at more than one flat-command count. */
static void test_corpus_differential(void) {
    static const float k_times[] = { 0.0f, 0.37f, 1.9f, 4.5f };
    int n = repl_example_count();

    for (int e = 0; e < n; e++)
        for (size_t s = 0; s < sizeof(k_times) / sizeof(k_times[0]); s++)
            compare_example_at_time(e, k_times[s]);
}

/* The fast path must still emit the enclosing loop/function bindings on a
 * literal command — replay's value-tracing annotations read them. */
static void test_literal_cmd_keeps_local_snapshot(void) {
    glr_ctrl_reset_all();
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("glVertex3f(1, 2, 3);");
    editor_feed_line("}");

    flatten_into(&g_fast, /*force_reparse=*/0, /*use_cache=*/1);

    TEST_ASSERT_INT(&g_harness, "literal-in-loop flat count",
                    g_fast.result.flat_cmd_count, 3);
    TEST_ASSERT_INT(&g_harness, "literal-in-loop has_vars stays 0",
                    g_fast.cmds[1].has_vars, 0);
    TEST_ASSERT_INT(&g_harness, "literal-in-loop keeps a local snapshot",
                    g_fast.locals[1].num_vars, 1);
    TEST_ASSERT_STR(&g_harness, "literal-in-loop snapshot names the iterator",
                    g_fast.locals[1].vars[0].name, "i");
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "literal-in-loop snapshot value",
                              g_fast.locals[1].vars[0].value, 1.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "literal-in-loop arg 0",
                              g_fast.cmds[1].args[0], 1.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "literal-in-loop arg 2",
                              g_fast.cmds[1].args[2], 3.0f);
}

/* Feed a triangle and synthesize its normal, leaving an is_auto CMD_NORMAL3F
 * in the source document. Returns the count of auto-normals in `run`. */
static int count_auto_normals(const FlattenRun *run) {
    int n = 0;
    for (int i = 0; i < run->result.flat_cmd_count; i++)
        if (run->cmds[i].type == CMD_NORMAL3F && run->cmds[i].is_auto)
            n++;
    return n;
}

static void load_auto_normal_doc(void) {
    glr_ctrl_reset_all();
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(1, NULL);
}

/* The document synthesizes one auto-normal per triangle vertex. */
#define EXPECTED_AUTO_NORMALS 3

/* An auto-generated normal is a literal command; the fast path must carry its
 * is_auto flag into the flat program. */
static void test_literal_cmd_keeps_is_auto(void) {
    load_auto_normal_doc();
    flatten_into(&g_fast, /*force_reparse=*/0, /*use_cache=*/1);

    TEST_ASSERT_INT(&g_harness, "auto normals reach the flat program as is_auto",
                    count_auto_normals(&g_fast), EXPECTED_AUTO_NORMALS);
}

/* The two paths must agree on the synthesized normal too. The forced path runs
 * repl_parser_parse_command_ctx, which memsets the whole ReplParsedLine and
 * never writes is_auto (except to clear it for CMD_EMPTY / CMD_COMMENT) or
 * var_idx at all — so without an explicit post-parse restore in
 * flatten_reparse_line, the reparsed normal loses is_auto and the seam is not
 * semantically equivalent. test_corpus_differential cannot catch this: no
 * built-in example carries an auto-normal, so the field never differs there. */
/* Flat index of the run's single glMultMatrixf, or -1. */
static int mult_matrix_flat_idx(const FlattenRun *run) {
    for (int i = 0; i < run->result.flat_cmd_count; i++)
        if (run->cmds[i].type == CMD_MULT_MATRIXF)
            return i;
    return -1;
}

static float mult_matrix_cell(const FlattenRun *run, int cell) {
    int idx = mult_matrix_flat_idx(run);
    return idx >= 0 ? run->cmds[idx].payload.matrix.m[cell] : 0.0f;
}

/* glMultMatrixf's 16 cells are read out of the scratch table at flatten
 * time, in stream order, and baked onto the flat command. Two things have
 * to hold: the bake sees the writes from the A[k] = ... lines above it (not
 * a stale or zeroed array), and it happens on every path into the flat
 * array — the command carries has_vars=0, so the fast path copies the
 * source command wholesale and would otherwise ship the parser's identity
 * placeholder while the forced-reparse path shipped the real matrix. */
static void test_mult_matrixf_snapshot(void) {
    glr_ctrl_reset_all();
    editor_feed_line("float s;");
    editor_feed_line("s = 3;");
    editor_feed_line("A[0] = s;");
    editor_feed_line("A[5] = 2;");
    editor_feed_line("A[10] = 1;");
    editor_feed_line("A[12] = 7;");
    editor_feed_line("A[15] = 1;");
    editor_feed_line("glMultMatrixf(A);");

    flatten_into(&g_fast, /*force_reparse=*/0, /*use_cache=*/1);

    TEST_ASSERT_TRUE(&g_harness, "glMultMatrixf reaches the flat program",
                     mult_matrix_flat_idx(&g_fast) >= 0);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "snapshot sees a var-backed cell",
                              mult_matrix_cell(&g_fast, 0), 3.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "snapshot sees a literal cell",
                              mult_matrix_cell(&g_fast, 5), 2.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "snapshot sees the translation cell",
                              mult_matrix_cell(&g_fast, 12), 7.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "unwritten cells stay zero",
                              mult_matrix_cell(&g_fast, 1), 0.0f);

    /* The baked matrix must not outlive the values behind it: a cell fed
     * by `t` re-reads on the next flatten, which is what makes an animated
     * matrix animate at all. */
    glr_ctrl_reset_all();
    editor_feed_line("A[0] = t * 2;");
    editor_feed_line("A[5] = 1;");
    editor_feed_line("A[10] = 1;");
    editor_feed_line("A[15] = 1;");
    editor_feed_line("glMultMatrixf(A);");

    repl_state_time_set(1.5f);
    flatten_into(&g_fast, /*force_reparse=*/0, /*use_cache=*/1);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "animated cell baked at t=1.5",
                              mult_matrix_cell(&g_fast, 0), 3.0f);

    repl_state_time_set(4.0f);
    flatten_into(&g_fast, /*force_reparse=*/0, /*use_cache=*/1);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "animated cell re-bakes at t=4",
                              mult_matrix_cell(&g_fast, 0), 8.0f);

    /* Both flatten paths must ship the same matrix — see the has_vars=0
     * fast-path note above. */
    flatten_into(&g_reparse, /*force_reparse=*/1, /*use_cache=*/0);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "forced reparse bakes the same cell",
                              mult_matrix_cell(&g_reparse, 0), 8.0f);
}

static void test_auto_normal_differential(void) {
    load_auto_normal_doc();
    flatten_into(&g_fast, /*force_reparse=*/0, /*use_cache=*/1);
    load_auto_normal_doc();
    flatten_into(&g_reparse, /*force_reparse=*/1, /*use_cache=*/0);

    TEST_ASSERT_INT(&g_harness, "auto-normal doc: same flat count",
                    g_fast.result.flat_cmd_count,
                    g_reparse.result.flat_cmd_count);
    TEST_ASSERT_INT(&g_harness, "auto-normal doc: fast path has auto normals",
                    count_auto_normals(&g_fast), EXPECTED_AUTO_NORMALS);
    TEST_ASSERT_INT(&g_harness, "auto normals survive the forced reparse",
                    count_auto_normals(&g_reparse), EXPECTED_AUTO_NORMALS);

    for (int i = 0; i < g_fast.result.flat_cmd_count; i++)
        TEST_ASSERT_INT(&g_harness,
                        "auto-normal doc: fast and forced flat cmds agree",
                        flat_cmd_equal(&g_fast.cmds[i], &g_reparse.cmds[i]), 1);
}

/* var_idx is commit-time state (compile.c) that the parser never writes, so a
 * reparse would zero it — retargeting the assignment at predef slot 0 (`t`).
 * flatten_reparse_line does NOT restore it, and does not need to: flatten_range
 * routes CMD_VAR_ASSIGN to flatten_var_assign before the reparse path can see
 * it. That routing is the invariant this test pins. If a future change lets a
 * var-assign reach flatten_reparse_line, var_idx starts surviving only by
 * accident and this test fails under force_reparse. */
static void test_var_assign_differential(void) {
    int checked = 0;

    for (int pass = 0; pass < 2; pass++) {
        glr_ctrl_reset_all();
        editor_feed_line("float k;");
        editor_feed_line("k = 2;");
        editor_feed_line("glVertex3f(1, 0, 0);");
        flatten_into(pass == 0 ? &g_fast : &g_reparse, /*force_reparse=*/pass,
                     /*use_cache=*/pass == 0);
    }

    TEST_ASSERT_INT(&g_harness, "var-assign doc: same flat count",
                    g_fast.result.flat_cmd_count,
                    g_reparse.result.flat_cmd_count);

    for (int i = 0; i < g_fast.result.flat_cmd_count; i++) {
        if (g_fast.cmds[i].type == CMD_VAR_ASSIGN) {
            checked = 1;
            TEST_ASSERT_INT(&g_harness,
                            "var-assign keeps a nonzero var_idx (not slot 0/t)",
                            g_fast.cmds[i].var_idx > 0, 1);
            TEST_ASSERT_INT(&g_harness,
                            "var-assign var_idx identical under force_reparse",
                            g_reparse.cmds[i].var_idx, g_fast.cmds[i].var_idx);
        }
        TEST_ASSERT_INT(&g_harness,
                        "var-assign doc: fast and forced flat cmds agree",
                        flat_cmd_equal(&g_fast.cmds[i], &g_reparse.cmds[i]), 1);
    }
    TEST_ASSERT_INT(&g_harness, "var-assign doc reached a CMD_VAR_ASSIGN",
                    checked, 1);
}

/* Exercise both Phase 1C paths together. k's assignment is evaluated without
 * the redundant C->REPL translation, then the known glClearColor shape is
 * evaluated without command-name dispatch. The dynamic RGB clamp is a parser
 * post-processing rule that the direct path must preserve. */
static void test_direct_eval_differential(void) {
    Baseline base;

    glr_ctrl_reset_all();
    editor_feed_line("float k;");
    editor_feed_line("k = t * 2;");
    editor_feed_line("glClearColor(k, k + 0.1, -k, 1);");
    editor_feed_line("glVertex3f(k, t, 0); // direct path");
    baseline_capture(&base);

    repl_set_time(0.75f);
    flatten_into(&g_fast, /*force_reparse=*/0, /*use_cache=*/0);
    baseline_restore(&base);
    repl_set_time(0.75f);
    flatten_into(&g_reparse, /*force_reparse=*/1, /*use_cache=*/0);
    baseline_restore(&base);

    TEST_ASSERT_INT(&g_harness, "direct eval: same flat count",
                    g_fast.result.flat_cmd_count,
                    g_reparse.result.flat_cmd_count);
    for (int i = 0; i < g_fast.result.flat_cmd_count; i++)
        TEST_ASSERT_INT(&g_harness, "direct eval: command matches parser",
                        flat_cmd_equal(&g_fast.cmds[i], &g_reparse.cmds[i]), 1);
    TEST_ASSERT_INT(&g_harness, "direct eval: post-flatten predefs match",
                    memcmp(g_fast.predef, g_reparse.predef,
                           sizeof(g_fast.predef)) == 0, 1);

    /* k=1.5, so positive RGB channels must clamp while -k remains intact. */
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "direct eval: red clamp",
                              g_fast.cmds[1].args[0], REPL_CLEAR_COLOR_MAX_V);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "direct eval: green clamp",
                              g_fast.cmds[1].args[1], REPL_CLEAR_COLOR_MAX_V);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "direct eval: negative blue kept",
                              g_fast.cmds[1].args[2], -1.5f);
}

/* One synthetic scene touching every command family the expression cache
 * captures, with variables in each expression slot so every family takes
 * its compiled path warm. The built-in corpus covers the common families;
 * this pins the rare ones (glMaterialfv value lists, glMaterialf,
 * glPointParameterfv, glClipPlane, an ENUM_OR_EXPR slot, label substitutions,
 * gluColor's defaulted alpha, the glClearColor clamp, scratch assigns,
 * if/else-if chains, and function-call args) regardless of catalog churn. */
static void test_capture_construct_coverage(void) {
    static const float k_times[] = { 0.0f, 1.3f };

    glr_ctrl_reset_all();
    editor_feed_line("float n;");
    editor_feed_line("n = t / 4 + 2;");
    editor_feed_line("glClearColor(n, 0.02, 0.05, 1);");
    editor_feed_line("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, n - 1);");
    editor_feed_line("glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){n / 4, 0.5, sin(n), 1});");
    editor_feed_line("glMaterialf(GL_FRONT, GL_SHININESS, n * 10);");
    editor_feed_line("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1, n / 2, 0);");
    editor_feed_line("glClipPlane(GL_CLIP_PLANE0, (GLdouble[]){1, 0, 0, n / 3});");
    editor_feed_line("A[0] = n * 2;");
    editor_feed_line("A[1] = A[0] + rand(n);");
    editor_feed_line("func0(a) {");
    editor_feed_line("glTranslatef(a, 0, 0);");
    editor_feed_line("glutSolidCube(a / 2 + 0.1);");
    editor_feed_line("}");
    editor_feed_line("func0(n / 3);");
    editor_feed_line("for(i, 0, n + 1) {");
    editor_feed_line("if(i > n / 2) {");
    editor_feed_line("glColor3f(i / 4, 0.5, 0.5);");
    editor_feed_line("} else if(i > 1) {");
    editor_feed_line("glColor3f(0.5, i / 4, 0.5);");
    editor_feed_line("} else {");
    editor_feed_line("glColor3f(0.5, 0.5, i + 0.25);");
    editor_feed_line("}");
    editor_feed_line("glVertex3f(i, n, sin(i));");
    editor_feed_line("}");
    editor_feed_line("glRasterPos3f(n, 0, A[1]);");
    editor_feed_line("label(\"v=%f\", n + 1);");
    editor_feed_line("gluBegin(GLU_POLYGON);");
    editor_feed_line("gluColor(n / 4, 0.5, 0.25);");
    editor_feed_line("gluVertex(0, 0, n);");
    editor_feed_line("gluVertex(1, 0, n);");
    editor_feed_line("gluVertex(0, 1, n);");
    editor_feed_line("gluEnd();");

    /* The scene above committed line by line, so the live cache may hold
     * partial builds from intermediate states; a source-dirty already
     * invalidated it on every commit — the last one leaves it EMPTY. */
    for (size_t s = 0; s < sizeof(k_times) / sizeof(k_times[0]); s++)
        compare_document_at_time("construct coverage", k_times[s]);

    TEST_ASSERT_INT(&g_harness, "construct scene flattens ok",
                    g_warm.result.ok, 1);
    TEST_ASSERT_INT(&g_harness, "construct scene emits commands",
                    g_warm.result.flat_cmd_count > 10, 1);
}

int main(void) {
    repl_eval_init_predef_vars();
    glr_ctrl_reset_all();   /* binds the live command store before the first load */

    test_corpus_differential();
    test_capture_construct_coverage();
    test_literal_cmd_keeps_local_snapshot();
    test_literal_cmd_keeps_is_auto();
    test_mult_matrixf_snapshot();
    test_auto_normal_differential();
    test_var_assign_differential();
    test_direct_eval_differential();

    printf("\n");
    return test_harness_report(&g_harness, "test_repl_flatten_differential");
}
