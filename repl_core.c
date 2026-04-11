/*
 * OpenGL REPL - Dynamic Display List Rendering
 *
 * A real-time OpenGL command interpreter. Type GL commands and watch the
 * geometry build up as each line is parsed when you press ';'.
 *
 * Supported commands:
 *   glBegin(MODE)        glEnd()
 *   glVertex3f(x,y,z)    glNormal3f(x,y,z)
 *   glColor3f(r,g,b)     glColor4f(r,g,b,a)
 *   glTranslatef(x,y,z)
 *   glEnable(CAP)        glDisable(CAP)
 *   glShadeModel(MODE)
 *
 * Math expressions: sin, cos, tan, sqrt, abs, pow, min, max, floor, ceil,
 *                   fmod, rand(seed[, iter]), PI, TAU
 *   Operators: + - * / % ( )   Comparison: > < >= <= == !=   Logical: && || !
 *   Example: glVertex3f(cos(PI/4), sin(PI/4), 0)
 *
 * Predefined variables: x, y, z, i, j, k, n, t
 *   Assignment: x = 1.5;
 *   't' auto-increments with time (Ctrl+T to play/pause)
 *
 * For-loops (saved as C for-loops, imported back as loops):
 *   for(i, 0, 24) glVertex3f(cos(i*TAU/24), sin(i*TAU/24), 0);
 *   for(i, 0, N) { body... }   Multi-line block
 *
 * Functions (define + call reusable blocks):
 *   func0 { body... }          Define function 0
 *   func0(radius, yoff) { ... } Define function 0 with arguments
 *   func0()                    Call function 0
 *   func0(1.5, x + 2)          Call function 0 with expressions
 *   Up to func0..func9
 *
 * Conditionals:
 *   if(expr) { body... }       Body included when expr is non-zero
 *
 * Controls:
 *   Type + ;       Execute / commit line
 *   Enter          Insert new line (works in middle of list)
 *   Up/Down        Navigate between command lines
 *   Left/Right     Move cursor within input line
 *   Home/End       Jump to start/end of input line
 *   Backspace      Delete character before cursor
 *   Ctrl+/         Toggle comment (// prefix)
 *   Shift+Up/Down  Select multiple lines
 *   Ctrl+A         Move cursor to start of input line
 *   Ctrl+C         Copy line/selection (whole for-loop on FOR_BEGIN)
 *   Ctrl+X         Cut line/selection (whole for-loop on FOR_BEGIN)
 *   Ctrl+V         Paste before current line
 *   Ctrl+E         Move cursor to end of input line
 *   Ctrl+Z         Undo last command
 *   Ctrl+D         Delete line at cursor
 *   Ctrl+L         Clear all commands
 *   Ctrl+R         Reformat all command lines
 *   Ctrl+P         Dump current editor code to stdout
 *   Ctrl+S         Save to output.c
 *   Ctrl+Q         Exit
 *   Escape         Clear input / exit insert mode / close help
 *   Tab            Accept autocomplete suggestion
 *   Left-drag      Orbit camera
 *   Right-drag     Pan camera
 *   Scroll wheel   Zoom (or scroll code panel when cursor is over panel)
 *   F1-F10         Toggle overlays (help/wire/grid/axes/vnums/normals/indices/guides/autonorm/lights)
 *   F12            Cycle predefined examples
 *   PgUp/PgDn      Scroll code panel
 *   Ctrl+T         Toggle time variable 't' play/pause
 *   Ctrl+U         Toggle multisample state
 *   Ctrl+N         Toggle GL_LINE_SMOOTH state
 *   Ctrl+B         Toggle accumulation-buffer AA
 *   Ctrl+=         Increase AA jitter samples (1→2→4→8→16)
 *   Ctrl+-         Decrease AA jitter samples (16→8→4→2→1)
 *
 * Command-line flags:
 *   --noaccum      Disable accumulation buffer (enabled by default)
 *   --dump-code    Print loaded editor buffer to stdout at startup
 *
 * Import/Export:
 *   Ctrl+S saves to output.c with snippet markers.
 *   Run ./sample output.c to reload a saved session.
 */

#include "sample.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "cmd_format.h"
#include "repl_examples.h"
#include "scene_render.h"
#include "ui_panels.h"

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
    { "for(",                "for(var, start, end[, step])",                             4, { "var", "start", "end", "step" } },
    { "if(",                 "if(expr)",                                                 1, { "expr" } },
    { "goto ",               "goto label",                                               0, { NULL } },
    { "func0 {",             "func0 {",                                                  0, { NULL } },
    { "func0(radius, yoff) {", "func0(radius, yoff) {",                                  0, { NULL } },
    { "func1 {",             "func1 {",                                                  0, { NULL } },
    { "func2 {",             "func2 {",                                                  0, { NULL } },
    { "func3 {",             "func3 {",                                                  0, { NULL } },
    { "func0()",             "func0()",                                                  0, { NULL } },
    { "func1()",             "func1()",                                                  0, { NULL } },
    { "func2()",             "func2()",                                                  0, { NULL } },
    { "func3()",             "func3()",                                                  0, { NULL } },
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

static void depth_cache_rebuild(void) {
    if (!g_depth_cache_dirty) return;

    g_for_depth_prefix[0] = 0;
    g_block_depth_prefix[0] = 0;
    g_begin_depth_prefix[0] = 0;
    g_tess_depth_prefix[0] = 0;

    for (int i = 0; i < g_num_cmds; i++) {
        int fd = g_for_depth_prefix[i];
        int bd = g_block_depth_prefix[i];
        int gd = g_begin_depth_prefix[i];
        int td = g_tess_depth_prefix[i];

        if (g_cmds[i].valid) {
            CmdType t = g_cmds[i].type;

            if (t == CMD_FOR_BEGIN) fd++;
            else if (t == CMD_FOR_END) fd--;

            if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) bd++;
            else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) bd--;

            if (t == CMD_BEGIN) gd++;
            else if (t == CMD_END) gd--;

            if (t == CMD_TESS_BEGIN_POLYGON || t == CMD_TESS_BEGIN_CONTOUR) td++;
            else if (t == CMD_TESS_END) td--;
        }

        if (fd < 0) fd = 0;
        if (bd < 0) bd = 0;
        if (gd < 0) gd = 0;
        if (td < 0) td = 0;

        g_for_depth_prefix[i + 1] = fd;
        g_block_depth_prefix[i + 1] = bd;
        g_begin_depth_prefix[i + 1] = gd;
        g_tess_depth_prefix[i + 1] = td;
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
float  g_cam_px = 0.0f, g_cam_py = 0.0f;

