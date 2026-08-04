/*
 * tools/render3d_demo/render3d_demo.c - independent binary that drives the
 * render3d module with a non-REPL geometry callback (a single glutSolidTeapot).
 *
 * Demonstrates that src/render3d/ has no hard dependency on the REPL editor or
 * controller. Adds a small interactive shell:
 *
 *   - mouse drag (orbit / pan) + wheel (zoom)
 *   - V toggles perspective / orthographic projection with an eased blend
 *   - keyboard toggles for lighting, wireframe, grid / axes / backdrop
 *   - per-light enable on number keys
 *   - bitmap HUD with the current state and a help panel
 *
 * App pseudo-code:
 *   render() {
 *     apply_camera_modelview(&cfg);   // inline glLoadIdentity + glTranslatef + glRotatef
 *     render3d_draw_scene(&state, &cfg);
 *     render_hud();
 *   }
 *   my_scene(...) { glEnable(GL_LIGHTING); ...; glutSolidTeapot(1.0); }
 *   build_cfg() { cfg.execute_fn = my_scene; cfg.cam_* = ...; ... }
 */
#include "gl_includes.h"
#include "config.h"
#include "render3d/render.h"
#include "render3d/render_types.h"
#include "render3d/view_mode.h"  /* GLR_VIEW_PROJECTION_TRANSITION_SECS */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Hot reload (opt-in, RENDER3D_HOT_RELOAD=1 via `make render3d-hot`) --
 *
 * When enabled, the src/render3d subtree is not linked into this binary; it
 * lives in a shared library the host dlopen()s. The host watches the render3d
 * sources and, on a save, rebuilds that library and re-dlopen()s a fresh copy,
 * so the module can be tweaked live. Every render3d_* call site below routes
 * through a function pointer resolved from the library (the macros further
 * down), while all demo state (camera/view/grid/lights/backdrop) stays in this
 * TU and survives the reload untouched. See the render3d-hot Makefile block. */
#if RENDER3D_HOT_RELOAD
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

/* Entry points resolved out of the reloadable library. */
static void (*g_hot_init_gl)(void);
static void (*g_hot_state_init)(Render3dState *);
static int  (*g_hot_draw_scene)(Render3dState *, const Render3dRenderConfig *);
static void (*g_hot_get_active_projection)(const Render3dState *,
                                           Render3dProjectionDesc *);

/* Route the render3d_* call sites through the pointers. Defined AFTER
 * render3d/render.h is included, so the header's prototypes are untouched -
 * these function-like macros only rewrite the call expressions in this file. */
#define render3d_init_gl()                   g_hot_init_gl()
#define render3d_state_init(s)               g_hot_state_init(s)
#define render3d_draw_scene(s, c)            g_hot_draw_scene((s), (c))
#define render3d_get_active_projection(s, o) g_hot_get_active_projection((s), (o))

static void  *g_hot_handle       = NULL;  /* currently dlopen'd image */
static char   g_hot_active_path[1024];    /* its unique on-disk copy */
static int    g_hot_seq          = 0;     /* unique-copy counter */
static int    g_hot_reload_count = 0;     /* successful reloads (HUD) */
static int    g_hot_last_ok      = 1;     /* last build+reload succeeded */
static time_t g_hot_src_mtime    = 0;     /* newest render3d source seen */

/* Rebuild is a two-step handshake with the frame loop so the on-screen
 * "rebuilding" banner is actually visible: a detected change only ARMS the
 * rebuild (HOT_BUILD_PENDING); display_func renders + swaps the banner frame,
 * then runs the blocking build (HOT_BUILDING), then returns to HOT_IDLE. */
enum { HOT_IDLE = 0, HOT_BUILD_PENDING, HOT_BUILDING };
static int    g_hot_state = HOT_IDLE;

/* Newest mtime of any .c/.h under `dir` (recursive). Skips dotfiles, which
 * also drops editor swap files (.grid.c.swp) that would otherwise thrash. */
static time_t hot_newest_mtime(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    time_t newest = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            time_t sub = hot_newest_mtime(path);
            if (sub > newest) newest = sub;
        } else {
            const char *dot = strrchr(e->d_name, '.');
            if (dot && (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0)
                && st.st_mtime > newest)
                newest = st.st_mtime;
        }
    }
    closedir(d);
    return newest;
}

static int hot_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return 0;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[1 << 16];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    return ok;
}

