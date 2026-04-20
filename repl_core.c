/*
 * repl_core.c — Parser, flattener, executor, and supporting infrastructure.
 *
 * Division of labor
 * -----------------
 * This file owns everything that transforms text into renderable GL state:
 *
 *   - Enum tables & completion metadata (g_begin_modes, g_func_completions, …)
 *   - Global state: command arrays (g_cmds / g_flat_cmds), camera, toggles,
 *     autocomplete, accumulation-buffer settings, etc.
 *   - Parsing      — parse_command(): text → GLCmd
 *   - Normalization — repl_parse_and_normalize(), repl_reformat_commands()
 *   - Autocomplete  — update_autocomplete(), accept_autocomplete()
 *                     (straddles editor and parser: reads g_input from
 *                      repl_editor.c, matches against the completion tables
 *                      defined here, and writes g_ac_* state rendered by
 *                      ui_panels.c — kept here because it's tightly coupled
 *                      to the parser tables)
 *   - Flattening   — flatten_range() / flatten_commands(): expand for-loops,
 *                     function calls, and if-blocks into g_flat_cmds[]
 *   - Auto-normals — recompute_autonormals()
 *   - Execution    — execute_commands(): walk g_flat_cmds[], issue GL calls
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
 *   repl_replay.c  — replay state machine and fade-batch rendering
 *   scene_render.c — 3D scene setup, grid / axes / overlay drawing
 *   ui_panels.c    — code panel, autocomplete popup, config menu, var panel
 *   repl_examples.c— predefined example scene data
 */

#include "sample.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_replay.h"
#include "cmd_format.h"
#include "repl_examples.h"
#include "scene_render.h"
#include "ui_panels.h"
#include "profile_panel.h"

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

const EnumEntry g_begin_modes[] = {
    { "GL_POINTS",         GL_POINTS },
    { "GL_LINES",          GL_LINES },
    { "GL_LINE_STRIP",     GL_LINE_STRIP },
    { "GL_LINE_LOOP",      GL_LINE_LOOP },
    { "GL_TRIANGLES",      GL_TRIANGLES },
    { "GL_TRIANGLE_STRIP", GL_TRIANGLE_STRIP },
    { "GL_TRIANGLE_FAN",   GL_TRIANGLE_FAN },
    { "GL_QUADS",          GL_QUADS },
    { "GL_QUAD_STRIP",     GL_QUAD_STRIP },
    { "GL_POLYGON",        GL_POLYGON },
    { NULL, 0 }
};

const EnumEntry g_enable_caps[] = {
    { "GL_DEPTH_TEST",      GL_DEPTH_TEST },
    { "GL_LIGHTING",        GL_LIGHTING },
    { "GL_COLOR_MATERIAL",  GL_COLOR_MATERIAL },
    { "GL_NORMALIZE",       GL_NORMALIZE },
    { "GL_LINE_SMOOTH",     GL_LINE_SMOOTH },
    { "GL_POINT_SMOOTH",    GL_POINT_SMOOTH },
    { "GL_BLEND",           GL_BLEND },
    { "GL_LIGHT0",          GL_LIGHT0 },
    { "GL_LIGHT1",          GL_LIGHT1 },
    { "GL_LIGHT2",          GL_LIGHT2 },
    { "GL_LIGHT3",          GL_LIGHT3 },
    { NULL, 0 }
};

const EnumEntry g_shade_models[] = {
    { "GL_SMOOTH", GL_SMOOTH },
    { "GL_FLAT",   GL_FLAT },
    { NULL, 0 }
};

const EnumEntry g_face_types[] = {
    { "GL_FRONT",            GL_FRONT },
    { "GL_BACK",             GL_BACK },
    { "GL_FRONT_AND_BACK",   GL_FRONT_AND_BACK },
    { NULL, 0 }
};

const EnumEntry g_front_face[] = {
    { "GL_CW",              GL_CW },
    { "GL_CCW",             GL_CCW },
    { NULL, 0 }
};

const EnumEntry g_material_params[] = {
    { "GL_AMBIENT",             GL_AMBIENT },
    { "GL_DIFFUSE",             GL_DIFFUSE },
    { "GL_SPECULAR",            GL_SPECULAR },
    { "GL_EMISSION",            GL_EMISSION },
    { "GL_SHININESS",           GL_SHININESS },
    { "GL_AMBIENT_AND_DIFFUSE", GL_AMBIENT_AND_DIFFUSE },
    { NULL, 0 }
};

const EnumEntry g_light_model_params[] = {
    { "GL_LIGHT_MODEL_LOCAL_VIEWER", GL_LIGHT_MODEL_LOCAL_VIEWER },
    { "GL_LIGHT_MODEL_TWO_SIDE",     GL_LIGHT_MODEL_TWO_SIDE },
    { NULL, 0 }
};

const EnumEntry g_bool_vals[] = {
    { "GL_TRUE",  GL_TRUE  },
    { "GL_FALSE", GL_FALSE },
    { NULL, 0 }
};

const EnumEntry g_point_param_pnames[] = {
    { "GL_POINT_DISTANCE_ATTENUATION", GL_POINT_DISTANCE_ATTENUATION },
    { NULL, 0 }
};

const EnumEntry g_blend_src_factors[] = {
    { "GL_SRC_ALPHA", GL_SRC_ALPHA },
    { NULL, 0 }
};

const EnumEntry g_blend_dst_factors[] = {
    { "GL_ONE_MINUS_SRC_ALPHA", GL_ONE_MINUS_SRC_ALPHA },
    { "GL_ONE",                 GL_ONE },
    { NULL, 0 }
};

const FuncCompletion g_func_completions[] = {
    { "glVertex3f(",         "glVertex3f(x, y, z)",                                      3, { "x", "y", "z" } },
    { "glVertex2f(",         "glVertex2f(x, y)",                                         2, { "x", "y" } },
    { "glNormal3f(",         "glNormal3f(nx, ny, nz)",                                   3, { "nx", "ny", "nz" } },
    { "glColor3f(",          "glColor3f(r, g, b)",                                       3, { "r", "g", "b" } },
    { "glColor4f(",          "glColor4f(r, g, b, a)",                                    4, { "r", "g", "b", "a" } },
    { "glClearColor(",       "glClearColor(r, g, b, a)",                                 4, { "r", "g", "b", "a" } },
    { "glBegin(",            "glBegin(mode)",                                            1, { "mode" } },
    { "glEnd()",             "glEnd()",                                                  0, { NULL } },
    { "glEnable(",           "glEnable(cap)",                                            1, { "cap" } },
    { "glDisable(",          "glDisable(cap)",                                           1, { "cap" } },
    { "glShadeModel(",       "glShadeModel(mode)",                                       1, { "mode" } },
    { "glPointSize(",        "glPointSize(size)",                                        1, { "size" } },
    { "glPointParameterfv(", "glPointParameterfv(pname, a, b, c)",                       4, { "pname", "a", "b", "c" } },
    { "glBlendFunc(",        "glBlendFunc(sfactor, dfactor)",                            2, { "sfactor", "dfactor" } },
    { "glTranslatef(",       "glTranslatef(x, y, z)",                                    3, { "x", "y", "z" } },
    { "glScalef(",           "glScalef(x, y, z)",                                        3, { "x", "y", "z" } },
    { "glRotatef(",          "glRotatef(angle, x, y, z)",                                4, { "angle", "x", "y", "z" } },
    { "glPushMatrix()",      "glPushMatrix()",                                           0, { NULL } },
    { "glPopMatrix()",       "glPopMatrix()",                                            0, { NULL } },
    { "glColorMaterial(",    "glColorMaterial(face, mode)",                              2, { "face", "mode" } },
    { "glLightModeli(",      "glLightModeli(pname, param)",                              2, { "pname", "param" } },
    { "glFrontFace(",        "glFrontFace(mode)",                                        1, { "mode" } },
    { "glMaterialf(",        "glMaterialf(face, pname, value[, g, b, a])",               6, { "face", "pname", "value", "g", "b", "a" } },
    { "gluSphere(",          "gluSphere(radius, slices, stacks)",                        3, { "radius", "slices", "stacks" } },
    { "gluCylinder(",        "gluCylinder(base_r, top_r, height, slices, stacks)",       5, { "base_r", "top_r", "height", "slices", "stacks" } },
    { "gluDisk(",            "gluDisk(inner_r, outer_r, slices, loops)",                 4, { "inner_r", "outer_r", "slices", "loops" } },
    { "gluPartialDisk(",     "gluPartialDisk(inner_r, outer_r, slices, loops, start, sweep)", 6, { "inner_r", "outer_r", "slices", "loops", "start", "sweep" } },
    { "glutSolidTorus(",     "glutSolidTorus(inner_r, outer_r, nsides, rings)",          4, { "inner_r", "outer_r", "nsides", "rings" } },
    { "gluBegin(GLU_POLYGON)", "gluBegin(GLU_POLYGON)",                                  0, { NULL } },
    { "gluBegin(GLU_CONTOUR)", "gluBegin(GLU_CONTOUR)",                                  0, { NULL } },
    { "gluEnd()",            "gluEnd()",                                                 0, { NULL } },
    { "gluNormal(",          "gluNormal(nx, ny, nz)",                                    3, { "nx", "ny", "nz" } },
    { "gluColor(",           "gluColor(r, g, b, a)",                                     4, { "r", "g", "b", "a" } },
    { "gluVertex(",          "gluVertex(x, y, z)",                                       3, { "x", "y", "z" } },
    { "float ",              "float name",                                               0, { NULL } },
    { "for(",                "for(var, start, end[, step])",                             4, { "var", "start", "end", "step" } },
    { "if(",                 "if(expr)",                                                 1, { "expr" } },
    { "goto ",               "goto label",                                               0, { NULL } },
    { "func0 {",             "func0 {",                                                  0, { NULL } },
    { "func0(radius, yoff) {", "func0(radius, yoff) {",                                  0, { NULL } },
    { "func1 {",             "func1 {",                                                  0, { NULL } },
    { "func2 {",             "func2 {",                                                  0, { NULL } },
    { "func3 {",             "func3 {",                                                  0, { NULL } },
    { "func4 {",             "func4 {",                                                  0, { NULL } },
    { "func5 {",             "func5 {",                                                  0, { NULL } },
    { "func6 {",             "func6 {",                                                  0, { NULL } },
    { "func7 {",             "func7 {",                                                  0, { NULL } },
    { "func8 {",             "func8 {",                                                  0, { NULL } },
    { "func9 {",             "func9 {",                                                  0, { NULL } },
    { "func0()",             "func0()",                                                  0, { NULL } },
    { "func1()",             "func1()",                                                  0, { NULL } },
    { "func2()",             "func2()",                                                  0, { NULL } },
    { "func3()",             "func3()",                                                  0, { NULL } },
    { "func4()",             "func4()",                                                  0, { NULL } },
    { "func5()",             "func5()",                                                  0, { NULL } },
    { "func6()",             "func6()",                                                  0, { NULL } },
    { "func7()",             "func7()",                                                  0, { NULL } },
    { "func8()",             "func8()",                                                  0, { NULL } },
    { "func9()",             "func9()",                                                  0, { NULL } },
    { "x = ",                "x = value",                                                0, { NULL } },
    { "y = ",                "y = value",                                                0, { NULL } },
    { "z = ",                "z = value",                                                0, { NULL } },
    { "i = ",                "i = value",                                                0, { NULL } },
    { "j = ",                "j = value",                                                0, { NULL } },
    { "k = ",                "k = value",                                                0, { NULL } },
    { "n = ",                "n = value",                                                0, { NULL } },
    { "t = ",                "t = value",                                                0, { NULL } },
    { "sin(",                "sin(x)",                                                   1, { "x" } },
    { "cos(",                "cos(x)",                                                   1, { "x" } },
    { "tan(",                "tan(x)",                                                   1, { "x" } },
    { "sqrt(",               "sqrt(x)",                                                  1, { "x" } },
    { "abs(",                "abs(x)",                                                   1, { "x" } },
    { "pow(",                "pow(base, exp)",                                           2, { "base", "exp" } },
    { "min(",                "min(a, b)",                                                2, { "a", "b" } },
    { "max(",                "max(a, b)",                                                2, { "a", "b" } },
    { "floor(",              "floor(x)",                                                 1, { "x" } },
    { "ceil(",               "ceil(x)",                                                  1, { "x" } },
    { "fmod(",               "fmod(x, y)",                                               2, { "x", "y" } },
    { "rand(",               "rand(seed[, iter])",                                       2, { "seed", "iter" } },
    { "PI",                  "PI",                                                       0, { NULL } },
    { "TAU",                 "TAU",                                                      0, { NULL } },
    { NULL, NULL, 0, { NULL } }
};

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

