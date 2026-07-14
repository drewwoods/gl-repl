/*
 * tools/cpuprof_demo/cpuprof_demo.c — standalone driver for the CPU
 * profiling subsystem, in the shape of a display-list micro-benchmark.
 *
 * Isolation proof: links ONLY src/support/cpuprof.c (the pure wall-time
 * sampler) + src/ui/support/cpuprof.c (the overlay panel) + ui/core theme.
 * No src/ui/app, src/app, src/repl, or src/editor — see CPUPROF_DEMO_DEP_SRCS
 * in the Makefile and check-cpuprof-demo-isolation. Sibling of memprof_demo.
 *
 * What it measures: the same 5 teapots drawn three ways each frame, each way
 * timed as its own profile section so the panel shows them side by side —
 *   - Immediate:           5x glutSolidTeapot (immediate-mode geometry);
 *   - Display list (reuse): a list compiled ONCE at startup, then 5x glCallList;
 *   - Recompile / frame:    glNewList/glEndList every frame (the "compile" span)
 *                           then 5x glCallList of that fresh list.
 * The "compile" sub-row is the number of interest: it is the per-frame display-
 * list compilation overhead — i.e. what you would pay PER accumulation-buffer
 * AA sample if the scene's list were rebuilt each pass instead of compiled once
 * and reused. "reuse > call" vs "recompile > compile + call" makes the trade
 * legible. 'd' collapses to the three method totals / expands the call+compile
 * breakdown; 'q' quits.
 *
 * Sections: this demo declares its OWN catalog (the CP_* enum below) — it does
 * NOT borrow gl-repl's section names. cpuprof_demo and gl-repl share the
 * generic cpuprof objects, so the timer's slot capacity is fixed at
 * PROF_SECTION_COUNT (gl-repl's count, force-included via prof_sections.h); the
 * demo simply uses the first CP_COUNT of those slots and labels the rest as
 * absent (prof_section_info returns {NULL} → the panel omits the row). depth is
 * carried explicitly per section; the panel derives each row's indentation from
 * it. gl-repl's own labels live in src/app/glr_prof.c.
 *
 * The panel renderer is snapshot-free: it consumes a UiProfilePanelView whose
 * panel_x/panel_y this demo bakes (top-right), exactly where the full app's
 * controller bakes the stacked anchor.
 */
#include "gl_includes.h"

#include "ui/support/cpuprof.h"   /* UiProfilePanelView, ui_profile_panel_* */
#include "support/cpuprof.h"      /* prof_begin/_end/_frame_tick + ProfSection */
#include "c_compat.h"             /* STATIC_ASSERT */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* The demo's own profile sections. Values index the shared timer's slots, so
 * CP_COUNT must not exceed its capacity (asserted below). */
enum {
    CP_FRAME_TOTAL = 0,
    CP_IMMEDIATE,                 /* 5x glutSolidTeapot */
    CP_REUSE,                     /* display list compiled once, then called */
    CP_REUSE_CALL,                /*   5x glCallList */
    CP_RECOMPILE,                 /* display list rebuilt every frame */
    CP_RECOMPILE_COMPILE,         /*   glNewList .. glEndList (the overhead) */
    CP_RECOMPILE_CALL,            /*   5x glCallList of the fresh list */
    CP_PANEL,                     /* the profile panel itself */
    CP_COUNT
};
STATIC_ASSERT(CP_COUNT <= PROF_SECTION_COUNT,
              "cpuprof_demo sections must fit the shared timer's slot capacity");

#define TEAPOTS_PER_ROW   5
#define TEAPOT_SIZE       0.55
#define SCENE_Z         -15.0f

static int   g_window_w = 940;
static int   g_window_h = 680;
static float g_spin     = 0.0f;
static UiProfilePanelMode g_mode = PROFILE_PANEL_HISTOGRAM;

/* Display lists: one compiled once and reused across frames, one rebuilt every
 * frame. Created lazily on the first display callback (needs a live context). */
static GLuint g_reuse_list     = 0;
static GLuint g_recompile_list = 0;
static int    g_lists_ready    = 0;

/* The demo's half of the cpuprof display contract (gl-repl's half is
 * src/app/glr_prof.c): { bare label, explicit depth, is_total }. depth — not
 * any baked-in indentation — drives how far the panel indents the row, and
 * depth>0 marks nested rows in the collapsible tree. Slots past CP_COUNT are
 * unused: prof_section_info returns {NULL} and the panel omits them. */
static const ProfSectionInfo k_sections[CP_COUNT] = {
    [CP_FRAME_TOTAL]       = { "Frame Total",          0, 1 },
    [CP_IMMEDIATE]         = { "Immediate x5",         0, 0 },
    [CP_REUSE]             = { "Display list (reuse)",  0, 0 },
    [CP_REUSE_CALL]        = { "call list x5",         1, 0 },
    [CP_RECOMPILE]         = { "Recompile / frame",     0, 0 },
    [CP_RECOMPILE_COMPILE] = { "compile",              1, 0 },
    [CP_RECOMPILE_CALL]    = { "call list x5",         1, 0 },
    [CP_PANEL]             = { "Profile Panel",        0, 0 },
};

ProfSectionInfo prof_section_info(ProfSection s) {
    if (s >= 0 && s < CP_COUNT)
        return k_sections[s];
    ProfSectionInfo none = { NULL, 0, 0 };
    return none;
}