/* Window */
int    g_win_w = 1200, g_win_h = 800;

/* Accumulation buffer — enabled by default, disabled with --noaccum.
 * Designed to be forward-compatible with FBO-based accumulation later. */
int    g_use_accum        = 1;  /* GLUT_ACCUM requested at init */
int    g_accum_aa_enabled = 1;  /* Ctrl+B toggles jitter AA on/off */
int    g_accum_samples    = 2;  /* current sample count */
float  g_accum_jitter_x   = 0.0f;
float  g_accum_jitter_y   = 0.0f;
int    g_multisample_enabled = 1;
int    g_line_smooth_enabled = 1; /* GL_LINE_SMOOTH state, looks nice with correct blending */

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
int    g_wireframe    = 0;
int    g_grid_theme   = 2;  /* 0=off, 1=classic, 2=fog, 3=tron, 4=ember, 5=faint, 6=focus */
const char *g_grid_names[] = {
    "Grid OFF", "Grid: Classic", "Grid: Fog", "Grid: Tron", "Grid: Ember",
    "Grid: Faint", "Grid: Focus"
};
float  g_focus_vtx[3] = { 0.0f, 0.0f, 0.0f };  /* last vertex pos for focus grid */
int    g_focus_vtx_valid = 0;
int    g_axes_theme   = 3;  /* 0=off, 1=classic, 2=pulse, 3=neon, 4=compass */
const char *g_axes_names[] = {
    "Axes OFF", "Axes: Classic", "Axes: Pulse", "Axes: Neon", "Axes: Compass"
};
int    g_show_vnums   = 1;
int    g_show_normals = 0;
int    g_show_indices = 1;
int    g_wrap_at_comma = 1;
int    g_layout_vertical = 0;  /* 0=left code panel, 1=top code panel */
int    g_show_guides  = 1;
int    g_autonormal   = 0;
int    g_show_lights  = 1;
int    g_cam_rotate   = 0;  /* auto-rotate camera around Y */
int    g_example_idx  = -1; /* current predefined example (-1 = none loaded yet) */
int    g_user_lighting_enabled = 0; /* tracks if user typed glEnable(GL_LIGHTING) */
int    g_show_outlines = 1; /* draw black wireframe over filled polygons */
int    g_show_vpoints  = 1; /* draw black dots at each vertex position */
int    g_highlight_current_poly = 1; /* highlight glBegin block under cursor */
int    g_current_block_begin = -1;  /* flat cmd index of cursor's glBegin */
int    g_current_block_end   = -1;  /* flat cmd index of cursor's glEnd */
static int g_current_block_line = -1; /* g_edit_line used to compute block */
int    g_ortho_mode = 0;  /* 0=perspective, 1=2D orthographic */

/* Replay */
int    g_replay_active = 0;
int    g_replay_state = REPLAY_OFF;
int    g_replay_pc = 0;
int    g_replay_mode = REPLAY_MODE_VERTEX;
float  g_replay_speed = 4.0f;
float  g_replay_accum = 0.0f;
float  g_replay_fade_alpha = 1.0f;
float  g_replay_fade_speed = 2.0f;
int    g_replay_fade_begin = -1;
int    g_replay_fade_end = -1;
int    g_replay_src_line = -1;
int    g_replay_total_flat = 0;
static float g_replay_baseline_predef_vals[MAX_PREDEF_VARS];
static int   g_replay_saved_t_playing = 1;
static int   g_replay_last_src_line = -1;

#define REPLAY_FADE_DURATION   0.5f
#define REPLAY_FADE_BATCH_MAX  64

typedef struct {
    int   old_pc;
    int   new_pc;
    float age;
} ReplayFadeBatch;

static ReplayFadeBatch g_replay_fade_batches[REPLAY_FADE_BATCH_MAX];
static int             g_replay_fade_batch_count = 0;
static float           g_execute_alpha_scale = 1.0f;

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

/* Status bar */
char   g_status[256] = "";
int    g_status_ttl = 0;

/* Autocomplete */
const char *g_ac_matches[MAX_AC_MATCHES];
int    g_ac_count = 0;
int    g_ac_sel = 0;
char   g_ac_ghost[MAX_LINE_LEN] = "";
char   g_ac_hint[MAX_LINE_LEN] = "";
static const char *g_ac_insert_matches[MAX_AC_MATCHES];
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
static int parse_command(const char *line, GLCmd *cmd);
static int parse_command_with_vars(const char *line, GLCmd *cmd,
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

static int cmd_type_needs_semicolon(CmdType t) {
    switch (t) {
    case CMD_COMMENT:
    case CMD_LABEL:
        return 0;
    default:
        return 1;
    }
}

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
        return 0;
    default:
        return 1;
    }
}

static const char *cmd_type_name(CmdType t) {
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
        "CMD_FOR_BEGIN", "CMD_FOR_END",
        "CMD_FUNC_DEF", "CMD_FUNC_END", "CMD_CALL",
        "CMD_IF_BEGIN", "CMD_IF_END",
        "CMD_COMMENT",
        "CMD_VAR_ASSIGN",
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
        "CMD_BLEND_FUNC"
    };

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
    fprintf(dst, "rx=%g ry=%g dist=%g px=%g py=%g\n",
            (double)g_cam_rx, (double)g_cam_ry, (double)g_cam_dist,
            (double)g_cam_px, (double)g_cam_py);
    update_lookat_strings();
    for (int i = 0; i < LOOKAT_LINE_COUNT; i++)
        fprintf(dst, "%s\n", g_lookat[i]);
    fprintf(dst, "--- init ---\n");
    for (int i = 0; i < init_section_line_count(); i++) {
        char line[MAX_LINE_LEN];
        init_section_line(i, line, sizeof(line));
        fprintf(dst, "%s\n", line);
    }
    fprintf(dst, "=== End REPL Editor Dump ===\n");
    fflush(dst);
}

