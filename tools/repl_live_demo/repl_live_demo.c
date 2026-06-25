/*
 * tools/repl_live_demo/repl_live_demo.c -- live, file-watching REPL demo.
 *
 * The *composition* counterpart to tools/repl_demo: where repl_demo proves the
 * REPL language pipeline links with no editor / controller / UI, this demo
 * proves the REPL pipeline and the variable-panel peer can be wired together by
 * a one-file host controller under a realistic external-editor workflow.
 *
 *   - The editor is external (vim, or anything). This demo never edits text.
 *   - Scenes are separate `.c` files (the canonical save/export format the app
 *     writes), listed in an INI config or passed on the command line.
 *   - The demo imports the active scene via repl_export_load_from_file(), applies
 *     its `// camera` block through a demo-local ReplExportCameraBridge, runs the
 *     executor each frame under a manual orbit camera, and surfaces the scene's
 *     predefined variables in the floating slider panel (drag = live geometry).
 *   - It polls the active file's mtime and re-imports on save, warning to the
 *     terminal on parse errors.
 *
 * Reload is NOT transactional: the import sequence resets live state first, and
 * repl_export_load_from_file() records per-line parse failures as warnings yet
 * still succeeds if the file merely opened. A malformed save can therefore
 * replace a good scene with a partial or empty one -- there is no rollback. The
 * file is the source of truth; the user fixes/undoes in their editor. The demo's
 * job is diagnostic clarity: every reload prints a banner and the importer's own
 * per-line warnings + summary go to stderr, so a partial load is never silent.
 *
 * Link set (Makefile REPL_LIVE_DEMO_DEP_SRCS): the full repl_demo pipeline set
 * (incl. tools/repl_demo/source_document.c, the editor-free static line store)
 * plus the four variable-panel TUs. No src/editor, src/app, src/render3d, or
 * src/ui/app. check-repl-live-demo-no-editor enforces the editor exclusion.
 *
 * Run:
 *   make repl_live_demo && ./repl_live_demo            # default INI
 *   ./repl_live_demo path/to/config.ini                # explicit INI
 *   ./repl_live_demo scene_a.c scene_b.c               # bypass the INI
 *   make repl_live_demo USE_GL_STUBS=1                 # headless link/isolation
 */
#include "gl_includes.h"

#include "repl/eval.h"            /* g_predef_vars_mut, repl_eval_find_predef_var_idx */
#include "repl/executor.h"        /* repl_execute_program, ReplExecutionOptions, point-param proc */
#include "repl/export.h"          /* repl_export_load_from_file, ReplExportCameraBridge */
#include "repl/flatten.h"         /* FlatProgramView */
#include "repl/host_effects.h"
#include "repl/pipeline.h"        /* repl_flatten_commands */
#include "repl/state_notify.h"
#include "repl/state_owners.h"    /* repl_state_reset_program, flat program views, document_count */
#include "repl/state_views.h"     /* repl_state_variables, ReplVariableView */
#include "source_document.h"

#include "ui/subsystems/variable_panel.h"                  /* UiVariable(List), render/hit/size */
#include "subsystems/variable_panel/variable_panel_state.h" /* visibility + drag handlers */
#include "subsystems/variable_panel/variable_panel_drag.h"  /* value source + value change */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- Config / scene list ------------------------------------------------- */

enum { MAX_SCENES = 32, PATH_MAX_LEN = 1024 };

static char  g_scene_paths[MAX_SCENES][PATH_MAX_LEN];
static int   g_scene_count;
static int   g_active_scene;
static time_t g_active_mtime;

static int   g_window_w = 960;
static int   g_window_h = 720;
static int   g_poll_ms  = 250;
static int   g_panel_on = 1;

/* --- Animation + camera state ------------------------------------------- */

static int   g_playing = 1;
static float g_anim_t  = 0.0f;

static float g_cam_dist = 6.0f;
static float g_cam_rx   = 20.0f;
static float g_cam_ry   = 30.0f;
static float g_cam_tx   = 0.0f;
static float g_cam_ty   = 0.0f;
static float g_cam_tz   = 0.0f;

/* Mouse drag state. g_drag_button == -1 means not orbiting/panning. */
static int g_drag_button   = -1;
static int g_drag_x        = 0;
static int g_drag_y        = 0;
static int g_slider_drag   = 0;

/* --- Variable-panel rows (rebuilt after each import) --------------------- */

