/*
 * repl_core.c — Parser and supporting infrastructure.
 *
 * Division of labor
 * -----------------
 * This file owns everything that transforms text into renderable GL state:
 *
 *   - Parser/control-flow state that is not pure command metadata
 *   - Global state: command arrays (g_cmds / g_flat_cmds), camera, toggles,
 *     accumulation-buffer settings, etc.
 *   - Parsing      — parse_command(): text → GLCmd
 *   - Normalization — repl_parse_and_normalize(), repl_reformat_commands()
 *   - Auto-normals — recompute_autonormals()
 *   - GLUT display / reshape callbacks
 *   - Example / user-scene management
 *   - Depth-cache  — prefix-sum arrays for O(1) block-depth queries
 *   - 2D helpers   — draw_string(), draw_quad(), begin_2d(), end_2d()
 *   - Public API wrappers forwarded from sample.c
 *
 * repl_editor.c owns the interactive editing layer:
 *   - Editor state (g_input, cursor, undo/redo ring, clipboard, selection)
 *   - Commit handlers that decide *where* a parsed command goes in g_cmds[]
 *   - GLUT keyboard / special / mouse / motion / timer dispatch
 *   - Camera momentum and panel resizing
 *   - feed_line() — the programmatic commit entry point used by file loading
 *     and test harnesses
 *
 * Other translation units:
 *   repl_eval.c    — expression evaluator, for-loop header parsers
 *   repl_export.c  — save / load  (output.c round-tripping)
 *   repl_search.c  — incremental search overlay
 *   cmd_format.c   — source-text formatting helpers
 *   repl_flatten.c — flatten_range() / flatten_commands()
 *   repl_executor.c— repl_execute_program() / execute_commands()
 *   repl_autocomplete.c — completions and parameter hints
 *   repl_replay.c  — replay state machine and fade-batch rendering
 *   scene_render.c — 3D scene setup, grid / axes / overlay drawing
 *   ui_panels.c    — code panel, autocomplete popup, config menu, var panel
 *   repl_examples.c— predefined example scene data
 */

#include "sample.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_command_spec.h"
#include "repl_command_store.h"
#include "repl_replay.h"
#include "cmd_format.h"
#include "repl_examples.h"
#include "scene_render.h"
#include "ui_panels.h"
#include "profile_panel.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

static const char *outfile = "output.c";

/* ========================================================================= */
/* Global state                                                               */
/* ========================================================================= */

GLCmd  g_cmds[MAX_COMMANDS];
int    g_num_cmds = 0;
int    g_normals_dirty = 1;
GLCmd           g_flat_cmds[MAX_COMMANDS];
int             g_num_flat_cmds = 0;
int             g_flat_dirty = 1;
FlatCmdLocalVars g_flat_cmd_local_vars[MAX_COMMANDS];

/* Lightweight prefix-depth caches for O(1) depth lookups at position `pos`. */
static int g_depth_cache_dirty = 1;
static int g_for_depth_prefix[MAX_COMMANDS + 1];
static int g_block_depth_prefix[MAX_COMMANDS + 1];
static int g_begin_depth_prefix[MAX_COMMANDS + 1];
static int g_tess_depth_prefix[MAX_COMMANDS + 1];

void depth_cache_invalidate(void) {
    g_depth_cache_dirty = 1;
}

/* Rebuild prefix-sum depth arrays so that depth_prefix[pos] gives the
 * nesting depth *before* command `pos`.  Each array tracks one kind of
 * scope opener/closer:
 *
 *   g_for_depth_prefix   — for-loop nesting only
 *   g_block_depth_prefix — any block (for/func/if) nesting (used for indent)
 *   g_begin_depth_prefix — glBegin/glEnd nesting
 *   g_tess_depth_prefix  — gluBegin/gluEnd nesting
 *
 * All queries (block_depth_at, in_begin_block_at, etc.) call this first;
 * the dirty flag is set by depth_cache_invalidate(). */
static void depth_cache_rebuild(void) {
    if (!g_depth_cache_dirty) return;

    g_for_depth_prefix[0] = 0;
    g_block_depth_prefix[0] = 0;
    g_begin_depth_prefix[0] = 0;
    g_tess_depth_prefix[0] = 0;

    for (int i = 0; i < g_num_cmds; i++) {
        int for_depth   = g_for_depth_prefix[i];
        int block_depth = g_block_depth_prefix[i];
        int begin_depth = g_begin_depth_prefix[i];
        int tess_depth  = g_tess_depth_prefix[i];

        if (g_cmds[i].valid) {
            CmdType t = g_cmds[i].type;

            if (t == CMD_FOR_BEGIN) for_depth++;
            else if (t == CMD_FOR_END) for_depth--;

            if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) block_depth++;
            else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) block_depth--;

            if (t == CMD_BEGIN) begin_depth++;
            else if (t == CMD_END) begin_depth--;

            if (t == CMD_TESS_BEGIN_POLYGON || t == CMD_TESS_BEGIN_CONTOUR) tess_depth++;
            else if (t == CMD_TESS_END) tess_depth--;
        }

        if (for_depth < 0)   for_depth = 0;
        if (block_depth < 0) block_depth = 0;
        if (begin_depth < 0) begin_depth = 0;
        if (tess_depth < 0)  tess_depth = 0;

        g_for_depth_prefix[i + 1]   = for_depth;
        g_block_depth_prefix[i + 1] = block_depth;
        g_begin_depth_prefix[i + 1] = begin_depth;
        g_tess_depth_prefix[i + 1]  = tess_depth;
    }

    g_depth_cache_dirty = 0;
}

void mark_normals_dirty(void) {
    g_normals_dirty = 1;
    g_flat_dirty = 1;
    depth_cache_invalidate();
}

/* Predefined variables — defined in repl_eval.c */

/* (no display list - commands are executed directly each frame) */

/* Camera */
float  g_cam_rx = 20.0f;
float  g_cam_ry = 30.0f;
float  g_cam_dist = 5.0f;
float  g_cam_tx = 0.0f, g_cam_ty = 0.0f, g_cam_tz = 0.0f;
float  g_cam_motion_glow = 0.0f;  /* 0..1, pulses to 1 on camera input, decays each tick */

/* Window */
int    g_win_w = 1200, g_win_h = 800;

/* Accumulation buffer — enabled by default, disabled with --noaccum.
 * Designed to be forward-compatible with FBO-based accumulation later. */
int    g_use_accum        = 1;  /* GLUT_ACCUM requested at init */
int    g_accum_aa_enabled = 1;  /* Ctrl+B toggles jitter AA on/off */
int    g_accum_samples    = 2;  /* current sample count */
float  g_accum_jitter_x   = 0.0f;
float  g_accum_jitter_y   = 0.0f;
int    g_multisample_enabled = CFG_DEFAULT_MULTISAMPLE;
int    g_line_smooth_enabled = CFG_DEFAULT_LINE_SMOOTH;

/* Sub-pixel jitter offsets (units: fraction of one pixel).
 * Table is ordered so the first N entries form a good N-sample set.
 * Supports 1, 2, 4, 8, or 16 samples. */
static const float g_jitter_table[MAX_ACCUM_SAMPLES][2] = {
    {  0.250f,  0.250f },
    { -0.250f, -0.250f },/* 2  */
    {  0.250f, -0.250f },
    { -0.250f,  0.250f },  /* 4  */
    { -0.125f,  0.375f },
    {  0.375f,  0.125f },
    { -0.375f, -0.125f },
    {  0.125f, -0.375f }, /* 8  */
    {  0.375f, -0.375f },
    { -0.375f,  0.375f },
    {  0.125f,  0.125f },
    { -0.125f, -0.125f },
    {  0.375f,  0.375f },
    { -0.375f, -0.375f },
    {  0.000f,  0.500f },
    {  0.500f,  0.000f },  /* 16 */
};

/* Animation */
float  g_anim_time = 0.0f;
int    g_t_playing = 1;    /* 1: 't' var auto-increments with time; 0: frozen */
int    g_t_var_idx = -1;   /* index of "t" in g_predef_vars[], cached at init */

/* Toggles */
int    g_show_help    = 0;
int    g_help_tab     = 0;   /* 0=Commands, 1=Keys */
int    g_help_scroll  = 0;
int    g_wireframe    = CFG_DEFAULT_WIREFRAME;
/* Names must match the GridTheme enum in sample.h. */
int    g_grid_theme   = CFG_DEFAULT_GRID_THEME;
const char *g_grid_names[GRID_THEME_COUNT] = {
    [GRID_THEME_OFF]     = "OFF",
    [GRID_THEME_CLASSIC] = "Classic",
    [GRID_THEME_FOG]     = "Fog",
    [GRID_THEME_TRON]    = "Tron",
    [GRID_THEME_EMBER]   = "Ember",
    [GRID_THEME_FAINT]   = "Faint",
    [GRID_THEME_FOCUS]   = "Focus",
    [GRID_THEME_OCEAN]   = "Ocean",
    [GRID_THEME_XZRULER] = "XZ Ruler",
    [GRID_THEME_PLANES]  = "Adaptive Planes",
};

/* Grid major tick spacing in world units. Includes 1 and 5 per request;
 * 2 and 10 fill in the common orders of magnitude. The minor step is
 * derived as major * 0.2 so every major cell holds five subdivisions. */
const float g_grid_major_steps[GRID_MAJOR_COUNT] = {
    [GRID_MAJOR_1]  = 1.0f,
    [GRID_MAJOR_2]  = 2.0f,
    [GRID_MAJOR_5]  = 5.0f,
    [GRID_MAJOR_10] = 10.0f,
};
const char *g_grid_major_names[GRID_MAJOR_COUNT] = {
    [GRID_MAJOR_1]  = "1",
    [GRID_MAJOR_2]  = "2",
    [GRID_MAJOR_5]  = "5",
    [GRID_MAJOR_10] = "10",
};
int g_grid_major_idx = CFG_DEFAULT_GRID_MAJOR_IDX;

/* Grid half-extent. Close keeps the grid tight around origin (good for
 * small scenes and the Classic theme); Far lets themes like Fog and
 * Tron stretch to the horizon. */
const float g_grid_extents[GRID_EXTENT_COUNT] = {
    [GRID_EXTENT_CLOSE] = 5.0f,
    [GRID_EXTENT_MID]   = 25.0f,
    [GRID_EXTENT_FAR]   = 100.0f,
};
const char *g_grid_extent_names[GRID_EXTENT_COUNT] = {
    [GRID_EXTENT_CLOSE] = "Close",
    [GRID_EXTENT_MID]   = "Mid",
    [GRID_EXTENT_FAR]   = "Far",
};
int g_grid_extent_idx = CFG_DEFAULT_GRID_EXTENT_IDX;  /* matches pre-existing Fog extent */
float  g_focus_vtx[3] = { 0.0f, 0.0f, 0.0f };  /* last vertex pos for focus grid */
int    g_focus_vtx_valid = 0;
/* Names must match the AxesTheme enum in sample.h. */
int    g_axes_theme   = CFG_DEFAULT_AXES_THEME;
const char *g_axes_names[AXES_THEME_COUNT] = {
    [AXES_THEME_OFF]     = "OFF",
    [AXES_THEME_CLASSIC] = "Classic",
    [AXES_THEME_PULSE]   = "Pulse",
    [AXES_THEME_NEON]    = "Neon",
    [AXES_THEME_COMPASS] = "Compass",
    [AXES_THEME_GIZMO]   = "Gizmo",
};
int    g_show_vnums   = CFG_DEFAULT_VERTEX_LABELS;
int    g_show_normals = CFG_DEFAULT_NORMAL_VECTORS;
int    g_show_indices = CFG_DEFAULT_VERTEX_INDICES;
int    g_wrap_at_comma = CFG_DEFAULT_WRAP_AT_COMMA;
int    g_code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
int    g_show_guides  = CFG_DEFAULT_VERTEX_GUIDES;
int    g_xform_guide_mode = CFG_DEFAULT_XFORM_GUIDE_MODE; /* 0=World (strict OpenGL reverse-order), 1=Frame (anchor at pre-cursor translations) */
int    g_autonormal   = 0;
int    g_show_lights  = CFG_DEFAULT_LIGHT_INDICATORS;
int    g_backdrop_mode = CFG_DEFAULT_BACKDROP_MODE; /* 0=off, 1=cityscape */
int    g_cam_rotate   = CFG_DEFAULT_CAMERA_ROTATE;  /* auto-rotate camera around Y */
int    g_example_idx  = -1; /* current predefined example (-1 = none loaded yet) */
char   g_scratch_buf[256];  /* shared scratch space for formatting strings, etc. */

/* User scene — multi-slot scene storage.  Slot 0 is the "home" scene
 * (captured on first example load) and is never evicted.  Other slots
 * hold auto-promoted examples, imported workspace scenes, etc.
 *
 *   g_active_user_scene  >= 0  => that slot is loaded into g_cmds[]
 *                        == -1 => an example or fresh workspace is active
 *
 *   last_touch is bumped from the monotonic g_user_scene_tick counter
 *   on every access (load/save/rename) so LRU eviction can pick the
 *   coldest slot when the 9th scene is needed. */
typedef struct {
    int      used;
    char     name[USER_SCENE_NAME_MAX];
    uint32_t last_touch;
    GLCmd    cmds[MAX_COMMANDS];
    int      num_cmds;
    int      edit_line;
    float    predef_vals[MAX_PREDEF_VARS];
    char     predef_names[MAX_PREDEF_VARS][16];
    int      num_predef_vars;
} UserScene;

static UserScene g_user_scenes[MAX_USER_SCENES];
static int       g_active_user_scene = -1;
static uint32_t  g_user_scene_tick = 0;

/* Currently-bound workspace directory (empty = none).  Set by
 * repl_save_workspace / repl_load_workspace, persisted in every
 * exported file via `// @workspace-dir`, and read by the LRU-eviction
 * path (task #7) to decide whether a 9th scene can be flushed to disk. */
