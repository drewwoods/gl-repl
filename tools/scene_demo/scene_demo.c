/*
 * tools/scene_demo/scene_demo.c — independent binary that drives the scene
 * module with a non-REPL geometry callback (a single glutSolidTeapot).
 *
 * Demonstrates that scene/ has no hard dependency on the REPL editor or
 * controller. Adds a small interactive shell:
 *
 *   - mouse drag (orbit / pan) + wheel (zoom)
 *   - V toggles perspective / orthographic projection with an eased blend
 *   - keyboard toggles for lighting, wireframe, grid / axes / backdrop
 *   - per-light enable on number keys
 *   - bitmap HUD with the current state and a help panel
 *
 * App pseudo-code:
 *   render() { scene_apply_camera(...); scene_render_3d_scene(&cfg); render_hud(); }
 *   my_scene(...) { glEnable(GL_LIGHTING); ...; glutSolidTeapot(1.0); }
 *   build_cfg() { cfg.execute_fn = my_scene; cfg.cam_* = ...; ... }
 */
#include "gl_includes.h"
#include "config.h"
#include "scene/render.h"
#include "scene/render_types.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Window + animation state ------------------------------------------ */

static int   g_window_w = 960;
static int   g_window_h = 720;
static float g_anim_t   = 0.0f;

/* --- Camera ------------------------------------------------------------ */

static float g_cam_rx   = 22.0f;
static float g_cam_ry   = 30.0f;
static float g_cam_dist = 5.0f;
static float g_cam_tx   = 0.0f;
static float g_cam_ty   = 0.0f;
static float g_cam_tz   = 0.0f;
static int   g_auto_rotate = 1;

/* Mouse drag state. button=-1 means not dragging. */
static int g_drag_button = -1;
static int g_drag_x      = 0;
static int g_drag_y      = 0;

/* --- Display config ---------------------------------------------------- */

static int g_lighting_on    = 1;                 /* GL_LIGHTING global enable */
static int g_lights_on[MAX_LIGHTS] = { 1, 0, 0, 0 };
static int g_show_indicators = 1;
static int g_wireframe       = 0;
static int g_grid_theme      = 1;
static int g_axes_theme      = 1;
static SceneBackdropMode g_backdrop_mode = SCENE_BACKDROP_OFF;
static float g_projection_mix = 1.0f;
static float g_projection_mix_target = 1.0f;
static int g_show_hud        = 1;
static int g_show_help       = 0;

