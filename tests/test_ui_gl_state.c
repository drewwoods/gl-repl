/*
 * test_ui_gl_state.c - real-GL regression: 2D-overlay state isolation.
 *
 * Creates an actual GL context via GLUT (so glPushAttrib/glPopAttrib and
 * glGetFloatv/glIsEnabled behave for real — the no-op stub harness can't
 * model the GL state machine) and verifies that the shared
 * gl2d_begin()/gl2d_end() bracket every overlay uses fully restores the
 * GL state it touches: current color, GL_DEPTH_TEST, GL_LIGHTING.
 *
 * Needs a display, so this is NOT part of `make test` / CI — run via
 * `make gl-tests` locally.
 */
#include "gl_includes.h"
#include "ui/core/gl_2d.h"
#include "support/test_harness.h"

#include <stdio.h>

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)

static int feq(float a, float b) {
    float d = a - b;
    return (d < 0 ? -d : d) < 1e-3f;
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
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH);
    glutInitWindowSize(64, 64);
    if (glutCreateWindow("gl-state-test") <= 0) {
        fprintf(stderr, "test_ui_gl_state: no GL context (need a display); "
                        "skipping\n");
        return 0;  /* opt-in target: absence of a display is not a failure */
    }

    printf("--- ui_gl_state tests (real GL context) ---\n");
    test_gl2d_restores_state();
    return test_harness_report(&g_harness, "ui_gl_state");
}
