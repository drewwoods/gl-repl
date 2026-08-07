/*
 * Mesa bug repro: retargeting glColorMaterial to a different face discards the
 * color the previously tracked face had already received.
 *
 * Build (plain GLX, no GLUT):
 *     cc -std=c99 -O0 -o mesa-colormaterial-face-switch \
 *        mesa-colormaterial-face-switch.c -lGL -lX11 -lm
 *     ./mesa-colormaterial-face-switch
 *
 * Exit status: 0 = conformant, 1 = bug reproduced, 77 = could not set up GL.
 *
 * The core of it is four calls:
 *
 *     glEnable(GL_COLOR_MATERIAL);
 *     glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
 *     glColor3f(0.2, 0.6, 0.3);                          // -> GL_FRONT
 *     glColorMaterial(GL_BACK,  GL_AMBIENT_AND_DIFFUSE); // retarget only
 *
 *     glGetMaterialfv(GL_FRONT, GL_AMBIENT, m);
 *     // expected: 0.20 0.60 0.30
 *     // Mesa:     1.00 1.00 1.00   (the untouched GL default)
 *
 * The last call names GL_BACK and changes no color, yet GL_FRONT loses the
 * value it tracked one call earlier.
 *
 * Spec: OpenGL 2.1 (December 1, 2006), section 2.14.3 "ColorMaterial", p. 66.
 * https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf#page=80
 *
 *   "The replacements made to material properties are permanent; the replaced
 *    values remain until changed by either sending a new color or by setting a
 *    new material value when ColorMaterial is not currently enabled to override
 *    that particular value. When COLOR_MATERIAL is enabled, the indicated
 *    parameter or parameters always track the current color."
 *
 * The spec names exactly two things that may overwrite a tracked value: a new
 * Color*, or a Material* call made while ColorMaterial is disabled. Retargeting
 * the face with ColorMaterial is neither, so GL_FRONT must keep its value.
 *
 * The same section (p. 68) gives the converse case explicitly - "calling
 * ColorMaterial(FRONT, AMBIENT) while COLOR_MATERIAL is enabled sets the front
 * material a_cm to the value of the current color" - and Mesa gets that right.
 * Writing the face being named works; the face being left behind is the bug.
 *
 * Observed on Mesa 25.2.8 (iris, Intel ADL-N) and Mesa 25.1.7
 * (llvmpipe via OSMesa). Not reproducible on NVIDIA's proprietary driver or
 * Apple's OpenGL, which both report 0.20 0.60 0.30.
 *
 * Two things that make this easy to miss, both checked below:
 *
 *   - Use a distinctive color. The unwritten material is left at GL's default
 *     white, so a test that tracks white cannot tell "tracked correctly" from
 *     "never written at all".
 *
 *   - Do not query between the glColor and the face switch. A glGetMaterialfv
 *     in that window makes the bug disappear (case C), which is the signature
 *     of pending material state that the query path realizes and the
 *     glColorMaterial path does not.
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>

/* Deliberately not white and not the 0.2 grey used for sentinels, so a
 * mismatch cannot be confused with either a default or a leftover. */
#define WANT_R 0.20f
#define WANT_G 0.60f
#define WANT_B 0.30f

/* Printed with the results so the output is self-contained when pasted into a
 * bug tracker. #page=80 is the PDF page; the spec's own printed page is 66. */
#define SPEC_CITE "OpenGL 2.1 (2006-12-01), sec 2.14.3 \"ColorMaterial\", p.66"
#define SPEC_URL  "https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf#page=80"

static int g_failures;

static void check(const char *label, GLenum face, float r, float g, float b)
{
    GLfloat m[4];
    int ok;

    glGetMaterialfv(face, GL_AMBIENT, m);
    ok = (m[0] > r - 0.01f && m[0] < r + 0.01f) &&
         (m[1] > g - 0.01f && m[1] < g + 0.01f) &&
         (m[2] > b - 0.01f && m[2] < b + 0.01f);
    if (!ok)
        g_failures++;

    printf("  [%s] %-44s got %.2f %.2f %.2f   want %.2f %.2f %.2f\n",
           ok ? "PASS" : "FAIL", label, m[0], m[1], m[2], r, g, b);
}

/*
 * switch_face   - retarget to GL_BACK after the glColor. This is the single
 *                 element that decides pass/fail; with it omitted GL_FRONT
 *                 keeps its color correctly.
 * probe_between - issue a state query between the glColor and the retarget,
 *                 which masks the bug.
 *
 * Each case runs in a context that has had no other GL calls, so nothing can
 * be attributed to leftover state. Note there is no reset helper here: a
 * glColorMaterial call used for cleanup would itself be a face selection and
 * would perturb what is being measured.
 */