static float smoothstep01(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static void step_projection_mix(float dt) {
    float step;

    if (GLR_VIEW_PROJECTION_TRANSITION_SECS <= 0.0f) {
        g_projection_mix = g_projection_mix_target;
        return;
    }

    step = dt / GLR_VIEW_PROJECTION_TRANSITION_SECS;
    if (g_projection_mix < g_projection_mix_target) {
        g_projection_mix += step;
        if (g_projection_mix > g_projection_mix_target)
            g_projection_mix = g_projection_mix_target;
    } else if (g_projection_mix > g_projection_mix_target) {
        g_projection_mix -= step;
        if (g_projection_mix < g_projection_mix_target)
            g_projection_mix = g_projection_mix_target;
    }
}

/* --- Scene callback ---------------------------------------------------- */

static void my_scene_execute(const SceneExecuteContext *ctx, void *user_data) {
    (void)ctx;
    (void)user_data;

    /* scene_lights_setup has set per-light properties but glDisable'd them.
     * The user (this demo) decides which lights to actually enable. */
    if (g_lighting_on) {
        glEnable(GL_LIGHTING);
        for (int i = 0; i < MAX_LIGHTS; i++)
            if (g_lights_on[i]) glEnable(GL_LIGHT0 + i);
    }

    glPushMatrix();
    glTranslatef(0.0f, 0.75f, 0.0f);
    glColor3f(0.85f, 0.70f, 0.40f);
    glutSolidTeapot(1.0f);
    glPopMatrix();
}

/* --- Light table ------------------------------------------------------- */

static void seed_lights(SceneLight lights[MAX_LIGHTS]) {
    SceneLight *l;

    /* Light 0: warm white key, upper right. */
    l = &lights[0];
    l->id = GL_LIGHT0; l->enabled = g_lights_on[0];
    l->pos[0] =  5.0f; l->pos[1] = 6.0f; l->pos[2] = 4.0f; l->pos[3] = 1.0f;
    l->diffuse[0]  = 1.00f; l->diffuse[1]  = 0.95f; l->diffuse[2]  = 0.85f; l->diffuse[3]  = 1.0f;
    l->ambient[0]  = 0.15f; l->ambient[1]  = 0.15f; l->ambient[2]  = 0.18f; l->ambient[3]  = 1.0f;
    l->specular[0] = 1.00f; l->specular[1] = 0.95f; l->specular[2] = 0.85f; l->specular[3] = 1.0f;

    /* Light 1: cool blue rim from upper left. */
    l = &lights[1];
    l->id = GL_LIGHT1; l->enabled = g_lights_on[1];
    l->pos[0] = -4.0f; l->pos[1] = 3.0f; l->pos[2] = -3.0f; l->pos[3] = 1.0f;
    l->diffuse[0]  = 0.40f; l->diffuse[1]  = 0.60f; l->diffuse[2]  = 1.00f; l->diffuse[3]  = 1.0f;
    l->ambient[0]  = 0.0f;  l->ambient[1]  = 0.0f;  l->ambient[2]  = 0.0f;  l->ambient[3]  = 1.0f;
    l->specular[0] = 0.40f; l->specular[1] = 0.60f; l->specular[2] = 1.00f; l->specular[3] = 1.0f;

    /* Light 2: warm orange fill from lower right. */
    l = &lights[2];
    l->id = GL_LIGHT2; l->enabled = g_lights_on[2];
    l->pos[0] = 4.0f; l->pos[1] = -1.5f; l->pos[2] = 2.0f; l->pos[3] = 1.0f;
    l->diffuse[0]  = 1.00f; l->diffuse[1]  = 0.55f; l->diffuse[2]  = 0.20f; l->diffuse[3]  = 1.0f;
    l->ambient[0]  = 0.0f;  l->ambient[1]  = 0.0f;  l->ambient[2]  = 0.0f;  l->ambient[3]  = 1.0f;
    l->specular[0] = 1.00f; l->specular[1] = 0.55f; l->specular[2] = 0.20f; l->specular[3] = 1.0f;

    /* Light 3: green directional from below/back. */
    l = &lights[3];
    l->id = GL_LIGHT3; l->enabled = g_lights_on[3];
    l->pos[0] = 0.2f; l->pos[1] = -1.0f; l->pos[2] = 0.5f; l->pos[3] = 0.0f; /* directional */
    l->diffuse[0]  = 0.55f; l->diffuse[1]  = 0.95f; l->diffuse[2]  = 0.55f; l->diffuse[3]  = 1.0f;
    l->ambient[0]  = 0.0f;  l->ambient[1]  = 0.0f;  l->ambient[2]  = 0.0f;  l->ambient[3]  = 1.0f;
    l->specular[0] = 0.55f; l->specular[1] = 0.95f; l->specular[2] = 0.55f; l->specular[3] = 1.0f;
}

static void build_config(SceneRenderConfig *cfg) {
    /* memset zeroes every field, which is the intended default for
     * point_parameter_supported: 0 == unsupported, so the star
     * backdrop skips its direct glPointParameterfv. scene_demo has no
     * GL-context capability-query path (it is a link-proof harness,
     * not a feature surface), and never calling the entry point
     * unless a caller has explicitly confirmed support is the safe
     * default — same as post_filter_mode defaulting off here. */
    memset(cfg, 0, sizeof(*cfg));

    cfg->execute_fn = my_scene_execute;
    cfg->anim_time  = g_anim_t;

    cfg->scene_x = 0; cfg->scene_y = 0;
    cfg->scene_w = g_window_w; cfg->scene_h = g_window_h;

    cfg->cam_dist = g_cam_dist;
    cfg->cam_rx   = g_cam_rx;
    cfg->cam_ry   = g_auto_rotate ? fmodf(g_cam_ry + g_anim_t * 25.0f, 360.0f)
                                  : g_cam_ry;
    cfg->cam_tx   = g_cam_tx;
    cfg->cam_ty   = g_cam_ty;
    cfg->cam_tz   = g_cam_tz;
    cfg->cam_motion_glow = (g_drag_button >= 0) ? 1.0f : 0.0f;

    cfg->user_lighting_enabled = g_lighting_on;
    seed_lights(cfg->lights);
    cfg->show_light_indicators = g_show_indicators;

    cfg->backdrop_mode = g_backdrop_mode;
    cfg->wireframe     = g_wireframe;

    cfg->grid_theme       = g_grid_theme;
    cfg->grid_extent_idx  = GRID_EXTENT_FAR;
    cfg->grid_major_idx   = GRID_MAJOR_1;
    cfg->axes_theme       = g_axes_theme;
    /* No transition machine in the demo: draw overlays fully opaque.
     * memset above zeroed these, which would otherwise blank grid/axes
     * once the renderer multiplies color alpha by *_opacity. */
    cfg->grid_opacity     = 1.0f;
    cfg->axes_opacity     = 1.0f;

    /* The grid renderer iterates `for (v = -extent; v <= extent; v += step)`
     * with step = grid_major_steps[major_idx] / 5. If the tables are zero
     * (the memset default) step is 0 and the loop never terminates, blocking
     * GLUT from ever calling idle. The REPL pulls these tables from
     * repl_state; we hard-code them here so the scene module's only runtime
     * deps remain GL / GLUT. */
    cfg->grid_major_steps[GRID_MAJOR_1]  = 1.0f;
    cfg->grid_major_steps[GRID_MAJOR_2]  = 2.0f;
    cfg->grid_major_steps[GRID_MAJOR_5]  = 5.0f;
    cfg->grid_major_steps[GRID_MAJOR_10] = 10.0f;
    cfg->grid_extents[GRID_EXTENT_CLOSE] = 5.0f;
    cfg->grid_extents[GRID_EXTENT_MID]   = 25.0f;
    cfg->grid_extents[GRID_EXTENT_FAR]   = 100.0f;

    cfg->clear_color[0] = 0.10f;
    cfg->clear_color[1] = 0.10f;
    cfg->clear_color[2] = 0.13f;
    cfg->clear_color[3] = 1.0f;

    cfg->multisample_enabled = 1;
    cfg->line_smooth_enabled = 1;

    cfg->alpha_scale = 1.0f;

    /* 1.0 = fully perspective projection, 0.0 = orthographic */
    cfg->projection_mix = smoothstep01(g_projection_mix);
}

/* --- HUD rendering ----------------------------------------------------- */

static void draw_text(int x, int y, const char *s) {
    glRasterPos2i(x, y);
    while (*s) {
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, (int)(unsigned char)*s);
        s++;
    }
}