/* User scene — saved when switching to an example, restored via F12 cycle or
 * dropdown.  Stored independently from predefined examples so multiple user
 * scenes can be supported in the future. */
typedef struct {
    GLCmd cmds[MAX_COMMANDS];
    int   num_cmds;
    int   edit_line;
    float predef_vals[MAX_PREDEF_VARS];
    char  predef_names[MAX_PREDEF_VARS][16];
    int   num_predef_vars;
} UserScene;

static UserScene g_user_scene;
static int       g_user_scene_valid = 0;
int    g_user_lighting_enabled = 0; /* tracks if user typed glEnable(GL_LIGHTING) */
int    g_show_outlines = CFG_DEFAULT_VERTEX_OUTLINES; /* draw black wireframe over filled polygons */
int    g_show_vpoints  = CFG_DEFAULT_VERTEX_POINTS; /* draw black dots at each vertex position */
int    g_highlight_current_poly = 1; /* highlight glBegin block under cursor */
int    g_current_block_begin = -1;  /* flat cmd index of cursor's glBegin */
int    g_current_block_end   = -1;  /* flat cmd index of cursor's glEnd */
static int g_current_block_line = -1; /* g_edit_line used to compute block */
int    g_ortho_mode = 0;  /* 0=perspective, 1=2D orthographic */

/* Execution context adjusted by repl_replay.c for fade-batch rendering. */
static float           g_execute_alpha_scale = 1.0f;
/* Skip expensive geometry-emitting commands (vertices, quadrics, tess) for
 * pc < this value.  State-setting commands (transforms, color, enable, var
 * assign, etc.) still run so the GL state at pc == skip_before matches a
 * full walk from 0.  Used by the replay fade pass to skip all primitives
 * that sit before the batch's active region; the caller pulls skip_before
 * back to the enclosing CMD_BEGIN / CMD_TESS_BEGIN_POLYGON when vertex-mode
 * batches land mid-primitive, so the full primitive still renders. */
static int             g_execute_skip_geom_before_pc = 0;

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

/* Autocomplete */
const char *g_ac_matches[MAX_AC_MATCHES];
int    g_ac_count = 0;
int    g_ac_sel = 0;
char   g_ac_ghost[MAX_LINE_LEN] = "";
char   g_ac_hint[MAX_LINE_LEN] = "";
const char *g_ac_insert_matches[MAX_AC_MATCHES];
static const FuncCompletion *g_ac_func_matches[MAX_AC_MATCHES];

typedef enum {
    AC_MODE_NONE = 0,
    AC_MODE_POINT_PARAM,
    AC_MODE_ENUM_ARG1,
    AC_MODE_ENUM_ARG2,
    AC_MODE_FUNC_PREFIX
} AutocompleteMode;

static AutocompleteMode g_ac_mode = AC_MODE_NONE;
static int g_ac_token_len = 0;
static char g_ac_suffix[8] = "";
int    g_cursor_px = 0;     /* screen pos of cursor, set during render */
int    g_cursor_py = 0;

/* Forward declarations (eval_expr, parse_for_header, etc. are in repl_eval.h) */
static int parse_command(const char *line, GLCmd *cmd,
                         ExprVar *vars, int num_vars);
static unsigned int line_func_scope_mask(int line);
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
    for (int i = 0; g_begin_modes[i].name; i++)
        if (g_begin_modes[i].value == mode) return g_begin_modes[i].name;
    return "???";
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


/* Normal command indent: 2 + 2*tess + 2*begin */
static void cmd_indent(int pos, char *buf, int buf_sz) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > g_num_cmds) pos = g_num_cmds;
    int td = g_tess_depth_prefix[pos];
    int bd = g_begin_depth_prefix[pos];
    int spaces = 2 + 2 * td + 2 * bd;
    if (spaces > buf_sz - 1) spaces = buf_sz - 1;
    if (spaces < 0) spaces = 0;
    memset(buf, ' ', (size_t)spaces);
    buf[spaces] = '\0';
}

/* Returns indent character count for a normal command at pos: 2 + 2*tess + 2*begin */
int cmd_indent_chars(int pos) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > g_num_cmds) pos = g_num_cmds;
    return 2 + 2 * g_tess_depth_prefix[pos] + 2 * g_begin_depth_prefix[pos];
}

/* Tessellator leaf command indent: 2 + 2*tess  (begin depth ignored) */
static void cmd_tess_indent(int pos, char *buf, int buf_sz) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > g_num_cmds) pos = g_num_cmds;
    int td = g_tess_depth_prefix[pos];
    int spaces = 2 + 2 * td;
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

/* Does this command type get a trailing ';' when reformatted?
 * Comments and labels have their own syntax, and float declarations
 * already include one. */
static int cmd_type_needs_semicolon(CmdType t) {
    switch (t) {
    case CMD_COMMENT:
    case CMD_LABEL:
    case CMD_VAR_DECLARE:
        return 0;
    default:
        return 1;
    }
}

/* Should this command type be indented deeper when inside a
 * for/func/if block?  Block structural commands (openers, closers)
 * and comments/labels/gotos handle their own indent logic. */
static int cmd_type_needs_block_indent(CmdType t) {
    switch (t) {
    case CMD_COMMENT:
    case CMD_LABEL:
    case CMD_GOTO:
    case CMD_CALL:
    case CMD_FOR_BEGIN:
    case CMD_FOR_END:
    case CMD_FUNC_DEF:
    case CMD_FUNC_END:
    case CMD_IF_BEGIN:
    case CMD_IF_END:
    case CMD_VAR_ASSIGN:
    case CMD_VAR_DECLARE:
        return 0;
    default:
        return 1;
    }
}

