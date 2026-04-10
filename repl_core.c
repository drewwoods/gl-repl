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

const char *g_func_completions[] = {
    "glVertex3f(",
    "glVertex2f(",
    "glNormal3f(",
    "glColor3f(",
    "glColor4f(",
    "glBegin(",
    "glEnd()",
    "glEnable(",
    "glDisable(",
    "glShadeModel(",
    "glPointSize(",
    "glPointParameterfv(",
    "glBlendFunc(",
    "glTranslatef(",
    "glScalef(",
    "glRotatef(",
    "glPushMatrix()",
    "glPopMatrix()",
    "glColorMaterial(",
    "glLightModeli(",
    "glFrontFace(",
    "glMaterialf(",
    "gluSphere(",
    "gluCylinder(",
    "gluDisk(",
    "gluPartialDisk(",
    "glutSolidTorus(",
    "gluBegin(GLU_POLYGON)",
    "gluBegin(GLU_CONTOUR)",
    "gluEnd()",
    "gluNormal(",
    "gluColor(",
    "gluVertex(",
    "for(",
    "if(",
    "goto ",
    "func0 {",
    "func0(radius, yoff) {",
    "func1 {",
    "func2 {",
    "func3 {",
    "func0()",
    "func1()",
    "func2()",
    "func3()",
    "x = ",
    "y = ",
    "z = ",
    NULL
};

const char *g_header_pre[] = {
    "#include <gl_includes.h>",
    "#include <math.h>",
    "",
    "#ifndef M_PI",
    "#define M_PI 3.14159265358979323846",
    "#endif",
    "",
    "static float g_angle = 0.0f;",
    "static int   g_rotating = 0;",
    "static GLUquadric *g_quadric = NULL;",
    "",
    "void display() {",
    "  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "  glLoadIdentity();",
    "  glPushAttrib(GL_ALL_ATTRIB_BITS);",
    NULL
};

char g_render_state_lines[RENDER_STATE_LINE_COUNT][64] = {
    "  glEnable(GL_MULTISAMPLE);",
    "  glDisable(GL_LINE_SMOOTH);"
};

char g_lookat[LOOKAT_LINE_COUNT][128] = {
    "  gluLookAt(0.00, 0.00, 5.00,",
    "            0.00, 0.00, 0.00,",
    "            0.00, 1.00, 0.00);"
};

const char *g_header_post[] = {
    "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);",
    NULL
};

const char *g_footer[] = {
    "",
    "  glPopAttrib();",
    "  glutSwapBuffers();",
    "}",
    "",
    "void reshape(int w, int h) {",
    "  glViewport(0, 0, w, h);",
    "  glMatrixMode(GL_PROJECTION);",
    "  glLoadIdentity();",
    "  gluPerspective(45.0, (float)w/(float)h, 0.1, 100.0);",
    "  glMatrixMode(GL_MODELVIEW);",
    "}",
    "",
    "void keyboard(unsigned char key, int x, int y) {",
    "  (void)x; (void)y;",
    "  if (key == ' ') g_rotating = !g_rotating;",
    "  if (key == 27) exit(0);",
    "}",
    "",
    "void idle() {",
    "  if (g_rotating) g_angle += 0.5f;",
    "  glutPostRedisplay();",
    "}",
    "",
    "void init() {",
    "  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);",
    "  glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);",
    "  g_quadric = gluNewQuadric();",
    "  gluQuadricNormals(g_quadric, GLU_SMOOTH);",
    "  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, (float[]){ 1.0f, 0.0f, 0.02f });",
    "}",
    "",
    "int main(int argc, char **argv) {",
    "  glutInit(&argc, argv);",
    "  glutInitDisplayMode(GLUT_DOUBLE|GLUT_RGB|GLUT_DEPTH|GLUT_MULTISAMPLE);",
    "  glutInitWindowSize(800, 600);",
    "  glutCreateWindow(\"OpenGL REPL\");",
    "  init();",
    "  glutDisplayFunc(display);",
    "  glutReshapeFunc(reshape);",
    "  glutKeyboardFunc(keyboard);",
    "  glutIdleFunc(idle);",
    "  glutMainLoop();",
    "  return 0;",
    "}",
    NULL
};

static const char *outfile = "output.c";
static const char *tempfile = "/tmp/temp-output.c";

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

