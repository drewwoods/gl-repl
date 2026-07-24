/*
 * test_attrib_bits_gl.c - real-GL oracle for the attrib_bits.c cell->bit table.
 *
 * The mapping in src/repl/attrib_bits.c (repl_attrib_bits_for_type / cell_cover)
 * is a *claim about real OpenGL semantics*: "clear colour rides
 * GL_COLOR_BUFFER_BIT", "fog parameters ride GL_FOG_BIT", "the GL_FOG enable
 * flag rides GL_FOG_BIT *and* GL_ENABLE_BIT", "depth func rides
 * GL_DEPTH_BUFFER_BIT", and so on. The executor, the state inspector, and the
 * overlay walkers all gate their save/restore on that table, so if it does not
 * match the driver the whole glPushAttrib/glPopAttrib feature scopes the wrong
 * state.
 *
 * This test uses a live GL context (GLUT, same setup as test_ui_gl_state.c) as
 * the oracle. For each supported bit and a representative state cell the table
 * maps to it, it runs two passes driving the same GL calls the executor emits:
 *
 *   positive:  set(V1) -> glPushAttrib(covering_bit) -> set(V2)
 *              -> glPopAttrib() -> glGet == V1   (the bit *covers* the cell)
 *   negative:  set(V1) -> glPushAttrib(other_bit)    -> set(V2)
 *              -> glPopAttrib() -> glGet == V2   (a bit the table says does
 *              *not* cover the cell also does not in the driver -> the mapping
 *              is not over-broad)
 *
 * It queries GL state only (no pixels), so it runs headless under OSMesa. Needs
 * a real GL context, so it is NOT part of `make test` / CI; run via `make
 * gl-tests` with a display or `make gl-tests FREEGLUT_OSMESA=1` headlessly.
 */
#include "gl_includes.h"
#include "repl/attrib_bits.h"
#include "repl/command.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <string.h>

#if defined(__APPLE__) && !defined(FREEGLUT_OSMESA)
#include <ApplicationServices/ApplicationServices.h>
#endif

/* attrib_bits.c pulls in the source-document collectors, which reference these
 * two read-only accessors. The oracle never calls the collectors, so satisfy
 * the linker without dragging in the whole REPL state machine. */
const GLCmd *repl_state_document_cmds(void) { return NULL; }
int          repl_state_document_count(void) { return 0; }

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)

static int feq(float a, float b) {
    float d = a - b;
    return (d < 0 ? -d : d) < 1e-3f;
}

/* --- table-membership assertions ------------------------------------------ */

/* Assert the table maps a setter command to exactly `want` (the single source
 * every consumer gates its restore on). */
static void expect_membership(const char *label, CmdType type, unsigned arg0,
                              unsigned want) {
    ASSERT_TRUE(label, repl_attrib_bits_for_type(type, arg0) == want);
}

/* --- read-back assertions -------------------------------------------------- */

static void expect_floatv(const char *label, GLenum pname,
                          const GLfloat *want, int n) {
    GLfloat got[16];
    char c[128];
    int i;
    glGetFloatv(pname, got);
    for (i = 0; i < n; i++) {
        snprintf(c, sizeof(c), "%s[%d]", label, i);
        ASSERT_TRUE(c, feq(got[i], want[i]));
    }
}

static void expect_intv(const char *label, GLenum pname, GLint want) {
    GLint got = 0;
    glGetIntegerv(pname, &got);
    ASSERT_TRUE(label, got == want);
}

static void expect_material(const char *label, GLenum face, GLenum pname,
                            const GLfloat *want, int n) {
    GLfloat got[4];
    char c[128];
    int i;
    glGetMaterialfv(face, pname, got);
    for (i = 0; i < n; i++) {
        snprintf(c, sizeof(c), "%s[%d]", label, i);
        ASSERT_TRUE(c, feq(got[i], want[i]));
    }
}

static void expect_enabled(const char *label, GLenum cap, GLboolean want) {
    ASSERT_TRUE(label, glIsEnabled(cap) == want);
}