static void run_case(const char *title, int switch_face, int probe_between,
                     Display *dpy, XVisualInfo *vi, Window win)
{
    GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);

    printf("%s\n", title);
    if (!ctx) {
        printf("  (could not create context)\n\n");
        g_failures++;
        return;
    }
    glXMakeCurrent(dpy, win, ctx);

    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    glColor3f(WANT_R, WANT_G, WANT_B);

    if (probe_between) {
        GLfloat scratch[4];
        glGetMaterialfv(GL_FRONT, GL_AMBIENT, scratch);
    }
    if (switch_face)
        glColorMaterial(GL_BACK, GL_AMBIENT_AND_DIFFUSE);

    check("GL_FRONT holds the color it tracked",
          GL_FRONT, WANT_R, WANT_G, WANT_B);
    if (switch_face)
        check("GL_BACK picked up the current color",
              GL_BACK, WANT_R, WANT_G, WANT_B);
    printf("\n");

    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
}

int main(void)
{
    Display *dpy;
    XVisualInfo *vi;
    GLXContext probe;
    Window win;
    XSetWindowAttributes swa;
    int attribs[] = { GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, None };

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "cannot open display (is DISPLAY set?)\n");
        return 77;
    }
    vi = glXChooseVisual(dpy, DefaultScreen(dpy), attribs);
    if (!vi) {
        fprintf(stderr, "no suitable GLX visual\n");
        return 77;
    }
    memset(&swa, 0, sizeof swa);
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen),
                                   vi->visual, AllocNone);
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, 64, 64, 0,
                        vi->depth, InputOutput, vi->visual, CWColormap, &swa);

    probe = glXCreateContext(dpy, vi, NULL, True);
    if (!probe) {
        fprintf(stderr, "cannot create GLX context\n");
        return 77;
    }
    glXMakeCurrent(dpy, win, probe);
    printf("GL_VENDOR  : %s\n", (const char *)glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL_VERSION : %s\n\n", (const char *)glGetString(GL_VERSION));
    printf("Spec       : %s\n", SPEC_CITE);
    printf("             %s\n\n", SPEC_URL);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, probe);

    run_case("Case A: no face switch                  [control]",
             0, 0, dpy, vi, win);
    run_case("Case B: retarget to GL_BACK afterwards  [the bug]",
             1, 0, dpy, vi, win);
    run_case("Case C: as B, but query before the switch",
             1, 1, dpy, vi, win);

    /* The spec's own example from p. 68, as a control: ColorMaterial issued
     * while enabled must set the material of the face it names. Mesa passes
     * this, which localizes the bug to the face being retargeted away from. */
    {
        GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
        printf("Case D: spec p.68 example, ColorMaterial sets the named face\n");
        glXMakeCurrent(dpy, win, ctx);
        glEnable(GL_COLOR_MATERIAL);
        glColor3f(WANT_R, WANT_G, WANT_B);
        glColorMaterial(GL_FRONT, GL_AMBIENT);
        check("GL_FRONT ambient = current color",
              GL_FRONT, WANT_R, WANT_G, WANT_B);
        printf("\n");
        glXMakeCurrent(dpy, None, NULL);
        glXDestroyContext(dpy, ctx);
    }

    printf("Expected: GL_FRONT is %.2f %.2f %.2f in every case.\n",
           WANT_R, WANT_G, WANT_B);
    printf("On Mesa:  A, C and D pass; only B fails, reporting GL_FRONT =\n");
    printf("          1.00 1.00 1.00 - retargeting the tracked face erased\n");
    printf("          the color it held. A state query in between (C) hides\n");
    printf("          the failure; writing the named face (D) works.\n\n");

    if (g_failures) {
        printf("RESULT: BUG REPRODUCED - %d check(s) failed.\n\n", g_failures);
        printf("Case B violates the permanence rule in %s:\n", SPEC_CITE);
        printf("  \"The replacements made to material properties are "
               "permanent; the replaced\n"
               "   values remain until changed by either sending a new color "
               "or by setting a\n"
               "   new material value when ColorMaterial is not currently "
               "enabled to override\n"
               "   that particular value.\"\n");
        printf("Retargeting the tracked face is neither of those, so GL_FRONT "
               "must keep\n"
               "its value. %s\n", SPEC_URL);
    } else {
        printf("RESULT: conformant.\n");
    }

    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return g_failures ? 1 : 0;
}