static void normalize_with_indent(const char *raw_expr, int indent_spaces,
                                  int ensure_semicolon, char *out, int out_sz) {
    if (out_sz <= 0) return;

    const char *p = raw_expr;
    while (*p == ' ' || *p == '\t') p++;

    char body[MAX_LINE_LEN];
    strncpy(body, p, sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';

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
        strncpy(body, spaced, sizeof(body) - 1);
        body[sizeof(body) - 1] = '\0';
    }

    if (indent_spaces < 0) indent_spaces = 0;
    if (indent_spaces > out_sz - 1) indent_spaces = out_sz - 1;
    memset(out, ' ', (size_t)indent_spaces);
    out[indent_spaces] = '\0';
    strncat(out, body, (size_t)(out_sz - 1 - indent_spaces));
}

int repl_parse_and_normalize(const char *line, int pos,
                             ExprVar *vars, int num_vars,
                             int preserve_expr, GLCmd *out_cmd) {
    int saved = g_edit_line;
    g_edit_line = pos;
    int parsed;
    if (vars && num_vars > 0)
        parsed = parse_command_with_vars(line, out_cmd, vars, num_vars);
    else
        parsed = parse_command(line, out_cmd);
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
            if (name && rhs[0])
                snprintf(fmt.source, sizeof(fmt.source), "%s%s = %s;", ind_s, name, rhs);
            else if (name)
                snprintf(fmt.source, sizeof(fmt.source), "%s%s = %g;", ind_s, name, orig.args[0]);
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
            ExprVar dvars[MAX_EXPR_VARS];
            int dnv = collect_visible_vars(i, dvars, MAX_EXPR_VARS);
            int preserve_expr = (dnv > 0) || orig.has_vars;
            GLCmd parsed;
            memset(&parsed, 0, sizeof(parsed));
            if (repl_parse_and_normalize(orig.source, i,
                                         dnv > 0 ? dvars : NULL,
                                         dnv > 0 ? dnv : 0,
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
}

/* ========================================================================= */
/* Autocomplete                                                               */
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

/* Forward declarations */
static int parse_command_with_vars(const char *line, GLCmd *cmd,
                                   ExprVar *vars, int num_vars);
static unsigned int line_func_scope_mask(int line);
static void get_for_var_name(const GLCmd *cmd, char *var, int var_sz);

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

static int parse_command_internal(const char *line, GLCmd *cmd,
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

    if (open_p && close_p && close_p > open_p) {
        int flen = (int)(open_p - p);
        if (flen > 0 && flen < (int)sizeof(func)) {
            strncpy(func, p, flen);
            func[flen] = '\0';
        }
        int alen = (int)(close_p - open_p - 1);
        if (alen > 0 && alen < (int)sizeof(args)) {
            strncpy(args, open_p + 1, alen);
            args[alen] = '\0';
        }
    } else {
        strncpy(func, p, sizeof(func) - 1);
    }

    /* Table-driven parsing for enum commands */
    for (const EnumCmdDef *def = g_enum_cmds; def->name; def++) {
        if (strcmp(func, def->name) == 0) {
            if (def->num_args == 1) {
                char *a = args;
                while (*a && isspace((unsigned char)*a)) a++;
                int al = (int)strlen(a);
                while (al > 0 && isspace((unsigned char)a[al - 1])) a[--al] = '\0';
                for (int i = 0; def->enums1[i].name; i++) {
                    if (strcmp(a, def->enums1[i].name) == 0) {
                        cmd->type = def->type;
                        cmd->mode = def->enums1[i].value;
                        cmd->valid = 1;
                        if (def->indent_type == 1) {
                            char _bi[32]; int _td=tess_scope_depth_at(g_edit_line),_sp=2+2*_td;
                            if(_sp>(int)sizeof(_bi)-1)_sp=(int)sizeof(_bi)-1; memset(_bi,' ',_sp);_bi[_sp]='\0';
                            snprintf(cmd->source,sizeof(cmd->source), def->fmt, _bi, def->enums1[i].name);
                        } else {
                            char _ind[32]; cmd_indent(g_edit_line,_ind,sizeof(_ind));
                            snprintf(cmd->source,sizeof(cmd->source), def->fmt, _ind, def->enums1[i].name);
                        }
                        return 1;
                    }
                }
                set_status(def->usage1);
                return 0;
            } else if (def->num_args == 2) {
                char a1[64] = "", a2[64] = "";
                char *comma = strchr(args, ',');
                if (!comma) { set_status(def->usage1 ? def->usage1 : "Invalid arguments"); return 0; }
                int l1 = (int)(comma - args);
                if (l1 >= (int)sizeof(a1)) l1 = (int)sizeof(a1) - 1;
                strncpy(a1, args, l1); a1[l1] = '\0';
                strncpy(a2, comma + 1, sizeof(a2) - 1);

                char *p1 = a1; while (*p1 == ' ') p1++;
                int e1 = (int)strlen(p1); while (e1 > 0 && p1[e1-1] == ' ') p1[--e1] = '\0';
                char *p2 = a2; while (*p2 == ' ') p2++;
                int e2 = (int)strlen(p2); while (e2 > 0 && p2[e2-1] == ' ') p2[--e2] = '\0';

                GLenum val1 = 0;
                int found1 = 0, found2 = 0;
                float val2_f = 0.0f;

                for (int i = 0; def->enums1[i].name; i++) {
                    if (strcmp(p1, def->enums1[i].name) == 0) { val1 = def->enums1[i].value; found1 = 1; break; }
                }
                for (int i = 0; def->enums2[i].name; i++) {
                    if (strcmp(p2, def->enums2[i].name) == 0) { val2_f = (float)def->enums2[i].value; found2 = 1; break; }
                }
                if (!found1) { set_status(def->usage1); return 0; }

                if (!found2 && def->type == CMD_LIGHT_MODEL_I) {
                    float fv; if (parse_exprs(p2, &fv, 1, vars, num_vars) == 1) { val2_f = fv; found2 = 1; }
                }

                if (!found2) { set_status(def->usage2); return 0; }

                cmd->type = def->type;
                cmd->valid = 1;
                cmd->mode = val1;
                cmd->args[0] = val2_f;
                cmd->num_args = 1;
                char _ind[32]; cmd_indent(g_edit_line,_ind,sizeof(_ind));
                snprintf(cmd->source, sizeof(cmd->source), def->fmt, _ind, p1, p2);
                return 1;
            }
        }
    }

    /* glEnd() — aligns with its matching glBegin (begin depth not added) */
    if (strcmp(func, "glEnd") == 0) {
        cmd->type = CMD_END;
        cmd->valid = 1;
        {
            int td = tess_scope_depth_at(g_edit_line);
            int spaces = 2 + 2 * td;
            char _ei[32];
            if (spaces > (int)sizeof(_ei) - 1) spaces = (int)sizeof(_ei) - 1;
            memset(_ei, ' ', (size_t)spaces);
            _ei[spaces] = '\0';
            snprintf(cmd->source, sizeof(cmd->source), "%sglEnd();", _ei);
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
            cmd->num_args = parse_exprs(args, cmd->args, def->num_args, vars, num_vars);
            if (cmd->num_args == def->num_args) {
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
                return 1;
            }
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

        char _ind[32]; cmd_indent(g_edit_line, _ind, sizeof(_ind));
        snprintf(cmd->source, sizeof(cmd->source),
                 "%sglPointParameterfv(%s, (GLfloat[]){%g, %g, %g});",
                 _ind, p1, parsed_args[0], parsed_args[1], parsed_args[2]);
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
        if (arg_count > 0)
            snprintf(cmd->source, sizeof(cmd->source), "%sfunc%d(%s);",
                     ind_str, fn, raw_args);
        else
            snprintf(cmd->source, sizeof(cmd->source), "%sfunc%d();", ind_str, fn);
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

    /* :label — define a label */
    if (p[0] == ':' && p[1] && !isspace((unsigned char)p[1])) {
        cmd->type = CMD_LABEL;
        cmd->valid = 1;
        /* labels go at column 0 in C */
        snprintf(cmd->source, sizeof(cmd->source), "%s:", p + 1);
        return 1;
    }

    set_status("Unknown cmd. Try glVertex3f, glBegin, glEnable, glShadeModel, ...");
    return 0;
}

static int parse_command(const char *line, GLCmd *cmd) {
    return parse_command_internal(line, cmd, NULL, 0);
}

static int parse_command_with_vars(const char *line, GLCmd *cmd,
                                   ExprVar *vars, int num_vars) {
    return parse_command_internal(line, cmd, vars, num_vars);
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

static void flat_cmd_set_provenance(GLCmd *cmd, int src_cmd_idx,
                                    int call_src_cmd_idx,
                                    int root_call_src_cmd_idx,
                                    unsigned int func_scope_mask) {
    cmd->src_cmd_idx = src_cmd_idx;
    cmd->call_src_cmd_idx = call_src_cmd_idx;
    cmd->root_call_src_cmd_idx = root_call_src_cmd_idx;
    cmd->func_scope_mask = func_scope_mask;
}

static void refresh_current_block_highlight(void) {
    g_current_block_begin = -1;
    g_current_block_end   = -1;
    g_current_block_line  = g_edit_line;

    /* Walk flat cmds: track which source line each cmd came from via g_cmds index */
    /* Approximate: find last BEGIN at or before g_edit_line, then matching END */
    int found_begin = -1;
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_BEGIN) { found_begin = i; }
        else if (g_flat_cmds[i].type == CMD_END && found_begin >= 0) {
            found_begin = -1;
        }
    }

    /* Better approach: scan g_cmds for the innermost BEGIN/END containing g_edit_line */
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
                    begin_src = -1; begin_flat = -1;
                    break;
                } else if (begin_src >= 0 && ci <= g_edit_line) {
                    begin_src = -1; begin_flat = -1;
                }
            }
            fcur++;
        }
    }
}