static void expect_clip_plane(const char *label, GLenum plane,
                              const GLdouble *want) {
    GLdouble got[4];
    char c[128];
    int i;
    glGetClipPlane(plane, got);
    for (i = 0; i < 4; i++) {
        double d = got[i] - want[i];
        snprintf(c, sizeof(c), "%s[%d]", label, i);
        ASSERT_TRUE(c, (d < 0 ? -d : d) < 1e-6);
    }
}

/* --- per-bit oracle passes ------------------------------------------------- */

static void test_current_bit(void) {
    static const GLfloat v1[4] = { 0.10f, 0.20f, 0.30f, 0.40f };
    static const GLfloat v2[4] = { 0.90f, 0.80f, 0.70f, 0.60f };

    expect_membership("CURRENT: glColor -> CURRENT_BIT", CMD_COLOR4F, 0,
                      GL_CURRENT_BIT);

    /* positive: GL_CURRENT_BIT restores the colour */
    glColor4f(v1[0], v1[1], v1[2], v1[3]);
    glPushAttrib(GL_CURRENT_BIT);
    glColor4f(v2[0], v2[1], v2[2], v2[3]);
    glPopAttrib();
    expect_floatv("CURRENT_BIT restores current colour", GL_CURRENT_COLOR, v1, 4);

    /* negative: GL_POINT_BIT must NOT restore the colour */
    ASSERT_TRUE("CURRENT: POINT_BIT not over-broad",
                (repl_attrib_bits_for_type(CMD_COLOR4F, 0) & GL_POINT_BIT) == 0);
    glColor4f(v1[0], v1[1], v1[2], v1[3]);
    glPushAttrib(GL_POINT_BIT);
    glColor4f(v2[0], v2[1], v2[2], v2[3]);
    glPopAttrib();
    expect_floatv("POINT_BIT leaves current colour", GL_CURRENT_COLOR, v2, 4);
}

static void test_point_bit(void) {
    static const GLfloat v1[1] = { 3.0f };
    static const GLfloat v2[1] = { 7.0f };

    expect_membership("POINT: glPointSize -> POINT_BIT", CMD_POINT_SIZE, 0,
                      GL_POINT_BIT);

    glPointSize(v1[0]);
    glPushAttrib(GL_POINT_BIT);
    glPointSize(v2[0]);
    glPopAttrib();
    expect_floatv("POINT_BIT restores point size", GL_POINT_SIZE, v1, 1);

    glPointSize(v1[0]);
    glPushAttrib(GL_LINE_BIT);
    glPointSize(v2[0]);
    glPopAttrib();
    expect_floatv("LINE_BIT leaves point size", GL_POINT_SIZE, v2, 1);
}

static void test_line_bit(void) {
    static const GLfloat v1[1] = { 2.0f };
    static const GLfloat v2[1] = { 5.0f };

    expect_membership("LINE: glLineWidth -> LINE_BIT", CMD_LINE_WIDTH, 0,
                      GL_LINE_BIT);

    glLineWidth(v1[0]);
    glPushAttrib(GL_LINE_BIT);
    glLineWidth(v2[0]);
    glPopAttrib();
    expect_floatv("LINE_BIT restores line width", GL_LINE_WIDTH, v1, 1);

    glLineWidth(v1[0]);
    glPushAttrib(GL_POINT_BIT);
    glLineWidth(v2[0]);
    glPopAttrib();
    expect_floatv("POINT_BIT leaves line width", GL_LINE_WIDTH, v2, 1);
}

static void test_polygon_bit(void) {
    expect_membership("POLYGON: glCullFace -> POLYGON_BIT", CMD_CULL_FACE, 0,
                      GL_POLYGON_BIT);

    glCullFace(GL_BACK);
    glPushAttrib(GL_POLYGON_BIT);
    glCullFace(GL_FRONT);
    glPopAttrib();
    expect_intv("POLYGON_BIT restores cull face", GL_CULL_FACE_MODE, GL_BACK);

    glCullFace(GL_BACK);
    glPushAttrib(GL_LINE_BIT);
    glCullFace(GL_FRONT);
    glPopAttrib();
    expect_intv("LINE_BIT leaves cull face", GL_CULL_FACE_MODE, GL_FRONT);
}