#define WORKSPACE_DIR_MAX 1024
char g_workspace_dir[WORKSPACE_DIR_MAX] = "";
int    g_user_lighting_enabled = 0; /* tracks if user typed glEnable(GL_LIGHTING) */
int    g_show_outlines = CFG_DEFAULT_VERTEX_OUTLINES; /* draw black wireframe over filled polygons */
int    g_show_vpoints  = CFG_DEFAULT_VERTEX_POINTS; /* draw black dots at each vertex position */
int    g_highlight_current_poly = 1; /* highlight glBegin block under cursor */
int    g_ortho_mode = 0;  /* 0=perspective, 1=2D orthographic */

/* GLU quadric (shared for sphere/cylinder/disk drawing) */
GLUquadric *g_quadric = NULL;

/* GLU tessellator (for concave polygon support) */
GLUtesselator *g_tess = NULL;

/* Tessellator vertex buffer (position + normal + color per vertex) */
TessVertex g_tess_verts[TESS_VERT_BUF_SIZE];
int        g_tess_vert_count = 0;

/* Lights */
SceneLight g_lights[MAX_LIGHTS] = {
    { GL_LIGHT0, 1,
      { 2.0f, 4.0f, 5.0f, 0.0f },           /* key light (directional) */
      { 0.80f, 0.80f, 0.75f, 1.0f },
      { 0.10f, 0.10f, 0.12f, 1.0f },
      { 1.0f, 1.0f, 0.95f, 1.0f } },
    { GL_LIGHT1, 1,
      { -3.0f, 2.0f, -2.0f, 1.0f },          /* warm fill (positional) */
      { 0.45f, 0.30f, 0.15f, 1.0f },
      { 0.05f, 0.03f, 0.02f, 1.0f },
      { 0.30f, 0.20f, 0.10f, 1.0f } },
    { GL_LIGHT2, 1,
      { 0.0f, -1.0f, 3.0f, 1.0f },           /* cool rim (positional) */
      { 0.15f, 0.25f, 0.50f, 1.0f },
      { 0.02f, 0.03f, 0.06f, 1.0f },
      { 0.10f, 0.15f, 0.35f, 1.0f } },
    { GL_LIGHT3, 0,
      { 1.0f, 1.0f, -4.0f, 0.0f },           /* back light (directional, off) */
      { 0.35f, 0.35f, 0.40f, 1.0f },
      { 0.05f, 0.05f, 0.06f, 1.0f },
      { 0.20f, 0.20f, 0.25f, 1.0f } },
};

/* Clear color (user-settable via glClearColor command) */
float  g_clear_color[4] = {0.10f, 0.10f, 0.13f, 1.0f};

/* Status bar */
char   g_status[256] = "";
int    g_status_ttl = 0;

int    g_cursor_px = 0;     /* screen pos of cursor, set during render */
int    g_cursor_py = 0;

/* Forward declarations (eval_expr, parse_for_header, etc. are in repl_eval.h) */
static int parse_command(const char *line, GLCmd *cmd,
                         ExprVar *vars, int num_vars);
static void get_for_var_name(const GLCmd *cmd, char *var, int var_sz);
static void load_example(int idx);

/* ========================================================================= */
/* Utility                                                                    */
/* ========================================================================= */

void set_status(const char *msg) {
    strncpy(g_status, msg, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    g_status_ttl = 240;
}

const char *mode_name(GLenum mode) {
    return repl_begin_mode_name(mode);
}

int in_begin_block_at(int pos) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > g_num_cmds) pos = g_num_cmds;
    return g_begin_depth_prefix[pos] > 0;
}

int in_begin_block(void) {
    return in_begin_block_at(g_num_cmds);
}

static int tess_scope_depth_at(int pos) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > g_num_cmds) pos = g_num_cmds;
    return g_tess_depth_prefix[pos];
}


/* Normal command indent: 2 + 2*tess + 2*begin + 2*block */
static void cmd_indent(int pos, char *buf, int buf_sz) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > g_num_cmds) pos = g_num_cmds;
    int td = g_tess_depth_prefix[pos];
    int bd = g_begin_depth_prefix[pos];
    int kd = g_block_depth_prefix[pos];
    int spaces = 2 + 2 * td + 2 * bd + 2 * kd;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

/* Returns indent character count for a normal command at pos: 2 + 2*tess + 2*begin + 2*block */
int cmd_indent_chars(int pos) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > g_num_cmds) pos = g_num_cmds;
    return 2 + 2 * g_tess_depth_prefix[pos] + 2 * g_begin_depth_prefix[pos]
             + 2 * g_block_depth_prefix[pos];
}

/* Tessellator leaf command indent: 2 + 2*tess + 2*block  (begin depth ignored) */
static void cmd_tess_indent(int pos, char *buf, int buf_sz) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > g_num_cmds) pos = g_num_cmds;
    int td = g_tess_depth_prefix[pos];
    int kd = g_block_depth_prefix[pos];
    int spaces = 2 + 2 * td + 2 * kd;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

GLenum current_begin_mode(void) {
    GLenum mode = GL_TRIANGLES;
    for (int i = 0; i < g_num_cmds; i++)
        if (g_cmds[i].valid && g_cmds[i].type == CMD_BEGIN)
            mode = g_cmds[i].mode;
    return mode;
}

int count_vertices(void) {
    int n = 0;
    for (int i = 0; i < g_num_flat_cmds; i++)
        if (g_flat_cmds[i].valid && g_flat_cmds[i].type == CMD_VERTEX3F) n++;
    return n;
}

void repl_normalize_from_parsed(const char *parsed_source,
                                const char *raw_expr,
                                int ensure_semicolon,
                                char *out, int out_sz) {
    if (out_sz <= 0) return;
    char tmp[MAX_LINE_LEN];
    fmt_reindent_from_parsed(parsed_source, raw_expr, tmp, sizeof(tmp));

    int len = (int)strlen(tmp);
    while (len > 0 && isspace((unsigned char)tmp[len - 1]))
        tmp[--len] = '\0';

    if (ensure_semicolon && len > 0) {
        char last = tmp[len - 1];
        if (last != ';' && last != ':' && last != '{' && last != '}') {
            if (len < (int)sizeof(tmp) - 1) {
                tmp[len++] = ';';
                tmp[len] = '\0';
            }
        }
    }

    strncpy(out, tmp, (size_t)out_sz - 1);
    out[out_sz - 1] = '\0';
}

const char *cmd_type_name(CmdType t) {
    return repl_cmd_type_name(t);
}

void repl_debug_dump_editor(FILE *out) {
    FILE *dst = out ? out : stdout;

    fprintf(dst, "=== REPL Editor Dump ===\n");
    fprintf(dst,
            "num_cmds=%d edit_line=%d inserting=%d flat_dirty=%d normals_dirty=%d\n",
            g_num_cmds, g_edit_line, g_inserting, g_flat_dirty, g_normals_dirty);

    for (int i = 0; i < g_num_cmds; i++) {
        const GLCmd *cmd = &g_cmds[i];
        fprintf(dst,
                "%4d | %-22s | valid=%d has_vars=%d is_auto=%d src_idx=%d | %s\n",
                i, cmd_type_name(cmd->type), cmd->valid, cmd->has_vars,
                cmd->is_auto, cmd->src_cmd_idx, cmd->source);
    }

    fprintf(dst, "--- source ---\n");
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        fprintf(dst, "%s\n", g_cmds[i].source);
    }
    fprintf(dst, "--- camera ---\n");
    fprintf(dst, "rx=%g ry=%g dist=%g tx=%g ty=%g tz=%g\n",
            (double)g_cam_rx, (double)g_cam_ry, (double)g_cam_dist,
            (double)g_cam_tx, (double)g_cam_ty, (double)g_cam_tz);
    update_cam_lines();
    for (int i = 0; i < CAM_LINE_COUNT; i++)
        fprintf(dst, "%s\n", g_cam_lines[i]);
    fprintf(dst, "--- init ---\n");
    for (int i = 0; i < init_section_line_count(); i++) {
        char line[MAX_LINE_LEN];
        init_section_line(i, line, sizeof(line));
        fprintf(dst, "%s\n", line);
    }
    fprintf(dst, "=== End REPL Editor Dump ===\n");
    fflush(dst);
}

void repl_debug_dump_flat_commands(FILE *out) {
    FILE *dst = out ? out : stdout;

    if (g_flat_dirty)
        flatten_commands();

    fprintf(dst, "=== REPL Flattened Commands Dump ===\n");
    fprintf(dst, "num_flat_cmds=%d\n", g_num_flat_cmds);

    for (int i = 0; i < g_num_flat_cmds; i++) {
        const GLCmd *cmd = &g_flat_cmds[i];
        fprintf(dst,
                "%4d | %-22s | valid=%d has_vars=%d src_idx=%d call_src_idx=%d root_call_src_idx=%d func_scope=0x%08x | %s\n",
                i, cmd_type_name(cmd->type), cmd->valid, cmd->has_vars,
                cmd->src_cmd_idx, cmd->call_src_cmd_idx,
                cmd->root_call_src_cmd_idx, cmd->func_scope_mask,
                cmd->source);
    }
    fprintf(dst, "=== End REPL Flattened Commands Dump ===\n");
    fflush(dst);
}

/* Strip leading/trailing whitespace from `raw_expr`, normalize comma
 * spacing (remove space before comma, ensure one space after), optionally
 * append a semicolon, and prepend `indent_spaces` spaces.  Used by
 * repl_parse_and_normalize() and repl_reformat_commands() to produce
 * canonical source text for a command. */
static void normalize_with_indent(const char *raw_expr, int indent_spaces,
                                  int ensure_semicolon, char *out, int out_sz) {
    if (out_sz <= 0) return;

    const char *p = raw_expr;
    while (*p == ' ' || *p == '\t') p++;

    char body[MAX_LINE_LEN];
    size_t body_len = strlen(p);
    if (body_len >= sizeof(body))
        body_len = sizeof(body) - 1;
    memcpy(body, p, body_len);
    body[body_len] = '\0';

    int len = (int)strlen(body);
    while (len > 0 && isspace((unsigned char)body[len - 1]))
        body[--len] = '\0';
    while (ensure_semicolon && len > 0 && body[len - 1] == ';')
        body[--len] = '\0';
    while (len > 0 && isspace((unsigned char)body[len - 1]))
        body[--len] = '\0';
    if (ensure_semicolon && len > 0 && len < (int)sizeof(body) - 1) {
        body[len++] = ';';
        body[len] = '\0';
    }

    /* Keep expression tokens but normalize comma delimiters for readability. */
    {
        char spaced[MAX_LINE_LEN];
        int si = 0;
        for (int i = 0; body[i] && si < (int)sizeof(spaced) - 1; i++) {
            char c = body[i];
            if (c == ',') {
                while (si > 0 && isspace((unsigned char)spaced[si - 1]))
                    si--;
                spaced[si++] = ',';
                if (si < (int)sizeof(spaced) - 1)
                    spaced[si++] = ' ';
                while (body[i + 1] && isspace((unsigned char)body[i + 1]))
                    i++;
                continue;
            }
            spaced[si++] = c;
        }
        spaced[si] = '\0';
        memcpy(body, spaced, (size_t)si + 1);
    }

    if (indent_spaces < 0) indent_spaces = 0;
    if (indent_spaces > out_sz - 1) indent_spaces = out_sz - 1;
    memset(out, ' ', (size_t)indent_spaces);
    size_t body_copy_len = strlen(body);
    size_t body_cap = (size_t)(out_sz - 1 - indent_spaces);
    if (body_copy_len > body_cap)
        body_copy_len = body_cap;
    memcpy(out + indent_spaces, body, body_copy_len);
    out[indent_spaces + (int)body_copy_len] = '\0';
}

int repl_parse_and_normalize(const char *line, int pos,
                             ExprVar *vars, int num_vars,
                             int preserve_expr, GLCmd *out_cmd) {
    int saved = g_edit_line;
    g_edit_line = pos;
    int parsed = parse_command(line, out_cmd, vars, num_vars);
    g_edit_line = saved;

    if (!parsed) return 0;
    if (preserve_expr) {
        int parsed_indent = 0;
        while (out_cmd->source[parsed_indent] == ' ' ||
               out_cmd->source[parsed_indent] == '\t')
            parsed_indent++;

        normalize_with_indent(line, parsed_indent,
                              repl_cmd_type_needs_semicolon(out_cmd->type),
                              out_cmd->source, (int)sizeof(out_cmd->source));
        out_cmd->has_vars = 1;
    }
    return 1;
}

