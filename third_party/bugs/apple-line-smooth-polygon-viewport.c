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
 *     ./apple-line-smooth-polygon-viewport --live
 *
 * Linux (for the cross-check; Mesa passes):
 *     cc -std=c99 -O0 -o apple-line-smooth-polygon-viewport \
 *        apple-line-smooth-polygon-viewport.c -lglut -lGL -lGLU -lm
 *
 * Exit status (batch mode):
 *     0 = conformant, 1 = bug reproduced, 77 = could not set up GL.
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
#include <string.h>

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
static unsigned char *g_pixels = NULL;
static size_t g_pixels_cap = 0;

typedef struct {
    int smooth;      /* GL_LINE_SMOOTH                                */
    int blend;       /* blending, standard src-over                   */
    int full_view;   /* viewport covers the window instead of VP_W/H  */
    int line_loop;   /* submit a GL_LINE_LOOP instead of a polygon    */
    int fill;        /* GL_FILL polygon mode instead of GL_LINE       */
} Case;

/* Dynamic buffer management for pixel readback. */
static void ensure_pixel_buffer(int w, int h) {
    size_t needed = (size_t)w * (size_t)h * 3;
    if (needed > g_pixels_cap) {
        free(g_pixels);
        g_pixels = (unsigned char *)malloc(needed);
        g_pixels_cap = needed;
    }
}

/* Draw the one triangle under `c`, then count lit pixels in the whole window.
 * Reading the back buffer of a double-buffered window (never swapped) keeps
 * the result independent of whether the window is obscured. */