static void test_lighting_bit(void) {
    static const GLfloat v1[4] = { 0.10f, 0.20f, 0.30f, 1.0f };
    static const GLfloat v2[4] = { 0.90f, 0.80f, 0.70f, 1.0f };

    expect_membership("LIGHTING: glMaterialfv -> LIGHTING_BIT", CMD_MATERIALFV, 0,
                      GL_LIGHTING_BIT);

    glMaterialfv(GL_FRONT, GL_DIFFUSE, v1);
    glPushAttrib(GL_LIGHTING_BIT);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, v2);
    glPopAttrib();
    expect_material("LIGHTING_BIT restores material diffuse",
                    GL_FRONT, GL_DIFFUSE, v1, 4);

    glMaterialfv(GL_FRONT, GL_DIFFUSE, v1);
    glPushAttrib(GL_POINT_BIT);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, v2);
    glPopAttrib();
    expect_material("POINT_BIT leaves material diffuse",
                    GL_FRONT, GL_DIFFUSE, v2, 4);
}

static void test_fog_bit(void) {
    static const GLfloat density1[1] = { 0.25f };
    static const GLfloat density2[1] = { 0.75f };
    static const GLfloat color1[4] = { 0.10f, 0.20f, 0.30f, 1.0f };
    static const GLfloat color2[4] = { 0.90f, 0.80f, 0.70f, 1.0f };

    /* Fog parameters (density / mode / colour) all ride GL_FOG_BIT. */
    expect_membership("FOG: glFogf(density) -> FOG_BIT", CMD_FOG_F,
                      GL_FOG_DENSITY, GL_FOG_BIT);
    expect_membership("FOG: glFogi(mode) -> FOG_BIT", CMD_FOG_I,
                      GL_FOG_MODE, GL_FOG_BIT);
    expect_membership("FOG: glFogfv(colour) -> FOG_BIT", CMD_FOG_FV,
                      GL_FOG_COLOR, GL_FOG_BIT);

    /* density: positive under FOG_BIT, negative under LIGHTING_BIT */
    glFogf(GL_FOG_DENSITY, density1[0]);
    glPushAttrib(GL_FOG_BIT);
    glFogf(GL_FOG_DENSITY, density2[0]);
    glPopAttrib();
    expect_floatv("FOG_BIT restores fog density", GL_FOG_DENSITY, density1, 1);

    ASSERT_TRUE("FOG: LIGHTING_BIT not over-broad for density",
                (repl_attrib_bits_for_type(CMD_FOG_F, GL_FOG_DENSITY) &
                 GL_LIGHTING_BIT) == 0);
    glFogf(GL_FOG_DENSITY, density1[0]);
    glPushAttrib(GL_LIGHTING_BIT);
    glFogf(GL_FOG_DENSITY, density2[0]);
    glPopAttrib();
    expect_floatv("LIGHTING_BIT leaves fog density", GL_FOG_DENSITY, density2, 1);

    /* mode: GL_FOG_BIT restores it */
    glFogi(GL_FOG_MODE, GL_EXP);
    glPushAttrib(GL_FOG_BIT);
    glFogi(GL_FOG_MODE, GL_EXP2);
    glPopAttrib();
    expect_intv("FOG_BIT restores fog mode", GL_FOG_MODE, GL_EXP);

    /* colour: GL_FOG_BIT restores it */
    glFogfv(GL_FOG_COLOR, color1);
    glPushAttrib(GL_FOG_BIT);
    glFogfv(GL_FOG_COLOR, color2);
    glPopAttrib();
    expect_floatv("FOG_BIT restores fog colour", GL_FOG_COLOR, color1, 4);
}

