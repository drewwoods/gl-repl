/*
 * tools/repl_live_demo/repl_live_demo.c -- live, file-watching REPL demo.
 *
 * The *composition* counterpart to tools/repl_demo: where repl_demo proves the
 * REPL language pipeline links with no editor / controller / UI, this demo
 * proves the REPL pipeline and the variable-panel peer can be wired together by
 * a one-file host controller under a realistic external-editor workflow.
 *
 *   - The editor is external (vim, or anything). This demo never edits text.
 *   - Scenes are separate files, listed in an INI config or passed on the
 *     command line: either `.c` (the app's standalone-C save/export format) or
 *     `.glr` (the scene-source format the built-in examples under
 *     examples/scenes/ are authored in). The extension picks the loader, and
 *     each format gets the loader the app itself uses for it - see
 *     path_is_glr(), which is also where the two are contrasted.
 *   - The demo loads the active scene via repl_export_load_from_file() (`.c`)
 *     or repl_load_example_lines() (`.glr`), applies its `// camera` block
 *     through a demo-local ReplExportCameraBridge, runs the executor each frame
 *     under a manual orbit camera, and surfaces the scene's predefined
 *     variables in the floating slider panel (drag = live geometry).
 *   - It polls the active file's mtime and re-imports on save, warning to the
 *     terminal on parse errors.
 *
 * Reload is NOT transactional: the load sequence resets live state first, and
 * neither loader rolls back. They fail differently, and that is the loaders'
 * behavior showing through, not a demo policy: repl_export_load_from_file()
 * records per-line parse failures as warnings and still succeeds if the file
 * merely opened (a `.c` save lands partially), while the example loader rejects
 * the whole body on the first bad line (a `.glr` save lands empty). Either way
 * a malformed save can replace a good scene -- the file is the source of truth;
 * the user fixes/undoes in their editor. The demo's job is diagnostic clarity:
 * every reload prints a banner, and the per-line warnings / parse errors both
 * loaders emit go to stderr, so a partial or dropped load is never silent.
 *
 * Link set (Makefile REPL_LIVE_DEMO_DEP_SRCS): the full repl_demo pipeline set
 * (incl. tools/repl_demo/source_document.c, the editor-free static line store)
 * plus the four variable-panel TUs. No src/editor, src/app, src/render3d, or
 * src/ui/app. check-repl-live-demo-no-editor enforces the editor exclusion.
 *
 * Run:
 *   make repl-live-demo && ./repl_live_demo            # default INI
 *   ./repl_live_demo path/to/config.ini                # explicit INI
 *   ./repl_live_demo scene_a.c scene_b.glr             # bypass the INI
 *   ./repl_live_demo examples/scenes/torus.glr         # edit a built-in example
 *   ./repl_live_demo --dump-code scene.glr             # round-trip to stdout
 *   make repl-live-demo USE_GL_STUBS=1                 # headless link/isolation
 */
#include "gl_includes.h"

#include "config.h"
#include "repl/eval.h"            /* g_predef_vars_mut, repl_eval_find_predef_var_idx */
#include "repl/example_loader.h"  /* repl_load_example_lines, EXAMPLE_BODY_LINES_MAX */
#include "repl/executor.h"        /* repl_execute_program, ReplExecutionOptions, point-param proc */
#include "repl/export.h"          /* repl_export_load_from_file, ReplExportCameraBridge */
#include "repl/flatten.h"         /* FlatProgramView */
#include "repl/host_effects.h"
#include "repl/pipeline.h"        /* repl_flatten_commands, repl_apply_init_bootstrap */
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
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>               /* getpid - --dump-code temp file */

/* --- Config / scene list ------------------------------------------------- */

enum { MAX_SCENES = 32, PATH_MAX_LEN = 1024 };

static char  g_scene_paths[MAX_SCENES][PATH_MAX_LEN];
static int   g_scene_count;
static int   g_active_scene;
static time_t g_active_mtime;