static UiVariable g_rows[UI_VARIABLE_PANEL_MAX_ROWS];
static int        g_row_count;

/* --- Host effects: edit-line int + stderr diagnostics ------------------- */

static int  g_edit_line = 0;
static int  demo_edit_line_get(void) { return g_edit_line; }
static void demo_edit_line_set(int line) { g_edit_line = line < 0 ? 0 : line; }

/* Only the error hook is installed: per-line import warnings and the load
 * summary already print to stderr directly from src/repl/import.c, so leaving
 * the info `status` hook unset avoids double-printing the summary while still
 * surfacing the error-class messages (e.g. truncated-line / capacity). */
static void demo_status_error(const char *msg) {
    if (msg && msg[0]) fprintf(stderr, "[repl_live_demo] %s\n", msg);
}

static const ReplHostEffects g_host_effects = {
    .status_error  = demo_status_error,
    .edit_line_get = demo_edit_line_get,
    .edit_line_set = demo_edit_line_set,
};

/* --- Demo-local camera bridge ------------------------------------------- *
 *
 * Mirrors the import-side parser in src/app/glr_camera_export.c (which the demo
 * cannot link -- it is app-layer), writing the saved `// camera` block's
 * glTranslatef/glRotatef sequence into the demo's orbit-camera variables. Only
 * the two import callbacks are needed: repl_export_load_from_file() calls
 * reset_import() once then try_consume_import_line() per line. The save/example
 * callbacks are unused (left NULL).
 *
 *   state 0  glTranslatef -> camera distance (-z)
 *   state 1  glRotatef axis (1,0,0) -> rx
 *   state 2  glRotatef axis (0,1,0) or g_angle placeholder -> ry
 *   state 3  glTranslatef -> pan target (negated)
 * Plus the `static float g_angle = N;` preamble, treated as a free ry input.
 */
static int g_cam_parse_state = 0;

static const char *cam_skip_sep(const char *p) {
    while (*p == 'f' || *p == 'F' || *p == ',' || *p == ' ' || *p == '\t') p++;
    return p;
}

static int cam_read_floats(const char *p, float *out, int n) {
    int i;
    for (i = 0; i < n; i++) {
        char *end = NULL;
        p = cam_skip_sep(p);
        out[i] = strtof(p, &end);
        if (end == p) return 0;
        p = end;
    }
    return 1;
}

static int cam_try_parse_angle_preamble(const char *text) {
    const char *p = text;
    const char *eq;
    char *end = NULL;
    float v;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "static float g_angle", 20) != 0) return 0;
    eq = strchr(p, '=');
    if (!eq) return 0;
    eq++;
    v = strtof(eq, &end);
    if (end == eq) return 0;
    g_cam_ry = v;
    return 1;
}

static int cam_try_consume_block_line(const char *text) {
    const char *p = text;
    if (g_cam_parse_state >= 4) return 0;
    while (*p == ' ' || *p == '\t') p++;

    if (g_cam_parse_state == 0 && strncmp(p, "glTranslatef", 12) == 0) {
        float v[3];
        p = strchr(p, '('); if (!p) return 0; p++;
        if (!cam_read_floats(p, v, 3)) return 0;
        g_cam_dist = -v[2];
        g_cam_parse_state = 1;
        return 1;
    }
    if (g_cam_parse_state == 1 && strncmp(p, "glRotatef", 9) == 0) {
        float v[4];
        p = strchr(p, '('); if (!p) return 0; p++;
        if (!cam_read_floats(p, v, 4)) return 0;
        if (v[1] != 1.0f || v[2] != 0.0f || v[3] != 0.0f) return 0;
        g_cam_rx = v[0];
        g_cam_parse_state = 2;
        return 1;
    }
    if (g_cam_parse_state == 2 && strncmp(p, "glRotatef", 9) == 0) {
        const char *q = strchr(p, '(');
        float v[4];
        if (q && strstr(q, "g_angle")) { g_cam_parse_state = 3; return 1; }
        p = strchr(p, '('); if (!p) return 0; p++;
        if (!cam_read_floats(p, v, 4)) return 0;
        if (v[1] != 0.0f || v[2] != 1.0f || v[3] != 0.0f) return 0;
        g_cam_ry = v[0];
        g_cam_parse_state = 3;
        return 1;
    }
    if (g_cam_parse_state == 3 && strncmp(p, "glTranslatef", 12) == 0) {
        float v[3];
        p = strchr(p, '('); if (!p) return 0; p++;
        if (!cam_read_floats(p, v, 3)) return 0;
        g_cam_tx = -v[0]; g_cam_ty = -v[1]; g_cam_tz = -v[2];
        g_cam_parse_state = 4;
        return 1;
    }
    return 0;
}