void repl_reformat_commands(void) {
    prof_begin(PROF_REFORMAT);
    int saved_edit_line = g_edit_line;
    int saved_inserting = g_inserting;
    char saved_input[MAX_INPUT_LEN];
    int saved_input_len = g_input_len;
    int saved_cursor_pos = g_cursor_pos;
    memcpy(saved_input, g_input, sizeof(saved_input));
    ReplCommandStore store = repl_command_store_live();

    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;

        GLCmd orig = g_cmds[i];
        GLCmd fmt = orig;

        int bb = in_begin_block_at(i);
        int bdepth = block_depth_at(i);
        int ind = (bb ? 4 : 2) + bdepth * 2;
        char ind_s[32];
        if (ind > (int)sizeof(ind_s) - 1) ind = (int)sizeof(ind_s) - 1;
        memset(ind_s, ' ', (size_t)ind);
        ind_s[ind] = '\0';

        switch (orig.type) {
        case CMD_FOR_BEGIN: {
            char var[16] = "";
            char args[128] = "";
            if (!extract_for_args_text(orig.source, var, sizeof(var), args, sizeof(args)))
                get_for_var_name(&orig, var, sizeof(var));
            if (!var[0]) strncpy(var, "i", sizeof(var) - 1);

            if (orig.has_vars && args[0]) {
                snprintf(fmt.source, sizeof(fmt.source), "%sfor(%s, %s) {", ind_s, var, args);
                fmt.has_vars = 1;
            } else if (orig.args[2] != 1.0f) {
                snprintf(fmt.source, sizeof(fmt.source), "%sfor(%s, %g, %g, %g) {",
                         ind_s, var, orig.args[0], orig.args[1], orig.args[2]);
            } else {
                snprintf(fmt.source, sizeof(fmt.source), "%sfor(%s, %g, %g) {",
                         ind_s, var, orig.args[0], orig.args[1]);
            }
            repl_command_store_replace_one(&store, i, &fmt);
            break;
        }
        case CMD_FOR_END:
        case CMD_FUNC_END:
        case CMD_IF_END: {
            int close_depth = block_depth_at(i) - 1;
            if (close_depth < 0) close_depth = 0;
            int cb = in_begin_block_at(i);
            int close_ind = (cb ? 4 : 2) + close_depth * 2;
            char close_s[32];
            if (close_ind > (int)sizeof(close_s) - 1) close_ind = (int)sizeof(close_s) - 1;
            memset(close_s, ' ', (size_t)close_ind);
            close_s[close_ind] = '\0';
            snprintf(fmt.source, sizeof(fmt.source), "%s}", close_s);
            repl_command_store_replace_one(&store, i, &fmt);
            break;
        }
        case CMD_FUNC_DEF: {
            int fn = (int)orig.args[0];
            int parsed_fn = fn;
            int param_count = 0;
            char param_names[MAX_EXPR_VARS][16];
            if (parse_repl_func_signature(orig.source, &parsed_fn,
                                          param_names, MAX_EXPR_VARS,
                                          &param_count))
                format_func_header(fmt.source, sizeof(fmt.source), ind_s,
                                   parsed_fn, param_names, param_count);
            else
                snprintf(fmt.source, sizeof(fmt.source), "%sfunc%d {", ind_s, fn);
            repl_command_store_replace_one(&store, i, &fmt);
            break;
        }
        case CMD_IF_BEGIN: {
            char cond[MAX_LINE_LEN] = "";
            if (!repl_extract_paren_payload(orig.source, cond, sizeof(cond)))
                snprintf(cond, sizeof(cond), "%g", orig.args[0]);
            snprintf(fmt.source, sizeof(fmt.source), "%sif(%s) {", ind_s, cond);
            repl_command_store_replace_one(&store, i, &fmt);
            break;
        }
        case CMD_VAR_ASSIGN: {
            const char *name = NULL;
            char rhs[MAX_LINE_LEN] = "";
            if (orig.num_args >= 0 && orig.num_args < g_num_predef_vars)
                name = g_predef_vars[orig.num_args].name;
            char fallback[16] = "";
            if (!name) {
                const char *p = orig.source;
                while (*p && isspace((unsigned char)*p)) p++;
                int n = 0;
                while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
                       n < (int)sizeof(fallback) - 1)
                    fallback[n++] = *p++;
                fallback[n] = '\0';
                if (fallback[0]) name = fallback;
            }
            repl_extract_assignment_parts(orig.source, NULL, 0, rhs, sizeof(rhs));
            {
                char comment[MAX_LINE_LEN] = "";
                const char *cp = strstr(orig.source, "//");
                if (cp) snprintf(comment, sizeof(comment), " %s", cp);
                if (name && rhs[0])
                    snprintf(fmt.source, sizeof(fmt.source), "%s%s = %s;%s", ind_s, name, rhs, comment);
                else if (name)
                    snprintf(fmt.source, sizeof(fmt.source), "%s%s = %g;%s", ind_s, name, orig.args[0], comment);
            }
            repl_command_store_replace_one(&store, i, &fmt);
            break;
        }
        case CMD_COMMENT: {
            const char *p = orig.source;
            while (*p && isspace((unsigned char)*p)) p++;
            if (p[0] == '/' && p[1] == '/') {
                char suffix[MAX_LINE_LEN];
                p += 2;
                strncpy(suffix, p, sizeof(suffix) - 1);
                suffix[sizeof(suffix) - 1] = '\0';
                int suffix_len = (int)strlen(suffix);
                while (suffix_len > 0 &&
                       isspace((unsigned char)suffix[suffix_len - 1]))
                    suffix[--suffix_len] = '\0';
                snprintf(fmt.source, sizeof(fmt.source), "%s//%s", ind_s, suffix);
            } else {
                snprintf(fmt.source, sizeof(fmt.source), "%s//", ind_s);
            }
            repl_command_store_replace_one(&store, i, &fmt);
            break;
        }
        case CMD_VAR_DECLARE: {
            int off = snprintf(fmt.source, sizeof(fmt.source), "%sfloat ", ind_s);
            for (int n = 0; n < orig.var_decl_count && off < (int)sizeof(fmt.source) - 4; n++) {
                if (n > 0) off += snprintf(fmt.source + off, sizeof(fmt.source) - off, ", ");
                off += snprintf(fmt.source + off, sizeof(fmt.source) - off, "%s", orig.var_names[n]);
            }
            snprintf(fmt.source + off, sizeof(fmt.source) - off, ";");
            repl_command_store_replace_one(&store, i, &fmt);
            break;
        }
        case CMD_LABEL: {
            char label[64] = "";
            if (repl_extract_label_name(orig.source, label, sizeof(label)))
                snprintf(fmt.source, sizeof(fmt.source), "%s:", label);
            repl_command_store_replace_one(&store, i, &fmt);
            break;
        }
        case CMD_GOTO: {
            char label[64] = "";
            if (repl_extract_goto_label(orig.source, label, sizeof(label)))
                snprintf(fmt.source, sizeof(fmt.source), "%sgoto %s;", ind_s, label);
            repl_command_store_replace_one(&store, i, &fmt);
            break;
        }
        default: {
            ExprVar vis_vars[MAX_EXPR_VARS];
            int num_vis_vars = collect_visible_vars(i, vis_vars, MAX_EXPR_VARS);
            int preserve_expr = (num_vis_vars > 0) || orig.has_vars;
            GLCmd parsed;
            memset(&parsed, 0, sizeof(parsed));
            if (repl_parse_and_normalize(orig.source, i,
                                         num_vis_vars > 0 ? vis_vars : NULL,
                                         num_vis_vars > 0 ? num_vis_vars : 0,
                                         preserve_expr, &parsed) &&
                parsed.type == orig.type) {
                parsed.is_auto = orig.is_auto;
                parsed.src_cmd_idx = orig.src_cmd_idx;
                if (!preserve_expr) parsed.has_vars = orig.has_vars;
                repl_command_store_replace_one(&store, i, &parsed);
            }
            break;
        }
        }
    }

    depth_cache_invalidate();
    mark_normals_dirty();

    g_edit_line = saved_edit_line;
    if (g_edit_line < 0) g_edit_line = 0;
    if (g_edit_line > g_num_cmds) g_edit_line = g_num_cmds;
    g_inserting = saved_inserting;
    if (g_inserting) {
        memcpy(g_input, saved_input, sizeof(g_input));
        g_input_len = saved_input_len;
        g_cursor_pos = saved_cursor_pos;
    } else {
        load_line_to_input(g_edit_line);
    }
    prof_end(PROF_REFORMAT);
}

/* ========================================================================= */
/* Parsing                                                                    */
/* ========================================================================= */

/* Expression evaluator, translators, for-loop parsers: see repl_eval.c */

/* --- REMOVED: eval_primary, eval_term, eval_expr, parse_exprs,
       repl_expr_to_c, c_expr_to_repl — now in repl_eval.c --- */

/* ========================================================================= */
/* For-loop state                                                             */
/* ========================================================================= */

#define MAX_LOOP_DEPTH  4

/* ========================================================================= */
/* Command Definitions (Table-Driven)                                         */
/* ========================================================================= */

static void set_incomplete_missing_paren_status(const char *func) {
    char msg[128];

    if (func && func[0])
        snprintf(msg, sizeof(msg), "Incomplete command: missing ')' in %s(...)", func);
    else
        snprintf(msg, sizeof(msg), "Incomplete command: missing ')'");
    set_status(msg);
}

static void set_incomplete_arg_count_status(const char *func, int expected, int got) {
    char msg[128];

    snprintf(msg, sizeof(msg),
             "Incomplete command: %s expects %d argument%s (got %d)",
             func, expected, expected == 1 ? "" : "s", got);
    set_status(msg);
}

static int command_name_matches_or_prefixes(const char *func, const char *known) {
    size_t flen;

    if (!func || !func[0] || !known || !known[0])
        return 0;
    if (strcmp(func, known) == 0)
        return 1;

    flen = strlen(func);
    return flen >= 4 && flen <= strlen(known) && strncmp(known, func, flen) == 0;
}

static int is_known_incomplete_func_name(const char *func) {
    static const char *const special_funcs[] = {
        "glEnd",
        "glPointParameterfv",
        "glPushMatrix",
        "glPopMatrix",
        "gluBegin",
        "gluEnd",
        "gluColor",
        NULL
    };

    if (!func || !func[0])
        return 0;

    for (const ReplEnumCommandSpec *def = repl_enum_command_specs(); def->name; def++) {
        if (command_name_matches_or_prefixes(func, def->name))
            return 1;
    }
    for (const ReplStdCommandSpec *def = repl_std_command_specs(); def->name; def++) {
        if (command_name_matches_or_prefixes(func, def->name))
            return 1;
    }
    for (int i = 0; special_funcs[i]; i++) {
        if (command_name_matches_or_prefixes(func, special_funcs[i]))
            return 1;
    }

    return strncmp(func, "func", 4) == 0 &&
           func[4] >= '0' && func[4] <= '9' &&
           func[5] == '\0';
}

/*
 * parse_command — Convert a single REPL text line into a GLCmd.
 *
 * This is the main entry point for the parser. It tries each command
 * grammar in order:
 *
 *   1. Comments (// …)
 *   2. Table-driven enum commands (glBegin, glEnable, glShadeModel, …)
 *   3. glEnd
 *   4. Table-driven standard commands (glVertex3f, glColor3f, glTranslatef, …)
 *   5. Ad-hoc commands (glMaterialf, glPointParameterfv, glPush/PopMatrix,
 *      funcN calls, glu* tessellator commands, goto/label)
 *
 * Returns 1 on success (cmd populated), 0 on parse failure (status set).
 */
