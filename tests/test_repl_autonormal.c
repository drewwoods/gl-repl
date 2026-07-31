#include "app/glr_ctrl.h"
#include "repl/pipeline.h"
#include "editor/input.h"
#include "repl/state_owners.h"
#include "support/test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_FLOAT(label, got, exp) \
    TEST_ASSERT_FLOAT(&g_harness, label, got, exp, 1e-5f)


static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("x", err, sizeof(err));
    repl_eval_declare_predef_var("y", err, sizeof(err));
    repl_eval_declare_predef_var("z", err, sizeof(err));
    repl_eval_declare_predef_var("n", err, sizeof(err));
    repl_eval_declare_predef_var("i", err, sizeof(err));
}

/* ------------------------------------------------------------------ */
/* face_normal: degenerate case (collinear vertices → zero normal)     */
/* ------------------------------------------------------------------ */

static void test_degenerate_normal(void) {
    printf("test_degenerate_normal\n");

    glr_ctrl_reset_all(); declare_test_vars();
    /* All three vertices collinear along x - cross product is zero */
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(2, 0, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(1, NULL);

    /* An auto-normal should be inserted before each vertex */
    ASSERT_INT("degenerate: cmd count", repl_state_document_count(), 8);
    ASSERT_TRUE("degenerate: first inserted is auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    ASSERT_FLOAT("degenerate: normal x is 0", repl_state_document_cmds_mut()[1].args[0], 0.0f);
    ASSERT_FLOAT("degenerate: normal y is 0", repl_state_document_cmds_mut()[1].args[1], 0.0f);
    ASSERT_FLOAT("degenerate: normal z is 0", repl_state_document_cmds_mut()[1].args[2], 0.0f);

}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_TRIANGLE_STRIP                            */
/* ------------------------------------------------------------------ */

static void test_triangle_strip(void) {
    printf("test_triangle_strip\n");

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_TRIANGLE_STRIP);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glVertex3f(1, 1, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(1, NULL);

    /* 4 vertices + 4 auto normals + BEGIN + END */
    ASSERT_INT("strip: cmd count", repl_state_document_count(), 10);
    ASSERT_TRUE("strip: v0 has auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    ASSERT_TRUE("strip: v1 has auto normal", repl_state_document_cmds_mut()[3].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[3].is_auto);
    /* All normals for a flat strip in xy-plane should point along +z */
    ASSERT_FLOAT("strip: n0 z", repl_state_document_cmds_mut()[1].args[2], 1.0f);
    ASSERT_FLOAT("strip: n1 z", repl_state_document_cmds_mut()[3].args[2], 1.0f);

}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_TRIANGLE_FAN                              */
/* ------------------------------------------------------------------ */

static void test_triangle_fan(void) {
    printf("test_triangle_fan\n");

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_TRIANGLE_FAN);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glVertex3f(-1, 0, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(1, NULL);

    /* 4 vertices + 4 auto normals + BEGIN + END */
    ASSERT_INT("fan: cmd count", repl_state_document_count(), 10);
    ASSERT_TRUE("fan: v0 has auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    /* Fan in xy-plane, normal should be +z */
    ASSERT_FLOAT("fan: n0 z", repl_state_document_cmds_mut()[1].args[2], 1.0f);

}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_QUADS                                     */
/* ------------------------------------------------------------------ */

static void test_quads(void) {
    printf("test_quads\n");

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_QUADS);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(1, 1, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(1, NULL);

    /* 4 vertices + 4 auto normals + BEGIN + END */
    ASSERT_INT("quads: cmd count", repl_state_document_count(), 10);
    ASSERT_TRUE("quads: v0 has auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    /* Quad in xy-plane: cross (1,0,0)x(1,1,0) = (0,0,1) */
    ASSERT_FLOAT("quads: n0 z", repl_state_document_cmds_mut()[1].args[2], 1.0f);

}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_QUAD_STRIP                                */
/* ------------------------------------------------------------------ */

static void test_quad_strip(void) {
    printf("test_quad_strip\n");

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_QUAD_STRIP);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glVertex3f(1, 1, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(1, NULL);

    /* 4 vertices + 4 auto normals + BEGIN + END */
    ASSERT_INT("quad_strip: cmd count", repl_state_document_count(), 10);
    ASSERT_TRUE("quad_strip: v0 has auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    /* First quad: v0=(0,0,0) v1=(1,0,0) v2=(0,1,0). Normal = (0,0,1) */
    ASSERT_FLOAT("quad_strip: n0 z", repl_state_document_cmds_mut()[1].args[2], 1.0f);

}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_POLYGON                                   */
/* ------------------------------------------------------------------ */

static void test_polygon(void) {
    printf("test_polygon\n");

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_POLYGON);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(1, 1, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(1, NULL);

    /* 4 vertices + 4 auto normals + BEGIN + END */
    ASSERT_INT("polygon: cmd count", repl_state_document_count(), 10);
    ASSERT_TRUE("polygon: v0 has auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    ASSERT_FLOAT("polygon: n0 z", repl_state_document_cmds_mut()[1].args[2], 1.0f);

}

/* ------------------------------------------------------------------ */
/* compute_block_normals: unsupported mode (default branch → no-op)   */
/* ------------------------------------------------------------------ */

static void test_unsupported_mode(void) {
    printf("test_unsupported_mode\n");

    glr_ctrl_reset_all(); declare_test_vars();
    /* GL_LINES is valid for glBegin but not handled in compute_block_normals */
    editor_feed_line("glBegin(GL_LINES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(1, NULL);

    /* compute_block_normals default branch leaves all norms at zero,
     * but recompute_autonormals still inserts (0,0,0) auto-normals */
    ASSERT_INT("unsupported mode: auto normals still inserted", repl_state_document_count(), 6);
    ASSERT_TRUE("unsupported mode: inserted cmd is auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    ASSERT_FLOAT("unsupported mode: normal is zero x", repl_state_document_cmds_mut()[1].args[0], 0.0f);
    ASSERT_FLOAT("unsupported mode: normal is zero z", repl_state_document_cmds_mut()[1].args[2], 0.0f);

}

/* ------------------------------------------------------------------ */
/* recompute_autonormals: skips for/func/if blocks                     */
/* ------------------------------------------------------------------ */

static void test_block_skipping(void) {
    printf("test_block_skipping\n");

    /* for-loop block before glBegin */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("for(n, 0, 2) {");
    editor_feed_line("glVertex3f(n, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    int cmds_before = repl_state_document_count();
    repl_recompute_autonormals(1, NULL);

    /* 3 normals inserted into the glBegin block; for-loop was skipped */
    ASSERT_INT("for-skip: cmds added", repl_state_document_count(), cmds_before + 3);

    /* func-def block before glBegin */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("func0(n) {");
    editor_feed_line("glVertex3f(n, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    cmds_before = repl_state_document_count();
    repl_recompute_autonormals(1, NULL);

    ASSERT_INT("func-skip: cmds added", repl_state_document_count(), cmds_before + 3);

    /* if-block before glBegin */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("if(n > 0) {");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    cmds_before = repl_state_document_count();
    repl_recompute_autonormals(1, NULL);

    ASSERT_INT("if-skip: cmds added", repl_state_document_count(), cmds_before + 3);
}

/* ------------------------------------------------------------------ */
/* recompute_autonormals: glr_state_presentation_mut()->autonormal=0 is a no-op                   */
/* ------------------------------------------------------------------ */

static void test_autonormal_disabled(void) {
    printf("test_autonormal_disabled\n");

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(0, NULL);  /* disabled — no normals inserted */
    ASSERT_INT("disabled: no cmds added", repl_state_document_count(), 5);
}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_TRIANGLES + front-face winding            */
/* ------------------------------------------------------------------ */

static void test_gl_triangles(void) {
    printf("test_gl_triangles\n");

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(1, NULL);
    ASSERT_TRUE("autonormal inserts before each triangle vertex", repl_state_document_count() == 8);
    ASSERT_TRUE("autonormal default front-face first cmd type", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F);
    ASSERT_TRUE("autonormal default front-face first cmd auto", repl_state_document_cmds_mut()[1].is_auto == 1);
    ASSERT_TRUE("autonormal default front-face keeps +z", fabsf(repl_state_document_cmds_mut()[1].args[2] - 1.0f) < 1e-6f);

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glFrontFace(GL_CW);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(1, NULL);
    ASSERT_TRUE("autonormal front-face cw inserts before each triangle vertex", repl_state_document_count() == 9);
    ASSERT_TRUE("autonormal front-face cw first cmd type", repl_state_document_cmds_mut()[2].type == CMD_NORMAL3F);
    ASSERT_TRUE("autonormal front-face cw first cmd auto", repl_state_document_cmds_mut()[2].is_auto == 1);
    ASSERT_TRUE("autonormal front-face cw flips z", fabsf(repl_state_document_cmds_mut()[2].args[2] - (-1.0f)) < 1e-6f);

    repl_state_document_cmds_mut()[0].args[0] = GL_CCW;
    repl_recompute_autonormals(1, NULL);
    ASSERT_TRUE("autonormal front-face update flips auto normal back", fabsf(repl_state_document_cmds_mut()[2].args[2] - 1.0f) < 1e-6f);

}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* recompute_autonormals: glBegin INSIDE funcN body with literal coords */
/* ------------------------------------------------------------------ */
/*
 * The autonormal pass used to skip over the entire body of any
 * CMD_FUNC_DEF / CMD_IF_BEGIN / CMD_FOR_BEGIN block. That left any
 * glBegin block inside a funcN body without normals — vertices got
 * the default GL normal (0, 0, 1) at draw time and lighting was
 * silently wrong for any face not in the XY plane.
 *
 * Fix: enter funcN / if / for bodies and process inner glBegin
 * blocks, but only when every vertex in the block has has_vars=0
 * (literal coords). Vars-dependent vertices (function params,
 * for-loop counters, predef refs) have parse-time source args that
 * don't reflect the evaluated values, so cross products on them
 * would emit (0, 0, 0) garbage — strictly worse than no normal.
 */
static void test_autonormal_inside_funcn_literal_coords(void) {
    printf("test_autonormal_inside_funcn_literal_coords\n");

    glr_ctrl_reset_all(); declare_test_vars();
    /* func0 with a literal-coord triangle. The three vertices span the
     * yz plane (varying y and z at x=1), so the cross product is along
     * +X. Auto-normal must compute and insert these. */
    editor_feed_line("func0() {");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(1, 1, 0);");
    editor_feed_line("glVertex3f(1, 0, 1);");
    editor_feed_line("glEnd();");
    editor_feed_line("}");
    int cmds_before = repl_state_document_count();
    repl_recompute_autonormals(1, NULL);

    /* Three auto-normals inserted, one per vertex. */
    ASSERT_INT("funcN literal: cmds added",
               repl_state_document_count(), cmds_before + 3);

    /* Find the first inserted auto-normal and verify it points along
     * +X (cross product of (1,1,0)-(1,0,0)=(0,1,0) and (1,0,1)-(1,0,0)=
     * (0,0,1) is (1,0,0)). With glFrontFace=GL_CCW (default) the
     * outward normal is +X. */
    int auto_idx = -1;
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_document_cmds_mut()[i].valid &&
            repl_state_document_cmds_mut()[i].type == CMD_NORMAL3F &&
            repl_state_document_cmds_mut()[i].is_auto) {
            auto_idx = i;
            break;
        }
    }
    ASSERT_TRUE("funcN literal: auto-normal exists", auto_idx >= 0);
    if (auto_idx >= 0) {
        ASSERT_FLOAT("funcN literal: normal x",
                     repl_state_document_cmds_mut()[auto_idx].args[0], 1.0f);
        ASSERT_FLOAT("funcN literal: normal y",
                     repl_state_document_cmds_mut()[auto_idx].args[1], 0.0f);
        ASSERT_FLOAT("funcN literal: normal z",
                     repl_state_document_cmds_mut()[auto_idx].args[2], 0.0f);
    }
}

/* ------------------------------------------------------------------ */
/* recompute_autonormals: glBegin inside funcN with var-bearing args   */
/* ------------------------------------------------------------------ */
/*
 * When the vertices depend on a function parameter / for-loop
 * counter / any var, source args are parse-time (often 0) and not a
 * reliable basis for cross products. The autonormal pass must NOT
 * insert (0, 0, 0) auto-normals in that case — leave the block
 * untouched so GL falls back to the default normal (or a previously
 * user-supplied glNormal3f survives).
 */
static void test_autonormal_inside_funcn_var_args_skipped(void) {
    printf("test_autonormal_inside_funcn_var_args_skipped\n");

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("func0(s) {");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(s, 0, 0);");
    editor_feed_line("glVertex3f(s, 1, 0);");
    editor_feed_line("glVertex3f(s, 0, 1);");
    editor_feed_line("glEnd();");
    editor_feed_line("}");
    int cmds_before = repl_state_document_count();
    repl_recompute_autonormals(1, NULL);

    /* Vars-bearing vertices → no auto-normals inserted. */
    ASSERT_INT("funcN with vars: cmds unchanged",
               repl_state_document_count(), cmds_before);
}

/* ------------------------------------------------------------------ */
/* REPL_AUTONORMAL_SMOOTH                                              */
/* ------------------------------------------------------------------ */

/* Two right triangles meeting along the edge (0,0,0)-(1,0,0), one in the
 * XY plane (face normal +z), one in the XZ plane (face normal +y), fed as
 * GL_TRIANGLES so the shared corners are *separate* glVertex3f lines.
 * Smooth mode must weld those by position: the two shared corners average
 * to (0, 1, 1)/sqrt(2), the two lone corners keep their own face normal. */
static void feed_folded_pair(void) {
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(0, 0, 1);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glEnd();");
}

static void test_smooth_welds_shared_corners(void) {
    printf("test_smooth_welds_shared_corners\n");

    const float k = 0.70710678f;  /* 1/sqrt(2) */

    feed_folded_pair();
    repl_recompute_autonormals(REPL_AUTONORMAL_SMOOTH, NULL);

    /* BEGIN + 6 vertices + 6 auto-normals + END */
    ASSERT_INT("smooth: cmd count", repl_state_document_count(), 14);

    /* v0 = (0,0,0), shared by both faces */
    ASSERT_FLOAT("smooth: shared v0 x", repl_state_document_cmds_mut()[1].args[0], 0.0f);
    ASSERT_FLOAT("smooth: shared v0 y", repl_state_document_cmds_mut()[1].args[1], k);
    ASSERT_FLOAT("smooth: shared v0 z", repl_state_document_cmds_mut()[1].args[2], k);
    /* v1 = (1,0,0), also shared */
    ASSERT_FLOAT("smooth: shared v1 y", repl_state_document_cmds_mut()[3].args[1], k);
    ASSERT_FLOAT("smooth: shared v1 z", repl_state_document_cmds_mut()[3].args[2], k);
    /* v2 = (0,1,0), only on the XY face */
    ASSERT_FLOAT("smooth: lone v2 y", repl_state_document_cmds_mut()[5].args[1], 0.0f);
    ASSERT_FLOAT("smooth: lone v2 z", repl_state_document_cmds_mut()[5].args[2], 1.0f);
    /* v3 = (0,0,0) again: the same welded average as v0 */
    ASSERT_FLOAT("smooth: duplicate v3 y", repl_state_document_cmds_mut()[7].args[1], k);
    ASSERT_FLOAT("smooth: duplicate v3 z", repl_state_document_cmds_mut()[7].args[2], k);
    /* v4 = (0,0,1), only on the XZ face */
    ASSERT_FLOAT("smooth: lone v4 y", repl_state_document_cmds_mut()[9].args[1], 1.0f);
    ASSERT_FLOAT("smooth: lone v4 z", repl_state_document_cmds_mut()[9].args[2], 0.0f);
    /* v5 = (1,0,0) again */
    ASSERT_FLOAT("smooth: duplicate v5 y", repl_state_document_cmds_mut()[11].args[1], k);
    ASSERT_FLOAT("smooth: duplicate v5 z", repl_state_document_cmds_mut()[11].args[2], k);
}

/* The same geometry under FACE mode keeps hard edges — the guard that
 * smooth mode is an addition, not a change to the existing behavior. */
static void test_face_mode_unchanged_by_smooth(void) {
    printf("test_face_mode_unchanged_by_smooth\n");

    feed_folded_pair();
    repl_recompute_autonormals(REPL_AUTONORMAL_FACE, NULL);

    ASSERT_FLOAT("face: v0 z", repl_state_document_cmds_mut()[1].args[2], 1.0f);
    ASSERT_FLOAT("face: v0 y", repl_state_document_cmds_mut()[1].args[1], 0.0f);
    ASSERT_FLOAT("face: v2 z", repl_state_document_cmds_mut()[5].args[2], 1.0f);
    ASSERT_FLOAT("face: v3 y", repl_state_document_cmds_mut()[7].args[1], 1.0f);
    ASSERT_FLOAT("face: v3 z", repl_state_document_cmds_mut()[7].args[2], 0.0f);
    ASSERT_FLOAT("face: v5 y", repl_state_document_cmds_mut()[11].args[1], 1.0f);
}

/* Strips share vertices by index, so they smooth without any welding.
 * A flat strip must still come out unit-length everywhere (-z for the
 * rung order below): if the alternating winding correction were dropped,
 * the interior vertices would accumulate opposing normals and cancel to
 * (0, 0, 0). */
static void test_smooth_triangle_strip_stays_unit(void) {
    printf("test_smooth_triangle_strip_stays_unit\n");

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_TRIANGLE_STRIP);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(1, 1, 0);");
    editor_feed_line("glVertex3f(2, 0, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(REPL_AUTONORMAL_SMOOTH, NULL);

    for (int v = 0; v < 5; v++) {
        char label[64];
        snprintf(label, sizeof(label), "smooth strip: v%d is unit -z", v);
        ASSERT_FLOAT(label, repl_state_document_cmds_mut()[1 + v * 2].args[2], -1.0f);
    }
}

/* Smooth mode inherits the glFrontFace flip: GL_CW negates every
 * accumulated face before it is averaged. */
static void test_smooth_front_face_cw(void) {
    printf("test_smooth_front_face_cw\n");

    const float k = 0.70710678f;

    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glFrontFace(GL_CW);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(0, 0, 1);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(REPL_AUTONORMAL_SMOOTH, NULL);

    ASSERT_FLOAT("smooth cw: shared v0 y", repl_state_document_cmds_mut()[2].args[1], -k);
    ASSERT_FLOAT("smooth cw: shared v0 z", repl_state_document_cmds_mut()[2].args[2], -k);
    ASSERT_FLOAT("smooth cw: lone v2 z", repl_state_document_cmds_mut()[6].args[2], -1.0f);
}

/* Switching mode on an already-normalled document rewrites the existing
 * is_auto rows in place instead of inserting a second set. */
static void test_smooth_rewrites_existing_auto_rows(void) {
    printf("test_smooth_rewrites_existing_auto_rows\n");

    feed_folded_pair();
    repl_recompute_autonormals(REPL_AUTONORMAL_FACE, NULL);
    int count_after_face = repl_state_document_count();
    repl_recompute_autonormals(REPL_AUTONORMAL_SMOOTH, NULL);

    ASSERT_INT("mode switch: no rows added", repl_state_document_count(),
               count_after_face);
    ASSERT_FLOAT("mode switch: v0 now averaged",
                 repl_state_document_cmds_mut()[1].args[1], 0.70710678f);
}

int main(void) {
    test_degenerate_normal();
    test_triangle_strip();
    test_triangle_fan();
    test_quads();
    test_quad_strip();
    test_polygon();
    test_unsupported_mode();
    test_block_skipping();
    test_autonormal_disabled();
    test_gl_triangles();
    test_autonormal_inside_funcn_literal_coords();
    test_autonormal_inside_funcn_var_args_skipped();
    test_smooth_welds_shared_corners();
    test_face_mode_unchanged_by_smooth();
    test_smooth_triangle_strip_stays_unit();
    test_smooth_front_face_cw();
    test_smooth_rewrites_existing_auto_rows();

    return test_harness_report(&g_harness, "test_repl_autonormal");
}