static int demo_cam_try_consume_import_line(const char *text) {
    if (cam_try_parse_angle_preamble(text)) return 1;
    return cam_try_consume_block_line(text);
}

static void demo_cam_reset_import(void) {
    g_cam_parse_state = 0;
}

static const ReplExportCameraBridge g_cam_bridge = {
    .reset_import            = demo_cam_reset_import,
    .try_consume_import_line = demo_cam_try_consume_import_line,
};

/* --- Variable panel wiring ---------------------------------------------- */

/* Rebuild the slider rows from the live predefined-variable table after each
 * import. Row value pointers reference g_predef_vars_mut so the panel reads (and
 * drags write) the live values. The animation clock `t` is included like any
 * other variable; dragging it scrubs the clock (see apply_var_change) so the
 * value persists through display_func's per-frame `t = g_anim_t` write. */
static void rebuild_variable_rows(void) {
    ReplVariableView v = repl_state_variables();
    int i;
    g_row_count = 0;
    for (i = 0; i < v.var_count && g_row_count < UI_VARIABLE_PANEL_MAX_ROWS; i++) {
        const char *name = v.vars[i].name;
        int idx;
        if (!name[0]) continue;
        idx = repl_eval_find_predef_var_idx(name);
        if (idx < 0) continue;
        snprintf(g_rows[g_row_count].name, sizeof(g_rows[g_row_count].name), "%s", name);
        g_rows[g_row_count].value = &g_predef_vars_mut[idx].value;
        g_rows[g_row_count].tuned = 0;
        g_row_count++;
    }
}

static int demo_var_read_row(int row, char *name_out, int name_cap, float *value_out) {
    if (row < 0 || row >= g_row_count) return 0;
    snprintf(name_out, (size_t)name_cap, "%s", g_rows[row].name);
    *value_out = *g_rows[row].value;
    return 1;
}
static const VariablePanelValueSource g_value_source = { demo_var_read_row };

static void apply_var_change(const VariablePanelValueChange *chg) {
    int idx = repl_eval_find_predef_var_idx(chg->name);
    if (idx >= 0) {
        g_predef_vars_mut[idx].value = chg->value;
        /* Dragging the time clock scrubs g_anim_t too, otherwise display_func's
         * per-frame `t = g_anim_t` write would clobber the drag next frame.
         * Playback (if running) then continues from the scrubbed value. */
        if (strcmp(chg->name, "t") == 0)
            g_anim_t = chg->value;
        repl_state_mark_flat_dirty();
    }
}

static UiVariablePanelView build_panel_view(void) {
    UiVariablePanelView v;
    int pw, ph;
    memset(&v, 0, sizeof v);
    v.visible  = g_panel_on && variable_panel_visible();
    v.window_w = g_window_w;
    v.window_h = g_window_h;
    ui_variable_panel_size(g_row_count, &pw, &ph);
    v.panel_x = g_window_w - pw - 8;
    v.panel_y = 8;
    v.vars            = g_rows;
    v.var_count       = g_row_count;
    v.drag_active_var = variable_panel_drag_active_var();
    v.drag_log_mode   = variable_panel_drag_log_mode();
    return v;
}

/* --- Import + file watching --------------------------------------------- */

static const char *scene_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int file_mtime(const char *path, time_t *out) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    *out = st.st_mtime;
    return 1;
}

static void import_active_scene(void) {
    const char *path;
    ReplImportResult result;
    int ok;

    if (g_scene_count == 0) return;
    path = g_scene_paths[g_active_scene];
    fprintf(stderr, "[repl_live_demo] reloading [%d/%d] %s\n",
            g_active_scene + 1, g_scene_count, path);

    /* Non-transactional reset before import (see file header). */
    variable_panel_handle_drag_reset();
    g_slider_drag = 0;
    repl_state_reset_program();
    source_document_clear();
    g_edit_line = 0;
    repl_dispatch_edit_line_set(0);

    memset(&result, 0, sizeof result);
    ok = repl_export_load_from_file(path, &result);
    if (!ok)
        fprintf(stderr, "[repl_live_demo] error: could not open %s\n", path);

    repl_state_mark_source_dirty();
    repl_state_mark_flat_dirty();
    repl_flatten_commands(repl_dispatch_edit_line_get());
    rebuild_variable_rows();

    /* Stamp mtime so the watcher only fires on the next change. */
    file_mtime(path, &g_active_mtime);
}