/* Flatten g_cmds (with for-loops) into g_flat_cmds (concrete commands) */
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
            int fe = find_block_end(i);
            GLCmd *fb_cmd = &g_cmds[i];
            char var_name[16];
            get_for_var_name(fb_cmd, var_name, sizeof(var_name));
            float s = fb_cmd->args[0], e = fb_cmd->args[1], st = fb_cmd->args[2];

            /* Re-evaluate for-loop bounds from source if they contain variables */
            if (fb_cmd->has_vars) {
                const char *unused_body;
                float rs, re, rst;
                char rv[16];
                if (parse_for_header_with_vars(fb_cmd->source, rv, sizeof(rv),
                                               &rs, &re, &rst,
                                               vars, nv, &unused_body)) {
                    s = rs; e = re; st = rst;
                }
            }

            if (fabsf(st) > 1e-9f &&
                !((st > 0 && s >= e) || (st < 0 && s <= e))) {
                int max_iters = 100000;
                for (float val = s;
                     (st > 0) ? (val < e - 1e-6f) : (val > e + 1e-6f);
                     val += st) {
                    if (--max_iters < 0) break;
                    if (g_flatten_abort) return;
                    ExprVar lvars[MAX_EXPR_VARS];
                    int lnv = 0;
                    if (lnv < MAX_EXPR_VARS) {
                        strncpy(lvars[lnv].name, var_name,
                                sizeof(lvars[lnv].name) - 1);
                        lvars[lnv].name[sizeof(lvars[lnv].name) - 1] = '\0';
                        lvars[lnv].value = val;
                        lnv++;
                    }
                    if (vars)
                        for (int v = 0; v < nv && lnv < MAX_EXPR_VARS; v++)
                            lvars[lnv++] = vars[v];
                    flatten_range(i + 1, fe, lvars, lnv,
                                  call_src_cmd_idx, root_call_src_cmd_idx,
                                  func_scope_mask);
                }
            }
            i = (fe < g_num_cmds) ? fe + 1 : g_num_cmds;
            continue;
        }

        if (g_cmds[i].type == CMD_FOR_END) { i++; continue; }

        /* Function definitions: skip body (expanded at call sites) */
        if (g_cmds[i].type == CMD_FUNC_DEF) {
            int fe = find_block_end(i);
            i = (fe < g_num_cmds) ? fe + 1 : g_num_cmds;
            continue;
        }
        if (g_cmds[i].type == CMD_FUNC_END) { i++; continue; }

        /* Function calls: find definition and expand body inline */
        if (g_cmds[i].type == CMD_CALL) {
            int fn = (int)g_cmds[i].args[0];
            if (g_flatten_call_depth >= MAX_FLATTEN_CALL_DEPTH) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "Recursive expansion exceeded depth limit (%d) at func%d",
                         MAX_FLATTEN_CALL_DEPTH, fn);
                flatten_fail(msg);
                i++;
                continue;
            }

            for (int k = 0; k < g_num_cmds; k++) {
                if (g_cmds[k].type == CMD_FUNC_DEF && (int)g_cmds[k].args[0] == fn) {
                    int fe = find_block_end(k);
                    int def_fn = fn;
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
                                 fn, param_count, arg_count);
                        set_status(msg);
                        break;
                    }

                    ExprVar lvars[MAX_EXPR_VARS];
                    int lnv = 0;
                    for (int p = 0; p < param_count && lnv < MAX_EXPR_VARS; p++) {
                        strncpy(lvars[lnv].name, param_names[p],
                                sizeof(lvars[lnv].name) - 1);
                        lvars[lnv].name[sizeof(lvars[lnv].name) - 1] = '\0';
                        lvars[lnv].value = arg_vals[p];
                        lnv++;
                    }
                    for (int v = 0; vars && v < nv && lnv < MAX_EXPR_VARS; v++)
                        lvars[lnv++] = vars[v];

                    unsigned int nested_func_mask = func_scope_mask;
                    if (fn >= 0 && fn < 32)
                        nested_func_mask |= (1u << fn);
                    int nested_root_call = (root_call_src_cmd_idx >= 0)
                                         ? root_call_src_cmd_idx : i;

                    g_flatten_call_depth++;
                    flatten_range(k + 1, fe, lvars, lnv,
                                  i, nested_root_call, nested_func_mask);
                    if (g_flatten_call_depth > 0) g_flatten_call_depth--;
                    break;
                }
            }
            i++;
            continue;
        }

        if (g_cmds[i].type == CMD_IF_BEGIN) {
            int fe = find_block_end(i);
            char cond_text[MAX_LINE_LEN];
            int needs_local_eval = 0;

            if (vars && nv > 0 &&
                repl_extract_paren_payload(g_cmds[i].source, cond_text, sizeof(cond_text)) &&
                input_has_expr_vars(cond_text, vars, nv)) {
                needs_local_eval = 1;
            }

            if (needs_local_eval) {
                ExprCtx ctx = { cond_text, vars, nv };
                float cond = eval_expr(&ctx);
                if (cond != 0.0f)
                    flatten_range(i + 1, fe, vars, nv,
                                  call_src_cmd_idx, root_call_src_cmd_idx,
                                  func_scope_mask);
                i = (fe < g_num_cmds) ? fe + 1 : g_num_cmds;
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

        /* Variable assignments: update predefined var and pass through */
        if (g_cmds[i].type == CMD_VAR_ASSIGN) {
            int vi = g_cmds[i].num_args; /* predef var index */
            float value = g_cmds[i].args[0];
            char rhs[MAX_LINE_LEN] = "";
            int local_rhs_vars = 0;

            if (repl_extract_assignment_parts(g_cmds[i].source, NULL, 0,
                                              rhs, sizeof(rhs)) && rhs[0]) {
                ExprCtx ctx = { rhs, vars, nv };
                value = eval_expr(&ctx);
                if (vars && nv > 0)
                    local_rhs_vars = input_has_expr_vars(rhs, vars, nv);
            }
            if (vi >= 0 && vi < g_num_predef_vars)
                g_predef_vars[vi].value = value;
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
            if (parse_command_with_vars(g_cmds[i].source, &tmp, vars, nv)) {
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
            if (parse_command(g_cmds[i].source, &tmp)) {
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
/* Replay helpers                                                             */
/* ========================================================================= */

static void copy_predef_values(float *dst) {
    for (int i = 0; i < g_num_predef_vars; i++)
        dst[i] = g_predef_vars[i].value;
}

static void restore_predef_values(const float *src) {
    for (int i = 0; i < g_num_predef_vars; i++)
        g_predef_vars[i].value = src[i];
}

static int replay_enabled(void) {
    return g_replay_active && g_replay_state != REPLAY_OFF;
}

static int replay_has_meaningful_cmds(void) {
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_COMMENT) continue;
        return 1;
    }
    return 0;
}

static int replay_find_open_begin_before(int limit) {
    int open_begin = -1;
    int in_begin = 0;

    for (int i = 0; i < limit && i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_BEGIN) {
            open_begin = i;
            in_begin = 1;
        } else if (g_flat_cmds[i].type == CMD_END && in_begin) {
            open_begin = -1;
            in_begin = 0;
        }
    }

    return open_begin;
}

static int replay_find_open_tess_polygon_before(int limit, int *out_depth) {
    int poly_start = -1;
    int tess_depth = 0;

    for (int i = 0; i < limit && i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        switch (g_flat_cmds[i].type) {
        case CMD_TESS_BEGIN_POLYGON:
            poly_start = i;
            tess_depth = 1;
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            if (tess_depth == 1)
                tess_depth = 2;
            break;
        case CMD_TESS_END:
            if (tess_depth == 2) {
                tess_depth = 1;
            } else if (tess_depth == 1) {
                tess_depth = 0;
                poly_start = -1;
            }
            break;
        default:
            break;
        }
    }

    if (out_depth) *out_depth = tess_depth;
    return poly_start;
}

static int replay_find_matching_gl_end(int begin_idx) {
    for (int i = begin_idx + 1; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_END)
            return i;
    }
    return g_num_flat_cmds > 0 ? g_num_flat_cmds - 1 : begin_idx;
}

static int replay_last_meaningful_src(int begin, int end_exclusive) {
    for (int i = end_exclusive - 1; i >= begin && i >= 0; i--) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_COMMENT) continue;
        if (g_flat_cmds[i].src_cmd_idx >= 0)
            return g_flat_cmds[i].src_cmd_idx;
    }
    return -1;
}