static int   g_window_w = 960;
static int   g_window_h = 352;
static int   g_poll_ms  = 250;
static int   g_panel_on = 1;

/* --- Animation + camera state ------------------------------------------- */

/* The animation clock `t` is NOT a separate demo variable: it lives in the
 * REPL predef table and is advanced in place by frame_timer, exactly like the
 * app. That way a scene's own `t = 0` reset - applied during flatten/execute -
 * sticks and `t` continues from there, instead of being clobbered by a
 * demo-side clock. */
#define DEMO_FRAME_DT GLR_FRAME_DT_SECS
#define DEMO_FRAME_DT_MS_EXACT (1000.0 / 60.0)

static int   g_playing = 1;

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
/* Display row showing `t`, for the panel's frame stepper. Not the predef slot:
 * rebuild_variable_rows compacts the table, so the two can disagree. -1 when
 * there is no clock row, which build_panel_view must set explicitly - a
 * zeroed view would claim row 0 is the clock. */
static int g_time_row = -1;
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
 * A pose adapter and nothing else. The demo used to carry a hand-copied clone
 * of the app's import parser - a copy that drifted by construction - and that
 * is gone: parsing a camera out of a file is src/repl/camera_header.c's job on
 * every path, including this one. What is left writes a resolved pose into the
 * demo's orbit variables and formats them back out for the 'e' round-trip
 * export.
 */
static void demo_cam_capture_pose(ReplCameraPose *out) {
    out->dist = g_cam_dist;
    out->rx   = g_cam_rx;
    out->ry   = g_cam_ry;
    out->tx   = g_cam_tx;
    out->ty   = g_cam_ty;
    out->tz   = g_cam_tz;
}

/* No easing and no scene default in the demo, so every mode lands the same
 * way; the parameter stays for interface parity with the app bridge. */
static void demo_cam_apply_pose(const ReplCameraPose *pose,
                                ReplCameraApplyMode mode) {
    (void)mode;
    g_cam_dist = pose->dist;
    g_cam_rx   = pose->rx;
    g_cam_ry   = pose->ry;
    g_cam_tx   = pose->tx;
    g_cam_ty   = pose->ty;
    g_cam_tz   = pose->tz;
}

/* Write side (used by the 'e' round-trip export): emit the tagged camera rows
 * from the demo's live orbit camera, so a re-exported file carries the view
 * the user is looking at. `with_anim_hook` adds the exported-C `@camera spin`
 * row; the demo's own .glr writes never want it. */