static int parse_command(const char *line, GLCmd *cmd,
                         ExprVar *vars, int num_vars) {
    char buf[MAX_LINE_LEN];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;

    int len = (int)strlen(p);
    while (len > 0 && (p[len - 1] == ';' || isspace((unsigned char)p[len - 1])))
        p[--len] = '\0';

    cmd->valid = 0;
    cmd->num_args = 0;
    cmd->source[0] = '\0';

    if (len == 0) return 0;

    /* Comment: line starts with // */
    if (p[0] == '/' && p[1] == '/') {
        cmd->type = CMD_COMMENT;
        cmd->valid = 1;
        cmd->is_auto = 0;
        cmd->num_args = 0;
        int fdepth = block_depth_at(g_edit_line);
        int bb = in_begin_block_at(g_edit_line);
        int ind = (bb ? 4 : 2) + fdepth * 2;
        char indent[32];
        if (ind > (int)sizeof(indent) - 1) ind = (int)sizeof(indent) - 1;
        memset(indent, ' ', ind);
        indent[ind] = '\0';
        snprintf(cmd->source, sizeof(cmd->source), "%s%s", indent, p);
        return 1;
    }

    char *open_p = strchr(p, '(');
    char *close_p = open_p ? strrchr(p, ')') : NULL;
    char func[64] = "";
    char args[MAX_LINE_LEN] = "";

    if (open_p) {
        int flen = (int)(open_p - p);
        if (flen > 0 && flen < (int)sizeof(func)) {
            memcpy(func, p, (size_t)flen);
            func[flen] = '\0';
        }

        if (!close_p || close_p < open_p) {
            if (!is_known_incomplete_func_name(func))
                goto unknown_command;
            set_incomplete_missing_paren_status(func);
            return 0;
        }

        int alen = (int)(close_p - open_p - 1);
        if (alen > 0 && alen < (int)sizeof(args)) {
            memcpy(args, open_p + 1, (size_t)alen);
            args[alen] = '\0';
        }
    } else {
        if (!repl_copy_string_fits(func, sizeof(func), p))
            goto unknown_command;
    }

    /* Table-driven parsing for enum commands */
    for (const ReplEnumCommandSpec *def = repl_enum_command_specs(); def->name; def++) {
        if (strcmp(func, def->name) == 0) {
            if (def->num_args == 1) {
                char *arg_str = args;
                while (*arg_str && isspace((unsigned char)*arg_str)) arg_str++;
                int arg_len = (int)strlen(arg_str);
                while (arg_len > 0 && isspace((unsigned char)arg_str[arg_len - 1])) arg_str[--arg_len] = '\0';
                for (int i = 0; def->enums1[i].name; i++) {
                    if (strcmp(arg_str, def->enums1[i].name) == 0) {
                        cmd->type = def->type;
                        cmd->mode = def->enums1[i].value;
                        cmd->valid = 1;
                        if (def->indent_type == 1) {
                            /* glBegin-style indent: 2 + 2*tess + 2*block  (begin depth excluded) */
                            char ind[32];
                            int td = tess_scope_depth_at(g_edit_line);
                            int kd = block_depth_at(g_edit_line);
                            int spaces = 2 + 2 * td + 2 * kd;
                            if (spaces > (int)sizeof(ind) - 1) spaces = (int)sizeof(ind) - 1;
                            memset(ind, ' ', (size_t)spaces);
                            ind[spaces] = '\0';
                            snprintf(cmd->source, sizeof(cmd->source), def->fmt, ind, def->enums1[i].name);
                        } else {
                            char ind[32];
                            cmd_indent(g_edit_line, ind, sizeof(ind));
                            snprintf(cmd->source, sizeof(cmd->source), def->fmt, ind, def->enums1[i].name);
                        }
                        return 1;
                    }
                }
                set_status(def->usage1);
                return 0;
            } else if (def->num_args == 2) {
                char raw_arg1[64] = "", raw_arg2[64] = "";
                char *comma = strchr(args, ',');
                if (!comma) { set_status(def->usage1 ? def->usage1 : "Invalid arguments"); return 0; }
                int len1 = (int)(comma - args);
                if (len1 >= (int)sizeof(raw_arg1)) len1 = (int)sizeof(raw_arg1) - 1;
                strncpy(raw_arg1, args, len1); raw_arg1[len1] = '\0';
                strncpy(raw_arg2, comma + 1, sizeof(raw_arg2) - 1);

                /* Trim whitespace from both arguments */
                char *trimmed1 = raw_arg1; while (*trimmed1 == ' ') trimmed1++;
                int tlen1 = (int)strlen(trimmed1); while (tlen1 > 0 && trimmed1[tlen1-1] == ' ') trimmed1[--tlen1] = '\0';
                char *trimmed2 = raw_arg2; while (*trimmed2 == ' ') trimmed2++;
                int tlen2 = (int)strlen(trimmed2); while (tlen2 > 0 && trimmed2[tlen2-1] == ' ') trimmed2[--tlen2] = '\0';

                GLenum val1 = 0;
                int found1 = 0, found2 = 0;
                float val2_f = 0.0f;

                for (int i = 0; def->enums1[i].name; i++) {
                    if (strcmp(trimmed1, def->enums1[i].name) == 0) { val1 = def->enums1[i].value; found1 = 1; break; }
                }
                for (int i = 0; def->enums2[i].name; i++) {
                    if (strcmp(trimmed2, def->enums2[i].name) == 0) { val2_f = (float)def->enums2[i].value; found2 = 1; break; }
                }
                if (!found1) { set_status(def->usage1); return 0; }

                if (!found2 && def->type == CMD_LIGHT_MODEL_I) {
                    char verr[128];
                    if (!validate_expression_idents(trimmed2, vars, num_vars, verr, sizeof(verr))) {
                        set_status(verr); return 0;
                    }
                    float fv; if (parse_exprs(trimmed2, &fv, 1, vars, num_vars) == 1) { val2_f = fv; found2 = 1; }
                }

                if (!found2) { set_status(def->usage2); return 0; }

                cmd->type = def->type;
                cmd->valid = 1;
                cmd->mode = val1;
                cmd->args[0] = val2_f;
                cmd->num_args = 1;
                char ind[32]; cmd_indent(g_edit_line, ind, sizeof(ind));
                snprintf(cmd->source, sizeof(cmd->source), def->fmt, ind, trimmed1, trimmed2);
                return 1;
            }
        }
    }

    /* glEnd() — aligns with its matching glBegin: 2 + 2*tess + 2*block (begin depth not added) */
    if (strcmp(func, "glEnd") == 0) {
        cmd->type = CMD_END;
        cmd->valid = 1;
        {
            int tess_depth = tess_scope_depth_at(g_edit_line);
            int kd = block_depth_at(g_edit_line);
            int spaces = 2 + 2 * tess_depth + 2 * kd;
            char end_ind[32];
            if (spaces > (int)sizeof(end_ind) - 1) spaces = (int)sizeof(end_ind) - 1;
            memset(end_ind, ' ', (size_t)spaces);
            end_ind[spaces] = '\0';
            snprintf(cmd->source, sizeof(cmd->source), "%sglEnd();", end_ind);
        }
        return 1;
    }

    /* Indent for gl commands: 2 + 2*tess + 2*begin */
    char indent_buf[32];
    cmd_indent(g_edit_line, indent_buf, sizeof(indent_buf));
    const char *indent = indent_buf;

    /* Indent for glu (tessellator) commands: 2 + 2*tess only.
     * glu commands belong to the tessellator scope, not the GL vertex block,
     * so glBegin depth is intentionally excluded. */
    char tess_indent_buf[32];
    cmd_tess_indent(g_edit_line, tess_indent_buf, sizeof(tess_indent_buf));
    const char *tess_indent = tess_indent_buf;

    /* Table-driven parsing for standard commands */
    for (const ReplStdCommandSpec *def = repl_std_command_specs(); def->name; def++) {
        if (strcmp(func, def->name) == 0) {
            {
                char verr[128];
                if (!validate_expression_idents(args, vars, num_vars, verr, sizeof(verr))) {
                    set_status(verr); return 0;
                }
            }
            int exact_count = 0;
            if (parse_expr_list_exact(args, cmd->args, def->num_args,
                                      vars, num_vars, &exact_count) &&
                exact_count == def->num_args) {
                cmd->num_args = exact_count;
                cmd->type = def->type;
                cmd->valid = 1;
                cmd->has_vars = input_has_any_visible_vars(args, vars, num_vars);

                const char *ind = def->is_tess ? tess_indent : indent;
                snprintf(cmd->source, sizeof(cmd->source), "%s", ind);
                size_t current_len = strlen(cmd->source);

                switch (def->num_args) {
                case 1:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0]);
                    break;
                case 2:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0], cmd->args[1]);
                    break;
                case 3:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0], cmd->args[1], cmd->args[2]);
                    break;
                case 4:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
                    break;
                case 5:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3], cmd->args[4]);
                    break;
                case 6:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3], cmd->args[4], cmd->args[5]);
                    break;
                }
                /* glClearColor: clamp each RGB channel and rebuild source */
                if (def->type == CMD_CLEAR_COLOR) {
                    int clamped = 0;
                    for (int ci = 0; ci < 3; ci++) {
                        if (cmd->args[ci] > CP_CLEAR_MAX_V) {
                            cmd->args[ci] = CP_CLEAR_MAX_V;
                            clamped = 1;
                        }
                    }
                    if (clamped) {
                        snprintf(cmd->source, sizeof(cmd->source), "%s", ind);
                        size_t cl = strlen(cmd->source);
                        snprintf(cmd->source + cl, sizeof(cmd->source) - cl,
                                 def->fmt, cmd->args[0], cmd->args[1],
                                 cmd->args[2], cmd->args[3]);
                        if (!cmd->has_vars)
                            set_status("glClearColor: channels clamped to 0.15 max");
                    }
                }
                return 1;
            }
            cmd->num_args = parse_exprs(args, cmd->args, def->num_args, vars, num_vars);
            if (cmd->num_args < def->num_args)
                set_incomplete_arg_count_status(def->name, def->num_args, cmd->num_args);
            else
                set_status(def->usage);
            return 0;
        }
    }

    /* glMaterialf(face, pname, param) */
    if (strcmp(func, "glMaterialf") == 0) {
        char a1[64] = "", a2[64] = "", a3[MAX_LINE_LEN] = "";
        char *comma1 = strchr(args, ',');
        char *comma2 = comma1 ? strchr(comma1 + 1, ',') : NULL;

        if (!comma1 || !comma2) { set_status("Usage: glMaterialf(face, pname, params...)"); return 0; }

        int l1 = (int)(comma1 - args);
        if (l1 >= (int)sizeof(a1)) l1 = (int)sizeof(a1) - 1;
        strncpy(a1, args, l1); a1[l1] = '\0';

        int l2 = (int)(comma2 - (comma1 + 1));
        if (l2 >= (int)sizeof(a2)) l2 = (int)sizeof(a2) - 1;
        strncpy(a2, comma1 + 1, l2); a2[l2] = '\0';

        strncpy(a3, comma2 + 1, sizeof(a3) - 1);

        char *p1 = a1; while (*p1 == ' ') p1++;
        int e1 = (int)strlen(p1); while (e1 > 0 && p1[e1-1] == ' ') p1[--e1] = '\0';
        char *p2 = a2; while (*p2 == ' ') p2++;
        int e2 = (int)strlen(p2); while (e2 > 0 && p2[e2-1] == ' ') p2[--e2] = '\0';

        GLenum face = 0, pname = 0;
        int found1 = 0, found2 = 0;

        const EnumEntry *face_types = repl_face_type_entries();
        const EnumEntry *material_params = repl_material_param_entries();
        for (int i = 0; face_types[i].name; i++) {
            if (strcmp(p1, face_types[i].name) == 0) { face = face_types[i].value; found1 = 1; break; }
        }
        for (int i = 0; material_params[i].name; i++) {
            if (strcmp(p2, material_params[i].name) == 0) { pname = material_params[i].value; found2 = 1; break; }
        }

        if (!found1) { set_status("face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK"); return 0; }
        if (!found2) { set_status("pname: GL_DIFFUSE, GL_AMBIENT, GL_SPECULAR, GL_SHININESS..."); return 0; }

        {
            char verr[128];
            if (!validate_expression_idents(a3, vars, num_vars, verr, sizeof(verr))) {
                set_status(verr); return 0;
            }
        }
        float parsed_args[8];
        int num_parsed = parse_exprs(a3, parsed_args, 8, vars, num_vars);
        if (num_parsed != 1 && num_parsed != 4) {
            set_status("Expected 1 or 4 float values");
            return 0;
        }

        cmd->type = CMD_MATERIALF;
        cmd->valid = 1;
        cmd->mode = face;
        cmd->args[0] = (float)pname;
        for (int k = 0; k < num_parsed; k++) cmd->args[k + 1] = parsed_args[k];
        cmd->num_args = num_parsed + 1;
        cmd->has_vars = input_has_any_visible_vars(a3, vars, num_vars);

        if (num_parsed == 1) {
            snprintf(cmd->source, sizeof(cmd->source), "%sglMaterialf(%s, %s, %g);", indent, p1, p2, parsed_args[0]);
        } else {
            snprintf(cmd->source, sizeof(cmd->source), "%sglMaterialfv(%s, %s, (GLfloat[]){%g, %g, %g, %g});",
                     indent, p1, p2, parsed_args[0], parsed_args[1], parsed_args[2], parsed_args[3]);
        }

        return 1;
    }

    /* glPointParameterfv(pname, const, linear, quadratic) —
     * only GL_POINT_DISTANCE_ATTENUATION (size *= 1 / sqrt(const + linear*d + quadratic*d*d)) */
    if (strcmp(func, "glPointParameterfv") == 0) {
        char a1[64] = "", rest[MAX_LINE_LEN] = "";
        char *comma = strchr(args, ',');
        if (!comma) {
            set_status("Usage: glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)");
            return 0;
        }
        int l1 = (int)(comma - args);
        if (l1 >= (int)sizeof(a1)) l1 = (int)sizeof(a1) - 1;
        strncpy(a1, args, l1); a1[l1] = '\0';
        strncpy(rest, comma + 1, sizeof(rest) - 1);

        char *p1 = a1; while (*p1 == ' ') p1++;
        int e1 = (int)strlen(p1); while (e1 > 0 && p1[e1 - 1] == ' ') p1[--e1] = '\0';

        GLenum pname = 0;
        int found = 0;
        const EnumEntry *point_param_pnames = repl_point_param_pname_entries();
        for (int i = 0; point_param_pnames[i].name; i++) {
            if (strcmp(p1, point_param_pnames[i].name) == 0) {
                pname = point_param_pnames[i].value;
                found = 1;
                break;
            }
        }
        if (!found) {
            set_status("pname: GL_POINT_DISTANCE_ATTENUATION");
            return 0;
        }

        {
            char verr[128];
            if (!validate_expression_idents(rest, vars, num_vars, verr, sizeof(verr))) {
                set_status(verr); return 0;
            }
        }
        float parsed_args[4];
        int num_parsed = parse_exprs(rest, parsed_args, 4, vars, num_vars);
        if (num_parsed != 3) {
            set_status("Expected 3 floats: const, linear, quadratic attenuation coefficients");
            return 0;
        }

        cmd->type = CMD_POINT_PARAMETER_FV;
        cmd->valid = 1;
        cmd->mode = pname;
        cmd->args[0] = parsed_args[0];
        cmd->args[1] = parsed_args[1];
        cmd->args[2] = parsed_args[2];
        cmd->num_args = 3;
        cmd->has_vars = (num_vars > 0);

        char ind[32]; cmd_indent(g_edit_line, ind, sizeof(ind));
        snprintf(cmd->source, sizeof(cmd->source),
                 "%sglPointParameterfv(%s, (GLfloat[]){%g, %g, %g});",
                 ind, p1, parsed_args[0], parsed_args[1], parsed_args[2]);
        return 1;
    }

    /* glPushMatrix() */
    if (strcmp(func, "glPushMatrix") == 0) {
        cmd->type = CMD_PUSH_MATRIX;
        cmd->valid = 1;
        snprintf(cmd->source, sizeof(cmd->source), "%sglPushMatrix();", indent);
        return 1;
    }

    /* glPopMatrix() */
    if (strcmp(func, "glPopMatrix") == 0) {
        cmd->type = CMD_POP_MATRIX;
        cmd->valid = 1;
        snprintf(cmd->source, sizeof(cmd->source), "%sglPopMatrix();", indent);
        return 1;
    }



    /* funcN([expr, ...]) — function call */
    if (strncmp(func, "func", 4) == 0 && func[4] >= '0' && func[4] <= '9' &&
        func[5] == '\0' && open_p && close_p) {
        int fn = func[4] - '0';
        float dummy_vals[MAX_EXPR_VARS];
        int arg_count = 0;
        if (args[0] != '\0') {
            char verr[128];
            if (!validate_expression_idents(args, vars, num_vars, verr, sizeof(verr))) {
                set_status(verr); return 0;
            }
        }
        if (!parse_expr_list_exact(args, dummy_vals, MAX_EXPR_VARS,
                                   vars, num_vars, &arg_count)) {
            set_status("Invalid function call arguments");
            return 0;
        }

        cmd->type = CMD_CALL;
        cmd->valid = 1;
        cmd->args[0] = (float)fn;
        cmd->num_args = arg_count;
        cmd->has_vars = input_has_any_visible_vars(args, vars, num_vars);

        int fdepth = block_depth_at(g_edit_line);
        int bb = in_begin_block_at(g_edit_line);
        int ind_v = (bb ? 4 : 2) + fdepth * 2;
        char ind_str[32];
        if (ind_v > (int)sizeof(ind_str) - 1) ind_v = (int)sizeof(ind_str) - 1;
        memset(ind_str, ' ', ind_v);
        ind_str[ind_v] = '\0';

        char raw_args[MAX_LINE_LEN];
        strncpy(raw_args, args, sizeof(raw_args) - 1);
        raw_args[sizeof(raw_args) - 1] = '\0';
        trim_in_place(raw_args);
        if (arg_count > 0) {
            if (!repl_format_fits(cmd->source, sizeof(cmd->source),
                                  "%sfunc%d(%s);", ind_str, fn, raw_args)) {
                set_status("Command too long");
                return 0;
            }
        } else if (!repl_format_fits(cmd->source, sizeof(cmd->source),
                                     "%sfunc%d();", ind_str, fn)) {
            set_status("Command too long");
            return 0;
        }
        return 1;
    }


    /* gluBegin(GLU_POLYGON) — start a tessellated polygon */
    if (strcmp(func, "gluBegin") == 0) {
        char *a = args; while (*a && isspace((unsigned char)*a)) a++;
        if (strncmp(a, "GLU_POLYGON", 11) == 0) {
            cmd->type = CMD_TESS_BEGIN_POLYGON;
            cmd->valid = 1;
            snprintf(cmd->source, sizeof(cmd->source), "%sgluBegin(GLU_POLYGON);", tess_indent);
            return 1;
        }
        if (strncmp(a, "GLU_CONTOUR", 11) == 0) {
            cmd->type = CMD_TESS_BEGIN_CONTOUR;
            cmd->valid = 1;
            snprintf(cmd->source, sizeof(cmd->source), "%sgluBegin(GLU_CONTOUR);", tess_indent);
            return 1;
        }
        set_status("Usage: gluBegin(GLU_POLYGON) or gluBegin(GLU_CONTOUR)");
        return 0;
    }

    /* gluEnd() — end tessellator contour or polygon.
     * Indent at the *enclosing* level (tess_depth - 1), same logic as glEnd()
     * always being at 2-space rather than the 4-space inside a glBegin block.
     * Formula: 2 + 2*(tess-1) + 2*block  (begin depth excluded). */
    if (strcmp(func, "gluEnd") == 0 || strcmp(p, "gluEnd()") == 0) {
        cmd->type = CMD_TESS_END;
        cmd->valid = 1;
        {
            int td = tess_scope_depth_at(g_edit_line);
            if (td > 0) td--;
            int kd = block_depth_at(g_edit_line);
            int spaces = 2 + 2 * td + 2 * kd;
            char close_ind[32];
            if (spaces > (int)sizeof(close_ind) - 1) spaces = (int)sizeof(close_ind) - 1;
            memset(close_ind, ' ', (size_t)spaces);
            close_ind[spaces] = '\0';
            snprintf(cmd->source, sizeof(cmd->source), "%sgluEnd();", close_ind);
        }
        return 1;
    }


    /* gluColor(r, g, b[, a]) — set per-vertex color for tessellator */
    if (strcmp(func, "gluColor") == 0) {
        {
            char verr[128];
            if (!validate_expression_idents(args, vars, num_vars, verr, sizeof(verr))) {
                set_status(verr); return 0;
            }
        }
        cmd->num_args = parse_exprs(args, cmd->args, 4, vars, num_vars);
        if (cmd->num_args >= 3) {
            if (cmd->num_args < 4) cmd->args[3] = 1.0f;
            cmd->num_args = 4;
            cmd->type = CMD_TESS_COLOR;
            cmd->valid = 1;
            cmd->has_vars = input_has_any_visible_vars(args, vars, num_vars);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sgluColor(%g, %g, %g, %g);",
                     tess_indent, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
            return 1;
        }
        set_status("Usage: gluColor(r, g, b) or gluColor(r, g, b, a)");
        return 0;
    }

    /* goto label — jump to a named label.
     *
     * Current limitations:
     * - top-level only; flatten rejects labels/gotos inside functions
     * - executor updates control flow, assignments, and if-conditions, but
     *   variable-driven GL commands inside goto loops are still using their
     *   flattened args rather than being re-evaluated per jump
     * - replay intentionally does not model dynamic goto traces
     */
    if (strncmp(p, "goto ", 5) == 0) {
        const char *lname = p + 5;
        while (*lname && isspace((unsigned char)*lname)) lname++;
        /* Extract clean label name (strip trailing ; or whitespace) */
        char clean_lname[64]; int ll = 0;
        while (ll < 63 && lname[ll] && lname[ll] != ';' && !isspace((unsigned char)lname[ll])) {
            clean_lname[ll] = lname[ll]; ll++;
        }
        clean_lname[ll] = '\0';
        if (ll > 0) {
            cmd->type = CMD_GOTO;
            cmd->valid = 1;
            int fdepth = block_depth_at(g_edit_line);
            int bb_v = in_begin_block_at(g_edit_line);
            int ind_v = (bb_v ? 4 : 2) + fdepth * 2;
            char ind_str[32];
            if (ind_v > (int)sizeof(ind_str) - 1) ind_v = (int)sizeof(ind_str) - 1;
            memset(ind_str, ' ', ind_v); ind_str[ind_v] = '\0';
            snprintf(cmd->source, sizeof(cmd->source), "%sgoto %s;", ind_str, clean_lname);
            return 1;
        }
    }

    /* :label or label: — define a label */
    if ((p[0] == ':' && p[1] && !isspace((unsigned char)p[1])) ||
        (len > 1 && p[len - 1] == ':' && !isspace((unsigned char)p[0]))) {
        cmd->type = CMD_LABEL;
        cmd->valid = 1;
        /* labels go at column 0 in C */
        if (p[0] == ':') {
            snprintf(cmd->source, sizeof(cmd->source), "%s:", p + 1);
        } else {
            char label[64];
            int n = 0;
            while (n < (int)sizeof(label) - 1 &&
                   p[n] && p[n] != ':' && !isspace((unsigned char)p[n])) {
                label[n] = p[n];
                n++;
            }
            label[n] = '\0';
            if (n <= 0)
                return 0;
            snprintf(cmd->source, sizeof(cmd->source), "%s:", label);
        }
        return 1;
    }

