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
 * Math expressions: sin, cos, tan, sqrt, abs, pow, min, max, PI, TAU
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
 *   func0()                    Call function 0
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
 *   Ctrl+C         Copy line/selection (whole for-loop on FOR_BEGIN)
 *   Ctrl+X         Cut line/selection (whole for-loop on FOR_BEGIN)
 *   Ctrl+V         Paste before current line
 *   Ctrl+Z         Undo last command
 *   Ctrl+D         Delete line at cursor
 *   Ctrl+L         Clear all commands
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
 *   Ctrl+A         Toggle accumulation-buffer AA
 *   Ctrl+=         Increase AA jitter samples (1→2→4→8→16)
 *   Ctrl+-         Decrease AA jitter samples (16→8→4→2→1)
 *
 * Command-line flags:
 *   --noaccum      Disable accumulation buffer (enabled by default)
 *
 * Import/Export:
 *   Ctrl+S saves to output.c with snippet markers.
 *   Run ./sample output.c to reload a saved session.
 */

#include "sample.h"
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
    "glTranslatef(",
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
    "static int   g_rotating = 1;",
    "static GLUquadric *g_quadric = NULL;",
    "",
    "void display() {",
    "  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "  glLoadIdentity();",
    NULL
};

char g_lookat[3][128] = {
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
    "  if (g_rotating) { g_angle += 0.5f; glutPostRedisplay(); }",
    "}",
    "",
    "void init() {",
    "  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);",
    "  glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);",
    "  g_quadric = gluNewQuadric();",
    "  gluQuadricNormals(g_quadric, GLU_SMOOTH);",
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
GLCmd  g_flat_cmds[MAX_COMMANDS];
int    g_num_flat_cmds = 0;
int    g_flat_dirty = 1;
void mark_normals_dirty(void) { g_normals_dirty = 1; g_flat_dirty = 1; }

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
int    g_scroll = 0;

/* Accumulation buffer — enabled by default, disabled with --noaccum.
 * Designed to be forward-compatible with FBO-based accumulation later. */
int    g_use_accum        = 1;  /* GLUT_ACCUM requested at init */
int    g_accum_aa_enabled = 1;  /* Ctrl+A toggles jitter AA on/off */
int    g_accum_samples    = 4;  /* current sample count */
float  g_accum_jitter_x   = 0.0f;
float  g_accum_jitter_y   = 0.0f;

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
int    g_show_normals = 1;
int    g_show_indices = 1;
int    g_show_guides  = 1;
int    g_autonormal   = 1;
int    g_show_lights  = 1;
int    g_cam_rotate   = 0;  /* auto-rotate camera around Y */
int    g_example_idx  = -1; /* current predefined example (-1 = none loaded yet) */
int    g_user_lighting_enabled = 0; /* tracks if user typed glEnable(GL_LIGHTING) */
int    g_show_outlines = 1; /* draw black wireframe over filled polygons */
int    g_highlight_current_poly = 1; /* highlight glBegin block under cursor */
int    g_current_block_begin = -1;  /* flat cmd index of cursor's glBegin */
int    g_current_block_end   = -1;  /* flat cmd index of cursor's glEnd */
int    g_ortho_mode = 0;  /* 0=perspective, 1=2D orthographic */

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
static int collect_for_vars(int pos, ExprVar *vars, int max_vars);
static int for_loop_depth_at(int pos);
static int block_depth_at(int pos);
static int try_commit_for_loop(void);
static int try_commit_func_def(void);
static int try_commit_if_block(void);
static int try_commit_close_brace(void);
static void load_example(int idx);

/* ========================================================================= */
/* Configuration menu item table (4.10)                                       */
/* ========================================================================= */

CfgItem g_cfg_items[] = {
    { "Wireframe",        "F2",  &g_wireframe,              2,               NULL          },
    { "Grid",             "F3",  &g_grid_theme,             GRID_THEME_COUNT, g_grid_names },
    { "Axes",             "F4",  &g_axes_theme,             AXES_THEME_COUNT, g_axes_names },
    { "Vertex labels",    "F5",  &g_show_vnums,             2,               NULL          },
    { "Normal vectors",   "F6",  &g_show_normals,           2,               NULL          },
    { "Outlines",         "F7",  &g_show_outlines,          2,               NULL          },
    { "Vertex guides",    "F8",  &g_show_guides,            2,               NULL          },
    { "Auto-normals",     "F9",  &g_autonormal,             2,               NULL          },
    { "Light indicators", "F10", &g_show_lights,            2,               NULL          },
    { "Camera rotate",    "F11", &g_cam_rotate,             2,               NULL          },
    { "Poly highlight",   "--",  &g_highlight_current_poly, 2,               NULL          },
    { "Variable panel",   "--",  &g_show_var_panel,         2,               NULL          },
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

/* Check begin block depth up to (but not including) line_idx */
int in_begin_block_at(int line_idx) {
    int depth = 0;
    int limit = (line_idx < g_num_cmds) ? line_idx : g_num_cmds;
    for (int i = 0; i < limit; i++) {
        if (!g_cmds[i].valid) continue;
        if (g_cmds[i].type == CMD_BEGIN) depth++;
        else if (g_cmds[i].type == CMD_END) depth--;
    }
    return depth > 0;
}

int in_begin_block(void) {
    return in_begin_block_at(g_num_cmds);
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

/* Write the body of a func def block (start..end exclusive) as C.
 * Used by write_func_defs_as_c to emit static helper functions. */
static void write_func_body_range_as_c(FILE *f, int start, int end_idx) {
    int for_depth = 0;
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
        case CMD_COMMENT:
        case CMD_VAR_ASSIGN:
            fprintf(f, "%s\n", g_cmds[i].source);
            break;
        case CMD_FUNC_DEF: case CMD_FUNC_END:
            break; /* nested func defs not supported */
        case CMD_CALL:
            fprintf(f, "  func%d();\n", (int)g_cmds[i].args[0]);
            break;
        default:
            if (for_depth > 0 || g_cmds[i].has_vars) {
                char c_src[MAX_LINE_LEN];
                repl_expr_to_c(g_cmds[i].source, c_src, sizeof(c_src));
                fprintf(f, "%s\n", c_src);
            } else {
                fprintf(f, "%s\n", g_cmds[i].source);
            }
            break;
        }
    }
}

/* Emit all user-defined functions as static C functions (before display()) */
static void write_func_defs_as_c(FILE *f) {
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid || g_cmds[i].type != CMD_FUNC_DEF) continue;
        int fn = (int)g_cmds[i].args[0];
        /* find the matching CMD_FUNC_END */
        int depth = 1, fe = g_num_cmds;
        for (int j = i + 1; j < g_num_cmds; j++) {
            CmdType t = g_cmds[j].type;
            if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) depth++;
            else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) {
                if (--depth == 0) { fe = j; break; }
            }
        }
        fprintf(f, "\nstatic void func%d(void) {\n", fn);
        write_func_body_range_as_c(f, i + 1, fe);
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