static void demo_cam_fill_block(ReplExportCameraBlock *block,
                                int with_anim_hook) {
    memset(block, 0, sizeof(*block));
    snprintf(block->lines[0], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(0.0000f, 0.0000f, %.4ff);   // @camera dist",
             -g_cam_dist);
    snprintf(block->lines[1], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(%.4ff, 1.0f, 0.0f, 0.0f);   // @camera rx", g_cam_rx);
    snprintf(block->lines[2], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(%.4ff, 0.0f, 1.0f, 0.0f);   // @camera ry", g_cam_ry);
    if (with_anim_hook)
        snprintf(block->lines[3], REPL_EXPORT_CAMERA_LINE_MAX,
                 "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);   // @camera spin");
    snprintf(block->lines[4], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(%.4ff, %.4ff, %.4ff);   // @camera pan",
             -g_cam_tx, -g_cam_ty, -g_cam_tz);
    block->present = 1;
}

static const ReplExportCameraBridge g_cam_bridge = {
    .fill_block   = demo_cam_fill_block,
    .capture_pose = demo_cam_capture_pose,
    .apply_pose   = demo_cam_apply_pose,
};

/* --- Variable panel wiring ---------------------------------------------- */

/* Rebuild the slider rows from the live predefined-variable table after each
 * import. Row value pointers reference g_predef_vars_mut so the panel reads (and
 * drags write) the live values. The animation clock `t` is included like any
 * other variable; the demo advances `t` in place (frame_timer), so a scene's own
 * `t = 0` reset sticks and dragging the t row simply scrubs that same clock. */
static void rebuild_variable_rows(void) {
    ReplVariableView v = repl_state_variables();
    int i;
    g_row_count = 0;
    g_time_row = -1;
    for (i = 0; i < v.var_count && g_row_count < UI_VARIABLE_PANEL_MAX_ROWS; i++) {
        const char *name = v.vars[i].name;
        int idx;
        if (!name[0]) continue;
        idx = repl_eval_find_predef_var_idx(name);
        if (idx < 0) continue;
        snprintf(g_rows[g_row_count].name, sizeof(g_rows[g_row_count].name), "%s", name);
        if (strcmp(name, "t") == 0)
            g_time_row = g_row_count;
        g_rows[g_row_count].value = &g_predef_vars_mut[idx].value;
        g_rows[g_row_count].tuned = 0;
        g_rows[g_row_count].is_bool = 0;
        g_rows[g_row_count].written = 0;
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
        /* `t` is just another predef slot here; frame_timer advances it in place,
         * so dragging the t row scrubs the clock and playback continues from
         * the scrubbed value with no special-casing. */
        g_predef_vars_mut[idx].value = chg->value;
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
    v.collapsed = variable_panel_collapsed();
    ui_variable_panel_size(g_row_count, v.collapsed, &pw, &ph);
    v.panel_x = g_window_w - pw - 8;
    v.panel_y = 25;
    v.vars            = g_rows;
    v.var_count       = g_row_count;
    v.drag_active_var = variable_panel_drag_active_var();
    v.drag_coarse     = variable_panel_drag_coarse();
    v.time_row        = g_time_row;
    v.time_playing    = g_playing;
    return v;
}

/* --- Import + file watching --------------------------------------------- */

static const char *scene_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + (ls - lf), suffix) == 0;
}

/* Which loader a scene gets. A `.glr` is scene source - the same text the
 * built-in examples under examples/scenes/ are authored in - so it loads
 * through the example loader, not the exported-C importer. The two are not
 * interchangeable: the example loader consumes the `@cfg` + `// camera`
 * headers and emits the body in two passes (func defs first), which is what
 * lets a scene call a function or read a `static float` declared further down
 * the file. The importer's raw-scene mode feeds lines in file order, so those
 * forward references fail to parse. The extension also picks the 'e'
 * round-trip writer. */
static int path_is_glr(const char *path) {
    return ends_with(path, ".glr");
}

static int file_mtime(const char *path, time_t *out) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    *out = st.st_mtime;
    return 1;
}

/* Read a .glr into the NULL-terminated line array repl_load_example_lines()
 * takes, mirroring the runtime example catalog's own reader (newline stripped,
 * EXAMPLE_BODY_LINES_MAX rows - the authoring cap a built-in example is held
 * to). Over-long files and over-long lines are truncated with a warning rather
 * than refused: the demo's contract is that a bad save degrades visibly, never
 * silently. */
static char  g_glr_lines[EXAMPLE_BODY_LINES_MAX][MAX_LINE_LEN];
static const char *g_glr_line_ptrs[EXAMPLE_BODY_LINES_MAX + 1];