static void replay_set_src_line(int src_line) {
    g_replay_src_line = src_line;
    if (src_line != g_replay_last_src_line) {
        g_replay_last_src_line = src_line;
        if (src_line >= 0)
            g_scroll_follow_cursor = 1;
    }
}

static float replay_batch_alpha(const ReplayFadeBatch *batch) {
    float alpha = batch->age * g_replay_fade_speed;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return alpha;
}

static void replay_sync_legacy_fade_state(void) {
    if (g_replay_fade_batch_count > 0) {
        ReplayFadeBatch *batch = &g_replay_fade_batches[g_replay_fade_batch_count - 1];
        g_replay_fade_begin = batch->old_pc;
        g_replay_fade_end = batch->new_pc - 1;
        g_replay_fade_alpha = replay_batch_alpha(batch);
    } else {
        g_replay_fade_begin = -1;
        g_replay_fade_end = -1;
        g_replay_fade_alpha = 1.0f;
    }
}

static void replay_clear_fade_batches(void) {
    g_replay_fade_batch_count = 0;
    replay_sync_legacy_fade_state();
}

static void replay_push_fade_batch(int old_pc, int new_pc) {
    ReplayFadeBatch *batch;

    if (new_pc <= old_pc)
        return;

    if (g_replay_fade_batch_count >= REPLAY_FADE_BATCH_MAX) {
        memmove(&g_replay_fade_batches[0], &g_replay_fade_batches[1],
                (size_t)(REPLAY_FADE_BATCH_MAX - 1) * sizeof(g_replay_fade_batches[0]));
        g_replay_fade_batch_count = REPLAY_FADE_BATCH_MAX - 1;
    }

    batch = &g_replay_fade_batches[g_replay_fade_batch_count++];
    batch->old_pc = old_pc;
    batch->new_pc = new_pc;
    batch->age = 0.016f;
    replay_sync_legacy_fade_state();
}

