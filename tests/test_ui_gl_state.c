/*
 * test_ui_gl_state.c - real-GL regression: 2D-overlay state isolation.
 *
 * Creates an actual GL context via GLUT (so glPushAttrib/glPopAttrib and
 * glGetFloatv/glIsEnabled behave for real — the no-op stub harness can't
 * model the GL state machine) and verifies that the shared
 * gl2d_begin()/gl2d_end() bracket every overlay uses fully restores the
 * GL state it touches: current color, GL_DEPTH_TEST, GL_LIGHTING.
 *
 * Needs a real GL context, so this is NOT part of `make test` / CI. Run via
 * `make gl-tests` with a display or `make gl-tests FREEGLUT_OSMESA=1`
 * headlessly.
 */
#include "gl_includes.h"
#include "ui/core/gl_2d.h"
#include "support/test_harness.h"

#include <stdio.h>

#if defined(__APPLE__) && !defined(FREEGLUT_OSMESA)
#include <ApplicationServices/ApplicationServices.h>
#endif

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)

static int feq(float a, float b) {
    float d = a - b;
    return (d < 0 ? -d : d) < 1e-3f;
}

static void assert_float_state(const char *label, GLenum pname,
                               const GLfloat *expected, int count) {
    GLfloat got[16];
    char component_label[128];
    int i;
    glGetFloatv(pname, got);
    for (i = 0; i < count; i++) {
        snprintf(component_label, sizeof(component_label), "%s[%d]", label, i);
        ASSERT_TRUE(component_label, feq(got[i], expected[i]));
    }
}

static void assert_material_state(const char *label, GLenum face, GLenum pname,
                                  const GLfloat *expected, int count) {
    GLfloat got[4];
    char component_label[128];
    int i;
    glGetMaterialfv(face, pname, got);
    for (i = 0; i < count; i++) {
        snprintf(component_label, sizeof(component_label), "%s[%d]", label, i);
        ASSERT_TRUE(component_label, feq(got[i], expected[i]));
    }
}

/* Confirm the OpenGL 2.1 initial values used by gl_state_inspector.c against
 * a fresh real context. This stays in the opt-in GL suite because the stub
 * headers deliberately do not implement a state machine. */