static void depth_cache_invalidate(void) {
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

/* ========================================================================= */
/* Undo / redo ring buffers                                                   */
/* ========================================================================= */

static void load_line_to_input(int idx);  /* defined later */

#define UNDO_DEPTH 32

typedef struct {
    GLCmd cmds[MAX_COMMANDS];
    int   num_cmds;
    int   edit_line;
    float predef_vals[MAX_PREDEF_VARS];
} UndoSnapshot;

static UndoSnapshot g_undo_buf[UNDO_DEPTH];
static int g_undo_head  = 0;  /* next slot to write into */
static int g_undo_count = 0;  /* number of valid undo snapshots */

static UndoSnapshot g_redo_buf[UNDO_DEPTH];
static int g_redo_head  = 0;  /* next slot to write into */
static int g_redo_count = 0;  /* number of valid redo snapshots */

static void snapshot_save(UndoSnapshot *s) {
    memcpy(s->cmds, g_cmds, (size_t)g_num_cmds * sizeof(GLCmd));
    s->num_cmds  = g_num_cmds;
    s->edit_line = g_edit_line;
    for (int i = 0; i < g_num_predef_vars; i++)
        s->predef_vals[i] = g_predef_vars[i].value;
}

static void snapshot_restore(const UndoSnapshot *s) {
    memcpy(g_cmds, s->cmds, (size_t)s->num_cmds * sizeof(GLCmd));
    g_num_cmds  = s->num_cmds;
    g_edit_line = s->edit_line;
    for (int i = 0; i < g_num_predef_vars; i++)
        g_predef_vars[i].value = s->predef_vals[i];
    g_inserting = 0;
    load_line_to_input(g_edit_line);
    mark_normals_dirty();
}

static void push_undo_snapshot(void) {
    snapshot_save(&g_undo_buf[g_undo_head]);
    g_undo_head = (g_undo_head + 1) % UNDO_DEPTH;
    if (g_undo_count < UNDO_DEPTH) g_undo_count++;
    /* new change invalidates the redo stack */
    g_redo_count = 0;
    g_redo_head  = 0;
}

static void pop_undo_snapshot(void) {
    if (g_undo_count == 0) { set_status("Nothing to undo"); return; }
    /* save current state so it can be redone */
    snapshot_save(&g_redo_buf[g_redo_head]);
    g_redo_head = (g_redo_head + 1) % UNDO_DEPTH;
    if (g_redo_count < UNDO_DEPTH) g_redo_count++;
    /* restore previous state */
    g_undo_head = (g_undo_head + UNDO_DEPTH - 1) % UNDO_DEPTH;
    g_undo_count--;
    snapshot_restore(&g_undo_buf[g_undo_head]);
    char msg[64];
    snprintf(msg, sizeof(msg), "Undo (%d more)", g_undo_count);
    set_status(msg);
}

static void do_redo(void) {
    if (g_redo_count == 0) { set_status("Nothing to redo"); return; }
    /* save current state so it can be undone */
    snapshot_save(&g_undo_buf[g_undo_head]);
    g_undo_head = (g_undo_head + 1) % UNDO_DEPTH;
    if (g_undo_count < UNDO_DEPTH) g_undo_count++;
    /* restore next redo state */
    g_redo_head = (g_redo_head + UNDO_DEPTH - 1) % UNDO_DEPTH;
    g_redo_count--;
    snapshot_restore(&g_redo_buf[g_redo_head]);
    char msg[64];
    snprintf(msg, sizeof(msg), "Redo (%d more)", g_redo_count);
    set_status(msg);
}

/* Predefined variables — defined in repl_eval.c */

/* Editor */
char   g_input[MAX_INPUT_LEN];
int    g_input_len = 0;
int    g_cursor_pos = 0;     /* cursor position within g_input (0..g_input_len) */
int    g_edit_line = 0;      /* 0..g_num_cmds; g_num_cmds = new line */
char   g_newline_buf[MAX_INPUT_LEN] = "";
int    g_newline_len = 0;
int    g_inserting = 0;      /* insert mode: virtual new line at g_edit_line */

/* (no display list - commands are executed directly each frame) */

/* Camera */
float  g_cam_rx = 20.0f;
float  g_cam_ry = 30.0f;
float  g_cam_dist = 5.0f;
float  g_cam_px = 0.0f, g_cam_py = 0.0f;
int    g_mouse_x, g_mouse_y;
int    g_mouse_btn = -1;

/* Camera momentum — motion_func feeds these; timer_func integrates + decays */
static float  g_vel_ry   = 0.0f;   /* orbit yaw   (deg/frame)   */
static float  g_vel_rx   = 0.0f;   /* orbit pitch (deg/frame)   */
static float  g_vel_px   = 0.0f;   /* pan X       (units/frame) */
static float  g_vel_py   = 0.0f;   /* pan Y       (units/frame) */
static float  g_vel_zoom = 0.0f;   /* zoom        (units/frame) */
#define CAM_DECAY 0.88f
#define CAM_DECAY_ZOOM 0.65f
#define CAM_MOMENTUM_THRESHOLD 1.0f

/* Window */
int    g_win_w = 1200, g_win_h = 800;

/* Code panel */
float  g_panel_frac = 0.42f;
int    g_resizing_panel = 0;
int    g_scroll = 0;
int    g_scroll_follow_cursor = 0;

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
static const int g_accum_steps[]     = { 1, 2, 4, 8, 16 };

/* Cursor blink */
int    g_cursor_on = 1;
int    g_blink_tick = 0;

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
int    g_wrap_at_comma = 0;
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

/* Variable slider panel (4.8) */
int    g_show_var_panel  = 1;   /* show predefined-variable sliders */
int    g_drag_var        = -1;  /* index of var being dragged, -1=none */
float  g_drag_start_val  = 0.0f;
int    g_drag_start_x    = 0;

/* Configuration menu (4.10) */
int    g_show_config     = 0;   /* show config overlay */
int    g_config_hover    = -1;  /* hovered row (-1=none) */

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

/* Search */
int    g_search_active = 0;
char   g_search_query[MAX_INPUT_LEN] = "";
int    g_search_query_len = 0;
int    g_search_cursor_pos = 0;
int    g_search_hit_line = -1;
int    g_search_hit_char = -1;
int    g_search_hit_ordinal = 0;
int    g_search_match_count = 0;

/* Autocomplete */
const char *g_ac_matches[MAX_AC_MATCHES];
int    g_ac_count = 0;
int    g_ac_sel = 0;
char   g_ac_ghost[MAX_LINE_LEN] = "";
int    g_cursor_px = 0;     /* screen pos of cursor, set during render */
int    g_cursor_py = 0;

/* Clipboard */
GLCmd  g_clipboard[MAX_COMMANDS];
int    g_clipboard_count = 0;

/* Selection (shift+arrow) */
int    g_sel_anchor = -1;   /* -1 = no selection */
int    g_sel_end = -1;

void clear_selection(void) { g_sel_anchor = g_sel_end = -1; }
int sel_active(void) { return g_sel_anchor >= 0 && g_sel_end >= 0; }
int sel_lo(void) {
    return g_sel_anchor < g_sel_end ? g_sel_anchor : g_sel_end;
}
int sel_hi(void) {
    return g_sel_anchor > g_sel_end ? g_sel_anchor : g_sel_end;
}

/* Forward declarations (eval_expr, parse_for_header, etc. are in repl_eval.h) */
static int parse_command(const char *line, GLCmd *cmd);
static int parse_command_with_vars(const char *line, GLCmd *cmd,
                                   ExprVar *vars, int num_vars);
static int collect_visible_vars(int pos, ExprVar *vars, int max_vars);
static int parse_expr_list_exact(const char *src, float *out_vals, int max_vals,
                                 ExprVar *vars, int num_vars, int *out_count);
static int parse_repl_func_signature(const char *src, int *fn,
                                     char param_names[][16], int max_params,
                                     int *param_count);
static int extract_func_call_args_text(const char *src, int *fn,
                                       char *args, int args_sz);
static void format_func_header(char *out, int out_sz, const char *indent,
                               int fn, char param_names[][16], int param_count);
static unsigned int line_func_scope_mask(int line);
static int block_depth_at(int pos);
static void get_for_var_name(const GLCmd *cmd, char *var, int var_sz);
static int try_commit_for_loop(void);
static int try_commit_func_def(void);
static int try_commit_if_block(void);
static int try_commit_close_brace(void);
static void load_example(int idx);
static int cmd_type_is_quadric(CmdType t);
static int import_make_repl_quadric_line(const char *line, char *out, int out_sz);
static void load_line_to_input(int idx);
static int feed_line(const char *line);
static void         trim_in_place(char *s);
static int search_row_is_live_input(int row_idx);
static int search_row_to_nav_line(int row_idx);
static int search_hit_exists(int row_idx, int char_pos);
static int search_row_occurrence_index(int row_idx, int char_pos);
static int search_char_for_row_occurrence(int row_idx, int occurrence_idx);
static int search_ordinal_for_hit(int row_idx, int char_pos);
static void search_store_hit(int row_idx, int char_pos);
static void search_clear_matches(void);
static void search_clear_all(void);
static int search_find_forward(int start_row, int start_char,
                               int *out_row, int *out_char);
static int search_find_backward(int start_row, int start_char,
                                int *out_row, int *out_char);
static void search_refresh_query(void);
static void search_navigate(int direction);
static void search_open(void);
static int handle_search_key(unsigned char key);
static int handle_search_special(int key);

/* ========================================================================= */
/* Configuration menu item table (4.10)                                       */
/* ========================================================================= */

static const char *replay_mode_names[] = { "Polygon", "Vertex" };

CfgItem g_cfg_items[] = {
    { "Wireframe",        "F2",     &g_wireframe,              2,               NULL          },
    { "Grid",             "F3",     &g_grid_theme,             GRID_THEME_COUNT, g_grid_names },
    { "Axes",             "F4",     &g_axes_theme,             AXES_THEME_COUNT, g_axes_names },
    { "Vertex labels",    "F5",     &g_show_vnums,             2,               NULL          },
    { "Normal vectors",   "F6",     &g_show_normals,           2,               NULL          },
    { "Vertex outlines",  "F7",     &g_show_outlines,          2,               NULL          },
    { "Vertex points",    "--",     &g_show_vpoints,           2,               NULL          },
    { "Wrap at commas",   "--",     &g_wrap_at_comma,          2,               NULL          },
    { "Vertex guides",    "F8",     &g_show_guides,            2,               NULL          },
    { "Auto-normals",     "F9",     &g_autonormal,             2,               NULL          },
    { "Light indicators", "F10",    &g_show_lights,            2,               NULL          },
    { "Camera rotate",    "F11",    &g_cam_rotate,             2,               NULL          },
    { "Auto time",        "Ctrl+t", &g_t_playing,              2,               NULL          },
    { "MSAA",             "Ctrl+u", &g_multisample_enabled,    2,               NULL          },
    { "Line smooth",      "Ctrl+n", &g_line_smooth_enabled,    2,               NULL          },
    { "Accum AA",         "Ctrl+b", &g_accum_aa_enabled,       2,               NULL          },
    { "Poly highlight",   "--",     &g_highlight_current_poly, 2,               NULL          },
    { "Variable panel",   "`",      &g_show_var_panel,         2,               NULL          },
    { "Replay",           "Ctrl+g", &g_replay_active,          2,               NULL          },
    { "Replay mode",      "m",      &g_replay_mode,            2,               replay_mode_names },
    { "Top code panel",   "--",     &g_layout_vertical,        2,               NULL          },
};
const int CFG_ITEM_COUNT = (int)(sizeof(g_cfg_items)/sizeof(g_cfg_items[0]));

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

int repl_search_row_count(void) {
    if (g_inserting || g_edit_line == g_num_cmds)
        return g_num_cmds + 1;
    return g_num_cmds;
}

static int search_row_is_live_input(int row_idx) {
    if (row_idx < 0 || row_idx >= repl_search_row_count())
        return 0;
    return row_idx == g_edit_line;
}

const char *repl_search_row_text(int row_idx) {
    if (row_idx < 0 || row_idx >= repl_search_row_count())
        return "";
    if (search_row_is_live_input(row_idx))
        return g_input;
    if (g_inserting && row_idx > g_edit_line)
        row_idx--;
    if (row_idx >= 0 && row_idx < g_num_cmds)
        return g_cmds[row_idx].source;
    return "";
}

int repl_search_row_for_cmd_index(int cmd_idx) {
    if (cmd_idx < 0 || cmd_idx >= g_num_cmds)
        return -1;
    return (g_inserting && cmd_idx >= g_edit_line) ? cmd_idx + 1 : cmd_idx;
}

static int search_row_to_nav_line(int row_idx) {
    if (row_idx < 0 || row_idx >= repl_search_row_count())
        return -1;
    if (search_row_is_live_input(row_idx)) {
        if (g_inserting)
            return -1;
        return g_edit_line;
    }
    if (g_inserting && row_idx > g_edit_line)
        return row_idx - 1;
    return row_idx;
}

static int search_text_matches_at(const char *text, const char *query, int pos) {
    int text_len;
    int query_len;

    if (!text || !query)
        return 0;

    text_len = (int)strlen(text);
    query_len = (int)strlen(query);
    if (query_len <= 0 || pos < 0 || pos + query_len > text_len)
        return 0;

    for (int i = 0; i < query_len; i++) {
        unsigned char tc = (unsigned char)text[pos + i];
        unsigned char qc = (unsigned char)query[i];
        if (tolower(tc) != tolower(qc))
            return 0;
    }
    return 1;
}

int repl_search_find_next_in_text(const char *text, const char *query,
                                  int start_pos) {
    int text_len;
    int query_len;

    if (!text || !query)
        return -1;

    text_len = (int)strlen(text);
    query_len = (int)strlen(query);
    if (query_len <= 0 || text_len < query_len)
        return -1;
    if (start_pos < 0)
        start_pos = 0;

    for (int pos = start_pos; pos + query_len <= text_len; pos++) {
        if (search_text_matches_at(text, query, pos))
            return pos;
    }
    return -1;
}

int repl_search_find_prev_in_text(const char *text, const char *query,
                                  int start_pos) {
    int text_len;
    int query_len;
    int max_start;

    if (!text || !query)
        return -1;

    text_len = (int)strlen(text);
    query_len = (int)strlen(query);
    if (query_len <= 0 || text_len < query_len)
        return -1;

    max_start = text_len - query_len;
    if (start_pos > max_start)
        start_pos = max_start;

    for (int pos = start_pos; pos >= 0; pos--) {
        if (search_text_matches_at(text, query, pos))
            return pos;
    }
    return -1;
}

static int search_total_matches(void) {
    int total = 0;

    if (g_search_query_len <= 0)
        return 0;

    for (int row = 0; row < repl_search_row_count(); row++) {
        const char *text = repl_search_row_text(row);
        int pos = repl_search_find_next_in_text(text, g_search_query, 0);
        while (pos >= 0) {
            total++;
            pos = repl_search_find_next_in_text(text, g_search_query, pos + 1);
        }
    }

    return total;
}

static int search_hit_exists(int row_idx, int char_pos) {
    const char *text;

    if (g_search_query_len <= 0)
        return 0;
    if (row_idx < 0 || row_idx >= repl_search_row_count() || char_pos < 0)
        return 0;

    text = repl_search_row_text(row_idx);
    return search_text_matches_at(text, g_search_query, char_pos);
}

static int search_row_occurrence_index(int row_idx, int char_pos) {
    const char *text;
    int occurrence = 0;

    if (!search_hit_exists(row_idx, char_pos))
        return -1;

    text = repl_search_row_text(row_idx);
    for (int pos = repl_search_find_next_in_text(text, g_search_query, 0);
         pos >= 0;
         pos = repl_search_find_next_in_text(text, g_search_query, pos + 1)) {
        if (pos == char_pos)
            return occurrence;
        occurrence++;
    }

    return -1;
}

static int search_char_for_row_occurrence(int row_idx, int occurrence_idx) {
    const char *text;
    int occurrence = 0;

    if (g_search_query_len <= 0 || row_idx < 0 || row_idx >= repl_search_row_count() ||
        occurrence_idx < 0)
        return -1;

    text = repl_search_row_text(row_idx);
    for (int pos = repl_search_find_next_in_text(text, g_search_query, 0);
         pos >= 0;
         pos = repl_search_find_next_in_text(text, g_search_query, pos + 1)) {
        if (occurrence == occurrence_idx)
            return pos;
        occurrence++;
    }

    return -1;
}

static int search_ordinal_for_hit(int row_idx, int char_pos) {
    int ordinal = 0;

    if (!search_hit_exists(row_idx, char_pos))
        return 0;

    for (int row = 0; row < repl_search_row_count(); row++) {
        const char *text = repl_search_row_text(row);
        int pos = repl_search_find_next_in_text(text, g_search_query, 0);
        while (pos >= 0) {
            ordinal++;
            if (row == row_idx && pos == char_pos)
                return ordinal;
            pos = repl_search_find_next_in_text(text, g_search_query, pos + 1);
        }
    }

    return 0;
}

static void search_store_hit(int row_idx, int char_pos) {
    if (!search_hit_exists(row_idx, char_pos)) {
        search_clear_matches();
        return;
    }

    g_search_match_count = search_total_matches();
    g_search_hit_line = row_idx;
    g_search_hit_char = char_pos;
    g_search_hit_ordinal = search_ordinal_for_hit(row_idx, char_pos);
}

static void search_clear_matches(void) {
    g_search_hit_line = -1;
    g_search_hit_char = -1;
    g_search_hit_ordinal = 0;
    g_search_match_count = 0;
}

static void search_clear_all(void) {
    g_search_active = 0;
    g_search_query[0] = '\0';
    g_search_query_len = 0;
    g_search_cursor_pos = 0;
    search_clear_matches();
}

static int search_find_forward(int start_row, int start_char,
                               int *out_row, int *out_char) {
    int row_count = repl_search_row_count();

    if (g_search_query_len <= 0 || row_count <= 0)
        return 0;

    if (start_row < 0)
        start_row = 0;
    if (start_row >= row_count)
        start_row = row_count - 1;
    if (start_char < 0)
        start_char = 0;

    for (int pass = 0; pass < 2; pass++) {
        for (int row = start_row; row < row_count; row++) {
            int pos = repl_search_find_next_in_text(
                repl_search_row_text(row), g_search_query,
                row == start_row ? start_char : 0);
            if (pos >= 0) {
                if (out_row) *out_row = row;
                if (out_char) *out_char = pos;
                return 1;
            }
        }
        start_row = 0;
        start_char = 0;
    }

    return 0;
}

static int search_find_backward(int start_row, int start_char,
                                int *out_row, int *out_char) {
    int row_count = repl_search_row_count();

    if (g_search_query_len <= 0 || row_count <= 0)
        return 0;

    if (start_row < 0)
        start_row = 0;
    if (start_row >= row_count)
        start_row = row_count - 1;

    for (int pass = 0; pass < 2; pass++) {
        for (int row = start_row; row >= 0; row--) {
            const char *text = repl_search_row_text(row);
            int max_start = (int)strlen(text) - g_search_query_len;
            int pos;

            if (max_start < 0)
                continue;

            pos = (row == start_row) ? start_char : max_start;
            if (pos > max_start)
                pos = max_start;

            pos = repl_search_find_prev_in_text(text, g_search_query, pos);
            if (pos >= 0) {
                if (out_row) *out_row = row;
                if (out_char) *out_char = pos;
                return 1;
            }
        }
        start_row = row_count - 1;
        start_char = MAX_INPUT_LEN;
    }

    return 0;
}

static void search_refresh_query(void) {
    int row;
    int char_pos;
    int row_occurrence;
    int nav_line;

    if (!g_search_active)
        return;

    g_search_query_len = (int)strlen(g_search_query);
    if (g_search_cursor_pos > g_search_query_len)
        g_search_cursor_pos = g_search_query_len;
    if (g_search_query_len <= 0) {
        search_clear_matches();
        return;
    }

    if (!search_find_forward(g_edit_line, 0, &row, &char_pos)) {
        search_clear_matches();
        return;
    }

    row_occurrence = search_row_occurrence_index(row, char_pos);
    nav_line = search_row_to_nav_line(row);
    if (nav_line >= 0) {
        g_scroll_follow_cursor = 1;
        navigate_to_line(nav_line);
        row = g_edit_line;
        if (row_occurrence >= 0) {
            int remapped_char = search_char_for_row_occurrence(row, row_occurrence);
            if (remapped_char >= 0)
                char_pos = remapped_char;
        }
    }
    search_store_hit(row, char_pos);
}

static void search_navigate(int direction) {
    int row;
    int char_pos;
    int row_occurrence;
    int found;
    int nav_line;

    if (!g_search_active || g_search_query_len <= 0)
        return;

    if (direction < 0) {
        if (g_search_hit_line >= 0 && g_search_hit_char >= 0)
            found = search_find_backward(g_search_hit_line,
                                         g_search_hit_char - 1,
                                         &row, &char_pos);
        else
            found = search_find_backward(g_edit_line, MAX_INPUT_LEN,
                                         &row, &char_pos);
    } else {
        if (g_search_hit_line >= 0 && g_search_hit_char >= 0)
            found = search_find_forward(g_search_hit_line,
                                        g_search_hit_char + 1,
                                        &row, &char_pos);
        else
            found = search_find_forward(g_edit_line, 0, &row, &char_pos);
    }

    if (!found) {
        search_clear_matches();
        return;
    }

    row_occurrence = search_row_occurrence_index(row, char_pos);
    nav_line = search_row_to_nav_line(row);
    if (nav_line >= 0) {
        g_scroll_follow_cursor = 1;
        navigate_to_line(nav_line);
        row = g_edit_line;
        if (row_occurrence >= 0) {
            int remapped_char = search_char_for_row_occurrence(row, row_occurrence);
            if (remapped_char >= 0)
                char_pos = remapped_char;
        }
    }
    search_store_hit(row, char_pos);
}

static void search_open(void) {
    if (g_search_active)
        return;

    g_search_active = 1;
    g_search_cursor_pos = g_search_query_len;
    g_show_help = 0;
    g_help_tab = 0;
    g_help_scroll = 0;
    g_show_config = 0;
    g_ac_count = 0;
    g_ac_ghost[0] = '\0';
}

static int handle_search_key(unsigned char key) {
    if (key == 6) {
        search_open();
        return 1;
    }
    if (!g_search_active)
        return 0;

    if (key == 27) {
        search_clear_all();
        return 1;
    }

    if (key == '\r' || key == '\n') {
        search_navigate(+1);
        return 1;
    }

    if (key == 8 || key == 127) {
        if (g_search_cursor_pos > 0 && g_search_query_len > 0) {
            memmove(&g_search_query[g_search_cursor_pos - 1],
                    &g_search_query[g_search_cursor_pos],
                    (size_t)(g_search_query_len - g_search_cursor_pos + 1));
            g_search_query_len--;
            g_search_cursor_pos--;
            search_refresh_query();
        }
        return 1;
    }

    if (key >= 32 && key < 127 && g_search_query_len < MAX_INPUT_LEN - 2) {
        memmove(&g_search_query[g_search_cursor_pos + 1],
                &g_search_query[g_search_cursor_pos],
                (size_t)(g_search_query_len - g_search_cursor_pos + 1));
        g_search_query[g_search_cursor_pos] = (char)key;
        g_search_query_len++;
        g_search_cursor_pos++;
        search_refresh_query();
        return 1;
    }

    return 1;
}

static int handle_search_special(int key) {
    if (!g_search_active)
        return 0;

    switch (key) {
    case GLUT_KEY_LEFT:
        if (g_search_cursor_pos > 0)
            g_search_cursor_pos--;
        break;
    case GLUT_KEY_RIGHT:
        if (g_search_cursor_pos < g_search_query_len)
            g_search_cursor_pos++;
        break;
    case GLUT_KEY_HOME:
        g_search_cursor_pos = 0;
        break;
    case GLUT_KEY_END:
        g_search_cursor_pos = g_search_query_len;
        break;
    case GLUT_KEY_UP:
        search_navigate(-1);
        break;
    case GLUT_KEY_DOWN:
        search_navigate(+1);
        break;
    default:
        break;
    }

    return 1;
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

/* Update the gluLookAt header lines from current camera orbit params */
static void update_render_state_strings(void) {
    snprintf(g_render_state_lines[0], sizeof(g_render_state_lines[0]),
             "  gl%s(GL_MULTISAMPLE);",
             g_multisample_enabled ? "Enable" : "Disable");
    snprintf(g_render_state_lines[1], sizeof(g_render_state_lines[1]),
             "  gl%s(GL_LINE_SMOOTH);",
             g_line_smooth_enabled ? "Enable" : "Disable");
}

void update_lookat_strings(void) {
    float rx = g_cam_rx * (float)M_PI / 180.0f;
    float ry = g_cam_ry * (float)M_PI / 180.0f;
    float crx = cosf(rx), srx = sinf(rx);
    float cry = cosf(ry), sry = sinf(ry);
    float d = g_cam_dist, px = g_cam_px, py = g_cam_py;

    /* Eye: Ry(-ry) * Rx(-rx) * (-px, -py, dist) */
    float ex = -px * cry - py * srx * sry - d * crx * sry;
    float ey = -py * crx + d * srx;
    float ez = -px * sry + py * srx * cry + d * crx * cry;

    /* Center: Ry(-ry) * Rx(-rx) * (-px, -py, 0) */
    float cx = -px * cry - py * srx * sry;
    float cy = -py * crx;
    float cz = -px * sry + py * srx * cry;

    snprintf(g_lookat[0], sizeof(g_lookat[0]),
             "  gluLookAt(%.2f, %.2f, %.2f,", ex, ey, ez);
    snprintf(g_lookat[1], sizeof(g_lookat[1]),
             "            %.2f, %.2f, %.2f,", cx, cy, cz);
    snprintf(g_lookat[2], sizeof(g_lookat[2]),
             "            0.0, 1.0, 0.0);");
}

/* Write light configuration to a file as C source */
static void write_light_setup(FILE *f) {
    static const char *light_names[] = {
        "GL_LIGHT0", "GL_LIGHT1", "GL_LIGHT2", "GL_LIGHT3"
    };

    fprintf(f, "\n  /* Light setup */\n");
    for (int i = 0; i < MAX_LIGHTS; i++) {
        const SceneLight *l = &g_lights[i];
        const char *ln = light_names[i];

        if (!l->enabled) continue;

        fprintf(f, "  {\n");
        fprintf(f, "    GLfloat pos[]  = { %.2ff, %.2ff, %.2ff, %.2ff };\n",
                l->pos[0], l->pos[1], l->pos[2], l->pos[3]);
        fprintf(f, "    GLfloat dif[]  = { %.2ff, %.2ff, %.2ff, 1.0f };\n",
                l->diffuse[0], l->diffuse[1], l->diffuse[2]);
        fprintf(f, "    GLfloat amb[]  = { %.2ff, %.2ff, %.2ff, 1.0f };\n",
                l->ambient[0], l->ambient[1], l->ambient[2]);
        fprintf(f, "    GLfloat spec[] = { %.2ff, %.2ff, %.2ff, 1.0f };\n",
                l->specular[0], l->specular[1], l->specular[2]);
        fprintf(f, "    glEnable(%s);\n", ln);
        fprintf(f, "    glLightfv(%s, GL_POSITION, pos);\n", ln);
        fprintf(f, "    glLightfv(%s, GL_DIFFUSE,  dif);\n", ln);
        fprintf(f, "    glLightfv(%s, GL_AMBIENT,  amb);\n", ln);
        fprintf(f, "    glLightfv(%s, GL_SPECULAR, spec);\n", ln);
        fprintf(f, "  }\n");
    }
}

/* Save the current session as a standalone C source file */
/* Write a FOR_BEGIN command as a C for-loop header */
static void write_for_begin_as_c(FILE *f, const GLCmd *cmd) {
    /* Extract var name and args from source: "  for(i, 0, 24) {" */
    char var_name[16];
    const char *p = cmd->source;
    /* Count leading whitespace for indent */
    int indent = 0;
    while (p[indent] && isspace((unsigned char)p[indent])) indent++;

    char ind[32];
    if (indent > (int)sizeof(ind) - 1) indent = (int)sizeof(ind) - 1;
    memset(ind, ' ', indent);
    ind[indent] = '\0';

    if (cmd->has_vars) {
        /* Extract raw arg expressions from source and translate to C */
        const char *hp = p;
        while (*hp && *hp != '(') hp++;
        if (*hp) hp++;
        /* skip var name */
        while (*hp && isspace((unsigned char)*hp)) hp++;
        int ni = 0;
        while (*hp && (isalnum((unsigned char)*hp) || *hp == '_') && ni < (int)sizeof(var_name) - 1)
            var_name[ni++] = *hp++;
        var_name[ni] = '\0';
        while (*hp && isspace((unsigned char)*hp)) hp++;
        if (*hp == ',') hp++;

        /* Extract each comma-separated arg expression */
        char start_s[128] = "", end_s[128] = "", step_s[128] = "";
        int nargs = 0;
        char *dests[] = { start_s, end_s, step_s };
        int dsizes[] = { (int)sizeof(start_s), (int)sizeof(end_s), (int)sizeof(step_s) };
        while (*hp && *hp != ')' && nargs < 3) {
            while (*hp && isspace((unsigned char)*hp)) hp++;
            const char *as = hp;
            int depth = 0;
            while (*hp && !(*hp == ',' && depth == 0) && !(*hp == ')' && depth == 0)) {
                if (*hp == '(') depth++;
                else if (*hp == ')') depth--;
                hp++;
            }
            int alen = (int)(hp - as);
            while (alen > 0 && isspace((unsigned char)as[alen-1])) alen--;
            if (alen > dsizes[nargs] - 1) alen = dsizes[nargs] - 1;
            memcpy(dests[nargs], as, alen);
            dests[nargs][alen] = '\0';
            nargs++;
            if (*hp == ',') hp++;
        }

        /* Translate expressions to C */
        char c_start[128], c_end[128], c_step[128];
        repl_expr_to_c(start_s, c_start, sizeof(c_start));
        repl_expr_to_c(end_s, c_end, sizeof(c_end));
        if (step_s[0])
            repl_expr_to_c(step_s, c_step, sizeof(c_step));
        else
            strncpy(c_step, "1.0f", sizeof(c_step));

        /* Determine direction from evaluated values */
        float step_v = cmd->args[2];
        if (step_v >= 0) {
            fprintf(f, "%sfor (float %s = %s; %s < %s; %s += %s) {\n",
                    ind, var_name, c_start, var_name, c_end, var_name, c_step);
        } else {
            fprintf(f, "%sfor (float %s = %s; %s > %s; %s += %s) {\n",
                    ind, var_name, c_start, var_name, c_end, var_name, c_step);
        }
        return;
    }

    /* Parse REPL for-header from source */
    float start_v, end_v, step_v;
    const char *body;
    if (parse_for_header(p, var_name, sizeof(var_name),
                         &start_v, &end_v, &step_v, &body)) {
        if (step_v == 1.0f) {
            fprintf(f, "%sfor (float %s = %g; %s < %g; %s += 1.0f) {\n",
                    ind, var_name, start_v, var_name, end_v, var_name);
        } else if (step_v == -1.0f) {
            fprintf(f, "%sfor (float %s = %g; %s > %g; %s -= 1.0f) {\n",
                    ind, var_name, start_v, var_name, end_v, var_name);
        } else if (step_v > 0) {
            fprintf(f, "%sfor (float %s = %g; %s < %g; %s += %gf) {\n",
                    ind, var_name, start_v, var_name, end_v, var_name, step_v);
        } else {
            fprintf(f, "%sfor (float %s = %g; %s > %g; %s += %gf) {\n",
                    ind, var_name, start_v, var_name, end_v, var_name, step_v);
        }
    } else {
        /* Fallback: write source as-is */
        fprintf(f, "%s\n", cmd->source);
    }
}

static void quadric_source_to_c(const char *src, char *out, int out_sz) {
    const char *p = src;
    const char *open;
    int indent = 0;
    int prefix_len;
    int written;

    if (out_sz <= 0) return;
    out[0] = '\0';

    while (p[indent] == ' ' || p[indent] == '\t')
        indent++;
    p += indent;

    open = strchr(p, '(');
    if (!open) {
        strncpy(out, src, (size_t)out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }

    prefix_len = indent + (int)(open - p);
    if (prefix_len > out_sz - 1)
        prefix_len = out_sz - 1;
    memcpy(out, src, (size_t)prefix_len);
    out[prefix_len] = '\0';

    written = snprintf(out + prefix_len, (size_t)(out_sz - prefix_len),
                       "(g_quadric%s%s",
                       open[1] == ')' ? "" : ", ",
                       open + 1);
    if (written < 0 || prefix_len + written >= out_sz)
        out[out_sz - 1] = '\0';
}

static int split_top_level_args(const char *src, char args[][MAX_LINE_LEN], int max_args) {
    const char *p = src;
    int         count = 0;

    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (count >= max_args)
            return -1;

        const char *start = p;
        int         depth = 0;
        while (*p) {
            if (*p == '(')
                depth++;
            else if (*p == ')') {
                if (depth == 0)
                    break;
                depth--;
            } else if (*p == ',' && depth == 0) {
                break;
            }
            p++;
        }

        int n = (int)(p - start);
        if (n > MAX_LINE_LEN - 1)
            n = MAX_LINE_LEN - 1;
        memcpy(args[count], start, (size_t)n);
        args[count][n] = '\0';
        trim_in_place(args[count]);
        count++;

        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '\0')
            break;
        return -1;
    }

    return count;
}

static int write_tess_source_as_c(FILE *f, const GLCmd *cmd) {
    char payload[MAX_LINE_LEN];
    char raw_args[4][MAX_LINE_LEN];
    char c_args[4][MAX_LINE_LEN];
    int  arg_count;

    if (!repl_extract_paren_payload(cmd->source, payload, sizeof(payload)))
        return 0;

    arg_count = split_top_level_args(payload, raw_args, 4);
    if (arg_count < 0)
        return 0;

    for (int i = 0; i < arg_count; i++)
        repl_expr_to_c(raw_args[i], c_args[i], sizeof(c_args[i]));

    switch (cmd->type) {
    case CMD_TESS_NORMAL:
        if (arg_count != 3)
            return 0;
        fprintf(f, "      { _tn[0]=%s; _tn[1]=%s; _tn[2]=%s; }\n", c_args[0], c_args[1], c_args[2]);
        return 1;
    case CMD_TESS_COLOR:
        if (arg_count == 3) {
            strncpy(c_args[3], "1", sizeof(c_args[3]) - 1);
            c_args[3][sizeof(c_args[3]) - 1] = '\0';
            arg_count = 4;
        }
        if (arg_count != 4)
            return 0;
        fprintf(f, "      { _tc[0]=%s; _tc[1]=%s; _tc[2]=%s; _tc[3]=%s; }\n", c_args[0], c_args[1],
                c_args[2], c_args[3]);
        return 1;
    case CMD_TESS_VERTEX:
        if (arg_count != 3)
            return 0;
        fprintf(f,
                "      { TessVertex *_v=&_tv[_tv_n++];"
                " _v->pos[0]=%s;_v->pos[1]=%s;_v->pos[2]=%s;"
                " memcpy(_v->normal,_tn,24); memcpy(_v->color,_tc,32);"
                " gluTessVertex(g_tess,_v->pos,_v); }\n",
                c_args[0], c_args[1], c_args[2]);
        return 1;
    default:
        return 0;
    }
}

static void write_cmd_source_as_c(FILE *f, const GLCmd *cmd, int translate_exprs) {
    char c_src[MAX_LINE_LEN];
    char quadric_src[MAX_LINE_LEN];

    if (translate_exprs)
        repl_expr_to_c(cmd->source, c_src, sizeof(c_src));
    else {
        strncpy(c_src, cmd->source, sizeof(c_src) - 1);
        c_src[sizeof(c_src) - 1] = '\0';
    }

    if (cmd_type_is_quadric(cmd->type)) {
        quadric_source_to_c(c_src, quadric_src, sizeof(quadric_src));
        fprintf(f, "%s\n", quadric_src);
    } else {
        fprintf(f, "%s\n", c_src);
    }
}

typedef enum {
    EXPORT_RENDER_CANONICAL = 0,
    EXPORT_RENDER_OUTLINE,
    EXPORT_RENDER_VPOINTS
} ExportRenderMode;

static int find_export_block_end(int begin_idx) {
    int depth = 1;

    for (int i = begin_idx + 1; i < g_num_cmds; i++) {
        CmdType t = g_cmds[i].type;
        if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) depth++;
        else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) {
            if (--depth == 0)
                return i;
        }
    }

    return g_num_cmds;
}

static int cmd_source_indent(const char *src) {
    int indent = 0;
    while (src[indent] == ' ' || src[indent] == '\t')
        indent++;
    return indent;
}

static void fprint_indent(FILE *f, int indent) {
    for (int i = 0; i < indent; i++)
        fputc(' ', f);
}

static void build_export_func_name(char *out, int out_sz, int fn,
                                   ExportRenderMode mode) {
    switch (mode) {
    case EXPORT_RENDER_OUTLINE:
        snprintf(out, out_sz, "render_repl_outline_func%d", fn);
        break;
    case EXPORT_RENDER_VPOINTS:
        snprintf(out, out_sz, "render_repl_vpoints_func%d", fn);
        break;
    case EXPORT_RENDER_CANONICAL:
    default:
        snprintf(out, out_sz, "func%d", fn);
        break;
    }
}

static int export_begin_mode_has_outline_overlay(GLenum mode) {
    switch (mode) {
    case GL_POINTS:
    case GL_LINES:
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        return 0;
    default:
        return 1;
    }
}