static const char *const *read_glr_lines(const char *path) {
    FILE *f = fopen(path, "r");
    char buf[MAX_LINE_LEN];
    int n = 0;

    if (!f) return NULL;
    while (fgets(buf, sizeof buf, f)) {
        char *nl = strchr(buf, '\n');
        if (n >= EXAMPLE_BODY_LINES_MAX) {
            fprintf(stderr, "[repl_live_demo] %s: over %d lines, truncated\n",
                    path, EXAMPLE_BODY_LINES_MAX);
            break;
        }
        if (nl) {
            *nl = '\0';
        } else if (!feof(f)) {
            /* Line longer than the buffer: drop the tail rather than let the
             * remainder come back as a second, syntactically bogus line. */
            int c;
            fprintf(stderr, "[repl_live_demo] %s:%d: line over %d chars, truncated\n",
                    path, n + 1, (int)sizeof buf - 1);
            while ((c = fgetc(f)) != EOF && c != '\n')
                ;
        }
        snprintf(g_glr_lines[n], MAX_LINE_LEN, "%s", buf);
        g_glr_line_ptrs[n] = g_glr_lines[n];
        n++;
    }
    fclose(f);
    g_glr_line_ptrs[n] = NULL;
    return g_glr_line_ptrs;
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

    if (path_is_glr(path)) {
        const char *const *lines = read_glr_lines(path);
        ok = lines && repl_load_example_lines(lines) > 0;
        if (!lines)
            fprintf(stderr, "[repl_live_demo] error: could not open %s\n", path);
        else if (!ok)
            fprintf(stderr, "[repl_live_demo] error: %s did not load "
                            "(see the parse error above)\n", path);
        else
            /* The importer prints its own load summary; the example loader
             * does not, so print the matching one here. */
            fprintf(stderr, "Loaded %d commands from %s\n",
                    repl_state_document_count(), path);
        /* The example loader owns the cursor and reports the post-load
         * position; the demo's own copy has to follow it. */
        g_edit_line = repl_state_document_count();
        repl_dispatch_edit_line_set(g_edit_line);
    } else {
        memset(&result, 0, sizeof result);
        ok = repl_export_load_from_file(path, &result);
        if (!ok)
            fprintf(stderr, "[repl_live_demo] error: could not open %s\n", path);
    }

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

/* Write the live REPL state through the same writers as gl-repl's Ctrl+S,
 * shared by the 'e' key and --dump-code. The writer follows the *source*
 * extension - a `.glr` scene re-exports as scene source (repl_export_save_glr,
 * the examples/scenes/ format) and anything else as standalone C - so a diff
 * against the imported file stays like-for-like. The demo installs the camera
 * write-bridge (above) so the view is captured, but not the config /
 * projection / light bridges, so `@cfg` rows are absent and those C lines fall
 * back to the exporter's defaults. */
static int write_active_scene(const char *out_path) {
    if (path_is_glr(g_scene_paths[g_active_scene]))
        return repl_export_save_glr(out_path, source_document_view());
    {
        ReplExportLayout layout;
        layout.render3d_w = g_window_w;
        layout.render3d_h = g_window_h;
        return repl_export_save_output(out_path, source_document_view(), &layout);
    }
}

/* Round-trip aid ('e' key): re-export the live REPL state next to the CWD as
 * ./<scene>.roundtrip.<ext>, so you can diff it against the imported source to
 * verify the import/export round-trips. */
static void export_active_scene(void) {
    char out[PATH_MAX_LEN + 16];
    const char *path;
    int rc;
    if (g_scene_count == 0) return;
    path = g_scene_paths[g_active_scene];
    snprintf(out, sizeof out, "%s.roundtrip.%s", scene_basename(path),
             path_is_glr(path) ? "glr" : "c");
    rc = write_active_scene(out);
    fprintf(stderr,
            "[repl_live_demo] %s ./%s (%d cmds) -- diff against %s\n",
            rc ? "exported ->" : "export FAILED:", out,
            repl_state_document_count(), g_scene_paths[g_active_scene]);
}

/* --dump-code: the 'e' round-trip with no window and no file left behind -
 * the exported text goes to stdout, so `./repl_live_demo --dump-code s.glr |
 * diff - s.glr` is the whole round-trip check. Same writers as export_active_scene
 * (and therefore the same missing bridges: no `@cfg` rows), so what lands here is
 * exactly what the 'e' key writes. The writers are filename-based, so the dump
 * goes through a temp file rather than a second, drifting FILE* path. */
static int dump_active_scene(void) {
    char tmp[PATH_MAX_LEN];
    const char *dir = getenv("TMPDIR");
    FILE *f;
    int c;

    if (g_scene_count == 0) return 0;
    snprintf(tmp, sizeof tmp, "%s/repl_live_demo_dump_%ld.%s",
             (dir && dir[0]) ? dir : "/tmp", (long)getpid(),
             path_is_glr(g_scene_paths[g_active_scene]) ? "glr" : "c");
    if (!write_active_scene(tmp)) {
        fprintf(stderr, "[repl_live_demo] error: could not write %s\n", tmp);
        return 0;
    }
    f = fopen(tmp, "r");
    if (!f) {
        fprintf(stderr, "[repl_live_demo] error: could not read back %s\n", tmp);
        return 0;
    }
    while ((c = fgetc(f)) != EOF)
        fputc(c, stdout);
    fclose(f);
    remove(tmp);
    return 1;
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

/* The app's default light theme (LIGHT_THEME_DEFAULT), reproduced verbatim so
 * lit scenes render - and re-export ('e') - with the SAME lights gl-repl uses,
 * not the GL defaults (or zeros, which is what an export with no light bridge
 * emits). These are exactly the per-slot values a real export's init() carries.
 * Single source of truth for both setup_frame_lights() (live render) and the
 * export light bridge (round-trip 'e'). Positions are world-space (applied after
 * the camera). */
enum { DEMO_LIGHT_COUNT = 4 };  /* GL_LIGHT0..3 - mirrors the app's MAX_LIGHTS */

typedef struct {
    GLfloat pos[4];
    GLfloat diffuse[4];
    GLfloat ambient[4];
    GLfloat specular[4];
} DemoLight;

static const GLfloat g_light_model_ambient[4] = { 0.15f, 0.15f, 0.20f, 1.0f };

static const DemoLight g_demo_lights[DEMO_LIGHT_COUNT] = {
    { .pos      = {  2.0f,  4.0f,  5.0f, 0.0f },
      .diffuse  = { 0.80f, 0.80f, 0.75f, 1.0f },
      .ambient  = { 0.10f, 0.10f, 0.12f, 1.0f },
      .specular = { 1.00f, 1.00f, 0.95f, 1.0f } },
    { .pos      = { -3.0f,  2.0f, -2.0f, 1.0f },
      .diffuse  = { 0.45f, 0.30f, 0.15f, 1.0f },
      .ambient  = { 0.05f, 0.03f, 0.02f, 1.0f },
      .specular = { 0.30f, 0.20f, 0.10f, 1.0f } },
    { .pos      = {  0.0f, -1.0f,  3.0f, 1.0f },
      .diffuse  = { 0.15f, 0.25f, 0.50f, 1.0f },
      .ambient  = { 0.02f, 0.03f, 0.06f, 1.0f },
      .specular = { 0.10f, 0.15f, 0.35f, 1.0f } },
    { .pos      = {  1.0f,  1.0f, -4.0f, 0.0f },
      .diffuse  = { 0.35f, 0.35f, 0.40f, 1.0f },
      .ambient  = { 0.05f, 0.05f, 0.06f, 1.0f },
      .specular = { 0.20f, 0.20f, 0.25f, 1.0f } },
};

/* The per-frame state the *host* owns, and nothing else. It is a short list on
 * purpose - the material/clear baseline a scene needs (glClear,
 * GL_COLOR_MATERIAL + its mode, two-sided lighting, specular, shininess) is
 * ordinary editable program text, seeded into new documents by
 * repl_load_default_display_baseline() and written into every .glr by hand. A
 * host that re-asserted it each frame would keep a scene that dropped those
 * lines looking correct here and wrong everywhere else, which is exactly the
 * authoring feedback this demo exists to give. Depth test is scene text too.
 *
 * What is left mirrors render3d_pass_setup(): lights (positions after the
 * camera, so they land in world space), the global ambient, and lighting off
 * as a baseline - execute_commands() turns it on if the program says so. The
 * one-shot half of the host baseline (clear color, blending, point
 * attenuation) is repl_apply_init_bootstrap(), applied once at startup, same
 * as the app and as an export's init(). */
static void setup_frame_lights(void) {
    int i;
    glDisable(GL_LIGHTING);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, g_light_model_ambient);

    for (i = 0; i < DEMO_LIGHT_COUNT; i++) {
        GLenum lid = (GLenum)(GL_LIGHT0 + i);
        glLightfv(lid, GL_POSITION, g_demo_lights[i].pos);
        glLightfv(lid, GL_DIFFUSE,  g_demo_lights[i].diffuse);
        glLightfv(lid, GL_AMBIENT,  g_demo_lights[i].ambient);
        glLightfv(lid, GL_SPECULAR, g_demo_lights[i].specular);
    }
}

/* Export light bridge ('e' round-trip): without it, repl_export_save_output
 * emits zeroed light colors. Report the same per-slot values setup_frame_lights
 * applies, so the exported standalone C lights the scene the way the demo does. */
static void demo_light_fill_slot(int slot, ReplExportLightInfo *out) {
    int i;
    if (slot < 0 || slot >= DEMO_LIGHT_COUNT) { memset(out, 0, sizeof(*out)); return; }
    for (i = 0; i < 4; i++) {
        out->pos[i]      = g_demo_lights[slot].pos[i];
        out->diffuse[i]  = g_demo_lights[slot].diffuse[i];
        out->ambient[i]  = g_demo_lights[slot].ambient[i];
        out->specular[i] = g_demo_lights[slot].specular[i];
    }
    out->pos_is_eye_space = 0;  /* world-space, set after the camera */
}
static const ReplExportLightBridge g_light_bridge = { demo_light_fill_slot };

static void draw_text(int x, int y, const char *s) {
    glRasterPos2i(x, y);
    while (*s) {
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, (int)(unsigned char)*s);
        s++;
    }
}

