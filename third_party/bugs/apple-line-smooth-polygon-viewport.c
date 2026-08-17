/*
 * Apple OpenGL bug repro: with GL_LINE_SMOOTH and blending enabled, a polygon
 * rasterized in GL_LINE polygon mode draws NOTHING when the viewport is
 * smaller than the drawable. The identical draw renders when the viewport
 * covers the whole window, when GL_LINE_SMOOTH is off, or when the same
 * outline is submitted as a GL_LINE_LOOP instead of a polygon in line mode.
 *
 * Build (GLUT; deprecation warnings are Apple's, the API is the point):
 *     cc -std=c99 -O0 -Wno-deprecated-declarations \
 *        -o apple-line-smooth-polygon-viewport \
 *        apple-line-smooth-polygon-viewport.c \
 *        -framework OpenGL -framework GLUT
 *     ./apple-line-smooth-polygon-viewport
 *
 * Linux (for the cross-check; Mesa passes):
 *     cc -std=c99 -O0 -o apple-line-smooth-polygon-viewport \
 *        apple-line-smooth-polygon-viewport.c -lglut -lGL -lGLU -lm
 *
 * Exit status: 0 = conformant, 1 = bug reproduced, 77 = could not set up GL.
 *
 * The bug in five calls - a window larger than the viewport drawn into, and
 * an antialiased polygon outline:
 *
 *     glutInitWindowSize(1200, 800);
 *     glViewport(0, 0, 1200, 440);          // viewport < drawable
 *     glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
 *     glEnable(GL_LINE_SMOOTH);
 *     glEnable(GL_BLEND);                   // smoothing needs it to show
 *     glBegin(GL_POLYGON); ...              // Apple: zero fragments
 *
 * No expected pixel value is hardcoded. Every case is checked against ink the
 * same driver produced from the same geometry under state that cannot change
 * whether the triangle is on screen:
 *
 *   1. the same draw with GL_LINE_SMOOTH off (case A), and
 *   2. the same draw with the viewport covering the window (case B).
 *
 * Both oracles draw the outline. So a failure in case C cannot be blamed on
 * this program's projection, camera or readback: the driver disagrees with
 * itself over a state change that is defined as an antialiasing quality
 * choice, not a visibility one.
 *
 * Cases D and E narrow it: a GL_LINE_LOOP through the same three vertices
 * under the failing state renders, and so does GL_FILL polygon mode. The
 * defect is specific to polygon edges rasterized as antialiased lines.
 *
 * It is not multisampling: the window here requests no multisample visual,
 * and the failure is unchanged with one (GLUT_MULTISAMPLE + glEnable(
 * GL_MULTISAMPLE)). It is also not a clean on/off - it is a clip. Sweeping
 * the window height against a fixed 440-tall viewport, the outline is eaten
 * progressively from one side as the mismatch grows:
 *
 *     window 1200x440 -> 1701 lit pixels (all of it)
 *     window 1200x441 -> 1018
 *     window 1200x450 ->  310
 *     window 1200x500 ->    0
 *
 * Spec: OpenGL 2.1 (December 1, 2006), sec 3.5.4 "Polygon Rasterization and
 * Depth Offset", p. 118, and sec 3.4.2 "Other Line Segment Features", p. 108.
 * https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf
 *
 *   "If PolygonMode is called with [...] LINE, [...] the polygon is
 *    rasterized by [...] drawing the boundary edges of the polygon as line
 *    segments. [...] the rasterization of each of these line segments is
 *    controlled by the line width, stipple, and antialiasing state."
 *
 * Line antialiasing (sec 3.4.2) is specified to change the *coverage* a
 * fragment carries, which blending then applies - it is nowhere permitted to
 * discard fragments, and the viewport (sec 2.11.1) only maps normalized
 * device coordinates to window coordinates. Nothing in either couples the
 * result to the size of the drawable the viewport sits in.
 *
 * Observed on Apple M2, GL 2.1 Metal - 90.5 (macOS 15). Not reproducible on
 * Mesa 25.2.8 (Intel ADL-N), which draws the outline in every case.
 *
 * Workaround for applications: any of - render with the viewport covering the
 * whole drawable and confine the scene with glScissor plus a matching
 * projection; submit outlines as real line primitives instead of relying on
 * GL_LINE polygon mode; or drop GL_LINE_SMOOTH while polygon mode is LINE.
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

/* Window, and the smaller viewport drawn into. The proportions are the ones
 * this was found at (a 3D viewport under a code panel); nothing depends on
 * the exact numbers, only on viewport < window. */
#define WIN_W 1200
#define WIN_H  800
#define VP_W  1200
#define VP_H   440

/* A pixel counts as ink if any channel clears this. The background is 0.1
 * grey (26) and the geometry is white, so the threshold is nowhere near
 * either - an antialiased edge at partial coverage still lands well above. */
#define INK_THRESHOLD 60

#define SPEC_CITE \
    "OpenGL 2.1 (2006-12-01), sec 3.5.4 \"Polygon Rasterization\", p.118"