static int write_translated_vertex_call_as_c(FILE *f, const GLCmd *cmd,
                                             const char *func_name) {
    char payload[MAX_LINE_LEN];
    char raw_args[4][MAX_LINE_LEN];
    char c_args[4][MAX_LINE_LEN];
    int expected = 0;
    int count;
    int indent = cmd_source_indent(cmd->source);

    switch (cmd->type) {
    case CMD_VERTEX3F:
    case CMD_TESS_VERTEX:
        expected = 3;
        break;
    case CMD_VERTEX2F:
        expected = 2;
        break;
    default:
        return 0;
    }

    if (repl_extract_paren_payload(cmd->source, payload, sizeof(payload))) {
        count = split_top_level_args(payload, raw_args, 4);
        if (count == expected) {
            for (int i = 0; i < count; i++)
                repl_expr_to_c(raw_args[i], c_args[i], sizeof(c_args[i]));
            fprint_indent(f, indent);
            if (expected == 3)
                fprintf(f, "%s(%s, %s, %s);\n",
                        func_name, c_args[0], c_args[1], c_args[2]);
            else
                fprintf(f, "%s(%s, %s);\n", func_name, c_args[0], c_args[1]);
            return 1;
        }
    }

    fprint_indent(f, indent);
    if (expected == 3)
        fprintf(f, "%s(%g, %g, %g);\n",
                func_name, cmd->args[0], cmd->args[1], cmd->args[2]);
    else
        fprintf(f, "%s(%g, %g);\n", func_name, cmd->args[0], cmd->args[1]);
    return 1;
}

static void write_export_func_call_as_c(FILE *f, const GLCmd *cmd,
                                        ExportRenderMode mode) {
    char args[MAX_LINE_LEN] = "";
    char c_args[MAX_LINE_LEN] = "";
    char func_name[64];
    int indent = cmd_source_indent(cmd->source);
    int fn = (int)cmd->args[0];

    build_export_func_name(func_name, sizeof(func_name), fn, mode);
    if (extract_func_call_args_text(cmd->source, &fn, args, sizeof(args)) && args[0]) {
        repl_expr_to_c(args, c_args, sizeof(c_args));
        fprint_indent(f, indent);
        fprintf(f, "%s(%s);\n", func_name, c_args);
        return;
    }

    fprint_indent(f, indent);
    fprintf(f, "%s();\n", func_name);
}

/* Emit a single command as C with GLU-tessellator translation and
 * REPL→C expression translation for var assignments. Shared between
 * render_repl_geometry() and canonical func-body emission. tess_depth tracks
 * polygon/contour nesting across successive calls. */
static void write_canonical_cmd_as_c(FILE *f, const GLCmd *cmd, int for_depth,
                                     int *tess_depth) {
    switch (cmd->type) {
    case CMD_COMMENT:
        fprintf(f, "%s\n", cmd->source);
        break;
    case CMD_VAR_ASSIGN: {
        char c_src[MAX_LINE_LEN];
        repl_expr_to_c(cmd->source, c_src, sizeof(c_src));
        fprintf(f, "%s\n", c_src);
        break;
    }
    case CMD_CALL:
        write_cmd_source_as_c(f, cmd, 1);
        break;
    case CMD_TESS_BEGIN_POLYGON:
        fprintf(f, "  { _tv_n=0; gluTessBeginPolygon(g_tess,NULL); }\n");
        *tess_depth = 1;
        break;
    case CMD_TESS_BEGIN_CONTOUR:
        fprintf(f, "    gluTessBeginContour(g_tess);\n");
        *tess_depth = 2;
        break;
    case CMD_TESS_END:
        if (*tess_depth == 2) {
            fprintf(f, "    gluTessEndContour(g_tess);\n");
            *tess_depth = 1;
        } else {
            fprintf(f, "  gluTessEndPolygon(g_tess);\n");
            *tess_depth = 0;
        }
        break;
    case CMD_TESS_NORMAL:
        if (!write_tess_source_as_c(f, cmd)) {
            fprintf(f, "      { _tn[0]=%g; _tn[1]=%g; _tn[2]=%g; }\n", cmd->args[0], cmd->args[1],
                    cmd->args[2]);
        }
        break;
    case CMD_TESS_COLOR:
        if (!write_tess_source_as_c(f, cmd)) {
            fprintf(f, "      { _tc[0]=%g; _tc[1]=%g; _tc[2]=%g; _tc[3]=%g; }\n", cmd->args[0],
                    cmd->args[1], cmd->args[2], cmd->args[3]);
        }
        break;
    case CMD_TESS_VERTEX:
        if (!write_tess_source_as_c(f, cmd)) {
            fprintf(f,
                    "      { TessVertex *_v=&_tv[_tv_n++];"
                    " _v->pos[0]=%g;_v->pos[1]=%g;_v->pos[2]=%g;"
                    " memcpy(_v->normal,_tn,24); memcpy(_v->color,_tc,32);"
                    " gluTessVertex(g_tess,_v->pos,_v); }\n",
                    cmd->args[0], cmd->args[1], cmd->args[2]);
        }
        break;
    default:
        write_cmd_source_as_c(f, cmd, for_depth > 0 || cmd->has_vars);
        break;
    }
}

static void write_overlay_cmd_as_c(FILE *f, const GLCmd *cmd, int for_depth,
                                   ExportRenderMode mode, int *in_begin,
                                   int *tess_depth) {
    switch (cmd->type) {
    case CMD_COMMENT:
        fprintf(f, "%s\n", cmd->source);
        break;
    case CMD_VAR_ASSIGN: {
        char c_src[MAX_LINE_LEN];
        repl_expr_to_c(cmd->source, c_src, sizeof(c_src));
        fprintf(f, "%s\n", c_src);
        break;
    }
    case CMD_CALL:
    case CMD_LABEL:
    case CMD_GOTO:
    case CMD_IF_BEGIN:
    case CMD_IF_END:
    case CMD_TRANSLATE3F:
    case CMD_SCALEF:
    case CMD_ROTATEF:
    case CMD_PUSH_MATRIX:
    case CMD_POP_MATRIX:
        if (cmd->type == CMD_CALL)
            write_export_func_call_as_c(f, cmd, mode);
        else
            write_cmd_source_as_c(f, cmd, for_depth > 0 || cmd->has_vars);
        break;
    case CMD_BEGIN:
        if (mode != EXPORT_RENDER_OUTLINE)
            break;
        if (*in_begin) {
            fprint_indent(f, cmd_source_indent(cmd->source));
            fprintf(f, "glEnd();\n");
            *in_begin = 0;
        }
        if (export_begin_mode_has_outline_overlay(cmd->mode)) {
            write_cmd_source_as_c(f, cmd, 0);
            *in_begin = 1;
        }
        break;
    case CMD_END:
        if (mode == EXPORT_RENDER_OUTLINE && *in_begin) {
            write_cmd_source_as_c(f, cmd, 0);
            *in_begin = 0;
        }
        break;
    case CMD_VERTEX3F:
        if (mode == EXPORT_RENDER_OUTLINE) {
            if (*in_begin)
                write_translated_vertex_call_as_c(f, cmd, "glVertex3f");
        } else {
            int indent = cmd_source_indent(cmd->source);
            fprint_indent(f, indent);
            fprintf(f, "glBegin(GL_POINTS);\n");
            write_translated_vertex_call_as_c(f, cmd, "glVertex3f");
            fprint_indent(f, indent);
            fprintf(f, "glEnd();\n");
        }
        break;
    case CMD_TESS_BEGIN_POLYGON:
        if (mode == EXPORT_RENDER_OUTLINE)
            *tess_depth = 1;
        break;
    case CMD_TESS_BEGIN_CONTOUR:
        if (mode == EXPORT_RENDER_OUTLINE) {
            int indent = cmd_source_indent(cmd->source);
            if (*tess_depth == 2) {
                fprint_indent(f, indent);
                fprintf(f, "glEnd();\n");
            }
            fprint_indent(f, indent);
            fprintf(f, "glBegin(GL_LINE_LOOP);\n");
            *tess_depth = 2;
        }
        break;
    case CMD_TESS_END:
        if (mode == EXPORT_RENDER_OUTLINE) {
            if (*tess_depth == 2) {
                int indent = cmd_source_indent(cmd->source);
                fprint_indent(f, indent);
                fprintf(f, "glEnd();\n");
                *tess_depth = 1;
            } else if (*tess_depth == 1) {
                *tess_depth = 0;
            }
        }
        break;
    case CMD_TESS_VERTEX:
        if (mode == EXPORT_RENDER_OUTLINE) {
            if (*tess_depth == 2)
                write_translated_vertex_call_as_c(f, cmd, "glVertex3f");
        } else {
            int indent = cmd_source_indent(cmd->source);
            fprint_indent(f, indent);
            fprintf(f, "glBegin(GL_POINTS);\n");
            write_translated_vertex_call_as_c(f, cmd, "glVertex3f");
            fprint_indent(f, indent);
            fprintf(f, "glEnd();\n");
        }
        break;
    default:
        break;
    }
}

static void write_render_body_range_as_c(FILE *f, int start, int end_idx,
                                         ExportRenderMode mode,
                                         int skip_func_defs) {
    int for_depth = 0;
    int tess_depth = 0;
    int in_begin = 0;

    for (int i = start; i < end_idx && i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        switch (g_cmds[i].type) {
        case CMD_FOR_BEGIN:
            write_for_begin_as_c(f, &g_cmds[i]);
            for_depth++;
            break;
        case CMD_FOR_END:
            for_depth--;
            fprintf(f, "%s\n", g_cmds[i].source);
            break;
        case CMD_FUNC_DEF:
            if (skip_func_defs)
                i = find_export_block_end(i);
            break;
        case CMD_FUNC_END:
            break;
        default:
            if (mode == EXPORT_RENDER_CANONICAL)
                write_canonical_cmd_as_c(f, &g_cmds[i], for_depth, &tess_depth);
            else
                write_overlay_cmd_as_c(f, &g_cmds[i], for_depth, mode,
                                       &in_begin, &tess_depth);
            break;
        }
    }
}

/* Emit predefined variables as file-scope statics, so that user-defined
 * functions (written above display()) can access them. display() re-assigns
 * them on each call to refresh the values from the current slider state. */
static void write_predef_var_globals(FILE *f) {
    if (g_num_predef_vars <= 0) return;
    fprintf(f, "\n/* Predefined REPL variables (file scope for func access) */\n");
    for (int i = 0; i < g_num_predef_vars; i++) {
        fprintf(f, "static float %s = 0.0f;\n", g_predef_vars[i].name);
    }
}

static void write_predef_var_reset_func(FILE *f) {
    fprintf(f, "\nstatic void reset_repl_vars(void) {\n");
    for (int i = 0; i < g_num_predef_vars; i++) {
        if (strcmp(g_predef_vars[i].name, "t") == 0) {
            fprintf(f, "  %s = 0.001f * (float)glutGet(GLUT_ELAPSED_TIME);\n",
                    g_predef_vars[i].name);
        } else {
            fprintf(f, "  %s = %g;\n",
                    g_predef_vars[i].name, g_predef_vars[i].value);
        }
    }
    fprintf(f, "}\n");
}

/* Emit deterministic stateless random helper used by translated rand(...). */
static void write_rand_helper(FILE *f) {
    fprintf(f,
        "\nstatic float repl_randf(float seed, float iter) {\n"
        "  float h = sinf(seed * 12.9898f + iter * 78.233f) * 43758.5453f;\n"
        "  float frac = h - floorf(h);\n"
        "  if (frac < 0.0f) frac += 1.0f;\n"
        "  return frac;\n"
        "}\n");
}

static void write_render_helper_as_c(FILE *f, const char *name,
                                     ExportRenderMode mode,
                                     int include_snippet_markers) {
    fprintf(f, "\nstatic void %s(void) {\n", name);
    if (include_snippet_markers)
        fprintf(f, "  // Snippet start\n");
    write_render_body_range_as_c(f, 0, g_num_cmds, mode, 1);
    if (mode == EXPORT_RENDER_CANONICAL) {
        int bb = 0;
        for (int i = 0; i < g_num_cmds; i++) {
            if (g_cmds[i].valid && g_cmds[i].type == CMD_BEGIN) bb++;
            else if (g_cmds[i].valid && g_cmds[i].type == CMD_END) bb--;
        }
        if (bb > 0)
            fprintf(f, "  glEnd();\n");
    }
    if (include_snippet_markers)
        fprintf(f, "  // Snippet end\n");
    fprintf(f, "}\n");
}

/* Emit all user-defined functions as static C functions (before display()) */
static void write_func_defs_as_c(FILE *f, ExportRenderMode mode) {
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid || g_cmds[i].type != CMD_FUNC_DEF) continue;
        int fn = (int)g_cmds[i].args[0];
        int parsed_fn = fn;
        int param_count = 0;
        char param_names[MAX_EXPR_VARS][16];
        char func_name[64];
        build_export_func_name(func_name, sizeof(func_name), fn, mode);
        /* find the matching CMD_FUNC_END */
        int fe = find_export_block_end(i);
        if (parse_repl_func_signature(g_cmds[i].source, &parsed_fn,
                                      param_names, MAX_EXPR_VARS,
                                      &param_count) && param_count > 0) {
            build_export_func_name(func_name, sizeof(func_name), parsed_fn, mode);
            fprintf(f, "\nstatic void %s(", func_name);
            for (int p = 0; p < param_count; p++)
                fprintf(f, "%sfloat %s", p == 0 ? "" : ", ", param_names[p]);
            fprintf(f, ") {\n");
        } else {
            fprintf(f, "\nstatic void %s(void) {\n", func_name);
        }
        write_render_body_range_as_c(f, i + 1, fe, mode, 0);
        fprintf(f, "}\n");
    }
}

static void write_tess_preamble(FILE *f) {
    fprintf(f,
        "#include <string.h>\n"
        "typedef struct { GLdouble pos[3]; GLdouble normal[3]; GLdouble color[4]; } TessVertex;\n"
        "static TessVertex _tv[256];\n"
        "static int _tv_n = 0;\n"
        "static GLdouble _tn[3] = {0.0, 0.0, 1.0};\n"
        "static GLdouble _tc[4] = {1.0, 1.0, 1.0, 1.0};\n"
        "static GLUtesselator *g_tess = NULL;\n"
        "static void _tess_vtx_cb(void *vd) {\n"
        "    TessVertex *v=(TessVertex*)vd;\n"
        "    glNormal3dv(v->normal); glColor4dv(v->color); glVertex3dv(v->pos);\n"
        "}\n"
        "static void _tess_comb_cb(GLdouble coords[3],void *vd[4],GLfloat w[4],void **out) {\n"
        "    if(_tv_n>=256){*out=NULL;return;}\n"
        "    TessVertex *v=&_tv[_tv_n++];\n"
        "    v->pos[0]=coords[0];v->pos[1]=coords[1];v->pos[2]=coords[2];\n"
        "    for(int c=0;c<3;c++)v->normal[c]=0.0;\n"
        "    for(int c=0;c<4;c++)v->color[c]=0.0;\n"
        "    for(int j=0;j<4;j++){\n"
        "        if(!vd[j])continue;\n"
        "        TessVertex *s=(TessVertex*)vd[j];\n"
        "        for(int c=0;c<3;c++)v->normal[c]+=w[j]*s->normal[c];\n"
        "        for(int c=0;c<4;c++)v->color[c]+=w[j]*s->color[c];\n"
        "    }\n"
        "    double len=sqrt(v->normal[0]*v->normal[0]+v->normal[1]*v->normal[1]+v->normal[2]*v->normal[2]);\n"
        "    if(len>1e-9){v->normal[0]/=len;v->normal[1]/=len;v->normal[2]/=len;}\n"
        "    *out=v;\n"
        "}\n"
        "__attribute__((constructor)) static void _init_tess(void) {\n"
        "    g_tess=gluNewTess();\n"
        "    gluTessCallback(g_tess,GLU_TESS_BEGIN,(void(*)())glBegin);\n"
        "    gluTessCallback(g_tess,GLU_TESS_END,(void(*)())glEnd);\n"
        "    gluTessCallback(g_tess,GLU_TESS_VERTEX,(void(*)())_tess_vtx_cb);\n"
        "    gluTessCallback(g_tess,GLU_TESS_COMBINE,(void(*)())_tess_comb_cb);\n"
        "}\n"
    );
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

static void trim_in_place(char *s) {
    int start = 0;
    int len = (int)strlen(s);
    while (start < len && isspace((unsigned char)s[start])) start++;
    while (len > start && isspace((unsigned char)s[len - 1])) len--;
    if (start > 0) memmove(s, s + start, (size_t)(len - start));
    s[len - start] = '\0';
}

int repl_extract_paren_payload(const char *src, char *out, int out_sz) {
    const char *p = strchr(src, '(');
    if (!p) return 0;
    p++;
    const char *start = p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if (depth > 0) p++;
    }
    if (depth != 0) return 0;
    int n = (int)(p - start);
    if (n > out_sz - 1) n = out_sz - 1;
    memcpy(out, start, (size_t)n);
    out[n] = '\0';
    trim_in_place(out);
    return 1;
}

static int extract_for_args_text(const char *src,
                                 char *var, int var_sz,
                                 char *args, int args_sz) {
    const char *p = strchr(src, '(');
    if (!p) return 0;
    p++;

    while (*p && isspace((unsigned char)*p)) p++;

    int vi = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && vi < var_sz - 1)
        var[vi++] = *p++;
    var[vi] = '\0';
    if (vi == 0) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ',') return 0;
    p++;

    const char *start = p;
    int depth = 0;
    while (*p) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            if (depth == 0) break;
            depth--;
        }
        p++;
    }
    if (*p != ')') return 0;

    int n = (int)(p - start);
    if (n > args_sz - 1) n = args_sz - 1;
    memcpy(args, start, (size_t)n);
    args[n] = '\0';
    trim_in_place(args);
    return 1;
}

static int parse_identifier_list(const char *src,
                                 char names[][16], int max_names) {
    const char *p = src;
    int count = 0;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (count >= max_names) return -1;
        if (!isalpha((unsigned char)*p) && *p != '_') return -1;

        int ni = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
            if (ni >= 15) return -1;
            names[count][ni++] = *p++;
        }
        names[count][ni] = '\0';
        count++;

        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p != ',') return -1;
        p++;
    }

    return count;
}

static int parse_expr_list_exact(const char *src, float *out_vals, int max_vals,
                                 ExprVar *vars, int num_vars, int *out_count) {
    const char *p = src;
    int count = 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) {
        if (out_count) *out_count = 0;
        return 1;
    }

    for (;;) {
        ExprCtx ctx = { p, vars, num_vars };
        float value = eval_expr(&ctx);
        if (ctx.p == p) return 0;
        if (count >= max_vals) return 0;
        if (out_vals) out_vals[count] = value;
        count++;

        p = ctx.p;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p != ',') return 0;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) return 0;
    }

    if (out_count) *out_count = count;
    return 1;
}

static int parse_repl_func_signature(const char *src, int *fn,
                                     char param_names[][16], int max_params,
                                     int *param_count) {
    const char *p = src;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "func", 4) != 0) return 0;
    p += 4;
    if (*p < '0' || *p > '9') return 0;
    if (fn) *fn = *p - '0';
    p++;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '{' || *p == '\0') {
        if (param_count) *param_count = 0;
        return 1;
    }
    if (*p != '(') return 0;

    const char *payload_start = ++p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if (depth > 0) p++;
    }
    if (depth != 0) return 0;

    char payload[MAX_LINE_LEN];
    int n = (int)(p - payload_start);
    if (n > (int)sizeof(payload) - 1) n = (int)sizeof(payload) - 1;
    memcpy(payload, payload_start, (size_t)n);
    payload[n] = '\0';
    trim_in_place(payload);

    while (*p == ')') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' && *p != '\0') return 0;

    if (!payload[0]) {
        if (param_count) *param_count = 0;
        return 1;
    }

    int count = parse_identifier_list(payload, param_names, max_params);
    if (count < 0) return 0;
    if (param_count) *param_count = count;
    return 1;
}

static int extract_func_call_args_text(const char *src, int *fn,
                                       char *args, int args_sz) {
    const char *p = src;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "func", 4) != 0) return 0;
    p += 4;
    if (*p < '0' || *p > '9') return 0;
    if (fn) *fn = *p - '0';
    p++;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(') return 0;
    p++;
    const char *start = p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if (depth > 0) p++;
    }
    if (depth != 0) return 0;

    int n = (int)(p - start);
    if (n > args_sz - 1) n = args_sz - 1;
    memcpy(args, start, (size_t)n);
    args[n] = '\0';
    trim_in_place(args);

    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0' && *p != ';') return 0;
    return 1;
}

static void format_func_header(char *out, int out_sz, const char *indent,
                               int fn, char param_names[][16], int param_count) {
    int written = snprintf(out, out_sz, "%sfunc%d", indent, fn);
    if (written < 0 || written >= out_sz) {
        if (out_sz > 0) out[out_sz - 1] = '\0';
        return;
    }
    if (param_count > 0) {
        written += snprintf(out + written, out_sz - written, "(");
        for (int i = 0; i < param_count && written < out_sz; i++) {
            written += snprintf(out + written, out_sz - written, "%s%s",
                                i == 0 ? "" : ", ", param_names[i]);
        }
        if (written < out_sz)
            written += snprintf(out + written, out_sz - written, ")");
    }
    if (written < out_sz)
        snprintf(out + written, out_sz - written, " {");
}

static int input_has_expr_vars(const char *s, ExprVar *vars, int num_vars) {
    while (*s) {
        if (!isalpha((unsigned char)*s) && *s != '_') { s++; continue; }
        const char *start = s;
        while (*s && (isalnum((unsigned char)*s) || *s == '_')) s++;
        int len = (int)(s - start);
        for (int i = 0; i < num_vars; i++) {
            int nlen = (int)strlen(vars[i].name);
            if (nlen == len && strncmp(start, vars[i].name, len) == 0)
                return 1;
        }
    }
    return 0;
}

static int input_has_any_visible_vars(const char *s, ExprVar *vars, int num_vars) {
    return input_has_predef_vars(s) || input_has_expr_vars(s, vars, num_vars);
}