/* Opaque backing strip behind a HUD row. Nothing clears the window on the
 * program's behalf (see display_func), so geometry - or last frame's smear -
 * runs straight under the text; the strip is what keeps it legible. Drawn in
 * the same 2D ortho space as the text, so y is the window-space bottom edge. */
static void draw_hud_strip(int y, int h) {
    glColor3f(0.05f, 0.06f, 0.08f);
    glBegin(GL_QUADS);
      glVertex2i(0, y);
      glVertex2i(g_window_w, y);
      glVertex2i(g_window_w, y + h);
      glVertex2i(0, y + h);
    glEnd();
}

static void draw_hud(void) {
    char buf[320];
    glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0.0, (double)g_window_w, 0.0, (double)g_window_h);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    draw_hud_strip(g_window_h - 24, 24);
    draw_hud_strip(0, 24);

    {
        int t_idx = repl_eval_find_predef_var_idx("t");
        float tval = (t_idx >= 0) ? g_predef_vars_mut[t_idx].value : 0.0f;
        glColor3f(0.92f, 0.94f, 1.0f);
        snprintf(buf, sizeof buf, "scene %d/%d  %s   t=%.2f%s   cmds=%d  vars=%d",
                 g_active_scene + 1, g_scene_count,
                 scene_basename(g_scene_paths[g_active_scene]),
                 tval, g_playing ? "" : " (paused)",
                 repl_state_document_count(), g_row_count);
        draw_text(10, g_window_h - 18, buf);
    }

    glColor3f(0.62f, 0.72f, 0.84f);
    draw_text(10, 12,
        "[ ] cycle   r reload   e export   v panel   space pause   "
        "LMB orbit  RMB pan  wheel zoom   q quit");

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
}

