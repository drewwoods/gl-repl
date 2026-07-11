/*
 * tests/test_repl_flatten_deps.c -- flatten dependency-mask derivation
 * (flatten plan phase 3a).
 *
 * Each case builds a small scene through the live commit pipeline, runs a
 * full flatten, and asserts the structural/value dependency masks and
 * rebake_ok the flatten reported — plus the dirty routing those masks drive
 * through repl_state_notify_predef_value_changed. Exact-equality asserts
 * are used wherever the expected mask is fully known, so overbroad masks
 * (which would silently cost performance) fail as loudly as missing bits
 * (which would break correctness).
 */
#include "app/glr_ctrl.h"
#include "editor/input.h"
#include "repl/eval.h"
#include "repl/pipeline.h"
#include "repl/state.h"
#include "repl/state_owners.h"
#include "repl/expr_program.h"

#include "support/test_harness.h"
#include <stdio.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    TEST_ASSERT_INT(&g_harness, label, got, exp); \
} while (0)

/* Feed `lines` through the commit pipeline into a fresh scene, then run a
 * full flatten so the dep masks describe exactly this program. */
static void load_scene(const char *const *lines, int count) {
    glr_ctrl_reset_all();
    for (int i = 0; i < count; i++)
        editor_feed_line(lines[i]);
    repl_flatten_commands(0);
    repl_state_flat_program_clear_dirty();
}

static ReplExprDepMask predef_bit(const char *name) {
    int idx = repl_eval_find_predef_var_idx(name);
    return idx >= 0 ? (ReplExprDepMask)1u << idx : 0;
}

/* glVertex3f(t, ...): t is a value-only root — a rebake suffices, so a t
 * change routes to args_dirty_mask, and a full flatten clears it again. */
static void test_value_only_t(void) {
    printf("--- value-only t ---\n");
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "glVertex3f(t, 0, 0);",
        "glEnd();",
    };
    load_scene(lines, 3);
    ReplExprDepMask t_bit = predef_bit("t");

    ASSERT_TRUE("t predef exists", t_bit != 0);
    ASSERT_INT("structural mask is empty",
               (int)repl_state_flat_program_structural_dep_mask(), 0);
    ASSERT_TRUE("value mask is exactly {t}",
                repl_state_flat_program_value_dep_mask() == t_bit);
    ASSERT_INT("compiled scene supports rebake",
               repl_state_flat_program_rebake_ok(), 1);

    repl_state_time_set(1.5f);
    ASSERT_INT("t change leaves full flag clean",
               repl_state_flat_program_dirty(), 0);
    ASSERT_TRUE("t change routes to t's args-dirty bit",
                repl_state_flat_program_args_dirty_mask() == t_bit);

    repl_flatten_commands(0);
    ASSERT_INT("full flatten clears args-dirty",
               (int)repl_state_flat_program_args_dirty_mask(), 0);
}

/* for(i, 0, t + 2): the loop bound decides topology, so t is structural —
 * and the iterator local carries {t} into the body's value deps. */
static void test_structural_t_via_loop_bound(void) {
    printf("--- structural t via loop bound ---\n");
    const char *lines[] = {
        "for(i, 0, t + 2) {",
        "glVertex3f(i, 0, 0);",
        "}",
    };
    load_scene(lines, 3);
    ReplExprDepMask t_bit = predef_bit("t");

    ASSERT_TRUE("structural mask is exactly {t}",
                repl_state_flat_program_structural_dep_mask() == t_bit);
    ASSERT_TRUE("iterator carries {t} into value mask",
                repl_state_flat_program_value_dep_mask() == t_bit);

    repl_state_time_set(1.0f);
    ASSERT_INT("structural t change marks flat dirty",
               repl_state_flat_program_dirty(), 1);
    ASSERT_INT("structural t change leaves args-dirty clean",
               (int)repl_state_flat_program_args_dirty_mask(), 0);
}

/* if(n > 1): an evaluated condition selects which commands exist, so its
 * roots are structural even when the arm is currently not taken. */
static void test_structural_via_if_condition(void) {
    printf("--- structural via if condition ---\n");
    const char *lines[] = {
        "float n;",
        "if(n > 1) {",
        "glVertex3f(1, 0, 0);",
        "}",
    };
    load_scene(lines, 4);
    ReplExprDepMask n_bit = predef_bit("n");

    ASSERT_TRUE("n predef exists", n_bit != 0);
    ASSERT_TRUE("condition puts n in structural mask",
                (repl_state_flat_program_structural_dep_mask() & n_bit)
                    == n_bit);

    repl_state_notify_predef_value_changed(
        repl_eval_find_predef_var_idx("n"));
    ASSERT_INT("structural n change marks flat dirty",
               repl_state_flat_program_dirty(), 1);
}