static void save_output(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        set_status("Error: cannot write output.c");
        return;
    }

    /* Detect whether any tess commands are present */
    int has_tess = 0;
    for (int i = 0; i < g_num_cmds; i++)
        if (g_cmds[i].valid && g_cmds[i].type >= CMD_TESS_BEGIN_POLYGON
                            && g_cmds[i].type <= CMD_TESS_VERTEX)
            has_tess = 1;

    /* Emit header, inserting func defs and optional tess preamble before void display() { */
    for (int i = 0; g_header_pre[i]; i++) {
        if (strcmp(g_header_pre[i], "void display() {") == 0) {
            write_func_defs_as_c(f);
            if (has_tess) write_tess_preamble(f);
        }
        fprintf(f, "%s\n", g_header_pre[i]);
    }
    for (int i = 0; i < 3; i++)
        fprintf(f, "%s\n", g_lookat[i]);
    for (int i = 0; g_header_post[i]; i++)
        fprintf(f, "%s\n", g_header_post[i]);
    write_light_setup(f);
    fprintf(f, "\n// Snippet start\n");

    /* Always emit predefined variable declarations */
    if (g_num_predef_vars > 0) {
        fprintf(f, "  float");
        int first = 1;
        for (int i = 0; i < g_num_predef_vars; i++) {
            fprintf(f, "%s %s = %g", first ? "" : ",",
                    g_predef_vars[i].name, g_predef_vars[i].value);
            first = 0;
        }
        fprintf(f, ";\n");
    }

    /* Write g_cmds[] preserving for-loop structure; skip func defs, emit calls */
    int for_depth = 0;
    int save_tess_depth = 0; /* tracks polygon/contour nesting for gluEnd translation */
    for (int i = 0; i < g_num_cmds; i++) {
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
        case CMD_COMMENT:
            fprintf(f, "%s\n", g_cmds[i].source);
            break;
        case CMD_VAR_ASSIGN:
            fprintf(f, "%s\n", g_cmds[i].source);
            break;
        case CMD_FUNC_DEF: {
            /* Skip entire function definition — already emitted above display() */
            int depth = 1;
            for (int j = i + 1; j < g_num_cmds; j++) {
                CmdType t = g_cmds[j].type;
                if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) depth++;
                else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) {
                    if (--depth == 0) { i = j; break; }
                }
            }
            break;
        }
        case CMD_FUNC_END:
            break; /* shouldn't be reached due to above skip */
        case CMD_CALL:
            fprintf(f, "  func%d();\n", (int)g_cmds[i].args[0]);
            break;
        case CMD_TESS_BEGIN_POLYGON:
            fprintf(f, "  { _tv_n=0; gluTessBeginPolygon(g_tess,NULL); }\n");
            save_tess_depth = 1; break;
        case CMD_TESS_BEGIN_CONTOUR:
            fprintf(f, "    gluTessBeginContour(g_tess);\n");
            save_tess_depth = 2; break;
        case CMD_TESS_END:
            if (save_tess_depth == 2) {
                fprintf(f, "    gluTessEndContour(g_tess);\n"); save_tess_depth = 1;
            } else {
                fprintf(f, "  gluTessEndPolygon(g_tess);\n"); save_tess_depth = 0;
            } break;
        case CMD_TESS_NORMAL:
            fprintf(f, "      { _tn[0]=%g; _tn[1]=%g; _tn[2]=%g; }\n",
                    g_cmds[i].args[0], g_cmds[i].args[1], g_cmds[i].args[2]); break;
        case CMD_TESS_COLOR:
            fprintf(f, "      { _tc[0]=%g; _tc[1]=%g; _tc[2]=%g; _tc[3]=%g; }\n",
                    g_cmds[i].args[0], g_cmds[i].args[1], g_cmds[i].args[2], g_cmds[i].args[3]); break;
        case CMD_TESS_VERTEX:
            fprintf(f, "      { TessVertex *_v=&_tv[_tv_n++];"
                       " _v->pos[0]=%g;_v->pos[1]=%g;_v->pos[2]=%g;"
                       " memcpy(_v->normal,_tn,24); memcpy(_v->color,_tc,32);"
                       " gluTessVertex(g_tess,_v->pos,_v); }\n",
                    g_cmds[i].args[0], g_cmds[i].args[1], g_cmds[i].args[2]); break;
        default:
            if (for_depth > 0 || g_cmds[i].has_vars) {
                /* Has expressions: translate REPL names to C */
                char c_src[MAX_LINE_LEN];
                repl_expr_to_c(g_cmds[i].source, c_src, sizeof(c_src));
                fprintf(f, "%s\n", c_src);
            } else {
                /* Outside for-loop, concrete values */
                fprintf(f, "%s\n", g_cmds[i].source);
            }
            break;
        }
    }

    /* Safety: close unclosed glBegin */
    {
        int bb = 0;
        for (int i = 0; i < g_num_cmds; i++) {
            if (g_cmds[i].valid && g_cmds[i].type == CMD_BEGIN) bb++;
            else if (g_cmds[i].valid && g_cmds[i].type == CMD_END) bb--;
        }
        if (bb > 0) fprintf(f, "  glEnd();\n");
    }
    fprintf(f, "// Snippet end\n");
    for (int i = 0; g_footer[i]; i++)
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
    int loaded = 0;
    int warnings = 0;
    /* Block stack: 1=for-loop, 2=if-block */
    int block_stack[64];
    int block_top = 0;
    int for_depth = 0; /* # of for-loop levels (for parse_command_with_vars context) */

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (!in_snippet) {
            /* Look for start marker */
            const char *p = line;
            while (*p && isspace((unsigned char)*p)) p++;
            if (strncmp(p, "// Snippet start", 16) == 0)
                in_snippet = 1;
            continue;
        }

        /* Check for end marker */
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncmp(p, "// Snippet end", 14) == 0)
            break;

        /* Skip empty lines */
        if (len == 0 || *p == '\0') continue;

        /* Skip predefined variable declarations (float x = ..., y = ...) */
        if (strncmp(p, "float ", 6) == 0 && for_depth == 0) {
            const char *q = p + 6;
            /* Check if it looks like "float x = 0, y = 0, ..." (predef vars) */
            while (*q && isspace((unsigned char)*q)) q++;
            if (isalpha((unsigned char)*q) && !isalpha((unsigned char)*(q+1))) {
                /* Single-char variable: likely a predefined var declaration.
                 * Parse values back into g_predef_vars. */
                const char *vp = p + 6;
                while (*vp) {
                    while (*vp && isspace((unsigned char)*vp)) vp++;
                    if (!isalpha((unsigned char)*vp)) break;
                    char vname[16]; int vi = 0;
                    while (*vp && (isalnum((unsigned char)*vp) || *vp == '_') &&
                           vi < (int)sizeof(vname) - 1)
                        vname[vi++] = *vp++;
                    vname[vi] = '\0';
                    while (*vp && isspace((unsigned char)*vp)) vp++;
                    if (*vp == '=') {
                        vp++;
                        ExprCtx ctx = { vp, NULL, 0 };
                        float val = eval_expr(&ctx);
                        vp = ctx.p;
                        for (int i = 0; i < g_num_predef_vars; i++) {
                            if (strcmp(g_predef_vars[i].name, vname) == 0) {
                                g_predef_vars[i].value = val;
                                break;
                            }
                        }
                    }
                    while (*vp && isspace((unsigned char)*vp)) vp++;
                    if (*vp == ',') vp++;
                    else break;
                }
                continue;
            }
        }

        /* Try C-style for-loop header */
        {
            char var_name[16];
            float start_v, end_v, step_v;
            if (parse_c_for_header(line, var_name, sizeof(var_name),
                                   &start_v, &end_v, &step_v)) {
                if (g_num_cmds < MAX_COMMANDS) {
                    GLCmd fb;
                    memset(&fb, 0, sizeof(fb));
                    fb.type = CMD_FOR_BEGIN;
                    fb.args[0] = start_v;
                    fb.args[1] = end_v;
                    fb.args[2] = step_v;
                    fb.valid = 1;
                    /* Count leading whitespace for indent */
                    int indent = 0;
                    while (line[indent] && isspace((unsigned char)line[indent]))
                        indent++;
                    char ind[32];
                    if (indent > (int)sizeof(ind) - 1) indent = (int)sizeof(ind) - 1;
                    memset(ind, ' ', indent);
                    ind[indent] = '\0';

                    /* Extract raw C expressions from the for-loop header
                     * to detect variable references. Format:
                     * for (float VAR = START; VAR <op> END; VAR +=/-= STEP) { */
                    char repl_line[MAX_LINE_LEN];
                    c_expr_to_repl(line, repl_line, sizeof(repl_line));
                    if (input_has_predef_vars(repl_line)) {
                        /* Re-parse from REPL-translated line to build
                         * source with variable names preserved */
                        char rv[16];
                        float rs, re, rst;
                        if (parse_c_for_header(repl_line, rv, sizeof(rv),
                                               &rs, &re, &rst)) {
                            /* Extract raw arg expressions from REPL line */
                            const char *rp = repl_line;
                            while (*rp && *rp != '=') rp++;
                            if (*rp) rp++;
                            /* rp now past '='; extract start expr up to ';' */
                            while (*rp && isspace((unsigned char)*rp)) rp++;
                            const char *se_start = rp;
                            while (*rp && *rp != ';') rp++;
                            char se[64]; int sl = (int)(rp - se_start);
                            if (sl > (int)sizeof(se) - 1) sl = (int)sizeof(se) - 1;
                            memcpy(se, se_start, sl); se[sl] = '\0';
                            while (sl > 0 && isspace((unsigned char)se[sl-1])) se[--sl] = '\0';
                            /* Skip ';', var, operator to get end expr */
                            if (*rp == ';') rp++;
                            while (*rp && isspace((unsigned char)*rp)) rp++;
                            while (*rp && (isalnum((unsigned char)*rp) || *rp == '_')) rp++;
                            while (*rp && isspace((unsigned char)*rp)) rp++;
                            while (*rp && (*rp == '<' || *rp == '>' || *rp == '=')) rp++;
                            while (*rp && isspace((unsigned char)*rp)) rp++;
                            const char *ee_start = rp;
                            while (*rp && *rp != ';') rp++;
                            char ee[64]; int el = (int)(rp - ee_start);
                            if (el > (int)sizeof(ee) - 1) el = (int)sizeof(ee) - 1;
                            memcpy(ee, ee_start, el); ee[el] = '\0';
                            while (el > 0 && isspace((unsigned char)ee[el-1])) ee[--el] = '\0';

                            fb.has_vars = 1;
                            if (step_v != 1.0f)
                                snprintf(fb.source, sizeof(fb.source),
                                         "%sfor(%s, %s, %s, %g) {",
                                         ind, var_name, se, ee, step_v);
                            else
                                snprintf(fb.source, sizeof(fb.source),
                                         "%sfor(%s, %s, %s) {",
                                         ind, var_name, se, ee);
                        }
                    }
                    if (!fb.has_vars) {
                        if (step_v != 1.0f)
                            snprintf(fb.source, sizeof(fb.source),
                                     "%sfor(%s, %g, %g, %g) {",
                                     ind, var_name, start_v, end_v, step_v);
                        else
                            snprintf(fb.source, sizeof(fb.source),
                                     "%sfor(%s, %g, %g) {",
                                     ind, var_name, start_v, end_v);
                    }
                    g_cmds[g_num_cmds++] = fb;
                    if (block_top < 64) block_stack[block_top++] = 1; /* BTYPE_FOR */
                    for_depth++;
                    loaded++;
                }
                continue;
            }
        }

        /* Try REPL-style for-loop header (backwards compatibility) */
        {
            char var_name[16];
            float start_v, end_v, step_v;
            const char *body_start;
            if (parse_for_header(line, var_name, sizeof(var_name),
                                 &start_v, &end_v, &step_v, &body_start)) {
                if (g_num_cmds < MAX_COMMANDS) {
                    GLCmd fb;
                    memset(&fb, 0, sizeof(fb));
                    fb.type = CMD_FOR_BEGIN;
                    fb.args[0] = start_v;
                    fb.args[1] = end_v;
                    fb.args[2] = step_v;
                    fb.valid = 1;
                    int indent = 0;
                    while (line[indent] && isspace((unsigned char)line[indent]))
                        indent++;
                    char ind[32];
                    if (indent > (int)sizeof(ind) - 1) indent = (int)sizeof(ind) - 1;
                    memset(ind, ' ', indent);
                    ind[indent] = '\0';
                    if (input_has_predef_vars(line)) {
                        fb.has_vars = 1;
                        strncpy(fb.source, line, sizeof(fb.source) - 1);
                        fb.source[sizeof(fb.source) - 1] = '\0';
                    } else if (step_v != 1.0f) {
                        snprintf(fb.source, sizeof(fb.source),
                                 "%sfor(%s, %g, %g, %g) {",
                                 ind, var_name, start_v, end_v, step_v);
                    } else {
                        snprintf(fb.source, sizeof(fb.source),
                                 "%sfor(%s, %g, %g) {",
                                 ind, var_name, start_v, end_v);
                    }
                    g_cmds[g_num_cmds++] = fb;
                    if (block_top < 64) block_stack[block_top++] = 1; /* BTYPE_FOR */
                    for_depth++;
                    loaded++;
                }
                continue;
            }
        }

        /* If-block: if(cond) { — create CMD_IF_BEGIN */
        if (strncmp(p, "if(", 3) == 0 || strncmp(p, "if (", 4) == 0) {
            /* Find opening paren */
            const char *ip = p;
            while (*ip && *ip != '(') ip++;
            if (*ip) {
                ip++; /* skip '(' */
                int paren = 1;
                const char *cond_start = ip;
                while (*ip && paren > 0) {
                    if (*ip == '(') paren++;
                    else if (*ip == ')') paren--;
                    if (paren > 0) ip++;
                }
                /* Check for opening brace */
                const char *brace = ip + 1;
                while (*brace && isspace((unsigned char)*brace)) brace++;
                if (*brace == '{' || *brace == '\0') {
                    /* Extract and evaluate condition */
                    char cond_text[MAX_LINE_LEN];
                    int clen = (int)(ip - cond_start);
                    if (clen > (int)sizeof(cond_text) - 1) clen = (int)sizeof(cond_text) - 1;
                    memcpy(cond_text, cond_start, clen);
                    cond_text[clen] = '\0';
                    float cond_args[1];
                    float cond_val = 0.0f;
                    if (parse_exprs(cond_text, cond_args, 1, NULL, 0) >= 1)
                        cond_val = cond_args[0];
                    /* Count leading whitespace for indent */
                    int indent = 0;
                    while (line[indent] && isspace((unsigned char)line[indent])) indent++;
                    char ind[32];
                    if (indent > (int)sizeof(ind) - 1) indent = (int)sizeof(ind) - 1;
                    memset(ind, ' ', indent); ind[indent] = '\0';
                    if (g_num_cmds < MAX_COMMANDS) {
                        GLCmd ib;
                        memset(&ib, 0, sizeof(ib));
                        ib.type = CMD_IF_BEGIN;
                        ib.args[0] = cond_val;
                        ib.valid = 1;
                        ib.has_vars = input_has_predef_vars(cond_text);
                        snprintf(ib.source, sizeof(ib.source), "%sif(%s) {", ind, cond_text);
                        g_cmds[g_num_cmds++] = ib;
                        if (block_top < 64) block_stack[block_top++] = 2; /* BTYPE_IF */
                        loaded++;
                    }
                    continue;
                }
            }
        }

        /* Closing brace: end of for-loop or if-block */
        if (*p == '}' && block_top > 0) {
            int btype = block_stack[--block_top];
            if (btype == 1) { /* BTYPE_FOR */
                if (g_num_cmds < MAX_COMMANDS) {
                    GLCmd fe;
                    memset(&fe, 0, sizeof(fe));
                    fe.type = CMD_FOR_END;
                    fe.valid = 1;
                    strncpy(fe.source, line, sizeof(fe.source) - 1);
                    fe.source[sizeof(fe.source) - 1] = '\0';
                    g_cmds[g_num_cmds++] = fe;
                    for_depth--;
                    loaded++;
                }
            } else { /* BTYPE_IF */
                if (g_num_cmds < MAX_COMMANDS) {
                    GLCmd ie;
                    memset(&ie, 0, sizeof(ie));
                    ie.type = CMD_IF_END;
                    ie.valid = 1;
                    strncpy(ie.source, line, sizeof(ie.source) - 1);
                    ie.source[sizeof(ie.source) - 1] = '\0';
                    g_cmds[g_num_cmds++] = ie;
                    loaded++;
                }
            }
            continue;
        }

        /* Inside for-loop: translate C expressions back to REPL */
        if (for_depth > 0) {
            char repl_line[MAX_LINE_LEN];
            c_expr_to_repl(line, repl_line, sizeof(repl_line));

            /* Parse with dummy loop vars so expression vars are recognized */
            ExprVar dvars[MAX_EXPR_VARS];
            int dnv = collect_for_vars(g_num_cmds, dvars, MAX_EXPR_VARS);
            GLCmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            int saved = g_edit_line;
            g_edit_line = g_num_cmds;
            if (parse_command_with_vars(repl_line, &cmd, dvars, dnv)) {
                /* Overwrite source with REPL expression text */
                strncpy(cmd.source, repl_line, sizeof(cmd.source) - 1);
                cmd.source[sizeof(cmd.source) - 1] = '\0';
                /* Ensure trailing semicolon */
                int slen = (int)strlen(cmd.source);
                while (slen > 0 && isspace((unsigned char)cmd.source[slen-1]))
                    slen--;
                if (slen > 0 && cmd.source[slen-1] != ';' &&
                    slen < (int)sizeof(cmd.source) - 1) {
                    cmd.source[slen] = ';';
                    cmd.source[slen+1] = '\0';
                }
                if (g_num_cmds < MAX_COMMANDS) {
                    g_cmds[g_num_cmds++] = cmd;
                    loaded++;
                }
            } else {
                fprintf(stderr, "Warning: could not parse line: %s\n", line);
                warnings++;
            }
            g_edit_line = saved;
            continue;
        }

        /* Try variable assignment: "  x = expr;" */
        {
            const char *ap = p;
            char vname[16]; int vi = 0;
            while (*ap && (isalpha((unsigned char)*ap) || *ap == '_') &&
                   vi < (int)sizeof(vname) - 1)
                vname[vi++] = *ap++;
            vname[vi] = '\0';
            if (vi > 0 && vi <= 2) { /* short var names like x, y, z, i, j */
                while (*ap && isspace((unsigned char)*ap)) ap++;
                if (*ap == '=' && *(ap+1) != '=') {
                    int pidx = -1;
                    for (int pv = 0; pv < g_num_predef_vars; pv++) {
                        if (strcmp(g_predef_vars[pv].name, vname) == 0) {
                            pidx = pv; break;
                        }
                    }
                    if (pidx >= 0) {
                        ap++;
                        ExprCtx ectx = { ap, NULL, 0 };
                        float val = eval_expr(&ectx);
                        g_predef_vars[pidx].value = val;
                        if (g_num_cmds < MAX_COMMANDS) {
                            GLCmd vc;
                            memset(&vc, 0, sizeof(vc));
                            vc.type = CMD_VAR_ASSIGN;
                            vc.valid = 1;
                            vc.args[0] = val;
                            vc.num_args = pidx;
                            strncpy(vc.source, line, sizeof(vc.source) - 1);
                            vc.source[sizeof(vc.source) - 1] = '\0';
                            g_cmds[g_num_cmds++] = vc;
                            loaded++;
                        }
                        continue;
                    }
                }
            }
        }

        /* C-style label: identifier: — create CMD_LABEL */
        {
            int llen = 0;
            while (p[llen] && (isalnum((unsigned char)p[llen]) || p[llen] == '_')) llen++;
            if (llen > 0 && p[llen] == ':' &&
                (p[llen+1] == '\0' || isspace((unsigned char)p[llen+1]))) {
                if (g_num_cmds < MAX_COMMANDS) {
                    GLCmd lc;
                    memset(&lc, 0, sizeof(lc));
                    lc.type = CMD_LABEL;
                    lc.valid = 1;
                    snprintf(lc.source, sizeof(lc.source), "%.*s:", llen, p);
                    g_cmds[g_num_cmds++] = lc;
                    loaded++;
                }
                continue;
            }
        }

        /* Tessellator C-style lines — match what save_output() emits and
         * reconstruct the corresponding CMD_TESS_* commands.
         * These lines are never inside a for-loop so they reach here directly. */

        /* { _tv_n=0; gluTessBeginPolygon(g_tess,NULL); } */
        if (strstr(p, "gluTessBeginPolygon") != NULL) {
            if (g_num_cmds < MAX_COMMANDS) {
                GLCmd tc; memset(&tc, 0, sizeof(tc));
                tc.type = CMD_TESS_BEGIN_POLYGON; tc.valid = 1;
                snprintf(tc.source, sizeof(tc.source), "  gluBegin(GLU_POLYGON);");
                g_cmds[g_num_cmds++] = tc; loaded++;
            }
            continue;
        }

        /* gluTessBeginContour(g_tess); */
        if (strstr(p, "gluTessBeginContour") != NULL) {
            if (g_num_cmds < MAX_COMMANDS) {
                GLCmd tc; memset(&tc, 0, sizeof(tc));
                tc.type = CMD_TESS_BEGIN_CONTOUR; tc.valid = 1;
                snprintf(tc.source, sizeof(tc.source), "    gluBegin(GLU_CONTOUR);");
                g_cmds[g_num_cmds++] = tc; loaded++;
            }
            continue;
        }

        /* gluTessEndContour(g_tess); / gluTessEndPolygon(g_tess); */
        if (strstr(p, "gluTessEndContour") != NULL ||
            strstr(p, "gluTessEndPolygon") != NULL) {
            if (g_num_cmds < MAX_COMMANDS) {
                GLCmd tc; memset(&tc, 0, sizeof(tc));
                tc.type = CMD_TESS_END; tc.valid = 1;
                const char *end_ind = strstr(p, "gluTessEndContour") ? "    " : "  ";
                snprintf(tc.source, sizeof(tc.source), "%sgluEnd();", end_ind);
                g_cmds[g_num_cmds++] = tc; loaded++;
            }
            continue;
        }

        /* { _tn[0]=X; _tn[1]=Y; _tn[2]=Z; } — CMD_TESS_NORMAL */
        if (strncmp(p, "{ _tn[", 6) == 0) {
            float nv[3] = {0, 0, 1};
            const char *np = p;
            for (int ni = 0; ni < 3; ni++) {
                const char *eq = strchr(np, '=');
                if (!eq) break; eq++;
                ExprCtx ctx = { eq, NULL, 0 };
                nv[ni] = eval_expr(&ctx); np = ctx.p;
            }
            if (g_num_cmds < MAX_COMMANDS) {
                GLCmd tc; memset(&tc, 0, sizeof(tc));
                tc.type = CMD_TESS_NORMAL; tc.valid = 1; tc.num_args = 3;
                tc.args[0] = nv[0]; tc.args[1] = nv[1]; tc.args[2] = nv[2];
                snprintf(tc.source, sizeof(tc.source),
                         "      gluNormal(%g, %g, %g);", nv[0], nv[1], nv[2]);
                g_cmds[g_num_cmds++] = tc; loaded++;
            }
            continue;
        }

        /* { _tc[0]=R; _tc[1]=G; _tc[2]=B; _tc[3]=A; } — CMD_TESS_COLOR */
        if (strncmp(p, "{ _tc[", 6) == 0) {
            float cv[4] = {1, 1, 1, 1};
            const char *cp = p;
            for (int ci = 0; ci < 4; ci++) {
                const char *eq = strchr(cp, '=');
                if (!eq) break; eq++;
                ExprCtx ctx = { eq, NULL, 0 };
                cv[ci] = eval_expr(&ctx); cp = ctx.p;
            }
            if (g_num_cmds < MAX_COMMANDS) {
                GLCmd tc; memset(&tc, 0, sizeof(tc));
                tc.type = CMD_TESS_COLOR; tc.valid = 1; tc.num_args = 4;
                tc.args[0] = cv[0]; tc.args[1] = cv[1];
                tc.args[2] = cv[2]; tc.args[3] = cv[3];
                snprintf(tc.source, sizeof(tc.source),
                         "      gluColor(%g, %g, %g, %g);",
                         cv[0], cv[1], cv[2], cv[3]);
                g_cmds[g_num_cmds++] = tc; loaded++;
            }
            continue;
        }

        /* { TessVertex *_v=...; _v->pos[0]=X; ... gluTessVertex(...); } — CMD_TESS_VERTEX */
        if (strstr(p, "TessVertex") != NULL && strstr(p, "gluTessVertex") != NULL) {
            float vv[3] = {0, 0, 0};
            const char *vp = strstr(p, "_v->pos[0]");
            if (vp) {
                for (int vi = 0; vi < 3; vi++) {
                    const char *eq = strchr(vp, '=');
                    if (!eq) break; eq++;
                    ExprCtx ctx = { eq, NULL, 0 };
                    vv[vi] = eval_expr(&ctx); vp = ctx.p;
                }
            }
            if (g_num_cmds < MAX_COMMANDS) {
                GLCmd tc; memset(&tc, 0, sizeof(tc));
                tc.type = CMD_TESS_VERTEX; tc.valid = 1; tc.num_args = 3;
                tc.args[0] = vv[0]; tc.args[1] = vv[1]; tc.args[2] = vv[2];
                snprintf(tc.source, sizeof(tc.source),
                         "      gluVertex(%g, %g, %g);", vv[0], vv[1], vv[2]);
                g_cmds[g_num_cmds++] = tc; loaded++;
            }
            continue;
        }

        /* Regular line (outside for-loop): parse as normal command */
        {
            /* Translate C expressions to REPL first */
            char repl_line[MAX_LINE_LEN];
            c_expr_to_repl(line, repl_line, sizeof(repl_line));
            GLCmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            if (parse_command(repl_line, &cmd)) {
                if (input_has_predef_vars(repl_line)) {
                    cmd.has_vars = 1;
                    strncpy(cmd.source, repl_line, sizeof(cmd.source) - 1);
                    cmd.source[sizeof(cmd.source) - 1] = '\0';
                }
                if (g_num_cmds < MAX_COMMANDS) {
                    g_cmds[g_num_cmds++] = cmd;
                    loaded++;
                }
            } else {
                fprintf(stderr, "Warning: could not parse line: %s\n", line);
                warnings++;
            }
        }
    }

    fclose(f);

    if (loaded > 0) {
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

static void update_autocomplete(void) {
    g_ac_count = 0;
    g_ac_sel = 0;
    g_ac_ghost[0] = '\0';

    if (g_input_len == 0) return;

    /* Only offer completions when cursor is at the end of input */
    if (g_cursor_pos != g_input_len) return;

    /* If inside glBegin(...), complete mode names */
    if (strncmp(g_input, "glBegin(", 8) == 0 && g_input_len > 8) {
        const char *after = g_input + 8;
        int alen = g_input_len - 8;
        for (int i = 0; g_begin_modes[i].name && g_ac_count < MAX_AC_MATCHES; i++) {
            if (strncmp(g_begin_modes[i].name, after, alen) == 0 &&
                (int)strlen(g_begin_modes[i].name) > alen) {
                g_ac_matches[g_ac_count++] = g_begin_modes[i].name;
            }
        }
        if (g_ac_count > 0) {
            const char *m = g_ac_matches[0];
            snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s)", m + alen);
        }
        return;
    }

    /* If inside glShadeModel(...), complete model names */
    if (strncmp(g_input, "glShadeModel(", 13) == 0 && g_input_len > 13) {
        const char *after = g_input + 13;
        int alen = g_input_len - 13;
        for (int i = 0; g_shade_models[i].name && g_ac_count < MAX_AC_MATCHES; i++) {
            if (strncmp(g_shade_models[i].name, after, alen) == 0 &&
                (int)strlen(g_shade_models[i].name) > alen) {
                g_ac_matches[g_ac_count++] = g_shade_models[i].name;
            }
        }
        if (g_ac_count > 0) {
            const char *m = g_ac_matches[0];
            snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s)", m + alen);
        }
        return;
    }

    /* If inside glEnable/glDisable(...), complete cap names */
    if ((strncmp(g_input, "glEnable(", 9) == 0 && g_input_len > 9) ||
        (strncmp(g_input, "glDisable(", 10) == 0 && g_input_len > 10)) {
        int prefix = (g_input[2] == 'E') ? 9 : 10;
        const char *after = g_input + prefix;
        int alen = g_input_len - prefix;
        for (int i = 0; g_enable_caps[i].name && g_ac_count < MAX_AC_MATCHES; i++) {
            if (strncmp(g_enable_caps[i].name, after, alen) == 0 &&
                (int)strlen(g_enable_caps[i].name) > alen) {
                g_ac_matches[g_ac_count++] = g_enable_caps[i].name;
            }
        }
        if (g_ac_count > 0) {
            const char *m = g_ac_matches[0];
            snprintf(g_ac_ghost, sizeof(g_ac_ghost), "%s)", m + alen);
        }
        return;
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
static int for_loop_depth_at(int pos);
static int find_block_end(int begin_idx);
static int block_depth_at(int pos);
static CmdType nearest_open_block_at(int pos);
static void get_for_var_name(const GLCmd *cmd, char *var, int var_sz);
static int collect_for_vars(int pos, ExprVar *vars, int max_vars);

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

    /* glBegin(MODE) */
    if (strcmp(func, "glBegin") == 0) {
        char *a = args;
        while (*a && isspace((unsigned char)*a)) a++;
        int al = (int)strlen(a);
        while (al > 0 && isspace((unsigned char)a[al - 1])) a[--al] = '\0';

        for (int i = 0; g_begin_modes[i].name; i++) {
            if (strcmp(a, g_begin_modes[i].name) == 0) {
                cmd->type = CMD_BEGIN;
                cmd->mode = g_begin_modes[i].value;
                cmd->valid = 1;
                snprintf(cmd->source, sizeof(cmd->source),
                         "  glBegin(%s);", g_begin_modes[i].name);
                return 1;
            }
        }
        set_status("Unknown mode. Try GL_TRIANGLES, GL_TRIANGLE_STRIP, ...");
        return 0;
    }

    /* glEnd() */
    if (strcmp(func, "glEnd") == 0) {
        cmd->type = CMD_END;
        cmd->valid = 1;
        snprintf(cmd->source, sizeof(cmd->source), "  glEnd();");
        return 1;
    }

    /* glEnable(CAP) */
    if (strcmp(func, "glEnable") == 0) {
        char *a = args;
        while (*a && isspace((unsigned char)*a)) a++;
        int al = (int)strlen(a);
        while (al > 0 && isspace((unsigned char)a[al - 1])) a[--al] = '\0';
        for (int i = 0; g_enable_caps[i].name; i++) {
            if (strcmp(a, g_enable_caps[i].name) == 0) {
                cmd->type = CMD_ENABLE;
                cmd->mode = g_enable_caps[i].value;
                cmd->valid = 1;
                snprintf(cmd->source, sizeof(cmd->source),
                         "  glEnable(%s);", g_enable_caps[i].name);
                return 1;
            }
        }
        set_status("Try GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL");
        return 0;
    }

    /* glDisable(CAP) */
    if (strcmp(func, "glDisable") == 0) {
        char *a = args;
        while (*a && isspace((unsigned char)*a)) a++;
        int al = (int)strlen(a);
        while (al > 0 && isspace((unsigned char)a[al - 1])) a[--al] = '\0';
        for (int i = 0; g_enable_caps[i].name; i++) {
            if (strcmp(a, g_enable_caps[i].name) == 0) {
                cmd->type = CMD_DISABLE;
                cmd->mode = g_enable_caps[i].value;
                cmd->valid = 1;
                snprintf(cmd->source, sizeof(cmd->source),
                         "  glDisable(%s);", g_enable_caps[i].name);
                return 1;
            }
        }
        set_status("Try GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL");
        return 0;
    }

    /* glShadeModel(MODE) */
    if (strcmp(func, "glShadeModel") == 0) {
        char *a = args;
        while (*a && isspace((unsigned char)*a)) a++;
        int al = (int)strlen(a);
        while (al > 0 && isspace((unsigned char)a[al - 1])) a[--al] = '\0';
        for (int i = 0; g_shade_models[i].name; i++) {
            if (strcmp(a, g_shade_models[i].name) == 0) {
                cmd->type = CMD_SHADE_MODEL;
                cmd->mode = g_shade_models[i].value;
                cmd->valid = 1;
                snprintf(cmd->source, sizeof(cmd->source),
                         "  glShadeModel(%s);", g_shade_models[i].name);
                return 1;
            }
        }
        set_status("Try GL_SMOOTH or GL_FLAT");
        return 0;
    }

    /* Indent based on cursor position context */
    const char *indent = in_begin_block_at(g_edit_line) ? "    " : "  ";

    /* glVertex3f(x, y, z) */
    if (strcmp(func, "glVertex3f") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 3, vars, num_vars);
        if (cmd->num_args == 3) {
            cmd->type = CMD_VERTEX3F;
            cmd->valid = 1;
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sglVertex3f(%g, %g, %g);",
                     indent, cmd->args[0], cmd->args[1], cmd->args[2]);
            return 1;
        }
        set_status("Usage: glVertex3f(x, y, z)");
        return 0;
    }

    /* glNormal3f(x, y, z) */
    if (strcmp(func, "glNormal3f") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 3, vars, num_vars);
        if (cmd->num_args == 3) {
            cmd->type = CMD_NORMAL3F;
            cmd->valid = 1;
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sglNormal3f(%g, %g, %g);",
                     indent, cmd->args[0], cmd->args[1], cmd->args[2]);
            return 1;
        }
        set_status("Usage: glNormal3f(nx, ny, nz)");
        return 0;
    }

    /* glTranslatef(x, y, z) */
    if (strcmp(func, "glTranslatef") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 3, vars, num_vars);
        if (cmd->num_args == 3) {
            cmd->type = CMD_TRANSLATE3F;
            cmd->valid = 1;
            cmd->has_vars = (num_vars > 0);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sglTranslatef(%g, %g, %g);",
                     indent, cmd->args[0], cmd->args[1], cmd->args[2]);
            return 1;
        }
        set_status("Usage: glTranslatef(x, y, z)");
        return 0;
    }

    /* glColor3f(r, g, b) */
    if (strcmp(func, "glColor3f") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 3, vars, num_vars);
        if (cmd->num_args == 3) {
            cmd->type = CMD_COLOR3F;
            cmd->valid = 1;
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sglColor3f(%g, %g, %g);",
                     indent, cmd->args[0], cmd->args[1], cmd->args[2]);
            return 1;
        }
        set_status("Usage: glColor3f(r, g, b)");
        return 0;
    }

    /* glColor4f(r, g, b, a) */
    if (strcmp(func, "glColor4f") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 4, vars, num_vars);
        if (cmd->num_args == 4) {
            cmd->type = CMD_COLOR4F;
            cmd->valid = 1;
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sglColor4f(%g, %g, %g, %g);",
                     indent, cmd->args[0], cmd->args[1], cmd->args[2],
                     cmd->args[3]);
            return 1;
        }
        set_status("Usage: glColor4f(r, g, b, a)");
        return 0;
    }

    /* funcN() — function call */
    if (strncmp(func, "func", 4) == 0 && func[4] >= '0' && func[4] <= '9' && func[5] == '\0') {
        int fn = func[4] - '0';
        cmd->type = CMD_CALL;
        cmd->valid = 1;
        cmd->args[0] = (float)fn;
        int fdepth = block_depth_at(g_edit_line);
        int bb = in_begin_block_at(g_edit_line);
        int ind_v = (bb ? 4 : 2) + fdepth * 2;
        char ind_str[32];
        if (ind_v > (int)sizeof(ind_str) - 1) ind_v = (int)sizeof(ind_str) - 1;
        memset(ind_str, ' ', ind_v);
        ind_str[ind_v] = '\0';
        snprintf(cmd->source, sizeof(cmd->source), "%sfunc%d();", ind_str, fn);
        return 1;
    }

    /* glVertex2f(x, y) — 2D vertex for ortho mode */
    if (strcmp(func, "glVertex2f") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 2, vars, num_vars);
        if (cmd->num_args == 2) {
            cmd->type = CMD_VERTEX2F;
            cmd->valid = 1;
            cmd->has_vars = (num_vars > 0);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sglVertex2f(%g, %g);",
                     indent, cmd->args[0], cmd->args[1]);
            return 1;
        }
        set_status("Usage: glVertex2f(x, y)");
        return 0;
    }

    /* gluSphere(radius, slices, stacks) */
    if (strcmp(func, "gluSphere") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 3, vars, num_vars);
        if (cmd->num_args == 3) {
            cmd->type = CMD_GLU_SPHERE;
            cmd->valid = 1;
            cmd->has_vars = (num_vars > 0);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sgluSphere(g_quadric, %g, %d, %d);",
                     indent, cmd->args[0], (int)cmd->args[1], (int)cmd->args[2]);
            return 1;
        }
        set_status("Usage: gluSphere(radius, slices, stacks)");
        return 0;
    }

    /* gluCylinder(baseRadius, topRadius, height, slices, stacks) */
    if (strcmp(func, "gluCylinder") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 5, vars, num_vars);
        if (cmd->num_args == 5) {
            cmd->type = CMD_GLU_CYLINDER;
            cmd->valid = 1;
            cmd->has_vars = (num_vars > 0);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sgluCylinder(g_quadric, %g, %g, %g, %d, %d);",
                     indent, cmd->args[0], cmd->args[1], cmd->args[2],
                     (int)cmd->args[3], (int)cmd->args[4]);
            return 1;
        }
        set_status("Usage: gluCylinder(baseR, topR, height, slices, stacks)");
        return 0;
    }

    /* gluDisk(innerRadius, outerRadius, slices, loops) */
    if (strcmp(func, "gluDisk") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 4, vars, num_vars);
        if (cmd->num_args == 4) {
            cmd->type = CMD_GLU_DISK;
            cmd->valid = 1;
            cmd->has_vars = (num_vars > 0);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sgluDisk(g_quadric, %g, %g, %d, %d);",
                     indent, cmd->args[0], cmd->args[1],
                     (int)cmd->args[2], (int)cmd->args[3]);
            return 1;
        }
        set_status("Usage: gluDisk(innerR, outerR, slices, loops)");
        return 0;
    }

    /* gluPartialDisk(innerR, outerR, slices, loops, startAngle, sweepAngle) */
    if (strcmp(func, "gluPartialDisk") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 6, vars, num_vars);
        if (cmd->num_args == 6) {
            cmd->type = CMD_GLU_PARTIAL_DISK;
            cmd->valid = 1;
            cmd->has_vars = (num_vars > 0);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sgluPartialDisk(g_quadric, %g, %g, %d, %d, %g, %g);",
                     indent, cmd->args[0], cmd->args[1],
                     (int)cmd->args[2], (int)cmd->args[3],
                     cmd->args[4], cmd->args[5]);
            return 1;
        }
        set_status("Usage: gluPartialDisk(innerR, outerR, slices, loops, startAngle, sweepAngle)");
        return 0;
    }

    /* glutSolidTorus(innerRadius, outerRadius, nsides, rings) */
    if (strcmp(func, "glutSolidTorus") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 4, vars, num_vars);
        if (cmd->num_args == 4) {
            cmd->type = CMD_GLUT_TORUS;
            cmd->valid = 1;
            cmd->has_vars = (num_vars > 0);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sglutstolidTorus(%g, %g, %d, %d);",
                     indent, cmd->args[0], cmd->args[1],
                     (int)cmd->args[2], (int)cmd->args[3]);
            /* Fix: use correct spelling */
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sglutSolidTorus(%g, %g, %d, %d);",
                     indent, cmd->args[0], cmd->args[1],
                     (int)cmd->args[2], (int)cmd->args[3]);
            return 1;
        }
        set_status("Usage: glutSolidTorus(innerR, outerR, nsides, rings)");
        return 0;
    }

    /* gluBegin(GLU_POLYGON) — start a tessellated polygon */
    if (strcmp(func, "gluBegin") == 0) {
        char *a = args; while (*a && isspace((unsigned char)*a)) a++;
        if (strncmp(a, "GLU_POLYGON", 11) == 0) {
            cmd->type = CMD_TESS_BEGIN_POLYGON;
            cmd->valid = 1;
            snprintf(cmd->source, sizeof(cmd->source), "  gluBegin(GLU_POLYGON);");
            return 1;
        }
        if (strncmp(a, "GLU_CONTOUR", 11) == 0) {
            cmd->type = CMD_TESS_BEGIN_CONTOUR;
            cmd->valid = 1;
            snprintf(cmd->source, sizeof(cmd->source), "    gluBegin(GLU_CONTOUR);");
            return 1;
        }
        set_status("Usage: gluBegin(GLU_POLYGON) or gluBegin(GLU_CONTOUR)");
        return 0;
    }

    /* gluEnd() — end tessellator contour or polygon */
    if (strcmp(func, "gluEnd") == 0 || strcmp(p, "gluEnd()") == 0) {
        cmd->type = CMD_TESS_END;
        cmd->valid = 1;
        snprintf(cmd->source, sizeof(cmd->source), "%sgluEnd();", indent);
        return 1;
    }

    /* gluNormal(x, y, z) — set per-vertex normal for tessellator */
    if (strcmp(func, "gluNormal") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 3, vars, num_vars);
        if (cmd->num_args == 3) {
            cmd->type = CMD_TESS_NORMAL;
            cmd->valid = 1;
            cmd->has_vars = (num_vars > 0);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sgluNormal(%g, %g, %g);",
                     indent, cmd->args[0], cmd->args[1], cmd->args[2]);
            return 1;
        }
        set_status("Usage: gluNormal(x, y, z)");
        return 0;
    }

    /* gluColor(r, g, b[, a]) — set per-vertex color for tessellator */
    if (strcmp(func, "gluColor") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 4, vars, num_vars);
        if (cmd->num_args >= 3) {
            if (cmd->num_args < 4) cmd->args[3] = 1.0f;
            cmd->num_args = 4;
            cmd->type = CMD_TESS_COLOR;
            cmd->valid = 1;
            cmd->has_vars = (num_vars > 0);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sgluColor(%g, %g, %g, %g);",
                     indent, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
            return 1;
        }
        set_status("Usage: gluColor(r, g, b) or gluColor(r, g, b, a)");
        return 0;
    }

    /* gluVertex(x, y, z) — add a vertex to the current tessellator contour */
    if (strcmp(func, "gluVertex") == 0) {
        cmd->num_args = parse_exprs(args, cmd->args, 3, vars, num_vars);
        if (cmd->num_args == 3) {
            cmd->type = CMD_TESS_VERTEX;
            cmd->valid = 1;
            cmd->has_vars = (num_vars > 0);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sgluVertex(%g, %g, %g);",
                     indent, cmd->args[0], cmd->args[1], cmd->args[2]);
            return 1;
        }
        set_status("Usage: gluVertex(x, y, z)");
        return 0;
    }

    /* goto label — jump to a named label */
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
                              int inside_begin) {
    GLCmd c;
    memset(&c, 0, sizeof(c));
    c.type = CMD_NORMAL3F;
    c.args[0] = nx; c.args[1] = ny; c.args[2] = nz;
    c.num_args = 3;
    c.valid = 1;
    c.is_auto = 1;
    const char *indent = inside_begin ? "    " : "  ";
    snprintf(c.source, sizeof(c.source),
             "%sglNormal3f(%g, %g, %g);", indent, nx, ny, nz);
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
            GLCmd nc = make_auto_normal(nx, ny, nz, 1);
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

/* Flatten g_cmds (with for-loops) into g_flat_cmds (concrete commands) */
static void flatten_range(int start, int end_idx, ExprVar *vars, int nv) {
    int i = start;
    while (i < end_idx && i < g_num_cmds) {
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
                if (parse_for_header(fb_cmd->source, rv, sizeof(rv),
                                     &rs, &re, &rst, &unused_body)) {
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
                    ExprVar lvars[MAX_EXPR_VARS];
                    int lnv = 0;
                    if (vars)
                        for (int v = 0; v < nv && lnv < MAX_EXPR_VARS; v++)
                            lvars[lnv++] = vars[v];
                    if (lnv < MAX_EXPR_VARS) {
                        strncpy(lvars[lnv].name, var_name,
                                sizeof(lvars[lnv].name) - 1);
                        lvars[lnv].name[sizeof(lvars[lnv].name) - 1] = '\0';
                        lvars[lnv].value = val;
                        lnv++;
                    }
                    flatten_range(i + 1, fe, lvars, lnv);
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
            for (int k = 0; k < g_num_cmds; k++) {
                if (g_cmds[k].type == CMD_FUNC_DEF && (int)g_cmds[k].args[0] == fn) {
                    int fe = find_block_end(k);
                    flatten_range(k + 1, fe, vars, nv);
                    break;
                }
            }
            i++;
            continue;
        }

        /* Conditionals: pass through to flat_cmds unchanged.
         * Conditions are evaluated at execute time so that goto loops
         * see updated variable values on each iteration. */
        if (g_cmds[i].type == CMD_IF_BEGIN || g_cmds[i].type == CMD_IF_END) {
            if (g_num_flat_cmds < MAX_COMMANDS) {
                g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
                g_flat_cmds[g_num_flat_cmds++].src_cmd_idx = i;
            }
            i++;
            continue;
        }

        /* Comments: pass through to flat array (skipped by execute, kept in save) */
        if (g_cmds[i].type == CMD_COMMENT) {
            if (g_num_flat_cmds < MAX_COMMANDS) {
                g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
                g_flat_cmds[g_num_flat_cmds++].src_cmd_idx = i;
            }
            i++;
            continue;
        }

        /* Variable assignments: update predefined var and pass through */
        if (g_cmds[i].type == CMD_VAR_ASSIGN) {
            int vi = g_cmds[i].num_args; /* predef var index */
            if (vi >= 0 && vi < g_num_predef_vars)
                g_predef_vars[vi].value = g_cmds[i].args[0];
            if (g_num_flat_cmds < MAX_COMMANDS) {
                g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
                g_flat_cmds[g_num_flat_cmds++].src_cmd_idx = i;
            }
            i++;
            continue;
        }

        /* Regular command */
        if (g_num_flat_cmds >= MAX_COMMANDS) { i++; continue; }

        if (vars && nv > 0) {
            GLCmd tmp;
            memset(&tmp, 0, sizeof(tmp));
            int saved = g_edit_line;
            g_edit_line = g_num_flat_cmds;
            if (parse_command_with_vars(g_cmds[i].source, &tmp, vars, nv)) {
                tmp.has_vars = g_cmds[i].has_vars;
                tmp.src_cmd_idx = i;
                strncpy(tmp.source, g_cmds[i].source, sizeof(tmp.source) - 1);
                tmp.source[sizeof(tmp.source) - 1] = '\0';
                g_flat_cmds[g_num_flat_cmds++] = tmp;
            }
            g_edit_line = saved;
        } else if (g_cmds[i].has_vars) {
            /* Outside loop but has predefined var references: re-evaluate */
            GLCmd tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (parse_command(g_cmds[i].source, &tmp)) {
                tmp.has_vars = 1;
                tmp.src_cmd_idx = i;
                strncpy(tmp.source, g_cmds[i].source, sizeof(tmp.source) - 1);
                tmp.source[sizeof(tmp.source) - 1] = '\0';
                g_flat_cmds[g_num_flat_cmds++] = tmp;
            }
        } else {
            g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
            g_flat_cmds[g_num_flat_cmds++].src_cmd_idx = i;
        }
        i++;
    }
}

void flatten_commands(void) {
    g_num_flat_cmds = 0;
    flatten_range(0, g_num_cmds, NULL, 0);

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

    /* Find glBegin/glEnd block containing the cursor line (g_edit_line) */
    g_current_block_begin = -1;
    g_current_block_end   = -1;
    /* Walk flat cmds: track which source line each cmd came from via g_cmds index */
    /* Approximate: find last BEGIN at or before g_edit_line, then matching END */
    int found_begin = -1;
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_BEGIN) { found_begin = i; }
        else if (g_flat_cmds[i].type == CMD_END && found_begin >= 0) {
            /* Check if g_edit_line is between the source lines of these */
            /* We check g_cmds array for a rough match using the flat cmd
               index position relative to what lines have been committed */
            found_begin = -1;
        }
    }
    /* Better approach: scan g_cmds for the innermost BEGIN/END containing g_edit_line */
    {
        int begin_src = -1, begin_flat = -1;
        /* Simple: track BEGIN positions in g_flat_cmds that correspond to g_cmds before g_edit_line */
        int fcur = 0;
        for (int ci = 0; ci < g_num_cmds && fcur < g_num_flat_cmds; ci++) {
            if (!g_cmds[ci].valid) continue;
            if (g_cmds[ci].type == CMD_FUNC_DEF || g_cmds[ci].type == CMD_FUNC_END ||
                g_cmds[ci].type == CMD_FOR_BEGIN || g_cmds[ci].type == CMD_FOR_END ||
                g_cmds[ci].type == CMD_IF_BEGIN  || g_cmds[ci].type == CMD_IF_END  ||
                g_cmds[ci].type == CMD_CALL)
                continue; /* these expand or skip */
            /* match next flat cmd */
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

/* ========================================================================= */
/* Command execution                                                          */
/* ========================================================================= */

void execute_commands(void) {
    int in_begin = 0;
    int tess_depth = 0; /* 0=outside, 1=in polygon, 2=in contour */
    GLdouble tess_current_normal[3] = {0.0, 0.0, 1.0};
    GLdouble tess_current_color[4]  = {1.0, 1.0, 1.0, 1.0};
    int goto_count = 0; /* safety guard against infinite goto loops */

    int pc = 0;
    while (pc < g_num_flat_cmds) {
        if (!g_flat_cmds[pc].valid) { pc++; continue; }
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
            glColor3f(g_flat_cmds[pc].args[0], g_flat_cmds[pc].args[1],
                      g_flat_cmds[pc].args[2]);
            break;
        case CMD_COLOR4F:
            glColor4f(g_flat_cmds[pc].args[0], g_flat_cmds[pc].args[1],
                      g_flat_cmds[pc].args[2], g_flat_cmds[pc].args[3]);
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
        case CMD_TRANSLATE3F:
            glTranslatef(g_flat_cmds[pc].args[0], g_flat_cmds[pc].args[1],
                         g_flat_cmds[pc].args[2]);
            break;
        case CMD_VERTEX2F:
            if (in_begin)
                glVertex2f(g_flat_cmds[pc].args[0], g_flat_cmds[pc].args[1]);
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
            tess_current_color[3] = (g_flat_cmds[pc].num_args >= 4) ? g_flat_cmds[pc].args[3] : 1.0;
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
            /* Find matching CMD_LABEL by name in source and set pc */
            const char *src = g_flat_cmds[pc].source;
            /* src is like "  goto labelname;" — skip past "goto " keyword */
            const char *goto_kw = strstr(src, "goto ");
            if (!goto_kw) break;
            const char *gname = goto_kw + 5;
            while (*gname == ' ') gname++; /* skip any extra spaces */
            char lname[64];
            int llen = 0;
            while (llen < 63 && gname[llen] && gname[llen] != ';' && !isspace((unsigned char)gname[llen])) {
                lname[llen] = gname[llen];
                llen++;
            }
            lname[llen] = '\0';
            if (llen == 0) break;
            /* Search for matching CMD_LABEL; guard against infinite loops */
            if (goto_count++ > 100000) {
                set_status("goto: loop limit reached");
                goto execute_done;
            }
            for (int li = 0; li < g_num_flat_cmds; li++) {
                if (g_flat_cmds[li].valid && g_flat_cmds[li].type == CMD_LABEL) {
                    const char *lsrc = g_flat_cmds[li].source;
                    /* source is "labelname:" */
                    if (strncmp(lsrc, lname, llen) == 0 && lsrc[llen] == ':') {
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
                const char *p = g_flat_cmds[pc].source;
                while (*p && *p != '(') p++;
                if (*p) p++;
                ExprCtx ctx = { p, g_predef_vars, g_num_predef_vars };
                cond = eval_expr(&ctx);
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
            if (vi >= 0 && vi < g_num_predef_vars)
                g_predef_vars[vi].value = g_flat_cmds[pc].args[0];
            break;
        }
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
    if (tess_depth == 2 && g_tess) { gluTessEndContour(g_tess); tess_depth = 1; }
    if (tess_depth == 1 && g_tess) { gluTessEndPolygon(g_tess); }
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
    if (g_normals_dirty) {
        recompute_autonormals();
        g_normals_dirty = 0;
    }
    if (g_flat_dirty) {
        flatten_commands();
        g_flat_dirty = 0;
    }
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
        render_3d_scene();
    }

    /* 2D overlays in full window coords */
    glViewport(0, 0, g_win_w, g_win_h);
    render_code_panel();
    render_autocomplete();
    render_var_panel();
    render_config_menu();
    render_help();

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


/* Check for-loop nesting depth at a position in g_cmds */
static int for_loop_depth_at(int pos) {
    int depth = 0;
    for (int i = 0; i < pos && i < g_num_cmds; i++) {
        if (g_cmds[i].type == CMD_FOR_BEGIN) depth++;
        else if (g_cmds[i].type == CMD_FOR_END) depth--;
    }
    return depth > 0 ? depth : 0;
}

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
    int depth = 0;
    for (int i = 0; i < pos && i < g_num_cmds; i++) {
        CmdType t = g_cmds[i].type;
        if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) depth++;
        else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) depth--;
    }
    return depth > 0 ? depth : 0;
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

/* Collect loop variable names/dummy values from enclosing FOR_BEGINs */
static int collect_for_vars(int pos, ExprVar *vars, int max_vars) {
    ExprVar stack[MAX_LOOP_DEPTH];
    int depth = 0;
    for (int i = 0; i < pos && i < g_num_cmds; i++) {
        if (g_cmds[i].type == CMD_FOR_BEGIN && depth < MAX_LOOP_DEPTH) {
            char vn[16];
            get_for_var_name(&g_cmds[i], vn, sizeof(vn));
            strncpy(stack[depth].name, vn, sizeof(stack[depth].name) - 1);
            stack[depth].name[sizeof(stack[depth].name) - 1] = '\0';
            stack[depth].value = g_cmds[i].args[0];
            depth++;
        } else if (g_cmds[i].type == CMD_FOR_END && depth > 0) {
            depth--;
        }
    }
    int nv = depth < max_vars ? depth : max_vars;
    for (int i = 0; i < nv; i++) vars[i] = stack[i];
    return nv;
}

/*
 * Try to handle input as a variable assignment: "x = expr"
 * Returns 1 if handled (valid or invalid assignment), 0 if not an assignment.
 */
static int try_assign_variable(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Collect identifier */
    char name[16];
    int ni = 0;
    while (*p && (isalpha((unsigned char)*p) || *p == '_') && ni < (int)sizeof(name) - 1)
        name[ni++] = *p++;
    name[ni] = '\0';
    if (ni == 0) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;
    /* Reject '==' */
    if (*p == '=') return 0;

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
    ExprCtx ctx = { p, NULL, 0 };
    float val = eval_expr(&ctx);
    g_predef_vars[var_idx].value = val;

    /* Build a CMD_VAR_ASSIGN command so it appears in the code panel */
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_VAR_ASSIGN;
    cmd.valid = 1;
    cmd.args[0] = val;
    cmd.num_args = var_idx; /* store predef var index */
    snprintf(cmd.source, sizeof(cmd.source), "  %s = %g;", name, val);

    int fpos = g_inserting ? g_edit_line :
               (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
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

    char var_name[16];
    float start, end, step;
    const char *body_start;
    if (!parse_for_header(p, var_name, sizeof(var_name),
                          &start, &end, &step, &body_start)) {
        set_status("for syntax: for(var, start, end[, step]) body;");
        return 1;
    }

    while (*body_start && isspace((unsigned char)*body_start)) body_start++;

    int pos = g_inserting ? g_edit_line :
              (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
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

    if (input_has_predef_vars(ra)) {
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
    ExprVar dv[1];
    strncpy(dv[0].name, var_name, sizeof(dv[0].name) - 1);
    dv[0].name[sizeof(dv[0].name) - 1] = '\0';
    dv[0].value = start;
    GLCmd body_cmd;
    memset(&body_cmd, 0, sizeof(body_cmd));
    int saved = g_edit_line;
    g_edit_line = pos;
    if (!parse_command_with_vars(body, &body_cmd, dv, 1)) {
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
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "func", 4) != 0) return 0;
    if (p[4] < '0' || p[4] > '9') return 0;
    int fn = p[4] - '0';
    const char *rest = p + 5;
    while (*rest && isspace((unsigned char)*rest)) rest++;
    if (*rest != '{' && *rest != '\0') return 0;

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
        snprintf(g_cmds[g_edit_line].source, sizeof(g_cmds[g_edit_line].source),
                 "%sfunc%d {", indent, fn);
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
    fd.valid = 1;
    snprintf(fd.source, sizeof(fd.source), "%sfunc%d {", indent, fn);

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
    int neval = parse_exprs(cond_text, cond_args, 1, NULL, 0);
    float cond_val = (neval >= 1) ? cond_args[0] : 0.0f;

    /* Check for { after ) */
    p++; /* skip ')' */
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' && *p != '\0') {
        set_status("if syntax: if(expr) {");
        return 1;
    }

    int pos = g_inserting ? g_edit_line :
              (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
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
    ib.has_vars = input_has_predef_vars(cond_text);

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


    /* Backtick: toggle configuration menu */
    if (key == '`') {
        g_show_config = !g_show_config;
        g_config_hover = -1;
        return;
    }

    /* Escape */
    if (key == 27) {
        if (g_show_config) {
            g_show_config = 0;
            return;
        }
        if (g_show_help) {
            g_show_help = 0;
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

    /* Ctrl+Z: undo (remove last command) */
    if (key == 26) {
        if (g_num_cmds > 0) {
            g_num_cmds--;
                        g_inserting = 0;
            if (g_edit_line > g_num_cmds) {
                g_edit_line = g_num_cmds;
            }
            load_line_to_input(g_edit_line);
            mark_normals_dirty();
            set_status("Undo: removed last command");
        } else {
            set_status("Nothing to undo");
        }
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

    /* Ctrl+A: toggle accumulation AA */
    if (key == 1) {
        if (g_use_accum) {
            g_accum_aa_enabled = !g_accum_aa_enabled;
            set_status(g_accum_aa_enabled ? "Accum AA: ON" : "Accum AA: OFF");
        } else {
            set_status("Accum buffer disabled (remove --noaccum to enable)");
        }
        return;
    }

    /* Ctrl+T: toggle time ('t' variable) play / pause */
    if (key == 20) {
        g_t_playing = !g_t_playing;
        set_status(g_t_playing ? "Time: playing" : "Time: paused (set 't' manually)");
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

        /* On existing line: if unmodified, just advance to insert mode */
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
                int in_loop = for_loop_depth_at(fpos) > 0;
                int parsed;
                if (in_loop) {
                    ExprVar dvars[MAX_EXPR_VARS];
                    int dnv = collect_for_vars(fpos, dvars, MAX_EXPR_VARS);
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
                int in_loop = for_loop_depth_at(fpos) > 0;
                int parsed = 0;
                if (in_loop) {
                    ExprVar dvars[MAX_EXPR_VARS];
                    int dnv = collect_for_vars(fpos, dvars, MAX_EXPR_VARS);
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
                int in_loop = for_loop_depth_at(fpos) > 0;
                int parsed;
                if (in_loop) {
                    ExprVar dvars[MAX_EXPR_VARS];
                    int dnv = collect_for_vars(fpos, dvars, MAX_EXPR_VARS);
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
            int in_loop = for_loop_depth_at(fpos) > 0;
            int parsed;
            if (in_loop) {
                ExprVar dvars[MAX_EXPR_VARS];
                int dnv = collect_for_vars(fpos, dvars, MAX_EXPR_VARS);
                int saved_el = g_edit_line;
                g_edit_line = fpos;
                parsed = parse_command_with_vars(g_input, &cmd, dvars, dnv);
                g_edit_line = saved_el;
                if (parsed) {
                    /* Preserve original expression source */
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
                    /* Preserve expression source with variable names */
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

    switch (key) {
    /* Cursor movement within line */
    case GLUT_KEY_LEFT:
        if (g_cursor_pos > 0) g_cursor_pos--;
        update_autocomplete();
        break;
    case GLUT_KEY_RIGHT:
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
        load_example((g_example_idx + 1) % NUM_EXAMPLES);
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
    /* Release: end any variable drag */
    if (state == GLUT_UP && g_drag_var >= 0) {
        g_drag_var = -1;
        glutPostRedisplay();
        return;
    }

    if (button == GLUT_LEFT_BUTTON && state == GLUT_DOWN) {
        /* Config menu click: toggle the clicked item */
        if (g_show_config) {
            int row = cfg_hit_row(x, y);
            if (row >= 0) {
                *g_cfg_items[row].value =
                    (*g_cfg_items[row].value + 1) % g_cfg_items[row].n_states;
                if (g_cfg_items[row].value == &g_autonormal && g_autonormal)
                    mark_normals_dirty();
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
                g_drag_var       = row;
                g_drag_start_val = g_predef_vars[row].value;
                g_drag_start_x   = x;
                glutPostRedisplay();
                return;
            }
        }

        /* Left-click in code panel: navigate to line + column */
        int panel_w = (int)(g_win_w * g_panel_frac);
        if (x < panel_w) {
            handle_code_panel_click(x, y);
            return;   /* don't start camera drag */
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
            int panel_w = (int)(g_win_w * g_panel_frac);
            if (x < panel_w) g_scroll--;
            else g_vel_zoom -= 0.3f;
        }
        glutPostRedisplay();
    } else if (button == 4 && state == GLUT_DOWN) {
        if (g_show_help) {
            g_help_scroll++;
        } else {
            int panel_w = (int)(g_win_w * g_panel_frac);
            if (x < panel_w) g_scroll++;
            else g_vel_zoom += 0.3f;
        }
        glutPostRedisplay();
    }
#endif
}

#ifndef USE_GLUT
/* FreeGLUT mouse wheel callback */
static void mousewheel_func(int wheel, int direction, int x, int y) {
    (void)wheel; (void)y;
    if (g_show_help) {
        /* direction > 0 = scroll up (towards top of help) */
        g_help_scroll -= direction;
    } else {
        int panel_w = (int)(g_win_w * g_panel_frac);
        if (x < panel_w) {
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
}

static void motion_func(int x, int y) {
    int dx = x - g_mouse_x;
    int dy = y - g_mouse_y;

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
                g_cmds[i].num_args == g_drag_var) {
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

/* Each example is an array of source lines terminated by NULL.
 * Lines are processed sequentially:
 *   "for(...) {"  → CMD_FOR_BEGIN + CMD_FOR_END, enters block
 *   "funcN {"     → CMD_FUNC_DEF + CMD_FUNC_END, enters block
 *   "if(...) {"   → CMD_IF_BEGIN + CMD_IF_END, enters block
 *   "}"           → closes current block
 *   "funcN()"     → CMD_CALL
 *   anything else → parse_command() as a regular GL command
 */

/* Helper: feed one line through the commit pipeline, as if the user typed it
 * and pressed ';'.  This reuses the existing try_commit_* functions so that
 * source strings, indentation, has_vars, and block nesting are handled
 * identically to interactive use. */
static void feed_line(const char *line) {
    strncpy(g_input, line, MAX_INPUT_LEN - 1);
    g_input[MAX_INPUT_LEN - 1] = '\0';
    g_input_len = (int)strlen(g_input);
    g_cursor_pos = g_input_len;

    /* Try structured blocks first (order matters) */
    if (try_commit_close_brace()) return;
    if (try_commit_for_loop()) return;
    if (try_commit_func_def()) return;
    if (try_commit_if_block()) return;

    /* Regular command */
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    int fpos = g_inserting ? g_edit_line : g_num_cmds;
    int in_loop = for_loop_depth_at(fpos) > 0;
    int parsed;
    if (in_loop) {
        ExprVar dvars[MAX_EXPR_VARS];
        int dnv = collect_for_vars(fpos, dvars, MAX_EXPR_VARS);
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
            int fdepth = block_depth_at(fpos);
            int ind_v = (bb_v ? 4 : 2) + fdepth * 2;
            char indent_v[32];
            if (ind_v > (int)sizeof(indent_v) - 1) ind_v = (int)sizeof(indent_v) - 1;
            memset(indent_v, ' ', ind_v);
            indent_v[ind_v] = '\0';
            snprintf(cmd.source, sizeof(cmd.source), "%s%s;", indent_v, stripped);
        }
    }
    if (parsed && g_num_cmds < MAX_COMMANDS) {
        if (g_inserting) {
            /* Insert at g_edit_line (inside a block) */
            for (int j = g_num_cmds; j > g_edit_line; j--)
                g_cmds[j] = g_cmds[j - 1];
            g_cmds[g_edit_line] = cmd;
            g_num_cmds++;
            g_edit_line++;
        } else {
            g_cmds[g_num_cmds++] = cmd;
            g_edit_line = g_num_cmds;
        }
    }
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
}

/* Load an example from an array of source lines */
static void load_example_lines(const char **lines) {
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

/* Example 0: Lit cube (default) */
static const char *g_example_cube[] = {
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_COLOR_MATERIAL);",
    "glEnable(GL_NORMALIZE);",
    "glShadeModel(GL_SMOOTH);",
    "glEnable(GL_LIGHT3);",
    "glEnable(GL_LIGHT2);",
    "//glEnable(GL_LINE_SMOOTH);",
    "glColor3f(1, 1, 1);",
    "glBegin(GL_QUAD_STRIP);",
    "glNormal3f(0, 0, 1);",
    "glVertex3f(1, 1, 1);",
    "glNormal3f(0, 0, 1);",
    "glVertex3f(-1, 1, 1);",
    "glNormal3f(0, 0, 1);",
    "glVertex3f(1, -1, 1);",
    "glNormal3f(0, 0, 1);",
    "glVertex3f(-1, -1, 1);",
    "glNormal3f(-0, -1, -0);",
    "glVertex3f(1, -1, -1);",
    "glNormal3f(-0, -1, -0);",
    "glVertex3f(-1, -1, -1);",
    "glNormal3f(0, 0, -1);",
    "glVertex3f(1, 1, -1);",
    "glNormal3f(0, 0, -1);",
    "glVertex3f(-1, 1, -1);",
    "glNormal3f(0, 1, -0);",
    "glVertex3f(1, 1, 1);",
    "glNormal3f(0, 1, -0);",
    "glVertex3f(-1, 1, 1);",
    "glEnd();",
    NULL
};

/* Example 1: Animated ring — for-loop + t variable */
static const char *g_example_ring[] = {
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_COLOR_MATERIAL);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT3);",
    "glBegin(GL_LINE_LOOP);",
    "for(i, 0, 48) {",
        "glColor3f(sin(i*TAU/48)*0.5+0.5, cos(i*TAU/48)*0.5+0.5, 0.5);",
        "glVertex3f(cos(i*TAU/48+t)*2, sin(i*TAU/48+t)*2, sin(i*TAU/12+t)*0.5);",
    "}",
    "glEnd();",
    "glBegin(GL_TRIANGLE_FAN);",
    "glColor3f(0.2, 0.5, 1);",
    "glVertex3f(0, 0, 0);",
    "for(i, 0, 25) {",
        "glColor3f(sin(i*TAU/24+t)*0.5+0.5, 0.3, cos(i*TAU/24+t)*0.5+0.5);",
        "glVertex3f(cos(i*TAU/24)*1.5, sin(i*TAU/24)*1.5, 0);",
    "}",
    "glEnd();",
    NULL
};

/* Example 2: Function demo — define reusable triangle, call with transforms */
static const char *g_example_func[] = {
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_COLOR_MATERIAL);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT3);",
    "func0 {",
        "glBegin(GL_TRIANGLES);",
        "glNormal3f(0, 0, 1);",
        "glVertex3f(0, 0.8, 0);",
        "glVertex3f(-0.7, -0.4, 0);",
        "glVertex3f(0.7, -0.4, 0);",
        "glEnd();",
    "}",
    "glColor3f(1, 0.3, 0.3);",
    "func0();",
    "glTranslatef(2, 0, 0);",
    "glColor3f(0.3, 1, 0.3);",
    "func0();",
    "glTranslatef(-4, 0, 0);",
    "glColor3f(0.3, 0.3, 1);",
    "func0();",
    NULL
};

/* Example 3: Conditional colors — if-blocks + t variable */
static const char *g_example_cond[] = {
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_COLOR_MATERIAL);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT3);",
    "glBegin(GL_QUADS);",
    "if(sin(t) > 0) {",
        "glColor3f(1, 0.2, 0.2);",
    "}",
    "if(sin(t) <= 0) {",
        "glColor3f(0.2, 0.2, 1);",
    "}",
    "glNormal3f(0, 0, 1);",
    "glVertex3f(-1, -1, 0);",
    "glVertex3f(1, -1, 0);",
    "glVertex3f(1, 1, 0);",
    "glVertex3f(-1, 1, 0);",
    "if(cos(t) > 0) {",
        "glColor3f(0.2, 1, 0.2);",
    "}",
    "if(cos(t) <= 0) {",
        "glColor3f(1, 1, 0.2);",
    "}",
    "glNormal3f(0, 0, -1);",
    "glVertex3f(-1, -1, -1);",
    "glVertex3f(-1, 1, -1);",
    "glVertex3f(1, 1, -1);",
    "glVertex3f(1, -1, -1);",
    "glEnd();",
    NULL
};

/* Example 4: Parametric torus — nested for-loops */
static const char *g_example_torus[] = {
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_COLOR_MATERIAL);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT3);",
    "glEnable(GL_LIGHT2);",
    "glShadeModel(GL_SMOOTH);",
    "for(i, 0, 24) {",
        "glBegin(GL_QUAD_STRIP);",
        "for(j, 0, 25) {",
            "glColor3f(sin(i*TAU/24)*0.4+0.6, cos(j*TAU/24)*0.4+0.6, 0.5);",
            "glVertex3f((2+cos(j*TAU/24))*cos(i*TAU/24), (2+cos(j*TAU/24))*sin(i*TAU/24), sin(j*TAU/24));",
            "glVertex3f((2+cos(j*TAU/24))*cos((i+1)*TAU/24), (2+cos(j*TAU/24))*sin((i+1)*TAU/24), sin(j*TAU/24));",
        "}",
        "glEnd();",
    "}",
    NULL
};

/* Example 5: GLU tessellator — concave arrow polygon with per-vertex color */
static const char *g_example_tess[] = {
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_COLOR_MATERIAL);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT3);",
    "glEnable(GL_LIGHT2);",
    "glShadeModel(GL_SMOOTH);",
    "// Arrow shape — concave, tessellated with per-vertex color",
    "gluBegin(GLU_POLYGON);",
    "gluBegin(GLU_CONTOUR);",
    "gluNormal(0, 0, 1);",
    "gluColor(0.2, 0.4, 1, 1);",
    "gluVertex(-1.2, -0.45, 0);",
    "gluColor(0.2, 0.4, 1, 1);",
    "gluVertex(-1.2, 0.45, 0);",
    "gluColor(0.55, 0.3, 1, 1);",
    "gluVertex(0, 0.45, 0);",
    "gluColor(1, 0.45, 0.05, 1);",
    "gluVertex(0, 1.1, 0);",
    "gluColor(1, 0.9, 0.1, 1);",
    "gluVertex(1.3, 0, 0);",
    "gluColor(1, 0.45, 0.05, 1);",
    "gluVertex(0, -1.1, 0);",
    "gluColor(0.55, 0.3, 1, 1);",
    "gluVertex(0, -0.45, 0);",
    "gluEnd();",
    "gluEnd();",
    NULL
};

static const char **g_examples[] = {
    g_example_cube,
    g_example_ring,
    g_example_func,
    g_example_cond,
    g_example_torus,
    g_example_tess,
};
static const char *g_example_names[] = {
    "Lit cube",
    "Animated ring (for + t)",
    "Function demo (func0)",
    "Conditional colors (if + t)",
    "Parametric torus (nested for)",
    "GLU tessellator (concave arrow)",
};
/* NUM_EXAMPLES defined in forward declarations section */

static void load_example(int idx) {
    if (idx < 0 || idx >= NUM_EXAMPLES) return;
    load_example_lines(g_examples[idx]);
    g_example_idx = idx;
    char msg[128];
    snprintf(msg, sizeof(msg), "Example %d/%d: %s (F12 for next)",
             idx + 1, NUM_EXAMPLES, g_example_names[idx]);
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
    glEnable(GL_MULTISAMPLE);
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
}

/* ========================================================================= */
/* Main                                                                       */
/* ========================================================================= */

int main(int argc, char **argv) {
    /* Pre-scan argv for flags and extract the optional input file path.
     * --noaccum disables the accumulation buffer (on by default). */
    const char *input_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--noaccum") == 0)
            g_use_accum = 0;
        else if (!input_file)
            input_file = argv[i];
    }

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH | GLUT_MULTISAMPLE |
                        (g_use_accum ? GLUT_ACCUM : 0));
    glutInitWindowSize(g_win_w, g_win_h);
    glutCreateWindow("OpenGL REPL - Display List Dynamic Rendering");

    init_gl();
    init_predef_vars();
    /* Cache index of 't' in predefined vars for the time animation */
    for (int i = 0; i < g_num_predef_vars; i++)
        if (strcmp(g_predef_vars[i].name, "t") == 0) { g_t_var_idx = i; break; }
    load_initial_commands(input_file);

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutKeyboardFunc(keyboard_func);
    glutSpecialFunc(special_func);
    glutMouseFunc(mouse_func);
    glutMotionFunc(motion_func);
    glutPassiveMotionFunc(passive_motion_func);
#ifndef USE_GLUT
    glutMouseWheelFunc(mousewheel_func);
#endif
    glutTimerFunc(16, timer_func, 0);

    glutMainLoop();
    return 0;
}
