/*
 * tools/color_picker_demo/color_picker_demo.c - standalone driver for the
 * color-picker subsystem.
 *
 * Isolation proof: links ONLY the src/subsystems/color_picker peer (state +
 * lifecycle + input) + src/ui/subsystems/color_picker.c (renderer/hit-test) +
 * ui/core theme. No src/ui/app, no src/app, no src/repl, no src/editor - see
 * COLOR_PICKER_DEMO_DEP_SRCS in the Makefile and
 * check-color-picker-demo-isolation.
 *
 * What it shows: a row of GLUT solid shapes, each with its own RGBA color.
 * Click a shape to open the floating picker on it; drag the SV square / hue
 * bar / alpha bar and the shape recolors live. This exercises the decoupling
 * seam the subsystem grew - a ColorPickerHostBridge the demo installs over an
 * in-memory color array, standing in for the REPL document + editor commit
 * pipeline + ui/app geometry the full app wires up.
 */
#include "gl_includes.h"

#include "subsystems/color_picker/color_picker_state.h"  /* peer: host bridge, lifecycle, input */
#include "ui/subsystems/color_picker.h"                  /* ui_color_picker_render */

#include <stdio.h>
#include <stdlib.h>

/* --- Window state -------------------------------------------------------- */
static int g_window_w = 960;
static int g_window_h = 600;

/* --- The "document": one editable RGBA color per shape ------------------- */
typedef struct { const char *name; float rgba[4]; } DemoShape;
static DemoShape g_shapes[] = {
    { "cube",   { 0.85f, 0.25f, 0.25f, 1.0f } },
    { "sphere", { 0.25f, 0.75f, 0.35f, 1.0f } },
    { "torus",  { 0.30f, 0.45f, 0.90f, 1.0f } },
    { "cone",   { 0.90f, 0.75f, 0.20f, 1.0f } },
    { "teapot", { 0.70f, 0.40f, 0.85f, 1.0f } },
};
enum { SHAPE_COUNT = (int)(sizeof(g_shapes) / sizeof(g_shapes[0])) };

/* --- Host bridge: read/write color by shape index, plus geometry --------- */
static int demo_cp_read(int idx, float *r, float *g, float *b, float *a,
                        int *has_alpha, float *value_max) {
    if (idx < 0 || idx >= SHAPE_COUNT) return 0;
    if (r) *r = g_shapes[idx].rgba[0];
    if (g) *g = g_shapes[idx].rgba[1];
    if (b) *b = g_shapes[idx].rgba[2];
    if (a) *a = g_shapes[idx].rgba[3];
    if (has_alpha) *has_alpha = 1;     /* the demo shapes are all RGBA */
    if (value_max) *value_max = 1.0f;
    return 1;
}
static int demo_cp_write(int idx, float r, float g, float b, float a,
                         int capture_undo) {
    (void)capture_undo;                /* no undo ring in the demo */
    if (idx < 0 || idx >= SHAPE_COUNT) return 0;
    g_shapes[idx].rgba[0] = r;
    g_shapes[idx].rgba[1] = g;
    g_shapes[idx].rgba[2] = b;
    g_shapes[idx].rgba[3] = a;
    return 1;
}
static void demo_cp_viewport(int *w, int *h) {
    if (w) *w = g_window_w;
    if (h) *h = g_window_h;
}
static void demo_cp_code_panel_rect(int *x, int *w) {
    if (x) *x = 0;                     /* no code panel: popup anchors at left */
    if (w) *w = 0;
}
static const ColorPickerHostBridge g_demo_host = {
    demo_cp_read, demo_cp_write, demo_cp_viewport, demo_cp_code_panel_rect,
};

/* World x for shape i, laid out left-to-right and centered. */
static float shape_world_x(int i) {
    return ((float)i - (float)(SHAPE_COUNT - 1) * 0.5f) * 2.4f;
}