static void display_func(void) {
    ReplExecutionOptions opts;

    /* `t` lives in the predef table and is advanced in place by frame_timer; just
     * re-bake so has_vars expressions pick up the new value (and any `t = ...`
     * the scene applied), exactly like the app's per-frame reflatten. */
    repl_state_mark_flat_dirty();
    repl_flatten_commands(repl_dispatch_edit_line_get());

    /* No host clear: the scene rect is cleared by the *program*, exactly as in
     * gl-repl and in the exported C. A scene whose glClear is missing or
     * commented out smears, and that is the feedback an authoring window owes
     * you - a host-side clear would run whether or not the source said so. */
    apply_projection();
    apply_camera_modelview();
    setup_frame_lights();

    memset(&opts, 0, sizeof opts);
    opts.flat_cmd_count = repl_state_flat_program_count();
    opts.program        = repl_state_flat_program_view();
    /* Bracketing the program the way render3d_pass_setup and the exported
     * display() do: whatever state the scene leaves set is undone before the
     * HUD draws and before the next frame, so nothing leaks across frames. */
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    repl_execute_program(&opts);
    glPopAttrib();

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
}

static double demo_timer_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* Move the clock by `dt`. The timer's per-frame advance and the panel's frame
 * stepper both go through here so a stepped frame is the same size as a played
 * one. This demo drives `t` directly rather than through repl_state_time_*:
 * it owns its own play flag and re-flattens every frame regardless. */