int repl_extract_label_name(const char *src, char *name, int name_sz) {
    const char *p = src;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == ':') p++;
    int n = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && n < name_sz - 1)
        name[n++] = *p++;
    name[n] = '\0';
    return n > 0;
}

int repl_extract_goto_label(const char *src, char *name, int name_sz) {
    const char *p = strstr(src, "goto");
    if (!p) p = src;
    else p += 4;
    while (*p && isspace((unsigned char)*p)) p++;
    int n = 0;
    while (*p && *p != ';' && !isspace((unsigned char)*p) &&
           (isalnum((unsigned char)*p) || *p == '_') && n < name_sz - 1)
        name[n++] = *p++;
    name[n] = '\0';
    return n > 0;
}

int repl_extract_assignment_parts(const char *src,
                                  char *name, int name_sz,
                                  char *rhs, int rhs_sz) {
    const char *p = src;
    const char *rhs_start;
    const char *rhs_end;
    int n = 0;

    while (*p && isspace((unsigned char)*p)) p++;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (name && n < name_sz - 1)
            name[n] = *p;
        n++;
        p++;
    }
    if (name && name_sz > 0)
        name[n < name_sz - 1 ? n : name_sz - 1] = '\0';
    if (n == 0)
        return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=' || p[1] == '=')
        return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p)
        return 0;

    rhs_start = p;
    rhs_end = src + strlen(src);
    while (rhs_end > rhs_start && isspace((unsigned char)rhs_end[-1])) rhs_end--;
    if (rhs_end > rhs_start && rhs_end[-1] == ';') rhs_end--;
    while (rhs_end > rhs_start && isspace((unsigned char)rhs_end[-1])) rhs_end--;
    if (rhs_end <= rhs_start)
        return 0;

    if (rhs && rhs_sz > 0) {
        int rn = (int)(rhs_end - rhs_start);
        if (rn > rhs_sz - 1) rn = rhs_sz - 1;
        memcpy(rhs, rhs_start, (size_t)rn);
        rhs[rn] = '\0';
        trim_in_place(rhs);
    }
    return 1;
}

static int import_parse_predef_decl(const char *line) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "float ", 6) != 0) return 0;
    p += 6;

    int updated = 0;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;

        char name[16];
        int ni = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
               ni < (int)sizeof(name) - 1)
            name[ni++] = *p++;
        name[ni] = '\0';
        if (ni == 0) break;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '=') break;
        p++;

        ExprCtx ctx = { p, NULL, 0 };
        float val = eval_expr(&ctx);
        p = ctx.p;

        for (int i = 0; i < g_num_predef_vars; i++) {
            if (strcmp(g_predef_vars[i].name, name) == 0) {
                g_predef_vars[i].value = val;
                updated = 1;
                break;
            }
        }

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ',') {
            p++;
            continue;
        }
        break;
    }
    return updated;
}

static int import_make_repl_for_header(const char *line, char *out, int out_sz) {
    char var[16];
    float start_v, end_v, step_v;
    if (!parse_c_for_header(line, var, sizeof(var), &start_v, &end_v, &step_v))
        return 0;

    char repl_line[MAX_LINE_LEN];
    c_expr_to_repl(line, repl_line, sizeof(repl_line));

    if (input_has_predef_vars(repl_line)) {
        const char *rp = repl_line;
        while (*rp && *rp != '=') rp++;
        if (!*rp) return 0;
        rp++;
        while (*rp && isspace((unsigned char)*rp)) rp++;

        const char *se_start = rp;
        int depth = 0;
        while (*rp) {
            if (*rp == '(') depth++;
            else if (*rp == ')') depth--;
            if (*rp == ';' && depth == 0) break;
            rp++;
        }
        if (*rp != ';') return 0;

        char start_expr[96];
        int sl = (int)(rp - se_start);
        if (sl > (int)sizeof(start_expr) - 1) sl = (int)sizeof(start_expr) - 1;
        memcpy(start_expr, se_start, (size_t)sl);
        start_expr[sl] = '\0';
        trim_in_place(start_expr);

        rp++;
        while (*rp && isspace((unsigned char)*rp)) rp++;
        while (*rp && (isalnum((unsigned char)*rp) || *rp == '_')) rp++;
        while (*rp && isspace((unsigned char)*rp)) rp++;
        while (*rp && (*rp == '<' || *rp == '>' || *rp == '=' || *rp == '!')) rp++;
        while (*rp && isspace((unsigned char)*rp)) rp++;

        const char *ee_start = rp;
        depth = 0;
        while (*rp) {
            if (*rp == '(') depth++;
            else if (*rp == ')') depth--;
            if (*rp == ';' && depth == 0) break;
            rp++;
        }
        if (*rp != ';') return 0;

        char end_expr[96];
        int el = (int)(rp - ee_start);
        if (el > (int)sizeof(end_expr) - 1) el = (int)sizeof(end_expr) - 1;
        memcpy(end_expr, ee_start, (size_t)el);
        end_expr[el] = '\0';
        trim_in_place(end_expr);

        if (step_v != 1.0f) {
            snprintf(out, out_sz, "for(%s, %s, %s, %g) {",
                     var, start_expr, end_expr, step_v);
        } else {
            snprintf(out, out_sz, "for(%s, %s, %s) {",
                     var, start_expr, end_expr);
        }
        return 1;
    }

    if (step_v != 1.0f)
        snprintf(out, out_sz, "for(%s, %g, %g, %g) {",
                 var, start_v, end_v, step_v);
    else
        snprintf(out, out_sz, "for(%s, %g, %g) {",
                 var, start_v, end_v);
    return 1;
}

static int import_make_repl_func_header(const char *line, char *out, int out_sz) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "static void func", 16) != 0)
        return 0;
    p += 16;

    if (*p < '0' || *p > '9')
        return 0;
    int fn = *p - '0';
    p++;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(')
        return 0;
    p++;
    const char *start = p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if (depth > 0) p++;
    }
    if (depth != 0)
        return 0;

    char payload[MAX_LINE_LEN];
    int n = (int)(p - start);
    if (n > (int)sizeof(payload) - 1) n = (int)sizeof(payload) - 1;
    memcpy(payload, start, (size_t)n);
    payload[n] = '\0';
    trim_in_place(payload);

    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{')
        return 0;

    if (!payload[0] || strcmp(payload, "void") == 0) {
        snprintf(out, out_sz, "func%d {", fn);
        return 1;
    }

    char names[MAX_EXPR_VARS][16];
    int count = 0;
    char *cursor = payload;
    while (*cursor) {
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (strncmp(cursor, "float", 5) != 0 || !isspace((unsigned char)cursor[5]))
            return 0;
        cursor += 5;
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (count >= MAX_EXPR_VARS) return 0;
        if (!isalpha((unsigned char)*cursor) && *cursor != '_') return 0;

        int ni = 0;
        while (*cursor && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
            if (ni >= 15) return 0;
            names[count][ni++] = *cursor++;
        }
        names[count][ni] = '\0';
        count++;

        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (!*cursor) break;
        if (*cursor != ',') return 0;
        cursor++;
    }

    int written = snprintf(out, out_sz, "func%d(", fn);
    for (int i = 0; i < count && written < out_sz; i++)
        written += snprintf(out + written, out_sz - written, "%s%s",
                            i == 0 ? "" : ", ", names[i]);
    if (written < out_sz)
        snprintf(out + written, out_sz - written, ") {");
    return 1;
}

static int import_make_repl_label(const char *line, char *out, int out_sz) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;

    int llen = 0;
    while (p[llen] && (isalnum((unsigned char)p[llen]) || p[llen] == '_'))
        llen++;
    if (llen <= 0 || p[llen] != ':')
        return 0;
    if (p[llen + 1] != '\0' && !isspace((unsigned char)p[llen + 1]))
        return 0;

    snprintf(out, out_sz, ":%.*s", llen, p);
    return 1;
}

static int import_make_repl_tess_line(const char *line, char *out, int out_sz) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;

    if (strstr(p, "gluTessBeginPolygon") != NULL) {
        snprintf(out, out_sz, "gluBegin(GLU_POLYGON);");
        return 1;
    }
    if (strstr(p, "gluTessBeginContour") != NULL) {
        snprintf(out, out_sz, "gluBegin(GLU_CONTOUR);");
        return 1;
    }
    if (strstr(p, "gluTessEndContour") != NULL ||
        strstr(p, "gluTessEndPolygon") != NULL) {
        snprintf(out, out_sz, "gluEnd();");
        return 1;
    }

    if (strncmp(p, "{ _tn[", 6) == 0) {
        float nv[3] = {0, 0, 1};
        const char *np = p;
        for (int i = 0; i < 3; i++) {
            const char *eq = strchr(np, '=');
            if (!eq) break;
            eq++;
            ExprCtx ctx = { eq, NULL, 0 };
            nv[i] = eval_expr(&ctx);
            np = ctx.p;
        }
        snprintf(out, out_sz, "gluNormal(%g, %g, %g);", nv[0], nv[1], nv[2]);
        return 1;
    }

    if (strncmp(p, "{ _tc[", 6) == 0) {
        float cv[4] = {1, 1, 1, 1};
        const char *cp = p;
        for (int i = 0; i < 4; i++) {
            const char *eq = strchr(cp, '=');
            if (!eq) break;
            eq++;
            ExprCtx ctx = { eq, NULL, 0 };
            cv[i] = eval_expr(&ctx);
            cp = ctx.p;
        }
        snprintf(out, out_sz, "gluColor(%g, %g, %g, %g);",
                 cv[0], cv[1], cv[2], cv[3]);
        return 1;
    }

    if (strstr(p, "TessVertex") != NULL && strstr(p, "gluTessVertex") != NULL) {
        float vv[3] = {0, 0, 0};
        const char *vp = strstr(p, "_v->pos[0]");
        if (!vp) return 0;
        for (int i = 0; i < 3; i++) {
            const char *eq = strchr(vp, '=');
            if (!eq) break;
            eq++;
            ExprCtx ctx = { eq, NULL, 0 };
            vv[i] = eval_expr(&ctx);
            vp = ctx.p;
        }
        snprintf(out, out_sz, "gluVertex(%g, %g, %g);",
                 vv[0], vv[1], vv[2]);
        return 1;
    }

    return 0;
}

static int import_make_repl_quadric_line(const char *line, char *out, int out_sz) {
    static const char *const names[] = {
        "gluSphere",
        "gluCylinder",
        "gluDisk",
        "gluPartialDisk",
        NULL
    };
    const char *p = line;
    int indent = 0;

    while (p[indent] == ' ' || p[indent] == '\t')
        indent++;
    p += indent;

    for (int i = 0; names[i]; i++) {
        const char *name = names[i];
        int name_len = (int)strlen(name);
        const char *open;
        const char *args;
        char tmp[MAX_LINE_LEN];
        int prefix_len;

        if (strncmp(p, name, (size_t)name_len) != 0 || p[name_len] != '(')
            continue;

        open = p + name_len;
        args = open + 1;
        while (*args == ' ' || *args == '\t')
            args++;
        if (strncmp(args, "g_quadric", 9) != 0)
            return 0;
        args += 9;
        while (*args == ' ' || *args == '\t')
            args++;
        if (*args == ',')
            args++;
        while (*args == ' ' || *args == '\t')
            args++;

        prefix_len = indent + name_len + 1;
        if (prefix_len >= (int)sizeof(tmp))
            prefix_len = (int)sizeof(tmp) - 1;
        memcpy(tmp, line, (size_t)prefix_len);
        tmp[prefix_len] = '\0';
        strncat(tmp, args, sizeof(tmp) - 1 - strlen(tmp));

        c_expr_to_repl(tmp, out, out_sz);
        return 1;
    }

    return 0;
}

static void import_feed_one_line(const char *line, int *loaded, int *warnings) {
    char repl_line[MAX_LINE_LEN];
    int before = g_num_cmds;
    int handled = 0;

    if (import_make_repl_for_header(line, repl_line, sizeof(repl_line))) {
        handled = feed_line(repl_line);
    } else if (import_make_repl_tess_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_quadric_line(line, repl_line, sizeof(repl_line)) ||
               import_make_repl_label(line, repl_line, sizeof(repl_line))) {
        handled = feed_line(repl_line);
    } else {
        c_expr_to_repl(line, repl_line, sizeof(repl_line));
        handled = feed_line(repl_line);
    }

    if (g_num_cmds > before) *loaded += (g_num_cmds - before);
    if (!handled) {
        fprintf(stderr, "Warning: could not parse line: %s\n", line);
        (*warnings)++;
    }
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
            if (p[0] == '/' && p[1] == '/') p += 2;
            while (*p == ' ') p++;
            char body[MAX_LINE_LEN];
            strncpy(body, p, sizeof(body) - 1);
            body[sizeof(body) - 1] = '\0';
            trim_in_place(body);
            if (body[0])
                snprintf(fmt.source, sizeof(fmt.source), "%s// %s", ind_s, body);
            else
                snprintf(fmt.source, sizeof(fmt.source), "%s//", ind_s);
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

static void save_output(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        set_status("Error: cannot write output.c");
        return;
    }

    /* Detect whether any tess commands are present */
    int has_tess = 0;
    int needs_rand = 0;
    for (int i = 0; i < g_num_cmds; i++)
        if (g_cmds[i].valid && g_cmds[i].type >= CMD_TESS_BEGIN_POLYGON
                            && g_cmds[i].type <= CMD_TESS_VERTEX)
            has_tess = 1;
        else if (g_cmds[i].valid && strstr(g_cmds[i].source, "rand(") != NULL)
            needs_rand = 1;

    update_render_state_strings();
    update_lookat_strings();

    for (int i = 0; g_header_pre[i]; i++) {
        if (strcmp(g_header_pre[i], "void display() {") == 0)
            break;
        fprintf(f, "%s\n", g_header_pre[i]);
    }
    write_predef_var_globals(f);
    if (needs_rand)
        write_rand_helper(f);
    if (has_tess) write_tess_preamble(f);
    write_predef_var_reset_func(f);
    write_func_defs_as_c(f, EXPORT_RENDER_CANONICAL);
    if (g_show_outlines)
        write_func_defs_as_c(f, EXPORT_RENDER_OUTLINE);
    if (g_show_vpoints)
        write_func_defs_as_c(f, EXPORT_RENDER_VPOINTS);
    write_render_helper_as_c(f, "render_repl_geometry",
                             EXPORT_RENDER_CANONICAL, 1);
    if (g_show_outlines)
        write_render_helper_as_c(f, "render_repl_outline_overlay",
                                 EXPORT_RENDER_OUTLINE, 0);
    if (g_show_vpoints)
        write_render_helper_as_c(f, "render_repl_vertex_points_overlay",
                                 EXPORT_RENDER_VPOINTS, 0);

    fprintf(f, "\nvoid display() {\n");
    fprintf(f, "  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);\n");
    fprintf(f, "  glLoadIdentity();\n");
    fprintf(f, "  glPushAttrib(GL_ALL_ATTRIB_BITS);\n");
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        fprintf(f, "%s\n", g_render_state_lines[i]);
    for (int i = 0; i < LOOKAT_LINE_COUNT; i++)
        fprintf(f, "%s\n", g_lookat[i]);
    for (int i = 0; g_header_post[i]; i++)
        fprintf(f, "%s\n", g_header_post[i]);
    write_light_setup(f);

    fprintf(f, "  glPushMatrix();\n");
    fprintf(f, "  reset_repl_vars();\n");
    fprintf(f, "  render_repl_geometry();\n");
    fprintf(f, "  glPopMatrix();\n");

    if (g_show_outlines) {
        fprintf(f, "\n  glDisable(GL_LIGHTING);\n");
        fprintf(f, "  glEnable(GL_DEPTH_TEST);\n");
        fprintf(f, "  glDepthMask(GL_FALSE);\n");
        fprintf(f, "  glEnable(GL_BLEND);\n");
        fprintf(f, "  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);\n");
        fprintf(f, "  glEnable(GL_POLYGON_OFFSET_LINE);\n");
        fprintf(f, "  glPolygonOffset(-1.0f, -1.0f);\n");
        fprintf(f, "  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);\n");
        fprintf(f, "  glColor3f(0.0f, 0.0f, 0.0f);\n");
        fprintf(f, "  glLineWidth(1.2f);\n");
        fprintf(f, "  glPushMatrix();\n");
        fprintf(f, "  reset_repl_vars();\n");
        fprintf(f, "  render_repl_outline_overlay();\n");
        fprintf(f, "  glPopMatrix();\n");
        fprintf(f, "  glLineWidth(1.0f);\n");
        fprintf(f, "  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);\n");
        fprintf(f, "  glDisable(GL_POLYGON_OFFSET_LINE);\n");
        fprintf(f, "  glDepthMask(GL_TRUE);\n");
        fprintf(f, "  glDisable(GL_BLEND);\n");
    }

    if (g_show_vpoints) {
        fprintf(f, "\n  glDisable(GL_LIGHTING);\n");
        fprintf(f, "  glEnable(GL_DEPTH_TEST);\n");
        fprintf(f, "  glColor3f(0.0f, 0.0f, 0.0f);\n");
        fprintf(f, "  glPointSize(8.0f);\n");
        fprintf(f, "  glPushMatrix();\n");
        fprintf(f, "  reset_repl_vars();\n");
        fprintf(f, "  render_repl_vertex_points_overlay();\n");
        fprintf(f, "  glPopMatrix();\n");
        fprintf(f, "  glPointSize(1.0f);\n");
    }

    fprintf(f, "\n  glPopAttrib();\n");
    fprintf(f, "  glutSwapBuffers();\n");
    fprintf(f, "}\n");
    for (int i = 4; g_footer[i]; i++)
        fprintf(f, "%s\n", g_footer[i]);

    fclose(f);

    char msg[128];
    snprintf(msg, sizeof(msg), "Saved to output.c (%d commands)", g_num_cmds);
    set_status(msg);
}

/* Load commands from a file, reading lines between snippet markers.
 * Returns 1 if at least one command was loaded, 0 otherwise. */
static int load_from_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return 0;

    char line[MAX_LINE_LEN];
    int in_snippet = 0;
    int import_func_depth = 0;
    int loaded = 0;
    int warnings = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;

        if (!in_snippet) {
            if (import_func_depth > 0) {
                import_feed_one_line(p, &loaded, &warnings);
                for (const char *bp = p; *bp; bp++) {
                    if (*bp == '{') import_func_depth++;
                    else if (*bp == '}') import_func_depth--;
                }
                continue;
            }

            char repl_func_line[MAX_LINE_LEN];
            if (import_make_repl_func_header(p, repl_func_line, sizeof(repl_func_line))) {
                int before = g_num_cmds;
                int handled = feed_line(repl_func_line);
                if (g_num_cmds > before) loaded += (g_num_cmds - before);
                if (!handled) {
                    fprintf(stderr, "Warning: could not parse line: %s\n", line);
                    warnings++;
                }
                import_func_depth = 1;
                continue;
            }

            /* Look for start marker */
            if (strncmp(p, "// Snippet start", 16) == 0)
                in_snippet = 1;
            continue;
        }

        /* Check for end marker */
        if (strncmp(p, "// Snippet end", 14) == 0)
            break;

        /* Skip empty lines */
        if (len == 0 || *p == '\0') continue;
        if (import_parse_predef_decl(p))
            continue;
        import_feed_one_line(p, &loaded, &warnings);
    }

    fclose(f);

    if (loaded > 0) {
        /* Canonicalize formatting after import so loaded files match
         * the same deterministic layout as interactive editing. */
        depth_cache_invalidate();
        repl_reformat_commands();
        char msg[256];
        if (warnings > 0)
            snprintf(msg, sizeof(msg),
                     "Loaded %d commands from %s (%d warnings)",
                     loaded, filename, warnings);
        else
            snprintf(msg, sizeof(msg),
                     "Loaded %d commands from %s", loaded, filename);
        set_status(msg);
        fprintf(stderr, "%s\n", msg);
    }
    return loaded > 0;
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
    fprintf(dst, "=== End REPL Editor Dump ===\n");
    fflush(dst);
}

static int dump_code_panel_available_chars(int panel_w, int x) {
    int avail_px = panel_w - x - 4;
    if (avail_px < FONT_W)
        return 0;
    return avail_px / FONT_W;
}

static int dump_code_panel_cont_indent_chars(const char *text) {
    const char *src = text ? text : "";
    const char *paren = strchr(src, '(');
    if (paren && paren[1] != '\0')
        return (int)(paren - src) + 1;

    int leading = 0;
    while (src[leading] && isspace((unsigned char)src[leading]))
        leading++;
    return leading + 4;
}

static int dump_code_panel_find_wrap_break(const char *text, int start,
                                           int max_chars, int len) {
    int end = start + max_chars - 1;
    if (end >= len)
        end = len - 1;

    for (int i = end; i > start; i--) {
        if (text[i] == ',')
            return i;
    }

    for (int i = end + 1; i < len; i++) {
        if (text[i] == ',')
            return i;
    }

    return -1;
}