static long draw_and_count(const Case *c) {
    int vp_w = c->full_view ? WIN_W : VP_W;
    int vp_h = c->full_view ? WIN_H : VP_H;
    double top = 1.42046, right = top * (double)vp_w / (double)vp_h;
    long ink = 0;
    size_t i;

    ensure_pixel_buffer(WIN_W, WIN_H);

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
    for (i = 0; i < (size_t)WIN_W * WIN_H * 3; i += 3) {
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

/* =========================================================================
 * Live Interactive Mode
 * ========================================================================= */

static int g_win_w = WIN_W;
static int g_win_h = WIN_H;
static int g_vp_w = VP_W;
static int g_vp_h = VP_H;
static int g_vp_x = 0;
static int g_vp_y = 0;
static Case g_case = { 1, 1, 0, 0, 0 }; /* Start in Case C (bug reproducer) */
static long g_live_ink = 0;
static int g_dragging = 0;

static void draw_hud_text(float x, float y, const char *str) {
    glRasterPos2f(x, y);
    while (*str) {
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, (unsigned char)*str);
        str++;
    }
}

static void display_live(void) {
    char buf[256];
    size_t i;
    long ink = 0;

    /* 1. Clear whole window background */
    glViewport(0, 0, g_win_w, g_win_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, g_win_w, 0, g_win_h, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* Background for inactive window area */
    glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* 2. Draw viewport container box and border in window coords */
    glColor3f(0.1f, 0.1f, 0.1f);
    glBegin(GL_QUADS);
    glVertex2i(g_vp_x, g_vp_y);
    glVertex2i(g_vp_x + g_vp_w, g_vp_y);
    glVertex2i(g_vp_x + g_vp_w, g_vp_y + g_vp_h);
    glVertex2i(g_vp_x, g_vp_y + g_vp_h);
    glEnd();

    /* Viewport boundary outline */
    glColor3f(0.25f, 0.45f, 0.65f);
    glBegin(GL_LINE_LOOP);
    glVertex2i(g_vp_x, g_vp_y);
    glVertex2i(g_vp_x + g_vp_w, g_vp_y);
    glVertex2i(g_vp_x + g_vp_w, g_vp_y + g_vp_h);
    glVertex2i(g_vp_x, g_vp_y + g_vp_h);
    glEnd();

    /* 3. Render reproducer geometry in the active viewport */
    glViewport(g_vp_x, g_vp_y, g_vp_w, g_vp_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    {
        double top = 1.42046;
        double right = top * (double)g_vp_w / (double)(g_vp_h > 0 ? g_vp_h : 1);
        glOrtho(-right, right, -top, top, -200.0, 200.0);
    }
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -3.4293f);

    if (g_case.smooth) glEnable(GL_LINE_SMOOTH); else glDisable(GL_LINE_SMOOTH);
    if (g_case.blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

    glPolygonMode(GL_FRONT_AND_BACK, g_case.fill ? GL_FILL : GL_LINE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(g_case.line_loop ? GL_LINE_LOOP : GL_POLYGON);
    glVertex2f(0.0f, 1.0f);
    glVertex2f(-1.0f, -1.0f);
    glVertex2f(1.0f, -1.0f);
    glEnd();
    glFinish();

    /* 4. Measure ink in the viewport */
    ensure_pixel_buffer(g_win_w, g_win_h);
    glReadPixels(0, 0, g_win_w, g_win_h, GL_RGB, GL_UNSIGNED_BYTE, g_pixels);
    for (i = 0; i < (size_t)g_win_w * g_win_h * 3; i += 3) {
        if (g_pixels[i] > INK_THRESHOLD ||
            g_pixels[i + 1] > INK_THRESHOLD ||
            g_pixels[i + 2] > INK_THRESHOLD)
            ink++;
    }
    g_live_ink = ink;

    /* 5. Render HUD Overlay */
    glViewport(0, 0, g_win_w, g_win_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, g_win_w, 0, g_win_h, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* Top HUD panel */
    int panel_h = 135;
    int panel_y = g_win_h - panel_h;
    if (panel_y < 0) panel_y = 0;

    glColor3f(0.10f, 0.12f, 0.16f);
    glBegin(GL_QUADS);
    glVertex2i(0, panel_y);
    glVertex2i(g_win_w, panel_y);
    glVertex2i(g_win_w, g_win_h);
    glVertex2i(0, g_win_h);
    glEnd();

    glColor3f(0.22f, 0.28f, 0.38f);
    glBegin(GL_LINES);
    glVertex2i(0, panel_y);
    glVertex2i(g_win_w, panel_y);
    glEnd();

    /* Title */
    glColor3f(0.95f, 0.95f, 0.95f);
    draw_hud_text(16, g_win_h - 22, "Apple OpenGL Bug Repro: Antialiased Polygon-Mode Line Viewport Clip");

    /* Status badge */
    if (g_live_ink == 0) {
        glColor3f(1.0f, 0.35f, 0.35f);
        snprintf(buf, sizeof(buf), "[ BUG TRIGGERED: 0 lit pixels - driver discarded outline! ]");
    } else {
        glColor3f(0.35f, 0.95f, 0.45f);
        snprintf(buf, sizeof(buf), "[ VISIBLE: %ld lit pixels rendered ]", g_live_ink);
    }
    draw_hud_text(16, g_win_h - 44, buf);

    /* Viewport and window details */
    glColor3f(0.85f, 0.85f, 0.85f);
    snprintf(buf, sizeof(buf),
             "Window: %dx%d  |  Viewport: %dx%d  (Height mismatch: %d px, Width mismatch: %d px)",
             g_win_w, g_win_h, g_vp_w, g_vp_h, g_win_h - g_vp_h, g_win_w - g_vp_w);
    draw_hud_text(16, g_win_h - 66, buf);

    /* State details */
    const char *prim_name = g_case.fill ? "GL_FILL (polygon)" :
                           (g_case.line_loop ? "GL_LINE_LOOP (primitive)" : "GL_POLYGON [GL_LINE mode]");
    snprintf(buf, sizeof(buf),
             "State: GL_LINE_SMOOTH=%s  |  GL_BLEND=%s  |  Primitive=%s",
             g_case.smooth ? "ON" : "OFF",
             g_case.blend ? "ON" : "OFF",
             prim_name);
    glColor3f(0.75f, 0.85f, 0.95f);
    draw_hud_text(16, g_win_h - 88, buf);

    /* Controls guide */
    glColor3f(0.9f, 0.8f, 0.4f);
    draw_hud_text(16, g_win_h - 110,
                  "Controls: [Up/Down or W/S] +/-Height  [Left/Right or A/D] +/-Width  [Mouse Drag] Resize  [Space] Smooth  [M] Mode");
    draw_hud_text(16, g_win_h - 126,
                  "Presets: [1] No Smooth  [2] Viewport=Window  [3] Bug Repro  [4] Line Loop  [5] Fill  |  [F] Toggle Full  [R] Reset  [Q] Quit");

    glutSwapBuffers();
}

static void reshape_live(int w, int h) {
    g_win_w = w;
    g_win_h = h;
    if (g_case.full_view) {
        g_vp_w = w;
        g_vp_h = h;
    } else {
        if (g_vp_w > g_win_w) g_vp_w = g_win_w;
        if (g_vp_h > g_win_h) g_vp_h = g_win_h;
    }
    glutPostRedisplay();
}

static void keyboard_live(unsigned char key, int x, int y) {
    (void)x; (void)y;
    int modifiers = glutGetModifiers();
    int step = (modifiers & GLUT_ACTIVE_SHIFT) ? 1 : 10;

    switch (key) {
        case 27: /* ESC */
        case 'q':
        case 'Q':
            exit(0);
            break;
        case 'w':
        case 'W':
            g_vp_h += step;
            if (g_vp_h > g_win_h) g_vp_h = g_win_h;
            g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
            glutPostRedisplay();
            break;
        case 's':
        case 'S':
            g_vp_h -= step;
            if (g_vp_h < 10) g_vp_h = 10;
            g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
            glutPostRedisplay();
            break;
        case 'd':
        case 'D':
            g_vp_w += step;
            if (g_vp_w > g_win_w) g_vp_w = g_win_w;
            g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
            glutPostRedisplay();
            break;
        case 'a':
        case 'A':
            g_vp_w -= step;
            if (g_vp_w < 10) g_vp_w = 10;
            g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
            glutPostRedisplay();
            break;
        case ' ':
            g_case.smooth = !g_case.smooth;
            glutPostRedisplay();
            break;
        case 'b':
        case 'B':
            g_case.blend = !g_case.blend;
            glutPostRedisplay();
            break;
        case 'm':
        case 'M':
            if (!g_case.line_loop && !g_case.fill) {
                g_case.line_loop = 1;
                g_case.fill = 0;
            } else if (g_case.line_loop) {
                g_case.line_loop = 0;
                g_case.fill = 1;
            } else {
                g_case.line_loop = 0;
                g_case.fill = 0;
            }
            glutPostRedisplay();
            break;
        case 'f':
        case 'F':
            if (g_vp_w == g_win_w && g_vp_h == g_win_h) {
                g_vp_w = VP_W > g_win_w ? g_win_w : VP_W;
                g_vp_h = VP_H > g_win_h ? g_win_h : VP_H;
                g_case.full_view = 0;
            } else {
                g_vp_w = g_win_w;
                g_vp_h = g_win_h;
                g_case.full_view = 1;
            }
            glutPostRedisplay();
            break;
        case 'r':
        case 'R':
            g_vp_w = VP_W > g_win_w ? g_win_w : VP_W;
            g_vp_h = VP_H > g_win_h ? g_win_h : VP_H;
            g_case = (Case){ 1, 1, 0, 0, 0 };
            glutPostRedisplay();
            break;
        case '1':
            /* Case A: Smooth OFF, viewport < window */
            g_case = (Case){ 0, 1, 0, 0, 0 };
            g_vp_w = VP_W > g_win_w ? g_win_w : VP_W;
            g_vp_h = VP_H > g_win_h ? g_win_h : VP_H;
            glutPostRedisplay();
            break;
        case '2':
            /* Case B: Smooth ON, viewport = window */
            g_case = (Case){ 1, 1, 1, 0, 0 };
            g_vp_w = g_win_w;
            g_vp_h = g_win_h;
            glutPostRedisplay();
            break;
        case '3':
            /* Case C: Bug repro (Smooth ON, viewport < window) */
            g_case = (Case){ 1, 1, 0, 0, 0 };
            g_vp_w = VP_W > g_win_w ? g_win_w : VP_W;
            g_vp_h = VP_H > g_win_h ? g_win_h : VP_H;
            glutPostRedisplay();
            break;
        case '4':
            /* Case D: GL_LINE_LOOP */
            g_case = (Case){ 1, 1, 0, 1, 0 };
            g_vp_w = VP_W > g_win_w ? g_win_w : VP_W;
            g_vp_h = VP_H > g_win_h ? g_win_h : VP_H;
            glutPostRedisplay();
            break;
        case '5':
            /* Case E: GL_FILL */
            g_case = (Case){ 1, 1, 0, 0, 1 };
            g_vp_w = VP_W > g_win_w ? g_win_w : VP_W;
            g_vp_h = VP_H > g_win_h ? g_win_h : VP_H;
            glutPostRedisplay();
            break;
    }
}

static void special_live(int key, int x, int y) {
    (void)x; (void)y;
    int modifiers = glutGetModifiers();
    int step = (modifiers & GLUT_ACTIVE_SHIFT) ? 1 : 10;

    switch (key) {
        case GLUT_KEY_UP:
            g_vp_h += step;
            if (g_vp_h > g_win_h) g_vp_h = g_win_h;
            g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
            glutPostRedisplay();
            break;
        case GLUT_KEY_DOWN:
            g_vp_h -= step;
            if (g_vp_h < 10) g_vp_h = 10;
            g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
            glutPostRedisplay();
            break;
        case GLUT_KEY_RIGHT:
            g_vp_w += step;
            if (g_vp_w > g_win_w) g_vp_w = g_win_w;
            g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
            glutPostRedisplay();
            break;
        case GLUT_KEY_LEFT:
            g_vp_w -= step;
            if (g_vp_w < 10) g_vp_w = 10;
            g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
            glutPostRedisplay();
            break;
        case GLUT_KEY_PAGE_UP:
            g_vp_h += 50;
            if (g_vp_h > g_win_h) g_vp_h = g_win_h;
            g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
            glutPostRedisplay();
            break;
        case GLUT_KEY_PAGE_DOWN:
            g_vp_h -= 50;
            if (g_vp_h < 10) g_vp_h = 10;
            g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
            glutPostRedisplay();
            break;
    }
}

static void mouse_live(int button, int state, int x, int y) {
    if (button == GLUT_LEFT_BUTTON) {
        if (state == GLUT_DOWN) {
            g_dragging = 1;
            int gl_y = g_win_h - y;
            if (x >= 10 && x <= g_win_w) g_vp_w = x;
            if (gl_y >= 10 && gl_y <= g_win_h) g_vp_h = gl_y;
            g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
            glutPostRedisplay();
        } else if (state == GLUT_UP) {
            g_dragging = 0;
        }
    }
}

static void motion_live(int x, int y) {
    if (g_dragging) {
        int gl_y = g_win_h - y;
        if (x < 10) x = 10;
        if (x > g_win_w) x = g_win_w;
        if (gl_y < 10) gl_y = 10;
        if (gl_y > g_win_h) gl_y = g_win_h;
        g_vp_w = x;
        g_vp_h = gl_y;
        g_case.full_view = (g_vp_w == g_win_w && g_vp_h == g_win_h);
        glutPostRedisplay();
    }
}

static void print_help(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Apple OpenGL bug repro: antialiased polygon-mode line viewport clip.\n\n");
    printf("Options:\n");
    printf("  --live, -l    Open interactive window to dynamically adjust viewport\n"
           "                size and observe the bug trigger in real time.\n");
    printf("  --help, -h    Show this help message.\n\n");
    printf("Exit status (batch mode):\n");
    printf("  0 = conformant, 1 = bug reproduced, 77 = could not set up GL.\n");
}

int main(int argc, char **argv) {
    int live_mode = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--live") == 0 || strcmp(argv[i], "-l") == 0) {
            live_mode = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n\n", argv[i]);
            print_help(argv[0]);
            return 2;
        }
    }

    Case control_no_smooth = { 0, 1, 0, 0, 0 };
    Case control_full_view = { 1, 1, 1, 0, 0 };
    Case failing           = { 1, 1, 0, 0, 0 };
    Case as_line_loop      = { 1, 1, 0, 1, 0 };
    Case as_fill           = { 1, 1, 0, 0, 1 };
    long ink_a, ink_b, ink_c, ink_d, ink_e;

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DEPTH | GLUT_DOUBLE);
    glutInitWindowSize(WIN_W, WIN_H);
    if (glutCreateWindow("Apple OpenGL Bug Repro: Line Smooth Polygon Viewport") <= 0) {
        fprintf(stderr, "cannot create a GL window\n");
        return 77;
    }

    printf("GL_VENDOR  : %s\n", (const char *)glGetString(GL_VENDOR));
    printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("GL_VERSION : %s\n\n", (const char *)glGetString(GL_VERSION));
    printf("Spec       : %s\n", SPEC_CITE);
    printf("             %s\n\n", SPEC_URL);

    if (live_mode) {
        printf("Starting interactive live mode (Window: %dx%d, Initial Viewport: %dx%d)...\n",
               WIN_W, WIN_H, VP_W, VP_H);
        printf("Use Up/Down (or W/S) to adjust viewport height, Left/Right (or A/D) for width,\n"
               "mouse drag to resize, or keys 1-5 for preset test cases.\n\n");
        glutDisplayFunc(display_live);
        glutReshapeFunc(reshape_live);
        glutKeyboardFunc(keyboard_live);
        glutSpecialFunc(special_live);
        glutMouseFunc(mouse_live);
        glutMotionFunc(motion_live);
        glutMainLoop();
        return 0;
    }

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

