/*
 * tools/memprof_demo/memprof_demo.c - standalone driver for the memory
 * profiling subsystem.
 *
 * Isolation proof: links ONLY src/support/memprof.c (the pure memory
 * sampler) + src/ui/support/memprof.c (the overlay panel) + src/ui/core
 * (theme). No src/ui/app, no src/app, no src/repl, no src/editor - see
 * MEMPROF_DEMO_DEP_SRCS in the Makefile and check-memprof-demo-isolation.sh.
 *
 * What it shows: a spinning teapot as filler scene plus the live memory
 * panel overlay (current / baseline / delta / limit + a time-anchored graph).
 * Press 'a' to allocate a ~4 MB block, 'f' to free the newest, 'c' to free
 * all - and watch the memory signal and graph respond.
 *
 * The panel renderer is snapshot-free: it consumes a UiMemoryPanelView
 * whose panel_x/panel_y this demo bakes directly (top-right corner),
 * exactly where the full app's controller bakes the stacked anchor.
 *
 * Timebase: the panel drives off memprof's own monotonic clock via
 * memprof_frame_tick(), so the graph's X axis (-85m .. now, spanning
 * MEMPROF_HISTORY_CAP * MEMPROF_PUSH_INTERVAL_S seconds) reflects real
 * wall-clock time - a sample lands every MEMPROF_PUSH_INTERVAL_S (~5s).
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
    /* Touch every page so the RSS (resident) figure actually grows -
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

/* Capture hook: GLR_DEMO_ALLOC=<n> allocates n blocks for a headless frame
 * grab, standing in for n presses of 'a'. It waits for the first sample push
 * because memprof defers its baseline to that moment (see memprof_init_at):
 * allocating before it would fold the blocks INTO the baseline and report a
 * delta of zero. Firing just after it instead yields a real init/delta pair
 * and a visible step in the graph. Names stay demo_*-prefixed:
 * check-memprof-demo-isolation greps nm for repl_/editor_/glr_ symbols. */
static int g_demo_stage_blocks = 0;   /* 0 = hook off */
static int g_demo_stage_done   = 0;

static void demo_stage_alloc_init(void) {
    const char *env = getenv("GLR_DEMO_ALLOC");
    if (env && *env) g_demo_stage_blocks = atoi(env);
}

static void demo_stage_alloc_tick(void) {
    if (g_demo_stage_done || g_demo_stage_blocks <= 0) return;
    if (memprof_history_count() <= 0) return;   /* baseline not captured yet */
    for (int i = 0; i < g_demo_stage_blocks; i++) alloc_block();
    g_demo_stage_done = 1;
}

/* Keep the staged blocks in the resident working set. A page written once and
 * never touched again is a candidate for eviction (on macOS the memory
 * compressor takes it within a few seconds), which walks RSS - the number the
 * panel plots - back to the baseline and erases the step this hook exists to
 * stage. One byte per page per frame is ~12k writes for the default 12
 * blocks. Runs only for hook-staged blocks: interactive 'a' presses keep
 * their untouched-allocation behaviour. */
static void demo_stage_keep_warm(void) {
    if (!g_demo_stage_done) return;
    for (int b = 0; b < g_block_count; b++) {
        unsigned char *p = g_blocks[b];
        size_t off;
        for (off = 0; off < (size_t)BLOCK_BYTES; off += 4096) p[off]++;
    }
}

/* --- GLUT callbacks ------------------------------------------------------ */
static void display_func(void) {
    memprof_frame_tick();           /* real monotonic clock -> truthful X axis */
    demo_stage_alloc_tick();
    demo_stage_keep_warm();

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

    /* Memory panel overlay - anchored top-right (gl2d is y-up). */
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
    demo_stage_alloc_init();

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutKeyboardFunc(keyboard_func);
    glutIdleFunc(idle_func);

    printf("memprof_demo: a=alloc 4MB  f=free  c=free all  m=toggle panel  q=quit\n");
    printf("  Current/delta text updates every frame; the graph samples every ~5s.\n");
    glutMainLoop();
    return 0;
}
