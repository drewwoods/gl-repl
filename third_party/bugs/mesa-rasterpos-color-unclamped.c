/*
 * Mesa bug repro: GL_CURRENT_RASTER_COLOR is latched unclamped.
 *
 * glRasterPos with an out-of-range current color stores the raw value instead
 * of clamping it to [0,1]. The spec clamps "after lighting (whether enabled or
 * not)", so this applies to the unlit path too - which is where it is easiest
 * to see, since no lighting equation is involved to argue about.
 *
 * Build (Linux):
 *     cc -std=c99 -O0 -o mesa-rasterpos-color-unclamped \
 *        mesa-rasterpos-color-unclamped.c -lglut -lGL -lGLU -lm
 *     ./mesa-rasterpos-color-unclamped
 *
 * macOS (cross-check; Apple GL clamps):
 *     cc -std=c99 -O0 -Wno-deprecated-declarations \
 *        -o mesa-rasterpos-color-unclamped mesa-rasterpos-color-unclamped.c \
 *        -framework OpenGL -framework GLUT
 *
 * Exit status: 0 = conformant, 1 = bug reproduced, 77 = could not set up GL.
 *
 * The whole bug is three calls:
 *
 *     glDisable(GL_LIGHTING);
 *     glColor4f(1.5, -0.5, 0.25, 1.0);
 *     glRasterPos3f(0, 0, 0);
 *
 *     glGetFloatv(GL_CURRENT_RASTER_COLOR, c);
 *     // expected: 1.00 0.00 0.25   (clamped)
 *     // Mesa:     1.50 -0.50 0.25  (raw)
 *
 * Spec: OpenGL 2.1 (July 30, 2006), sec 2.14.6 "Clamping or Masking", p. 70.
 * https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf
 * (Wording carried through to the 4.6 compatibility profile, where
 * CLAMP_VERTEX_COLOR - the state that can switch this off - defaults to TRUE.)
 *
 *   "After lighting (whether enabled or not), all components of both primary
 *    and secondary colors are clamped to the range [0, 1]."
 *
 * The raster color is the color the raster position derives as a vertex would
 * (sec 2.13, p. 54: "The coordinates are treated as if they were specified in
 * a Vertex command [...] used to generate primary and secondary colors [...]
 * just as is done for a vertex"), so it is a primary color and the clamp above
 * applies to it. The parenthetical "whether enabled or not" is what makes the
 * unlit case below normative rather than an inference.
 *
 * GL_CURRENT_COLOR is deliberately checked too and must stay RAW: it is the
 * current color, not a color produced for a primitive, and nothing has
 * consumed it yet. Both drivers agree there. That contrast is the point - the
 * two cells are specified differently, so a driver storing the same value in
 * both is not "consistent", it is missing the clamp.
 *
 * Observed on Mesa 25.2.8 (iris, Intel ADL-N). Apple GL (2.1 Metal - 90.5) and
 * NVIDIA 595.84 both clamp.
 */
#ifdef __APPLE__
#include <GLUT/glut.h>
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#include <GL/glut.h>
#endif

#include <stdio.h>
#include <stdlib.h>

/* One component over 1, one under 0, one in range: a driver that clamps only
 * the high side, or only the low side, is still distinguishable. */
#define RAW_R  1.5f
#define RAW_G (-0.5f)
#define RAW_B  0.25f

#define SPEC_CITE \
    "OpenGL 2.1 (2006-07-30), sec 2.14.6 \"Clamping or Masking\", p.70"
#define SPEC_URL \
    "https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf"

static int g_failures;

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static void check(const char *label, const GLfloat *got, const GLfloat *want) {
    int i, ok = 1;
    for (i = 0; i < 3; i++) {
        float d = got[i] - want[i];
        if (d < -0.005f || d > 0.005f)
            ok = 0;
    }
    if (!ok)
        g_failures++;
    printf("  [%s] %-36s got %6.2f %6.2f %6.2f   want %6.2f %6.2f %6.2f\n",
           ok ? "PASS" : "FAIL", label, got[0], got[1], got[2],
           want[0], want[1], want[2]);
}

static void latch(GLfloat *out) {
    GLboolean valid = GL_FALSE;

    glRasterPos3f(0.0f, 0.0f, 0.0f);
    glGetBooleanv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
    if (!valid) {
        fprintf(stderr, "raster position clipped - result would be undefined\n");
        exit(77);
    }
    glGetFloatv(GL_CURRENT_RASTER_COLOR, out);
}

int main(int argc, char **argv) {
    GLfloat raw[4]     = { RAW_R, RAW_G, RAW_B, 1.0f };
    GLfloat clamped[4];
    GLfloat got[4], cur[4];
    GLfloat emission[4] = { 1.75f, 0.5f, -0.25f, 1.0f };
    int i;

    for (i = 0; i < 4; i++)
        clamped[i] = clamp01(raw[i]);

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DEPTH | GLUT_SINGLE);
    glutInitWindowSize(64, 64);
    if (glutCreateWindow("rasterpos clamp probe") <= 0) {
        fprintf(stderr, "cannot create a GL window\n");
        return 77;
    }

    printf("GL_VENDOR  : %s\n", (const char *)glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL_VERSION : %s\n\n", (const char *)glGetString(GL_VERSION));
    printf("Spec       : %s\n", SPEC_CITE);
    printf("             %s\n\n", SPEC_URL);

    printf("Case A: lighting disabled, out-of-range color   [the bug]\n");
    glDisable(GL_LIGHTING);
    glColor4fv(raw);
    latch(got);
    check("GL_CURRENT_RASTER_COLOR clamped", got, clamped);

    /* Control: the *current* color is not a color produced for a primitive and
     * must keep the raw value. Both drivers agree here, so a failure above is
     * a missing clamp rather than a different idea of glColor. */
    glGetFloatv(GL_CURRENT_COLOR, cur);
    check("GL_CURRENT_COLOR stays raw", cur, raw);
    printf("\n");

    /* Case B: the same clamp, reached through lighting instead - emission past
     * 1.0 with no light enabled. This is the half of the sentence every driver
     * gets right, which localizes the failure above to the "whether enabled or
     * not" clause rather than to clamping in general. */
    printf("Case B: lit, emission past 1.0                 [control]\n");
    {
        GLfloat zero[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        GLfloat want[4];
        for (i = 0; i < 4; i++)
            want[i] = clamp01(emission[i]);
        glEnable(GL_LIGHTING);
        /* Zero every other term - including the light-model ambient, which is
         * 0.2 by default and would otherwise be added to the emission and make
         * the expected value depend on the whole equation rather than on the
         * clamp being tested. */
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, zero);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, zero);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, zero);
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, emission);
        latch(got);
        check("GL_CURRENT_RASTER_COLOR clamped", got, want);
        glDisable(GL_LIGHTING);
    }
    printf("\n");

    if (g_failures) {
        printf("RESULT: BUG REPRODUCED - %d check(s) failed.\n\n", g_failures);
        printf("Per %s:\n", SPEC_CITE);
        printf("  \"After lighting (whether enabled or not), all components of "
               "both primary\n"
               "   and secondary colors are clamped to the range [0, 1].\"\n\n");
        printf("Case B shows this driver does apply that clamp on the lit path;\n"
               "case A shows it skips it when lighting is disabled, which the\n"
               "parenthetical explicitly covers. %s\n", SPEC_URL);
    } else {
        printf("RESULT: conformant.\n");
    }
    return g_failures ? 1 : 0;
}
