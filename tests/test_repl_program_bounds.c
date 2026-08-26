/*
 * test_repl_program_bounds.c - the world-space AABB walk over a flat
 * program (src/repl/program_bounds.c) and the software matrix stack it
 * rides on (repl/transform_utils.h's Mat4Stack).
 *
 * Drives the walk with hand-built GLCmd arrays - no flatten, no GL context,
 * no frame. What is pinned here is what a consumer placing geometry around
 * the user's scene actually depends on:
 *
 *   - vertices land in WORLD space, i.e. the modelview in effect at the
 *     vertex is applied, and glPushMatrix/glPopMatrix scope it;
 *   - the GLUT solids contribute their real freeglut extents, including the
 *     two that are not centred symmetric cubes (torus, cone);
 *   - geometry the executor drops (a vertex outside glBegin/glEnd) does not
 *     widen the box, because it does not widen what is on screen;
 *   - degenerate input reports `valid = 0` rather than a plausible-looking
 *     wrong box - the whole point of the flag.
 */
#include "repl/program_bounds.h"
#include "repl/transform_utils.h"
#include "support/test_harness.h"

#include <string.h>

static TestHarness g_h = TEST_HARNESS_INIT;
#define AT(label, cond) TEST_ASSERT_TRUE(&g_h, label, cond)
#define AF(label, got, expected) TEST_ASSERT_FLOAT(&g_h, label, got, expected, 1e-4f)

static GLCmd cmd(CmdType type, float a0, float a1, float a2, float a3) {
    GLCmd c;
    memset(&c, 0, sizeof c);
    c.type = type;
    c.valid = 1;
    c.args[0] = a0;
    c.args[1] = a1;
    c.args[2] = a2;
    c.args[3] = a3;
    return c;
}

static FlatProgramView view_of(const GLCmd *cmds, int n) {
    FlatProgramView v;
    memset(&v, 0, sizeof v);
    v.cmds = cmds;
    v.cmd_count = n;
    return v;
}

static void check_box(const char *what, const ReplSceneBounds *b,
                      float x0, float y0, float z0,
                      float x1, float y1, float z1) {
    char label[128];
    static const char *axis = "xyz";
    const float lo[3] = { x0, y0, z0 };
    const float hi[3] = { x1, y1, z1 };

    snprintf(label, sizeof label, "%s: bounds are valid", what);
    TEST_ASSERT_TRUE(&g_h, label, b->valid);
    if (!b->valid)
        return;
    for (int i = 0; i < 3; i++) {
        snprintf(label, sizeof label, "%s: min %c", what, axis[i]);
        AF(label, b->min[i], lo[i]);
        snprintf(label, sizeof label, "%s: max %c", what, axis[i]);
        AF(label, b->max[i], hi[i]);
    }
}

/* --- vertices ------------------------------------------------------- */

static void test_untransformed_vertices(void) {
    GLCmd cmds[5];
    ReplSceneBounds b;

    cmds[0] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[1] = cmd(CMD_VERTEX3F, -1.0f, 0.0f, 2.0f, 0);
    cmds[2] = cmd(CMD_VERTEX3F, 3.0f, -4.0f, 0.5f, 0);
    cmds[3] = cmd(CMD_VERTEX3F, 0.0f, 1.0f, -6.0f, 0);
    cmds[4] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 5), 5);
    check_box("plain vertices", &b, -1.0f, -4.0f, -6.0f, 3.0f, 1.0f, 2.0f);
}

static void test_vertex2f_pins_z_to_zero(void) {
    GLCmd cmds[4];
    ReplSceneBounds b;

    cmds[0] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[1] = cmd(CMD_VERTEX2F, 1.0f, 2.0f, 99.0f, 0);
    cmds[2] = cmd(CMD_VERTEX2F, -1.0f, -2.0f, 99.0f, 0);
    cmds[3] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 4), 4);
    check_box("glVertex2f", &b, -1.0f, -2.0f, 0.0f, 1.0f, 2.0f, 0.0f);
}

static void test_vertex4f_divides_through_by_w(void) {
    GLCmd cmds[3];
    ReplSceneBounds b;

    cmds[0] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[1] = cmd(CMD_VERTEX4F, 4.0f, 6.0f, -2.0f, 2.0f);
    cmds[2] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 3), 3);
    check_box("glVertex4f", &b, 2.0f, 3.0f, -1.0f, 2.0f, 3.0f, -1.0f);
}