/* func0(n): call arguments freeze into local snapshots (structural), and
 * the parameter binding carries {n} into the body's value deps. */
static void test_structural_via_call_arg(void) {
    printf("--- structural via call arg ---\n");
    const char *lines[] = {
        "float n;",
        "func0(a) {",
        "glTranslatef(a, 0, 0);",
        "}",
        "func0(n);",
    };
    load_scene(lines, 5);
    ReplExprDepMask n_bit = predef_bit("n");

    ASSERT_TRUE("call arg puts n in structural mask",
                (repl_state_flat_program_structural_dep_mask() & n_bit)
                    == n_bit);
    ASSERT_TRUE("param binding carries {n} into value mask",
                (repl_state_flat_program_value_dep_mask() & n_bit) == n_bit);
}

/* n = t * 2; glVertex3f(n, ...): reads of n after the assignment resolve
 * to the RHS's roots {t}, so n's own bit drops out of the masks entirely —
 * a slider change to n is a routing no-op (the reflatten would re-bake
 * n = t * 2 over it anyway). */
static void test_transitive_assignment(void) {
    printf("--- transitive assignment ---\n");
    const char *lines[] = {
        "float n;",
        "n = t * 2;",
        "glVertex3f(n, 0, 0);",
    };
    load_scene(lines, 3);
    ReplExprDepMask t_bit = predef_bit("t");
    ReplExprDepMask n_bit = predef_bit("n");

    ASSERT_TRUE("value mask is exactly {t}",
                repl_state_flat_program_value_dep_mask() == t_bit);
    ASSERT_INT("structural mask is empty",
               (int)repl_state_flat_program_structural_dep_mask(), 0);
    ASSERT_TRUE("n's own bit is absent",
                (repl_state_flat_program_value_dep_mask() & n_bit) == 0);

    repl_state_notify_predef_value_changed(
        repl_eval_find_predef_var_idx("n"));
    ASSERT_INT("overwritten n change is a routing no-op (dirty)",
               repl_state_flat_program_dirty(), 0);
    ASSERT_INT("overwritten n change is a routing no-op (args)",
               (int)repl_state_flat_program_args_dirty_mask(), 0);
}

/* n = n + 1: a self-referencing assignment keeps n in its own mask, so a
 * live n change still routes. */
static void test_self_referencing_assignment(void) {
    printf("--- self-referencing assignment ---\n");
    const char *lines[] = {
        "float n;",
        "n = n + 1;",
        "glVertex3f(n, 0, 0);",
    };
    load_scene(lines, 3);
    ReplExprDepMask n_bit = predef_bit("n");

    ASSERT_TRUE("self-ref keeps n in value mask",
                (repl_state_flat_program_value_dep_mask() & n_bit) == n_bit);
}

/* A constant program depends on nothing: both masks empty, and any predef
 * change routes to neither the full flag nor args-dirty. */
static void test_constant_program(void) {
    printf("--- constant program ---\n");
    const char *lines[] = {
        "for(i, 0, 10) {",
        "glVertex3f(i, 0, i / 10);",
        "}",
    };
    load_scene(lines, 3);

    ASSERT_INT("structural mask is empty",
               (int)repl_state_flat_program_structural_dep_mask(), 0);
    ASSERT_INT("value mask is empty",
               (int)repl_state_flat_program_value_dep_mask(), 0);
    ASSERT_INT("constant scene supports rebake",
               repl_state_flat_program_rebake_ok(), 1);

    repl_state_time_set(3.0f);
    ASSERT_INT("t change on constant scene is a no-op (dirty)",
               repl_state_flat_program_dirty(), 0);
    ASSERT_INT("t change on constant scene is a no-op (args)",
               (int)repl_state_flat_program_args_dirty_mask(), 0);
}

/* for(i, 0, A[0]): scratch cells carry no dependency metadata (v1 rule),
 * so a scratch read in a structural position widens to all bits. */
static void test_scratch_read_in_structural_position(void) {
    printf("--- scratch read in structural position ---\n");
    const char *lines[] = {
        "A[0] = 4;",
        "for(i, 0, A[0]) {",
        "glVertex3f(i, 0, 0);",
        "}",
    };
    load_scene(lines, 4);

    ASSERT_TRUE("scratch-fed loop bound widens structural to all bits",
                repl_state_flat_program_structural_dep_mask()
                    == REPL_EXPR_DEP_ALL);
}

int main(void) {
    printf("--- repl flatten dependency-mask tests ---\n");
    test_value_only_t();
    test_structural_t_via_loop_bound();
    test_structural_via_if_condition();
    test_structural_via_call_arg();
    test_transitive_assignment();
    test_self_referencing_assignment();
    test_constant_program();
    test_scratch_read_in_structural_position();
    return test_harness_report(&g_harness, "test_repl_flatten_deps");
}
