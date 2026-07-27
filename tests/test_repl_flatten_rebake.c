/*
 * tests/test_repl_flatten_rebake.c -- in-place rebake differential
 * (flatten plan phase 3b).
 *
 * The rebake path (repl_flatten_rebake_program) re-evaluates the VALUES of an
 * existing flat stream without re-expanding it, for a value-only predef change
 * whose bit does not intersect the structural mask. This suite proves rebake
 * reproduces a full flatten's output for that class of change:
 *
 *   - Load a scene, full-flatten at t0 into the LIVE flat program.
 *   - Snapshot it. Change t (routing through the dep masks), then run the
 *     in-place rebake at t1.
 *   - Full-flatten the SAME scene at t1 into a private buffer (the reference).
 *   - The rebaked live program must match the reference command-for-command:
 *     args, provenance, flags, local snapshots, count, and lighting — plus the
 *     post-pass predef/scratch state.
 *
 * It also checks the routing/guard behaviour: a value-only change takes the
 * rebake (not a full flatten), a structural change refuses it, and a failed
 * rebake leaves the baseline restored.
 */
#include <stdio.h>
#include <string.h>

#include "app/glr_ctrl.h"
#include "editor/input.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/example_loader.h"
#include "repl/examples.h"
#include "repl/flatten.h"
#include "repl/pipeline.h"
#include "repl/state.h"
#include "repl/state_notify.h"
#include "repl/state_owners.h"
#include "source_document.h"
#include "support/cpuprof.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, exp) TEST_ASSERT_INT(&g_harness, label, got, exp)

/* A reference full flatten into private buffers (never touches the live
 * flat program). */
typedef struct {
    GLCmd             cmds[MAX_FLAT_COMMANDS];
    FlatCmdLocalVars  locals[MAX_FLAT_COMMANDS];
    ReplFlattenResult result;
} RefRun;

static RefRun g_ref;

typedef struct {
    float predef[MAX_PREDEF_VARS];
    float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
} ValueBaseline;