static void dump_code_panel_wrapped_line(FILE *dst, const char *text,
                                         int first_x, int panel_w) {
    const char *src = text ? text : "";
    int len = (int)strlen(src);
    int pos = 0;
    int x = first_x;
    int cont_indent_chars = dump_code_panel_cont_indent_chars(src);
    int cont_x = first_x + cont_indent_chars * FONT_W;

    if (len == 0) {
        fputc('\n', dst);
        return;
    }

    for (;;) {
        int width_chars = dump_code_panel_available_chars(panel_w, x);
        int remaining = len - pos;
        int seg_len = remaining;
        int prefix_chars = (x - first_x) / FONT_W;

        if (g_wrap_at_comma && width_chars >= 1 && remaining > width_chars) {
            int break_idx = dump_code_panel_find_wrap_break(src, pos,
                                                            width_chars, len);
            if (break_idx >= 0)
                seg_len = break_idx - pos + 1;
        }

        fprintf(dst, "%*s%.*s\n", prefix_chars, "", seg_len, src + pos);

        if (!g_wrap_at_comma || width_chars < 1 || seg_len == remaining)
            break;

        pos += seg_len;
        x = cont_x;
    }
}

void repl_dump_code_panel_text(FILE *out) {
    FILE *dst = out ? out : stdout;

    update_render_state_strings();
    update_lookat_strings();

    fprintf(dst, "--- header_pre ---\n");
    for (int i = 0; g_header_pre[i]; i++)
        fprintf(dst, "%s\n", g_header_pre[i]);

    fprintf(dst, "--- render_state ---\n");
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        fprintf(dst, "%s\n", g_render_state_lines[i]);

    fprintf(dst, "--- lookat ---\n");
    for (int i = 0; i < LOOKAT_LINE_COUNT; i++)
        fprintf(dst, "%s\n", g_lookat[i]);

    fprintf(dst, "--- header_post ---\n");
    for (int i = 0; g_header_post[i]; i++)
        fprintf(dst, "%s\n", g_header_post[i]);

    fprintf(dst, "--- source ---\n");
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        fprintf(dst, "%s\n", g_cmds[i].source);
    }

    fflush(dst);
}

void repl_dump_code_panel_visual_text(FILE *out) {
    FILE *dst = out ? out : stdout;
    int panel_w = (int)(g_win_w * g_panel_frac);
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;

    update_render_state_strings();
    update_lookat_strings();

    fprintf(dst, "--- header_pre ---\n");
    for (int i = 0; g_header_pre[i]; i++)
        dump_code_panel_wrapped_line(dst, g_header_pre[i], text_x, panel_w);

    fprintf(dst, "--- render_state ---\n");
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        dump_code_panel_wrapped_line(dst, g_render_state_lines[i], text_x, panel_w);

    fprintf(dst, "--- lookat ---\n");
    for (int i = 0; i < LOOKAT_LINE_COUNT; i++)
        dump_code_panel_wrapped_line(dst, g_lookat[i], text_x, panel_w);

    fprintf(dst, "--- header_post ---\n");
    for (int i = 0; g_header_post[i]; i++)
        dump_code_panel_wrapped_line(dst, g_header_post[i], text_x, panel_w);

    fprintf(dst, "--- source ---\n");
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        dump_code_panel_wrapped_line(dst, g_cmds[i].source, text_x, panel_w);
    }

    fflush(dst);
}

/* ========================================================================= */
/* Line editor helpers                                                        */
/* ========================================================================= */

/* Load a command line's text into the input buffer for editing */
static void load_line_to_input(int idx) {
    if (idx >= 0 && idx < g_num_cmds) {
        /* Strip leading whitespace and trailing ; from source */
        const char *s = g_cmds[idx].source;
        while (*s && isspace((unsigned char)*s)) s++;
        int len = (int)strlen(s);
        while (len > 0 && (s[len - 1] == ';' || isspace((unsigned char)s[len - 1])))
            len--;
        if (len >= MAX_INPUT_LEN) len = MAX_INPUT_LEN - 1;
        memcpy(g_input, s, len);
        g_input[len] = '\0';
        g_input_len = len;
        g_cursor_pos = len;
    } else {
        /* New line: restore saved buffer */
        memcpy(g_input, g_newline_buf, g_newline_len + 1);
        g_input_len = g_newline_len;
        g_cursor_pos = g_newline_len;
    }
}

/* Save current input as the new-line buffer */
static void save_newline_buf(void) {
    memcpy(g_newline_buf, g_input, g_input_len + 1);
    g_newline_len = g_input_len;
}

/* Navigate to a different line */
void navigate_to_line(int target) {
    if (target < 0) target = 0;
    if (target > g_num_cmds) target = g_num_cmds;
    if (target == g_edit_line && !g_inserting) return;

    /* Save new-line buffer if leaving it */
    if (g_edit_line == g_num_cmds && !g_inserting)
        save_newline_buf();

    g_edit_line = target;
    g_inserting = 0;
    load_line_to_input(target);
    g_ac_count = 0;
    g_ac_ghost[0] = '\0';
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

static void update_autocomplete(void) {
    g_ac_count = 0;
    g_ac_sel = 0;
    g_ac_ghost[0] = '\0';

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
                    g_ac_matches[g_ac_count++] = g_point_param_pnames[j].name;
                }
            }
            if (g_ac_count > 0) {
                snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s, ", g_ac_matches[0] + alen);
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
                        g_ac_matches[g_ac_count++] = g_enum_cmds[i].enums1[j].name;
                    }
                }
                if (g_ac_count > 0) {
                    if (abs(g_enum_cmds[i].num_args) == 1) {
                        snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s)", g_ac_matches[0] + alen);
                    } else if (abs(g_enum_cmds[i].num_args) == 2) {
                        snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s, ", g_ac_matches[0] + alen);
                    }
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
                            g_ac_matches[g_ac_count++] = g_enum_cmds[i].enums2[j].name;
                        }
                    }
                    if (g_ac_count > 0) {
                        snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s)", g_ac_matches[0] + arg2_len);
                    }
                    return;
                }
            }
        }
    }

    /* Complete function names */
    for (int i = 0; g_func_completions[i] && g_ac_count < MAX_AC_MATCHES; i++) {
        if (strncmp(g_func_completions[i], g_input, g_input_len) == 0 &&
            (int)strlen(g_func_completions[i]) > g_input_len) {
            g_ac_matches[g_ac_count++] = g_func_completions[i];
        }
    }
    if (g_ac_count > 0) {
        const char *m = g_ac_matches[0];
        snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s", m + g_input_len);
    }
}

