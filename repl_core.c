/*
 * repl_core.c — Core state, normalization, and display infrastructure.
 *
 * Division of labor
 * -----------------
 * This file owns everything that transforms text into renderable GL state:
 *
 *   - Control-flow state that is not pure command metadata
 *   - Global state: command arrays (g_cmds / g_flat_cmds), camera, toggles,
 *     accumulation-buffer settings, etc.
 *   - Normalization — repl_parse_and_normalize(), repl_reformat_commands()
 *   - GLUT display / reshape callbacks
 *   - 2D helpers   — draw_string(), draw_quad(), begin_2d(), end_2d()
 *   - Public API wrappers forwarded from sample.c
 *
 * repl_editor.c owns the interactive editing layer:
 *   - Editor state (g_input, cursor)
 *   - Commit handlers that decide *where* a parsed command goes in g_cmds[]
 *   - GLUT keyboard / special / mouse / motion / timer dispatch
 *   - Panel resizing and routing to variable-drag ownership
 *   - feed_line() — the programmatic commit entry point used by file loading
 *     and test harnesses
 *
 * Other translation units:
 *   repl_eval.c    — expression evaluator, for-loop header parsers
 *   repl_export.c  — save / load  (output.c round-tripping)
 *   repl_undo.c    — undo/redo snapshots and history rings
 *   repl_camera_controls.c — viewport camera drag and momentum controls
 *   repl_actions.c — config shortcuts, menu actions, startup config defaults
 *   repl_code_panel_layout.c — pure code-panel wrapping and row lookup
 *   repl_code_panel_document.c — code-panel document rows and hit targets
 *   repl_search.c  — incremental search overlay
 *   cmd_format.c   — source-text formatting helpers
 *   repl_parser.c  — parse_command(): text → GLCmd
 *   repl_source_scope.c — source block/depth queries and indent cache
 *   repl_flatten.c — flatten_range() / flatten_commands()
 *   repl_executor.c— repl_execute_program() / execute_commands()
 *   repl_autocomplete.c — completions and parameter hints
 *   repl_autonormal.c — auto-generated normals and feeding-state lookup
 *   repl_example_loader.c — built-in example loading and metadata
 *   repl_replay.c  — replay state machine and fade-batch rendering
 *   repl_replay_annotations.c — code-panel replay variable annotations
 *   scene_render.c — 3D scene setup, grid / axes / overlay drawing
 *   ui_menu_bar.c — code-panel menus, dropdowns, and search slot
 *   ui_color_picker.c — floating literal-color editor
 *   ui_autocomplete_panel.c — floating autocomplete popup renderer
 *   ui_help_overlay.c — modal F1 help overlay
 *   ui_variable_panel.c — floating variable panel renderer
 *   repl_inline_rename.c — scene-rename input buffer
 *   repl_var_drag.c — variable slider drag state/writeback
 *   ui_panels.c    — code rows, scene status, hit routing
 *   repl_examples.c— predefined example scene data
 */

#include "sample.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_command_spec.h"
#include "repl_command_store.h"
#include "repl_clipboard.h"
#include "repl_replay.h"
#include "cmd_format.h"
#include "scene_render.h"
#include "ui_panels.h"
#include "ui_autocomplete_panel.h"
#include "ui_help_overlay.h"
#include "ui_variable_panel.h"
#include "ui_profile_panel.h"

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
char   g_scratch_buf[256];  /* shared scratch space for formatting strings, etc. */

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
static void get_for_var_name(const GLCmd *cmd, char *var, int var_sz);

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
    ReplParseContext parse_ctx = { pos, vars, num_vars };
    int parsed = repl_parse_command_ctx(line, out_cmd, &parse_ctx);

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
    ui_autocomplete_panel_render();
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
    repl_load_example(0);
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
    repl_scenes_reset();
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