static void baseline_capture(ValueBaseline *baseline) {
    memset(baseline, 0, sizeof(*baseline));
    repl_copy_predef_values(baseline->predef, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(baseline->scratch);
}

static void baseline_restore(const ValueBaseline *baseline) {
    repl_restore_predef_values(baseline->predef, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(baseline->scratch);
}

static void full_flatten_into(RefRun *run) {
    ReplFlattenOptions opts = {
        .source_cmds      = repl_state_document_cmds(),
        .source_cmd_count = repl_state_document_count(),
        .flat_cmds        = run->cmds,
        .flat_local_vars  = run->locals,
        .flat_capacity    = MAX_FLAT_COMMANDS,
        .text             = source_document_view(),
        .func_aliases     = repl_func_alias_view(),
        .expr_cache       = repl_expr_cache_live(),
    };
    memset(run->cmds, 0, sizeof(run->cmds));
    memset(run->locals, 0, sizeof(run->locals));
    repl_flatten_program(&opts, &run->result);
}

static void load_scene(const char *const *lines) {
    glr_ctrl_reset_all();
    for (int i = 0; lines[i]; i++)
        editor_feed_line(lines[i]);
    repl_flatten_commands(0);
    repl_state_flat_program_clear_dirty();
}

/* Bit-exact command comparison (same fields the differential test checks). */
static int flat_cmd_equal(const GLCmd *a, const GLCmd *b) {
    if (a->type != b->type || a->valid != b->valid ||
        a->has_vars != b->has_vars || a->is_auto != b->is_auto ||
        a->num_args != b->num_args || a->var_idx != b->var_idx)
        return 0;
    for (int i = 0; i < a->num_args; i++)
        if (memcmp(&a->args[i], &b->args[i], sizeof(a->args[i])) != 0)
            return 0;
    if (a->src_cmd_idx != b->src_cmd_idx ||
        a->call_src_cmd_idx != b->call_src_cmd_idx ||
        a->root_call_src_cmd_idx != b->root_call_src_cmd_idx ||
        a->func_scope_mask != b->func_scope_mask ||
        a->call_depth != b->call_depth)
        return 0;
    if (a->type == CMD_LABEL &&
        strcmp(a->payload.label.fmt, b->payload.label.fmt) != 0)
        return 0;
    return 1;
}

static int flat_locals_equal(const FlatCmdLocalVars *a,
                             const FlatCmdLocalVars *b) {
    if (a->num_vars != b->num_vars)
        return 0;
    for (int v = 0; v < a->num_vars; v++) {
        if (strcmp(a->vars[v].name, b->vars[v].name) != 0 ||
            memcmp(&a->vars[v].value, &b->vars[v].value,
                   sizeof(float)) != 0)
            return 0;
    }
    return 1;
}

static void assert_live_matches_reference(
    const char *name, const ValueBaseline *rebaked_state,
    const ValueBaseline *full_state) {
    char label[192];
    int mismatch = -1;
    const GLCmd *live = repl_state_flat_program_cmds();
    const FlatCmdLocalVars *live_locals =
        repl_state_flat_program_local_vars();
    int n = repl_state_flat_program_count();

    snprintf(label, sizeof(label), "%s: rebaked flat count == full", name);
    TEST_ASSERT_INT(&g_harness, label, n, g_ref.result.flat_cmd_count);
    if (n > g_ref.result.flat_cmd_count)
        n = g_ref.result.flat_cmd_count;
    for (int k = 0; k < n && mismatch < 0; k++) {
        if (!flat_cmd_equal(&live[k], &g_ref.cmds[k]) ||
            !flat_locals_equal(&live_locals[k], &g_ref.locals[k]))
            mismatch = k;
    }
    if (mismatch >= 0) {
        fprintf(stderr,
                "  %s mismatch[%d]: live type=%d args=%g,%g,%g; "
                "full type=%d args=%g,%g,%g\n",
                name, mismatch, (int)live[mismatch].type,
                (double)live[mismatch].args[0],
                (double)live[mismatch].args[1],
                (double)live[mismatch].args[2],
                (int)g_ref.cmds[mismatch].type,
                (double)g_ref.cmds[mismatch].args[0],
                (double)g_ref.cmds[mismatch].args[1],
                (double)g_ref.cmds[mismatch].args[2]);
    }
    snprintf(label, sizeof(label),
             "%s: rebaked stream matches full flatten (first mismatch idx)",
             name);
    TEST_ASSERT_INT(&g_harness, label, mismatch, -1);

    snprintf(label, sizeof(label), "%s: rebaked lighting == full", name);
    TEST_ASSERT_INT(&g_harness, label,
                    repl_state_flat_program_user_lighting_enabled(),
                    g_ref.result.user_lighting_enabled);
    snprintf(label, sizeof(label), "%s: post-rebake predefs == full", name);
    TEST_ASSERT_INT(&g_harness, label,
                    memcmp(rebaked_state->predef, full_state->predef,
                           sizeof(rebaked_state->predef)) == 0, 1);
    snprintf(label, sizeof(label), "%s: post-rebake scratch == full", name);
    TEST_ASSERT_INT(&g_harness, label,
                    memcmp(rebaked_state->scratch, full_state->scratch,
                           sizeof(rebaked_state->scratch)) == 0, 1);
}

/* Full-flatten `scene` at t0 into the live program, rebake at t1, and compare
 * against a full flatten at t1. Asserts the change actually took the rebake
 * route (not a full re-flatten). */
static void check_rebake_matches_full(const char *name,
                                      const char *const *scene,
                                      float t0, float t1) {
    char label[192];
    ValueBaseline before_refresh;
    ValueBaseline rebaked_state;
    ValueBaseline full_state;

    load_scene(scene);
    repl_state_time_set(t0);
    repl_ensure_flat_program_with_live_vars(0);

    /* Change t. For a value-only scene this routes to args_dirty_mask; a
     * full re-flatten is NOT expected here. */
    repl_state_flat_program_clear_dirty();
    repl_state_time_set(t1);

    snprintf(label, sizeof(label), "%s: t change routes to rebake (not full)",
             name);
    TEST_ASSERT_INT(&g_harness, label, repl_state_flat_program_dirty(), 0);
    snprintf(label, sizeof(label), "%s: t change set args-dirty", name);
    TEST_ASSERT_TRUE(&g_harness, label,
                     repl_state_flat_program_args_dirty_mask() != 0);
    baseline_capture(&before_refresh);

    /* Refresh the live program through the public boundary at t1. */
    snprintf(label, sizeof(label), "%s: rebake succeeds", name);
    TEST_ASSERT_INT(&g_harness, label, repl_refresh_flat_program(0),
                    REPL_FLAT_REFRESH_REBAKE);
    snprintf(label, sizeof(label), "%s: rebake cleared args-dirty", name);
    TEST_ASSERT_INT(&g_harness, label,
                    (int)repl_state_flat_program_args_dirty_mask(), 0);
    baseline_capture(&rebaked_state);

    /* Reference: a full flatten of the same scene at t1. Predef 't' is
     * already at t1. Restore the exact pre-refresh value/scratch baseline so
     * self-referential assignments are evaluated once in both paths. */
    baseline_restore(&before_refresh);
    full_flatten_into(&g_ref);
    baseline_capture(&full_state);
    assert_live_matches_reference(name, &rebaked_state, &full_state);
    baseline_restore(&before_refresh);
}

/* value-only: amp scales already-emitted vertices, t drives a vertex arg. */
static const char *const k_scene_value[] = {
    "float amp = 1;",
    "for(i, 0, 20) {",
    "glBegin(GL_TRIANGLES);",
    "glVertex3f(amp*i, sin(t + i), 0);",
    "glVertex3f(amp*i, amp, cos(t));",
    "glEnd();",
    "}",
    NULL,
};

/* assignment threading: n = t*2 feeds later vertices; the rebake must apply
 * the assignment in stream order so downstream reads see it. */
static const char *const k_scene_assign[] = {
    "float n;",
    "n = t * 2;",
    "glBegin(GL_LINES);",
    "glVertex3f(n, 0, 0);",
    "glVertex3f(n + 1, t, 0);",
    "glEnd();",
    NULL,
};

/* scratch threading in a value position. */
static const char *const k_scene_scratch[] = {
    "A[0] = t;",
    "A[1] = A[0] * 2;",
    "glBegin(GL_POINTS);",
    "glVertex3f(A[0], A[1], 0);",
    "glEnd();",
    NULL,
};

/* Constant writes are part of assignment threading even though their own
 * baked args do not change. Rebake must replay the reset before the dynamic
 * self-reference, for both scalar and scratch storage. */
static const char *const k_scene_constant_resets[] = {
    "float n;",
    "n = 1;",
    "n = n + t;",
    "A[0] = 2;",
    "A[0] = A[0] + t;",
    "glVertex3f(n, A[0], t);",
    NULL,
};

/* A function-scoped local is the one assignment target rebake must NOT
 * re-evaluate. Its FlatCmdLocalVars snapshot is taken *after* the write, so
 * re-running a self-referential row like `u = u * 2` against it would apply
 * the row twice. Nothing here makes the local's value stale: `u` depends only
 * on a constant argument, while `t` reaches an ordinary vertex, so a `t`
 * change is value-only and takes the rebake route past the local rows. */
static const char *const k_scene_local_self_ref[] = {
    "float k = 3;",
    "spoke(seed) {",
    "float u;",
    "u = seed + 1;",
    "u = u * 2;",
    "glVertex3f(u, 0, 0);",
    "}",
    "glBegin(GL_LINES);",
    "spoke(k);",
    "glVertex3f(k*t, 1, 0);",
    "glEnd();",
    NULL,
};

/* A stencil reference is an expression slot, not an enum or a pre-truncated
 * parser literal. It must be clamped after every evaluation, including the
 * fast rebake path. */
static const char *const k_scene_stencil_ref[] = {
    "glStencilFunc(GL_EQUAL, t * 100.75 - 1.9, 0xFF);",
    NULL,
};

static int live_stencil_ref(void) {
    const GLCmd *cmds = repl_state_flat_program_cmds();
    int n = repl_state_flat_program_count();

    for (int i = 0; i < n; i++)
        if (cmds[i].type == CMD_STENCIL_FUNC)
            return (int)cmds[i].args[1];
    return -1;
}

static int c_stencil_ref(float t) {
    GLint ref = (GLint)(t * 100.75f - 1.9f);

    if (ref < 0)
        return 0;
    if (ref > 255)
        return 255;
    return (int)ref;
}

static void test_dynamic_stencil_ref_rebake(void) {
    static const float values[] = { -0.1f, 0.05f, 3.0f };
    const char *const loop_scene[] = {
        "for(i, 0, 4) {",
        "glStencilFunc(GL_EQUAL, i + 0.9, 0xFF);",
        "}",
        NULL,
    };

    printf("--- rebake: dynamic stencil reference ---\n");
    load_scene(k_scene_stencil_ref);
    repl_state_time_set(0.0f);
    repl_ensure_flat_program_with_live_vars(0);

    for (int i = 0; i < (int)ARRAY_LEN(values); i++) {
        char label[160];

        repl_state_flat_program_clear_dirty();
        repl_state_time_set(values[i]);
        snprintf(label, sizeof(label), "stencil ref %g uses rebake", values[i]);
        TEST_ASSERT_INT(&g_harness, label, repl_refresh_flat_program(0),
                        REPL_FLAT_REFRESH_REBAKE);
        snprintf(label, sizeof(label),
                 "stencil ref %g matches C GLint truncation and clamp", values[i]);
        TEST_ASSERT_INT(&g_harness, label, live_stencil_ref(),
                        c_stencil_ref(values[i]));
    }

    /* A cache-warm full reflatten has the same post-evaluation clamp. */
    repl_state_time_set(0.05f);
    (void)repl_refresh_flat_program(0);
    repl_state_mark_flat_dirty();
    ASSERT_INT("stencil ref warm-cache refresh takes full flatten",
               repl_refresh_flat_program(0), REPL_FLAT_REFRESH_FULL);
    ASSERT_INT("stencil ref warm-cache reflatten keeps truncation",
               live_stencil_ref(), c_stencil_ref(0.05f));

    /* glClearStencil rides the same policy through its own fixup arm: the
     * clear value is the buffer the viz reads, so an unclamped rebake here
     * would be worse than for ref. */
    {
        static const char *const clear_scene[] = {
            "glClearStencil(t * 100.75 - 1.9);",
            NULL,
        };
        load_scene(clear_scene);
        repl_state_time_set(0.0f);
        repl_ensure_flat_program_with_live_vars(0);

        for (int i = 0; i < (int)ARRAY_LEN(values); i++) {
            const GLCmd *cmds;
            int found = -1;
            char label[160];

            repl_state_flat_program_clear_dirty();
            repl_state_time_set(values[i]);
            snprintf(label, sizeof(label), "clear stencil %g uses rebake",
                     values[i]);
            TEST_ASSERT_INT(&g_harness, label, repl_refresh_flat_program(0),
                            REPL_FLAT_REFRESH_REBAKE);
            cmds = repl_state_flat_program_cmds();
            for (int k = 0; k < repl_state_flat_program_count(); k++)
                if (cmds[k].type == CMD_CLEAR_STENCIL)
                    found = (int)cmds[k].args[0];
            snprintf(label, sizeof(label),
                     "clear stencil %g matches C GLint truncation and clamp",
                     values[i]);
            TEST_ASSERT_INT(&g_harness, label, found, c_stencil_ref(values[i]));
        }
    }

    /* Local loop variables exercise the normal flatten path rather than the
     * value-only rebake path. Each fractional local reference truncates after
     * substitution, exactly like a predef-backed expression. */
    load_scene(loop_scene);
    repl_ensure_flat_program_with_live_vars(0);
    {
        const GLCmd *cmds = repl_state_flat_program_cmds();
        int found = 0;

        for (int i = 0; i < repl_state_flat_program_count(); i++) {
            if (cmds[i].type != CMD_STENCIL_FUNC)
                continue;
            ASSERT_INT("loop stencil ref truncates local value",
                       (int)cmds[i].args[1], found);
            found++;
        }
        ASSERT_INT("loop emits four dynamic stencil refs", found, 4);
    }
}

static void test_rebake_value_scene(void) {
    printf("--- rebake: value-only scene ---\n");
    check_rebake_matches_full("value", k_scene_value, 0.0f, 1.7f);
}

static void test_rebake_assignment_scene(void) {
    printf("--- rebake: assignment threading ---\n");
    check_rebake_matches_full("assign", k_scene_assign, 0.25f, 2.5f);
}

static void test_rebake_scratch_scene(void) {
    printf("--- rebake: scratch threading ---\n");
    check_rebake_matches_full("scratch", k_scene_scratch, 0.5f, 3.0f);
}

static void test_rebake_replays_constant_assignment_resets(void) {
    printf("--- rebake: constant assignment resets ---\n");
    check_rebake_matches_full("constant resets", k_scene_constant_resets,
                              0.0f, 2.0f);
}

static void test_rebake_leaves_local_assignments_alone(void) {
    printf("--- rebake: local-target assignments are not re-evaluated ---\n");
    check_rebake_matches_full("local self-reference",
                              k_scene_local_self_ref, 0.5f, 1.25f);
}

/* A structural change must refuse the value-only rebake route: changing a
 * loop-bound variable marks the program fully dirty, not args-dirty, so the
 * frame gate re-flattens. */
static void test_structural_change_takes_full_flatten(void) {
    printf("--- rebake: structural change refuses rebake ---\n");
    const char *const scene[] = {
        "float bound = 4;",
        "for(i, 0, bound) {",
        "glVertex3f(i, 0, 0);",
        "}",
        NULL,
    };
    load_scene(scene);
    repl_ensure_flat_program_with_live_vars(0);

    int b_idx = repl_eval_find_predef_var_idx("bound");
    ASSERT_TRUE("bound predef exists", b_idx >= 0);

    repl_state_flat_program_clear_dirty();
    g_predef_vars_mut[b_idx].value = 8.0f;
    repl_state_notify_predef_value_changed(b_idx);

    ASSERT_INT("structural change marks full dirty",
               repl_state_flat_program_dirty(), 1);
    ASSERT_INT("structural change leaves args-dirty clean",
               (int)repl_state_flat_program_args_dirty_mask(), 0);
}

/* rebake_ok == 0 (a scene the last flatten could not fully compile) refuses
 * the in-place rebake so the caller falls back to a full flatten. */
static void test_not_ok_routes_to_full_flatten(void) {
    printf("--- refresh: rebake_ok=0 routes to full flatten ---\n");
    const char *const scene[] = { "glVertex3f(t, 0, 0);", NULL };
    load_scene(scene);
    repl_ensure_flat_program_with_live_vars(0);

    /* Force the eligibility flag off (as a partially-compiled scene would). */
    repl_state_flat_program_set_dep_state(
        repl_state_flat_program_structural_dep_mask(),
        repl_state_flat_program_value_dep_mask(), /*rebake_ok=*/0);
    repl_state_time_set(1.0f);

    ASSERT_INT("!rebake_ok value change marks full dirty",
               repl_state_flat_program_dirty(), 1);
    ASSERT_INT("!rebake_ok refresh takes full path",
               repl_refresh_flat_program(0), REPL_FLAT_REFRESH_FULL);
}

/* Force a later cache line to FAILED after a value-only change is routed.
 * The rebake updates n before it reaches that stale line. The refresh
 * boundary must restore n before its full-flatten fallback; otherwise the
 * self-referential assignment runs twice and leaves n==3 instead of 2. */
static void test_midwalk_failure_rolls_back_before_full(void) {
    const char *const scene[] = {
        "float n = 1;",
        "n = n + t;",
        "glVertex3f(n, t, 0);",
        NULL,
    };
    ReplExprCache *cache;
    int n_idx;

    printf("--- refresh: mid-walk rebake failure rolls back ---\n");
    load_scene(scene);
    n_idx = repl_eval_find_predef_var_idx("n");
    ASSERT_TRUE("rollback scene n predef exists", n_idx >= 0);
    ASSERT_TRUE("later expression line begins forced failure",
                repl_expr_cache_line_begin(repl_expr_cache_live(), 2));
    cache = repl_expr_cache_live();
    repl_expr_cache_line_finish(cache, 2, 0);

    repl_state_time_set(1.0f);
    ASSERT_TRUE("rollback scene routes value refresh",
                repl_state_flat_program_args_dirty_mask() != 0);
    ASSERT_INT("stale line escalates rebake to full",
               repl_refresh_flat_program(0), REPL_FLAT_REFRESH_FULL);
    ASSERT_TRUE("failed rebake restored n before full fallback",
                n_idx >= 0 && g_predef_vars_mut[n_idx].value == 2.0f);
}

static void test_negative_flat_count_is_invalid(void) {
    ReplRebakeOptions options;
    ReplRebakeResult result;

    printf("--- rebake: negative flat count rejected ---\n");
    memset(&options, 0, sizeof(options));
    options.flat_count = -1;
    options.expr_cache = repl_expr_cache_live();
    ASSERT_INT("negative flat count fails",
               repl_flatten_rebake_program(&options, &result), 0);
}

static int example_index_by_name(const char *name) {
    for (int i = 0; i < repl_example_count(); i++)
        if (strcmp(repl_example_name(i), name) == 0)
            return i;
    return -1;
}

static void assert_real_scene_time_route(const char *display_name,
                                         ReplFlatRefreshKind expected) {
    char label[192];
    int idx = example_index_by_name(display_name);

    snprintf(label, sizeof(label), "%s: example exists", display_name);
    TEST_ASSERT_TRUE(&g_harness, label, idx >= 0);
    if (idx < 0)
        return;
    repl_load_example_lines_for_test(repl_example_lines(idx));
    repl_state_time_set(0.0f);
    repl_ensure_flat_program_with_live_vars(0);
    repl_state_flat_program_clear_dirty();
    repl_state_time_set(1.0f);

    snprintf(label, sizeof(label), "%s: production t route", display_name);
    TEST_ASSERT_INT(&g_harness, label, repl_refresh_flat_program(0), expected);
}

static void test_real_scene_time_routes(void) {
    printf("--- refresh: real-scene t routes ---\n");
    /* Wave is the production value-only case: `t` reaches vertex arguments
     * and nothing else, so the existing flat topology is reusable. */
    assert_real_scene_time_route("Animated wave surface (analytic normals)",
                                 REPL_FLAT_REFRESH_REBAKE);
    /* Grass used to rebake here. Its blade() temporaries are function-scoped
     * locals now, and `t` feeds them — every dep of a local's RHS is
     * structural because a rebake cannot carry a local's new value into the
     * frozen snapshots of the commands after it. Full flatten is the correct
     * route, and the measured cost on this scene is ~1.14 ms -> ~1.59 ms per
     * refresh (bench_repl --only flatten_refresh). */
    assert_real_scene_time_route("Swaying grass field (rand + t)",
                                 REPL_FLAT_REFRESH_FULL);
    assert_real_scene_time_route("Orrery (labels track 3D orbits)",
                                 REPL_FLAT_REFRESH_FULL);
    assert_real_scene_time_route("Whale (particle system + lit model)",
                                 REPL_FLAT_REFRESH_FULL);
}

static int profile_histogram_samples(ProfSection section) {
    ProfHistogramBin bins[PROF_HISTOGRAM_BIN_COUNT];
    int total = 0;

    prof_section_histogram(section, bins, PROF_HISTOGRAM_BIN_COUNT);
    for (int i = 0; i < PROF_HISTOGRAM_BIN_COUNT; i++)
        total += (int)bins[i];
    return total;
}

/* The display-frame scope must turn an initial refresh plus N motion-blur
 * refreshes into one profiler sample. Outside that scope the same public
 * boundary remains one-sample-per-call for exports/debug/replay/bench. This
 * checks histogram publication counts rather than wall-clock magnitudes so
 * the contract is deterministic under sanitizers and slow CI hosts. */
static void test_refresh_profile_frame_aggregation(void) {
    static const char *const scene[] = {
        "glVertex3f(t, 0, 0);",
        NULL
    };

    printf("--- refresh: displayed-frame profile aggregation ---\n");
    load_scene(scene);

    prof_test_reset();
    repl_state_time_set(1.0f);
    ASSERT_INT("standalone rebake refresh 1",
               repl_refresh_flat_program(0), REPL_FLAT_REFRESH_REBAKE);
    repl_state_time_set(2.0f);
    ASSERT_INT("standalone rebake refresh 2",
               repl_refresh_flat_program(0), REPL_FLAT_REFRESH_REBAKE);
    ASSERT_INT("standalone rebakes publish per-call parent samples",
               profile_histogram_samples(PROF_REBAKE), 2);
    ASSERT_INT("standalone rebakes publish per-call eval samples",
               profile_histogram_samples(PROF_REBAKE_EVAL), 2);

    prof_test_reset();
    repl_flat_refresh_profile_frame_begin();
    repl_state_time_set(3.0f);
    ASSERT_INT("frame rebake refresh 1",
               repl_refresh_flat_program(0), REPL_FLAT_REFRESH_REBAKE);
    repl_state_time_set(4.0f);
    ASSERT_INT("frame rebake refresh 2",
               repl_refresh_flat_program(0), REPL_FLAT_REFRESH_REBAKE);
    repl_flat_refresh_profile_frame_end();
    ASSERT_INT("frame rebakes commit one parent total",
               profile_histogram_samples(PROF_REBAKE), 1);
    ASSERT_INT("frame rebakes commit one eval-walk total",
               profile_histogram_samples(PROF_REBAKE_EVAL), 1);
    ASSERT_TRUE("rebake parent contains eval-walk child",
                prof_section_last_us(PROF_REBAKE) >=
                    prof_section_last_us(PROF_REBAKE_EVAL));

    prof_test_reset();
    repl_flat_refresh_profile_frame_begin();
    repl_state_mark_flat_dirty();
    ASSERT_INT("frame full refresh 1",
               repl_refresh_flat_program(0), REPL_FLAT_REFRESH_FULL);
    repl_state_mark_flat_dirty();
    ASSERT_INT("frame full refresh 2",
               repl_refresh_flat_program(0), REPL_FLAT_REFRESH_FULL);
    repl_flat_refresh_profile_frame_end();
    ASSERT_INT("frame full refreshes commit one parent total",
               profile_histogram_samples(PROF_FLATTEN), 1);
    ASSERT_INT("frame full refreshes commit one reparse-child total",
               profile_histogram_samples(PROF_FLATTEN_REPARSE), 1);
    ASSERT_TRUE("flatten parent contains reparse child",
                prof_section_last_us(PROF_FLATTEN) >=
                    prof_section_last_us(PROF_FLATTEN_REPARSE));
}

/* Exercise every predefined root which each built-in's dependency analysis
 * classifies as value-only. This is the corpus-scale contract Phase 3 relies
 * on: rebake and a warm full flatten must agree from the same pre-refresh
 * value baseline for every root the router permits to rebake. */
static void test_corpus_value_only_rebakes(void) {
    const float t0 = 0.37f;
    int eligible = 0;

    printf("--- rebake: eligible corpus predef differential ---\n");
    for (int e = 0; e < repl_example_count(); e++) {
        ValueBaseline scene_base;
        ReplExprDepMask structural;
        ReplExprDepMask values;

        repl_load_example_lines_for_test(repl_example_lines(e));
        repl_state_time_set(t0);
        repl_ensure_flat_program_with_live_vars(0);
        if (!repl_state_flat_program_rebake_ok())
            continue;
        structural = repl_state_flat_program_structural_dep_mask();
        values = repl_state_flat_program_value_dep_mask();
        baseline_capture(&scene_base);

        for (int slot = 0; slot < g_num_predef_vars; slot++) {
            ValueBaseline before_refresh;
            ValueBaseline rebaked_state;
            ValueBaseline full_state;
            ReplExprDepMask bit = (ReplExprDepMask)1u << slot;
            char name[224];

            if ((structural & bit) || !(values & bit))
                continue;
            baseline_restore(&scene_base);
            repl_state_flat_program_clear_dirty();
            repl_state_flat_program_clear_args_dirty();
            g_predef_vars_mut[slot].value +=
                0.125f + 0.03125f * (float)(slot & 3);
            repl_state_notify_predef_value_changed(slot);
            baseline_capture(&before_refresh);
            snprintf(name, sizeof(name), "corpus[%d] %s / %s", e,
                     repl_example_name(e), g_predef_vars[slot].name);
            {
                char label[256];
                snprintf(label, sizeof(label), "%s: refresh uses rebake",
                         name);
                TEST_ASSERT_INT(&g_harness, label,
                                repl_refresh_flat_program(0),
                                REPL_FLAT_REFRESH_REBAKE);
            }
            baseline_capture(&rebaked_state);

            baseline_restore(&before_refresh);
            full_flatten_into(&g_ref);
            baseline_capture(&full_state);
            assert_live_matches_reference(name, &rebaked_state, &full_state);
            eligible++;
        }
    }
    ASSERT_TRUE("corpus contains value-only predef rebake cases", eligible > 0);
}

int main(void) {
    printf("--- repl flatten rebake differential ---\n");
    test_rebake_value_scene();
    test_rebake_assignment_scene();
    test_rebake_scratch_scene();
    test_rebake_replays_constant_assignment_resets();
    test_rebake_leaves_local_assignments_alone();
    test_dynamic_stencil_ref_rebake();
    test_structural_change_takes_full_flatten();
    test_not_ok_routes_to_full_flatten();
    test_midwalk_failure_rolls_back_before_full();
    test_negative_flat_count_is_invalid();
    test_real_scene_time_routes();
    test_refresh_profile_frame_aggregation();
    test_corpus_value_only_rebakes();
    return test_harness_report(&g_harness, "test_repl_flatten_rebake");
}