static void accept_autocomplete(void) {
    if (g_ac_count == 0 || g_ac_ghost[0] == '\0') return;

    int ghost_len = (int)strlen(g_ac_ghost);
    if (g_input_len + ghost_len < MAX_INPUT_LEN - 1) {
        strcat(g_input, g_ac_ghost);
        g_input_len += ghost_len;
        g_cursor_pos = g_input_len;
    }
    g_ac_count = 0;
    g_ac_ghost[0] = '\0';
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
static int find_block_end(int begin_idx);
static int block_depth_at(int pos);
static CmdType nearest_open_block_at(int pos);
static void get_for_var_name(const GLCmd *cmd, char *var, int var_sz);
static int cmd_type_is_quadric(CmdType t);
static void quadric_source_to_c(const char *src, char *out, int out_sz);
static int import_make_repl_quadric_line(const char *line, char *out, int out_sz);
static void write_cmd_source_as_c(FILE *f, const GLCmd *cmd, int translate_exprs);

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

static int cmd_type_is_quadric(CmdType t) {
    return t == CMD_GLU_SPHERE ||
           t == CMD_GLU_CYLINDER ||
           t == CMD_GLU_DISK ||
           t == CMD_GLU_PARTIAL_DISK;
}

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
static void compute_block_normals(GLenum mode, int *vi, int nv,
                                  float norms[][3]) {
    /* Default: zero (will be overwritten for valid faces) */
    for (int i = 0; i < nv; i++)
        norms[i][0] = norms[i][1] = norms[i][2] = 0;

    float n[3];
    switch (mode) {
    case GL_TRIANGLES:
        for (int i = 0; i + 2 < nv; i += 3) {
            face_normal(g_cmds[vi[i]].args, g_cmds[vi[i+1]].args,
                        g_cmds[vi[i+2]].args, n);
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
            for (int j = 0; j < 4; j++)
                memcpy(norms[i+j], n, sizeof(n));
        }
        break;
    case GL_QUAD_STRIP:
        for (int i = 0; i + 3 < nv; i += 2) {
            face_normal(g_cmds[vi[i]].args, g_cmds[vi[i+1]].args,
                        g_cmds[vi[i+2]].args, n);
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
 * Existing normals (whether auto-generated or manually edited) are never
 * overwritten, so the user can freely modify them.
 */
void recompute_autonormals(void) {
    if (!g_autonormal) return;

    /* Process each begin/end block (skip for-loop regions) */
    int i = 0;
    while (i < g_num_cmds) {
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
        compute_block_normals(mode, vi, nv, norms);

        /* For each vertex, insert an auto-normal only if none precedes it */
        int offset = 0; /* tracks insertions shifting indices */
        for (int v = 0; v < nv; v++) {
            int vidx = vi[v] + offset;
            float nx = norms[v][0], ny = norms[v][1], nz = norms[v][2];

            /* Skip if there is already any normal (auto or manual) before
               this vertex — the user may have edited it */
            if (vidx > 0 && g_cmds[vidx - 1].valid &&
                g_cmds[vidx - 1].type == CMD_NORMAL3F) {
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

static void replay_tick_fade_batches(float dt) {
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

static void replay_seek(int new_pc) {
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

static void replay_step_back(void) {
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
            glEnable(g_flat_cmds[pc].mode);
            for (int li = 0; li < MAX_LIGHTS; li++)
                if (g_lights[li].id == g_flat_cmds[pc].mode)
                    g_lights[li].enabled = 1;
            break;
        case CMD_DISABLE:
            glDisable(g_flat_cmds[pc].mode);
            for (int li = 0; li < MAX_LIGHTS; li++)
                if (g_lights[li].id == g_flat_cmds[pc].mode)
                    g_lights[li].enabled = 0;
            break;
        case CMD_SHADE_MODEL:
            glShadeModel(g_flat_cmds[pc].mode);
            break;
        case CMD_COLOR_MATERIAL:
            glColorMaterial(g_flat_cmds[pc].mode, (GLenum)g_flat_cmds[pc].args[0]);
            break;
        case CMD_MATERIALF:
            if (g_flat_cmds[pc].num_args == 2) {
                glMaterialf(g_flat_cmds[pc].mode, (GLenum)g_flat_cmds[pc].args[0], g_flat_cmds[pc].args[1]);
            } else if (g_flat_cmds[pc].num_args == 5) {
                GLfloat mat[4] = { g_flat_cmds[pc].args[1], g_flat_cmds[pc].args[2],
                                   g_flat_cmds[pc].args[3],
                                   g_flat_cmds[pc].args[4] * g_execute_alpha_scale };
                glMaterialfv(g_flat_cmds[pc].mode, (GLenum)g_flat_cmds[pc].args[0], mat);
            }
            break;
        case CMD_LIGHT_MODEL_I:
            glLightModeli(g_flat_cmds[pc].mode, (GLint)g_flat_cmds[pc].args[0]);
            break;
        case CMD_VERTEX2F:
            if (in_begin)
                glVertex2f(g_flat_cmds[pc].args[0], g_flat_cmds[pc].args[1]);
            break;
        case CMD_FRONT_FACE:
            glFrontFace(g_flat_cmds[pc].mode);
            break;
        case CMD_POINT_SIZE:
            if (in_begin) { glEnd(); in_begin = 0; }
            glPointSize(g_flat_cmds[pc].args[0]);
            break;
        case CMD_POINT_PARAMETER_FV: {
            if (in_begin) { glEnd(); in_begin = 0; }
            GLfloat params[3] = {
                g_flat_cmds[pc].args[0],
                g_flat_cmds[pc].args[1],
                g_flat_cmds[pc].args[2],
            };
            glPointParameterfv(g_flat_cmds[pc].mode, params);
            break;
        }
        case CMD_BLEND_FUNC:
            if (in_begin) { glEnd(); in_begin = 0; }
            glBlendFunc(g_flat_cmds[pc].mode, (GLenum)g_flat_cmds[pc].args[0]);
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
static int find_block_end(int begin_idx) {
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
static int block_depth_at(int pos) {
    depth_cache_rebuild();
    if (pos < 0) pos = 0;
    if (pos > g_num_cmds) pos = g_num_cmds;
    return g_block_depth_prefix[pos];
}

/* Return the innermost unclosed block type at pos, or CMD_TYPE_COUNT if none */
static CmdType nearest_open_block_at(int pos) {
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

static int collect_visible_vars(int pos, ExprVar *vars, int max_vars) {
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

/*
 * Try to handle input as a variable assignment: "x = expr"
 * Returns 1 if handled (valid or invalid assignment), 0 if not an assignment.
 */
static int try_assign_variable(void) {
    char name[16];
    char rhs[MAX_LINE_LEN];
    int has_rhs_vars;
    float val;
    char indent[32];
    int ind;

    if (!repl_extract_assignment_parts(g_input, name, sizeof(name), rhs, sizeof(rhs)))
        return 0;

    /* Check if it's a known predefined variable */
    int var_idx = -1;
    for (int i = 0; i < g_num_predef_vars; i++) {
        if (strcmp(name, g_predef_vars[i].name) == 0) {
            var_idx = i;
            break;
        }
    }
    if (var_idx < 0) return 0;

    /* Evaluate RHS expression */
    ExprCtx ctx = { rhs, g_predef_vars, g_num_predef_vars };
    val = eval_expr(&ctx);
    g_predef_vars[var_idx].value = val;
    has_rhs_vars = input_has_predef_vars(rhs);

    /* Build a CMD_VAR_ASSIGN command so it appears in the code panel */
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_VAR_ASSIGN;
    cmd.valid = 1;
    cmd.args[0] = val;
    cmd.num_args = var_idx; /* store predef var index */
    cmd.has_vars = has_rhs_vars;

    int fpos = g_inserting ? g_edit_line :
               (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
    ind = (in_begin_block_at(fpos) ? 4 : 2) + block_depth_at(fpos) * 2;
    if (ind > (int)sizeof(indent) - 1) ind = (int)sizeof(indent) - 1;
    memset(indent, ' ', (size_t)ind);
    indent[ind] = '\0';
    snprintf(cmd.source, sizeof(cmd.source), "%s%s = %s;", indent, name, rhs);

    if (g_inserting) {
        if (g_num_cmds < MAX_COMMANDS) {
            for (int j = g_num_cmds; j > fpos; j--)
                g_cmds[j] = g_cmds[j - 1];
            g_cmds[fpos] = cmd;
            g_num_cmds++;
            g_edit_line++;
        }
    } else if (fpos < g_num_cmds) {
        g_cmds[fpos] = cmd;
        g_edit_line++;
        load_line_to_input(g_edit_line);
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s = %g", name, val);
            set_status(msg);
        }
        mark_normals_dirty();
        return 1;
    } else {
        if (g_num_cmds < MAX_COMMANDS)
            g_cmds[g_num_cmds++] = cmd;
        g_edit_line = g_num_cmds;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "%s = %g", name, val);
    set_status(msg);

    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    mark_normals_dirty();
    return 1;
}

/*
 * Try to handle the input as a for-loop header.
 * Single-line: for(i,0,24) glVertex3f(...) -> inserts FOR_BEGIN + body + FOR_END
 * Multi-line:  for(i,0,24) {             -> inserts FOR_BEGIN + FOR_END, enters insert mode between
 */
static int try_commit_for_loop(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "for(", 4) != 0 && strncmp(p, "for (", 5) != 0)
        return 0;

    int pos = g_inserting ? g_edit_line :
              (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
    ExprVar visible_vars[MAX_EXPR_VARS];
    int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS);

    char var_name[16];
    float start, end, step;
    const char *body_start;
    if (!parse_for_header_with_vars(p, var_name, sizeof(var_name),
                                    &start, &end, &step,
                                    visible_vars, visible_nv, &body_start)) {
        set_status("for syntax: for(var, start, end[, step]) body;");
        return 1;
    }

    while (*body_start && isspace((unsigned char)*body_start)) body_start++;

    int fdepth = block_depth_at(pos);
    int bb = in_begin_block_at(pos);
    int ind = (bb ? 4 : 2) + fdepth * 2;
    char indent[32];
    if (ind > (int)sizeof(indent) - 1) ind = (int)sizeof(indent) - 1;
    memset(indent, ' ', ind);
    indent[ind] = '\0';

    /* Build FOR_BEGIN cmd */
    GLCmd fb;
    memset(&fb, 0, sizeof(fb));
    fb.type = CMD_FOR_BEGIN;
    fb.args[0] = start;
    fb.args[1] = end;
    fb.args[2] = step;
    fb.valid = 1;

    /* Extract raw arg text from input to preserve variable references */
    const char *raw = p;
    while (*raw && *raw != '(') raw++;
    if (*raw) raw++;
    /* skip var name and comma */
    while (*raw && isspace((unsigned char)*raw)) raw++;
    while (*raw && (isalnum((unsigned char)*raw) || *raw == '_')) raw++;
    while (*raw && isspace((unsigned char)*raw)) raw++;
    if (*raw == ',') raw++;
    /* raw now points at start expr; find closing paren */
    const char *args_start = raw;
    int paren = 1;
    const char *ap = args_start;
    while (*ap && paren > 0) {
        if (*ap == '(') paren++;
        else if (*ap == ')') paren--;
        if (paren > 0) ap++;
    }
    /* ap points at closing ')'; extract raw args text */
    char raw_args[MAX_LINE_LEN];
    int rlen = (int)(ap - args_start);
    if (rlen > (int)sizeof(raw_args) - 1) rlen = (int)sizeof(raw_args) - 1;
    memcpy(raw_args, args_start, rlen);
    raw_args[rlen] = '\0';
    /* Trim whitespace */
    while (rlen > 0 && isspace((unsigned char)raw_args[rlen-1])) raw_args[--rlen] = '\0';
    char *ra = raw_args;
    while (*ra && isspace((unsigned char)*ra)) ra++;

    if (input_has_any_visible_vars(ra, visible_vars, visible_nv)) {
        fb.has_vars = 1;
        snprintf(fb.source, sizeof(fb.source),
                 "%sfor(%s, %s) {", indent, var_name, ra);
    } else if (step != 1.0f) {
        snprintf(fb.source, sizeof(fb.source),
                 "%sfor(%s, %g, %g, %g) {", indent, var_name, start, end, step);
    } else {
        snprintf(fb.source, sizeof(fb.source),
                 "%sfor(%s, %g, %g) {", indent, var_name, start, end);
    }

    /* Build FOR_END cmd */
    GLCmd fe;
    memset(&fe, 0, sizeof(fe));
    fe.type = CMD_FOR_END;
    fe.valid = 1;
    snprintf(fe.source, sizeof(fe.source), "%s}", indent);

    if (*body_start == '{' || *body_start == '\0') {
        /* Editing an existing FOR_BEGIN in-place: update header, leave body/end untouched */
        if (!g_inserting && g_edit_line < g_num_cmds &&
            g_cmds[g_edit_line].type == CMD_FOR_BEGIN) {
            g_cmds[g_edit_line] = fb;
            g_edit_line++;
            g_inserting = 1;
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
            g_ac_count = 0;
            g_ac_ghost[0] = '\0';
            set_status("for-loop header updated");
            mark_normals_dirty();
            return 1;
        }

        /* Multi-line block: insert FOR_BEGIN and FOR_END, enter insert mode between */
        if (g_num_cmds + 2 > MAX_COMMANDS) {
            set_status("Command buffer full!");
            return 1;
        }
        memmove(&g_cmds[pos + 2], &g_cmds[pos],
                (g_num_cmds - pos) * sizeof(GLCmd));
        g_cmds[pos] = fb;
        g_cmds[pos + 1] = fe;
        g_num_cmds += 2;

        g_edit_line = pos + 1;
        g_inserting = 1;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        set_status("for-loop: type body lines, press Esc when done");
        mark_normals_dirty();
        return 1;
    }

    /* Single-line: for(...) body; */
    char body[MAX_LINE_LEN];
    strncpy(body, body_start, MAX_LINE_LEN - 1);
    body[MAX_LINE_LEN - 1] = '\0';
    int blen = (int)strlen(body);
    while (blen > 0 && (body[blen-1] == ';' || isspace((unsigned char)body[blen-1])))
        body[--blen] = '\0';
    if (blen == 0) {
        set_status("for-loop needs a body");
        return 1;
    }

    /* Validate body with dummy vars */
    ExprVar dv[MAX_EXPR_VARS];
    int dvn = 0;
    strncpy(dv[dvn].name, var_name, sizeof(dv[dvn].name) - 1);
    dv[dvn].name[sizeof(dv[dvn].name) - 1] = '\0';
    dv[dvn].value = start;
    dvn++;
    for (int i = 0; i < visible_nv && dvn < MAX_EXPR_VARS; i++)
        dv[dvn++] = visible_vars[i];
    GLCmd body_cmd;
    memset(&body_cmd, 0, sizeof(body_cmd));
    int saved = g_edit_line;
    g_edit_line = pos;
    if (!parse_command_with_vars(body, &body_cmd, dv, dvn)) {
        g_edit_line = saved;
        set_status("Invalid for-loop body command");
        return 1;
    }
    g_edit_line = saved;

    /* Overwrite source with original expression text */
    char bind[32];
    int bi = ind + 2;
    if (bi > (int)sizeof(bind) - 1) bi = (int)sizeof(bind) - 1;
    memset(bind, ' ', bi);
    bind[bi] = '\0';
    snprintf(body_cmd.source, sizeof(body_cmd.source), "%s%s;", bind, body);

    if (g_num_cmds + 3 > MAX_COMMANDS) {
        set_status("Command buffer full!");
        return 1;
    }
    memmove(&g_cmds[pos + 3], &g_cmds[pos],
            (g_num_cmds - pos) * sizeof(GLCmd));
    g_cmds[pos] = fb;
    g_cmds[pos + 1] = body_cmd;
    g_cmds[pos + 2] = fe;
    g_num_cmds += 3;

    g_edit_line = pos + 3;
    g_inserting = 0;
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    g_newline_buf[0] = '\0';
    g_newline_len = 0;

    char msg[128];
    snprintf(msg, sizeof(msg), "for-loop: %s from %g to %g", var_name, start, end);
    set_status(msg);
    mark_normals_dirty();
    return 1;
}

/*
 * Try to handle the input as a function definition: func0 { ... func9 {
 * Inserts CMD_FUNC_DEF + CMD_FUNC_END, enters insert mode between them.
 */
static int try_commit_func_def(void) {
    int fn = -1;
    int param_count = 0;
    char param_names[MAX_EXPR_VARS][16];
    const char *trimmed = g_input;
    while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
    if (strchr(trimmed, '(') && strchr(trimmed, '{') == NULL)
        return 0;
    if (!parse_repl_func_signature(g_input, &fn,
                                   param_names, MAX_EXPR_VARS,
                                   &param_count))
        return 0;

    int pos = g_inserting ? g_edit_line :
              (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
    int bdepth = block_depth_at(pos);
    int bb = in_begin_block_at(pos);
    int ind = (bb ? 4 : 2) + bdepth * 2;
    char indent[32];
    if (ind > (int)sizeof(indent) - 1) ind = (int)sizeof(indent) - 1;
    memset(indent, ' ', ind);
    indent[ind] = '\0';

    /* Editing an existing FUNC_DEF in-place */
    if (!g_inserting && g_edit_line < g_num_cmds &&
        g_cmds[g_edit_line].type == CMD_FUNC_DEF) {
        g_cmds[g_edit_line].args[0] = (float)fn;
        g_cmds[g_edit_line].num_args = param_count;
        format_func_header(g_cmds[g_edit_line].source,
                           (int)sizeof(g_cmds[g_edit_line].source),
                           indent, fn, param_names, param_count);
        g_edit_line++;
        g_inserting = 1;
        g_input[0] = '\0'; g_input_len = 0; g_cursor_pos = 0;
        g_ac_count = 0; g_ac_ghost[0] = '\0';
        set_status("func def header updated");
        mark_normals_dirty();
        return 1;
    }

    /* Build FUNC_DEF + FUNC_END */
    GLCmd fd;
    memset(&fd, 0, sizeof(fd));
    fd.type = CMD_FUNC_DEF;
    fd.args[0] = (float)fn;
    fd.num_args = param_count;
    fd.valid = 1;
    format_func_header(fd.source, (int)sizeof(fd.source),
                       indent, fn, param_names, param_count);

    GLCmd fe;
    memset(&fe, 0, sizeof(fe));
    fe.type = CMD_FUNC_END;
    fe.valid = 1;
    snprintf(fe.source, sizeof(fe.source), "%s}", indent);

    if (g_num_cmds + 2 > MAX_COMMANDS) {
        set_status("Command buffer full!");
        return 1;
    }
    memmove(&g_cmds[pos + 2], &g_cmds[pos],
            (g_num_cmds - pos) * sizeof(GLCmd));
    g_cmds[pos] = fd;
    g_cmds[pos + 1] = fe;
    g_num_cmds += 2;

    g_edit_line = pos + 1;
    g_inserting = 1;
    g_input[0] = '\0'; g_input_len = 0; g_cursor_pos = 0;
    set_status("func def: type body lines, press Esc when done");
    mark_normals_dirty();
    return 1;
}

/*
 * Try to handle the input as an if-block: if(expr) {
 * Inserts CMD_IF_BEGIN + CMD_IF_END, enters insert mode between them.
 */
static int try_commit_if_block(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "if(", 3) != 0 && strncmp(p, "if (", 4) != 0)
        return 0;

    int pos = g_inserting ? g_edit_line :
              (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
    ExprVar visible_vars[MAX_EXPR_VARS];
    int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS);

    /* Find opening paren */
    while (*p && *p != '(') p++;
    if (!*p) return 0;
    p++; /* skip '(' */

    /* Find matching closing paren (handle nested parens) */
    int paren = 1;
    const char *expr_start = p;
    while (*p && paren > 0) {
        if (*p == '(') paren++;
        else if (*p == ')') paren--;
        if (paren > 0) p++;
    }
    if (paren != 0) {
        set_status("if syntax: if(expr) {");
        return 1;
    }

    /* Extract condition expression text */
    char cond_text[MAX_LINE_LEN];
    int clen = (int)(p - expr_start);
    if (clen > (int)sizeof(cond_text) - 1) clen = (int)sizeof(cond_text) - 1;
    memcpy(cond_text, expr_start, clen);
    cond_text[clen] = '\0';

    /* Evaluate condition */
    float cond_args[1];
    int neval = parse_exprs(cond_text, cond_args, 1,
                            visible_nv > 0 ? visible_vars : NULL, visible_nv);
    float cond_val = (neval >= 1) ? cond_args[0] : 0.0f;

    /* Check for { after ) */
    p++; /* skip ')' */
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' && *p != '\0') {
        set_status("if syntax: if(expr) {");
        return 1;
    }

    int bdepth = block_depth_at(pos);
    int bb = in_begin_block_at(pos);
    int ind = (bb ? 4 : 2) + bdepth * 2;
    char indent[32];
    if (ind > (int)sizeof(indent) - 1) ind = (int)sizeof(indent) - 1;
    memset(indent, ' ', ind);
    indent[ind] = '\0';

    /* Build IF_BEGIN */
    GLCmd ib;
    memset(&ib, 0, sizeof(ib));
    ib.type = CMD_IF_BEGIN;
    ib.args[0] = cond_val;
    ib.valid = 1;
    ib.has_vars = input_has_any_visible_vars(cond_text, visible_vars, visible_nv);

    /* Trim whitespace from condition text for display */
    char *ct = cond_text;
    while (*ct && isspace((unsigned char)*ct)) ct++;
    int ctlen = (int)strlen(ct);
    while (ctlen > 0 && isspace((unsigned char)ct[ctlen-1])) ct[--ctlen] = '\0';
    snprintf(ib.source, sizeof(ib.source), "%sif(%s) {", indent, ct);

    /* Editing an existing IF_BEGIN in-place */
    if (!g_inserting && g_edit_line < g_num_cmds &&
        g_cmds[g_edit_line].type == CMD_IF_BEGIN) {
        g_cmds[g_edit_line] = ib;
        g_edit_line++;
        g_inserting = 1;
        g_input[0] = '\0'; g_input_len = 0; g_cursor_pos = 0;
        g_ac_count = 0; g_ac_ghost[0] = '\0';
        set_status("if condition updated");
        mark_normals_dirty();
        return 1;
    }

    /* Build IF_END */
    GLCmd ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = CMD_IF_END;
    ie.valid = 1;
    snprintf(ie.source, sizeof(ie.source), "%s}", indent);

    if (g_num_cmds + 2 > MAX_COMMANDS) {
        set_status("Command buffer full!");
        return 1;
    }
    memmove(&g_cmds[pos + 2], &g_cmds[pos],
            (g_num_cmds - pos) * sizeof(GLCmd));
    g_cmds[pos] = ib;
    g_cmds[pos + 1] = ie;
    g_num_cmds += 2;

    g_edit_line = pos + 1;
    g_inserting = 1;
    g_input[0] = '\0'; g_input_len = 0; g_cursor_pos = 0;
    set_status("if-block: type body lines, press Esc when done");
    mark_normals_dirty();
    return 1;
}

/*
 * Try to handle '}' closing any block (for-loop, function, or if).
 * If there's already a pre-inserted end at the current position, skip past it.
 * Otherwise, if there's an unclosed block, insert the appropriate end command.
 */
static int try_commit_close_brace(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '}') return 0;

    int pos = g_inserting ? g_edit_line :
              (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);

    /* Check for any unclosed block */
    CmdType open_type = nearest_open_block_at(pos);
    if (open_type == CMD_TYPE_COUNT) return 0;

    /* Determine matching end type and label */
    CmdType end_type;
    const char *label;
    if (open_type == CMD_FOR_BEGIN)  { end_type = CMD_FOR_END;  label = "for-loop"; }
    else if (open_type == CMD_FUNC_DEF) { end_type = CMD_FUNC_END; label = "func def"; }
    else if (open_type == CMD_IF_BEGIN)  { end_type = CMD_IF_END;   label = "if-block"; }
    else return 0;

    /* If we're in insert mode right before the matching end, just exit insert mode */
    if (g_inserting && pos < g_num_cmds && g_cmds[pos].type == end_type) {
        g_edit_line = pos + 1;
        g_inserting = 0;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        load_line_to_input(g_edit_line);
        char msg[64];
        snprintf(msg, sizeof(msg), "%s block closed", label);
        set_status(msg);
        mark_normals_dirty();
        return 1;
    }

    /* Otherwise insert an end command */
    int bdepth = block_depth_at(pos) - 1;
    if (bdepth < 0) bdepth = 0;
    int bb_val = in_begin_block_at(pos);
    int ind_len = (bb_val ? 4 : 2) + bdepth * 2;
    char indent[32];
    if (ind_len > (int)sizeof(indent) - 1) ind_len = (int)sizeof(indent) - 1;
    memset(indent, ' ', ind_len);
    indent[ind_len] = '\0';

    GLCmd fe;
    memset(&fe, 0, sizeof(fe));
    fe.type = end_type;
    fe.valid = 1;
    snprintf(fe.source, sizeof(fe.source), "%s}", indent);

    if (g_num_cmds >= MAX_COMMANDS) {
        set_status("Command buffer full!");
        return 1;
    }
    memmove(&g_cmds[pos + 1], &g_cmds[pos],
            (g_num_cmds - pos) * sizeof(GLCmd));
    g_cmds[pos] = fe;
    g_num_cmds++;
    g_edit_line = pos + 1;
    g_inserting = 0;
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    g_newline_buf[0] = '\0';
    g_newline_len = 0;
    char msg[64];
    snprintf(msg, sizeof(msg), "%s block closed", label);
    set_status(msg);
    mark_normals_dirty();
    return 1;
}

static void keyboard_func(unsigned char key, int x, int y) {
    (void)x; (void)y;

    /* Reset cursor blink on any input */
    g_cursor_on = 1;
    g_blink_tick = 0;

    /* Clear selection on any key except Ctrl+C/X (which consume it) */
    if (key != 3 && key != 24)
        clear_selection();

    /* Any keyboard input re-reveals the cursor line on next render. */
    g_scroll_follow_cursor = 1;


    /* Backtick: toggle configuration menu */
    if (!g_search_active && key == '`') {
        if (g_replay_active)
            replay_stop();
        g_show_config = !g_show_config;
        g_config_hover = -1;
        return;
    }

    if (g_replay_active) {
        if (key == 7) {
            replay_stop();
            set_status("Replay: off");
            return;
        }
        if (key == ' ') {
            if (g_replay_state == REPLAY_PLAYING) {
                g_replay_state = REPLAY_PAUSED;
                set_status("Replay: paused");
            } else if (g_replay_state == REPLAY_PAUSED) {
                g_replay_state = REPLAY_PLAYING;
                set_status("Replay: playing");
            } else if (g_replay_state == REPLAY_DONE) {
                g_replay_pc = 0;
                g_replay_accum = 0.0f;
                replay_clear_fade_batches();
                g_replay_state = REPLAY_PLAYING;
                g_replay_src_line = -1;
                g_replay_last_src_line = -1;
                set_status("Replay: restarted");
            }
            return;
        }
        if (key == '+' || key == '=') {
            char msg[64];
            g_replay_speed *= 1.5f;
            if (g_replay_speed > 200.0f) g_replay_speed = 200.0f;
            snprintf(msg, sizeof(msg), "Replay: %.1f step/s", g_replay_speed);
            set_status(msg);
            return;
        }
        if (key == '-') {
            char msg[64];
            g_replay_speed /= 1.5f;
            if (g_replay_speed < 0.5f) g_replay_speed = 0.5f;
            snprintf(msg, sizeof(msg), "Replay: %.1f step/s", g_replay_speed);
            set_status(msg);
            return;
        }
        if (key == 'm' || key == 'M') {
            int was_playing = (g_replay_state == REPLAY_PLAYING);
            g_replay_mode = (g_replay_mode == REPLAY_MODE_VERTEX)
                          ? REPLAY_MODE_POLYGON
                          : REPLAY_MODE_VERTEX;
            replay_seek(g_replay_pc);
            if (was_playing && g_replay_state != REPLAY_DONE)
                g_replay_state = REPLAY_PLAYING;
            set_status(g_replay_mode == REPLAY_MODE_VERTEX
                     ? "Replay: vertex mode"
                     : "Replay: polygon mode");
            return;
        }
        if (key == 27) {
            replay_stop();
            set_status("Replay: off");
            return;
        }
        replay_stop();
    }

    if (handle_search_key(key))
        return;

    /* Escape */
    if (key == 27) {
        if (g_show_config) {
            g_show_config = 0;
            return;
        }
        if (g_show_help) {
            g_show_help = 0;
            g_help_tab = 0;
            g_help_scroll = 0;
        } else if (g_ac_count > 0) {
            /* Dismiss autocomplete */
            g_ac_count = 0;
            g_ac_ghost[0] = '\0';
        } else if (g_inserting) {
            /* Exit insert mode */
            g_inserting = 0;
            if (g_edit_line <= g_num_cmds)
                load_line_to_input(g_edit_line);
            set_status("Insert mode exited");
        } else {
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
            set_status("Input cleared");
        }
        return;
    }

    /* Ctrl+A / Ctrl+E: line start / end */
    if (key == 1) {
        g_cursor_pos = 0;
        update_autocomplete();
        return;
    }
    if (key == 5) {
        g_cursor_pos = g_input_len;
        update_autocomplete();
        return;
    }

    /* Ctrl+Z: undo  /  Ctrl+Shift+Z: redo */
    if (key == 26) {
        if (glutGetModifiers() & GLUT_ACTIVE_SHIFT)
            do_redo();
        else
            pop_undo_snapshot();
        return;
    }

    /* Ctrl+Y: redo */
    if (key == 25) {
        do_redo();
        return;
    }

    /* Ctrl+G: replay */
    if (key == 7) {
        replay_start();
        return;
    }

    /* Ctrl+D: delete line at cursor */
    if (key == 4) {
        if (g_inserting) {
            /* Exit insert mode without deleting */
            g_inserting = 0;
            if (g_edit_line <= g_num_cmds)
                load_line_to_input(g_edit_line);
            set_status("Insert mode exited");
        } else if (g_edit_line < g_num_cmds) {
            push_undo_snapshot();
            for (int i = g_edit_line; i < g_num_cmds - 1; i++)
                g_cmds[i] = g_cmds[i + 1];
            g_num_cmds--;
                        if (g_edit_line > g_num_cmds)
                g_edit_line = g_num_cmds;
            load_line_to_input(g_edit_line);
            mark_normals_dirty();
            set_status("Line deleted");
        }
        return;
    }

    /* Ctrl+L: clear all */
    if (key == 12) {
        push_undo_snapshot();
        g_num_cmds = 0;
        g_edit_line = 0;
                g_inserting = 0;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        g_newline_buf[0] = '\0';
        g_newline_len = 0;
        mark_normals_dirty();
        set_status("All commands cleared");
        return;
    }

    /* Ctrl+R: reformat command buffer */
    if (key == 18) {
        if (g_num_cmds > 0) {
            push_undo_snapshot();
            repl_reformat_commands();
            set_status("Reformatted command buffer");
        } else {
            set_status("Nothing to reformat");
        }
        return;
    }

    /* Ctrl+P: dump editor command buffer to stdout */
    if (key == 16) {
        repl_debug_dump_editor(stdout);
        set_status("Dumped editor code to stdout");
        return;
    }

    /* Ctrl+S: save to output.c */
    if (key == 19) {
        save_output(outfile);
        return;
    }

    /* Ctrl+C: copy line/selection (or whole for-loop if on FOR_BEGIN) */
    if (key == 3) {
        if (g_inserting) { clear_selection(); return; }
        g_clipboard_count = 0;
        if (sel_active()) {
            /* Copy selected range */
            int lo = sel_lo(), hi = sel_hi();
            if (hi >= g_num_cmds) hi = g_num_cmds - 1;
            for (int i = lo; i <= hi && g_clipboard_count < MAX_COMMANDS; i++)
                g_clipboard[g_clipboard_count++] = g_cmds[i];
            char msg[64];
            snprintf(msg, sizeof(msg), "Copied %d line%s",
                     g_clipboard_count, g_clipboard_count > 1 ? "s" : "");
            set_status(msg);
        } else if (g_edit_line < g_num_cmds) {
            if (g_cmds[g_edit_line].type == CMD_FOR_BEGIN) {
                int fe = find_block_end(g_edit_line);
                int end_idx = (fe < g_num_cmds) ? fe + 1 : g_num_cmds;
                for (int i = g_edit_line; i < end_idx &&
                     g_clipboard_count < MAX_COMMANDS; i++)
                    g_clipboard[g_clipboard_count++] = g_cmds[i];
                char msg[64];
                snprintf(msg, sizeof(msg), "Copied for-loop (%d lines)",
                         g_clipboard_count);
                set_status(msg);
            } else {
                g_clipboard[0] = g_cmds[g_edit_line];
                g_clipboard_count = 1;
                set_status("Copied line");
            }
        }
        clear_selection();
        return;
    }

    /* Ctrl+X: cut line/selection (or whole for-loop if on FOR_BEGIN) */
    if (key == 24) {
        if (g_inserting) { clear_selection(); return; }
        g_clipboard_count = 0;
        int start, count;
        if (sel_active()) {
            start = sel_lo();
            int hi = sel_hi();
            if (hi >= g_num_cmds) hi = g_num_cmds - 1;
            count = hi - start + 1;
        } else if (g_edit_line < g_num_cmds) {
            start = g_edit_line;
            if (g_cmds[start].type == CMD_FOR_BEGIN) {
                int fe = find_block_end(start);
                count = ((fe < g_num_cmds) ? fe + 1 : g_num_cmds) - start;
            } else {
                count = 1;
            }
        } else {
            clear_selection();
            return;
        }
        push_undo_snapshot();
        for (int i = 0; i < count && g_clipboard_count < MAX_COMMANDS; i++)
            g_clipboard[g_clipboard_count++] = g_cmds[start + i];
        memmove(&g_cmds[start], &g_cmds[start + count],
                (g_num_cmds - start - count) * sizeof(GLCmd));
        g_num_cmds -= count;
        g_edit_line = start;
        if (g_edit_line > g_num_cmds) g_edit_line = g_num_cmds;
        load_line_to_input(g_edit_line);
        mark_normals_dirty();
        char msg[64];
        snprintf(msg, sizeof(msg), "Cut %d line%s",
                 count, count > 1 ? "s" : "");
        set_status(msg);
        clear_selection();
        return;
    }

    /* Ctrl+V: paste clipboard at current position */
    if (key == 22) {
        if (g_clipboard_count > 0) {
            if (g_num_cmds + g_clipboard_count > MAX_COMMANDS) {
                set_status("Command buffer full!");
                return;
            }
            push_undo_snapshot();
            int pos = g_inserting ? g_edit_line :
                      (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
            memmove(&g_cmds[pos + g_clipboard_count], &g_cmds[pos],
                    (g_num_cmds - pos) * sizeof(GLCmd));
            memcpy(&g_cmds[pos], g_clipboard,
                   g_clipboard_count * sizeof(GLCmd));
            g_num_cmds += g_clipboard_count;
            g_edit_line = pos + g_clipboard_count;
            g_inserting = 0;
            load_line_to_input(g_edit_line);
            mark_normals_dirty();
            char msg[64];
            snprintf(msg, sizeof(msg), "Pasted %d line%s",
                     g_clipboard_count, g_clipboard_count > 1 ? "s" : "");
            set_status(msg);
        } else {
            set_status("Clipboard empty");
        }
        return;
    }

    /* Ctrl+/: toggle comment on current line */
    if (key == '/' && glutGetModifiers() & GLUT_ACTIVE_CTRL) {
        if (g_edit_line < g_num_cmds && !g_inserting) {
            push_undo_snapshot();
            GLCmd *cur = &g_cmds[g_edit_line];
            if (cur->type == CMD_COMMENT) {
                /* Uncomment: strip // prefix and re-parse */
                const char *s = cur->source;
                while (*s && isspace((unsigned char)*s)) s++;
                if (s[0] == '/' && s[1] == '/') {
                    s += 2;
                    if (*s == ' ') s++;
                }
                GLCmd new_cmd;
                memset(&new_cmd, 0, sizeof(new_cmd));
                if (parse_command(s, &new_cmd)) {
                    g_cmds[g_edit_line] = new_cmd;
                    load_line_to_input(g_edit_line);
                    mark_normals_dirty();
                    set_status("Uncommented");
                } else {
                    set_status("Cannot uncomment: not a valid command");
                }
            } else if (cur->type != CMD_FOR_BEGIN &&
                       cur->type != CMD_FOR_END) {
                /* Comment out: prepend // to source, preserve indent */
                char new_src[MAX_LINE_LEN];
                const char *s = cur->source;
                int ind = 0;
                while (s[ind] && isspace((unsigned char)s[ind])) ind++;
                snprintf(new_src, sizeof(new_src), "%.*s// %s",
                         ind, s, s + ind);
                cur->type = CMD_COMMENT;
                cur->valid = 1;
                strncpy(cur->source, new_src, sizeof(cur->source) - 1);
                cur->source[sizeof(cur->source) - 1] = '\0';
                load_line_to_input(g_edit_line);
                mark_normals_dirty();
                set_status("Commented out");
            }
        }
        return;
    }

    /* Ctrl+B: toggle accumulation AA */
    if (key == 2) {
        if (g_use_accum) {
            g_accum_aa_enabled = !g_accum_aa_enabled;
            set_status(g_accum_aa_enabled ? "Accum AA: ON" : "Accum AA: OFF");
        } else {
            set_status("Accum buffer disabled (remove --noaccum to enable)");
        }
        return;
    }

    /* Ctrl+N: toggle GL_LINE_SMOOTH baseline state */
    if (key == 14) {
        g_line_smooth_enabled = !g_line_smooth_enabled;
        set_status(g_line_smooth_enabled ? "Line smooth: ON" : "Line smooth: OFF");
        return;
    }

    /* Ctrl+T: toggle time ('t' variable) play / pause */
    if (key == 20) {
        g_t_playing = !g_t_playing;
        set_status(g_t_playing ? "Time: playing" : "Time: paused (set 't' manually)");
        return;
    }

    /* Ctrl+U: toggle multisample baseline state */
    if (key == 21) {
        g_multisample_enabled = !g_multisample_enabled;
        set_status(g_multisample_enabled ? "MSAA: ON" : "MSAA: OFF");
        return;
    }

    /* Ctrl+= or Ctrl++: increase jitter sample count */
    if ((key == '=' || key == '+') && (glutGetModifiers() & GLUT_ACTIVE_CTRL)) {
        if (g_use_accum) {
            for (int i = 0; i < ACCUM_STEP_COUNT - 1; i++) {
                if (g_accum_samples <= g_accum_steps[i]) {
                    g_accum_samples = g_accum_steps[i + 1];
                    break;
                }
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "Accum samples: %d", g_accum_samples);
            set_status(msg);
        }
        return;
    }

    /* Ctrl+-: decrease jitter sample count */
    if (key == 31 || (key == '-' && (glutGetModifiers() & GLUT_ACTIVE_CTRL))) {
        if (g_use_accum) {
            for (int i = ACCUM_STEP_COUNT - 1; i > 0; i--) {
                if (g_accum_samples >= g_accum_steps[i]) {
                    g_accum_samples = g_accum_steps[i - 1];
                    break;
                }
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "Accum samples: %d", g_accum_samples);
            set_status(msg);
        }
        return;
    }

    /* Backspace: delete character before cursor */
    if (key == 8 || key == 127) {
        if (g_cursor_pos > 0 && g_input_len > 0) {
            memmove(&g_input[g_cursor_pos - 1], &g_input[g_cursor_pos],
                    g_input_len - g_cursor_pos + 1);
            g_input_len--;
            g_cursor_pos--;
            update_autocomplete();
        }
        return;
    }

    /* Tab: accept autocomplete */
    if (key == '\t') {
        if (g_ac_count > 0) {
            accept_autocomplete();
            update_autocomplete();
        }
        return;
    }

    /* Enter: accept autocomplete if active, otherwise insert new line */
    if (key == '\r' || key == '\n') {
        if (g_ac_count > 0) {
            accept_autocomplete();
            update_autocomplete();
            return;
        }

        /* On existing line: if unmodified, enter insert mode.
         * At column 0, insert before the current line; otherwise insert after. */
        if (!g_inserting && g_edit_line < g_num_cmds) {
            int unmodified = 0;
            {
                const char *s = g_cmds[g_edit_line].source;
                while (*s && isspace((unsigned char)*s)) s++;
                int slen = (int)strlen(s);
                while (slen > 0 && (s[slen-1] == ';' ||
                       isspace((unsigned char)s[slen-1])))
                    slen--;
                if ((slen == g_input_len &&
                     strncmp(g_input, s, slen) == 0) ||
                    g_input_len == 0)
                    unmodified = 1;
            }
            if (unmodified) {
                if (g_cursor_pos > 0)
                    g_edit_line++;
                g_inserting = 1;
                g_input[0] = '\0';
                g_input_len = 0;
                g_cursor_pos = 0;
                g_ac_count = 0;
                g_ac_ghost[0] = '\0';
                set_status("Insert mode");
                mark_normals_dirty();
                return;
            }
        }

        if (g_input_len > 0) push_undo_snapshot();

        /* Check for } closing a block (only for insert/new-line, not existing lines) */
        if ((g_inserting || g_edit_line >= g_num_cmds) &&
            g_input_len > 0 && try_commit_close_brace()) {
            g_ac_count = 0;
            g_ac_ghost[0] = '\0';
            return;
        }
        /* Check for for-loop (only for insert/new-line, not existing lines) */
        if ((g_inserting || g_edit_line >= g_num_cmds) &&
            g_input_len > 0 && try_commit_for_loop()) {
            g_ac_count = 0;
            g_ac_ghost[0] = '\0';
            return;
        }
        /* Check for func def (only for insert/new-line, not existing lines) */
        if ((g_inserting || g_edit_line >= g_num_cmds) &&
            g_input_len > 0 && try_commit_func_def()) {
            g_ac_count = 0;
            g_ac_ghost[0] = '\0';
            return;
        }
        /* Check for if-block (only for insert/new-line, not existing lines) */
        if ((g_inserting || g_edit_line >= g_num_cmds) &&
            g_input_len > 0 && try_commit_if_block()) {
            g_ac_count = 0;
            g_ac_ghost[0] = '\0';
            return;
        }
        if (g_inserting) {
            /* Already in insert mode */
            if (g_input_len > 0) {
                /* Commit insertion, stay in insert mode */
                GLCmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                int fpos = g_edit_line;
                int parsed;
                ExprVar dvars[MAX_EXPR_VARS];
                int dnv = collect_visible_vars(fpos, dvars, MAX_EXPR_VARS);
                if (dnv > 0) {
                    int saved_el = g_edit_line;
                    g_edit_line = fpos;
                    parsed = parse_command_with_vars(g_input, &cmd, dvars, dnv);
                    g_edit_line = saved_el;
                    if (parsed) {
                        char stripped[MAX_LINE_LEN];
                        const char *sp = g_input;
                        while (*sp && isspace((unsigned char)*sp)) sp++;
                        strncpy(stripped, sp, MAX_LINE_LEN - 1);
                        stripped[MAX_LINE_LEN - 1] = '\0';
                        int slen = (int)strlen(stripped);
                        while (slen > 0 && (stripped[slen-1] == ';' ||
                               isspace((unsigned char)stripped[slen-1])))
                            stripped[--slen] = '\0';
                        int fdepth = block_depth_at(fpos);
                        int bb_v = in_begin_block_at(fpos);
                        int ind_v = (bb_v ? 4 : 2) + fdepth * 2;
                        char indent_v[32];
                        if (ind_v > (int)sizeof(indent_v) - 1) ind_v = (int)sizeof(indent_v) - 1;
                        memset(indent_v, ' ', ind_v);
                        indent_v[ind_v] = '\0';
                        snprintf(cmd.source, sizeof(cmd.source), "%s%s;", indent_v, stripped);
                    }
                } else {
                    parsed = parse_command(g_input, &cmd);
                }
                if (parsed && g_num_cmds < MAX_COMMANDS) {
                    for (int j = g_num_cmds; j > g_edit_line; j--)
                        g_cmds[j] = g_cmds[j - 1];
                    g_cmds[g_edit_line] = cmd;
                    g_num_cmds++;
                    g_edit_line++;
                    g_input[0] = '\0';
                    g_input_len = 0;
                    g_cursor_pos = 0;
                    set_status("Inserted");
                }
                /* Parse failure: keep input, stay in insert mode */
            } else {
                /* Empty input: exit insert mode */
                g_inserting = 0;
                if (g_edit_line <= g_num_cmds)
                    load_line_to_input(g_edit_line);
            }
        } else if (g_edit_line < g_num_cmds) {
            /* On existing line with modified content — try to re-parse */
            int can_advance = 1;

            if (g_input_len > 0) {
                /* Block headers: delegate to their commit functions
                 * which know how to update in-place. */
                if (g_cmds[g_edit_line].type == CMD_FOR_BEGIN) {
                    if (try_commit_for_loop()) return;
                    can_advance = 0;
                }
                if (g_cmds[g_edit_line].type == CMD_FUNC_DEF) {
                    if (try_commit_func_def()) return;
                    can_advance = 0;
                }
                if (g_cmds[g_edit_line].type == CMD_IF_BEGIN) {
                    if (try_commit_if_block()) return;
                    can_advance = 0;
                }

                GLCmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                int fpos = g_edit_line;
                int parsed = 0;
                ExprVar dvars[MAX_EXPR_VARS];
                int dnv = collect_visible_vars(fpos, dvars, MAX_EXPR_VARS);
                if (dnv > 0) {
                    int saved_el = g_edit_line;
                    g_edit_line = fpos;
                    parsed = parse_command_with_vars(g_input, &cmd, dvars, dnv);
                    g_edit_line = saved_el;
                    if (parsed) {
                        char stripped[MAX_LINE_LEN];
                        const char *sp = g_input;
                        while (*sp && isspace((unsigned char)*sp)) sp++;
                        strncpy(stripped, sp, MAX_LINE_LEN - 1);
                        stripped[MAX_LINE_LEN - 1] = '\0';
                        int slen = (int)strlen(stripped);
                        while (slen > 0 && (stripped[slen-1] == ';' ||
                               isspace((unsigned char)stripped[slen-1])))
                            stripped[--slen] = '\0';
                        int fdepth = block_depth_at(fpos);
                        int bb_v = in_begin_block_at(fpos);
                        int ind_v = (bb_v ? 4 : 2) + fdepth * 2;
                        char indent_v[32];
                        if (ind_v > (int)sizeof(indent_v) - 1) ind_v = (int)sizeof(indent_v) - 1;
                        memset(indent_v, ' ', ind_v);
                        indent_v[ind_v] = '\0';
                        snprintf(cmd.source, sizeof(cmd.source), "%s%s;", indent_v, stripped);
                    }
                } else {
                    /* Try var assignment first */
                    if (try_assign_variable()) {
                        /* try_assign_variable already updated g_cmds and
                         * advanced g_edit_line — just enter insert mode */
                        g_inserting = 1;
                        g_input[0] = '\0';
                        g_input_len = 0;
                        g_cursor_pos = 0;
                        g_ac_count = 0;
                        g_ac_ghost[0] = '\0';
                        set_status("Insert mode");
                        mark_normals_dirty();
                        return;
                    }
                    parsed = parse_command(g_input, &cmd);
                    if (parsed && input_has_predef_vars(g_input)) {
                        cmd.has_vars = 1;
                        char stripped[MAX_LINE_LEN];
                        const char *sp = g_input;
                        while (*sp && isspace((unsigned char)*sp)) sp++;
                        strncpy(stripped, sp, MAX_LINE_LEN - 1);
                        stripped[MAX_LINE_LEN - 1] = '\0';
                        int slen = (int)strlen(stripped);
                        while (slen > 0 && (stripped[slen-1] == ';' ||
                               isspace((unsigned char)stripped[slen-1])))
                            stripped[--slen] = '\0';
                        int bb_v = in_begin_block_at(fpos);
                        int ind_v = bb_v ? 4 : 2;
                        char indent_v[32];
                        if (ind_v > (int)sizeof(indent_v) - 1) ind_v = (int)sizeof(indent_v) - 1;
                        memset(indent_v, ' ', ind_v);
                        indent_v[ind_v] = '\0';
                        snprintf(cmd.source, sizeof(cmd.source), "%s%s;", indent_v, stripped);
                    }
                }
                if (parsed) {
                    g_cmds[g_edit_line] = cmd;
                } else {
                    can_advance = 0;
                }
            }
            /* else: empty input — keep existing line as-is */

            if (can_advance) {
                g_edit_line++;
                g_inserting = 1;
                g_input[0] = '\0';
                g_input_len = 0;
                g_cursor_pos = 0;
                set_status("Insert mode");
            }
        } else {
            /* On new-line slot: commit if content */
            if (g_input_len > 0) {
                GLCmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                int fpos = g_num_cmds;
                int parsed;
                ExprVar dvars[MAX_EXPR_VARS];
                int dnv = collect_visible_vars(fpos, dvars, MAX_EXPR_VARS);
                if (dnv > 0) {
                    int saved_el = g_edit_line;
                    g_edit_line = fpos;
                    parsed = parse_command_with_vars(g_input, &cmd, dvars, dnv);
                    g_edit_line = saved_el;
                    if (parsed) {
                        char stripped[MAX_LINE_LEN];
                        const char *sp = g_input;
                        while (*sp && isspace((unsigned char)*sp)) sp++;
                        strncpy(stripped, sp, MAX_LINE_LEN - 1);
                        stripped[MAX_LINE_LEN - 1] = '\0';
                        int slen = (int)strlen(stripped);
                        while (slen > 0 && (stripped[slen-1] == ';' ||
                               isspace((unsigned char)stripped[slen-1])))
                            stripped[--slen] = '\0';
                        int fdepth = block_depth_at(fpos);
                        int bb_v = in_begin_block_at(fpos);
                        int ind_v = (bb_v ? 4 : 2) + fdepth * 2;
                        char indent_v[32];
                        if (ind_v > (int)sizeof(indent_v) - 1) ind_v = (int)sizeof(indent_v) - 1;
                        memset(indent_v, ' ', ind_v);
                        indent_v[ind_v] = '\0';
                        snprintf(cmd.source, sizeof(cmd.source), "%s%s;", indent_v, stripped);
                    }
                } else {
                    parsed = parse_command(g_input, &cmd);
                    if (parsed && input_has_predef_vars(g_input)) {
                        cmd.has_vars = 1;
                        char stripped[MAX_LINE_LEN];
                        const char *sp = g_input;
                        while (*sp && isspace((unsigned char)*sp)) sp++;
                        strncpy(stripped, sp, MAX_LINE_LEN - 1);
                        stripped[MAX_LINE_LEN - 1] = '\0';
                        int slen = (int)strlen(stripped);
                        while (slen > 0 && (stripped[slen-1] == ';' ||
                               isspace((unsigned char)stripped[slen-1])))
                            stripped[--slen] = '\0';
                        int bb_v = in_begin_block_at(fpos);
                        int ind_v = bb_v ? 4 : 2;
                        char indent_v[32];
                        if (ind_v > (int)sizeof(indent_v) - 1) ind_v = (int)sizeof(indent_v) - 1;
                        memset(indent_v, ' ', ind_v);
                        indent_v[ind_v] = '\0';
                        snprintf(cmd.source, sizeof(cmd.source), "%s%s;", indent_v, stripped);
                    }
                }
                if (parsed && g_num_cmds < MAX_COMMANDS) {
                    g_cmds[g_num_cmds++] = cmd;
                    g_edit_line = g_num_cmds;
                    g_input[0] = '\0';
                    g_input_len = 0;
                    g_cursor_pos = 0;
                    g_newline_buf[0] = '\0';
                    g_newline_len = 0;
                    set_status("OK");
                }
            }
        }
        g_ac_count = 0;
        g_ac_ghost[0] = '\0';
        mark_normals_dirty();
        return;
    }

    /* Semicolon: parse and commit */
    if (key == ';') {
        if (g_input_len > 0) {
            push_undo_snapshot();
            /* Check for variable assignment (x = expr) */
            if (try_assign_variable()) {
                g_ac_count = 0;
                g_ac_ghost[0] = '\0';
                return;
            }
            /* Check for } closing a loop block */
            if (try_commit_close_brace()) {
                g_ac_count = 0;
                g_ac_ghost[0] = '\0';
                return;
            }
            /* Check for for-loop */
            if (try_commit_for_loop()) {
                g_ac_count = 0;
                g_ac_ghost[0] = '\0';
                return;
            }
            /* Check for func def */
            if (try_commit_func_def()) {
                g_ac_count = 0;
                g_ac_ghost[0] = '\0';
                return;
            }
            /* Check for if-block */
            if (try_commit_if_block()) {
                g_ac_count = 0;
                g_ac_ghost[0] = '\0';
                return;
            }
            GLCmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            int fpos = g_inserting ? g_edit_line :
                       (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
            int parsed;
            ExprVar dvars[MAX_EXPR_VARS];
            int dnv = collect_visible_vars(fpos, dvars, MAX_EXPR_VARS);
            if (dnv > 0) {
                parsed = repl_parse_and_normalize(g_input, fpos, dvars, dnv, 1, &cmd);
            } else {
                int preserve = input_has_predef_vars(g_input);
                parsed = repl_parse_and_normalize(g_input, fpos, NULL, 0, preserve, &cmd);
            }
            if (parsed) {
                if (g_inserting) {
                    /* Insert at current position */
                    if (g_num_cmds < MAX_COMMANDS) {
                        for (int j = g_num_cmds; j > g_edit_line; j--)
                            g_cmds[j] = g_cmds[j - 1];
                        g_cmds[g_edit_line] = cmd;
                        g_num_cmds++;
                        g_edit_line++;
                        /* Stay in insert mode */
                        g_input[0] = '\0';
                        g_input_len = 0;
                        g_cursor_pos = 0;
                        set_status("Inserted");
                    } else {
                        set_status("Command buffer full!");
                    }
                } else if (g_edit_line < g_num_cmds) {
                    /* Replace existing line */
                    g_cmds[g_edit_line] = cmd;
                    set_status("Line updated");
                    /* Advance to next line */
                    g_edit_line++;
                    load_line_to_input(g_edit_line);
                } else {
                    /* Append new command */
                    if (g_num_cmds < MAX_COMMANDS) {
                        g_cmds[g_num_cmds++] = cmd;
                        g_edit_line = g_num_cmds;
                        set_status("OK");
                        g_input[0] = '\0';
                        g_input_len = 0;
                        g_cursor_pos = 0;
                        g_newline_buf[0] = '\0';
                        g_newline_len = 0;
                    } else {
                        set_status("Command buffer full!");
                    }
                }
            }
            /* On parse failure, leave input for the user to fix */
        }
        g_ac_count = 0;
        g_ac_ghost[0] = '\0';
        mark_normals_dirty();
        return;
    }

    if (key == 0x11) { /* Ctrl+Q: quit */
        save_output(tempfile);
        printf("Saved to %s\n", tempfile);
        exit(0);
    }

    /* Printable character: insert at cursor position */
    if (key >= 32 && key < 127 && g_input_len < MAX_INPUT_LEN - 2) {
        memmove(&g_input[g_cursor_pos + 1], &g_input[g_cursor_pos],
                g_input_len - g_cursor_pos + 1);
        g_input[g_cursor_pos] = (char)key;
        g_input_len++;
        g_cursor_pos++;
        update_autocomplete();
    }
}

static void special_func(int key, int x, int y) {
    (void)x; (void)y;

    /* Reset cursor blink on navigation */
    g_cursor_on = 1;
    g_blink_tick = 0;

    if (g_replay_active) {
        if ((g_replay_state == REPLAY_PAUSED || g_replay_state == REPLAY_DONE) &&
            key == GLUT_KEY_LEFT) {
            replay_step_back();
            return;
        }
        if (g_replay_state == REPLAY_PAUSED && key == GLUT_KEY_RIGHT) {
            replay_advance();
            return;
        }
        replay_stop();
    }

    if (handle_search_special(key))
        return;

    switch (key) {
    /* Cursor movement within line */
    case GLUT_KEY_LEFT:
        if (g_show_help) {
            if (g_help_tab > 0) { g_help_tab--; g_help_scroll = 0; }
            break;
        }
        if (g_cursor_pos > 0) g_cursor_pos--;
        update_autocomplete();
        break;
    case GLUT_KEY_RIGHT:
        if (g_show_help) {
            if (g_help_tab < 1) { g_help_tab++; g_help_scroll = 0; }
            break;
        }
        if (g_cursor_pos < g_input_len) g_cursor_pos++;
        update_autocomplete();
        break;
    case GLUT_KEY_HOME:
        g_cursor_pos = 0;
        update_autocomplete();
        break;
    case GLUT_KEY_END:
        g_cursor_pos = g_input_len;
        update_autocomplete();
        break;

    /* Line navigation */
    case GLUT_KEY_UP:
        if (g_show_help) {
            g_help_scroll--;
            break;
        }
        if (g_ac_count > 1) {
            /* Navigate autocomplete popup */
            g_ac_sel = (g_ac_sel - 1 + g_ac_count) % g_ac_count;
            /* Update ghost to selected match */
            const char *m = g_ac_matches[g_ac_sel];
            /* Find the prefix length inside parentheses for enum completions */
            char *paren = strchr(g_input, '(');
            if (paren && (int)(paren - g_input + 1) < g_input_len) {
                int alen = g_input_len - (int)(paren - g_input + 1);
                snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s)", m + alen);
            } else {
                snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s",
                         m + g_input_len);
            }
        } else if (glutGetModifiers() & GLUT_ACTIVE_SHIFT) {
            /* Shift+Up: extend selection */
            if (!sel_active()) {
                g_sel_anchor = g_edit_line;
                g_sel_end = g_edit_line;
            }
            if (g_sel_end > 0) g_sel_end--;
            navigate_to_line(g_sel_end);
        } else {
            clear_selection();
            navigate_to_line(g_edit_line - 1);
        }
        break;

    case GLUT_KEY_DOWN:
        if (g_show_help) {
            g_help_scroll++;
            break;
        }
        if (g_ac_count > 1) {
            g_ac_sel = (g_ac_sel + 1) % g_ac_count;
            const char *m2 = g_ac_matches[g_ac_sel];
            char *paren2 = strchr(g_input, '(');
            if (paren2 && (int)(paren2 - g_input + 1) < g_input_len) {
                int alen = g_input_len - (int)(paren2 - g_input + 1);
                snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s)", m2 + alen);
            } else {
                snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s",
                         m2 + g_input_len);
            }
        } else if (glutGetModifiers() & GLUT_ACTIVE_SHIFT) {
            /* Shift+Down: extend selection */
            if (!sel_active()) {
                g_sel_anchor = g_edit_line;
                g_sel_end = g_edit_line;
            }
            if (g_sel_end < g_num_cmds - 1) g_sel_end++;
            navigate_to_line(g_sel_end);
        } else {
            clear_selection();
            navigate_to_line(g_edit_line + 1);
        }
        break;

    /* Toggle keys */
    case GLUT_KEY_F1:
        g_show_help = !g_show_help;
        g_help_tab = 0;
        g_help_scroll = 0;
        break;
    case GLUT_KEY_F2:
        g_wireframe = !g_wireframe;
        set_status(g_wireframe ? "Wireframe ON" : "Wireframe OFF"); break;
    case GLUT_KEY_F3:
        g_grid_theme = (g_grid_theme + 1) % GRID_THEME_COUNT;
        set_status(g_grid_names[g_grid_theme]); break;
    case GLUT_KEY_F4:
        g_axes_theme = (g_axes_theme + 1) % AXES_THEME_COUNT;
        set_status(g_axes_names[g_axes_theme]); break;
    case GLUT_KEY_F5:
        g_show_vnums = !g_show_vnums;
        set_status(g_show_vnums ? "Vertex numbers ON" : "Vertex numbers OFF");
        break;
    case GLUT_KEY_F6:
        g_show_normals = !g_show_normals;
        set_status(g_show_normals ? "Normal vectors ON" :
                   "Normal vectors OFF");
        break;
    case GLUT_KEY_F7:
        g_show_outlines = !g_show_outlines;
        set_status(g_show_outlines ? "Vertex outlines ON" : "Vertex outlines OFF");
        break;
    case GLUT_KEY_F8:
        g_show_guides = !g_show_guides;
        set_status(g_show_guides ? "Vertex guides ON" :
                   "Vertex guides OFF");
        break;
    case GLUT_KEY_F9:
        g_autonormal = !g_autonormal;
        if (g_autonormal) {
            mark_normals_dirty();
            set_status("Auto-normals ON");
        } else {
            set_status("Auto-normals OFF (existing normals kept)");
        }
        break;
    case GLUT_KEY_F10:
        g_show_lights = !g_show_lights;
        set_status(g_show_lights ? "Light indicators ON" :
                   "Light indicators OFF");
        break;
    case GLUT_KEY_F11:
        g_cam_rotate = !g_cam_rotate;
        set_status(g_cam_rotate ? "Camera rotate ON" : "Camera rotate OFF");
        break;
    case GLUT_KEY_F12:
        if (repl_examples_count() > 0)
            load_example((g_example_idx + 1) % repl_examples_count());
        break;

    /* Scroll */
    case GLUT_KEY_PAGE_UP:
        if (g_show_help) g_help_scroll -= 5;
        else g_scroll -= 5;
        break;
    case GLUT_KEY_PAGE_DOWN:
        if (g_show_help) g_help_scroll += 5;
        else g_scroll += 5;
        break;
    default: break;
    }

}

static void mouse_func(int button, int state, int x, int y) {
    /* Release: end any variable drag or panel resize */
    if (state == GLUT_UP) {
        handle_code_panel_release();
        if (g_drag_var >= 0) {
            g_drag_var = -1;
            glutPostRedisplay();
            return;
        }
        if (g_resizing_panel) {
            g_resizing_panel = 0;
            glutSetCursor(GLUT_CURSOR_INHERIT);
            glutPostRedisplay();
            return;
        }
    }

    if (button == GLUT_LEFT_BUTTON && state == GLUT_DOWN) {
        /* Config menu click: toggle the clicked item */
        if (g_show_config) {
            int row = cfg_hit_row(x, y);
            if (row >= 0) {
                if (g_cfg_items[row].value == &g_replay_active) {
                    if (g_replay_active) {
                        replay_stop();
                        set_status("Replay: off");
                    } else {
                        replay_start();
                    }
                } else {
                    if (g_replay_active)
                        replay_stop();
                    *g_cfg_items[row].value =
                        (*g_cfg_items[row].value + 1) % g_cfg_items[row].n_states;
                    if (g_cfg_items[row].value == &g_layout_vertical) {
                        g_panel_frac = 0.3f;
                        set_status(g_layout_vertical ? "Layout: top code panel"
                                                     : "Layout: left code panel");
                    }
                    if (g_cfg_items[row].value == &g_wrap_at_comma)
                        set_status(g_wrap_at_comma ? "Wrap at commas: ON"
                                                   : "Wrap at commas: OFF");
                    if (g_cfg_items[row].value == &g_autonormal && g_autonormal)
                        mark_normals_dirty();
                    if (g_cfg_items[row].value == &g_replay_mode)
                        set_status(g_replay_mode == REPLAY_MODE_VERTEX
                                 ? "Replay: vertex mode"
                                 : "Replay: polygon mode");
                }
                glutPostRedisplay();
                return;
            }
            /* Click outside config panel closes it */
            g_show_config = 0;
            glutPostRedisplay();
            return;
        }

        /* Variable panel drag start */
        if (g_show_var_panel) {
            int row;
            if (var_panel_hit(x, y, &row)) {
                if (g_replay_active)
                    replay_stop();
                g_drag_var       = row;
                g_drag_start_val = g_predef_vars[row].value;
                g_drag_start_x   = x;
                glutPostRedisplay();
                return;
            }
        }

        /* Left-click in code panel: navigate to line + column */
        if (g_layout_vertical) {
            int panel_h_px = (int)(g_win_h * g_panel_frac);
            if (abs(y - panel_h_px) < 10) {
                g_resizing_panel = 1;
                glutSetCursor(GLUT_CURSOR_UP_DOWN);
                return;
            }
            if (y < panel_h_px) {
                handle_code_panel_press(x, y);
                glutPostRedisplay();
                return;
            }
        } else {
            int panel_w = (int)(g_win_w * g_panel_frac);
            if (abs(x - panel_w) < 10) {
                g_resizing_panel = 1;
                glutSetCursor(GLUT_CURSOR_LEFT_RIGHT);
                return;
            }
            if (x < panel_w) {
                handle_code_panel_press(x, y);
                glutPostRedisplay();
                return;   /* don't start camera drag */
            }
        }
    }

    if (state == GLUT_DOWN) {
        g_mouse_btn = button;
        g_mouse_x = x;
        g_mouse_y = y;
        /* Cancel any coasting so a fresh grab starts clean */
        g_vel_ry = g_vel_rx = g_vel_px = g_vel_py = g_vel_zoom = 0.0f;
    } else {
        g_vel_ry = fabsf(g_vel_ry) > CAM_MOMENTUM_THRESHOLD ? g_vel_ry : 0.0f;
        g_vel_rx = fabsf(g_vel_rx) > CAM_MOMENTUM_THRESHOLD ? g_vel_rx : 0.0f;
        g_vel_px = fabsf(g_vel_px) > CAM_MOMENTUM_THRESHOLD ? g_vel_px : 0.0f;
        g_vel_py = fabsf(g_vel_py) > CAM_MOMENTUM_THRESHOLD ? g_vel_py : 0.0f;
        g_vel_zoom = fabsf(g_vel_zoom) > CAM_MOMENTUM_THRESHOLD ? g_vel_zoom : 0.0f;
        g_mouse_btn = -1;
    }

#ifdef USE_GLUT
    /* Apple GLUT reports scroll wheel as button 3/4 */
    if (button == 3 && state == GLUT_DOWN) {
        if (g_show_help) {
            g_help_scroll--;
        } else {
            int in_code_panel = g_layout_vertical
                ? (y < (int)(g_win_h * g_panel_frac))
                : (x < (int)(g_win_w * g_panel_frac));
            if (in_code_panel) g_scroll--;
            else g_vel_zoom -= 0.3f;
        }
        glutPostRedisplay();
    } else if (button == 4 && state == GLUT_DOWN) {
        if (g_show_help) {
            g_help_scroll++;
        } else {
            int in_code_panel = g_layout_vertical
                ? (y < (int)(g_win_h * g_panel_frac))
                : (x < (int)(g_win_w * g_panel_frac));
            if (in_code_panel) g_scroll++;
            else g_vel_zoom += 0.3f;
        }
        glutPostRedisplay();
    }
#endif
}

#ifndef USE_GLUT
/* FreeGLUT mouse wheel callback */
static void mousewheel_func(int wheel, int direction, int x, int y) {
    (void)wheel;
    if (g_show_help) {
        /* direction > 0 = scroll up (towards top of help) */
        g_help_scroll -= direction;
    } else {
        int in_code_panel = g_layout_vertical
            ? (y < (int)(g_win_h * g_panel_frac))
            : (x < (int)(g_win_w * g_panel_frac));
        if (in_code_panel) {
            g_scroll -= direction;
        } else {
            /* direction > 0 = wheel up = zoom in */
            g_vel_zoom -= direction * 0.1f;
        }
    }
    glutPostRedisplay();
}
#endif

static void passive_motion_func(int x, int y) {
    g_mouse_x = x;
    g_mouse_y = y;
    /* Update config menu hover */
    if (g_show_config) {
        int prev = g_config_hover;
        g_config_hover = cfg_hit_row(x, y);
        if (g_config_hover != prev) glutPostRedisplay();
    }

    if (g_layout_vertical) {
        int panel_h_px = (int)(g_win_h * g_panel_frac);
        if (abs(y - panel_h_px) < 10) {
            glutSetCursor(GLUT_CURSOR_UP_DOWN);
        } else {
            glutSetCursor(GLUT_CURSOR_INHERIT);
        }
    } else {
        int panel_w = (int)(g_win_w * g_panel_frac);
        if (abs(x - panel_w) < 10) {
            glutSetCursor(GLUT_CURSOR_LEFT_RIGHT);
        } else {
            glutSetCursor(GLUT_CURSOR_INHERIT);
        }
    }
}

static void motion_func(int x, int y) {
    int dx = x - g_mouse_x;
    int dy = y - g_mouse_y;

    if (g_resizing_panel) {
        if (g_layout_vertical) {
            /* GLUT y=0 is at top; y directly maps to panel height fraction. */
            g_panel_frac = (float)y / (float)g_win_h;
        } else {
            g_panel_frac = (float)x / (float)g_win_w;
        }
        if (g_panel_frac < 0.1f) g_panel_frac = 0.1f;
        if (g_panel_frac > 0.9f) g_panel_frac = 0.9f;
        glutPostRedisplay();
        return;
    }

    /* Variable drag */
    if (g_drag_var >= 0) {
        float delta = (float)(x - g_drag_start_x) * 0.05f;
        float new_val = g_drag_start_val + delta;
        g_predef_vars[g_drag_var].value = new_val;
        /* Update any CMD_VAR_ASSIGN commands for this variable so that
         * flatten_range doesn't overwrite the slider value on the next frame. */
        const char *vname = g_predef_vars[g_drag_var].name;
        for (int i = 0; i < g_num_cmds; i++) {
            if (g_cmds[i].valid && g_cmds[i].type == CMD_VAR_ASSIGN &&
                g_cmds[i].num_args == g_drag_var &&
                !g_cmds[i].has_vars) {
                g_cmds[i].args[0] = new_val;
                snprintf(g_cmds[i].source, sizeof(g_cmds[i].source),
                         "  %s = %g;", vname, (double)new_val);
            }
        }
        g_flat_dirty = 1;
        g_mouse_x = x; g_mouse_y = y;
        glutPostRedisplay();
        return;
    }

    if (handle_code_panel_drag(x, y)) {
        g_mouse_x = x;
        g_mouse_y = y;
        glutPostRedisplay();
        return;
    }

    if (g_mouse_btn == GLUT_LEFT_BUTTON) {
        /* Direct: responsive while held */
        g_cam_ry += (float)dx * 0.5f;
        g_cam_rx += (float)dy * 0.5f;
        if (g_cam_rx >  89.0f) g_cam_rx =  89.0f;
        if (g_cam_rx < -89.0f) g_cam_rx = -89.0f;
        /* Accumulate: feeds the coast after release */
        g_vel_rx *= CAM_DECAY;
        g_vel_ry *= CAM_DECAY;
        g_vel_ry += (float)dx * 0.25f;
        g_vel_rx += (float)dy * 0.25f;
    } else if (g_mouse_btn == GLUT_RIGHT_BUTTON) {
        g_cam_px += (float)dx * 0.01f;
        g_cam_py -= (float)dy * 0.01f;
    } else if (g_mouse_btn == GLUT_MIDDLE_BUTTON) {
        g_cam_dist += (float)dy * 0.02f;
        if (g_cam_dist < 0.5f)  g_cam_dist = 0.5f;
        if (g_cam_dist > 50.0f) g_cam_dist = 50.0f;
    }

    g_mouse_x = x;
    g_mouse_y = y;
}

static void timer_func(int value) {
    (void)value;

    g_anim_time += 0.016f;  /* ~60 fps */

    /* Drive predefined 't' variable with elapsed time when playing */
    if (g_t_playing && g_t_var_idx >= 0) {
        g_predef_vars[g_t_var_idx].value = g_anim_time;
        g_flat_dirty = 1;   /* re-flatten so expressions referencing t update */
    }

    if (g_replay_active)
        replay_tick_fade_batches(0.016f);

    if (g_replay_active && g_replay_state == REPLAY_PLAYING) {
        g_replay_accum += g_replay_speed * 0.016f;
        while (g_replay_accum >= 1.0f && g_replay_state == REPLAY_PLAYING) {
            g_replay_accum -= 1.0f;
            replay_advance();
        }
    }

    /* Apply momentum only while no button is held (coast after release).
     * Velocity always decays so it drains cleanly whether or not it drives. */
    if (g_mouse_btn == -1) {
        g_cam_ry += g_vel_ry;
        g_cam_rx += g_vel_rx;
        if (g_cam_rx >  89.0f) { g_cam_rx =  89.0f; g_vel_rx = 0.0f; }
        if (g_cam_rx < -89.0f) { g_cam_rx = -89.0f; g_vel_rx = 0.0f; }

        g_cam_px   += g_vel_px;
        g_cam_py   += g_vel_py;

        g_cam_dist += g_vel_zoom;
        if (g_cam_dist < 0.5f)  { g_cam_dist = 0.5f;  g_vel_zoom = 0.0f; }
        if (g_cam_dist > 50.0f) { g_cam_dist = 50.0f; g_vel_zoom = 0.0f; }
    }

    g_vel_ry   *= CAM_DECAY;
    g_vel_rx   *= CAM_DECAY;
    g_vel_px   *= CAM_DECAY;
    g_vel_py   *= CAM_DECAY;
    g_vel_zoom *= CAM_DECAY_ZOOM;

    /* Auto-rotate bypasses momentum (constant angular rate) */
    if (g_cam_rotate)
        g_cam_ry += 0.3f;

    g_blink_tick++;
    if (g_blink_tick >= 30) {
        g_blink_tick = 0;
        g_cursor_on = !g_cursor_on;
    }

    if (g_status_ttl > 0) g_status_ttl--;

    glutPostRedisplay();
    glutTimerFunc(16, timer_func, 0);
}

/* ========================================================================= */
/* Predefined examples (F12 to cycle)                                         */
/* ========================================================================= */

/* Helper: feed one line through the commit pipeline, as if the user typed it
 * and pressed ';'.  This reuses the existing try_commit_* functions so that
 * source strings, indentation, has_vars, and block nesting are handled
 * identically to interactive use. */
static int feed_line(const char *line) {
    strncpy(g_input, line, MAX_INPUT_LEN - 1);
    g_input[MAX_INPUT_LEN - 1] = '\0';
    g_input_len = (int)strlen(g_input);
    g_cursor_pos = g_input_len;

    /* Try structured blocks first (order matters) */
    if (try_commit_close_brace()) return 1;
    if (try_commit_for_loop()) return 1;
    if (try_commit_func_def()) return 1;
    if (try_commit_if_block()) return 1;
    if (try_assign_variable()) return 1;

    /* Regular command */
    int handled = 0;
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    int fpos = g_inserting ? g_edit_line :
               (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
    int parsed;
    ExprVar dvars[MAX_EXPR_VARS];
    int dnv = collect_visible_vars(fpos, dvars, MAX_EXPR_VARS);
    if (dnv > 0) {
        parsed = repl_parse_and_normalize(g_input, fpos, dvars, dnv, 1, &cmd);
    } else {
        int preserve = input_has_predef_vars(g_input);
        parsed = repl_parse_and_normalize(g_input, fpos, NULL, 0, preserve, &cmd);
    }
    if (parsed && g_num_cmds < MAX_COMMANDS) {
        if (g_inserting) {
            /* Insert at g_edit_line (inside a block) */
            for (int j = g_num_cmds; j > g_edit_line; j--)
                g_cmds[j] = g_cmds[j - 1];
            g_cmds[g_edit_line] = cmd;
            g_num_cmds++;
            g_edit_line++;
        } else if (g_edit_line < g_num_cmds) {
            /* Replace the current placeholder/end line, like interactive edit mode */
            g_cmds[g_edit_line] = cmd;
            g_edit_line++;
        } else {
            g_cmds[g_num_cmds++] = cmd;
            g_edit_line = g_num_cmds;
        }
        depth_cache_invalidate();
        handled = 1;
    }
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    return handled;
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

static void load_initial_commands(const char *import_file) {
    /* Try importing from file first */
    if (import_file && load_from_file(import_file)) {
        g_edit_line = g_num_cmds;
        return;
    }

    /* Fall back to default example (cube) */
    load_example(0);
    set_status("Ready - type GL commands, press ; to execute. F1 for help. F12 for examples.");
}

/* GLU tessellator callbacks for explicit gluBegin/gluEnd tessellation */
static void tess_vertex_callback(void *vertex_data) {
    TessVertex *v = (TessVertex *)vertex_data;
    glNormal3dv(v->normal);
    glColor4dv(v->color);
    glVertex3dv(v->pos);
}

static void tess_combine_callback(GLdouble coords[3],
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

static void tess_error_callback(GLenum err) {
    (void)err; /* silently ignore tessellation errors */
}

static void init_gl(void) {
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    GLfloat lm_amb[] = { 0.15f, 0.15f, 0.20f, 1.0f };
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);

    /* Init GLU quadric for gluSphere/gluCylinder/gluDisk */
    g_quadric = gluNewQuadric();
    gluQuadricNormals(g_quadric, GLU_SMOOTH);
    gluQuadricTexture(g_quadric, GL_FALSE);

    /* Init GLU tessellator for concave polygon support */
    g_tess = gluNewTess();
    gluTessCallback(g_tess, GLU_TESS_BEGIN,
                    (void (*)())glBegin);
    gluTessCallback(g_tess, GLU_TESS_END,
                    (void (*)())glEnd);
    gluTessCallback(g_tess, GLU_TESS_VERTEX,
                    (void (*)())tess_vertex_callback);
    gluTessCallback(g_tess, GLU_TESS_COMBINE,
                    (void (*)())tess_combine_callback);
    gluTessCallback(g_tess, GLU_TESS_ERROR,
                    (void (*)())tess_error_callback);

    glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, (GLfloat[]){ 1.0f, 0.0f, 0.02f });
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

void repl_navigate_to_line(int target) {
    navigate_to_line(target);
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

void repl_keyboard_func(unsigned char key, int x, int y) {
    keyboard_func(key, x, y);
}

void repl_special_func(int key, int x, int y) {
    special_func(key, x, y);
}

void repl_mouse_func(int button, int state, int x, int y) {
    mouse_func(button, state, x, y);
}

void repl_motion_func(int x, int y) {
    motion_func(x, y);
}

void repl_passive_motion_func(int x, int y) {
    passive_motion_func(x, y);
}

#ifndef USE_GLUT
void repl_mousewheel_func(int wheel, int direction, int x, int y) {
    mousewheel_func(wheel, direction, x, y);
}
#endif

void repl_timer_func(int value) {
    timer_func(value);
}

void repl_init_gl(void) {
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
    g_wrap_at_comma = 0;
    g_layout_vertical = 0;
    g_panel_frac = 0.42f;
    g_flat_dirty = 1;
    g_normals_dirty = 1;
    g_ac_count = 0;
    g_ac_sel = 0;
    g_ac_ghost[0] = '\0';
    search_clear_all();
    update_render_state_strings();
    depth_cache_invalidate();
    clear_selection();
}

void repl_feed_line_public(const char *line) {
    feed_line(line);
}