unknown_command:
    set_status("Unknown cmd. Try glVertex3f, glBegin, glEnable, glShadeModel, ...");
    return 0;
}

/* ========================================================================= */
/* Auto-normal: insert / update glNormal3f commands in the command list       */
/* ========================================================================= */

static void face_normal(const float *a, const float *b, const float *c,
                        float *n) {
    float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
    float e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
    n[0] = e1[1]*e2[2] - e1[2]*e2[1];
    n[1] = e1[2]*e2[0] - e1[0]*e2[2];
    n[2] = e1[0]*e2[1] - e1[1]*e2[0];
    float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    else { n[0] = 0; n[1] = 0; n[2] = 0; }
}

/* Build an auto-normal command */
static GLCmd make_auto_normal(float nx, float ny, float nz,
                              int insert_pos) {
    GLCmd c;
    memset(&c, 0, sizeof(c));
    c.type = CMD_NORMAL3F;
    c.args[0] = nx; c.args[1] = ny; c.args[2] = nz;
    c.num_args = 3;
    c.valid = 1;
    c.is_auto = 1;
    char ind[32];
    cmd_indent(insert_pos, ind, sizeof(ind));
    snprintf(c.source, sizeof(c.source),
             "%sglNormal3f(%g, %g, %g);", ind, nx, ny, nz);
    return c;
}

/* Insert a command at position pos, shifting everything after it */
static void insert_cmd_at(int pos, const GLCmd *cmd) {
    ReplCommandStore store = repl_command_store_live();
    repl_command_store_insert_one(&store, pos, cmd,
                                  REPL_COMMAND_STORE_ADJUST_EDIT_LINE);
}


/* Compute per-vertex normals for a block and store into norms[] */
static void apply_front_face_to_normal(GLenum front_face, float *n) {
    if (front_face == GL_CW) {
        n[0] = -n[0];
        n[1] = -n[1];
        n[2] = -n[2];
    }
}

static void compute_block_normals(GLenum mode, GLenum front_face,
                                  int *vi, int nv, float norms[][3]) {
    /* Default: zero (will be overwritten for valid faces) */
    for (int i = 0; i < nv; i++)
        norms[i][0] = norms[i][1] = norms[i][2] = 0;

    float n[3];
    switch (mode) {
    case GL_TRIANGLES:
        for (int i = 0; i + 2 < nv; i += 3) {
            face_normal(g_cmds[vi[i]].args, g_cmds[vi[i+1]].args,
                        g_cmds[vi[i+2]].args, n);
            apply_front_face_to_normal(front_face, n);
            for (int j = 0; j < 3; j++)
                memcpy(norms[i+j], n, sizeof(n));
        }
        break;
    case GL_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < nv; i++) {
            if (i % 2 == 0)
                face_normal(g_cmds[vi[i]].args, g_cmds[vi[i+1]].args,
                            g_cmds[vi[i+2]].args, n);
            else
                face_normal(g_cmds[vi[i]].args, g_cmds[vi[i+2]].args,
                            g_cmds[vi[i+1]].args, n);
            apply_front_face_to_normal(front_face, n);
            memcpy(norms[i+2], n, sizeof(n));
            if (i == 0) {
                memcpy(norms[0], n, sizeof(n));
                memcpy(norms[1], n, sizeof(n));
            }
        }
        break;
    case GL_TRIANGLE_FAN:
        for (int i = 1; i + 1 < nv; i++) {
            face_normal(g_cmds[vi[0]].args, g_cmds[vi[i]].args,
                        g_cmds[vi[i+1]].args, n);
            apply_front_face_to_normal(front_face, n);
            memcpy(norms[i+1], n, sizeof(n));
            if (i == 1) {
                memcpy(norms[0], n, sizeof(n));
                memcpy(norms[1], n, sizeof(n));
            }
        }
        break;
    case GL_QUADS:
        for (int i = 0; i + 3 < nv; i += 4) {
            face_normal(g_cmds[vi[i]].args, g_cmds[vi[i+1]].args,
                        g_cmds[vi[i+2]].args, n);
            apply_front_face_to_normal(front_face, n);
            for (int j = 0; j < 4; j++)
                memcpy(norms[i+j], n, sizeof(n));
        }
        break;
    case GL_QUAD_STRIP:
        for (int i = 0; i + 3 < nv; i += 2) {
            face_normal(g_cmds[vi[i]].args, g_cmds[vi[i+1]].args,
                        g_cmds[vi[i+2]].args, n);
            apply_front_face_to_normal(front_face, n);
            memcpy(norms[i+2], n, sizeof(n));
            memcpy(norms[i+3], n, sizeof(n));
            if (i == 0) {
                memcpy(norms[0], n, sizeof(n));
                memcpy(norms[1], n, sizeof(n));
            }
        }
        break;
    case GL_POLYGON:
        if (nv >= 3) {
            face_normal(g_cmds[vi[0]].args, g_cmds[vi[1]].args,
                        g_cmds[vi[2]].args, n);
            apply_front_face_to_normal(front_face, n);
            for (int i = 0; i < nv; i++)
                memcpy(norms[i], n, sizeof(n));
        }
        break;
    default:
        break;
    }
}

/*
 * Scan the entire command list: for every vertex inside a begin/end block
 * that does not already have a normal command (auto or manual) immediately
 * before it, insert a new auto-normal with the computed face normal.
 * Existing manual normals are preserved; existing auto-generated normals are
 * refreshed so geometry or state changes can update them in place.
 */
void recompute_autonormals(void) {
    if (!g_autonormal) return;

    /* Process each begin/end block (skip for-loop regions) */
    int i = 0;
    GLenum front_face = GL_CCW;
    while (i < g_num_cmds) {
        if (g_cmds[i].valid && g_cmds[i].type == CMD_FRONT_FACE) {
            front_face = g_cmds[i].mode;
            i++;
            continue;
        }
        if (g_cmds[i].type == CMD_FOR_BEGIN ||
            g_cmds[i].type == CMD_FUNC_DEF ||
            g_cmds[i].type == CMD_IF_BEGIN) {
            i = find_block_end(i);
            if (i < g_num_cmds) i++;
            continue;
        }
        if (!g_cmds[i].valid || g_cmds[i].type != CMD_BEGIN) { i++; continue; }

        GLenum mode = g_cmds[i].mode;
        i++;

        /* Collect vertex indices in this block (skip auto-normals for now) */
        int vi[MAX_COMMANDS];
        int nv = 0;
        int block_end = g_num_cmds; /* if no glEnd found */
        for (int j = i; j < g_num_cmds; j++) {
            if (!g_cmds[j].valid) continue;
            if (g_cmds[j].type == CMD_END) { block_end = j; break; }
            if (g_cmds[j].type == CMD_BEGIN) { block_end = j; break; }
            if (g_cmds[j].type == CMD_VERTEX3F)
                vi[nv++] = j;
        }

        /* Compute desired normals */
        float norms[MAX_COMMANDS][3];
        compute_block_normals(mode, front_face, vi, nv, norms);

        /* For each vertex, insert an auto-normal if none precedes it.
           Existing manual normals are preserved; existing auto-normals are
           refreshed so state changes like glFrontFace take effect. */
        int offset = 0; /* tracks insertions shifting indices */
        for (int v = 0; v < nv; v++) {
            int vidx = vi[v] + offset;
            float nx = norms[v][0], ny = norms[v][1], nz = norms[v][2];

            /* Preserve manual normals, but keep auto-generated ones current. */
            if (vidx > 0 && g_cmds[vidx - 1].valid &&
                g_cmds[vidx - 1].type == CMD_NORMAL3F) {
                if (g_cmds[vidx - 1].is_auto) {
                    ReplCommandStore store = repl_command_store_live();
                    GLCmd auto_normal = make_auto_normal(nx, ny, nz, vidx - 1);
                    repl_command_store_replace_one(&store, vidx - 1, &auto_normal);
                }
                continue;
            }

            /* Insert new auto-normal before vertex */
            GLCmd nc = make_auto_normal(nx, ny, nz, vidx);
            insert_cmd_at(vidx, &nc);
            offset++;
            block_end++;
        }

        i = block_end + 1;
    }
}

