/*
 * tools/memprof_demo/memprof_demo.c — standalone driver for the memory
 * profiling subsystem.
 *
 * Isolation proof: links ONLY src/support/memprof.c (the pure memory
 * sampler) + src/ui/support/memprof.c (the overlay panel) + src/ui/core
 * (theme). No src/ui/app, no src/app, no src/repl, no src/editor — see
 * MEMPROF_DEMO_DEP_SRCS in the Makefile and check-memprof-demo-isolation.sh.
 *
 * What it shows: a spinning teapot as filler scene plus the live memory
 * panel overlay (current / baseline / delta / limit + a time-anchored graph).
 * Press 'a' to allocate a ~4 MB block, 'f' to free the newest, 'c' to free
 * all — and watch the memory signal and graph respond.
 *
 * The panel renderer is snapshot-free: it consumes a UiMemoryPanelView
 * whose panel_x/panel_y this demo bakes directly (top-right corner),
 * exactly where the full app's controller bakes the stacked anchor.
 *
 * Timebase: the panel drives off memprof's own monotonic clock via
 * memprof_frame_tick(), so the graph's X axis (-85m .. now, spanning
 * MEMPROF_HISTORY_CAP * MEMPROF_PUSH_INTERVAL_S seconds) reflects real
 * wall-clock time — a sample lands every MEMPROF_PUSH_INTERVAL_S (~5s).
 * The current/baseline/delta text rows refresh every frame, so an
 * alloc/free shows in the numbers immediately and on the graph at the
 * next sample tick.
 */
#include "gl_includes.h"

#include "ui/support/memprof.h"   /* UiMemoryPanelView, ui_memory_panel_* */
#include "support/memprof.h"      /* memprof_init, memprof_frame_tick */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Window + scene state ------------------------------------------------ */
static int    g_window_w = 900;
static int    g_window_h = 640;
static float  g_spin     = 0.0f;
static int    g_panel_on = 1;

/* --- Allocation playground ---------------------------------------------- */
#define BLOCK_BYTES (4 * 1024 * 1024)
#define MAX_BLOCKS  256
static unsigned char *g_blocks[MAX_BLOCKS];
static int            g_block_count = 0;

static void alloc_block(void) {
    if (g_block_count >= MAX_BLOCKS) return;
    unsigned char *p = (unsigned char *)malloc(BLOCK_BYTES);
    if (!p) return;
    /* Touch every page so the RSS (resident) figure actually grows —
     * malloc alone may not fault the pages in. */
    memset(p, 0xAB, BLOCK_BYTES);
    g_blocks[g_block_count++] = p;
}

static void free_block(void) {
    if (g_block_count <= 0) return;
    free(g_blocks[--g_block_count]);
    g_blocks[g_block_count] = NULL;
}

static void free_all(void) {
    while (g_block_count > 0) free_block();
}

/* --- GLUT callbacks ------------------------------------------------------ */
static void display_func(void) {
    memprof_frame_tick();           /* real monotonic clock → truthful X axis */

    glClearColor(0.09f, 0.10f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Filler scene: a slowly spinning lit teapot. */
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
    GLfloat light_pos[4] = { 2.0f, 3.0f, 4.0f, 0.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    GLfloat diffuse[4] = { 0.55f, 0.70f, 0.95f, 1.0f };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, diffuse);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -5.0f);
    glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
    glRotatef(g_spin, 0.0f, 1.0f, 0.0f);
    glutSolidTeapot(1.3);

    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);

    /* Memory panel overlay — anchored top-right (gl2d is y-up). */
    UiMemoryPanelView view;
    view.window_w = g_window_w;
    view.window_h = g_window_h;
    view.mode     = g_panel_on ? MEMORY_PANEL_ON : MEMORY_PANEL_OFF;
    view.panel_x  = g_window_w - ui_memory_panel_width()  - 16;
    view.panel_y  = g_window_h - ui_memory_panel_height() - 16;
    ui_memory_panel_render(&view);

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
    case 'a': case 'A': alloc_block(); break;
    case 'f': case 'F': free_block();  break;
    case 'c': case 'C': free_all();    break;
    case 'm': case 'M': g_panel_on = !g_panel_on; break;
    case 27: case 'q': case 'Q': free_all(); exit(0);
    default: break;
    }
    glutPostRedisplay();
}

int main(int argc, char **argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(g_window_w, g_window_h);
    glutCreateWindow("memprof_demo");

    memprof_init();

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutKeyboardFunc(keyboard_func);
    glutIdleFunc(idle_func);

    printf("memprof_demo: a=alloc 4MB  f=free  c=free all  m=toggle panel  q=quit\n");
    printf("  Current/delta text updates every frame; the graph samples every ~5s.\n");
    glutMainLoop();
    return 0;
}