const char *cmd_type_name(CmdType t) {
    static const char *const names[] = {
        "CMD_BEGIN", "CMD_END",
        "CMD_VERTEX3F", "CMD_VERTEX2F",
        "CMD_NORMAL3F",
        "CMD_COLOR3F", "CMD_COLOR4F",
        "CMD_ENABLE", "CMD_DISABLE",
        "CMD_SHADE_MODEL",
        "CMD_TRANSLATE3F",
        "CMD_SCALEF",
        "CMD_ROTATEF",
        "CMD_PUSH_MATRIX",
        "CMD_POP_MATRIX",
        "CMD_COLOR_MATERIAL",
        "CMD_LIGHT_MODEL_I",
        "CMD_FRONT_FACE",
        "CMD_FOR_BEGIN", "CMD_FOR_END",
        "CMD_FUNC_DEF", "CMD_FUNC_END", "CMD_CALL",
        "CMD_IF_BEGIN", "CMD_IF_END",
        "CMD_COMMENT",
        "CMD_VAR_ASSIGN",
        "CMD_VAR_DECLARE",
        "CMD_LABEL", "CMD_GOTO",
        "CMD_GLU_SPHERE", "CMD_GLU_CYLINDER", "CMD_GLU_DISK",
        "CMD_GLU_PARTIAL_DISK",
        "CMD_GLUT_TORUS",
        "CMD_TESS_BEGIN_POLYGON",
        "CMD_TESS_BEGIN_CONTOUR",
        "CMD_TESS_END",
        "CMD_TESS_NORMAL",
        "CMD_TESS_COLOR",
        "CMD_TESS_VERTEX",
        "CMD_MATERIALF",
        "CMD_POINT_SIZE",
        "CMD_POINT_PARAMETER_FV",
        "CMD_BLEND_FUNC",
        "CMD_CLEAR_COLOR"
    };

    _Static_assert(sizeof(names) / sizeof(names[0]) == CMD_TYPE_COUNT,
                    "cmd_type_name table must have exactly CMD_TYPE_COUNT entries");
    if (t >= 0 && t < CMD_TYPE_COUNT)
        return names[t];
    return "CMD_UNKNOWN";
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

        int indent = parsed_indent;
        if (cmd_type_needs_block_indent(out_cmd->type))
            indent += block_depth_at(pos) * 2;

        normalize_with_indent(line, indent,
                              cmd_type_needs_semicolon(out_cmd->type),
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
            g_cmds[i] = fmt;
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
            g_cmds[i] = fmt;
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
            g_cmds[i] = fmt;
            break;
        }
        case CMD_IF_BEGIN: {
            char cond[MAX_LINE_LEN] = "";
            if (!repl_extract_paren_payload(orig.source, cond, sizeof(cond)))
                snprintf(cond, sizeof(cond), "%g", orig.args[0]);
            snprintf(fmt.source, sizeof(fmt.source), "%sif(%s) {", ind_s, cond);
            g_cmds[i] = fmt;
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
            g_cmds[i] = fmt;
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
            g_cmds[i] = fmt;
            break;
        }
        case CMD_VAR_DECLARE: {
            int off = snprintf(fmt.source, sizeof(fmt.source), "%sfloat ", ind_s);
            for (int n = 0; n < orig.var_decl_count && off < (int)sizeof(fmt.source) - 4; n++) {
                if (n > 0) off += snprintf(fmt.source + off, sizeof(fmt.source) - off, ", ");
                off += snprintf(fmt.source + off, sizeof(fmt.source) - off, "%s", orig.var_names[n]);
            }
            snprintf(fmt.source + off, sizeof(fmt.source) - off, ";");
            g_cmds[i] = fmt;
            break;
        }
        case CMD_LABEL: {
            char label[64] = "";
            if (repl_extract_label_name(orig.source, label, sizeof(label)))
                snprintf(fmt.source, sizeof(fmt.source), "%s:", label);
            g_cmds[i] = fmt;
            break;
        }
        case CMD_GOTO: {
            char label[64] = "";
            if (repl_extract_goto_label(orig.source, label, sizeof(label)))
                snprintf(fmt.source, sizeof(fmt.source), "%sgoto %s;", ind_s, label);
            g_cmds[i] = fmt;
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
                g_cmds[i] = parsed;
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
/* Autocomplete                                                               */
/*                                                                             */
/* NOTE on placement: the autocomplete system sits at the boundary between    */
/* repl_core.c and repl_editor.c.  It reads g_input (owned by the editor)    */
/* and matches against the enum/function completion tables (defined here).    */
/* It's kept in repl_core.c because the match tables are parser-specific     */
/* metadata that would otherwise need to be exported. The g_ac_* outputs     */
/* are rendered by ui_panels.c.                                              */
/* ========================================================================= */

typedef struct {
    const char *name;
    CmdType     type;
    int         num_args;
    const EnumEntry *enums1;
    const EnumEntry *enums2;
    const char *fmt;
    const char *usage1;
    const char *usage2;
    int         indent_type; /* 0: normal, 1: begin/end style */
} EnumCmdDef;

static const EnumCmdDef g_enum_cmds[] = {
    { "glBegin",         CMD_BEGIN,         1, g_begin_modes,        NULL,              "%sglBegin(%s);",             "Unknown mode. Try GL_TRIANGLES, GL_TRIANGLE_STRIP, ...", NULL, 1 },
    { "glEnable",        CMD_ENABLE,        1, g_enable_caps,        NULL,              "%sglEnable(%s);",            "Try GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL", NULL, 0 },
    { "glDisable",       CMD_DISABLE,       1, g_enable_caps,        NULL,              "%sglDisable(%s);",           "Try GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL", NULL, 0 },
    { "glShadeModel",    CMD_SHADE_MODEL,   1, g_shade_models,       NULL,              "%sglShadeModel(%s);",        "Try GL_SMOOTH or GL_FLAT", NULL, 0 },
    { "glFrontFace",     CMD_FRONT_FACE,    1, g_front_face,         NULL,              "%sglFrontFace(%s);",         "Try GL_CW or GL_CCW", NULL, 0 },
    { "glColorMaterial", CMD_COLOR_MATERIAL,2, g_face_types,         g_material_params, "%sglColorMaterial(%s, %s);", "face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK", "mode: GL_AMBIENT, GL_DIFFUSE, GL_AMBIENT_AND_DIFFUSE...", 0 },
    { "glMaterialf",     CMD_MATERIALF,    -2, g_face_types,         g_material_params, NULL,                         "face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK", "pname: GL_DIFFUSE, GL_AMBIENT, GL_SPECULAR, GL_SHININESS", 0 },
    { "glLightModeli",   CMD_LIGHT_MODEL_I, 2, g_light_model_params, g_bool_vals,       "%sglLightModeli(%s, %s);",   "pname: GL_LIGHT_MODEL_TWO_SIDE, GL_LIGHT_MODEL_LOCAL_VIEWER", "param: GL_TRUE, GL_FALSE, or integer", 0 },
    { "glBlendFunc",     CMD_BLEND_FUNC,    2, g_blend_src_factors,  g_blend_dst_factors, "%sglBlendFunc(%s, %s);",  "sfactor: GL_SRC_ALPHA", "dfactor: GL_ONE_MINUS_SRC_ALPHA, GL_ONE", 0 },
    { NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, 0 }
};

static void hint_append(char *out, int out_sz, const char *text) {
    int len = (int)strlen(out);
    if (len >= out_sz - 1)
        return;
    snprintf(out + len, (size_t)(out_sz - len), "%s", text);
}

static void build_param_hint_text(const char *const *params, int param_count,
                                  const char *after, char *out, int out_sz) {
    int arg_index = 0;
    int arg_has_text = 0;
    int depth = 0;

    out[0] = '\0';
    if (!after || !params || param_count <= 0)
        return;

    for (const char *p = after; *p; p++) {
        unsigned char ch = (unsigned char)*p;

        if (depth == 0 && ch == ')')
            return;
        if (depth == 0 && ch == ',') {
            arg_index++;
            arg_has_text = 0;
            continue;
        }

        if (ch == '(') depth++;
        else if (ch == ')' && depth > 0) depth--;

        if (!isspace(ch))
            arg_has_text = 1;
    }

    if (arg_index < 0 || arg_index > param_count)
        return;

    int next_param = arg_has_text ? arg_index + 1 : arg_index;
    if (next_param < 0 || next_param > param_count)
        return;

    if (next_param == param_count) {
        if (arg_has_text)
            snprintf(out, (size_t)out_sz, ")");
        return;
    }

    if (arg_has_text)
        hint_append(out, out_sz, ", ");

    for (int i = next_param; i < param_count; i++) {
        if (i > next_param)
            hint_append(out, out_sz, ", ");
        hint_append(out, out_sz, params[i]);
    }
    hint_append(out, out_sz, ")");
}

static const FuncCompletion *find_builtin_completion_for_input(const char *input,
                                                               const char **after_out) {
    for (int i = 0; g_func_completions[i].insert_text; i++) {
        int plen = (int)strlen(g_func_completions[i].insert_text);
        if (g_func_completions[i].param_count <= 0)
            continue;
        if (strncmp(input, g_func_completions[i].insert_text, (size_t)plen) == 0) {
            if (after_out)
                *after_out = input + plen;
            return &g_func_completions[i];
        }
    }
    return NULL;
}

static int find_defined_func_call_params(const char *input, const char **after_out,
                                         const char *params_out[MAX_EXPR_VARS],
                                         int *count_out,
                                         char param_storage[MAX_EXPR_VARS][16]) {
    const char *p = input;
    int fn = 0;

    if (strncmp(p, "func", 4) != 0)
        return 0;
    p += 4;
    if (!isdigit((unsigned char)*p))
        return 0;

    while (isdigit((unsigned char)*p)) {
        fn = fn * 10 + (*p - '0');
        p++;
    }
    if (*p != '(')
        return 0;

    if (after_out)
        *after_out = p + 1;

    for (int i = 0; i < g_num_cmds; i++) {
        int parsed_fn = -1;
        int param_count = 0;
        if (!g_cmds[i].valid || g_cmds[i].type != CMD_FUNC_DEF)
            continue;
        if ((int)g_cmds[i].args[0] != fn)
            continue;
        if (!parse_repl_func_signature(g_cmds[i].source, &parsed_fn,
                                       param_storage, MAX_EXPR_VARS,
                                       &param_count))
            continue;
        if (parsed_fn != fn || param_count <= 0)
            continue;
        for (int j = 0; j < param_count; j++)
            params_out[j] = param_storage[j];
        if (count_out)
            *count_out = param_count;
        return 1;
    }

    return 0;
}

static void update_input_param_hint(void) {
    const char *after = NULL;
    const FuncCompletion *builtin = find_builtin_completion_for_input(g_input, &after);
    if (builtin) {
        build_param_hint_text(builtin->params, builtin->param_count,
                              after, g_ac_hint, (int)sizeof(g_ac_hint));
        return;
    }

    {
        const char *params[MAX_EXPR_VARS];
        char param_storage[MAX_EXPR_VARS][16];
        int param_count = 0;

        if (find_defined_func_call_params(g_input, &after, params,
                                          &param_count, param_storage)) {
            build_param_hint_text(params, param_count, after,
                                  g_ac_hint, (int)sizeof(g_ac_hint));
        }
    }
}

void update_selected_autocomplete_preview(void) {
    g_ac_ghost[0] = '\0';
    g_ac_hint[0] = '\0';

    if (g_ac_count <= 0 || !g_ac_insert_matches[g_ac_sel])
        return;

    if (g_ac_mode == AC_MODE_FUNC_PREFIX) {
        const char *after = NULL;
        const char *params[MAX_EXPR_VARS];
        char param_storage[MAX_EXPR_VARS][16];
        int param_count = 0;

        snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s",
                 g_ac_insert_matches[g_ac_sel] + g_input_len);
        if (g_ac_func_matches[g_ac_sel] && g_ac_func_matches[g_ac_sel]->param_count > 0) {
            build_param_hint_text(g_ac_func_matches[g_ac_sel]->params,
                                  g_ac_func_matches[g_ac_sel]->param_count,
                                  "", g_ac_hint, (int)sizeof(g_ac_hint));
        } else if (find_defined_func_call_params(g_input, &after, params,
                                                 &param_count, param_storage)) {
            g_ac_ghost[0] = '\0';
            build_param_hint_text(params, param_count, after,
                                  g_ac_hint, (int)sizeof(g_ac_hint));
        }
        return;
    }

    if (g_ac_mode == AC_MODE_POINT_PARAM ||
        g_ac_mode == AC_MODE_ENUM_ARG1 ||
        g_ac_mode == AC_MODE_ENUM_ARG2) {
        snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s%s",
                 g_ac_insert_matches[g_ac_sel] + g_ac_token_len, g_ac_suffix);
    }
}