static void test_depth_buffer_bit(void) {
    expect_membership("DEPTH: glDepthFunc -> DEPTH_BUFFER_BIT", CMD_DEPTH_FUNC, 0,
                      GL_DEPTH_BUFFER_BIT);

    glDepthFunc(GL_LESS);
    glPushAttrib(GL_DEPTH_BUFFER_BIT);
    glDepthFunc(GL_GREATER);
    glPopAttrib();
    expect_intv("DEPTH_BUFFER_BIT restores depth func", GL_DEPTH_FUNC, GL_LESS);

    glDepthFunc(GL_LESS);
    glPushAttrib(GL_POINT_BIT);
    glDepthFunc(GL_GREATER);
    glPopAttrib();
    expect_intv("POINT_BIT leaves depth func", GL_DEPTH_FUNC, GL_GREATER);

    /* Depth clear value: the counterpart to glClearColor on COLOR_BUFFER_BIT. */
    {
        static const GLfloat d1[1] = { 0.25f };
        static const GLfloat d2[1] = { 0.75f };

        expect_membership("DEPTH: glClearDepth -> DEPTH_BUFFER_BIT",
                          CMD_CLEAR_DEPTH, 0, GL_DEPTH_BUFFER_BIT);

        glClearDepth((GLclampd)d1[0]);
        glPushAttrib(GL_DEPTH_BUFFER_BIT);
        glClearDepth((GLclampd)d2[0]);
        glPopAttrib();
        expect_floatv("DEPTH_BUFFER_BIT restores depth clear value",
                      GL_DEPTH_CLEAR_VALUE, d1, 1);

        glClearDepth((GLclampd)d1[0]);
        glPushAttrib(GL_COLOR_BUFFER_BIT);
        glClearDepth((GLclampd)d2[0]);
        glPopAttrib();
        expect_floatv("COLOR_BUFFER_BIT leaves depth clear value",
                      GL_DEPTH_CLEAR_VALUE, d2, 1);
    }
}

static void test_transform_bit(void) {
    static const GLdouble v1[4] = { 1.0, 0.0, 0.0, 0.5 };
    static const GLdouble v2[4] = { 0.0, 1.0, 0.0, -0.5 };

    /* Clip-plane coefficients are stored in eye space (modelview at call time);
     * the default identity modelview makes the stored value equal the input. */
    expect_membership("TRANSFORM: glClipPlane -> TRANSFORM_BIT", CMD_CLIP_PLANE,
                      GL_CLIP_PLANE0, GL_TRANSFORM_BIT);

    glClipPlane(GL_CLIP_PLANE0, v1);
    glPushAttrib(GL_TRANSFORM_BIT);
    glClipPlane(GL_CLIP_PLANE0, v2);
    glPopAttrib();
    expect_clip_plane("TRANSFORM_BIT restores clip plane", GL_CLIP_PLANE0, v1);

    glClipPlane(GL_CLIP_PLANE0, v1);
    glPushAttrib(GL_POINT_BIT);
    glClipPlane(GL_CLIP_PLANE0, v2);
    glPopAttrib();
    expect_clip_plane("POINT_BIT leaves clip plane", GL_CLIP_PLANE0, v2);
}

static void test_color_buffer_bit(void) {
    static const GLfloat v1[4] = { 0.10f, 0.20f, 0.30f, 0.40f };
    static const GLfloat v2[4] = { 0.50f, 0.60f, 0.70f, 0.80f };

    expect_membership("COLOR_BUFFER: glClearColor -> COLOR_BUFFER_BIT",
                      CMD_CLEAR_COLOR, 0, GL_COLOR_BUFFER_BIT);

    glClearColor(v1[0], v1[1], v1[2], v1[3]);
    glPushAttrib(GL_COLOR_BUFFER_BIT);
    glClearColor(v2[0], v2[1], v2[2], v2[3]);
    glPopAttrib();
    expect_floatv("COLOR_BUFFER_BIT restores clear colour",
                  GL_COLOR_CLEAR_VALUE, v1, 4);

    glClearColor(v1[0], v1[1], v1[2], v1[3]);
    glPushAttrib(GL_POINT_BIT);
    glClearColor(v2[0], v2[1], v2[2], v2[3]);
    glPopAttrib();
    expect_floatv("POINT_BIT leaves clear colour", GL_COLOR_CLEAR_VALUE, v2, 4);
}

/* GL_ENABLE_BIT covers every enable flag; a capability's enable also rides its
 * own group bit (glEnable(GL_DEPTH_TEST) under ENABLE|DEPTH_BUFFER). */