static void select_scene(int idx) {
    if (g_scene_count == 0) return;
    g_active_scene = ((idx % g_scene_count) + g_scene_count) % g_scene_count;
    import_active_scene();
}

static void poll_active_file(void) {
    time_t m;
    if (g_scene_count == 0) return;
    if (!file_mtime(g_scene_paths[g_active_scene], &m)) return; /* missing: keep last good */
    if (m != g_active_mtime)
        import_active_scene();
}

/* --- Rendering ----------------------------------------------------------- */

static void apply_projection(void) {
    double aspect = (g_window_h > 0) ? (double)g_window_w / (double)g_window_h : 1.0;
    glViewport(0, 0, g_window_w, g_window_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, aspect, 0.1, 200.0);
    glMatrixMode(GL_MODELVIEW);
}

static void apply_camera_modelview(void) {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -g_cam_dist);
    glRotatef(g_cam_rx, 1.0f, 0.0f, 0.0f);
    glRotatef(g_cam_ry, 0.0f, 1.0f, 0.0f);
    glTranslatef(-g_cam_tx, -g_cam_ty, -g_cam_tz);
}

static void draw_text(int x, int y, const char *s) {
    glRasterPos2i(x, y);
    while (*s) {
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, (int)(unsigned char)*s);
        s++;
    }
}

static void draw_hud(void) {
    char buf[320];
    glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0.0, (double)g_window_w, 0.0, (double)g_window_h);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glColor3f(0.92f, 0.94f, 1.0f);
    snprintf(buf, sizeof buf, "scene %d/%d  %s   t=%.2f%s   cmds=%d  vars=%d",
             g_active_scene + 1, g_scene_count,
             scene_basename(g_scene_paths[g_active_scene]),
             g_anim_t, g_playing ? "" : " (paused)",
             repl_state_document_count(), g_row_count);
    draw_text(10, g_window_h - 18, buf);

    glColor3f(0.62f, 0.72f, 0.84f);
    draw_text(10, 12,
        "[ ] cycle scene   r reload   v panel   space pause   "
        "LMB orbit  RMB pan  wheel zoom   q quit");

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
}

