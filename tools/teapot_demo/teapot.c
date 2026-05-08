/*
 * tools/teapot_demo/teapot.c — independent binary that drives the scene
 * module with a non-REPL geometry callback (a single glutSolidTeapot).
 *
 * Demonstrates that scene/ has no hard dependency on the REPL editor or
 * controller. The geometry callback receives a FlatProgramView purely to
 * match the SceneExecuteProgramFn contract; this demo ignores it.
 *
 * App pseudo-code:
 *   render() { scene_apply_camera(...); scene_render_3d_scene(&cfg); }
 *   my_scene(...) { glutSolidTeapot(1.0); }
 *   build_cfg() { cfg.execute_fn = my_scene; cfg.cam_* = ...; }
 */
#include <gl_includes.h>
#include "scene/render.h"
#include "scene/render_types.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int   g_window_w = 800;
static int   g_window_h = 600;
static float g_anim_t   = 0.0f;

static void my_scene_execute(float alpha,
                             int   skip_geom_before_pc,
                             int   flat_cmd_count,
                             FlatProgramView program,
                             void *user_data) {
    (void)alpha;
    (void)skip_geom_before_pc;
    (void)flat_cmd_count;
    (void)program;
    (void)user_data;

    glColor3f(0.85f, 0.70f, 0.40f);
    glutSolidTeapot(1.0f);
}

static void seed_light0(SceneLight *l) {
    l->id       = GL_LIGHT0;
    l->enabled  = 1;
    l->pos[0]   = 5.0f; l->pos[1] = 6.0f; l->pos[2] = 4.0f; l->pos[3] = 1.0f;
    l->diffuse[0]  = 1.0f; l->diffuse[1]  = 1.0f; l->diffuse[2]  = 1.0f; l->diffuse[3]  = 1.0f;
    l->ambient[0]  = 0.2f; l->ambient[1]  = 0.2f; l->ambient[2]  = 0.2f; l->ambient[3]  = 1.0f;
    l->specular[0] = 1.0f; l->specular[1] = 1.0f; l->specular[2] = 1.0f; l->specular[3] = 1.0f;
}

static void build_config(SceneRenderConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->execute_fn = my_scene_execute;
    cfg->anim_time  = g_anim_t;

    cfg->viewport_w = g_window_w;
    cfg->viewport_h = g_window_h;
    cfg->scene_x = 0;
    cfg->scene_y = 0;
    cfg->scene_w = g_window_w;
    cfg->scene_h = g_window_h;

    cfg->cam_dist = 5.0f;
    cfg->cam_rx   = 18.0f;
    cfg->cam_ry   = fmodf(g_anim_t * 25.0f, 360.0f);
    cfg->cam_tx   = 0.0f;
    cfg->cam_ty   = 0.0f;
    cfg->cam_tz   = 0.0f;
    cfg->cam_motion_glow = 0.0f;

    cfg->user_lighting_enabled = 1;
    seed_light0(&cfg->lights[0]);
    cfg->show_light_indicators = 0;

    cfg->backdrop_mode = 0;
    cfg->wireframe     = 0;

    cfg->grid_theme       = 0;
    cfg->grid_extent_idx  = 0;
    cfg->grid_major_idx   = 0;
    cfg->axes_theme       = 0;

    cfg->cursor_block_begin_idx  = -1;
    cfg->cursor_block_end_idx    = -1;
    cfg->cursor_block_source_line = -1;
    cfg->cursor_call_src_cmd_idx = -1;
    cfg->edit_line_idx           = -1;

    cfg->multisample_enabled = 1;
    cfg->line_smooth_enabled = 1;

    cfg->replaying        = 0;
    cfg->replay_has_fades = 0;
    cfg->alpha_scale      = 1.0f;
}

static void display_func(void) {
    SceneRenderConfig cfg;
    build_config(&cfg);

    scene_apply_camera(cfg.cam_rx, cfg.cam_ry, cfg.cam_dist,
                       cfg.cam_tx, cfg.cam_ty, cfg.cam_tz);
    scene_render_3d_scene(&cfg);

    glutSwapBuffers();
}

static void reshape_func(int w, int h) {
    g_window_w = w;
    g_window_h = h;
    glutPostRedisplay();
}

static void idle_func(void) {
    g_anim_t += 0.016f;
    glutPostRedisplay();
}

static void keyboard_func(unsigned char key, int x, int y) {
    (void)x; (void)y;
    if (key == 27 /* Esc */ || key == 'q')
        exit(0);
}

int main(int argc, char **argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH | GLUT_MULTISAMPLE);
    glutInitWindowSize(g_window_w, g_window_h);
    glutCreateWindow("scene-module teapot demo");

    glEnable(GL_DEPTH_TEST);
    scene_render_init_gl();

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutIdleFunc(idle_func);
    glutKeyboardFunc(keyboard_func);

    glutMainLoop();
    return 0;
}
