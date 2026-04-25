#include "repl_core.h"
#include "repl_state.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s] (line %d)\n", label, __LINE__); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    g_run++; \
    if ((got) == (exp)) g_pass++; \
    else printf("FAIL [%s] got %d, expected %d (line %d)\n", label, (int)(got), (int)(exp), __LINE__); \
} while (0)

#define ASSERT_FLOAT(label, got, exp) do { \
    g_run++; \
    if (fabsf((got) - (exp)) < 1e-5f) g_pass++; \
    else printf("FAIL [%s] got %f, expected %f (line %d)\n", label, (float)(got), (float)(exp), __LINE__); \
} while (0)


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

    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    /* All three vertices collinear along x - cross product is zero */
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(2, 0, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();

    /* An auto-normal should be inserted before each vertex */
    ASSERT_INT("degenerate: cmd count", repl_state_document_count(), 8);
    ASSERT_TRUE("degenerate: first inserted is auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    ASSERT_FLOAT("degenerate: normal x is 0", repl_state_document_cmds_mut()[1].args[0], 0.0f);
    ASSERT_FLOAT("degenerate: normal y is 0", repl_state_document_cmds_mut()[1].args[1], 0.0f);
    ASSERT_FLOAT("degenerate: normal z is 0", repl_state_document_cmds_mut()[1].args[2], 0.0f);

    *repl_state_presentation_mut()->autonormal = 0;
}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_TRIANGLE_STRIP                            */
/* ------------------------------------------------------------------ */

static void test_triangle_strip(void) {
    printf("test_triangle_strip\n");

    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    repl_feed_line_public("glBegin(GL_TRIANGLE_STRIP);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glVertex3f(1, 1, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();

    /* 4 vertices + 4 auto normals + BEGIN + END */
    ASSERT_INT("strip: cmd count", repl_state_document_count(), 10);
    ASSERT_TRUE("strip: v0 has auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    ASSERT_TRUE("strip: v1 has auto normal", repl_state_document_cmds_mut()[3].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[3].is_auto);
    /* All normals for a flat strip in xy-plane should point along +z */
    ASSERT_FLOAT("strip: n0 z", repl_state_document_cmds_mut()[1].args[2], 1.0f);
    ASSERT_FLOAT("strip: n1 z", repl_state_document_cmds_mut()[3].args[2], 1.0f);

    *repl_state_presentation_mut()->autonormal = 0;
}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_TRIANGLE_FAN                              */
/* ------------------------------------------------------------------ */

static void test_triangle_fan(void) {
    printf("test_triangle_fan\n");

    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    repl_feed_line_public("glBegin(GL_TRIANGLE_FAN);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glVertex3f(-1, 0, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();

    /* 4 vertices + 4 auto normals + BEGIN + END */
    ASSERT_INT("fan: cmd count", repl_state_document_count(), 10);
    ASSERT_TRUE("fan: v0 has auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    /* Fan in xy-plane, normal should be +z */
    ASSERT_FLOAT("fan: n0 z", repl_state_document_cmds_mut()[1].args[2], 1.0f);

    *repl_state_presentation_mut()->autonormal = 0;
}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_QUADS                                     */
/* ------------------------------------------------------------------ */

static void test_quads(void) {
    printf("test_quads\n");

    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    repl_feed_line_public("glBegin(GL_QUADS);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 1, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();

    /* 4 vertices + 4 auto normals + BEGIN + END */
    ASSERT_INT("quads: cmd count", repl_state_document_count(), 10);
    ASSERT_TRUE("quads: v0 has auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    /* Quad in xy-plane: cross (1,0,0)x(1,1,0) = (0,0,1) */
    ASSERT_FLOAT("quads: n0 z", repl_state_document_cmds_mut()[1].args[2], 1.0f);

    *repl_state_presentation_mut()->autonormal = 0;
}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_QUAD_STRIP                                */
/* ------------------------------------------------------------------ */

static void test_quad_strip(void) {
    printf("test_quad_strip\n");

    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    repl_feed_line_public("glBegin(GL_QUAD_STRIP);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glVertex3f(1, 1, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();

    /* 4 vertices + 4 auto normals + BEGIN + END */
    ASSERT_INT("quad_strip: cmd count", repl_state_document_count(), 10);
    ASSERT_TRUE("quad_strip: v0 has auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    /* First quad: v0=(0,0,0) v1=(1,0,0) v2=(0,1,0). Normal = (0,0,1) */
    ASSERT_FLOAT("quad_strip: n0 z", repl_state_document_cmds_mut()[1].args[2], 1.0f);

    *repl_state_presentation_mut()->autonormal = 0;
}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_POLYGON                                   */
/* ------------------------------------------------------------------ */

static void test_polygon(void) {
    printf("test_polygon\n");

    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    repl_feed_line_public("glBegin(GL_POLYGON);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 1, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();

    /* 4 vertices + 4 auto normals + BEGIN + END */
    ASSERT_INT("polygon: cmd count", repl_state_document_count(), 10);
    ASSERT_TRUE("polygon: v0 has auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    ASSERT_FLOAT("polygon: n0 z", repl_state_document_cmds_mut()[1].args[2], 1.0f);

    *repl_state_presentation_mut()->autonormal = 0;
}

/* ------------------------------------------------------------------ */
/* compute_block_normals: unsupported mode (default branch → no-op)   */
/* ------------------------------------------------------------------ */

static void test_unsupported_mode(void) {
    printf("test_unsupported_mode\n");

    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    /* GL_LINES is valid for glBegin but not handled in compute_block_normals */
    repl_feed_line_public("glBegin(GL_LINES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();

    /* compute_block_normals default branch leaves all norms at zero,
     * but recompute_autonormals still inserts (0,0,0) auto-normals */
    ASSERT_INT("unsupported mode: auto normals still inserted", repl_state_document_count(), 6);
    ASSERT_TRUE("unsupported mode: inserted cmd is auto normal", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F && repl_state_document_cmds_mut()[1].is_auto);
    ASSERT_FLOAT("unsupported mode: normal is zero x", repl_state_document_cmds_mut()[1].args[0], 0.0f);
    ASSERT_FLOAT("unsupported mode: normal is zero z", repl_state_document_cmds_mut()[1].args[2], 0.0f);

    *repl_state_presentation_mut()->autonormal = 0;
}

/* ------------------------------------------------------------------ */
/* recompute_autonormals: skips for/func/if blocks                     */
/* ------------------------------------------------------------------ */

static void test_block_skipping(void) {
    printf("test_block_skipping\n");

    /* for-loop block before glBegin */
    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    repl_feed_line_public("for(n, 0, 2) {");
    repl_feed_line_public("glVertex3f(n, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glEnd();");
    int cmds_before = repl_state_document_count();
    repl_recompute_autonormals();

    /* 3 normals inserted into the glBegin block; for-loop was skipped */
    ASSERT_INT("for-skip: cmds added", repl_state_document_count(), cmds_before + 3);
    *repl_state_presentation_mut()->autonormal = 0;

    /* func-def block before glBegin */
    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    repl_feed_line_public("func0(n) {");
    repl_feed_line_public("glVertex3f(n, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glEnd();");
    cmds_before = repl_state_document_count();
    repl_recompute_autonormals();

    ASSERT_INT("func-skip: cmds added", repl_state_document_count(), cmds_before + 3);
    *repl_state_presentation_mut()->autonormal = 0;

    /* if-block before glBegin */
    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    repl_feed_line_public("if(n > 0) {");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glEnd();");
    cmds_before = repl_state_document_count();
    repl_recompute_autonormals();

    ASSERT_INT("if-skip: cmds added", repl_state_document_count(), cmds_before + 3);
    *repl_state_presentation_mut()->autonormal = 0;
}

/* ------------------------------------------------------------------ */
/* recompute_autonormals: *repl_state_presentation_mut()->autonormal=0 is a no-op                   */
/* ------------------------------------------------------------------ */

static void test_autonormal_disabled(void) {
    printf("test_autonormal_disabled\n");

    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 0;
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();
    ASSERT_INT("disabled: no cmds added", repl_state_document_count(), 5);
}

/* ------------------------------------------------------------------ */
/* compute_block_normals: GL_TRIANGLES + front-face winding            */
/* ------------------------------------------------------------------ */

static void test_gl_triangles(void) {
    printf("test_gl_triangles\n");

    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();
    ASSERT_TRUE("autonormal inserts before each triangle vertex", repl_state_document_count() == 8);
    ASSERT_TRUE("autonormal default front-face first cmd type", repl_state_document_cmds_mut()[1].type == CMD_NORMAL3F);
    ASSERT_TRUE("autonormal default front-face first cmd auto", repl_state_document_cmds_mut()[1].is_auto == 1);
    ASSERT_TRUE("autonormal default front-face keeps +z", fabsf(repl_state_document_cmds_mut()[1].args[2] - 1.0f) < 1e-6f);

    repl_reset_state(); declare_test_vars();
    *repl_state_presentation_mut()->autonormal = 1;
    repl_feed_line_public("glFrontFace(GL_CW);");
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();
    ASSERT_TRUE("autonormal front-face cw inserts before each triangle vertex", repl_state_document_count() == 9);
    ASSERT_TRUE("autonormal front-face cw first cmd type", repl_state_document_cmds_mut()[2].type == CMD_NORMAL3F);
    ASSERT_TRUE("autonormal front-face cw first cmd auto", repl_state_document_cmds_mut()[2].is_auto == 1);
    ASSERT_TRUE("autonormal front-face cw flips z", fabsf(repl_state_document_cmds_mut()[2].args[2] - (-1.0f)) < 1e-6f);

    repl_state_document_cmds_mut()[0].mode = GL_CCW;
    repl_recompute_autonormals();
    ASSERT_TRUE("autonormal front-face update flips auto normal back", fabsf(repl_state_document_cmds_mut()[2].args[2] - 1.0f) < 1e-6f);

    *repl_state_presentation_mut()->autonormal = 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

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

    printf("test_repl_autonormal: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