static void display_func(void) {
    ReplExecutionOptions opts;
    int t_idx;

    /* Push the animation clock into the predef table and re-bake so has_vars
     * expressions animate, exactly like the full app's per-frame reflatten. */
    t_idx = repl_eval_find_predef_var_idx("t");
    if (t_idx >= 0)
        g_predef_vars_mut[t_idx].value = g_anim_t;
    repl_state_mark_flat_dirty();
    repl_flatten_commands(repl_dispatch_edit_line_get());

    glClearColor(0.09f, 0.10f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    apply_projection();
    apply_camera_modelview();

    memset(&opts, 0, sizeof opts);
    opts.flat_cmd_count = repl_state_flat_program_count();
    opts.program        = repl_state_flat_program_view();
    opts.text           = source_document_view();
    repl_execute_program(&opts);

    draw_hud();

    /* The panel renderer self-manages its 2D projection (gl2d_begin); draw it
     * last, before the swap, like tools/variable_panel_demo. */
    if (g_panel_on) {
        UiVariablePanelView view = build_panel_view();
        ui_variable_panel_render(&view);
    }

    glutSwapBuffers();
}

static void reshape_func(int w, int h) {
    if (h < 1) h = 1;
    g_window_w = w;
    g_window_h = h;
    glViewport(0, 0, w, h);
    glutPostRedisplay();
}

static void idle_func(void) {
    if (g_playing)
        g_anim_t += 1.0f / 60.0f;
    glutPostRedisplay();
}

static void watch_timer(int value) {
    (void)value;
    poll_active_file();
    glutTimerFunc((unsigned int)g_poll_ms, watch_timer, 0);
}

/* --- Input --------------------------------------------------------------- */

static void mouse_func(int button, int state, int x, int y) {
    if (state == GLUT_DOWN) {
        if (button == GLUT_LEFT_BUTTON || button == GLUT_RIGHT_BUTTON) {
            UiVariablePanelView view = build_panel_view();
            UiHit hit = ui_variable_panel_hit_test(&view, x, y);
            if (hit.kind == UI_HIT_VARIABLE_SLIDER) {
                int log_mode = (button == GLUT_RIGHT_BUTTON) ? 1 : 0;
                variable_panel_handle_drag_begin(hit.item_idx, log_mode, x);
                g_slider_drag = 1;
                glutPostRedisplay();
                return;
            }
        }
        g_drag_button = button;
        g_drag_x = x;
        g_drag_y = y;
    } else { /* GLUT_UP */
        if (g_slider_drag) {
            variable_panel_handle_drag_reset();
            g_slider_drag = 0;
        }
        g_drag_button = -1;
    }
    glutPostRedisplay();
}

static void motion_func(int x, int y) {
    if (g_slider_drag) {
        VariablePanelValueChange chg;
        if (variable_panel_handle_drag_motion(x, &chg))
            apply_var_change(&chg);
        glutPostRedisplay();
        return;
    }
    if (g_drag_button < 0) return;

    {
        int dx = x - g_drag_x;
        int dy = y - g_drag_y;
        g_drag_x = x;
        g_drag_y = y;
        if (g_drag_button == GLUT_LEFT_BUTTON) {
            g_cam_ry += (float)dx * 0.5f;
            g_cam_rx += (float)dy * 0.5f;
            if (g_cam_rx > 89.0f)  g_cam_rx = 89.0f;
            if (g_cam_rx < -89.0f) g_cam_rx = -89.0f;
        } else {
            float k = 0.005f * g_cam_dist;
            g_cam_tx += (float)dx * k;
            g_cam_ty -= (float)dy * k;
        }
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
        if (g_cam_dist > 200.0f) g_cam_dist = 200.0f;
    }
    glutPostRedisplay();
}

static void keyboard_func(unsigned char key, int x, int y) {
    (void)x; (void)y;
    switch (key) {
    case 27: case 'q': case 'Q':
        exit(0);
    case '[':
        select_scene(g_active_scene - 1);
        break;
    case ']':
        select_scene(g_active_scene + 1);
        break;
    case 'r': case 'R':
        import_active_scene();
        break;
    case 'v': case 'V':
        g_panel_on = !g_panel_on;
        variable_panel_set_visible(g_panel_on);
        break;
    case ' ':
        g_playing = !g_playing;
        break;
    default:
        return;
    }
    glutPostRedisplay();
}

/* --- GL capability install ---------------------------------------------- */

static void install_point_parameter_proc(void) {
#ifndef GL_STUBS
#if defined(__APPLE__) && defined(USE_GLUT)
    repl_executor_install_point_parameter_proc(&glPointParameterfv);
    repl_executor_set_point_parameter_supported(1);
#else
    ReplExecutorPointParameterProc proc =
        (ReplExecutorPointParameterProc)glutGetProcAddress("glPointParameterfv");
    if (!proc)
        proc = (ReplExecutorPointParameterProc)glutGetProcAddress("glPointParameterfvARB");
    if (!proc)
        proc = (ReplExecutorPointParameterProc)glutGetProcAddress("glPointParameterfvEXT");
    repl_executor_install_point_parameter_proc(proc);
    repl_executor_set_point_parameter_supported(proc != NULL);
#endif
#endif /* GL_STUBS */
}

/* --- Config / argument parsing ------------------------------------------ */

static char *str_trim(char *s) {
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static int ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + (ls - lf), suffix) == 0;
}

static void add_scene_path(const char *path) {
    if (g_scene_count >= MAX_SCENES) {
        fprintf(stderr, "[repl_live_demo] too many scenes (max %d), ignoring %s\n",
                MAX_SCENES, path);
        return;
    }
    snprintf(g_scene_paths[g_scene_count], PATH_MAX_LEN, "%s", path);
    g_scene_count++;
}

static void path_dirname(const char *path, char *out, int cap) {
    const char *slash = strrchr(path, '/');
    if (!slash) { snprintf(out, (size_t)cap, "."); return; }
    {
        int n = (int)(slash - path);
        if (n >= cap) n = cap - 1;
        memcpy(out, path, (size_t)n);
        out[n] = '\0';
    }
}

/* INI: flat `key = value`, `#`/`;` comments, repeatable `scene=`. Relative
 * scene paths resolve against the INI's directory so `./repl_live_demo` works
 * from the repo root with the bundled tools/repl_live_demo/repl_live_demo.ini. */