static void draw_shape(int i) {
    switch (i) {
    case 0: glutSolidCube(0.9);                 break;
    case 1: glutSolidSphere(0.6, 24, 18);       break;
    case 2: glutSolidTorus(0.2, 0.5, 16, 28);   break;
    case 3: glPushMatrix(); glTranslatef(0, -0.5f, 0);
            glRotatef(-90, 1, 0, 0);
            glutSolidCone(0.55, 1.1, 22, 4); glPopMatrix(); break;
    default: glutSolidTeapot(0.55);             break;
    }
}

/* Map a window x to a shape column. */
static int shape_at_x(int x) {
    if (g_window_w <= 0) return 0;
    int col = x * SHAPE_COUNT / g_window_w;
    if (col < 0) col = 0;
    if (col >= SHAPE_COUNT) col = SHAPE_COUNT - 1;
    return col;
}

/* --- GLUT callbacks ------------------------------------------------------ */
static void display_func(void) {
    glClearColor(0.11f, 0.12f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    GLfloat light_pos[4] = { 2.0f, 3.0f, 4.0f, 0.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);

    int active = color_picker_active_line();
    for (int i = 0; i < SHAPE_COUNT; i++) {
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(shape_world_x(i), 0.0f, -9.0f);
        glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
        /* Lift the active shape slightly so the open target is obvious. */
        if (i == active) glTranslatef(0.0f, 0.35f, 0.0f);
        glColor4fv(g_shapes[i].rgba);
        draw_shape(i);
    }

    glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);

    /* Floating picker overlay (no-op when closed). */
    ColorPickerView view = color_picker_view();
    ui_color_picker_render(&view, g_window_w, g_window_h);

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

static void mouse_func(int button, int state, int x, int y) {
    if (button == GLUT_LEFT_BUTTON && state == GLUT_DOWN) {
        /* If the picker is open, let it claim slider drags / dismissal first. */
        if (color_picker_active_line() >= 0) {
            ColorPickerInputResult r = color_picker_handle_press(x, y);
            if (r.consumed) { glutPostRedisplay(); return; }
            /* r.closed: fell outside the picker - fall through to (re)open
             * on whatever shape column was clicked. */
        }
        color_picker_start(shape_at_x(x), y);
    } else if (state == GLUT_UP) {
        color_picker_handle_release();
    }
    glutPostRedisplay();
}

static void motion_func(int x, int y) {
    color_picker_handle_motion(x, y);
    glutPostRedisplay();
}

static void keyboard_func(unsigned char key, int x, int y) {
    (void)x; (void)y;
    switch (key) {
    case 27: case 'q': case 'Q': exit(0);
    default: break;
    }
    glutPostRedisplay();
}

/* Capture hook: GLR_DEMO_OPEN_PICKER=<shape index> opens the picker on that
 * shape at startup, so a headless frame grab shows the popup without a
 * synthetic click. Mirrors the app's GLR_OPEN_COLOR_PICKER=<line>. Names stay
 * demo_*-prefixed: check-color-picker-demo-isolation greps nm for
 * repl_/editor_/glr_ symbols. */
static void demo_stage_open_picker(void) {
    const char *env = getenv("GLR_DEMO_OPEN_PICKER");
    if (!env || !*env) return;
    int idx = atoi(env);
    if (idx < 0 || idx >= SHAPE_COUNT) return;
    /* Anchor as a click on the shape row would: vertically centered. */
    color_picker_start(idx, g_window_h / 2);
}

int main(int argc, char **argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(g_window_w, g_window_h);
    glutCreateWindow("color_picker_demo");

    color_picker_state_reset();
    color_picker_install_host(&g_demo_host);
    demo_stage_open_picker();

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutMouseFunc(mouse_func);
    glutMotionFunc(motion_func);
    glutKeyboardFunc(keyboard_func);

    printf("color_picker_demo: click a shape to open the picker; drag the\n");
    printf("  SV square / hue bar / alpha bar to recolor it. q=quit\n");
    glutMainLoop();
    return 0;
}
