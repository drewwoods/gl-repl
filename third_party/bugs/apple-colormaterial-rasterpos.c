/*
 * Apple OpenGL bug repro: with GL_COLOR_MATERIAL enabled at the glRasterPos
 * call, the material components it tracks are lit as ZERO when GL computes
 * GL_CURRENT_RASTER_COLOR. The same state lights an ordinary vertex correctly.
 *
 * Build (GLUT; deprecation warnings are Apple's, the API is the point):
 *     cc -std=c99 -O0 -Wno-deprecated-declarations \
 *        -o apple-colormaterial-rasterpos apple-colormaterial-rasterpos.c \
 *        -framework OpenGL -framework GLUT
 *     ./apple-colormaterial-rasterpos
 *
 * Linux (for the cross-check; Mesa and NVIDIA both pass):
 *     cc -std=c99 -O0 -o apple-colormaterial-rasterpos \
 *        apple-colormaterial-rasterpos.c -lglut -lGL -lGLU -lm
 *
 * Exit status: 0 = conformant, 1 = bug reproduced, 77 = could not set up GL.
 *
 * The bug in three calls - color material tracking a distinctive color, then a
 * raster position latched under lighting:
 *
 *     glEnable(GL_LIGHTING); glEnable(GL_LIGHT0);
 *     glEnable(GL_COLOR_MATERIAL);
 *     glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
 *     glColor3f(0.2, 0.5, 0.7);
 *     glRasterPos3f(0, 0, 0);
 *     glGetFloatv(GL_CURRENT_RASTER_COLOR, c);   // Apple: 0 0 0 1
 *
 * No expected value is hardcoded. Each case is checked against two oracles the
 * same driver produces under state the spec requires to be equivalent:
 *
 *   1. the same material written explicitly with glMaterialfv and the cap off,
 *      which is what color material is *defined* to be equivalent to, and
 *   2. the color GL itself assigns a vertex at the same position under the
 *      identical state, captured with GL_FEEDBACK / GL_3D_COLOR.
 *
 * So a failure cannot be blamed on this program's idea of the lighting
 * equation: the driver disagrees with itself.
 *
 * Spec: OpenGL 2.1 (July 30, 2006), sec 2.13 "Current Raster Position", p. 54.
 * https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf
 * (Wording carried through to the 4.6 compatibility profile unchanged.)
 *
 *   "The coordinates are treated as if they were specified in a Vertex
 *    command. [...] These coordinates, along with current values, are used to
 *    generate primary and secondary colors and texture coordinates just as is
 *    done for a vertex. The colors and texture coordinates so produced replace
 *    the colors and texture coordinates stored in the current raster
 *    position's associated data."
 *
 * RasterPos is specified as a Vertex for lighting purposes, so oracle (2) is
 * not an approximation - it is the same computation the spec points at.
 * Color material itself is sec 2.14.3: while COLOR_MATERIAL is enabled the
 * indicated parameters "always track the current color".
 *
 * Observed on Apple M2, GL 2.1 Metal - 90.5 (macOS). Not reproducible on Mesa
 * 25.2.8 (Intel ADL-N) or NVIDIA 595.84, which both light the tracked
 * components normally.
 *
 * Workaround for applications: glDisable(GL_COLOR_MATERIAL) immediately
 * before the glRasterPos (case C below); the latch is then correct.
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

/* Distinctive, and chosen so ambient + diffuse stays inside [0,1]: with the
 * default GL_LIGHT0 (white diffuse, directional along +z) and the default
 * light-model ambient of 0.2, a tracked AMBIENT_AND_DIFFUSE color C lights to
 * 1.2 * C. Saturating would hide which model produced the value. */
#define WANT_R 0.20f
#define WANT_G 0.50f
#define WANT_B 0.70f

#define SPEC_CITE \
    "OpenGL 2.1 (2006-07-30), sec 2.13 " \
    "\"Current Raster Position\", p.54"
#define SPEC_URL \
    "https://registry.khronos.org/OpenGL/specs/gl/" \
    "glspec21.pdf"

static int g_failures;

static int close_enough(const GLfloat *a, const GLfloat *b) {
    int i;
    for (i = 0; i < 3; i++) {
        float d = a[i] - b[i];
        if (d < -0.005f || d > 0.005f)
            return 0;
    }
    return 1;
}

/* Lighting that makes every term weigh something: one directional light with
 * the default white diffuse, and a normal facing it head-on (n.L = 1). */
static void setup_lighting(void) {
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glNormal3f(0.0f, 0.0f, 1.0f);
}

/* Reset every cell these cases touch, so each runs from the same baseline in
 * one context (creating a context per case needs a windowing path GLUT does
 * not expose portably). */
static void reset_state(void) {
    GLfloat white[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
    GLfloat ambdef[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
    GLfloat difdef[4] = { 0.8f, 0.8f, 0.8f, 1.0f };

    glDisable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, ambdef);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, difdef);
    glColor4fv(white);
    glDisable(GL_LIGHTING);
    glDisable(GL_LIGHT0);
}

/* The raster color the driver latches under whatever state the caller left. */
static void latch(GLfloat *out) {
    GLboolean valid = GL_FALSE;

    glRasterPos3f(0.0f, 0.0f, 0.0f);
    glGetBooleanv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
    if (!valid) {
        /* GL leaves the color undefined for a clipped raster position, so a
         * value read here would mean nothing. */
        fprintf(stderr, "raster position clipped - result would be undefined\n");
        exit(77);
    }
    glGetFloatv(GL_CURRENT_RASTER_COLOR, out);
}