/* dlopen a *fresh copy* of the freshly-built library and swap its symbols in.
 * The copy-to-unique-path dance is deliberate: macOS dyld caches images by
 * path and keeps them mapped past dlclose, so re-dlopen()ing the canonical
 * path would hand back the stale code. Resolving into locals first means a
 * broken library can't clobber the working pointers. Returns 1 on success. */
static int hot_load(void) {
    char newpath[1024];
    snprintf(newpath, sizeof newpath, "%s.reload.%d",
             RENDER3D_HOT_LIB_PATH, g_hot_seq++);

    if (!hot_copy_file(RENDER3D_HOT_LIB_PATH, newpath)) {
        fprintf(stderr, "[hot] copy of %s failed\n", RENDER3D_HOT_LIB_PATH);
        return 0;
    }

    void *h = dlopen(newpath, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "[hot] dlopen failed: %s\n", dlerror());
        remove(newpath);
        return 0;
    }

    void *a = dlsym(h, "render3d_init_gl");
    void *b = dlsym(h, "render3d_state_init");
    void *c = dlsym(h, "render3d_draw_scene");
    void *e = dlsym(h, "render3d_get_active_projection");
    if (!a || !b || !c || !e) {
        fprintf(stderr, "[hot] dlsym failed: %s\n", dlerror());
        dlclose(h);
        remove(newpath);
        return 0;
    }

    if (g_hot_handle) dlclose(g_hot_handle);
    if (g_hot_active_path[0]) remove(g_hot_active_path);
    g_hot_handle = h;
    snprintf(g_hot_active_path, sizeof g_hot_active_path, "%s", newpath);

    g_hot_init_gl               = (void (*)(void))a;
    g_hot_state_init            = (void (*)(Render3dState *))b;
    g_hot_draw_scene            = (int (*)(Render3dState *,
                                           const Render3dRenderConfig *))c;
    g_hot_get_active_projection = (void (*)(const Render3dState *,
                                            Render3dProjectionDesc *))e;
    return 1;
}

/* Polled from idle: if a render3d source changed, ARM a rebuild for the next
 * frame. Throttled so the directory walk stays cheap. Deliberately does not
 * build here - the build runs post-swap in display_func so the banner frame
 * reaches the screen first. */