static void test_enable_bit(void) {
    expect_membership("ENABLE: glEnable(DEPTH_TEST) -> ENABLE|DEPTH_BUFFER",
                      CMD_ENABLE, GL_DEPTH_TEST,
                      (unsigned)(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT));

    /* positive via GL_ENABLE_BIT */
    glDisable(GL_DEPTH_TEST);
    glPushAttrib(GL_ENABLE_BIT);
    glEnable(GL_DEPTH_TEST);
    glPopAttrib();
    expect_enabled("ENABLE_BIT restores depth-test enable",
                   GL_DEPTH_TEST, GL_FALSE);

    /* positive via the group bit (GL_DEPTH_BUFFER_BIT saves the enable too) */
    glDisable(GL_DEPTH_TEST);
    glPushAttrib(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glPopAttrib();
    expect_enabled("DEPTH_BUFFER_BIT restores depth-test enable",
                   GL_DEPTH_TEST, GL_FALSE);

    /* negative: an unrelated bit does not */
    ASSERT_TRUE("ENABLE: LINE_BIT not over-broad for depth-test",
                (repl_attrib_bits_for_type(CMD_ENABLE, GL_DEPTH_TEST) &
                 GL_LINE_BIT) == 0);
    glDisable(GL_DEPTH_TEST);
    glPushAttrib(GL_LINE_BIT);
    glEnable(GL_DEPTH_TEST);
    glPopAttrib();
    expect_enabled("LINE_BIT leaves depth-test enable", GL_DEPTH_TEST, GL_TRUE);
    glDisable(GL_DEPTH_TEST);
}

/* The GL_FOG enable flag has dual membership: GL_FOG_BIT *and* GL_ENABLE_BIT,
 * matching real GL. This is the behaviour the fog attrib support rests on. */
static void test_fog_enable_dual_membership(void) {
    expect_membership("FOG enable: glEnable(GL_FOG) -> ENABLE|FOG",
                      CMD_ENABLE, GL_FOG,
                      (unsigned)(GL_ENABLE_BIT | GL_FOG_BIT));

    /* positive via GL_FOG_BIT */
    glDisable(GL_FOG);
    glPushAttrib(GL_FOG_BIT);
    glEnable(GL_FOG);
    glPopAttrib();
    expect_enabled("FOG_BIT restores fog enable", GL_FOG, GL_FALSE);

    /* positive via GL_ENABLE_BIT */
    glDisable(GL_FOG);
    glPushAttrib(GL_ENABLE_BIT);
    glEnable(GL_FOG);
    glPopAttrib();
    expect_enabled("ENABLE_BIT restores fog enable", GL_FOG, GL_FALSE);

    /* negative: an unrelated bit does not */
    ASSERT_TRUE("FOG enable: LINE_BIT not over-broad",
                (repl_attrib_bits_for_type(CMD_ENABLE, GL_FOG) &
                 GL_LINE_BIT) == 0);
    glDisable(GL_FOG);
    glPushAttrib(GL_LINE_BIT);
    glEnable(GL_FOG);
    glPopAttrib();
    expect_enabled("LINE_BIT leaves fog enable", GL_FOG, GL_TRUE);
    glDisable(GL_FOG);
}

int main(int argc, char **argv) {
#if defined(__APPLE__) && !defined(FREEGLUT_OSMESA)
    uint32_t display_count = 0;
    if (CGGetActiveDisplayList(0, NULL, &display_count) != kCGErrorSuccess ||
        display_count == 0) {
        fprintf(stderr, "test_attrib_bits_gl: no active macOS display; skipping\n");
        return 0;
    }
#endif
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH);
    glutInitWindowSize(64, 64);
    if (glutCreateWindow("attrib-bits-oracle") <= 0) {
        fprintf(stderr, "test_attrib_bits_gl: no GL context (need a display); "
                        "skipping\n");
        return 0;  /* opt-in target: absence of a display is not a failure */
    }

    printf("--- attrib_bits real-GL oracle (table vs driver) ---\n");
    test_current_bit();
    test_point_bit();
    test_line_bit();
    test_polygon_bit();
    test_lighting_bit();
    test_fog_bit();
    test_depth_buffer_bit();
    test_transform_bit();
    test_color_buffer_bit();
    test_enable_bit();
    test_fog_enable_dual_membership();
    return test_harness_report(&g_harness, "attrib_bits_gl");
}