static int find_feeding_state_cmd(int line_idx, int want_normal) {
    if (line_idx < 0 || line_idx >= g_num_cmds) return -1;
    if (!g_cmds[line_idx].valid) return -1;

    CmdType target = g_cmds[line_idx].type;
    int is_gl_vtx = (target == CMD_VERTEX3F || target == CMD_VERTEX2F);
    int is_tess_vtx = (target == CMD_TESS_VERTEX);
    if (!is_gl_vtx && !is_tess_vtx) return -1;

    for (int i = line_idx - 1; i >= 0; i--) {
        if (!g_cmds[i].valid) continue;
        CmdType t = g_cmds[i].type;
        if (want_normal) {
            if (is_gl_vtx && t == CMD_NORMAL3F) return i;
            if (is_tess_vtx && t == CMD_TESS_NORMAL) return i;
        } else {
            if (is_gl_vtx && (t == CMD_COLOR3F || t == CMD_COLOR4F)) return i;
            if (is_tess_vtx && t == CMD_TESS_COLOR) return i;
        }
    }

    return -1;
}

int repl_find_feeding_normal_cmd(int line_idx) {
    return find_feeding_state_cmd(line_idx, 1);
}

int repl_find_feeding_color_cmd(int line_idx) {
    return find_feeding_state_cmd(line_idx, 0);
}

/* ========================================================================= */
/* 2D rendering helpers                                                       */
/* ========================================================================= */

void draw_string(float x, float y, const char *s, void *font) {
    glRasterPos2f(x, y);
    for (; *s; s++)
        glutBitmapCharacter(font, (unsigned char)*s);
}

void draw_quad(float x, float y, float w, float h) {
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void begin_2d(void) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, g_win_w, 0, g_win_h);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
}

void end_2d(void) {
    glEnable(GL_DEPTH_TEST);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
    else glDisable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

/* ========================================================================= */
/* GLUT callbacks                                                             */
/* ========================================================================= */

static void display_func(void) {
    int saved_flat_count;
    float live_predef_vals[MAX_PREDEF_VARS] = { 0 };

    prof_frame_tick();
    prof_begin(PROF_FRAME_TOTAL);

    if (g_normals_dirty) {
        recompute_autonormals();
        g_normals_dirty = 0;
    }
    if (g_flat_dirty) {
        prof_begin(PROF_FLATTEN);
        flatten_commands();
        g_flat_dirty = 0;
        prof_end(PROF_FLATTEN);
    }

    saved_flat_count = g_num_flat_cmds;
    repl_copy_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    if (g_replay_active)
        g_num_flat_cmds = replay_prepare_frame(saved_flat_count);

    update_render_state_strings();
    update_cam_lines();

    /* Full-window clear — use last glClearColor cmd if present, else default */
    glViewport(0, 0, g_win_w, g_win_h);
    {
        float cr = 0.10f, cg = 0.10f, cb = 0.13f, ca = 1.0f;
        for (int ci = 0; ci < g_num_flat_cmds; ci++) {
            if (g_flat_cmds[ci].valid &&
                g_flat_cmds[ci].type == CMD_CLEAR_COLOR) {
                cr = g_flat_cmds[ci].args[0];
                cg = g_flat_cmds[ci].args[1];
                cb = g_flat_cmds[ci].args[2];
                ca = g_flat_cmds[ci].args[3];
            }
        }
        glClearColor(cr, cg, cb, ca);
    }

    /* 3D scene — with optional accumulation-buffer jitter AA */
    /* Reset subsection accumulators so timings across all AA samples sum up
     * correctly before the first (or only) render_3d_scene() call. */
    for (ProfSection s = PROF_SCENE_3D_SETUP; s <= PROF_SCENE_3D_HUD; s++)
        prof_accum_reset(s);
    prof_begin(PROF_SCENE_3D);
    if (g_use_accum && g_accum_aa_enabled && g_accum_samples > 1) {
        /* Clear the accumulation buffer once per frame */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ACCUM_BUFFER_BIT);
        float weight = 1.0f / (float)g_accum_samples;
        for (int j = 0; j < g_accum_samples; j++) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (g_replay_active)
                replay_restore_baseline_predef_values();
            g_accum_jitter_x = g_jitter_table[j % MAX_ACCUM_SAMPLES][0];
            g_accum_jitter_y = g_jitter_table[j % MAX_ACCUM_SAMPLES][1];
            render_3d_scene();
            glAccum(GL_ACCUM, weight);
        }
        g_accum_jitter_x = 0.0f;
        g_accum_jitter_y = 0.0f;
        glAccum(GL_RETURN, 1.0f);
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (g_replay_active)
            replay_restore_baseline_predef_values();
        render_3d_scene();
    }
    prof_end(PROF_SCENE_3D);
    /* Commit the accumulated subsection totals now that all AA samples are done. */
    for (ProfSection s = PROF_SCENE_3D_SETUP; s <= PROF_SCENE_3D_HUD; s++)
        prof_accum_commit(s);

    /* 2D overlays in full window coords */
    glViewport(0, 0, g_win_w, g_win_h);
    prof_begin(PROF_CODE_PANEL);
    render_code_panel();
    prof_end(PROF_CODE_PANEL);

    prof_begin(PROF_UI_PANELS);
    render_autocomplete();
    render_example_dropdown();
    render_var_panel();
    render_scene_status();
    render_help();
    prof_end(PROF_UI_PANELS);

    render_profile_panel();

    g_num_flat_cmds = saved_flat_count;
    repl_restore_predef_values(live_predef_vals, MAX_PREDEF_VARS);

    prof_end(PROF_FRAME_TOTAL);

    glutSwapBuffers();
}

static void reshape_func(int w, int h) {
    if (h < 1) h = 1;
    g_win_w = w;
    g_win_h = h;
}

/* ========================================================================= */
/* For-loop parsing and expansion                                             */
/* ========================================================================= */

/* parse_for_header, parse_c_for_header: see repl_eval.c */


/* Find matching block end for any block-opening command (FOR/FUNC/IF).
 * Handles mixed nesting correctly. */
int find_block_end(int begin_idx) {
    int depth = 1;
    for (int j = begin_idx + 1; j < g_num_cmds; j++) {
        CmdType t = g_cmds[j].type;
        if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) depth++;
        else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) {
            depth--;
            if (depth == 0) return j;
        }
    }
    return g_num_cmds;
}

/* Total nesting depth across all block types (for indentation) */
int block_depth_at(int pos) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > g_num_cmds) pos = g_num_cmds;
    return g_block_depth_prefix[pos];
}

/* Return the innermost unclosed block type at pos, or CMD_TYPE_COUNT if none */
CmdType nearest_open_block_at(int pos) {
    CmdType stack[64];
    int depth = 0;
    for (int i = 0; i < pos && i < g_num_cmds; i++) {
        CmdType t = g_cmds[i].type;
        if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) {
            if (depth < 64) stack[depth++] = t;
        } else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) {
            if (depth > 0) depth--;
        }
    }
    return depth > 0 ? stack[depth - 1] : CMD_TYPE_COUNT;
}

/* Parse variable name from a FOR_BEGIN source string */
static void get_for_var_name(const GLCmd *cmd, char *var, int var_sz) {
    const char *p = cmd->source;
    while (*p && *p != '(') p++;
    if (*p) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    int i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && i < var_sz - 1)
        var[i++] = *p++;
    var[i] = '\0';
}

int collect_visible_vars(int pos, ExprVar *vars, int max_vars) {
    typedef struct {
        CmdType type;
        ExprVar vars[MAX_EXPR_VARS];
        int count;
    } ScopeFrame;

    ScopeFrame frames[64];
    int depth = 0;

    for (int i = 0; i < pos && i < g_num_cmds; i++) {
        CmdType t = g_cmds[i].type;
        if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) {
            if (depth >= (int)(sizeof(frames) / sizeof(frames[0])))
                break;

            frames[depth].type = t;
            frames[depth].count = 0;

            if (t == CMD_FOR_BEGIN) {
                char vn[16];
                get_for_var_name(&g_cmds[i], vn, sizeof(vn));
                repl_copy_string_fits(frames[depth].vars[0].name,
                                      sizeof(frames[depth].vars[0].name),
                                      vn);
                frames[depth].vars[0].value = g_cmds[i].args[0];
                frames[depth].count = 1;
            } else if (t == CMD_FUNC_DEF) {
                int fn = -1;
                int param_count = 0;
                char param_names[MAX_EXPR_VARS][16];
                if (parse_repl_func_signature(g_cmds[i].source, &fn,
                                              param_names, MAX_EXPR_VARS,
                                              &param_count)) {
                    for (int p = 0; p < param_count; p++) {
                        repl_copy_string_fits(frames[depth].vars[p].name,
                                              sizeof(frames[depth].vars[p].name),
                                              param_names[p]);
                        frames[depth].vars[p].value = 0.0f;
                    }
                    frames[depth].count = param_count;
                }
            }
            depth++;
        } else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) {
            if (depth > 0) depth--;
        }
    }

    int count = 0;
    for (int i = depth - 1; i >= 0 && count < max_vars; i--) {
        for (int v = 0; v < frames[i].count && count < max_vars; v++)
            vars[count++] = frames[i].vars[v];
    }

    return count;
}

static uint32_t next_user_scene_tick(void) {
    return ++g_user_scene_tick;
}

static int user_scene_slot_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_USER_SCENES; i++)
        if (g_user_scenes[i].used) n++;
    return n;
}

/* Default home-scene name — used when slot 0 is captured on first example load. */
#define USER_SCENE_HOME_NAME "Your Scene"

/* Writes a unique scene name into `out` based on `base`.  If `base` is
 * already taken by another slot (not `ignore_slot`), appends " (2)",
 * " (3)", etc. until it finds a free one.  Silently truncates to fit. */
static void derive_unique_scene_name(char *out, size_t out_sz,
                                     const char *base, int ignore_slot) {
    if (!out || out_sz == 0) return;
    if (!base || !*base) base = "Scene";

    char candidate[USER_SCENE_NAME_MAX];
    int n = snprintf(candidate, sizeof(candidate), "%s", base);
    if (n < 0) { candidate[0] = '\0'; }

    int suffix = 2;
    while (1) {
        int collision = 0;
        for (int i = 0; i < MAX_USER_SCENES; i++) {
            if (!g_user_scenes[i].used || i == ignore_slot) continue;
            if (strcmp(g_user_scenes[i].name, candidate) == 0) {
                collision = 1; break;
            }
        }
        if (!collision) break;
        snprintf(candidate, sizeof(candidate), "%s (%d)", base, suffix++);
        if (suffix > 999) break; /* give up — should never happen */
    }

    snprintf(out, out_sz, "%s", candidate);
}

/* Copy current g_cmds/g_edit_line/predef state into slot `idx`.  Caller
 * supplies the desired name; uniqueness is the caller's responsibility. */
static void save_scene_to_slot(int idx, const char *name) {
    if (idx < 0 || idx >= MAX_USER_SCENES) return;
    UserScene *s = &g_user_scenes[idx];
    memcpy(s->cmds, g_cmds, (size_t)g_num_cmds * sizeof(GLCmd));
    s->num_cmds        = g_num_cmds;
    s->edit_line       = g_edit_line;
    s->num_predef_vars = g_num_predef_vars;
    for (int i = 0; i < g_num_predef_vars; i++) {
        s->predef_vals[i] = g_predef_vars[i].value;
        memcpy(s->predef_names[i], g_predef_vars[i].name, 16);
    }
    if (name && *name)
        snprintf(s->name, sizeof(s->name), "%s", name);
    else if (s->name[0] == '\0')
        snprintf(s->name, sizeof(s->name), "%s", USER_SCENE_HOME_NAME);
    s->used       = 1;
    s->last_touch = next_user_scene_tick();
}

/* Load slot `idx` into the live editor state.  The slot is preserved
 * (not cleared) so the user can switch back later. */
static void load_scene_from_slot(int idx) {
    if (idx < 0 || idx >= MAX_USER_SCENES) return;
    UserScene *s = &g_user_scenes[idx];
    if (!s->used) return;
    memcpy(g_cmds, s->cmds, (size_t)s->num_cmds * sizeof(GLCmd));
    g_num_cmds        = s->num_cmds;
    g_num_flat_cmds   = 0;
    g_edit_line       = s->edit_line;
    g_num_predef_vars = s->num_predef_vars;
    for (int i = 0; i < s->num_predef_vars; i++) {
        g_predef_vars[i].value = s->predef_vals[i];
        memcpy(g_predef_vars[i].name, s->predef_names[i], 16);
    }
    g_inserting = 0;
    load_line_to_input(g_edit_line);
    mark_normals_dirty();
    s->last_touch         = next_user_scene_tick();
    g_active_user_scene   = idx;
    g_example_idx         = -1;
    char msg[128];
    snprintf(msg, sizeof(msg), "Loaded scene: %s", s->name);
    set_status(msg);
}

/* Legacy save/restore used by load_example() (saves slot 0 once, if empty)
 * and by the F12/Scene back-compat paths.  slot-0 is the "home" scene. */
static void save_user_scene(void) {
    save_scene_to_slot(0, USER_SCENE_HOME_NAME);
}

static void restore_user_scene(void) {
    if (!g_user_scenes[0].used) return;
    load_scene_from_slot(0);
}

/* First-free-slot scan.  Skips slot 0 (pinned home) and any occupied
 * slot.  Returns -1 if every non-home slot is taken. */
static int find_free_user_scene_slot(void) {
    for (int s = 1; s < MAX_USER_SCENES; s++)
        if (!g_user_scenes[s].used) return s;
    return -1;
}

/* Copy a slot's payload into the live editor arrays.  Unlike
 * load_scene_from_slot this is silent — no status bar, no
 * load_line_to_input, no mark_normals_dirty, no g_active_user_scene
 * update — so it's safe to call from inside save_workspace while
 * iterating slots. */