static void test_fresh_context_defaults(void) {
    static const GLenum disabled_caps[] = {
        GL_BLEND,
        GL_CLIP_PLANE0, GL_CLIP_PLANE1, GL_CLIP_PLANE2,
        GL_CLIP_PLANE3, GL_CLIP_PLANE4, GL_CLIP_PLANE5,
        GL_COLOR_MATERIAL, GL_CULL_FACE, GL_DEPTH_TEST,
        GL_LIGHT0, GL_LIGHT1, GL_LIGHT2, GL_LIGHT3, GL_LIGHTING,
        GL_LINE_SMOOTH, GL_LINE_STIPPLE, GL_NORMALIZE, GL_POINT_SMOOTH
    };
    static const GLfloat white[4] = { 1, 1, 1, 1 };
    static const GLfloat normal[3] = { 0, 0, 1 };
    static const GLfloat identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    static const GLfloat ambient[4] = { 0.2f, 0.2f, 0.2f, 1 };
    static const GLfloat diffuse[4] = { 0.8f, 0.8f, 0.8f, 1 };
    static const GLfloat black_alpha_one[4] = { 0, 0, 0, 1 };
    static const GLfloat point_attenuation[3] = { 1, 0, 0 };
    static const GLfloat zeros[4] = { 0, 0, 0, 0 };
    static const GLfloat raster_pos[4] = { 0, 0, 0, 1 };
    GLint iv;
    GLboolean bv[4];
    GLdouble clip[4];
    char label[128];
    int i;

    assert_float_state("default current color", GL_CURRENT_COLOR, white, 4);
    assert_float_state("default current normal", GL_CURRENT_NORMAL, normal, 3);
    assert_float_state("default modelview", GL_MODELVIEW_MATRIX, identity, 16);
    assert_float_state("default point attenuation",
                       GL_POINT_DISTANCE_ATTENUATION, point_attenuation, 3);
    assert_float_state("default clear color", GL_COLOR_CLEAR_VALUE, zeros, 4);
    assert_float_state("default raster position",
                       GL_CURRENT_RASTER_POSITION, raster_pos, 4);

    for (i = 0; i < (int)(sizeof(disabled_caps) / sizeof(disabled_caps[0])); i++) {
        snprintf(label, sizeof(label), "cap 0x%X initially disabled",
                 (unsigned)disabled_caps[i]);
        ASSERT_TRUE(label, glIsEnabled(disabled_caps[i]) == GL_FALSE);
    }
    ASSERT_TRUE("GL_MULTISAMPLE initially enabled",
                glIsEnabled(GL_MULTISAMPLE) == GL_TRUE);

    glGetIntegerv(GL_SHADE_MODEL, &iv);
    ASSERT_TRUE("default shade model smooth", iv == GL_SMOOTH);
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &iv);
    ASSERT_TRUE("default modelview stack depth", iv == 1);
    glGetIntegerv(GL_COLOR_MATERIAL_FACE, &iv);
    ASSERT_TRUE("default color material face", iv == GL_FRONT_AND_BACK);
    glGetIntegerv(GL_COLOR_MATERIAL_PARAMETER, &iv);
    ASSERT_TRUE("default color material parameter", iv == GL_AMBIENT_AND_DIFFUSE);
    glGetIntegerv(GL_FRONT_FACE, &iv);
    ASSERT_TRUE("default front face", iv == GL_CCW);
    glGetIntegerv(GL_CULL_FACE_MODE, &iv);
    ASSERT_TRUE("default cull face", iv == GL_BACK);
    glGetIntegerv(GL_DEPTH_FUNC, &iv);
    ASSERT_TRUE("default depth func", iv == GL_LESS);
    glGetIntegerv(GL_BLEND_SRC, &iv);
    ASSERT_TRUE("default blend source", iv == GL_ONE);
    glGetIntegerv(GL_BLEND_DST, &iv);
    ASSERT_TRUE("default blend destination", iv == GL_ZERO);
    glGetIntegerv(GL_LINE_STIPPLE_REPEAT, &iv);
    ASSERT_TRUE("default line stipple repeat", iv == 1);
    glGetIntegerv(GL_LINE_STIPPLE_PATTERN, &iv);
    ASSERT_TRUE("default line stipple pattern", iv == 0xFFFF);

    glGetBooleanv(GL_LIGHT_MODEL_LOCAL_VIEWER, bv);
    ASSERT_TRUE("default light model local viewer", bv[0] == GL_FALSE);
    glGetBooleanv(GL_LIGHT_MODEL_TWO_SIDE, bv);
    ASSERT_TRUE("default light model two side", bv[0] == GL_FALSE);
    glGetBooleanv(GL_DEPTH_WRITEMASK, bv);
    ASSERT_TRUE("default depth write mask", bv[0] == GL_TRUE);
    glGetBooleanv(GL_COLOR_WRITEMASK, bv);
    ASSERT_TRUE("default color write mask",
                bv[0] == GL_TRUE && bv[1] == GL_TRUE &&
                bv[2] == GL_TRUE && bv[3] == GL_TRUE);
    glGetBooleanv(GL_EDGE_FLAG, bv);
    ASSERT_TRUE("default edge flag", bv[0] == GL_TRUE);

    {
        static const GLfloat one[1] = { 1 };
        assert_float_state("default point size", GL_POINT_SIZE, one, 1);
        assert_float_state("default line width", GL_LINE_WIDTH, one, 1);
    }
    assert_material_state("front ambient", GL_FRONT, GL_AMBIENT, ambient, 4);
    assert_material_state("back ambient", GL_BACK, GL_AMBIENT, ambient, 4);
    assert_material_state("front diffuse", GL_FRONT, GL_DIFFUSE, diffuse, 4);
    assert_material_state("back diffuse", GL_BACK, GL_DIFFUSE, diffuse, 4);
    assert_material_state("front specular", GL_FRONT, GL_SPECULAR,
                          black_alpha_one, 4);
    assert_material_state("back specular", GL_BACK, GL_SPECULAR,
                          black_alpha_one, 4);
    assert_material_state("front emission", GL_FRONT, GL_EMISSION,
                          black_alpha_one, 4);
    assert_material_state("back emission", GL_BACK, GL_EMISSION,
                          black_alpha_one, 4);
    assert_material_state("front shininess", GL_FRONT, GL_SHININESS, zeros, 1);
    assert_material_state("back shininess", GL_BACK, GL_SHININESS, zeros, 1);

    for (i = 0; i < 6; i++) {
        int j;
        glGetClipPlane((GLenum)(GL_CLIP_PLANE0 + i), clip);
        for (j = 0; j < 4; j++) {
            snprintf(label, sizeof(label), "default clip plane %d[%d]", i, j);
            ASSERT_TRUE(label, clip[j] == 0.0);
        }
    }
}