static void hot_poll_sources(void) {
    if (g_hot_state != HOT_IDLE) return;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    double now = (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
    static double last_check = 0.0;
    if (now - last_check < 0.25) return;
    last_check = now;

    time_t newest = hot_newest_mtime(RENDER3D_HOT_SRC_DIR);
    if (newest <= g_hot_src_mtime) return;
    g_hot_src_mtime = newest;

    g_hot_state = HOT_BUILD_PENDING;
    glutPostRedisplay();
}

/* The blocking rebuild + reload. MUST be called after display_func has already
 * rendered and swapped a frame showing the "rebuilding" banner, so it is on
 * screen while this (frozen) compile runs. The window pauses for the compile -
 * fine for a dev tool. */
static void hot_run_build(void) {
    fprintf(stderr, "[hot] change detected - rebuilding src/render3d ...\n");
    int rc = system(RENDER3D_HOT_BUILD_CMD);
    if (rc != 0) {
        fprintf(stderr, "[hot] build failed (status %d) - keeping module #%d\n",
                rc, g_hot_reload_count);
        g_hot_last_ok = 0;
        return;
    }
    if (hot_load()) {
        g_hot_reload_count++;
        g_hot_last_ok = 1;
        /* Re-run one-time GL init so the fresh code re-establishes any GL-side
         * global state it owns (ambient term, post-process caches). Demo state
         * lives in this TU and is deliberately not reset. */
        render3d_init_gl();
        fprintf(stderr, "[hot] reloaded module #%d\n", g_hot_reload_count);
    } else {
        g_hot_last_ok = 0;
    }
}
#endif /* RENDER3D_HOT_RELOAD */

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

/* Per-renderer scene state - the demo owns one instance, init'd in
 * main() after the GL context is current. Mirrors what glr_ctrl owns
 * in src/app/glr_ctrl.c (the single-renderer assumption is now per
 * binary). */
static Render3dState g_scene_renderer;

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
static Render3dBackdropMode g_backdrop_mode = RENDER3D_BACKDROP_OFF;
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

static void my_scene_execute(const Render3dExecuteContext *ctx, void *user_data) {
    (void)user_data;
    int wireframe_line_pass =
        ctx && (ctx->purpose == RENDER3D_EXEC_WIREFRAME_HIDDEN_LINES ||
                ctx->purpose == RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES);

    /* The scene owns its clear - render3d_draw_scene no longer clears the
     * scene rect for anyone. In gl-repl the equivalent glClear is a visible
     * line in the user's program; here it is this call. It sits inside
     * execute_fn (not before draw_scene) so it lands under the same
     * per-pass scissor the REPL's program clear does, and re-runs per
     * accumulation sample. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* render3d_lights_setup has set per-light properties but glDisable'd them.
     * The user (this demo) decides which lights to actually enable. */
    if (g_lighting_on && !wireframe_line_pass) {
        glEnable(GL_LIGHTING);
        for (int i = 0; i < MAX_LIGHTS; i++)
            if (g_lights_on[i]) glEnable(GL_LIGHT0 + i);
    }

    glPushMatrix();
    glTranslatef(0.0f, 0.75f, 0.0f);
    if (!wireframe_line_pass)
        glColor3f(0.85f, 0.70f, 0.40f);
    glutSolidTeapot(1.0f);
    glPopMatrix();
}

/* --- Light table ------------------------------------------------------- */

static void seed_lights(Render3dLight lights[MAX_LIGHTS]) {
    Render3dLight *l;

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

static void build_config(Render3dRenderConfig *cfg) {
    /* memset zeroes every field, which is the intended default for
     * point_parameter_supported: 0 == unsupported, so the star
     * backdrop skips its direct glPointParameterfv. render3d_demo has no
     * GL-context capability-query path (it is a link-proof harness,
     * not a feature surface), and never calling the entry point
     * unless a caller has explicitly confirmed support is the safe
     * default - same as post_filter_mode defaulting off here. */
    memset(cfg, 0, sizeof(*cfg));

    cfg->execute_fn = my_scene_execute;
    cfg->anim_time  = g_anim_t;

    cfg->render3d_x = 0; cfg->render3d_y = 0;
    cfg->render3d_w = g_window_w; cfg->render3d_h = g_window_h;

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
    cfg->highlight_light_slot = -1; /* no cursor / editor in this harness */

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
     * repl_state; we hard-code them here so the render3d module's only runtime
     * deps remain GL / GLUT. */
    cfg->grid_major_steps[GRID_MAJOR_1]  = 1.0f;
    cfg->grid_major_steps[GRID_MAJOR_2]  = 2.0f;
    cfg->grid_major_steps[GRID_MAJOR_5]  = 5.0f;
    cfg->grid_major_steps[GRID_MAJOR_10] = 10.0f;
    cfg->grid_extents[GRID_EXTENT_CLOSE] = 5.0f;
    cfg->grid_extents[GRID_EXTENT_MID]   = 25.0f;
    cfg->grid_extents[GRID_EXTENT_FAR]   = 100.0f;

    /* The demo has no program observing its own clears, so the color the walk
     * starts from and the background the helpers fade toward are the same
     * value - written to both fields rather than to one shared field. */
    cfg->baseline_clear_color[0] = 0.10f;
    cfg->baseline_clear_color[1] = 0.10f;
    cfg->baseline_clear_color[2] = 0.13f;
    cfg->baseline_clear_color[3] = 1.0f;
    memcpy(cfg->presentation_rgba, cfg->baseline_clear_color,
           sizeof(cfg->presentation_rgba));

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
#if RENDER3D_HOT_RELOAD
    int panel_lines = 7;
#else
    int panel_lines = 6;
#endif
    int panel_h = line_h * panel_lines + 10;
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
    draw_text(14, y, buf); y -= line_h;
#if RENDER3D_HOT_RELOAD
    const char *hot_status = (g_hot_state != HOT_IDLE) ? "building..."
                           : (g_hot_last_ok ? "ok" : "FAILED");
    snprintf(buf, sizeof buf,
             "Hot     reloads=%d  status=%s",
             g_hot_reload_count, hot_status);
    if (g_hot_state != HOT_IDLE)   glColor4f(1.00f, 0.80f, 0.35f, 1.0f);
    else if (!g_hot_last_ok)       glColor4f(1.00f, 0.55f, 0.45f, 1.0f);
    draw_text(14, y, buf); y -= line_h;
    glColor4f(0.95f, 0.95f, 1.00f, 1.0f);
#endif
    y -= 6;

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

#if RENDER3D_HOT_RELOAD
/* A prominent centered banner shown while a rebuild is armed / running, so the
 * "rebuilding" state is visible even with the HUD hidden. Drawn every frame
 * whose g_hot_state != HOT_IDLE; the frame carrying it is swapped to screen
 * before hot_run_build() blocks (see display_func). */
static void render_hot_banner(void) {
    if (g_hot_state == HOT_IDLE) return;

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

    const char *msg = "* Rebuilding & reloading src/render3d ...";
    int text_w = (int)strlen(msg) * 8;   /* GLUT_BITMAP_8_BY_13 is 8px wide */
    int pad    = 16;
    int bw     = text_w + pad * 2;
    int bh     = 34;
    int bx     = (g_window_w - bw) / 2;
    if (bx < 8) bx = 8;
    int by     = g_window_h - 78;

    glColor4f(0.14f, 0.11f, 0.03f, 0.90f);
    glBegin(GL_QUADS);
    glVertex2i(bx,      by);
    glVertex2i(bx + bw, by);
    glVertex2i(bx + bw, by + bh);
    glVertex2i(bx,      by + bh);
    glEnd();

    glColor4f(1.00f, 0.75f, 0.25f, 1.0f);
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2i(bx,      by);
    glVertex2i(bx + bw, by);
    glVertex2i(bx + bw, by + bh);
    glVertex2i(bx,      by + bh);
    glEnd();

    glColor4f(1.00f, 0.92f, 0.55f, 1.0f);
    draw_text(bx + pad, by + bh / 2 - 5, msg);

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopAttrib();
}
#endif /* RENDER3D_HOT_RELOAD */

/* --- GLUT callbacks ---------------------------------------------------- */

/* Populate GL_MODELVIEW with the orbit-camera transform. The scene
 * module documents this as the caller's responsibility - see the
 * comment block in src/render3d/render.h above render3d_draw_scene.
 * Inlined here (instead of importing src/app/glr_camera.h's
 * glr_camera_load_modelview) because render3d_demo's purpose is proving
 * src/render3d/ has no hard dependency on app code. */
static void apply_camera_modelview(const Render3dRenderConfig *cfg) {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -cfg->cam_dist);
    glRotatef(cfg->cam_rx, 1, 0, 0);
    glRotatef(cfg->cam_ry, 0, 1, 0);
    glTranslatef(-cfg->cam_tx, -cfg->cam_ty, -cfg->cam_tz);
}

static void display_func(void) {
    Render3dRenderConfig cfg;
    build_config(&cfg);

    apply_camera_modelview(&cfg);
    if (render3d_draw_scene(&g_scene_renderer, &cfg) != 0) {
        fprintf(stderr,
                "render3d_demo: render3d_draw_scene rejected config (errno=%d)\n",
                errno);
        exit(1);
    }

    if (g_show_hud) render_hud();

#if RENDER3D_HOT_RELOAD
    render_hot_banner();
#endif

    glutSwapBuffers();

#if RENDER3D_HOT_RELOAD
    /* The banner frame is now on screen. Only NOW run the blocking rebuild +
     * reload, so the "rebuilding" state is visible for the whole compile
     * (the window is frozen showing this swapped frame until it returns). */
    if (g_hot_state == HOT_BUILD_PENDING) {
        g_hot_state = HOT_BUILDING;
        hot_run_build();
        g_hot_state = HOT_IDLE;
        glutPostRedisplay();
    }
#endif
}

static void reshape_func(int w, int h) {
    g_window_w = w;
    g_window_h = h;
    glutPostRedisplay();
}

static void idle_func(void) {
#if RENDER3D_HOT_RELOAD
    hot_poll_sources();
#endif
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
                (Render3dBackdropMode)((g_backdrop_mode + 1) % RENDER3D_BACKDROP_COUNT);
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
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH | GLUT_STENCIL |
                        GLUT_MULTISAMPLE);
    glutInitWindowSize(g_window_w, g_window_h);
    glutCreateWindow("scene-module teapot demo");

    glEnable(GL_DEPTH_TEST);

#if RENDER3D_HOT_RELOAD
    /* Load the reloadable src/render3d library before its first use, then
     * arm the watcher against the current source state so we only rebuild on
     * an actual edit. */
    if (!hot_load()) {
        fprintf(stderr, "[hot] initial load of %s failed - run `make render3d-hot` first\n",
                RENDER3D_HOT_LIB_PATH);
        return 1;
    }
    g_hot_src_mtime = hot_newest_mtime(RENDER3D_HOT_SRC_DIR);
    fprintf(stderr, "[hot] watching %s - edit any .c/.h and save to reload live\n",
            RENDER3D_HOT_SRC_DIR);
#endif

    render3d_init_gl();
    render3d_state_init(&g_scene_renderer);

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