static void test_vertex_outside_begin_is_ignored(void) {
    GLCmd cmds[4];
    ReplSceneBounds b;

    /* The executor drops this one, so it draws nothing and must not
     * enlarge the box either. */
    cmds[0] = cmd(CMD_VERTEX3F, 100.0f, 100.0f, 100.0f, 0);
    cmds[1] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[2] = cmd(CMD_VERTEX3F, 1.0f, 1.0f, 1.0f, 0);
    cmds[3] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 4), 4);
    check_box("stray vertex", &b, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
}

static void test_invalid_commands_are_skipped(void) {
    GLCmd cmds[4];
    ReplSceneBounds b;

    cmds[0] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[1] = cmd(CMD_VERTEX3F, 1.0f, 1.0f, 1.0f, 0);
    cmds[2] = cmd(CMD_VERTEX3F, 50.0f, 50.0f, 50.0f, 0);
    cmds[2].valid = 0;
    cmds[3] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 4), 4);
    check_box("deleted row", &b, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
}

/* --- transforms ----------------------------------------------------- */

static void test_translate_moves_the_box(void) {
    GLCmd cmds[4];
    ReplSceneBounds b;

    cmds[0] = cmd(CMD_TRANSLATE3F, 10.0f, 0.0f, -5.0f, 0);
    cmds[1] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[2] = cmd(CMD_VERTEX3F, 1.0f, 2.0f, 3.0f, 0);
    cmds[3] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 4), 4);
    check_box("translate", &b, 11.0f, 2.0f, -2.0f, 11.0f, 2.0f, -2.0f);
}

static void test_push_pop_scopes_the_transform(void) {
    GLCmd cmds[8];
    ReplSceneBounds b;

    cmds[0] = cmd(CMD_PUSH_MATRIX, 0, 0, 0, 0);
    cmds[1] = cmd(CMD_TRANSLATE3F, 5.0f, 0.0f, 0.0f, 0);
    cmds[2] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[3] = cmd(CMD_VERTEX3F, 0.0f, 0.0f, 0.0f, 0);
    cmds[4] = cmd(CMD_END, 0, 0, 0, 0);
    cmds[5] = cmd(CMD_POP_MATRIX, 0, 0, 0, 0);
    /* Outside the pop the translate is gone: this vertex is at the origin,
     * so the box spans 0..5 rather than 5..10. */
    cmds[6] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[7] = cmd(CMD_VERTEX3F, 0.0f, 0.0f, 0.0f, 0);

    b = repl_program_bounds(view_of(cmds, 8), 8);
    check_box("push/pop", &b, 0.0f, 0.0f, 0.0f, 5.0f, 0.0f, 0.0f);
}

static void test_rotate_then_translate_composes_in_gl_order(void) {
    GLCmd cmds[5];
    ReplSceneBounds b;

    /* glRotatef(90, 0,1,0) then glTranslatef(1,0,0) puts the origin-local
     * vertex at world (0, 0, -1): GL post-multiplies, so the translate
     * happens in the rotated frame, not before it. */
    cmds[0] = cmd(CMD_ROTATEF, 90.0f, 0.0f, 1.0f, 0.0f);
    cmds[1] = cmd(CMD_TRANSLATE3F, 1.0f, 0.0f, 0.0f, 0);
    cmds[2] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[3] = cmd(CMD_VERTEX3F, 0.0f, 0.0f, 0.0f, 0);
    cmds[4] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 5), 5);
    check_box("rotate*translate", &b, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f);
}

static void test_scale_scales_a_solid(void) {
    GLCmd cmds[2];
    ReplSceneBounds b;

    cmds[0] = cmd(CMD_SCALEF, 2.0f, 3.0f, 4.0f, 0);
    cmds[1] = cmd(CMD_GLUT_CUBE, 1.0f, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 2), 2);
    check_box("scaled cube", &b, -1.0f, -1.5f, -2.0f, 1.0f, 1.5f, 2.0f);
}

static void test_load_identity_resets(void) {
    GLCmd cmds[5];
    ReplSceneBounds b;

    cmds[0] = cmd(CMD_TRANSLATE3F, 100.0f, 0.0f, 0.0f, 0);
    cmds[1] = cmd(CMD_LOAD_IDENTITY, 0, 0, 0, 0);
    cmds[2] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[3] = cmd(CMD_VERTEX3F, 1.0f, 0.0f, 0.0f, 0);
    cmds[4] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 5), 5);
    check_box("load identity", &b, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
}