#define SPEC_URL \
    "https://registry.khronos.org/OpenGL/specs/gl/" \
    "glspec21.pdf"

static int g_failures;
static unsigned char g_pixels[WIN_W * WIN_H * 3];

typedef struct {
    int smooth;      /* GL_LINE_SMOOTH                                */
    int blend;       /* blending, standard src-over                   */
    int full_view;   /* viewport covers the window instead of VP_W/H  */
    int line_loop;   /* submit a GL_LINE_LOOP instead of a polygon    */
    int fill;        /* GL_FILL polygon mode instead of GL_LINE       */
} Case;

/* Draw the one triangle under `c`, then count lit pixels in the whole window.
 * Reading the back buffer of a double-buffered window (never swapped) keeps
 * the result independent of whether the window is obscured. */
static long draw_and_count(const Case *c) {
    int vp_w = c->full_view ? WIN_W : VP_W;
    int vp_h = c->full_view ? WIN_H : VP_H;
    double top = 1.42046, right = top * (double)vp_w / (double)vp_h;
    long ink = 0;
    int i;

    glViewport(0, 0, vp_w, vp_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-right, right, -top, top, -200.0, 200.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -3.4293f);

    if (c->smooth) glEnable(GL_LINE_SMOOTH); else glDisable(GL_LINE_SMOOTH);
    if (c->blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPolygonMode(GL_FRONT_AND_BACK, c->fill ? GL_FILL : GL_LINE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(c->line_loop ? GL_LINE_LOOP : GL_POLYGON);
    glVertex2f(0.0f, 1.0f);
    glVertex2f(-1.0f, -1.0f);
    glVertex2f(1.0f, -1.0f);
    glEnd();
    glFinish();

    glReadPixels(0, 0, WIN_W, WIN_H, GL_RGB, GL_UNSIGNED_BYTE, g_pixels);
    for (i = 0; i < WIN_W * WIN_H * 3; i += 3) {
        if (g_pixels[i] > INK_THRESHOLD ||
            g_pixels[i + 1] > INK_THRESHOLD ||
            g_pixels[i + 2] > INK_THRESHOLD)
            ink++;
    }
    return ink;
}

static void report(const char *label, long ink, const char *expectation) {
    int ok = ink > 0;
    if (!ok)
        g_failures++;
    printf("  [%s] %-52s %7ld lit pixels   (%s)\n",
           ok ? "PASS" : "FAIL", label, ink, expectation);
}

int main(int argc, char **argv) {
    Case control_no_smooth = { 0, 1, 0, 0, 0 };
    Case control_full_view = { 1, 1, 1, 0, 0 };
    Case failing           = { 1, 1, 0, 0, 0 };
    Case as_line_loop      = { 1, 1, 0, 1, 0 };
    Case as_fill           = { 1, 1, 0, 0, 1 };
    long ink_a, ink_b, ink_c, ink_d, ink_e;

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DEPTH | GLUT_DOUBLE);
    glutInitWindowSize(WIN_W, WIN_H);
    if (glutCreateWindow("line smooth polygon-mode viewport probe") <= 0) {
        fprintf(stderr, "cannot create a GL window\n");
        return 77;
    }

    printf("GL_VENDOR  : %s\n", (const char *)glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL_VERSION : %s\n\n", (const char *)glGetString(GL_VERSION));
    printf("Spec       : %s\n", SPEC_CITE);
    printf("             %s\n\n", SPEC_URL);
    printf("Window %dx%d, viewport %dx%d unless stated.\n"
           "Same triangle every time; every case must put ink on screen.\n\n",
           WIN_W, WIN_H, VP_W, VP_H);

    /* Oracles first: the same geometry the failing case draws, under state
     * that cannot decide whether it is on screen. */
    ink_a = draw_and_count(&control_no_smooth);
    report("A oracle: GL_LINE_SMOOTH off, viewport < window", ink_a,
           "the outline, aliased");
    ink_b = draw_and_count(&control_full_view);
    report("B oracle: GL_LINE_SMOOTH on, viewport = window", ink_b,
           "the outline, antialiased");

    ink_c = draw_and_count(&failing);
    report("C  bug:   GL_LINE_SMOOTH on, viewport < window", ink_c,
           "must match A and B in kind");

    ink_d = draw_and_count(&as_line_loop);
    report("D narrow: same state, GL_LINE_LOOP not GL_POLYGON", ink_d,
           "line primitives are unaffected");
    ink_e = draw_and_count(&as_fill);
    report("E narrow: same state, GL_FILL polygon mode", ink_e,
           "fill rasterization is unaffected");

    printf("\n");
    if (g_failures == 0) {
        printf("conformant: every case drew the triangle\n");
        return 0;
    }
    printf("BUG REPRODUCED: %d case(s) drew nothing.\n", g_failures);
    if (ink_c == 0 && ink_a > 0 && ink_b > 0)
        printf("Antialiased polygon-mode lines are discarded when the "
               "viewport is\nsmaller than the drawable; the same draw with "
               "smoothing off (A) or with\nthe viewport covering the window "
               "(B) renders normally.\n");
    return 1;
}