static void draw_panel_bg(int x, int y, int w, int h) {
    glColor4f(0.05f, 0.06f, 0.08f, 0.6f);
    glBegin(GL_QUADS);
    glVertex2i(x,     y);
    glVertex2i(x + w, y);
    glVertex2i(x + w, y + h);
    glVertex2i(x,     y + h);
    glEnd();
}

static void render_hud(void) {
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT
                 | GL_LINE_BIT | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, g_window_w, 0, g_window_h);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    char buf[256];
    const int line_h = 14;
    int y = g_window_h - 20;

    /* Status panel (always on while HUD is on). */
    int panel_h = line_h * 6 + 10;
    draw_panel_bg(8, y - panel_h + line_h, 360, panel_h);

    glColor4f(0.95f, 0.95f, 1.00f, 1.0f);
    snprintf(buf, sizeof buf,
             "Camera  rx=%6.1f  ry=%6.1f  dist=%5.2f",
             g_cam_rx, g_cam_ry, g_cam_dist);
    draw_text(14, y, buf); y -= line_h;
    snprintf(buf, sizeof buf,
             "Pan     tx=%6.2f  ty=%6.2f  tz=%6.2f",
             g_cam_tx, g_cam_ty, g_cam_tz);
    draw_text(14, y, buf); y -= line_h;
    snprintf(buf, sizeof buf,
             "Lights  [%c1] [%c2] [%c3] [%c4]   global=%s",
             g_lights_on[0] ? 'X' : ' ',
             g_lights_on[1] ? 'X' : ' ',
             g_lights_on[2] ? 'X' : ' ',
             g_lights_on[3] ? 'X' : ' ',
             g_lighting_on ? "ON" : "off");
    draw_text(14, y, buf); y -= line_h;
    snprintf(buf, sizeof buf,
             "Display wire=%s  auto-rot=%s  indicators=%s",
             g_wireframe ? "on" : "off",
             g_auto_rotate ? "on" : "off",
             g_show_indicators ? "on" : "off");
    draw_text(14, y, buf); y -= line_h;
    snprintf(buf, sizeof buf,
             "View    proj=%s  mix=%4.2f",
             (g_projection_mix_target < 0.5f) ? "ortho" : "persp",
             smoothstep01(g_projection_mix));
    draw_text(14, y, buf); y -= line_h;
    snprintf(buf, sizeof buf,
             "Themes  grid=%d  axes=%d  backdrop=%d",
             g_grid_theme, g_axes_theme, g_backdrop_mode);
    draw_text(14, y, buf); y -= line_h + 6;

    if (g_show_help) {
        static const char *help[] = {
            "--- Controls ---",
            "Drag LMB        orbit",
            "Drag RMB/MMB    pan",
            "Wheel / + -     zoom",
            "Arrows          orbit (Shift = pan)",
            "1 2 3 4         toggle lights",
            "L  lighting    W  wireframe   I  indicators",
            "V  projection  G  grid theme  X  axes theme",
            "B  backdrop    R  auto-rotate",
            "H  toggle HUD  ?  toggle help",
            "Q or Esc        quit",
        };
        int n = (int)(sizeof help / sizeof help[0]);
        int help_h = line_h * n + 10;
        draw_panel_bg(8, y - help_h + line_h, 360, help_h);
        glColor4f(0.85f, 0.95f, 1.00f, 1.0f);
        for (int i = 0; i < n; i++) {
            draw_text(14, y, help[i]);
            y -= line_h;
        }
    } else {
        glColor4f(0.65f, 0.75f, 0.85f, 1.0f);
        draw_text(14, y, "Press ? for controls, H to hide HUD");
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    glPopAttrib();
}