static void replay_clamp_fade_batches(int max_pc) {
    int dst = 0;

    for (int i = 0; i < g_replay_fade_batch_count; i++) {
        ReplayFadeBatch batch = g_replay_fade_batches[i];

        if (batch.old_pc > max_pc)
            continue;
        if (batch.new_pc > max_pc)
            batch.new_pc = max_pc;
        if (batch.new_pc <= batch.old_pc)
            continue;
        g_replay_fade_batches[dst++] = batch;
    }

    g_replay_fade_batch_count = dst;
    replay_sync_legacy_fade_state();
}

void replay_tick_fade_batches(float dt) {
    int dst = 0;

    for (int i = 0; i < g_replay_fade_batch_count; i++) {
        ReplayFadeBatch batch = g_replay_fade_batches[i];
        batch.age += dt;
        if (batch.age >= REPLAY_FADE_DURATION)
            continue;
        g_replay_fade_batches[dst++] = batch;
    }

    g_replay_fade_batch_count = dst;
    replay_sync_legacy_fade_state();
}

int replay_has_active_fades(void) {
    return g_replay_active && g_replay_fade_batch_count > 0;
}

int replay_fill_base_limit(void) {
    if (!replay_has_active_fades())
        return g_num_flat_cmds;
    if (g_replay_fade_batches[0].old_pc < 0)
        return 0;
    if (g_replay_fade_batches[0].old_pc > g_num_flat_cmds)
        return g_num_flat_cmds;
    return g_replay_fade_batches[0].old_pc;
}

static int replay_next_polygon_limit(int start, int *fade_begin, int *fade_end) {
    int saw_meaningful = 0;

    *fade_begin = -1;
    *fade_end = -1;

    for (int i = start; i < g_num_flat_cmds; i++) {
        CmdType t;

        if (!g_flat_cmds[i].valid || g_flat_cmds[i].type == CMD_COMMENT)
            continue;

        t = g_flat_cmds[i].type;
        saw_meaningful = 1;

        switch (t) {
        case CMD_BEGIN: {
            int end = replay_find_matching_gl_end(i);
            *fade_begin = start;
            *fade_end = end;
            return end + 1;
        }
        case CMD_GLU_SPHERE:
        case CMD_GLU_CYLINDER:
        case CMD_GLU_DISK:
        case CMD_GLU_PARTIAL_DISK:
        case CMD_GLUT_TORUS:
            *fade_begin = start;
            *fade_end = i;
            return i + 1;
        case CMD_TESS_BEGIN_POLYGON: {
            int tess_depth = 1;
            for (int j = i + 1; j < g_num_flat_cmds; j++) {
                if (!g_flat_cmds[j].valid) continue;
                if (g_flat_cmds[j].type == CMD_TESS_BEGIN_POLYGON) {
                    tess_depth = 1;
                } else if (g_flat_cmds[j].type == CMD_TESS_BEGIN_CONTOUR) {
                    if (tess_depth == 1)
                        tess_depth = 2;
                } else if (g_flat_cmds[j].type == CMD_TESS_END) {
                    if (tess_depth == 2) {
                        tess_depth = 1;
                    } else if (tess_depth == 1) {
                        *fade_begin = start;
                        *fade_end = j;
                        return j + 1;
                    }
                }
            }
            *fade_begin = start;
            *fade_end = g_num_flat_cmds > 0 ? g_num_flat_cmds - 1 : i;
            return g_num_flat_cmds;
        }
        default:
            break;
        }
    }

    if (saw_meaningful)
        return g_num_flat_cmds;
    return start;
}

