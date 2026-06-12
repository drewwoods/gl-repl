/*
 * tools/variable_panel_demo/variable_panel_demo.c — standalone driver for the
 * variable-panel subsystem.
 *
 * Isolation proof: links ONLY the src/subsystems/variable_panel peer (state +
 * drag math) + src/ui/subsystems/variable_panel.c (the renderer) + ui/core
 * theme. No src/ui/app, no src/app, no src/repl, no src/editor — see
 * VARIABLE_PANEL_DEMO_DEP_SRCS in the Makefile and
 * check-variable-panel-demo-isolation.
 *
 * What it shows: a torus whose inner/outer radius and tilt are driven by
 * three named float "variables". The floating slider panel renders those
 * variables; drag a row (left = linear, right = log) and the torus reshapes
 * live. This exercises the two decoupling seams the subsystem grew:
 *   - UiVariablePanelView: the demo builds the narrow render view directly
 *     (full-window scene rect, no statusbar), never UiRenderSnapshot.
 *   - VariablePanelValueSource: the demo installs an in-memory reader so the
 *     drag handlers read name+value without the REPL eval table.
 */
#include "gl_includes.h"

#include "ui/subsystems/variable_panel.h"                 /* UiVariablePanelView, render/hit */
#include "subsystems/variable_panel/variable_panel_state.h" /* visibility + drag handlers */
#include "subsystems/variable_panel/variable_panel_drag.h"  /* value source + value change */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Window state -------------------------------------------------------- */
static int   g_window_w = 900;
static int   g_window_h = 640;
static float g_auto_spin = 0.0f;   /* gentle view rotation, not a variable */

/* --- The demo's "variables" --------------------------------------------- */
typedef struct { char name[UI_VARIABLE_NAME_MAX]; float value; } DemoVar;
static DemoVar g_vars[] = {
    { "inner", 0.20f },
    { "outer", 0.60f },
    { "tilt",  25.0f },
};
enum { DEMO_VAR_COUNT = (int)(sizeof(g_vars) / sizeof(g_vars[0])) };

static float demo_var(const char *name) {
    for (int i = 0; i < DEMO_VAR_COUNT; i++)
        if (strcmp(g_vars[i].name, name) == 0) return g_vars[i].value;
    return 0.0f;
}

/* --- Value source: drag handlers read name+value by row from g_vars ------ */
static int demo_var_read_row(int row, char *name_out, int name_cap, float *value_out) {
    if (row < 0 || row >= DEMO_VAR_COUNT) return 0;
    snprintf(name_out, (size_t)name_cap, "%s", g_vars[row].name);
    *value_out = g_vars[row].value;
    return 1;
}
static const VariablePanelValueSource g_demo_value_source = { demo_var_read_row };

/* Build the narrow render view directly from window + demo state. The full
 * app's controller does this from a UiRenderSnapshot via ui/app; the demo
 * proves the renderer needs none of that. */
static UiVariablePanelView demo_build_view(void) {
    static UiVariable rows[DEMO_VAR_COUNT];
    for (int i = 0; i < DEMO_VAR_COUNT; i++) {
        snprintf(rows[i].name, sizeof(rows[i].name), "%s", g_vars[i].name);
        rows[i].value = &g_vars[i].value;
    }
    UiVariablePanelView v;
    v.visible        = variable_panel_visible();
    v.window_w       = g_window_w;
    v.window_h       = g_window_h;
    /* Standalone placement: pin to the window's bottom-right. The full app
     * resolves panel_x/panel_y through its overlay layout engine
     * (src/ui/app/overlay_layout.c); the renderer just draws at the view's
     * position either way. */
    {
        int pw, ph;
        ui_variable_panel_size(DEMO_VAR_COUNT, &pw, &ph);
        v.panel_x = g_window_w - pw - 8;
        v.panel_y = 8;
    }
    v.vars            = rows;
    v.var_count       = DEMO_VAR_COUNT;
    v.drag_active_var = variable_panel_drag_active_var();
    v.drag_log_mode   = variable_panel_drag_log_mode();
    return v;
}

/* --- GLUT callbacks ------------------------------------------------------ */
static void display_func(void) {
    glClearColor(0.10f, 0.11f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Torus driven by the slider variables (radii clamped positive). */
    float inner = demo_var("inner"); if (inner < 0.01f) inner = 0.01f;
    float outer = demo_var("outer"); if (outer < 0.02f) outer = 0.02f;
    float tilt  = demo_var("tilt");

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
    GLfloat light_pos[4] = { 2.0f, 3.0f, 4.0f, 0.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    GLfloat diffuse[4] = { 0.85f, 0.70f, 0.45f, 1.0f };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, diffuse);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -4.0f);
    glRotatef(tilt,        1.0f, 0.0f, 0.0f);
    glRotatef(g_auto_spin, 0.0f, 1.0f, 0.0f);
    glutSolidTorus(inner, outer, 24, 40);

    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);

    /* Variable slider panel overlay. */
    UiVariablePanelView view = demo_build_view();
    ui_variable_panel_render(&view);

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
    g_auto_spin += 0.3f;
    if (g_auto_spin >= 360.0f) g_auto_spin -= 360.0f;
    glutPostRedisplay();
}

static void apply_change(const VariablePanelValueChange *chg) {
    for (int i = 0; i < DEMO_VAR_COUNT; i++)
        if (strcmp(g_vars[i].name, chg->name) == 0) { g_vars[i].value = chg->value; return; }
}

static void mouse_func(int button, int state, int x, int y) {
    if (state == GLUT_DOWN &&
        (button == GLUT_LEFT_BUTTON || button == GLUT_RIGHT_BUTTON)) {
        UiVariablePanelView view = demo_build_view();
        UiHit hit = ui_variable_panel_hit_test(&view, x, y);
        if (hit.kind == UI_HIT_VARIABLE_SLIDER) {
            int log_mode = (button == GLUT_RIGHT_BUTTON) ? 1 : 0;
            variable_panel_handle_drag_begin(hit.item_idx, log_mode, x);
        }
    } else if (state == GLUT_UP) {
        variable_panel_handle_drag_reset();
    }
    glutPostRedisplay();
}

static void motion_func(int x, int y) {
    (void)y;
    VariablePanelValueChange chg;
    if (variable_panel_handle_drag_motion(x, &chg))
        apply_change(&chg);
    glutPostRedisplay();
}

static void keyboard_func(unsigned char key, int x, int y) {
    (void)x; (void)y;
    switch (key) {
    case 'v': case 'V':
        variable_panel_set_visible(!variable_panel_visible());
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
    glutCreateWindow("variable_panel_demo");

    variable_panel_state_reset();
    variable_panel_set_visible(1);
    variable_panel_install_value_source(&g_demo_value_source);

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutMouseFunc(mouse_func);
    glutMotionFunc(motion_func);
    glutKeyboardFunc(keyboard_func);
    glutIdleFunc(idle_func);

    printf("variable_panel_demo: drag a slider row (left=linear, right=log) to\n");
    printf("  reshape the torus. v=toggle panel  q=quit\n");
    glutMainLoop();
    return 0;
}