/* --- GLUT callbacks ---------------------------------------------------- */

static void display_func(void) {
    SceneRenderConfig cfg;
    build_config(&cfg);

    SceneCameraPose pose = {
        .rx = cfg.cam_rx, .ry = cfg.cam_ry, .dist = cfg.cam_dist,
        .tx = cfg.cam_tx, .ty = cfg.cam_ty, .tz = cfg.cam_tz,
    };
    scene_apply_camera(&pose);
    if (scene_render_3d_scene(&cfg) != 0) {
        fprintf(stderr,
                "scene_demo: scene_render_3d_scene rejected config (errno=%d)\n",
                errno);
        exit(1);
    }

    if (g_show_hud) render_hud();

    glutSwapBuffers();
}

static void reshape_func(int w, int h) {
    g_window_w = w;
    g_window_h = h;
    glutPostRedisplay();
}

static void idle_func(void) {
    g_anim_t += 0.016f;
    step_projection_mix(0.016f);
    glutPostRedisplay();
}

static void keyboard_func(unsigned char key, int x, int y) {
    (void)x; (void)y;
    switch (key) {
        case 27: case 'q': case 'Q':
            exit(0);
        case '+': case '=':
            g_cam_dist *= 0.9f;
            if (g_cam_dist < 0.5f) g_cam_dist = 0.5f;
            break;
        case '-': case '_':
            g_cam_dist *= 1.1f;
            if (g_cam_dist > 100.0f) g_cam_dist = 100.0f;
            break;
        case 'l': case 'L': g_lighting_on    = !g_lighting_on; break;
        case 'v': case 'V':
            g_projection_mix_target =
                (g_projection_mix_target < 0.5f) ? 1.0f : 0.0f;
            break;
        case 'w': case 'W': g_wireframe      = !g_wireframe; break;
        case 'i': case 'I': g_show_indicators = !g_show_indicators; break;
        case 'r': case 'R': g_auto_rotate    = !g_auto_rotate; break;
        case 'h': case 'H': g_show_hud       = !g_show_hud; break;
        case '?': case '/': g_show_help      = !g_show_help; break;
        case 'g': case 'G': g_grid_theme     = (g_grid_theme    + 1) % GRID_THEME_COUNT; break;
        case 'x': case 'X': g_axes_theme     = (g_axes_theme    + 1) % AXES_THEME_COUNT; break;
        case 'b': case 'B':
            g_backdrop_mode =
                (SceneBackdropMode)((g_backdrop_mode + 1) % SCENE_BACKDROP_COUNT);
            break;
        case '1': g_lights_on[0] = !g_lights_on[0]; break;
        case '2': g_lights_on[1] = !g_lights_on[1]; break;
        case '3': g_lights_on[2] = !g_lights_on[2]; break;
        case '4': g_lights_on[3] = !g_lights_on[3]; break;
        default: return;
    }
    glutPostRedisplay();
}

