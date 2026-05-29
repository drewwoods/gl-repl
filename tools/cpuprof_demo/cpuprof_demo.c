/*
 * tools/cpuprof_demo/cpuprof_demo.c — standalone driver for the CPU
 * profiling subsystem.
 *
 * Isolation proof: links ONLY src/support/cpuprof.c (the pure wall-time
 * sampler) + src/ui/support/cpuprof.c (the overlay panel) + ui/core theme.
 * No src/ui/app, src/app, src/repl, or src/editor — see CPUPROF_DEMO_DEP_SRCS
 * in the Makefile and check-cpuprof-demo-isolation. Sibling of memprof_demo.
 *
 * What it shows: a spinning teapot whose per-frame render is bracketed by
 * prof_begin/prof_end sections, plus the live CPU-profile panel (per-section
 * last/avg µs, green/yellow/red budget colors). 'w' injects extra work into
 * the scene section so a row visibly spikes; 'd' toggles the detailed view.
 *
 * The panel renderer is snapshot-free: it consumes a UiProfilePanelView whose
 * panel_x/panel_y this demo bakes (top-right), exactly where the full app's
 * controller bakes the stacked anchor.
 */
#include "gl_includes.h"

#include "ui/support/cpuprof.h"   /* UiProfilePanelView, ui_profile_panel_* */
#include "support/cpuprof.h"      /* prof_begin/_end/_frame_tick + ProfSection */

#include <stdio.h>
#include <stdlib.h>

static int   g_window_w = 900;
static int   g_window_h = 640;
static float g_spin     = 0.0f;
static UiProfilePanelMode g_mode = PROFILE_PANEL_ON;
static int   g_extra_work = 0;    /* 'w' injects busywork into the scene section */

/* Burn ~n µs of CPU so a profiled section shows a non-trivial time. */
static volatile double g_sink = 0.0;
static void burn_work(int iters) {
    double acc = 0.0;
    for (int i = 0; i < iters; i++) acc += (double)i * 1.000001;
    g_sink += acc;
}

static void display_func(void) {
    prof_frame_tick();
    prof_begin(PROF_FRAME_TOTAL);

    glClearColor(0.10f, 0.11f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    prof_begin(PROF_SCENE_3D);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
    GLfloat light_pos[4] = { 2.0f, 3.0f, 4.0f, 0.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    GLfloat diffuse[4] = { 0.55f, 0.80f, 0.65f, 1.0f };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, diffuse);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -5.0f);
    glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
    glRotatef(g_spin, 0.0f, 1.0f, 0.0f);
    glutSolidTeapot(1.3);
    if (g_extra_work) burn_work(4000000);   /* spikes the Scene 3D row */
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    prof_end(PROF_SCENE_3D);

    /* CPU profile panel overlay — anchored top-right (gl2d is y-up). */
    prof_begin(PROF_PROFILE_PANEL);
    UiProfilePanelView view;
    view.window_w = g_window_w;
    view.window_h = g_window_h;
    view.mode     = g_mode;
    view.panel_x  = g_window_w - ui_profile_panel_width()      - 16;
    view.panel_y  = g_window_h - ui_profile_panel_height(g_mode) - 16;
    ui_profile_panel_render(&view);
    prof_end(PROF_PROFILE_PANEL);

    prof_end(PROF_FRAME_TOTAL);
    glutSwapBuffers();
}

static void reshape_func(int w, int h) {
    if (h < 1) h = 1;
    g_window_w = w;
    g_window_h = h;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)w / (double)h, 0.5, 50.0);
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
        g_mode = (g_mode == PROFILE_PANEL_DETAILS) ? PROFILE_PANEL_ON
                                                   : PROFILE_PANEL_DETAILS;
        break;
    case 'w': case 'W': g_extra_work = !g_extra_work; break;
    case 27: case 'q': case 'Q': exit(0);
    default: break;
    }
    glutPostRedisplay();
}

int main(int argc, char **argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(g_window_w, g_window_h);
    glutCreateWindow("cpuprof_demo");

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutKeyboardFunc(keyboard_func);
    glutIdleFunc(idle_func);

    printf("cpuprof_demo: w=toggle extra scene work  d=toggle details  q=quit\n");
    glutMainLoop();
    return 0;
}