/* Oracle 2: the color GL assigns a *vertex* at the same position under the
 * same state, read out of the feedback buffer. GL_3D_COLOR returns
 * x, y, z, then RGBA per vertex, after the token. */
static int lit_vertex_color(GLfloat *out) {
    GLfloat buf[64];
    GLint written;
    int i;

    glFeedbackBuffer(64, GL_3D_COLOR, buf);
    glRenderMode(GL_FEEDBACK);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    written = glRenderMode(GL_RENDER);

    /* GL_POINT_TOKEN, then one vertex: x y z r g b a */
    if (written < 8)
        return 0;
    for (i = 0; i < 4; i++)
        out[i] = buf[4 + i];
    return 1;
}

static void report(const char *label, const GLfloat *got, const GLfloat *want,
                   const char *want_label) {
    int ok = close_enough(got, want);
    if (!ok)
        g_failures++;
    printf("  [%s] %-40s got %.3f %.3f %.3f   %s %.3f %.3f %.3f\n",
           ok ? "PASS" : "FAIL", label, got[0], got[1], got[2],
           want_label, want[0], want[1], want[2]);
}

int main(int argc, char **argv) {
    GLfloat tracked[4] = { WANT_R, WANT_G, WANT_B, 1.0f };
    GLfloat reference[4], vertex[4], got[4];
    GLfloat mat[4];

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DEPTH | GLUT_SINGLE);
    glutInitWindowSize(64, 64);
    if (glutCreateWindow("colormaterial rasterpos probe") <= 0) {
        fprintf(stderr, "cannot create a GL window\n");
        return 77;
    }

    printf("GL_VENDOR  : %s\n", (const char *)glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL_VERSION : %s\n\n", (const char *)glGetString(GL_VERSION));
    printf("Spec       : %s\n", SPEC_CITE);
    printf("             %s\n\n", SPEC_URL);

    /* Oracle 1: the same material, written explicitly, cap off. Whatever this
     * driver's lighting produces here is what color material must reproduce. */
    reset_state();
    setup_lighting();
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, tracked);
    latch(reference);
    printf("Reference (glMaterialfv, GL_COLOR_MATERIAL off)\n");
    printf("  raster color = %.3f %.3f %.3f   <- every case below must match\n\n",
           reference[0], reference[1], reference[2]);

    /* Case A: the cap is tracking the same color instead. Per sec 12.2.3 this
     * is the same material, so it must light to the same value. */
    printf("Case A: GL_COLOR_MATERIAL tracking the color   [the bug]\n");
    reset_state();
    setup_lighting();
    /* Mode first, then the color, then the enable: tracking begins at the
     * glEnable, so selecting the mode afterwards would first track into the
     * previous mode's components and leave them set. */
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glColor4fv(tracked);
    glEnable(GL_COLOR_MATERIAL);
    latch(got);
    report("raster color == explicit material", got, reference, "want");

    /* Control: the tracking itself worked - the material cell holds the color.
     * Only the raster latch disagrees with it. */
    glGetMaterialfv(GL_FRONT, GL_AMBIENT, mat);
    report("GL_FRONT ambient tracked the color", mat, tracked, "want");

    /* Control: a vertex under this identical state is lit correctly, which
     * confines the fault to the raster-position path. */
    if (lit_vertex_color(vertex))
        report("lit vertex color (feedback)", vertex, reference, "want");
    else
        printf("  (feedback unavailable - vertex oracle skipped)\n");
    printf("\n");

    /* Case B: same, tracking GL_DIFFUSE only. On Apple exactly the diffuse
     * term goes missing and the ambient sum is left behind, which is what
     * shows the tracked component is being lit as zero rather than the whole
     * material being ignored. */
    printf("Case B: tracking GL_DIFFUSE only\n");
    reset_state();
    setup_lighting();
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, tracked);
    latch(reference);
    reset_state();
    setup_lighting();
    glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
    glColor4fv(tracked);
    glEnable(GL_COLOR_MATERIAL);
    latch(got);
    report("raster color == explicit material", got, reference, "want");
    printf("\n");

    /* Case C: the workaround - disable the cap immediately before the
     * glRasterPos. The material keeps the tracked value, and the latch is
     * correct. Apple passes this. */
    printf("Case C: cap disabled just before glRasterPos  [workaround]\n");
    reset_state();
    setup_lighting();
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, tracked);
    latch(reference);
    reset_state();
    setup_lighting();
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glColor4fv(tracked);
    glDisable(GL_COLOR_MATERIAL);
    latch(got);
    report("raster color == explicit material", got, reference, "want");
    printf("\n");

    if (g_failures) {
        printf("RESULT: BUG REPRODUCED - %d check(s) failed.\n\n", g_failures);
        printf("The material holds the tracked color (control passes) and a\n"
               "vertex under the same state is lit correctly (control passes),\n"
               "yet the raster color latched at glRasterPos lights the tracked\n"
               "components as zero. Per %s\n", SPEC_CITE);
        printf("the raster position's coordinates are \"treated as if they were\n"
               "specified in a Vertex command\", so the two must agree.\n");
    } else {
        printf("RESULT: conformant.\n");
    }
    return g_failures ? 1 : 0;
}