static void special_func(int key, int x, int y) {
    (void)x; (void)y;
    int shift = (glutGetModifiers() & GLUT_ACTIVE_SHIFT) != 0;
    const float orbit_step = 5.0f;
    const float pan_step   = 0.10f * g_cam_dist;

    switch (key) {
        case GLUT_KEY_LEFT:
            if (shift) g_cam_tx -= pan_step; else g_cam_ry -= orbit_step;
            break;
        case GLUT_KEY_RIGHT:
            if (shift) g_cam_tx += pan_step; else g_cam_ry += orbit_step;
            break;
        case GLUT_KEY_UP:
            if (shift) g_cam_ty += pan_step; else g_cam_rx -= orbit_step;
            break;
        case GLUT_KEY_DOWN:
            if (shift) g_cam_ty -= pan_step; else g_cam_rx += orbit_step;
            break;
        default:
            return;
    }
    glutPostRedisplay();
}

static void mouse_func(int button, int state, int x, int y) {
    if (state == GLUT_DOWN) {
        g_drag_button = button;
        g_drag_x = x;
        g_drag_y = y;
        /* Pause auto-rotate while the user is steering the camera so the
         * drag has predictable feel. */
        if (button == GLUT_LEFT_BUTTON) g_auto_rotate = 0;
    } else {
        g_drag_button = -1;
    }
    glutPostRedisplay();
}

static void motion_func(int x, int y) {
    if (g_drag_button < 0) return;
    int dx = x - g_drag_x;
    int dy = y - g_drag_y;
    g_drag_x = x;
    g_drag_y = y;

    if (g_drag_button == GLUT_LEFT_BUTTON) {
        g_cam_ry += (float)dx * 0.5f;
        g_cam_rx += (float)dy * 0.5f;
        if (g_cam_rx > 89.0f)  g_cam_rx = 89.0f;
        if (g_cam_rx < -89.0f) g_cam_rx = -89.0f;
    } else if (g_drag_button == GLUT_MIDDLE_BUTTON
            || g_drag_button == GLUT_RIGHT_BUTTON) {
        float k = 0.005f * g_cam_dist;
        g_cam_tx += (float)dx * k;
        g_cam_ty -= (float)dy * k;
    }
    glutPostRedisplay();
}

static void mousewheel_func(int wheel, int direction, int x, int y) {
    (void)wheel; (void)x; (void)y;
    if (direction > 0) {
        g_cam_dist *= 0.9f;
        if (g_cam_dist < 0.5f) g_cam_dist = 0.5f;
    } else {
        g_cam_dist *= 1.1f;
        if (g_cam_dist > 100.0f) g_cam_dist = 100.0f;
    }
    glutPostRedisplay();
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
    glutSpecialFunc(special_func);
    glutMouseFunc(mouse_func);
    glutMotionFunc(motion_func);
    glutMouseWheelFunc(mousewheel_func);

    glutMainLoop();
    return 0;
}