static int replay_next_vertex_limit(int start, int *fade_begin, int *fade_end) {
    int open_begin = replay_find_open_begin_before(start);
    int tess_depth = 0;
    int open_tess_poly = replay_find_open_tess_polygon_before(start, &tess_depth);
    int saw_meaningful = 0;

    *fade_begin = -1;
    *fade_end = -1;

    for (int i = start; i < g_num_flat_cmds; i++) {
        CmdType t;

        if (!g_flat_cmds[i].valid || g_flat_cmds[i].type == CMD_COMMENT)
            continue;

        t = g_flat_cmds[i].type;
        saw_meaningful = 1;

        switch (t) {
        case CMD_BEGIN:
            open_begin = i;
            break;
        case CMD_END:
            open_begin = -1;
            break;
        case CMD_TESS_BEGIN_POLYGON:
            open_tess_poly = i;
            tess_depth = 1;
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            if (tess_depth == 1)
                tess_depth = 2;
            break;
        case CMD_TESS_END:
            if (tess_depth == 2) {
                tess_depth = 1;
            } else if (tess_depth == 1) {
                *fade_begin = (open_tess_poly >= 0) ? open_tess_poly : start;
                *fade_end = i;
                return i + 1;
            }
            break;
        case CMD_VERTEX3F:
        case CMD_VERTEX2F:
            *fade_begin = (open_begin >= 0) ? open_begin : start;
            *fade_end = i;
            return i + 1;
        case CMD_TESS_VERTEX:
            return i + 1;
        case CMD_GLU_SPHERE:
        case CMD_GLU_CYLINDER:
        case CMD_GLU_DISK:
        case CMD_GLU_PARTIAL_DISK:
        case CMD_GLUT_TORUS:
            *fade_begin = start;
            *fade_end = i;
            return i + 1;
        default:
            break;
        }
    }

    if (saw_meaningful)
        return g_num_flat_cmds;
    return start;
}

static int replay_prev_limit(int current_pc) {
    int pc = 0;
    int prev_pc = 0;

    if (current_pc <= 0)
        return 0;

    while (pc < current_pc) {
        int fade_begin = -1;
        int fade_end = -1;
        int next_pc = (g_replay_mode == REPLAY_MODE_POLYGON)
                    ? replay_next_polygon_limit(pc, &fade_begin, &fade_end)
                    : replay_next_vertex_limit(pc, &fade_begin, &fade_end);

        // used next_pc to make sure we didnt somehow go backwards
        if (next_pc <= pc) /* Shouldn't happen */
            break;

        prev_pc = pc;
        pc = next_pc;
    }

    return prev_pc;
}

void replay_seek(int new_pc) {
    if (new_pc < 0)
        new_pc = 0;
    if (new_pc > g_num_flat_cmds)
        new_pc = g_num_flat_cmds;

    g_replay_pc = new_pc;
    g_replay_accum = 0.0f;
    replay_clear_fade_batches();
    replay_set_src_line(replay_last_meaningful_src(0, new_pc));
    g_replay_state = (new_pc >= g_num_flat_cmds && g_num_flat_cmds > 0)
                   ? REPLAY_DONE
                   : REPLAY_PAUSED;
}

int replay_seek_to_src_line(int target_line) {
    int pc = 0;
    int landed_pc = -1;
    int landed_src = -1;

    if (g_flat_dirty) {
        float live_predef_vals[MAX_PREDEF_VARS];
        copy_predef_values(live_predef_vals);
        flatten_commands();
        g_flat_dirty = 0;
        restore_predef_values(live_predef_vals);
    }

    while (pc < g_num_flat_cmds) {
        int fade_begin = -1;
        int fade_end = -1;
        int next_pc = (g_replay_mode == REPLAY_MODE_POLYGON)
                    ? replay_next_polygon_limit(pc, &fade_begin, &fade_end)
                    : replay_next_vertex_limit(pc, &fade_begin, &fade_end);

        if (next_pc <= pc)
            break;

        int step_src = replay_last_meaningful_src(pc, next_pc);
        if (step_src >= target_line) {
            landed_pc = next_pc;
            landed_src = step_src;
            break;
        }
        pc = next_pc;
    }

    if (landed_pc < 0)
        return -1;

    replay_seek(landed_pc);
    return landed_src;
}

void replay_restart_from_beginning(void) {
    g_replay_pc = 0;
    g_replay_accum = 0.0f;
    replay_clear_fade_batches();
    g_replay_state = REPLAY_PLAYING;
    g_replay_src_line = -1;
    g_replay_last_src_line = -1;
}

void replay_start(void) {
    float live_predef_vals[MAX_PREDEF_VARS];

    copy_predef_values(live_predef_vals);
    if (g_flat_dirty) {
        flatten_commands();
        g_flat_dirty = 0;
        restore_predef_values(live_predef_vals);
    }

    if (!replay_has_meaningful_cmds()) {
        set_status("Replay: nothing to play");
        return;
    }

    copy_predef_values(g_replay_baseline_predef_vals);
    g_replay_saved_t_playing = g_t_playing;
    g_t_playing = 0;

    g_replay_active = 1;
    g_replay_state = REPLAY_PLAYING;
    g_replay_pc = 0;
    g_replay_accum = 0.0f;
    replay_clear_fade_batches();
    g_replay_src_line = -1;
    g_replay_total_flat = g_num_flat_cmds;
    g_replay_last_src_line = -1;
    set_status("Replay: playing");
}

void replay_stop(void) {
    g_t_playing = g_replay_saved_t_playing;
    g_replay_active = 0;
    g_replay_state = REPLAY_OFF;
    g_replay_pc = 0;
    g_replay_accum = 0.0f;
    replay_clear_fade_batches();
    g_replay_src_line = -1;
    g_replay_total_flat = 0;
    g_replay_last_src_line = -1;
}

void replay_advance(void) {
    int old_pc;
    int next_pc;
    int src_line = -1;

    if (!replay_enabled())
        return;

    if (g_replay_pc >= g_num_flat_cmds) {
        g_replay_state = REPLAY_DONE;
        return;
    }

    old_pc = g_replay_pc;
    next_pc = (g_replay_mode == REPLAY_MODE_POLYGON)
            ? replay_next_polygon_limit(old_pc, &(int){ -1 }, &(int){ -1 })
            : replay_next_vertex_limit(old_pc, &(int){ -1 }, &(int){ -1 });

    if (next_pc <= old_pc)
        next_pc = g_num_flat_cmds;
    if (next_pc > g_num_flat_cmds)
        next_pc = g_num_flat_cmds;

    g_replay_pc = next_pc;
    replay_push_fade_batch(old_pc, next_pc);

    src_line = replay_last_meaningful_src(old_pc, next_pc);
    replay_set_src_line(src_line);

    if (g_replay_pc >= g_num_flat_cmds) {
        g_replay_state = REPLAY_DONE;
        set_status("Replay: done");
    }
}