/* X position of teapot i in a centered row of TEAPOTS_PER_ROW. */
static float teapot_x(int i) {
    return ((float)i - (float)(TEAPOTS_PER_ROW - 1) * 0.5f) * 2.0f;
}

/* Position teapot i in the given row and leave the modelview ready to draw. */
static void place_teapot(int i, float row_y) {
    glLoadIdentity();
    glTranslatef(teapot_x(i), row_y, SCENE_Z);
    glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
    glRotatef(g_spin, 0.0f, 1.0f, 0.0f);
}

static void set_row_color(float r, float g, float b) {
    GLfloat diffuse[4] = { r, g, b, 1.0f };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, diffuse);
}

static void ensure_lists(void) {
    if (g_lists_ready) return;
    g_reuse_list = glGenLists(1);
    glNewList(g_reuse_list, GL_COMPILE);
    glutSolidTeapot(TEAPOT_SIZE);
    glEndList();
    g_recompile_list = glGenLists(1);   /* contents (re)compiled each frame */
    g_lists_ready = 1;
}

static void display_func(void) {
    ensure_lists();
    prof_frame_tick();
    prof_begin(CP_FRAME_TOTAL);

    glClearColor(0.10f, 0.11f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
    GLfloat light_pos[4] = { 2.0f, 3.0f, 4.0f, 0.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    glMatrixMode(GL_MODELVIEW);

    /* Row 1 — immediate mode. */
    set_row_color(0.55f, 0.80f, 0.65f);
    prof_begin(CP_IMMEDIATE);
    for (int i = 0; i < TEAPOTS_PER_ROW; i++) {
        place_teapot(i, 2.4f);
        glutSolidTeapot(TEAPOT_SIZE);
    }
    prof_end(CP_IMMEDIATE);

    /* Row 2 — display list compiled once at startup, reused every frame. */
    set_row_color(0.55f, 0.70f, 0.95f);
    prof_begin(CP_REUSE);
    prof_begin(CP_REUSE_CALL);
    for (int i = 0; i < TEAPOTS_PER_ROW; i++) {
        place_teapot(i, 0.0f);
        glCallList(g_reuse_list);
    }
    prof_end(CP_REUSE_CALL);
    prof_end(CP_REUSE);

    /* Row 3 — display list rebuilt this frame, then called. */
    set_row_color(0.95f, 0.72f, 0.45f);
    prof_begin(CP_RECOMPILE);
    prof_begin(CP_RECOMPILE_COMPILE);
    glNewList(g_recompile_list, GL_COMPILE);
    glutSolidTeapot(TEAPOT_SIZE);
    glEndList();
    prof_end(CP_RECOMPILE_COMPILE);
    prof_begin(CP_RECOMPILE_CALL);
    for (int i = 0; i < TEAPOTS_PER_ROW; i++) {
        place_teapot(i, -2.4f);
        glCallList(g_recompile_list);
    }
    prof_end(CP_RECOMPILE_CALL);
    prof_end(CP_RECOMPILE);

    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);

    /* CPU profile panel overlay — anchored top-right (gl2d is y-up), with
     * the section-histogram panel stacked directly beneath it. Two of the
     * three sections above are display-list paths whose per-frame cost is
     * near-constant while the immediate-mode row's is not, so the overlaid
     * histograms show the spread the panel's single EMA column hides. */
    prof_begin(CP_PANEL);
    UiProfilePanelView view;
    view.window_w = g_window_w;
    view.window_h = g_window_h;
    view.mode     = g_mode;
    view.collapsed_sections = 0;
    view.panel_x  = g_window_w - ui_profile_panel_width()      - 16;
    view.panel_y  = g_window_h - ui_profile_panel_height(g_mode) - 16;
    ui_profile_panel_render(&view);

    UiHistogramPanelView hist;
    hist.window_w = g_window_w;
    hist.window_h = g_window_h;
    hist.visible  = (g_mode == PROFILE_PANEL_HISTOGRAM);
    hist.panel_x  = g_window_w - ui_histogram_panel_width() - 16;
    hist.panel_y  = view.panel_y - ui_histogram_panel_height() - 8;
    ui_histogram_panel_render(&hist);
    prof_end(CP_PANEL);

    prof_end(CP_FRAME_TOTAL);
    glutSwapBuffers();
}

static void reshape_func(int w, int h) {
    if (h < 1) h = 1;
    g_window_w = w;
    g_window_h = h;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)w / (double)h, 0.5, 60.0);
    glMatrixMode(GL_MODELVIEW);
}

static void idle_func(void) {
    g_spin += 0.4f;
    if (g_spin >= 360.0f) g_spin -= 360.0f;
    glutPostRedisplay();
}

static void keyboard_func(unsigned char key, int x, int y) {
    (void)x; (void)y;
    switch (key) {
    case 'd': case 'D':
        g_mode = (g_mode == PROFILE_PANEL_HISTOGRAM)
               ? PROFILE_PANEL_SECTIONS : PROFILE_PANEL_HISTOGRAM;
        break;
    case 27: case 'q': case 'Q': exit(0);
    default: break;
    }
    glutPostRedisplay();
}

int main(int argc, char **argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(g_window_w, g_window_h);
    glutCreateWindow("cpuprof_demo — display-list benchmark");

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutKeyboardFunc(keyboard_func);
    glutIdleFunc(idle_func);

    printf("cpuprof_demo: immediate vs display-list reuse vs recompile/frame\n");
    printf("  d=toggle sections/histogram   q=quit\n");
    glutMainLoop();
    return 0;
}