static void nudge_animation_clock(float dt) {
    int t_idx = repl_eval_find_predef_var_idx("t");
    if (t_idx >= 0)
        g_predef_vars_mut[t_idx].value += dt;
}

static void advance_animation_clock(void) {
    if (g_playing)
        nudge_animation_clock(DEMO_FRAME_DT);
}

static void frame_timer(int value) {
    (void)value;
    static double next_deadline_ms = 0.0;

    advance_animation_clock();
    glutPostRedisplay();

    double now = demo_timer_now_ms();
    if (next_deadline_ms == 0.0)
        next_deadline_ms = now;
    next_deadline_ms += DEMO_FRAME_DT_MS_EXACT;
    if (next_deadline_ms < now)
        next_deadline_ms = now;

    int delay = (int)lround(next_deadline_ms - now);
    if (delay < 1)
        delay = 1;
    glutTimerFunc((unsigned int)delay, frame_timer, 0);
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
                int coarse = (button == GLUT_RIGHT_BUTTON) ? 1 : 0;
                variable_panel_handle_drag_begin(hit.item_idx, coarse, x);
                g_slider_drag = 1;
                return;
            } else if (hit.kind == UI_HIT_VARIABLE_TIME_STEP) {
                /* One frame per click, ten on a right-press - the same coarse
                 * modifier the slider drag uses, and what the app does. */
                float frames = (button == GLUT_RIGHT_BUTTON)
                             ? GLR_ADJUST_COARSE_SCALE : 1.0f;
                nudge_animation_clock((hit.item_idx > 0 ? 1.0f : -1.0f) *
                                      frames * DEMO_FRAME_DT);
                glutPostRedisplay();
                return;
            } else if (hit.kind == UI_HIT_VARIABLE_COLLAPSE_TOGGLE &&
                       button == GLUT_LEFT_BUTTON) {
                variable_panel_toggle_collapsed();
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
}