void update_autocomplete(void) {
    clear_autocomplete_state();
    g_ac_mode = AC_MODE_NONE;
    g_ac_token_len = 0;
    g_ac_suffix[0] = '\0';

    if (g_input_len == 0) return;

    /* Only offer completions when cursor is at the end of input */
    if (g_cursor_pos != g_input_len) return;

    /* glPointParameterfv enum completion (custom: 1 enum + 3 floats) */
    {
        static const char prefix[] = "glPointParameterfv(";
        int plen = (int)sizeof(prefix) - 1;
        if (strncmp(g_input, prefix, plen) == 0 && g_input_len > plen &&
            strchr(g_input + plen, ',') == NULL) {
            const char *after = g_input + plen;
            int alen = g_input_len - plen;
            for (int j = 0; g_point_param_pnames[j].name && g_ac_count < MAX_AC_MATCHES; j++) {
                if (strncmp(g_point_param_pnames[j].name, after, alen) == 0 &&
                    (int)strlen(g_point_param_pnames[j].name) > alen) {
                    g_ac_matches[g_ac_count] = g_point_param_pnames[j].name;
                    g_ac_insert_matches[g_ac_count] = g_point_param_pnames[j].name;
                    g_ac_func_matches[g_ac_count] = NULL;
                    g_ac_count++;
                }
            }
            if (g_ac_count > 0) {
                g_ac_mode = AC_MODE_POINT_PARAM;
                g_ac_token_len = alen;
                snprintf(g_ac_suffix, sizeof(g_ac_suffix), ", ");
                update_selected_autocomplete_preview();
                return;
            }
        }
    }

    /* Enum-based commands completion */
    for (int i = 0; g_enum_cmds[i].name; i++) {
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%s(", g_enum_cmds[i].name);
        int plen = (int)strlen(prefix);

        if (strncmp(g_input, prefix, plen) == 0 && g_input_len > plen) {
            const char *after = g_input + plen;
            int alen = g_input_len - plen;
            char *comma = strchr(after, ',');

            if (!comma) {
                /* Complete enum1 */
                for (int j = 0; g_enum_cmds[i].enums1 && g_enum_cmds[i].enums1[j].name && g_ac_count < MAX_AC_MATCHES; j++) {
                    if (strncmp(g_enum_cmds[i].enums1[j].name, after, alen) == 0 &&
                        (int)strlen(g_enum_cmds[i].enums1[j].name) > alen) {
                        g_ac_matches[g_ac_count] = g_enum_cmds[i].enums1[j].name;
                        g_ac_insert_matches[g_ac_count] = g_enum_cmds[i].enums1[j].name;
                        g_ac_func_matches[g_ac_count] = NULL;
                        g_ac_count++;
                    }
                }
                if (g_ac_count > 0) {
                    g_ac_mode = AC_MODE_ENUM_ARG1;
                    g_ac_token_len = alen;
                    if (abs(g_enum_cmds[i].num_args) == 1)
                        snprintf(g_ac_suffix, sizeof(g_ac_suffix), ")");
                    else if (abs(g_enum_cmds[i].num_args) == 2)
                        snprintf(g_ac_suffix, sizeof(g_ac_suffix), ", ");
                    update_selected_autocomplete_preview();
                }
                return;
            } else {
                /* Complete enum2 */
                if (abs(g_enum_cmds[i].num_args) == 2 && g_enum_cmds[i].enums2) {
                    const char *arg2 = comma + 1;
                    while (*arg2 == ' ') arg2++;
                    int arg2_len = g_input_len - (int)(arg2 - g_input);

                    for (int j = 0; g_enum_cmds[i].enums2[j].name && g_ac_count < MAX_AC_MATCHES; j++) {
                        if (strncmp(g_enum_cmds[i].enums2[j].name, arg2, arg2_len) == 0 &&
                            (int)strlen(g_enum_cmds[i].enums2[j].name) > arg2_len) {
                            g_ac_matches[g_ac_count] = g_enum_cmds[i].enums2[j].name;
                            g_ac_insert_matches[g_ac_count] = g_enum_cmds[i].enums2[j].name;
                            g_ac_func_matches[g_ac_count] = NULL;
                            g_ac_count++;
                        }
                    }
                    if (g_ac_count > 0) {
                        g_ac_mode = AC_MODE_ENUM_ARG2;
                        g_ac_token_len = arg2_len;
                        snprintf(g_ac_suffix, sizeof(g_ac_suffix), ")");
                        update_selected_autocomplete_preview();
                    }
                    return;
                }
            }
        }
    }

    /* Complete function names */
    for (int i = 0; g_func_completions[i].insert_text && g_ac_count < MAX_AC_MATCHES; i++) {
        if (strncmp(g_func_completions[i].insert_text, g_input, (size_t)g_input_len) == 0 &&
            (int)strlen(g_func_completions[i].insert_text) > g_input_len) {
            g_ac_matches[g_ac_count] = g_func_completions[i].display_text;
            g_ac_insert_matches[g_ac_count] = g_func_completions[i].insert_text;
            g_ac_func_matches[g_ac_count] = &g_func_completions[i];
            g_ac_count++;
        }
    }
    if (g_ac_count > 0) {
        g_ac_mode = AC_MODE_FUNC_PREFIX;
        update_selected_autocomplete_preview();
        return;
    }

    update_input_param_hint();
}

void accept_autocomplete(void) {
    if (g_ac_count == 0 || g_ac_ghost[0] == '\0') return;

    int ghost_len = (int)strlen(g_ac_ghost);
    if (g_input_len + ghost_len < MAX_INPUT_LEN - 1) {
        strcat(g_input, g_ac_ghost);
        g_input_len += ghost_len;
        g_cursor_pos = g_input_len;
    }
    clear_autocomplete_state();
    g_ac_mode = AC_MODE_NONE;
    g_ac_token_len = 0;
    g_ac_suffix[0] = '\0';
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

typedef struct {
    const char *name;
    CmdType     type;
    int         num_args;
    const char *fmt;    /* e.g. "glVertex3f(%g, %g, %g);" */
    const char *usage;
    int         is_tess; /* 1 if uses tess_indent, 0 for regular indent */
} StdCmdDef;

static const StdCmdDef g_std_cmds[] = {
    { "glVertex3f",     CMD_VERTEX3F,         3, "glVertex3f(%g, %g, %g);",         "Usage: glVertex3f(x, y, z)", 0 },
    { "glNormal3f",     CMD_NORMAL3F,         3, "glNormal3f(%g, %g, %g);",         "Usage: glNormal3f(nx, ny, nz)", 0 },
    { "glColor3f",      CMD_COLOR3F,          3, "glColor3f(%g, %g, %g);",          "Usage: glColor3f(r, g, b)", 0 },
    { "glColor4f",      CMD_COLOR4F,          4, "glColor4f(%g, %g, %g, %g);",      "Usage: glColor4f(r, g, b, a)", 0 },
    { "glClearColor",   CMD_CLEAR_COLOR,      4, "glClearColor(%g, %g, %g, %g);",   "Usage: glClearColor(r, g, b, a)", 0 },
    { "glTranslatef",   CMD_TRANSLATE3F,      3, "glTranslatef(%g, %g, %g);",       "Usage: glTranslatef(x, y, z)", 0 },
    { "glScalef",       CMD_SCALEF,           3, "glScalef(%g, %g, %g);",           "Usage: glScalef(x, y, z)", 0 },
    { "glRotatef",      CMD_ROTATEF,          4, "glRotatef(%g, %g, %g, %g);",      "Usage: glRotatef(angle, x, y, z)", 0 },
    { "glVertex2f",     CMD_VERTEX2F,         2, "glVertex2f(%g, %g);",             "Usage: glVertex2f(x, y)", 0 },
    { "gluSphere",      CMD_GLU_SPHERE,       3, "gluSphere(%g, %g, %g);", "Usage: gluSphere(radius, slices, stacks)", 0 },
    { "gluCylinder",    CMD_GLU_CYLINDER,     5, "gluCylinder(%g, %g, %g, %g, %g);", "Usage: gluCylinder(baseR, topR, height, slices, stacks)", 0 },
    { "gluDisk",        CMD_GLU_DISK,         4, "gluDisk(%g, %g, %g, %g);", "Usage: gluDisk(innerR, outerR, slices, loops)", 0 },
    { "gluPartialDisk", CMD_GLU_PARTIAL_DISK, 6, "gluPartialDisk(%g, %g, %g, %g, %g, %g);", "Usage: gluPartialDisk(innerR, outerR, slices, loops, startAngle, sweepAngle)", 0 },
    { "glutSolidTorus", CMD_GLUT_TORUS,       4, "glutSolidTorus(%g, %g, %g, %g);", "Usage: glutSolidTorus(innerR, outerR, nsides, rings)", 0 },
    { "glPointSize",    CMD_POINT_SIZE,       1, "glPointSize(%g);",                "Usage: glPointSize(size)", 0 },
    { "gluNormal",      CMD_TESS_NORMAL,      3, "gluNormal(%g, %g, %g);",          "Usage: gluNormal(x, y, z)", 1 },
    { "gluVertex",      CMD_TESS_VERTEX,      3, "gluVertex(%g, %g, %g);",          "Usage: gluVertex(x, y, z)", 1 },
    { NULL, 0, 0, NULL, NULL, 0 }
};

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

    for (const EnumCmdDef *def = g_enum_cmds; def->name; def++) {
        if (command_name_matches_or_prefixes(func, def->name))
            return 1;
    }
    for (const StdCmdDef *def = g_std_cmds; def->name; def++) {
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
    for (const EnumCmdDef *def = g_enum_cmds; def->name; def++) {
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
                            /* glBegin-style indent: tess depth only, no begin depth */
                            char ind[32];
                            int td = tess_scope_depth_at(g_edit_line);
                            int spaces = 2 + 2 * td;
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

    /* glEnd() — aligns with its matching glBegin (begin depth not added) */
    if (strcmp(func, "glEnd") == 0) {
        cmd->type = CMD_END;
        cmd->valid = 1;
        {
            int tess_depth = tess_scope_depth_at(g_edit_line);
            int spaces = 2 + 2 * tess_depth;
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
    for (const StdCmdDef *def = g_std_cmds; def->name; def++) {
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

        for (int i = 0; g_face_types[i].name; i++) {
            if (strcmp(p1, g_face_types[i].name) == 0) { face = g_face_types[i].value; found1 = 1; break; }
        }
        for (int i = 0; g_material_params[i].name; i++) {
            if (strcmp(p2, g_material_params[i].name) == 0) { pname = g_material_params[i].value; found2 = 1; break; }
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
        for (int i = 0; g_point_param_pnames[i].name; i++) {
            if (strcmp(p1, g_point_param_pnames[i].name) == 0) {
                pname = g_point_param_pnames[i].value;
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
     * always being at 2-space rather than the 4-space inside a glBegin block. */
    if (strcmp(func, "gluEnd") == 0 || strcmp(p, "gluEnd()") == 0) {
        cmd->type = CMD_TESS_END;
        cmd->valid = 1;
        {
            int td = tess_scope_depth_at(g_edit_line);
            if (td > 0) td--;
            int spaces = 2 + 2 * td;
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
    if (g_num_cmds >= MAX_COMMANDS) return;
    memmove(&g_cmds[pos + 1], &g_cmds[pos],
            (g_num_cmds - pos) * sizeof(GLCmd));
    g_cmds[pos] = *cmd;
    g_num_cmds++;
    if (g_edit_line >= pos) g_edit_line++;
    depth_cache_invalidate();
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
                if (g_cmds[vidx - 1].is_auto)
                    g_cmds[vidx - 1] = make_auto_normal(nx, ny, nz, vidx - 1);
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

/* ========================================================================= */
/* Flatten for-loops into concrete commands                                    */
/* ========================================================================= */

#define MAX_FLATTEN_CALL_DEPTH 64
#define MAX_FLATTEN_VISIT_BUDGET 200000

static int g_flatten_call_depth = 0;
static int g_flatten_abort = 0;
static int g_flatten_visit_budget = 0;

static void flatten_fail(const char *msg) {
    if (!g_flatten_abort)
        set_status(msg);
    g_flatten_abort = 1;
}

/* Tag a flat command with its origin so cursor-highlighting, replay, and
 * debug dumps can trace each expanded command back to:
 *   src_cmd_idx          — the g_cmds[] line this command came from
 *   call_src_cmd_idx     — the funcN() call site that triggered expansion
 *                          (-1 if top-level)
 *   root_call_src_cmd_idx— the outermost call site in nested func calls
 *                          (-1 if top-level)
 *   func_scope_mask      — bitmask of which func bodies this cmd is inside */
static void flat_cmd_set_provenance(GLCmd *cmd, int src_cmd_idx,
                                    int call_src_cmd_idx,
                                    int root_call_src_cmd_idx,
                                    unsigned int func_scope_mask) {
    cmd->src_cmd_idx = src_cmd_idx;
    cmd->call_src_cmd_idx = call_src_cmd_idx;
    cmd->root_call_src_cmd_idx = root_call_src_cmd_idx;
    cmd->func_scope_mask = func_scope_mask;
}

/* Determine which flat-command range corresponds to the innermost
 * glBegin/glEnd block containing g_edit_line.  The result is stored in
 * g_current_block_begin / g_current_block_end and used by
 * repl_flat_cmd_matches_cursor() to highlight the active geometry
 * batch in the 3D view. */
static void refresh_current_block_highlight(void) {
    g_current_block_begin = -1;
    g_current_block_end   = -1;
    g_current_block_line  = g_edit_line;

    /* Scan g_cmds alongside g_flat_cmds to find the innermost
     * BEGIN/END block (in flat-cmd indices) that contains g_edit_line
     * in source-cmd space.  Skips for/func/if structural commands that
     * don't appear in the flat stream. */
    {
        int begin_src = -1, begin_flat = -1;
        int fcur = 0;
        for (int ci = 0; ci < g_num_cmds && fcur < g_num_flat_cmds; ci++) {
            if (!g_cmds[ci].valid) continue;
            if (g_cmds[ci].type == CMD_FUNC_DEF || g_cmds[ci].type == CMD_FUNC_END ||
                g_cmds[ci].type == CMD_FOR_BEGIN || g_cmds[ci].type == CMD_FOR_END ||
                g_cmds[ci].type == CMD_IF_BEGIN  || g_cmds[ci].type == CMD_IF_END  ||
                g_cmds[ci].type == CMD_CALL)
                continue;
            while (fcur < g_num_flat_cmds && !g_flat_cmds[fcur].valid) fcur++;
            if (fcur >= g_num_flat_cmds) break;
            if (g_cmds[ci].type == CMD_BEGIN) {
                if (ci <= g_edit_line) { begin_src = ci; begin_flat = fcur; }
            } else if (g_cmds[ci].type == CMD_END) {
                if (begin_src >= 0 && ci > g_edit_line) {
                    g_current_block_begin = begin_flat;
                    g_current_block_end   = fcur;
                    break;
                } else if (begin_src >= 0 && ci <= g_edit_line) {
                    begin_src = -1; begin_flat = -1;
                }
            }
            fcur++;
        }
    }
}

/* Recursively expand the source commands in g_cmds[start..end_idx) into
 * the flat command array g_flat_cmds[].  For-loops are unrolled, function
 * calls are inlined, and if-blocks with loop-variable conditions are
 * evaluated.  `vars`/`nv` carry loop-variable and function-parameter
 * bindings from enclosing scopes. */
static void flatten_range(int start, int end_idx, ExprVar *vars, int nv,
                          int call_src_cmd_idx, int root_call_src_cmd_idx,
                          unsigned int func_scope_mask) {
    int i = start;
    while (i < end_idx && i < g_num_cmds) {
        if (g_flatten_abort) return;
        if (--g_flatten_visit_budget < 0) {
            flatten_fail("Recursive expansion exceeded visit budget");
            return;
        }
        if (!g_cmds[i].valid) { i++; continue; }

        if (g_cmds[i].type == CMD_FOR_BEGIN) {
            int loop_end = find_block_end(i);
            GLCmd *loop_cmd = &g_cmds[i];
            char var_name[16];
            get_for_var_name(loop_cmd, var_name, sizeof(var_name));
            float start_val = loop_cmd->args[0];
            float end_val   = loop_cmd->args[1];
            float step_val  = loop_cmd->args[2];

            /* Re-evaluate for-loop bounds from source if they contain variables */
            if (loop_cmd->has_vars) {
                const char *unused_body;
                float re_start, re_end, re_step;
                char rv[16];
                if (parse_for_header_with_vars(loop_cmd->source, rv, sizeof(rv),
                                               &re_start, &re_end, &re_step,
                                               vars, nv, &unused_body)) {
                    start_val = re_start;
                    end_val   = re_end;
                    step_val  = re_step;
                }
            }

            if (fabsf(step_val) > 1e-9f &&
                !((step_val > 0 && start_val >= end_val) ||
                  (step_val < 0 && start_val <= end_val))) {
                int max_iters = 100000;
                for (float val = start_val;
                     (step_val > 0) ? (val < end_val - 1e-6f) : (val > end_val + 1e-6f);
                     val += step_val) {
                    if (--max_iters < 0) break;
                    if (g_flatten_abort) return;
                    ExprVar lvars[MAX_EXPR_VARS];
                    int lnv = 0;
                    if (lnv < MAX_EXPR_VARS) {
                        repl_copy_string_fits(lvars[lnv].name,
                                              sizeof(lvars[lnv].name),
                                              var_name);
                        lvars[lnv].value = val;
                        lnv++;
                    }
                    if (vars)
                        for (int v = 0; v < nv && lnv < MAX_EXPR_VARS; v++)
                            lvars[lnv++] = vars[v];
                    flatten_range(i + 1, loop_end, lvars, lnv,
                                  call_src_cmd_idx, root_call_src_cmd_idx,
                                  func_scope_mask);
                }
            }
            i = (loop_end < g_num_cmds) ? loop_end + 1 : g_num_cmds;
            continue;
        }

        if (g_cmds[i].type == CMD_FOR_END) { i++; continue; }

        /* Function definitions: skip body (expanded at call sites) */
        if (g_cmds[i].type == CMD_FUNC_DEF) {
            int func_end = find_block_end(i);
            i = (func_end < g_num_cmds) ? func_end + 1 : g_num_cmds;
            continue;
        }
        if (g_cmds[i].type == CMD_FUNC_END) { i++; continue; }

        /* Function calls: find definition and expand body inline */
        if (g_cmds[i].type == CMD_CALL) {
            int func_num = (int)g_cmds[i].args[0];
            if (g_flatten_call_depth >= MAX_FLATTEN_CALL_DEPTH) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "Recursive expansion exceeded depth limit (%d) at func%d",
                         MAX_FLATTEN_CALL_DEPTH, func_num);
                flatten_fail(msg);
                i++;
                continue;
            }

            for (int k = 0; k < g_num_cmds; k++) {
                if (g_cmds[k].type == CMD_FUNC_DEF && (int)g_cmds[k].args[0] == func_num) {
                    int body_end = find_block_end(k);
                    int def_fn = func_num;
                    int param_count = 0;
                    char param_names[MAX_EXPR_VARS][16];
                    char arg_text[MAX_LINE_LEN];
                    float arg_vals[MAX_EXPR_VARS];
                    int arg_count = 0;

                    if (!parse_repl_func_signature(g_cmds[k].source, &def_fn,
                                                   param_names, MAX_EXPR_VARS,
                                                   &param_count))
                        break;
                    if (!extract_func_call_args_text(g_cmds[i].source, NULL,
                                                     arg_text, sizeof(arg_text)))
                        break;
                    if (!parse_expr_list_exact(arg_text, arg_vals, MAX_EXPR_VARS,
                                               vars, nv, &arg_count))
                        break;
                    if (arg_count != param_count) {
                        char msg[128];
                        snprintf(msg, sizeof(msg),
                                 "func%d expects %d args, got %d",
                                 func_num, param_count, arg_count);
                        set_status(msg);
                        break;
                    }

                    ExprVar lvars[MAX_EXPR_VARS];
                    int lnv = 0;
                    for (int p = 0; p < param_count && lnv < MAX_EXPR_VARS; p++) {
                        repl_copy_string_fits(lvars[lnv].name,
                                              sizeof(lvars[lnv].name),
                                              param_names[p]);
                        lvars[lnv].value = arg_vals[p];
                        lnv++;
                    }
                    for (int v = 0; vars && v < nv && lnv < MAX_EXPR_VARS; v++)
                        lvars[lnv++] = vars[v];

                    unsigned int nested_func_mask = func_scope_mask;
                    if (func_num >= 0 && func_num < 32)
                        nested_func_mask |= (1u << func_num);
                    int nested_root_call = (root_call_src_cmd_idx >= 0)
                                         ? root_call_src_cmd_idx : i;

                    g_flatten_call_depth++;
                    flatten_range(k + 1, body_end, lvars, lnv,
                                  i, nested_root_call, nested_func_mask);
                    if (g_flatten_call_depth > 0) g_flatten_call_depth--;
                    break;
                }
            }
            i++;
            continue;
        }

        if (g_cmds[i].type == CMD_IF_BEGIN) {
            int if_end = find_block_end(i);
            char cond_text[MAX_LINE_LEN];
            int needs_local_eval = 0;

            if (vars && nv > 0 &&
                repl_extract_paren_payload(g_cmds[i].source, cond_text, sizeof(cond_text)) &&
                (input_has_expr_vars(cond_text, vars, nv) ||
                 input_has_predef_vars(cond_text))) {
                needs_local_eval = 1;
            }

            if (needs_local_eval) {
                char repl_cond[MAX_LINE_LEN];
                c_expr_to_repl(cond_text, repl_cond, sizeof(repl_cond));
                ExprCtx ctx = { repl_cond, vars, nv };
                float cond = eval_expr(&ctx);
                if (cond != 0.0f)
                    flatten_range(i + 1, if_end, vars, nv,
                                  call_src_cmd_idx, root_call_src_cmd_idx,
                                  func_scope_mask);
                i = (if_end < g_num_cmds) ? if_end + 1 : g_num_cmds;
                continue;
            }

            if (g_num_flat_cmds < MAX_COMMANDS) {
                g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
                flat_cmd_set_provenance(&g_flat_cmds[g_num_flat_cmds],
                                        i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                g_num_flat_cmds++;
            } else {
                flatten_fail("Flattened command limit reached");
                return;
            }
            i++;
            continue;
        }

        if (g_cmds[i].type == CMD_IF_END) {
            if (g_num_flat_cmds < MAX_COMMANDS) {
                g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
                flat_cmd_set_provenance(&g_flat_cmds[g_num_flat_cmds],
                                        i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                g_num_flat_cmds++;
            } else {
                flatten_fail("Flattened command limit reached");
                return;
            }
            i++;
            continue;
        }

        if ((g_cmds[i].type == CMD_LABEL || g_cmds[i].type == CMD_GOTO) &&
            func_scope_mask != 0) {
            flatten_fail("goto and labels are not supported inside functions");
            return;
        }

        /* Comments: pass through to flat array (skipped by execute, kept in save) */
        if (g_cmds[i].type == CMD_COMMENT) {
            if (g_num_flat_cmds < MAX_COMMANDS) {
                g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
                flat_cmd_set_provenance(&g_flat_cmds[g_num_flat_cmds],
                                        i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                g_num_flat_cmds++;
            } else {
                flatten_fail("Flattened command limit reached");
                return;
            }
            i++;
            continue;
        }

        if (g_cmds[i].type == CMD_VAR_DECLARE) { i++; continue; }

        /* Variable assignments: update predefined var and pass through */
        if (g_cmds[i].type == CMD_VAR_ASSIGN) {
            int var_idx = g_cmds[i].num_args; /* predef var index */
            float value = g_cmds[i].args[0];
            char rhs[MAX_LINE_LEN] = "";
            int local_rhs_vars = 0;

            if (repl_extract_assignment_parts(g_cmds[i].source, NULL, 0,
                                              rhs, sizeof(rhs)) && rhs[0]) {
                char repl_rhs[MAX_LINE_LEN];
                c_expr_to_repl(rhs, repl_rhs, sizeof(repl_rhs));
                ExprCtx ctx = { repl_rhs, vars, nv };
                value = eval_expr(&ctx);
                if (vars && nv > 0)
                    local_rhs_vars = input_has_expr_vars(rhs, vars, nv);
            }
            if (var_idx >= 0 && var_idx < g_num_predef_vars)
                g_predef_vars[var_idx].value = value;
            if (g_num_flat_cmds < MAX_COMMANDS) {
                GLCmd tmp = g_cmds[i];
                tmp.args[0] = value;
                tmp.has_vars = g_cmds[i].has_vars || local_rhs_vars;
                g_flat_cmds[g_num_flat_cmds] = tmp;
                g_flat_cmd_local_vars[g_num_flat_cmds].num_vars = 0;
                if (vars && nv > 0) {
                    int snap_n = nv < MAX_EXPR_VARS ? nv : MAX_EXPR_VARS;
                    g_flat_cmd_local_vars[g_num_flat_cmds].num_vars = snap_n;
                    memcpy(g_flat_cmd_local_vars[g_num_flat_cmds].vars, vars,
                           (size_t)snap_n * sizeof(ExprVar));
                }
                flat_cmd_set_provenance(&g_flat_cmds[g_num_flat_cmds],
                                        i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                g_num_flat_cmds++;
            } else {
                flatten_fail("Flattened command limit reached");
                return;
            }
            i++;
            continue;
        }

        /* Regular command */
        if (g_num_flat_cmds >= MAX_COMMANDS) {
            flatten_fail("Flattened command limit reached");
            return;
        }

        if (vars && nv > 0) {
            GLCmd tmp;
            memset(&tmp, 0, sizeof(tmp));
            int saved = g_edit_line;
            g_edit_line = g_num_flat_cmds;
            if (parse_command(g_cmds[i].source, &tmp, vars, nv)) {
                tmp.has_vars = g_cmds[i].has_vars;
                strncpy(tmp.source, g_cmds[i].source, sizeof(tmp.source) - 1);
                tmp.source[sizeof(tmp.source) - 1] = '\0';
                flat_cmd_set_provenance(&tmp, i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                /* Snapshot local vars so replay can show correct substitution */
                int snap_n = nv < MAX_EXPR_VARS ? nv : MAX_EXPR_VARS;
                g_flat_cmd_local_vars[g_num_flat_cmds].num_vars = snap_n;
                memcpy(g_flat_cmd_local_vars[g_num_flat_cmds].vars, vars,
                       (size_t)snap_n * sizeof(ExprVar));
                g_flat_cmds[g_num_flat_cmds++] = tmp;
            }
            g_edit_line = saved;
        } else if (g_cmds[i].has_vars) {
            /* Outside loop but has predefined var references: re-evaluate */
            GLCmd tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (parse_command(g_cmds[i].source, &tmp, NULL, 0)) {
                tmp.has_vars = 1;
                strncpy(tmp.source, g_cmds[i].source, sizeof(tmp.source) - 1);
                tmp.source[sizeof(tmp.source) - 1] = '\0';
                flat_cmd_set_provenance(&tmp, i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                g_flat_cmd_local_vars[g_num_flat_cmds].num_vars = 0;
                g_flat_cmds[g_num_flat_cmds++] = tmp;
            }
        } else {
            g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
            flat_cmd_set_provenance(&g_flat_cmds[g_num_flat_cmds],
                                    i, call_src_cmd_idx,
                                    root_call_src_cmd_idx,
                                    func_scope_mask);
            g_flat_cmd_local_vars[g_num_flat_cmds].num_vars = 0;
            g_num_flat_cmds++;
        }
        i++;
    }
}

void flatten_commands(void) {
    g_num_flat_cmds = 0;
    g_flatten_call_depth = 0;
    g_flatten_abort = 0;
    g_flatten_visit_budget = MAX_FLATTEN_VISIT_BUDGET;
    flatten_range(0, g_num_cmds, NULL, 0, -1, -1, 0);
    if (g_flatten_abort)
        g_num_flat_cmds = 0;

    /* Track whether user enabled lighting (for correct default state) */
    g_user_lighting_enabled = 0;
    for (int i = 0; i < g_num_cmds; i++) {
        if (g_cmds[i].valid && g_cmds[i].type == CMD_ENABLE &&
            g_cmds[i].mode == GL_LIGHTING)
            g_user_lighting_enabled = 1;
        if (g_cmds[i].valid && g_cmds[i].type == CMD_DISABLE &&
            g_cmds[i].mode == GL_LIGHTING)
            g_user_lighting_enabled = 0;
    }

    refresh_current_block_highlight();
}

int repl_flat_cmd_matches_cursor(int flat_idx) {
    if (flat_idx < 0 || flat_idx >= g_num_flat_cmds) return 0;
    if (g_edit_line < 0 || g_edit_line >= g_num_cmds) return 0;
    if (!g_flat_cmds[flat_idx].valid) return 0;
    if (g_current_block_line != g_edit_line)
        refresh_current_block_highlight();

    GLCmd *cmd = &g_flat_cmds[flat_idx];
    GLCmd *cursor_cmd = &g_cmds[g_edit_line];

    if (cursor_cmd->valid && cursor_cmd->type == CMD_CALL) {
        return cmd->call_src_cmd_idx == g_edit_line ||
               cmd->root_call_src_cmd_idx == g_edit_line;
    }

    {
        unsigned int cursor_func_mask = line_func_scope_mask(g_edit_line);
        if (cursor_func_mask != 0)
            return (cmd->func_scope_mask & cursor_func_mask) != 0;
    }

    if (g_current_block_begin >= 0 && g_current_block_end >= g_current_block_begin)
        return flat_idx >= g_current_block_begin && flat_idx <= g_current_block_end;

    /* Top-level color/normal commands outside glBegin/glEnd still affect later
     * vertices. Match those vertices to the most recent applicable state line
     * so block highlighting also works when the state is set before the block. */
    switch (cursor_cmd->type) {
    case CMD_COLOR3F:
    case CMD_COLOR4F: {
        if (cmd->type == CMD_VERTEX3F) {
            int last_color_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!g_flat_cmds[i].valid) continue;
                if (g_flat_cmds[i].type == CMD_COLOR3F ||
                    g_flat_cmds[i].type == CMD_COLOR4F)
                    last_color_src = g_flat_cmds[i].src_cmd_idx;
            }
            if (last_color_src == g_edit_line)
                return 1;
        }
        break;
    }
    case CMD_NORMAL3F: {
        if (cmd->type == CMD_VERTEX3F) {
            int last_normal_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!g_flat_cmds[i].valid) continue;
                if (g_flat_cmds[i].type == CMD_NORMAL3F)
                    last_normal_src = g_flat_cmds[i].src_cmd_idx;
            }
            if (last_normal_src == g_edit_line)
                return 1;
        }
        break;
    }
    case CMD_TESS_COLOR: {
        if (cmd->type == CMD_TESS_VERTEX) {
            int last_tess_color_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!g_flat_cmds[i].valid) continue;
                if (g_flat_cmds[i].type == CMD_TESS_COLOR)
                    last_tess_color_src = g_flat_cmds[i].src_cmd_idx;
            }
            if (last_tess_color_src == g_edit_line)
                return 1;
        }
        break;
    }
    case CMD_TESS_NORMAL: {
        if (cmd->type == CMD_TESS_VERTEX) {
            int last_tess_normal_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!g_flat_cmds[i].valid) continue;
                if (g_flat_cmds[i].type == CMD_TESS_NORMAL)
                    last_tess_normal_src = g_flat_cmds[i].src_cmd_idx;
            }
            if (last_tess_normal_src == g_edit_line)
                return 1;
        }
        break;
    }
    default:
        break;
    }

    return cmd->src_cmd_idx == g_edit_line;
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
/* Core execution helpers used by replay and display code. */
/* ========================================================================= */

void repl_copy_predef_values(float *dst, int max_vals) {
    int n;

    if (!dst || max_vals <= 0)
        return;

    n = g_num_predef_vars < max_vals ? g_num_predef_vars : max_vals;
    for (int i = 0; i < n; i++)
        dst[i] = g_predef_vars[i].value;
}

void repl_restore_predef_values(const float *src, int max_vals) {
    int n;

    if (!src || max_vals <= 0)
        return;

    n = g_num_predef_vars < max_vals ? g_num_predef_vars : max_vals;
    for (int i = 0; i < n; i++)
        g_predef_vars[i].value = src[i];
}

void repl_execute_set_fade_context(float alpha_scale, int skip_geom_before_pc) {
    g_execute_alpha_scale = alpha_scale;
    g_execute_skip_geom_before_pc = skip_geom_before_pc;
}

/* ========================================================================= */
/* Command execution                                                          */
/* ========================================================================= */

int apply_state_cmd(const GLCmd *cmd, float alpha_scale) {
    if (!cmd)
        return 0;

    switch (cmd->type) {
    case CMD_ENABLE:
        glEnable(cmd->mode);
        for (int li = 0; li < MAX_LIGHTS; li++)
            if (g_lights[li].id == cmd->mode)
                g_lights[li].enabled = 1;
        return 1;
    case CMD_DISABLE:
        glDisable(cmd->mode);
        for (int li = 0; li < MAX_LIGHTS; li++)
            if (g_lights[li].id == cmd->mode)
                g_lights[li].enabled = 0;
        return 1;
    case CMD_SHADE_MODEL:
        glShadeModel(cmd->mode);
        return 1;
    case CMD_COLOR_MATERIAL:
        glColorMaterial(cmd->mode, (GLenum)cmd->args[0]);
        return 1;
    case CMD_MATERIALF:
        if (cmd->num_args == 2) {
            glMaterialf(cmd->mode, (GLenum)cmd->args[0], cmd->args[1]);
        } else if (cmd->num_args == 5) {
            GLfloat mat[4] = {
                cmd->args[1], cmd->args[2], cmd->args[3],
                cmd->args[4] * alpha_scale
            };
            glMaterialfv(cmd->mode, (GLenum)cmd->args[0], mat);
        }
        return 1;
    case CMD_LIGHT_MODEL_I:
        glLightModeli(cmd->mode, (GLint)cmd->args[0]);
        return 1;
    case CMD_FRONT_FACE:
        glFrontFace(cmd->mode);
        return 1;
    case CMD_POINT_PARAMETER_FV: {
        GLfloat params[3] = { cmd->args[0], cmd->args[1], cmd->args[2] };
        glPointParameterfv(cmd->mode, params);
        return 1;
    }
    case CMD_BLEND_FUNC:
        glBlendFunc(cmd->mode, (GLenum)cmd->args[0]);
        return 1;
    default:
        return 0;
    }
}

/* Walk g_flat_cmds[0..g_num_flat_cmds) and issue the corresponding GL
 * calls.  Handles vertex submission, state changes (enable, material,
 * blend, etc.), GLU quadrics and tessellator commands, transforms,
 * goto/label control flow, if-block evaluation, and variable assignments.
 *
 * Called once per frame from display_func (or twice when accumulation AA
 * is active) with g_num_flat_cmds optionally clamped by the replay
 * subsystem. */
void execute_commands(void) {
    int in_begin = 0;
    int tess_depth = 0; /* 0=outside, 1=in polygon, 2=in contour */
    int matrix_depth = 0;
    GLdouble tess_current_normal[3] = {0.0, 0.0, 1.0};
    GLdouble tess_current_color[4]  = {1.0, 1.0, 1.0, 1.0};
    int goto_count = 0; /* safety guard against infinite goto loops */

    tess_current_color[3] = g_execute_alpha_scale;

    int pc = 0;
    while (pc < g_num_flat_cmds) {
        if (!g_flat_cmds[pc].valid) { pc++; continue; }
        if (is_transform_cmd(g_flat_cmds[pc].type)) {
            apply_tracked_transform_cmd(&g_flat_cmds[pc], &matrix_depth);
            pc++;
            continue;
        }
        if (pc < g_execute_skip_geom_before_pc) {
            /* Prefix walk: accumulate state but skip the expensive geometry.
             * Structural commands (CMD_BEGIN, CMD_END, CMD_TESS_BEGIN_POLYGON,
             * CMD_TESS_BEGIN_CONTOUR, CMD_TESS_END) are preserved so that in
             * REPLAY_MODE_VERTEX - where old_pc/new_pc may fall inside an open
             * begin/tess block - execute_commands still enters the right scope
             * before emitting the incremental vertices that live at
             * pc >= g_execute_skip_geom_before_pc. */
            switch (g_flat_cmds[pc].type) {
            case CMD_VERTEX3F:
            case CMD_VERTEX2F:
            case CMD_GLU_SPHERE:
            case CMD_GLU_CYLINDER:
            case CMD_GLU_DISK:
            case CMD_GLU_PARTIAL_DISK:
            case CMD_GLUT_TORUS:
            case CMD_TESS_VERTEX:
                pc++;
                continue;
            default:
                break;
            }
        }
        switch (g_flat_cmds[pc].type) {
        case CMD_BEGIN:
            if (in_begin) glEnd();
            glBegin(g_flat_cmds[pc].mode);
            in_begin = 1;
            break;
        case CMD_END:
            if (in_begin) { glEnd(); in_begin = 0; }
            break;
        case CMD_VERTEX3F:
            if (in_begin)
                glVertex3f(g_flat_cmds[pc].args[0], g_flat_cmds[pc].args[1],
                           g_flat_cmds[pc].args[2]);
            break;
        case CMD_NORMAL3F:
            glNormal3f(g_flat_cmds[pc].args[0], g_flat_cmds[pc].args[1],
                       g_flat_cmds[pc].args[2]);
            break;
        case CMD_COLOR3F:
            glColor4f(g_flat_cmds[pc].args[0], g_flat_cmds[pc].args[1],
                      g_flat_cmds[pc].args[2], g_execute_alpha_scale);
            break;
        case CMD_COLOR4F:
            glColor4f(g_flat_cmds[pc].args[0], g_flat_cmds[pc].args[1],
                      g_flat_cmds[pc].args[2],
                      g_flat_cmds[pc].args[3] * g_execute_alpha_scale);
            break;
        case CMD_ENABLE:
        case CMD_DISABLE:
        case CMD_SHADE_MODEL:
        case CMD_COLOR_MATERIAL:
        case CMD_MATERIALF:
        case CMD_LIGHT_MODEL_I:
            apply_state_cmd(&g_flat_cmds[pc], g_execute_alpha_scale);
            break;
        case CMD_VERTEX2F:
            if (in_begin)
                glVertex2f(g_flat_cmds[pc].args[0], g_flat_cmds[pc].args[1]);
            break;
        case CMD_FRONT_FACE:
            apply_state_cmd(&g_flat_cmds[pc], g_execute_alpha_scale);
            break;
        case CMD_POINT_SIZE:
            if (in_begin) { glEnd(); in_begin = 0; }
            glPointSize(g_flat_cmds[pc].args[0]);
            break;
        case CMD_POINT_PARAMETER_FV:
        case CMD_BLEND_FUNC:
            if (in_begin) { glEnd(); in_begin = 0; }
            apply_state_cmd(&g_flat_cmds[pc], g_execute_alpha_scale);
            break;
        case CMD_CLEAR_COLOR:
            if (in_begin) { glEnd(); in_begin = 0; }
            g_clear_color[0] = g_flat_cmds[pc].args[0];
            g_clear_color[1] = g_flat_cmds[pc].args[1];
            g_clear_color[2] = g_flat_cmds[pc].args[2];
            g_clear_color[3] = g_flat_cmds[pc].args[3];
            break;
        case CMD_GLU_SPHERE:
            if (in_begin) { glEnd(); in_begin = 0; }
            if (g_quadric)
                gluSphere(g_quadric,
                          (double)g_flat_cmds[pc].args[0],
                          (int)g_flat_cmds[pc].args[1],
                          (int)g_flat_cmds[pc].args[2]);
            break;
        case CMD_GLU_CYLINDER:
            if (in_begin) { glEnd(); in_begin = 0; }
            if (g_quadric)
                gluCylinder(g_quadric,
                            (double)g_flat_cmds[pc].args[0],
                            (double)g_flat_cmds[pc].args[1],
                            (double)g_flat_cmds[pc].args[2],
                            (int)g_flat_cmds[pc].args[3],
                            (int)g_flat_cmds[pc].args[4]);
            break;
        case CMD_GLU_DISK:
            if (in_begin) { glEnd(); in_begin = 0; }
            if (g_quadric)
                gluDisk(g_quadric,
                        (double)g_flat_cmds[pc].args[0],
                        (double)g_flat_cmds[pc].args[1],
                        (int)g_flat_cmds[pc].args[2],
                        (int)g_flat_cmds[pc].args[3]);
            break;
        case CMD_GLU_PARTIAL_DISK:
            if (in_begin) { glEnd(); in_begin = 0; }
            if (g_quadric)
                gluPartialDisk(g_quadric,
                               (double)g_flat_cmds[pc].args[0],
                               (double)g_flat_cmds[pc].args[1],
                               (int)g_flat_cmds[pc].args[2],
                               (int)g_flat_cmds[pc].args[3],
                               (double)g_flat_cmds[pc].args[4],
                               (double)g_flat_cmds[pc].args[5]);
            break;
        case CMD_GLUT_TORUS:
            if (in_begin) { glEnd(); in_begin = 0; }
            glutSolidTorus((double)g_flat_cmds[pc].args[0],
                           (double)g_flat_cmds[pc].args[1],
                           (int)g_flat_cmds[pc].args[2],
                           (int)g_flat_cmds[pc].args[3]);
            break;
        case CMD_TESS_BEGIN_POLYGON:
            if (in_begin) { glEnd(); in_begin = 0; }
            if (g_tess) { g_tess_vert_count = 0; gluTessBeginPolygon(g_tess, NULL); tess_depth = 1; }
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            if (g_tess && tess_depth == 1) { gluTessBeginContour(g_tess); tess_depth = 2; }
            break;
        case CMD_TESS_END:
            if (g_tess && tess_depth == 2) { gluTessEndContour(g_tess); tess_depth = 1; }
            else if (g_tess && tess_depth == 1) { gluTessEndPolygon(g_tess); tess_depth = 0; }
            break;
        case CMD_TESS_NORMAL:
            tess_current_normal[0] = g_flat_cmds[pc].args[0];
            tess_current_normal[1] = g_flat_cmds[pc].args[1];
            tess_current_normal[2] = g_flat_cmds[pc].args[2];
            break;
        case CMD_TESS_COLOR:
            tess_current_color[0] = g_flat_cmds[pc].args[0];
            tess_current_color[1] = g_flat_cmds[pc].args[1];
            tess_current_color[2] = g_flat_cmds[pc].args[2];
            tess_current_color[3] = ((g_flat_cmds[pc].num_args >= 4)
                                   ? g_flat_cmds[pc].args[3] : 1.0)
                                  * g_execute_alpha_scale;
            break;
        case CMD_TESS_VERTEX:
            if (g_tess && tess_depth == 2 && g_tess_vert_count < TESS_VERT_BUF_SIZE) {
                TessVertex *v = &g_tess_verts[g_tess_vert_count++];
                v->pos[0] = g_flat_cmds[pc].args[0];
                v->pos[1] = g_flat_cmds[pc].args[1];
                v->pos[2] = g_flat_cmds[pc].args[2];
                memcpy(v->normal, tess_current_normal, sizeof(v->normal));
                memcpy(v->color,  tess_current_color,  sizeof(v->color));
                gluTessVertex(g_tess, v->pos, v);
            }
            break;
        case CMD_LABEL:
            break; /* no-op marker */
        case CMD_GOTO: {
            /* Experimental top-level control-flow only.
             * This jumps the flat-command program counter, but it does not
             * rebuild or re-specialize the flat stream, so goto loops are only
             * reliable for control flow and assignments. Variable-driven GL
             * commands still use the args baked into g_flat_cmds[]. Replay also
             * cannot follow the dynamic jump trace. */
            char lname[64];
            if (!repl_extract_goto_label(g_flat_cmds[pc].source, lname, sizeof(lname)))
                break;
            /* Search for matching CMD_LABEL; guard against infinite loops */
            if (goto_count++ > 100000) {
                set_status("goto: loop limit reached");
                goto execute_done;
            }
            for (int li = 0; li < g_num_flat_cmds; li++) {
                if (g_flat_cmds[li].valid && g_flat_cmds[li].type == CMD_LABEL) {
                    char target_label[64];
                    if (repl_extract_label_name(g_flat_cmds[li].source,
                                                target_label,
                                                sizeof(target_label)) &&
                        strcmp(target_label, lname) == 0) {
                        pc = li; /* will be incremented at end of loop */
                        goto goto_done;
                    }
                }
            }
            goto_done:;
            break;
        }
        case CMD_IF_BEGIN: {
            /* Evaluate condition at execute time so goto loops see updated vars */
            float cond = g_flat_cmds[pc].args[0];
            if (g_flat_cmds[pc].has_vars) {
                char cond_text[MAX_LINE_LEN] = "";
                ExprVar *eval_vars = g_predef_vars;
                int eval_num_vars = g_num_predef_vars;
                if (g_flat_cmd_local_vars[pc].num_vars > 0) {
                    eval_vars = g_flat_cmd_local_vars[pc].vars;
                    eval_num_vars = g_flat_cmd_local_vars[pc].num_vars;
                }
                if (repl_extract_paren_payload(g_flat_cmds[pc].source,
                                              cond_text, sizeof(cond_text)) &&
                    cond_text[0]) {
                    char repl_cond[MAX_LINE_LEN];
                    c_expr_to_repl(cond_text, repl_cond, sizeof(repl_cond));
                    ExprCtx ctx = { repl_cond, eval_vars, eval_num_vars };
                    cond = eval_expr(&ctx);
                }
            }
            if (cond == 0.0f) {
                /* Skip to matching CMD_IF_END */
                int depth = 1;
                while (depth > 0 && ++pc < g_num_flat_cmds) {
                    if (g_flat_cmds[pc].type == CMD_IF_BEGIN) depth++;
                    else if (g_flat_cmds[pc].type == CMD_IF_END) depth--;
                }
                /* pc now points to CMD_IF_END; outer pc++ steps past it */
            }
            break;
        }
        case CMD_IF_END:
            break; /* body executed; just step past */
        case CMD_VAR_ASSIGN: {
            /* Re-apply variable assignment so goto loops see updated values */
            int var_idx = g_flat_cmds[pc].num_args;
            float value = g_flat_cmds[pc].args[0];
            if (g_flat_cmds[pc].has_vars) {
                char rhs[MAX_LINE_LEN] = "";
                if (repl_extract_assignment_parts(g_flat_cmds[pc].source, NULL, 0,
                                                  rhs, sizeof(rhs)) && rhs[0]) {
                    ExprVar *eval_vars = g_predef_vars;
                    int eval_num_vars = g_num_predef_vars;
                    if (g_flat_cmd_local_vars[pc].num_vars > 0) {
                        eval_vars = g_flat_cmd_local_vars[pc].vars;
                        eval_num_vars = g_flat_cmd_local_vars[pc].num_vars;
                    }
                    char repl_rhs[MAX_LINE_LEN];
                    c_expr_to_repl(rhs, repl_rhs, sizeof(repl_rhs));
                    ExprCtx ctx = { repl_rhs, eval_vars, eval_num_vars };
                    value = eval_expr(&ctx);
                }
            }
            if (var_idx >= 0 && var_idx < g_num_predef_vars)
                g_predef_vars[var_idx].value = value;
            break;
        }
        /* Transforms handled by is_transform_cmd() early-continue above */
        case CMD_TRANSLATE3F: case CMD_SCALEF: case CMD_ROTATEF:
        case CMD_PUSH_MATRIX: case CMD_POP_MATRIX:
        /* These are resolved during flatten and shouldn't appear in flat_cmds */
        case CMD_FOR_BEGIN: case CMD_FOR_END:
        case CMD_FUNC_DEF: case CMD_FUNC_END: case CMD_CALL:
        case CMD_COMMENT:
        case CMD_VAR_DECLARE:
        case CMD_TYPE_COUNT:
            break;
        }
        pc++;
    }
execute_done:;
    if (in_begin) glEnd();
    if (!(g_replay_active && g_replay_mode == REPLAY_MODE_VERTEX)) {
        if (tess_depth == 2 && g_tess) { gluTessEndContour(g_tess); tess_depth = 1; }
        if (tess_depth == 1 && g_tess) { gluTessEndPolygon(g_tess); }
    }
    unwind_tracked_transform_stack(&matrix_depth);
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

static unsigned int line_func_scope_mask(int line) {
    unsigned int mask = 0;
    int stack[32];
    int depth = 0;

    if (line < 0) return 0;
    if (line >= g_num_cmds) line = g_num_cmds - 1;

    for (int i = 0; i <= line && i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;

        if (g_cmds[i].type == CMD_FUNC_DEF) {
            int fn = (int)g_cmds[i].args[0];
            if (fn >= 0 && fn < 32 && depth < (int)(sizeof(stack) / sizeof(stack[0]))) {
                stack[depth++] = fn;
                mask |= (1u << fn);
            }
            continue;
        }

        if (g_cmds[i].type == CMD_FUNC_END) {
            if (i == line)
                return mask;
            if (depth > 0) {
                int fn = stack[--depth];
                mask &= ~(1u << fn);
            }
        }
    }

    return mask;
}

static void save_user_scene(void) {
    memcpy(g_user_scene.cmds, g_cmds, (size_t)g_num_cmds * sizeof(GLCmd));
    g_user_scene.num_cmds  = g_num_cmds;
    g_user_scene.edit_line = g_edit_line;
    g_user_scene.num_predef_vars = g_num_predef_vars;
    for (int i = 0; i < g_num_predef_vars; i++) {
        g_user_scene.predef_vals[i] = g_predef_vars[i].value;
        memcpy(g_user_scene.predef_names[i], g_predef_vars[i].name, 16);
    }
    g_user_scene_valid = 1;
}

static void restore_user_scene(void) {
    if (!g_user_scene_valid) return;
    memcpy(g_cmds, g_user_scene.cmds,
           (size_t)g_user_scene.num_cmds * sizeof(GLCmd));
    g_num_cmds     = g_user_scene.num_cmds;
    g_num_flat_cmds = 0;
    g_edit_line    = g_user_scene.edit_line;
    g_num_predef_vars = g_user_scene.num_predef_vars;
    for (int i = 0; i < g_user_scene.num_predef_vars; i++) {
        g_predef_vars[i].value = g_user_scene.predef_vals[i];
        memcpy(g_predef_vars[i].name, g_user_scene.predef_names[i], 16);
    }
    g_inserting = 0;
    load_line_to_input(g_edit_line);
    mark_normals_dirty();
    g_user_scene_valid = 0;
    g_example_idx = -1;
    set_status("Restored your scene");
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

    /* Preserve the user's work before overwriting with an example */
    if (!g_user_scene_valid)
        save_user_scene();

    load_example_lines(lines);
    g_example_idx = idx;
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
    /* Try importing from file first */
    if (import_file && load_from_file(import_file)) {
        g_edit_line = g_num_cmds;
        scroll_to_display_function();
        return;
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
    return g_user_scene_valid;
}

void repl_load_user_scene(void) {
    restore_user_scene();
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
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    g_newline_buf[0] = '\0';
    g_newline_len = 0;
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
