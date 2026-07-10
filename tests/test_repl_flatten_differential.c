/*
 * test_repl_flatten_differential.c - Flatten fast-path differential.
 *
 * flatten_reparse_line() appends a `has_vars == 0` source command verbatim
 * instead of re-parsing its text (docs/plans/in-review/
 * flatten-performance-without-vm.md, phase 1A). The claim that makes it safe
 * is that a literal command's committed args/payload are already the parse of
 * its current text: nothing visible at that source position can change them.
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
 * MAX_COMMANDS-sized buffers stay off the stack. */
typedef struct {
    GLCmd             cmds[MAX_COMMANDS];
    FlatCmdLocalVars  locals[MAX_COMMANDS];
    ReplFlattenResult result;
    float             predef[MAX_PREDEF_VARS];
    float             scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
} FlattenRun;

static FlattenRun g_fast;
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
 * selects the old always-reparse path. The live flat program is untouched. */
static void flatten_into(FlattenRun *run, int force_reparse) {
    ReplFlattenOptions opts;

    memset(run->cmds, 0, sizeof(run->cmds));
    memset(run->locals, 0, sizeof(run->locals));
    memset(&run->result, 0, sizeof(run->result));

    opts = (ReplFlattenOptions){
        .source_cmds      = repl_state_document_cmds(),
        .source_cmd_count = repl_state_document_count(),
        .flat_cmds        = run->cmds,
        .flat_local_vars  = run->locals,
        .flat_capacity    = MAX_COMMANDS,
        .text             = source_document_view(),
        .func_aliases     = repl_func_alias_view(),
        .max_call_depth   = 0,
        .visit_budget     = 0,
        .force_reparse    = force_reparse,
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

/* Load `example_idx`, then flatten it both ways at `t` from the identical
 * post-load baseline and compare everything the two runs produce. */
static void compare_example_at_time(int example_idx, float t) {
    char label[192];
    const char *name = repl_example_name(example_idx);
    Baseline base;
    int mismatch_cmd = -1;
    int mismatch_locals = -1;

    repl_load_example_lines_for_test(repl_example_lines(example_idx));
    baseline_capture(&base);

    repl_set_time(t);
    flatten_into(&g_fast, /*force_reparse=*/0);

    baseline_restore(&base);
    repl_set_time(t);
    flatten_into(&g_reparse, /*force_reparse=*/1);

    baseline_restore(&base);

    snprintf(label, sizeof(label), "%s @ t=%g: result ok", name, (double)t);
    TEST_ASSERT_INT(&g_harness, label, g_fast.result.ok, g_reparse.result.ok);

    snprintf(label, sizeof(label), "%s @ t=%g: flat count", name, (double)t);
    TEST_ASSERT_INT(&g_harness, label, g_fast.result.flat_cmd_count,
                    g_reparse.result.flat_cmd_count);

    snprintf(label, sizeof(label), "%s @ t=%g: lighting", name, (double)t);
    TEST_ASSERT_INT(&g_harness, label, g_fast.result.user_lighting_enabled,
                    g_reparse.result.user_lighting_enabled);

    snprintf(label, sizeof(label), "%s @ t=%g: status", name, (double)t);
    TEST_ASSERT_STR(&g_harness, label, g_fast.result.status,
                    g_reparse.result.status);

    if (g_fast.result.flat_cmd_count != g_reparse.result.flat_cmd_count)
        return;   /* counts already reported; per-command diffs would spam */

    for (int i = 0; i < g_fast.result.flat_cmd_count; i++) {
        if (mismatch_cmd < 0 && !flat_cmd_equal(&g_fast.cmds[i], &g_reparse.cmds[i]))
            mismatch_cmd = i;
        if (mismatch_locals < 0 &&
            !flat_locals_equal(&g_fast.locals[i], &g_reparse.locals[i]))
            mismatch_locals = i;
    }

    snprintf(label, sizeof(label), "%s @ t=%g: flat cmds identical (first diff)",
             name, (double)t);
    TEST_ASSERT_INT(&g_harness, label, mismatch_cmd, -1);

    snprintf(label, sizeof(label), "%s @ t=%g: local snapshots identical (first diff)",
             name, (double)t);
    TEST_ASSERT_INT(&g_harness, label, mismatch_locals, -1);

    snprintf(label, sizeof(label), "%s @ t=%g: post-flatten predef state",
             name, (double)t);
    TEST_ASSERT_INT(&g_harness, label,
                    memcmp(g_fast.predef, g_reparse.predef,
                           sizeof(g_fast.predef)) == 0, 1);

    snprintf(label, sizeof(label), "%s @ t=%g: post-flatten scratch state",
             name, (double)t);
    TEST_ASSERT_INT(&g_harness, label,
                    memcmp(g_fast.scratch, g_reparse.scratch,
                           sizeof(g_fast.scratch)) == 0, 1);
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

    flatten_into(&g_fast, /*force_reparse=*/0);

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
    flatten_into(&g_fast, /*force_reparse=*/0);

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
static void test_auto_normal_differential(void) {
    load_auto_normal_doc();
    flatten_into(&g_fast, /*force_reparse=*/0);
    load_auto_normal_doc();
    flatten_into(&g_reparse, /*force_reparse=*/1);

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
        flatten_into(pass == 0 ? &g_fast : &g_reparse, /*force_reparse=*/pass);
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

int main(void) {
    repl_eval_init_predef_vars();
    glr_ctrl_reset_all();   /* binds the live command store before the first load */

    test_corpus_differential();
    test_literal_cmd_keeps_local_snapshot();
    test_literal_cmd_keeps_is_auto();
    test_auto_normal_differential();
    test_var_assign_differential();

    printf("\n");
    return test_harness_report(&g_harness, "test_repl_flatten_differential");
}