static void motion_func(int x, int y) {
    if (g_slider_drag) {
        VariablePanelValueChange chg;
        if (variable_panel_handle_drag_motion(x, &chg))
            apply_var_change(&chg);
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
    case 'e': case 'E':
        export_active_scene();
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
"Usage: %s [--dump-code] [config.ini | scene_a.c scene_b.glr ...]\n"
"\n"
"Live, file-watching REPL demo. Imports scene files -- .c (the app's standalone\n"
"C save/export format) or .glr (the scene source the built-in examples under\n"
"examples/scenes/ are written in) -- watches their mtime, re-imports on save,\n"
"and drives predefined-variable sliders live. Edit the scene files in your own\n"
"editor (vim, ...).\n"
"\n"
"  (no args)         Load the default INI (repl_live_demo.ini, or the bundled\n"
"                    tools/repl_live_demo/repl_live_demo.ini).\n"
"  config.ini        Load scenes + options from this INI.\n"
"  *.c / *.glr       Use these scene files directly (bypass the INI).\n"
"  --dump-code       Import the first scene, write what the 'e' key would\n"
"                    export to stdout, and exit. No window, no GLUT init.\n"
"                    `--dump-code s.glr | diff - s.glr` is the round-trip check.\n"
"\n"
"Keys:  [ ] cycle scene   r reload   e export (round-trip)   v toggle panel\n"
"       space pause   q/Esc quit   LMB orbit  RMB pan  wheel zoom  drag a slider\n",
        prog);
}

int main(int argc, char **argv) {
    const char *ini_path = NULL;
    int got_files = 0;
    int dump_code = 0;
    int i;

    /* --dump-code is a headless leg: no window, and no glutInit() either -
     * that reaches the window server (and fails outright on a Linux box with
     * no DISPLAY), which a text dump has no business needing. Scan for the
     * flag before GLUT ever sees argv. */
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--dump-code") == 0)
            dump_code = 1;

    if (!dump_code)
        glutInit(&argc, argv);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--dump-code") == 0) {
            continue;  /* handled above, before glutInit */
        } else if (ends_with(a, ".ini")) {
            ini_path = a;
        } else if (ends_with(a, ".c") || path_is_glr(a)) {
            add_scene_path(a);
            got_files = 1;
        } else {
            fprintf(stderr, "[repl_live_demo] ignoring argument '%s'\n", a);
        }
    }

    if (!got_files && !load_default_ini(ini_path)) {
        fprintf(stderr,
            "[repl_live_demo] no scenes: pass .c / .glr files or an INI with `scene=` lines\n"
            "  (looked for %s).\n",
            ini_path ? ini_path : "repl_live_demo.ini / tools/repl_live_demo/repl_live_demo.ini");
        print_usage(argv[0]);
        return 2;
    }

    repl_install_host_effects(&g_host_effects);
    repl_export_install_camera_bridge(&g_cam_bridge);
    repl_export_install_light_bridge(&g_light_bridge);
    variable_panel_state_reset();
    variable_panel_install_value_source(&g_value_source);

    if (dump_code) {
        import_active_scene();
        return dump_active_scene() ? 0 : 1;
    }

    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(g_window_w, g_window_h);
    glutCreateWindow("repl_live_demo");

    /* The one-shot host baseline: clear color, src-over blending, point
     * attenuation - the same commands the app applies at startup and an
     * export's init() carries, straight out of src/repl rather than
     * hand-copied here. After install_point_parameter_proc(), which is what
     * tells the attenuation entry whether the entry point exists. */
    install_point_parameter_proc();
    /* The GLU tessellator the executor uses for glTess* blocks: every tess
     * opcode is a silent no-op until this allocates it. */
    repl_executor_init_resources();
    repl_apply_init_bootstrap();

    variable_panel_set_visible(g_panel_on);

    import_active_scene();

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutKeyboardFunc(keyboard_func);
    glutMouseFunc(mouse_func);
    glutMotionFunc(motion_func);
    glutMouseWheelFunc(mousewheel_func);
    glutTimerFunc(16, frame_timer, 0);
    glutTimerFunc((unsigned int)g_poll_ms, watch_timer, 0);

    printf("repl_live_demo: watching %d scene(s); edit them in your editor and save.\n",
           g_scene_count);
    printf("  keys: [ ] cycle  r reload  e export  v panel  space pause  q quit\n");

    glutMainLoop();
    return 0;
}