static void test_mult_matrix_is_applied(void) {
    GLCmd cmds[4];
    ReplSceneBounds b;

    /* A column-major translate by (0, 7, 0), carried on the command the way
     * flatten bakes it. apply_tracked_transform honours this command and so
     * must the software twin - the divergence this pins is a real one the
     * edit-overlay walk used to have. */
    memset(&cmds[0], 0, sizeof cmds[0]);
    cmds[0].type = CMD_MULT_MATRIXF;
    cmds[0].valid = 1;
    mat4_identity(cmds[0].payload.matrix.m);
    cmds[0].payload.matrix.m[13] = 7.0f;

    cmds[1] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[2] = cmd(CMD_VERTEX3F, 0.0f, 0.0f, 0.0f, 0);
    cmds[3] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 4), 4);
    check_box("glMultMatrixf", &b, 0.0f, 7.0f, 0.0f, 0.0f, 7.0f, 0.0f);
}

/* --- GLUT solids ---------------------------------------------------- */

static void test_glut_solid_extents(void) {
    GLCmd c;
    ReplSceneBounds b;

    c = cmd(CMD_GLUT_CUBE, 2.0f, 0, 0, 0);
    b = repl_program_bounds(view_of(&c, 1), 1);
    check_box("cube(2)", &b, -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f);

    c = cmd(CMD_GLUT_SPHERE, 3.0f, 8, 8, 0);
    b = repl_program_bounds(view_of(&c, 1), 1);
    check_box("sphere(3)", &b, -3.0f, -3.0f, -3.0f, 3.0f, 3.0f, 3.0f);

    /* freeglut's torus is a ring in XY about Z: the tube radius is the
     * INNER argument, so it is the one that sets the thin axis. */
    c = cmd(CMD_GLUT_TORUS, 0.25f, 2.0f, 12, 24);
    b = repl_program_bounds(view_of(&c, 1), 1);
    check_box("torus(0.25, 2)", &b,
              -2.25f, -2.25f, -0.25f, 2.25f, 2.25f, 0.25f);

    /* The cone is NOT centred - it grows along +Z from its base. */
    c = cmd(CMD_GLUT_CONE, 1.0f, 4.0f, 12, 2);
    b = repl_program_bounds(view_of(&c, 1), 1);
    check_box("cone(1, 4)", &b, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 4.0f);

    /* The teapot's hull is wider than it is tall, and wider than its
     * nominal size - a caller that assumed +-size would clip the spout. */
    c = cmd(CMD_GLUT_TEAPOT, 1.0f, 0, 0, 0);
    b = repl_program_bounds(view_of(&c, 1), 1);
    check_box("teapot(1)", &b,
              -1.7625f, -0.7875f, -1.7625f, 1.7625f, 0.7875f, 1.7625f);
}

static void test_solid_and_vertices_share_one_box(void) {
    GLCmd cmds[4];
    ReplSceneBounds b;

    cmds[0] = cmd(CMD_GLUT_SPHERE, 1.0f, 8, 8, 0);
    cmds[1] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[2] = cmd(CMD_VERTEX3F, 0.0f, 5.0f, 0.0f, 0);
    cmds[3] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 4), 4);
    check_box("solid + vertex", &b, -1.0f, -1.0f, -1.0f, 1.0f, 5.0f, 1.0f);
}

/* --- tess ----------------------------------------------------------- */

static void test_tess_vertices_count_inside_a_polygon(void) {
    GLCmd cmds[5];
    ReplSceneBounds b;

    /* A tess vertex outside a polygon buffers nothing, exactly like a
     * stray glVertex outside glBegin. */
    cmds[0] = cmd(CMD_TESS_VERTEX, 40.0f, 40.0f, 40.0f, 0);
    cmds[1] = cmd(CMD_TESS_BEGIN_POLYGON, 0, 0, 0, 0);
    cmds[2] = cmd(CMD_TESS_VERTEX, -2.0f, 0.0f, 1.0f, 0);
    cmds[3] = cmd(CMD_TESS_VERTEX, 2.0f, 1.0f, -1.0f, 0);
    cmds[4] = cmd(CMD_TESS_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 5), 5);
    check_box("tess polygon", &b, -2.0f, 0.0f, -1.0f, 2.0f, 1.0f, 1.0f);
}