void replay_step_back(void) {
    if (!g_replay_active)
        return;

    if (g_replay_pc <= 0) {
        replay_seek(0);
        set_status("Replay: at start");
        return;
    }

    replay_seek(replay_prev_limit(g_replay_pc));
}

int replay_exec_limit(void) {
    if (replay_enabled())
        return g_replay_pc;
    return g_num_flat_cmds;
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
        switch (g_flat_cmds[pc].type) {
        case CMD_BEGIN:
            if (in_begin) { glEnd(); in_begin = 0; }
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
                    ExprCtx ctx = { cond_text, eval_vars, eval_num_vars };
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
            int vi = g_flat_cmds[pc].num_args;
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
                    ExprCtx ctx = { rhs, eval_vars, eval_num_vars };
                    value = eval_expr(&ctx);
                }
            }
            if (vi >= 0 && vi < g_num_predef_vars)
                g_predef_vars[vi].value = value;
            break;
        }
        /* Transforms handled by is_transform_cmd() early-continue above */
        case CMD_TRANSLATE3F: case CMD_SCALEF: case CMD_ROTATEF:
        case CMD_PUSH_MATRIX: case CMD_POP_MATRIX:
        /* These are resolved during flatten and shouldn't appear in flat_cmds */
        case CMD_FOR_BEGIN: case CMD_FOR_END:
        case CMD_FUNC_DEF: case CMD_FUNC_END: case CMD_CALL:
        case CMD_COMMENT:
        case CMD_TYPE_COUNT:
            break;
        }
        pc++;
    }
execute_done:;
    if (in_begin) glEnd();
    if (!(replay_enabled() && g_replay_mode == REPLAY_MODE_VERTEX)) {
        if (tess_depth == 2 && g_tess) { gluTessEndContour(g_tess); tess_depth = 1; }
        if (tess_depth == 1 && g_tess) { gluTessEndPolygon(g_tess); }
    }
    unwind_tracked_transform_stack(&matrix_depth);
}

void execute_replay_fade_batches(void) {
    int saved_flat_count = g_num_flat_cmds;

    if (!replay_has_active_fades())
        return;

    for (int i = 0; i < g_replay_fade_batch_count; i++) {
        float alpha = replay_batch_alpha(&g_replay_fade_batches[i]);

        if (alpha <= 0.0f)
            continue;

        restore_predef_values(g_replay_baseline_predef_vals);
        g_num_flat_cmds = g_replay_fade_batches[i].new_pc;
        g_execute_alpha_scale = alpha;

        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glPushMatrix();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0.70f, 0.70f, 0.80f, alpha);
        execute_commands();
        glPopMatrix();
        glPopAttrib();
    }

    g_execute_alpha_scale = 1.0f;
    g_num_flat_cmds = saved_flat_count;
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
    float live_predef_vals[MAX_PREDEF_VARS];

    if (g_normals_dirty) {
        recompute_autonormals();
        g_normals_dirty = 0;
    }
    if (g_flat_dirty) {
        flatten_commands();
        g_flat_dirty = 0;
    }

    saved_flat_count = g_num_flat_cmds;
    copy_predef_values(live_predef_vals);
    if (replay_enabled()) {
        g_replay_total_flat = saved_flat_count;
        if (g_replay_pc > g_num_flat_cmds)
            g_replay_pc = g_num_flat_cmds;
        if (g_replay_pc >= g_num_flat_cmds && g_num_flat_cmds > 0 &&
            g_replay_state == REPLAY_PLAYING)
            g_replay_state = REPLAY_DONE;
        replay_clamp_fade_batches(g_replay_pc);
        g_num_flat_cmds = replay_exec_limit();
    }

    update_render_state_strings();
    update_lookat_strings();

    /* Full-window clear */
    glViewport(0, 0, g_win_w, g_win_h);
    glClearColor(0.10f, 0.10f, 0.13f, 1.0f);

    /* 3D scene — with optional accumulation-buffer jitter AA */
    if (g_use_accum && g_accum_aa_enabled && g_accum_samples > 1) {
        /* Clear the accumulation buffer once per frame */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ACCUM_BUFFER_BIT);
        float weight = 1.0f / (float)g_accum_samples;
        for (int j = 0; j < g_accum_samples; j++) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (replay_enabled())
                restore_predef_values(g_replay_baseline_predef_vals);
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
        if (replay_enabled())
            restore_predef_values(g_replay_baseline_predef_vals);
        render_3d_scene();
    }

    /* 2D overlays in full window coords */
    glViewport(0, 0, g_win_w, g_win_h);
    render_code_panel();
    render_autocomplete();
    render_example_dropdown();
    render_var_panel();
    render_config_menu();
    render_help();

    g_num_flat_cmds = saved_flat_count;
    restore_predef_values(live_predef_vals);

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
                strncpy(frames[depth].vars[0].name, vn,
                        sizeof(frames[depth].vars[0].name) - 1);
                frames[depth].vars[0].name[sizeof(frames[depth].vars[0].name) - 1] = '\0';
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
                        strncpy(frames[depth].vars[p].name, param_names[p],
                                sizeof(frames[depth].vars[p].name) - 1);
                        frames[depth].vars[p].name[sizeof(frames[depth].vars[p].name) - 1] = '\0';
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

/* Load an example from an array of source lines */
static void load_example_lines(const char *const *lines) {
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

    for (int i = 0; lines[i]; i++)
        feed_line(lines[i]);

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
    return parse_command(line, cmd);
}

int repl_parse_command_with_vars(const char *line, GLCmd *cmd,
                                 ExprVar *vars, int num_vars) {
    return parse_command_with_vars(line, cmd, vars, num_vars);
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
    g_multisample_enabled = 1;
    g_line_smooth_enabled = 0;
    g_init_attenuate_points = 1;
    g_wrap_at_comma = 1;
    g_layout_vertical = 0;
    g_panel_frac = 0.42f;
    g_flat_dirty = 1;
    g_normals_dirty = 1;
    clear_autocomplete_state();
    search_clear_all();
    update_render_state_strings();
    depth_cache_invalidate();
    clear_selection();
}
