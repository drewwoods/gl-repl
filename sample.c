#include "sample.h"

/* Pull in stb-style implementations in this translation unit only */
#define PROCEDURAL_ROCK_IMPLEMENTATION
#include "procedural_rock.h"
#define PROCEDURAL_SHAPES_IMPLEMENTATION
#include "procedural_shapes.h"
#define GL_MATRIX_DEBUG_IMPLEMENTATION
#include "gl_matrix_debug.h"
#include "repl_core.h"

static void display_func(void) {
    repl_display_func();
}

static void reshape_func(int w, int h) {
    repl_reshape_func(w, h);
}

static void keyboard_func(unsigned char key, int x, int y) {
    repl_keyboard_func(key, x, y);
}

static void special_func(int key, int x, int y) {
    repl_special_func(key, x, y);
}

static void mouse_func(int button, int state, int x, int y) {
    repl_mouse_func(button, state, x, y);
}

static void motion_func(int x, int y) {
    repl_motion_func(x, y);
}

static void passive_motion_func(int x, int y) {
    repl_passive_motion_func(x, y);
}

#ifndef USE_GLUT
static void mousewheel_func(int wheel, int direction, int x, int y) {
    repl_mousewheel_func(wheel, direction, x, y);
}
#endif

static void timer_func(int value) {
    repl_timer_func(value);
}

int main(int argc, char **argv) {
    const char *input_file = NULL;
    int dump_code = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--noaccum") == 0)
            g_use_accum = 0;
        else if (strcmp(argv[i], "--dump-code") == 0)
            dump_code = 1;
        else if (!input_file)
            input_file = argv[i];
    }

    if (dump_code) {
        init_predef_vars();
        for (int i = 0; i < g_num_predef_vars; i++)
            if (strcmp(g_predef_vars[i].name, "t") == 0) { g_t_var_idx = i; break; }
        repl_load_initial_commands(input_file);
        repl_debug_dump_editor(stdout);
        return 0;
    }

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH | GLUT_MULTISAMPLE |
                        (g_use_accum ? GLUT_ACCUM : 0));
    glutInitWindowSize(g_win_w, g_win_h);
    glutCreateWindow("OpenGL REPL - Display List Dynamic Rendering");

    repl_init_gl();
    init_predef_vars();
    for (int i = 0; i < g_num_predef_vars; i++)
        if (strcmp(g_predef_vars[i].name, "t") == 0) { g_t_var_idx = i; break; }
    repl_load_initial_commands(input_file);

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutKeyboardFunc(keyboard_func);
    glutSpecialFunc(special_func);
    glutMouseFunc(mouse_func);
    glutMotionFunc(motion_func);
    glutPassiveMotionFunc(passive_motion_func);
#ifndef USE_GLUT
    glutMouseWheelFunc(mousewheel_func);
#endif
    glutTimerFunc(16, timer_func, 0);

    glutMainLoop();
    return 0;
}