/* --- degenerate ----------------------------------------------------- */

static void test_empty_and_geometryless_programs_are_invalid(void) {
    GLCmd cmds[2];
    ReplSceneBounds b;
    FlatProgramView empty;

    memset(&empty, 0, sizeof empty);
    b = repl_program_bounds(empty, 0);
    AT("no program: invalid", !b.valid);

    cmds[0] = cmd(CMD_TRANSLATE3F, 1.0f, 1.0f, 1.0f, 0);
    cmds[1] = cmd(CMD_COLOR3F, 1.0f, 0.0f, 0.0f, 0);
    b = repl_program_bounds(view_of(cmds, 2), 2);
    AT("no geometry: invalid", !b.valid);
    AT("no geometry: min is left zeroed", b.min[0] == 0.0f);
    AT("no geometry: max is left zeroed", b.max[0] == 0.0f);
}

static void test_count_is_clamped_to_the_view(void) {
    GLCmd cmds[3];
    ReplSceneBounds b;

    cmds[0] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[1] = cmd(CMD_VERTEX3F, 1.0f, 0.0f, 0.0f, 0);
    cmds[2] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 3), 9999);
    check_box("over-long count", &b, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);

    /* A short count narrows the walk - this is how a caller would ask for
     * the bounds of a replay-clamped prefix. */
    b = repl_program_bounds(view_of(cmds, 3), 1);
    AT("prefix before any vertex: invalid", !b.valid);
}

static void test_stack_overflow_reports_invalid(void) {
    static GLCmd cmds[MAT4_STACK_MAX + 4];
    ReplSceneBounds b;
    int n = 0;

    for (int i = 0; i < MAT4_STACK_MAX + 1; i++)
        cmds[n++] = cmd(CMD_PUSH_MATRIX, 0, 0, 0, 0);
    cmds[n++] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[n++] = cmd(CMD_VERTEX3F, 1.0f, 1.0f, 1.0f, 0);
    cmds[n++] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, n), n);
    AT("stack overflow: reported invalid rather than plausibly wrong",
       !b.valid);
}

/* --- derived quantities --------------------------------------------- */

static void test_center_and_radius(void) {
    GLCmd cmds[4];
    ReplSceneBounds b;
    float c[3];

    cmds[0] = cmd(CMD_BEGIN, 0, 0, 0, 0);
    cmds[1] = cmd(CMD_VERTEX3F, -1.0f, 0.0f, -1.0f, 0);
    cmds[2] = cmd(CMD_VERTEX3F, 3.0f, 4.0f, 1.0f, 0);
    cmds[3] = cmd(CMD_END, 0, 0, 0, 0);

    b = repl_program_bounds(view_of(cmds, 4), 4);
    repl_scene_bounds_center(&b, c);
    AF("center x", c[0], 1.0f);
    AF("center y", c[1], 2.0f);
    AF("center z", c[2], 0.0f);
    /* half-diagonal of a 4 x 4 x 2 box */
    AF("radius", repl_scene_bounds_radius(&b), 3.0f);

    b.valid = 0;
    repl_scene_bounds_center(&b, c);
    AF("invalid bounds center at origin", c[0], 0.0f);
    AF("invalid bounds radius is zero", repl_scene_bounds_radius(&b), 0.0f);
}

int main(void) {
    test_untransformed_vertices();
    test_vertex2f_pins_z_to_zero();
    test_vertex4f_divides_through_by_w();
    test_vertex_outside_begin_is_ignored();
    test_invalid_commands_are_skipped();
    test_translate_moves_the_box();
    test_push_pop_scopes_the_transform();
    test_rotate_then_translate_composes_in_gl_order();
    test_scale_scales_a_solid();
    test_load_identity_resets();
    test_mult_matrix_is_applied();
    test_glut_solid_extents();
    test_solid_and_vertices_share_one_box();
    test_tess_vertices_count_inside_a_polygon();
    test_empty_and_geometryless_programs_are_invalid();
    test_count_is_clamped_to_the_view();
    test_stack_overflow_reports_invalid();
    test_center_and_radius();
    return test_harness_report(&g_h, "repl_program_bounds");
}