static void install_scene_into_live(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return;
    const UserScene *s = &g_user_scenes[slot];
    if (!s->used) return;
    memcpy(g_cmds, s->cmds, (size_t)s->num_cmds * sizeof(GLCmd));
    g_num_cmds        = s->num_cmds;
    g_edit_line       = s->edit_line;
    g_num_predef_vars = s->num_predef_vars;
    for (int i = 0; i < s->num_predef_vars; i++) {
        g_predef_vars[i].value = s->predef_vals[i];
        memcpy(g_predef_vars[i].name, s->predef_names[i], 16);
    }
}

/* Dump live editor state into an UserScene-shaped buffer, without
 * registering it in g_user_scenes[].  Used to stash current editing
 * state so save_workspace can iterate slots destructively and restore
 * afterwards. */
static void stash_live_state(UserScene *dst) {
    memset(dst, 0, sizeof(*dst));
    memcpy(dst->cmds, g_cmds, (size_t)g_num_cmds * sizeof(GLCmd));
    dst->num_cmds        = g_num_cmds;
    dst->edit_line       = g_edit_line;
    dst->num_predef_vars = g_num_predef_vars;
    for (int i = 0; i < g_num_predef_vars; i++) {
        dst->predef_vals[i] = g_predef_vars[i].value;
        memcpy(dst->predef_names[i], g_predef_vars[i].name, 16);
    }
}

static void restore_live_from_stash(const UserScene *src) {
    memcpy(g_cmds, src->cmds, (size_t)src->num_cmds * sizeof(GLCmd));
    g_num_cmds        = src->num_cmds;
    g_edit_line       = src->edit_line;
    g_num_predef_vars = src->num_predef_vars;
    for (int i = 0; i < src->num_predef_vars; i++) {
        g_predef_vars[i].value = src->predef_vals[i];
        memcpy(g_predef_vars[i].name, src->predef_names[i], 16);
    }
}

/* Filesystem slug: lowercase alnum + underscores, from the scene name.
 * Does not coordinate with collisions — the caller should. */
static void scene_filename_slug(const char *name, char *out, size_t out_sz) {
    size_t j = 0;
    if (name) {
        for (size_t i = 0; name[i] && j + 1 < out_sz; i++) {
            unsigned char c = (unsigned char)name[i];
            if (isalnum(c))                            out[j++] = (char)tolower(c);
            else if (c == ' ' || c == '-' || c == '_') out[j++] = '_';
        }
    }
    if (j == 0 && out_sz > 0) out[j++] = 's';  /* avoid empty file names */
    if (j >= out_sz) j = out_sz - 1;
    out[j] = '\0';
}

/* Extract the filename stem (no dir, no .c extension) into `out`. */
static void scene_name_from_filename(const char *path,
                                     char *out, size_t out_sz) {
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    int n = 0;
    while (base[n] && base[n] != '.' && n < (int)out_sz - 1) {
        out[n] = base[n];
        n++;
    }
    if (out_sz > 0) out[n] = '\0';
}

int repl_save_workspace(const char *dir) {
    if (!dir || !*dir) {
        set_status("Workspace save: no folder provided");
        return -1;
    }

    /* mkdir is idempotent-friendly: EEXIST is fine. */
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Workspace save: cannot create %s", dir);
        set_status(msg);
        return -1;
    }

    /* Flush the active user scene's live edits back to its slot so the
     * export reflects what the user is seeing right now. */
    if (g_active_user_scene >= 0 && g_active_user_scene < MAX_USER_SCENES) {
        save_scene_to_slot(g_active_user_scene,
                           g_user_scenes[g_active_user_scene].name);
    }

    /* Ensure header emits @workspace-dir for single-file round-tripping. */
    snprintf(g_workspace_dir, WORKSPACE_DIR_MAX, "%s", dir);

    UserScene stash;
    stash_live_state(&stash);

    int written = 0;
    for (int s = 0; s < MAX_USER_SCENES; s++) {
        if (!g_user_scenes[s].used) continue;
        install_scene_into_live(s);

        char slug[USER_SCENE_NAME_MAX];
        scene_filename_slug(g_user_scenes[s].name, slug, sizeof(slug));

        char path[WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
        snprintf(path, sizeof(path), "%s/%s.c", dir, slug);

        g_export_scene_name_hint = g_user_scenes[s].name;
        save_output(path);
        g_export_scene_name_hint = NULL;
        written++;
    }

    restore_live_from_stash(&stash);

    char msg[256];
    snprintf(msg, sizeof(msg), "Saved %d scene%s to %s",
             written, written == 1 ? "" : "s", dir);
    set_status(msg);
    return written;
}

/* Load a single scene file into a fresh user-scene slot.  The caller
 * is responsible for stashing/restoring live state if needed.  Returns
 * the slot index on success, or -1 on failure (file unreadable or no
 * free slot — eviction is not performed here; workspace import relies
 * on always having room for at most MAX_USER_SCENES files). */
static int load_scene_file_into_slot(const char *path) {
    /* Reset live editor arrays so the load starts clean. */
    g_num_cmds        = 0;
    g_edit_line       = 0;
    g_num_predef_vars = 0;

    if (!load_from_file(path)) return -1;

    int slot = find_free_user_scene_slot();
    if (!g_user_scenes[0].used) slot = 0;  /* first-time home fill */
    if (slot < 0) return -1;                /* no room; caller may LRU */

    char scene_name[USER_SCENE_NAME_MAX];
    if (g_pending_scene_name[0]) {
        snprintf(scene_name, sizeof(scene_name), "%s", g_pending_scene_name);
    } else {
        scene_name_from_filename(path, scene_name, sizeof(scene_name));
        if (!scene_name[0])
            snprintf(scene_name, sizeof(scene_name), "Scene %d", slot);
    }

    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique), scene_name, -1);
    save_scene_to_slot(slot, unique);
    return slot;
}

/* Case-insensitive ".c" suffix check. */
static int has_dot_c_ext(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n < 3) return 0;
    return name[n - 2] == '.' && (name[n - 1] == 'c' || name[n - 1] == 'C');
}

int repl_load_workspace(const char *dir) {
    if (!dir || !*dir) return 0;

    DIR *d = opendir(dir);
    if (!d) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Workspace load: cannot open %s", dir);
        set_status(msg);
        return -1;
    }

    /* Stash live state so the loader can scribble into globals freely. */
    UserScene stash;
    stash_live_state(&stash);
    int stash_example   = g_example_idx;
    int stash_active    = g_active_user_scene;

    int loaded = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;
        if (!has_dot_c_ext(name)) continue;

        char path[WORKSPACE_DIR_MAX + 256];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (load_scene_file_into_slot(path) >= 0)
            loaded++;
    }
    closedir(d);

    /* Restore live state. */
    restore_live_from_stash(&stash);
    g_example_idx       = stash_example;
    g_active_user_scene = stash_active;

    snprintf(g_workspace_dir, WORKSPACE_DIR_MAX, "%s", dir);

    char msg[256];
    snprintf(msg, sizeof(msg), "Loaded %d scene%s from %s",
             loaded, loaded == 1 ? "" : "s", dir);
    set_status(msg);
    return loaded;
}

/* Flush `slot` to <g_workspace_dir>/<slug>.c and mark it unused.
 * Caller owns stash/restore of live editor state. Returns 1 on success,
 * 0 on failure (no workspace dir, write error). */
static int evict_scene_to_workspace(int slot) {
    if (slot <= 0 || slot >= MAX_USER_SCENES) return 0;
    if (!g_user_scenes[slot].used)            return 0;
    if (!g_workspace_dir[0])                  return 0;

    if (mkdir(g_workspace_dir, 0755) != 0 && errno != EEXIST) return 0;

    install_scene_into_live(slot);

    char slug[USER_SCENE_NAME_MAX];
    scene_filename_slug(g_user_scenes[slot].name, slug, sizeof(slug));

    char path[WORKSPACE_DIR_MAX + USER_SCENE_NAME_MAX + 8];
    snprintf(path, sizeof(path), "%s/%s.c", g_workspace_dir, slug);

    g_export_scene_name_hint = g_user_scenes[slot].name;
    save_output(path);
    g_export_scene_name_hint = NULL;

    g_user_scenes[slot].used = 0;
    return 1;
}

/* Pick the least-recently-touched non-pinned slot.  Excludes slot 0
 * (home) and the currently active slot.  Returns -1 if there is no
 * evictable slot. */
static int pick_lru_user_scene_slot(void) {
    int best = -1;
    uint32_t best_tick = 0;
    for (int s = 1; s < MAX_USER_SCENES; s++) {
        if (!g_user_scenes[s].used)    continue;
        if (s == g_active_user_scene)  continue;
        if (best < 0 || g_user_scenes[s].last_touch < best_tick) {
            best = s;
            best_tick = g_user_scenes[s].last_touch;
        }
    }
    return best;
}

int repl_promote_example_if_needed(void) {
    if (g_active_user_scene >= 0) return -1;     /* already in a user scene */
    if (g_example_idx < 0)        return -1;     /* no example to promote */

    if (!g_user_scenes[0].used)
        save_user_scene();                       /* ensure home captured */

    int slot = find_free_user_scene_slot();
    if (slot < 0) {
        /* All slots taken.  If a workspace folder is bound, flush the
         * LRU non-pinned slot to disk and reuse its index. */
        if (g_workspace_dir[0]) {
            int victim = pick_lru_user_scene_slot();
            if (victim >= 0) {
                UserScene stash;
                stash_live_state(&stash);
                int ok = evict_scene_to_workspace(victim);
                restore_live_from_stash(&stash);
                if (ok) {
                    slot = victim;
                }
            }
        }
        if (slot < 0) {
            set_status("All user scene slots full — "
                       "save workspace to free a slot");
            return -1;
        }
    }

    const char *example_name = repl_examples_name(g_example_idx);
    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique),
                             example_name ? example_name : "Scene", -1);
    save_scene_to_slot(slot, unique);
    g_active_user_scene = slot;
    g_example_idx       = -1;

    char msg[128];
    snprintf(msg, sizeof(msg), "Promoted to scene: %s", unique);
    set_status(msg);
    return slot;
}

static const char *example_cam_skip_ws(const char *text) {
    while (*text && isspace((unsigned char)*text))
        text++;
    return text;
}

static const char *example_cam_skip_sep(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == ',' ||
           *text == 'f' || *text == 'F')
        text++;
    return text;
}

static int example_cam_read_floats(const char *text, float *out_vals,
                                   int out_count, const char **end_out) {
    for (int i = 0; i < out_count; i++) {
        char *end = NULL;

        text = example_cam_skip_sep(text);
        out_vals[i] = strtof(text, &end);
        if (end == text)
            return 0;
        text = end;
    }

    if (end_out)
        *end_out = text;
    return 1;
}

static int example_cam_finish_call(const char *text) {
    text = example_cam_skip_sep(text);
    while (*text == ')' || *text == ';' || isspace((unsigned char)*text))
        text++;
    return *text == '\0';
}

static int example_cam_parse_translate(const char *text,
                                       float *x, float *y, float *z) {
    const char *end = NULL;
    float vals[3];

    text = example_cam_skip_ws(text);
    if (strncmp(text, "glTranslatef", 12) != 0)
        return 0;

    text = strchr(text, '(');
    if (!text)
        return 0;
    text++;

    if (!example_cam_read_floats(text, vals, 3, &end) ||
        !example_cam_finish_call(end))
        return 0;

    *x = vals[0];
    *y = vals[1];
    *z = vals[2];
    return 1;
}

static int example_cam_parse_rotate(const char *text,
                                    float axis_x, float axis_y, float axis_z,
                                    float *angle_out) {
    const char *end = NULL;
    float vals[4];

    text = example_cam_skip_ws(text);
    if (strncmp(text, "glRotatef", 9) != 0)
        return 0;

    text = strchr(text, '(');
    if (!text)
        return 0;
    text++;

    if (!example_cam_read_floats(text, vals, 4, &end) ||
        !example_cam_finish_call(end))
        return 0;

    if (fabsf(vals[1] - axis_x) > 1e-4f ||
        fabsf(vals[2] - axis_y) > 1e-4f ||
        fabsf(vals[3] - axis_z) > 1e-4f)
        return 0;

    *angle_out = vals[0];
    return 1;
}

static int try_apply_example_camera_header(const char *const *lines) {
    float dist_x, dist_y, dist_z;
    float rx, ry;
    float tx, ty, tz;

    if (!lines || !lines[0] || strcmp(lines[0], "// camera") != 0)
        return 0;
    if (!lines[1] || !lines[2] || !lines[3] || !lines[4])
        return 0;

    if (!example_cam_parse_translate(lines[1], &dist_x, &dist_y, &dist_z) ||
        fabsf(dist_x) > 1e-4f || fabsf(dist_y) > 1e-4f ||
        !example_cam_parse_rotate(lines[2], 1.0f, 0.0f, 0.0f, &rx) ||
        !example_cam_parse_rotate(lines[3], 0.0f, 1.0f, 0.0f, &ry) ||
        !example_cam_parse_translate(lines[4], &tx, &ty, &tz))
        return 0;

    g_cam_dist = -dist_z;
    g_cam_rx = rx;
    g_cam_ry = ry;
    g_cam_tx = -tx;
    g_cam_ty = -ty;
    g_cam_tz = -tz;
    return 1;
}