static int load_ini(const char *path) {
    FILE *f = fopen(path, "r");
    char dir[PATH_MAX_LEN];
    char line[PATH_MAX_LEN + 64];
    if (!f) return 0;
    path_dirname(path, dir, (int)sizeof dir);

    while (fgets(line, sizeof line, f)) {
        char *s = str_trim(line);
        char *eq, *key, *val;
        if (!*s || *s == '#' || *s == ';') continue;
        eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        key = str_trim(s);
        val = str_trim(eq + 1);
        if (strcmp(key, "scene") == 0) {
            if (val[0] == '/' || dir[0] == '\0') {
                add_scene_path(val);
            } else {
                char resolved[PATH_MAX_LEN];
                snprintf(resolved, sizeof resolved, "%s/%s", dir, val);
                add_scene_path(resolved);
            }
        } else if (strcmp(key, "window") == 0) {
            int w, h;
            if (sscanf(val, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                g_window_w = w;
                g_window_h = h;
            }
        } else if (strcmp(key, "poll_ms") == 0) {
            int ms = atoi(val);
            if (ms > 0) g_poll_ms = ms;
        } else if (strcmp(key, "panel") == 0) {
            g_panel_on = (strcmp(val, "on") == 0 || strcmp(val, "1") == 0 ||
                          strcmp(val, "true") == 0);
        } else {
            fprintf(stderr, "[repl_live_demo] unknown INI key '%s' (ignored)\n", key);
        }
    }
    fclose(f);
    return g_scene_count > 0;
}

static int load_default_ini(const char *explicit_path) {
    static const char *const candidates[] = {
        "repl_live_demo.ini",
        "tools/repl_live_demo/repl_live_demo.ini",
        NULL,
    };
    int i;
    if (explicit_path)
        return load_ini(explicit_path);
    for (i = 0; candidates[i]; i++)
        if (load_ini(candidates[i])) return 1;
    return 0;
}

static void print_usage(const char *prog) {
    printf(
"Usage: %s [config.ini | scene_a.c scene_b.c ...]\n"
"\n"
"Live, file-watching REPL demo. Imports scene .c files (the app's save/export\n"
"format), watches their mtime, re-imports on save, and drives predefined-\n"
"variable sliders live. Edit the scene files in your own editor (vim, ...).\n"
"\n"
"  (no args)     Load the default INI (repl_live_demo.ini, or the bundled\n"
"                tools/repl_live_demo/repl_live_demo.ini).\n"
"  config.ini    Load scenes + options from this INI.\n"
"  *.c files     Use these scene files directly (bypass the INI).\n"
"\n"
"Keys:  [ ] cycle scene   r reload   v toggle panel   space pause   q/Esc quit\n"
"       LMB orbit   RMB pan   wheel zoom   drag a slider (RMB = log)\n",
        prog);
}

int main(int argc, char **argv) {
    const char *ini_path = NULL;
    int got_files = 0;
    int i;

    glutInit(&argc, argv);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (ends_with(a, ".ini")) {
            ini_path = a;
        } else if (ends_with(a, ".c")) {
            add_scene_path(a);
            got_files = 1;
        } else {
            fprintf(stderr, "[repl_live_demo] ignoring argument '%s'\n", a);
        }
    }

    if (!got_files && !load_default_ini(ini_path)) {
        fprintf(stderr,
            "[repl_live_demo] no scenes: pass *.c files or an INI with `scene=` lines\n"
            "  (looked for %s).\n",
            ini_path ? ini_path : "repl_live_demo.ini / tools/repl_live_demo/repl_live_demo.ini");
        print_usage(argv[0]);
        return 2;
    }

    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(g_window_w, g_window_h);
    glutCreateWindow("repl_live_demo");

    glEnable(GL_DEPTH_TEST);
    install_point_parameter_proc();

    repl_install_host_effects(&g_host_effects);
    repl_export_install_camera_bridge(&g_cam_bridge);
    variable_panel_state_reset();
    variable_panel_set_visible(g_panel_on);
    variable_panel_install_value_source(&g_value_source);

    import_active_scene();

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutIdleFunc(idle_func);
    glutKeyboardFunc(keyboard_func);
    glutMouseFunc(mouse_func);
    glutMotionFunc(motion_func);
    glutMouseWheelFunc(mousewheel_func);
    glutTimerFunc((unsigned int)g_poll_ms, watch_timer, 0);

    printf("repl_live_demo: watching %d scene(s); edit them in your editor and save.\n",
           g_scene_count);
    printf("  keys: [ ] cycle  r reload  v panel  space pause  q quit\n");

    glutMainLoop();
    return 0;
}