/* Set a known baseline, run a gl2d overlay block that deliberately
 * mutates color / depth / lighting, then confirm gl2d_end() restored
 * every bit gl2d_begin() pushes (GL_CURRENT_BIT | GL_DEPTH_BUFFER_BIT |
 * GL_LIGHTING_BIT). Guards regressions in that push mask. */
static void test_gl2d_restores_state(void) {
    GLfloat col[4];

    glColor4f(0.20f, 0.30f, 0.40f, 0.50f);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);

    gl2d_begin(64, 64);
    /* Simulate what an overlay does inside the bracket. */
    glColor4f(1.0f, 0.5f, 0.0f, 1.0f);
    glEnable(GL_LIGHTING);
    /* gl2d_begin already disabled GL_DEPTH_TEST. */
    gl2d_end();

    glGetFloatv(GL_CURRENT_COLOR, col);
    ASSERT_TRUE("gl2d_end restores current color (r)", feq(col[0], 0.20f));
    ASSERT_TRUE("gl2d_end restores current color (g)", feq(col[1], 0.30f));
    ASSERT_TRUE("gl2d_end restores current color (b)", feq(col[2], 0.40f));
    ASSERT_TRUE("gl2d_end restores current color (a)", feq(col[3], 0.50f));
    ASSERT_TRUE("gl2d_end restores GL_DEPTH_TEST on",
                glIsEnabled(GL_DEPTH_TEST) == GL_TRUE);
    ASSERT_TRUE("gl2d_end restores GL_LIGHTING off",
                glIsEnabled(GL_LIGHTING) == GL_FALSE);

    /* Inverse baseline: depth off / lighting on must also round-trip. */
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    gl2d_begin(64, 64);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    gl2d_end();
    ASSERT_TRUE("gl2d_end restores GL_DEPTH_TEST off",
                glIsEnabled(GL_DEPTH_TEST) == GL_FALSE);
    ASSERT_TRUE("gl2d_end restores GL_LIGHTING on",
                glIsEnabled(GL_LIGHTING) == GL_TRUE);
}

int main(int argc, char **argv) {
#if defined(__APPLE__) && !defined(FREEGLUT_OSMESA)
    uint32_t display_count = 0;
    if (CGGetActiveDisplayList(0, NULL, &display_count) != kCGErrorSuccess ||
        display_count == 0) {
        fprintf(stderr, "test_ui_gl_state: no active macOS display; skipping\n");
        return 0;
    }
#endif
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH);
    glutInitWindowSize(64, 64);
    if (glutCreateWindow("gl-state-test") <= 0) {
        fprintf(stderr, "test_ui_gl_state: no GL context (need a display); "
                        "skipping\n");
        return 0;  /* opt-in target: absence of a display is not a failure */
    }

    printf("--- ui_gl_state tests (real GL context) ---\n");
    test_fresh_context_defaults();
    test_gl2d_restores_state();
    return test_harness_report(&g_harness, "ui_gl_state");
}