static void reset_example_presentation_defaults(void) {
    g_wireframe = CFG_DEFAULT_WIREFRAME;
    g_grid_theme = CFG_DEFAULT_GRID_THEME;
    g_grid_major_idx = CFG_DEFAULT_GRID_MAJOR_IDX;
    g_grid_extent_idx = CFG_DEFAULT_GRID_EXTENT_IDX;
    g_axes_theme = CFG_DEFAULT_AXES_THEME;
    g_show_vnums = CFG_DEFAULT_VERTEX_LABELS;
    g_show_indices = CFG_DEFAULT_VERTEX_INDICES;
    g_show_normals = CFG_DEFAULT_NORMAL_VECTORS;
    g_show_outlines = CFG_DEFAULT_VERTEX_OUTLINES;
    g_show_vpoints = CFG_DEFAULT_VERTEX_POINTS;
    g_show_guides = CFG_DEFAULT_VERTEX_GUIDES;
    g_xform_guide_mode = CFG_DEFAULT_XFORM_GUIDE_MODE;
    g_show_lights = CFG_DEFAULT_LIGHT_INDICATORS;
    g_backdrop_mode = CFG_DEFAULT_BACKDROP_MODE;
    g_cam_rotate = CFG_DEFAULT_CAMERA_ROTATE;
}

static int example_cfg_extract_slug(const char *text,
                                    char *slug, int slug_sz) {
    const char *p = text;
    int slug_len = 0;

    if (!text || !slug || slug_sz < 2)
        return 0;

    p = example_cam_skip_ws(p);
    if (p[0] != '/' || p[1] != '/')
        return 0;
    p += 2;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '@')
        return 0;
    p++;

    if (strncmp(p, "cfg", 3) != 0 || !isspace((unsigned char)p[3]))
        return 0;
    p += 4;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '_' && !isalnum((unsigned char)*p))
        return 0;

    while ((*p == '_' || isalnum((unsigned char)*p)) &&
           slug_len < slug_sz - 1)
        slug[slug_len++] = *p++;
    slug[slug_len] = '\0';
    if (slug_len == 0)
        return 0;

    while (*p && isspace((unsigned char)*p))
        p++;
    return *p == '=';
}

static int example_cfg_slug_allowed(const char *slug) {
    static const char *const allowed_slugs[] = {
        "wireframe",
        "grid",
        "grid_major",
        "grid_extent",
        "axes",
        "vertex_labels",
        "normal_vectors",
        "vertex_outlines",
        "vertex_points",
        "vertex_guides",
        "light_indicators",
        "backdrop",
        "camera_rotate",
        NULL
    };

    for (int i = 0; allowed_slugs[i]; i++) {
        if (strcmp(allowed_slugs[i], slug) == 0)
            return 1;
    }
    return 0;
}

static int consume_example_cfg_header(const char *const *lines) {
    int count = 0;

    while (lines && lines[count]) {
        char slug[32];

        if (!example_cfg_extract_slug(lines[count], slug, sizeof(slug)))
            break;
        if (example_cfg_slug_allowed(slug))
            parse_workspace_header_line(lines[count]);
        count++;
    }

    return count;
}

/* Load an example from an array of source lines */
static void load_example_lines(const char *const *lines) {
    const char *const *body = lines;

    /* Clear state */
    g_num_cmds = 0;
    g_num_flat_cmds = 0;
    g_edit_line = 0;
    g_inserting = 0;
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    g_newline_buf[0] = '\0';
    g_newline_len = 0;
    repl_editor_reset_transients();
    init_predef_vars();
    reset_example_presentation_defaults();

    if (body)
        body += consume_example_cfg_header(body);

    if (body && body[0] && strcmp(body[0], "// camera") == 0) {
        try_apply_example_camera_header(body);
        for (int skip = 0; skip < 5 && body[0]; skip++)
            body++;
    }

    for (; body && *body; body++)
        feed_line(*body);

    /* Clean up: exit insert mode if still active */
    g_inserting = 0;
    g_edit_line = g_num_cmds;
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    mark_normals_dirty();
}

static void load_example(int idx) {
    int count = repl_examples_count();
    const char *const *lines;
    const char *name;

    if (idx < 0 || idx >= count) return;
    lines = repl_examples_lines(idx);
    name = repl_examples_name(idx);
    if (!lines || !name) return;

    /* Preserve the user's work (once, into slot 0) before overwriting with
     * an example.  Subsequent example loads leave the home slot untouched. */
    if (!g_user_scenes[0].used)
        save_user_scene();

    load_example_lines(lines);
    g_example_idx       = idx;
    g_active_user_scene = -1;
    char msg[128];
    snprintf(msg, sizeof(msg), "Example %d/%d: %s (F12 for next)",
             idx + 1, count, name);
    set_status(msg);
}

/* ========================================================================= */
/* Initialization                                                             */
/* ========================================================================= */

static void scroll_to_display_function(void) {
    refresh_workspace_header_lines();
    int target = g_workspace_header_line_count;
    for (int i = 0; g_header_pre[i]; i++) {
        if (strcmp(g_header_pre[i], "void display() {") == 0)
            break;
        target++;
    }
    g_scroll = target;
    g_scroll_follow_cursor = 0;
}

static void load_initial_commands(const char *import_file) {
    if (import_file) {
        struct stat st;
        if (stat(import_file, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (repl_load_workspace(import_file) > 0) {
                g_edit_line = g_num_cmds;
                scroll_to_display_function();
                return;
            }
        } else if (load_from_file(import_file)) {
            g_edit_line = g_num_cmds;
            scroll_to_display_function();
            return;
        }
    }

    /* Fall back to default example (cube) */
    load_example(0);
    set_status("Ready - type GL commands, press ; to execute. F1 for help. F12 for examples.");
    scroll_to_display_function();
}

static void ensure_t_var_idx_cached(void) {
    if (g_t_var_idx >= 0 && g_t_var_idx < g_num_predef_vars &&
        strcmp(g_predef_vars[g_t_var_idx].name, "t") == 0)
        return;

    g_t_var_idx = -1;
    for (int i = 0; i < g_num_predef_vars; i++) {
        if (strcmp(g_predef_vars[i].name, "t") == 0) {
            g_t_var_idx = i;
            break;
        }
    }
}

/* GLU tessellator callbacks for explicit gluBegin/gluEnd tessellation */
static void _tess_vtx_begin_cb(GLenum mode) {
    glBegin(mode);
}

static void _tess_vtx_end_cb(void) {
    glEnd();
}

static void _tess_vtx_cb(void *vertex_data) {
    TessVertex *v = (TessVertex *)vertex_data;
    glNormal3dv(v->normal);
    glColor4dv(v->color);
    glVertex3dv(v->pos);
}

static void _tess_comb_cb(GLdouble coords[3],
                          void *vertex_data[4],
                          GLfloat weight[4],
                          void **out_data) {
    if (g_tess_vert_count >= TESS_VERT_BUF_SIZE) { *out_data = NULL; return; }
    TessVertex *v = &g_tess_verts[g_tess_vert_count++];
    v->pos[0] = coords[0]; v->pos[1] = coords[1]; v->pos[2] = coords[2];
    for (int c = 0; c < 3; c++) v->normal[c] = 0.0;
    for (int c = 0; c < 4; c++) v->color[c]  = 0.0;
    for (int j = 0; j < 4; j++) {
        if (!vertex_data[j]) continue;
        TessVertex *src = (TessVertex *)vertex_data[j];
        for (int c = 0; c < 3; c++) v->normal[c] += weight[j] * src->normal[c];
        for (int c = 0; c < 4; c++) v->color[c]  += weight[j] * src->color[c];
    }
    /* Renormalize interpolated normal */
    double len = sqrt(v->normal[0]*v->normal[0] + v->normal[1]*v->normal[1]
                    + v->normal[2]*v->normal[2]);
    if (len > 1e-9) { v->normal[0]/=len; v->normal[1]/=len; v->normal[2]/=len; }
    *out_data = v;
}

static void _tess_err_cb(GLenum err) {
    (void)err; /* silently ignore tessellation errors */
}

static void init_gl(void) {
    GLfloat lm_amb[] = { 0.15f, 0.15f, 0.20f, 1.0f };
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);

    /* Init GLU quadric for gluSphere/gluCylinder/gluDisk */
    g_quadric = gluNewQuadric();
    gluQuadricNormals(g_quadric, GLU_SMOOTH);
    gluQuadricTexture(g_quadric, GL_FALSE);

    /* Init GLU tessellator for concave polygon support */
    g_tess = gluNewTess();
    gluTessCallback(g_tess, GLU_TESS_BEGIN,
                    (void (*)())_tess_vtx_begin_cb);
    gluTessCallback(g_tess, GLU_TESS_END,
                    (void (*)())_tess_vtx_end_cb);
    gluTessCallback(g_tess, GLU_TESS_VERTEX,
                    (void (*)())_tess_vtx_cb);
    gluTessCallback(g_tess, GLU_TESS_COMBINE,
                    (void (*)())_tess_comb_cb);
    gluTessCallback(g_tess, GLU_TESS_ERROR,
                    (void (*)())_tess_err_cb);
    gluTessCallback(g_tess, GLU_TESS_EDGE_FLAG,
                    (void (*)())glEdgeFlag);

    apply_init_bootstrap();
}

/* ========================================================================= */
/* Public API wrappers                                                        */
/* ========================================================================= */

int repl_parse_command(const char *line, GLCmd *cmd) {
    return parse_command(line, cmd, NULL, 0);
}

int repl_parse_command_with_vars(const char *line, GLCmd *cmd,
                                 ExprVar *vars, int num_vars) {
    return parse_command(line, cmd, vars, num_vars);
}

int repl_load_from_file(const char *filename) {
    return load_from_file(filename);
}

void repl_save_default_output(void) {
    save_output(outfile);
}

void repl_save_output(const char *filename) {
    save_output(filename);
}

void repl_flatten_commands(void) {
    flatten_commands();
}

void repl_recompute_autonormals(void) {
    recompute_autonormals();
}

int repl_example_count(void) {
    return repl_examples_count();
}

const char *repl_example_name(int idx) {
    return repl_examples_name(idx);
}

void repl_load_example(int idx) {
    load_example(idx);
}

void repl_load_example_lines_for_test(const char *const *lines) {
    load_example_lines(lines);
}

int repl_user_scene_valid(void) {
    return user_scene_slot_count() > 0;
}

void repl_load_user_scene(void) {
    restore_user_scene();
}

int repl_user_scene_count(void) {
    return user_scene_slot_count();
}

int repl_user_scene_slot_used(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return 0;
    return g_user_scenes[slot].used;
}

const char *repl_user_scene_name(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return NULL;
    if (!g_user_scenes[slot].used) return NULL;
    return g_user_scenes[slot].name;
}

int repl_load_user_scene_idx(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return 0;
    if (!g_user_scenes[slot].used) return 0;
    load_scene_from_slot(slot);
    return 1;
}

int repl_active_user_scene(void) {
    return g_active_user_scene;
}

const char *repl_workspace_dir(void) {
    return g_workspace_dir;
}

void repl_set_workspace_dir(const char *dir) {
    if (!dir) { g_workspace_dir[0] = '\0'; return; }
    snprintf(g_workspace_dir, WORKSPACE_DIR_MAX, "%s", dir);
}

int repl_user_scene_rename(int slot, const char *new_name) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return 0;
    if (!g_user_scenes[slot].used) return 0;
    if (!new_name) return 0;

    /* Trim leading/trailing whitespace and reject empty names at the
     * API boundary so every caller (inline rename, future CLI, tests)
     * gets the same invariant. */
    while (*new_name == ' ' || *new_name == '\t') new_name++;
    size_t n = strlen(new_name);
    while (n > 0 && (new_name[n - 1] == ' ' || new_name[n - 1] == '\t')) n--;
    if (n == 0) return 0;

    char trimmed[USER_SCENE_NAME_MAX];
    if (n >= sizeof(trimmed)) n = sizeof(trimmed) - 1;
    memcpy(trimmed, new_name, n);
    trimmed[n] = '\0';

    derive_unique_scene_name(g_user_scenes[slot].name,
                             sizeof(g_user_scenes[slot].name),
                             trimmed, slot);
    g_user_scenes[slot].last_touch = next_user_scene_tick();
    return 1;
}

void repl_load_initial_commands(const char *import_file) {
    load_initial_commands(import_file);
}

void repl_display_func(void) {
    display_func();
}

void repl_reshape_func(int w, int h) {
    reshape_func(w, h);
}

void repl_init_gl(void) {
    ensure_init_bootstrap_ready();
    init_gl();
}

void repl_advance_time(float dt) {
    if (dt <= 0.0f)
        return;

    g_anim_time += dt;
    ensure_t_var_idx_cached();
    if (g_t_playing && g_t_var_idx >= 0) {
        g_predef_vars[g_t_var_idx].value += dt;
        g_flat_dirty = 1;
    }
}

void repl_reset_time_to_zero(void) {
    ensure_t_var_idx_cached();
    if (g_t_var_idx < 0)
        return;

    g_predef_vars[g_t_var_idx].value = 0.0f;
    g_flat_dirty = 1;
}

void repl_reset_state(void) {
    g_num_cmds = 0;
    g_num_flat_cmds = 0;
    g_edit_line = 0;
    g_inserting = 0;
    memset(g_user_scenes, 0, sizeof(g_user_scenes));
    g_active_user_scene = -1;
    g_user_scene_tick   = 0;
    g_example_idx       = -1;
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    g_newline_buf[0] = '\0';
    g_newline_len = 0;
    repl_editor_reset_transients();
    g_scroll = 0;
    g_scroll_follow_cursor = 0;
    g_multisample_enabled = CFG_DEFAULT_MULTISAMPLE;
    g_line_smooth_enabled = CFG_DEFAULT_LINE_SMOOTH;
    g_init_attenuate_points = CFG_DEFAULT_ATTENUATE_POINTS;
    g_wrap_at_comma = CFG_DEFAULT_WRAP_AT_COMMA;
    g_code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
    g_anim_time = 0.0f;
    g_flat_dirty = 1;
    g_normals_dirty = 1;
    g_clear_color[0] = 0.10f; g_clear_color[1] = 0.10f;
    g_clear_color[2] = 0.13f; g_clear_color[3] = 1.0f;
    init_predef_vars();
    ensure_t_var_idx_cached();
    clear_autocomplete_state();
    search_clear_all();
    update_render_state_strings();
    depth_cache_invalidate();
    clear_selection();
}
