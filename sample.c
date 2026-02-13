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
 *   glEnable(CAP)        glDisable(CAP)
 *   glShadeModel(MODE)
 *
 * Math expressions: sin, cos, tan, sqrt, abs, pow, min, max, PI, TAU
 *   Example: glVertex3f(cos(PI/4), sin(PI/4), 0)
 *
 * Predefined variables: x, y, z, i, j, k, n, t
 *   Assignment: x = 1.5;
 *
 * For-loops (saved as C for-loops, imported back as loops):
 *   for(i, 0, 24) glVertex3f(cos(i*TAU/24), sin(i*TAU/24), 0);
 *   for(i, 0, N) { body... }   Multi-line block
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
 *   Scroll wheel   Zoom
 *   F1-F10         Toggle overlays (help/wire/grid/axes/vnums/normals/indices/guides/autonorm/lights)
 *   PgUp/PgDn      Scroll code panel
 *
 * Import/Export:
 *   Ctrl+S saves to output.c with snippet markers.
 *   Run ./sample output.c to reload a saved session.
 */

#include <gl_includes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "repl_eval.h"

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

#define MAX_COMMANDS    4096
#define MAX_LINE_LEN    256
#define MAX_INPUT_LEN   1024
#define MAX_AC_MATCHES  10

#define FONT_MONO       GLUT_BITMAP_9_BY_15
#define FONT_SMALL      GLUT_BITMAP_8_BY_13
#define FONT_W          9
#define FONT_H          15
#define LINE_H          18
#define CODE_MARGIN_X   10
#define CODE_MARGIN_Y   8

/* ========================================================================= */
/* Types                                                                      */
/* ========================================================================= */

typedef enum {
    CMD_BEGIN,
    CMD_END,
    CMD_VERTEX3F,
    CMD_NORMAL3F,
    CMD_COLOR3F,
    CMD_COLOR4F,
    CMD_ENABLE,
    CMD_DISABLE,
    CMD_SHADE_MODEL,
    CMD_FOR_BEGIN,
    CMD_FOR_END,
    CMD_COMMENT,
    CMD_VAR_ASSIGN,
    CMD_TYPE_COUNT
} CmdType;

typedef struct {
    const char *name;
    GLenum      value;
} EnumEntry;

typedef struct {
    CmdType type;
    GLenum  mode;
    float   args[4];
    int     num_args;
    char    source[MAX_LINE_LEN];
    int     valid;
    int     is_auto;
    int     has_vars;   /* source contains predefined var references, re-eval on flatten */
} GLCmd;

/* ExprVar, ExprCtx, MAX_EXPR_VARS defined in repl_eval.h */

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

static const EnumEntry g_begin_modes[] = {
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

static const EnumEntry g_enable_caps[] = {
    { "GL_DEPTH_TEST",      GL_DEPTH_TEST },
    { "GL_LIGHTING",        GL_LIGHTING },
    { "GL_COLOR_MATERIAL",  GL_COLOR_MATERIAL },
    { "GL_NORMALIZE",       GL_NORMALIZE },
    { "GL_LIGHT0",          GL_LIGHT0 },
    { "GL_LIGHT1",          GL_LIGHT1 },
    { "GL_LIGHT2",          GL_LIGHT2 },
    { "GL_LIGHT3",          GL_LIGHT3 },
    { NULL, 0 }
};

static const EnumEntry g_shade_models[] = {
    { "GL_SMOOTH", GL_SMOOTH },
    { "GL_FLAT",   GL_FLAT },
    { NULL, 0 }
};

static const char *g_func_completions[] = {
    "glVertex3f(",
    "glNormal3f(",
    "glColor3f(",
    "glColor4f(",
    "glBegin(",
    "glEnd()",
    "glEnable(",
    "glDisable(",
    "glShadeModel(",
    "for(",
    "x = ",
    "y = ",
    "z = ",
    NULL
};

static const char *g_header_pre[] = {
    "#include <gl_includes.h>",
    "#include <math.h>",
    "",
    "#ifndef M_PI",
    "#define M_PI 3.14159265358979323846",
    "#endif",
    "",
    "static float g_angle = 0.0f;",
    "static int   g_rotating = 1;",
    "",
    "void display() {",
    "  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "  glLoadIdentity();",
    NULL
};

static char g_lookat[3][128] = {
    "  gluLookAt(0.00, 0.00, 5.00,",
    "            0.00, 0.00, 0.00,",
    "            0.00, 1.00, 0.00);"
};

static const char *g_header_post[] = {
    "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);",
    NULL
};

static const char *g_footer[] = {
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

static GLCmd  g_cmds[MAX_COMMANDS];
static int    g_num_cmds = 0;
static int    g_normals_dirty = 1;
static GLCmd  g_flat_cmds[MAX_COMMANDS];
static int    g_num_flat_cmds = 0;
static int    g_flat_dirty = 1;
static void mark_normals_dirty(void) { g_normals_dirty = 1; g_flat_dirty = 1; }

/* Predefined variables — defined in repl_eval.c */

/* Editor */
static char   g_input[MAX_INPUT_LEN];
static int    g_input_len = 0;
static int    g_cursor_pos = 0;     /* cursor position within g_input (0..g_input_len) */
static int    g_edit_line = 0;      /* 0..g_num_cmds; g_num_cmds = new line */
static char   g_newline_buf[MAX_INPUT_LEN] = "";
static int    g_newline_len = 0;
static int    g_inserting = 0;      /* insert mode: virtual new line at g_edit_line */

/* (no display list - commands are executed directly each frame) */

/* Camera */
static float  g_cam_rx = 20.0f;
static float  g_cam_ry = 30.0f;
static float  g_cam_dist = 5.0f;
static float  g_cam_px = 0.0f, g_cam_py = 0.0f;
static int    g_mouse_x, g_mouse_y;
static int    g_mouse_btn = -1;

/* Window */
static int    g_win_w = 1200, g_win_h = 800;

/* Code panel */
static float  g_panel_frac = 0.42f;
static int    g_scroll = 0;

/* Cursor blink */
static int    g_cursor_on = 1;
static int    g_blink_tick = 0;

/* Animation */
static float  g_anim_time = 0.0f;

/* Toggles */
static int    g_show_help    = 0;
static int    g_help_scroll  = 0;
static int    g_wireframe    = 0;
static int    g_grid_theme   = 2;  /* 0=off, 1=classic, 2=fog, 3=tron, 4=ember, 5=faint, 6=focus */
#define GRID_THEME_COUNT 7
static const char *g_grid_names[] = {
    "Grid OFF", "Grid: Classic", "Grid: Fog", "Grid: Tron", "Grid: Ember",
    "Grid: Faint", "Grid: Focus"
};
static float  g_focus_vtx[3] = { 0.0f, 0.0f, 0.0f };  /* last vertex pos for focus grid */
static int    g_focus_vtx_valid = 0;
static int    g_axes_theme   = 3;  /* 0=off, 1=classic, 2=pulse, 3=neon, 4=compass */
#define AXES_THEME_COUNT 5
static const char *g_axes_names[] = {
    "Axes OFF", "Axes: Classic", "Axes: Pulse", "Axes: Neon", "Axes: Compass"
};
static int    g_show_vnums   = 1;
static int    g_show_normals = 1;
static int    g_show_indices = 0;
static int    g_show_guides  = 1;
static int    g_autonormal   = 1;
static int    g_show_lights  = 1;
static int    g_cam_rotate   = 0;  /* auto-rotate camera around Y */

/* Lights */
#define MAX_LIGHTS 4
typedef struct {
    GLenum   id;         /* GL_LIGHT0 .. GL_LIGHT3 */
    int      enabled;
    float    pos[4];     /* xyz + w (0=directional, 1=positional) */
    float    diffuse[4];
    float    ambient[4];
    float    specular[4];
} SceneLight;

static SceneLight g_lights[MAX_LIGHTS] = {
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
static char   g_status[256] = "";
static int    g_status_ttl = 0;

/* Autocomplete */
static const char *g_ac_matches[MAX_AC_MATCHES];
static int    g_ac_count = 0;
static int    g_ac_sel = 0;
static char   g_ac_ghost[MAX_LINE_LEN] = "";
static int    g_cursor_px = 0;     /* screen pos of cursor, set during render */
static int    g_cursor_py = 0;

/* Clipboard */
static GLCmd  g_clipboard[MAX_COMMANDS];
static int    g_clipboard_count = 0;

/* Selection (shift+arrow) */
static int    g_sel_anchor = -1;   /* -1 = no selection */
static int    g_sel_end = -1;

static void clear_selection(void) { g_sel_anchor = g_sel_end = -1; }
static int sel_active(void) { return g_sel_anchor >= 0 && g_sel_end >= 0; }
static int sel_lo(void) {
    return g_sel_anchor < g_sel_end ? g_sel_anchor : g_sel_end;
}
static int sel_hi(void) {
    return g_sel_anchor > g_sel_end ? g_sel_anchor : g_sel_end;
}

/* Forward declarations (eval_expr, parse_for_header, etc. are in repl_eval.h) */
static int parse_command(const char *line, GLCmd *cmd);
static int parse_command_with_vars(const char *line, GLCmd *cmd,
                                   ExprVar *vars, int num_vars);
static int collect_for_vars(int pos, ExprVar *vars, int max_vars);

/* ========================================================================= */
/* Utility                                                                    */
/* ========================================================================= */

static void set_status(const char *msg) {
    strncpy(g_status, msg, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    g_status_ttl = 240;
}

static const char *mode_name(GLenum mode) {
    for (int i = 0; g_begin_modes[i].name; i++)
        if (g_begin_modes[i].value == mode) return g_begin_modes[i].name;
    return "???";
}

/* Check begin block depth up to (but not including) line_idx */
static int in_begin_block_at(int line_idx) {
    int depth = 0;
    int limit = (line_idx < g_num_cmds) ? line_idx : g_num_cmds;
    for (int i = 0; i < limit; i++) {
        if (!g_cmds[i].valid) continue;
        if (g_cmds[i].type == CMD_BEGIN) depth++;
        else if (g_cmds[i].type == CMD_END) depth--;
    }
    return depth > 0;
}

static int in_begin_block(void) {
    return in_begin_block_at(g_num_cmds);
}

static GLenum current_begin_mode(void) {
    GLenum mode = GL_TRIANGLES;
    for (int i = 0; i < g_num_cmds; i++)
        if (g_cmds[i].valid && g_cmds[i].type == CMD_BEGIN)
            mode = g_cmds[i].mode;
    return mode;
}

static int count_vertices(void) {
    int n = 0;
    for (int i = 0; i < g_num_flat_cmds; i++)
        if (g_flat_cmds[i].valid && g_flat_cmds[i].type == CMD_VERTEX3F) n++;
    return n;
}

/* Update the gluLookAt header lines from current camera orbit params */
static void update_lookat_strings(void) {
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

static void save_output(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        set_status("Error: cannot write output.c");
        return;
    }

    for (int i = 0; g_header_pre[i]; i++)
        fprintf(f, "%s\n", g_header_pre[i]);
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

    /* Write g_cmds[] preserving for-loop structure */
    int for_depth = 0;
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
    int for_depth = 0;

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
                    for_depth++;
                    loaded++;
                }
                continue;
            }
        }

        /* Closing brace: for-loop end */
        if (*p == '}' && for_depth > 0) {
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
static void navigate_to_line(int target) {
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
static int find_for_end(int for_begin_idx);
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
        int fdepth = for_loop_depth_at(g_edit_line);
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
static void recompute_autonormals(void) {
    if (!g_autonormal) return;

    /* Process each begin/end block (skip for-loop regions) */
    int i = 0;
    while (i < g_num_cmds) {
        if (g_cmds[i].type == CMD_FOR_BEGIN) {
            i = find_for_end(i);
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
            int fe = find_for_end(i);
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

        /* Comments: pass through to flat array (skipped by execute, kept in save) */
        if (g_cmds[i].type == CMD_COMMENT) {
            if (g_num_flat_cmds < MAX_COMMANDS)
                g_flat_cmds[g_num_flat_cmds++] = g_cmds[i];
            i++;
            continue;
        }

        /* Variable assignments: update predefined var and pass through */
        if (g_cmds[i].type == CMD_VAR_ASSIGN) {
            int vi = g_cmds[i].num_args; /* predef var index */
            if (vi >= 0 && vi < g_num_predef_vars)
                g_predef_vars[vi].value = g_cmds[i].args[0];
            if (g_num_flat_cmds < MAX_COMMANDS)
                g_flat_cmds[g_num_flat_cmds++] = g_cmds[i];
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
                strncpy(tmp.source, g_cmds[i].source, sizeof(tmp.source) - 1);
                tmp.source[sizeof(tmp.source) - 1] = '\0';
                g_flat_cmds[g_num_flat_cmds++] = tmp;
            }
        } else {
            g_flat_cmds[g_num_flat_cmds++] = g_cmds[i];
        }
        i++;
    }
}

static void flatten_commands(void) {
    g_num_flat_cmds = 0;
    flatten_range(0, g_num_cmds, NULL, 0);
}

/* ========================================================================= */
/* Command execution                                                          */
/* ========================================================================= */

static void execute_commands(void) {
    int in_begin = 0;
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        switch (g_flat_cmds[i].type) {
        case CMD_BEGIN:
            if (in_begin) glEnd();
            glBegin(g_flat_cmds[i].mode);
            in_begin = 1;
            break;
        case CMD_END:
            if (in_begin) { glEnd(); in_begin = 0; }
            break;
        case CMD_VERTEX3F:
            if (in_begin)
                glVertex3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                           g_flat_cmds[i].args[2]);
            break;
        case CMD_NORMAL3F:
            glNormal3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                       g_flat_cmds[i].args[2]);
            break;
        case CMD_COLOR3F:
            glColor3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                      g_flat_cmds[i].args[2]);
            break;
        case CMD_COLOR4F:
            glColor4f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                      g_flat_cmds[i].args[2], g_flat_cmds[i].args[3]);
            break;
        case CMD_ENABLE:
            glEnable(g_flat_cmds[i].mode);
            for (int li = 0; li < MAX_LIGHTS; li++)
                if (g_lights[li].id == g_flat_cmds[i].mode)
                    g_lights[li].enabled = 1;
            break;
        case CMD_DISABLE:
            glDisable(g_flat_cmds[i].mode);
            for (int li = 0; li < MAX_LIGHTS; li++)
                if (g_lights[li].id == g_flat_cmds[i].mode)
                    g_lights[li].enabled = 0;
            break;
        case CMD_SHADE_MODEL:
            glShadeModel(g_flat_cmds[i].mode);
            break;
        default:
            break;
        }
    }
    if (in_begin) glEnd();
}

/* ========================================================================= */
/* 2D rendering helpers                                                       */
/* ========================================================================= */

static void draw_string(float x, float y, const char *s, void *font) {
    glRasterPos2f(x, y);
    for (; *s; s++)
        glutBitmapCharacter(font, (unsigned char)*s);
}

static void draw_quad(float x, float y, float w, float h) {
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static void begin_2d(void) {
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

static void end_2d(void) {
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

/* ========================================================================= */
/* Syntax color helpers                                                       */
/* ========================================================================= */

static void color_for_type(CmdType t) {
    switch (t) {
    case CMD_BEGIN:
    case CMD_END:      glColor3f(0.85f, 0.45f, 0.85f); break;
    case CMD_VERTEX3F: glColor3f(0.40f, 0.90f, 0.40f); break;
    case CMD_NORMAL3F: glColor3f(0.40f, 0.80f, 0.95f); break;
    case CMD_COLOR3F:
    case CMD_COLOR4F:  glColor3f(0.95f, 0.85f, 0.30f); break;
    case CMD_FOR_BEGIN:
    case CMD_FOR_END:  glColor3f(0.95f, 0.60f, 0.30f); break;
    case CMD_COMMENT:    glColor3f(0.45f, 0.50f, 0.45f); break;
    case CMD_VAR_ASSIGN: glColor3f(0.55f, 0.80f, 0.95f); break;
    default:             glColor3f(0.70f, 0.70f, 0.70f); break;
    }
}

/* ========================================================================= */
/* Code panel                                                                 */
/* ========================================================================= */

static void render_edit_line(int x, int y, int indent_chars) {
    int indent_px = indent_chars * FONT_W;

    /* Highlight background for active line */
    glEnable(GL_BLEND);
    glColor4f(0.15f, 0.18f, 0.28f, 0.70f);
    int panel_w = (int)(g_win_w * g_panel_frac);
    draw_quad(0, (float)(y - 3), (float)panel_w, (float)(LINE_H));
    glDisable(GL_BLEND);

    /* Indent (dimmed) */
    glColor3f(0.30f, 0.30f, 0.38f);
    { char spaces[16];
      memset(spaces, ' ', indent_chars);
      spaces[indent_chars] = '\0';
      draw_string((float)x, (float)y, spaces, FONT_MONO); }

    /* User-typed text */
    glColor3f(0.95f, 0.95f, 0.90f);
    draw_string((float)(x + indent_px), (float)y, g_input, FONT_MONO);

    /* Ghost autocomplete text (only when cursor is at end) */
    if (g_ac_ghost[0] && g_cursor_pos == g_input_len) {
        glEnable(GL_BLEND);
        glColor4f(0.50f, 0.55f, 0.65f, 0.55f);
        draw_string((float)(x + indent_px + g_input_len * FONT_W),
                    (float)y, g_ac_ghost, FONT_MONO);
        glDisable(GL_BLEND);
    }

    /* Blinking cursor at g_cursor_pos */
    if (g_cursor_on) {
        int cx = x + indent_px + g_cursor_pos * FONT_W;
        glEnable(GL_BLEND);
        glColor4f(0.90f, 0.80f, 0.25f, 0.85f);
        draw_quad((float)cx, (float)(y - 2), 2.0f, (float)(FONT_H + 2));
        glDisable(GL_BLEND);
    }

    /* Save cursor screen position for autocomplete popup */
    g_cursor_px = x + indent_px;
    g_cursor_py = y;
}

static void render_code_panel(void) {
    int panel_w = (int)(g_win_w * g_panel_frac);
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;
    int visible_lines = (g_win_h - 2 * CODE_MARGIN_Y - LINE_H) / LINE_H;

    int n_hpre = 0;
    for (int i = 0; g_header_pre[i]; i++) n_hpre++;
    int n_hpost = 0;
    for (int i = 0; g_header_post[i]; i++) n_hpost++;
    int n_header = n_hpre + 3 + n_hpost;  /* pre + gluLookAt + post */
    int n_footer = 0;
    for (int i = 0; g_footer[i]; i++) n_footer++;
    /* +1 for the new-line slot, +1 if inserting */
    int total_lines = n_header + g_num_cmds + (g_inserting ? 1 : 0)
                    + 1 + n_footer;

    /* Which document line is the cursor on? (offset by header) */
    int cursor_doc_line = n_header + g_edit_line;

    /* Clamp scroll */
    int max_scroll = total_lines - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    /* Auto-scroll to keep cursor visible */
    if (cursor_doc_line < g_scroll)
        g_scroll = cursor_doc_line;
    if (cursor_doc_line >= g_scroll + visible_lines)
        g_scroll = cursor_doc_line - visible_lines + 1;

    begin_2d();

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.06f, 0.06f, 0.10f, 0.92f);
    draw_quad(0, 0, (float)panel_w, (float)g_win_h);

    /* Border */
    glColor4f(0.30f, 0.30f, 0.50f, 0.80f);
    glBegin(GL_LINES);
    glVertex2f((float)panel_w, 0);
    glVertex2f((float)panel_w, (float)g_win_h);
    glEnd();
    glDisable(GL_BLEND);

    /* Top info bar */
    {
        char info[256];
        int nv = count_vertices();
        if (g_inserting) {
            snprintf(info, sizeof(info),
                     "F1:Help | %d cmds | %d verts | Ln %d [INSERT]",
                     g_num_cmds, nv, g_edit_line + 1);
        } else if (in_begin_block()) {
            snprintf(info, sizeof(info),
                     "F1:Help | %d cmds | %d verts | %s | Ln %d",
                     g_num_cmds, nv, mode_name(current_begin_mode()),
                     g_edit_line + 1);
        } else {
            snprintf(info, sizeof(info),
                     "F1:Help | %d cmds | %d verts | Ln %d",
                     g_num_cmds, nv, g_edit_line + 1);
        }
        glColor3f(0.50f, 0.55f, 0.65f);
        draw_string(CODE_MARGIN_X, g_win_h - CODE_MARGIN_Y - 2, info,
                    FONT_SMALL);
    }

    /* Code lines */
    int line_y = g_win_h - CODE_MARGIN_Y - LINE_H - LINE_H;
    int cur = 0;
    int file_line = 1;

    /* Macro for rendering a static line (header/footer) */
    #define RENDER_STATIC_LINE(text, set_color) do {                           \
        if (cur >= g_scroll && cur < g_scroll + visible_lines) {               \
            glColor3f(0.30f, 0.30f, 0.38f);                                   \
            { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);         \
              draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }             \
            set_color;                                                          \
            draw_string(text_x, line_y, text, FONT_MONO);                      \
            line_y -= LINE_H;                                                   \
        }                                                                       \
        file_line++;                                                            \
        cur++;                                                                  \
    } while (0)

    /* Header pre-lookAt (dimmed) */
    for (int i = 0; g_header_pre[i]; i++) {
        RENDER_STATIC_LINE(g_header_pre[i], glColor3f(0.38f, 0.38f, 0.42f));
    }
    /* gluLookAt lines (slightly brighter - dynamic) */
    for (int i = 0; i < 3; i++) {
        RENDER_STATIC_LINE(g_lookat[i], glColor3f(0.50f, 0.45f, 0.55f));
    }
    /* Header post-lookAt */
    for (int i = 0; g_header_post[i]; i++) {
        RENDER_STATIC_LINE(g_header_post[i], glColor3f(0.38f, 0.38f, 0.42f));
    }

    /* Commands + insert line + new-line slot */
    for (int i = 0; i <= g_num_cmds; i++) {
        /* If inserting, render the virtual insert line before command[g_edit_line] */
        if (g_inserting && i == g_edit_line) {
            if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                glColor3f(0.55f, 0.55f, 0.30f);
                { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                  draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                int ind = in_begin_block_at(i) ? 4 : 2;
                render_edit_line(text_x, line_y, ind);
                line_y -= LINE_H;
            }
            file_line++;
            cur++;
        }

        if (i < g_num_cmds) {
            int is_edit = (!g_inserting && i == g_edit_line);
            if (is_edit) {
                /* Active editing line */
                if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                    glColor3f(0.55f, 0.55f, 0.30f);
                    { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                      draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                    if (g_show_indices) {
                        char idx_s[16]; snprintf(idx_s, sizeof(idx_s), "[%d]", i);
                        glColor3f(0.45f, 0.50f, 0.65f);
                        draw_string((float)idx_x, (float)line_y, idx_s, FONT_MONO);
                    }
                    int ind = in_begin_block_at(i) ? 4 : 2;
                    render_edit_line(text_x, line_y, ind);
                    line_y -= LINE_H;
                }
                file_line++;
                cur++;
            } else {
                /* Existing command, not being edited */
                if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                    /* Selection highlight */
                    if (sel_active() && i >= sel_lo() && i <= sel_hi()) {
                        glEnable(GL_BLEND);
                        glColor4f(0.20f, 0.30f, 0.50f, 0.55f);
                        draw_quad(0, (float)(line_y - 3),
                                  (float)panel_w, (float)LINE_H);
                        glDisable(GL_BLEND);
                    }
                    glColor3f(0.30f, 0.30f, 0.38f);
                    { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                      draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                    if (g_show_indices) {
                        char idx_s[16]; snprintf(idx_s, sizeof(idx_s), "[%d]", i);
                        glColor3f(0.45f, 0.50f, 0.65f);
                        draw_string((float)idx_x, (float)line_y, idx_s, FONT_MONO);
                    }
                    color_for_type(g_cmds[i].type);
                    draw_string((float)text_x, (float)line_y,
                                g_cmds[i].source, FONT_MONO);
                    line_y -= LINE_H;
                }
                file_line++;
                cur++;
            }
        } else {
            /* i == g_num_cmds: new-line slot */
            int is_edit_nl = (!g_inserting && g_edit_line == g_num_cmds);
            if (is_edit_nl) {
                if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                    glColor3f(0.55f, 0.55f, 0.30f);
                    { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                      draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                    int ind = in_begin_block() ? 4 : 2;
                    render_edit_line(text_x, line_y, ind);
                    line_y -= LINE_H;
                }
            } else {
                if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                    glColor3f(0.30f, 0.30f, 0.38f);
                    { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                      draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                    glColor3f(0.28f, 0.28f, 0.35f);
                    const char *ind_s = in_begin_block() ? "    " : "  ";
                    draw_string((float)text_x, (float)line_y, ind_s, FONT_MONO);
                    line_y -= LINE_H;
                }
            }
            file_line++;
            cur++;
        }
    }

    /* Footer (dimmed) */
    for (int i = 0; g_footer[i]; i++) {
        RENDER_STATIC_LINE(g_footer[i], glColor3f(0.38f, 0.38f, 0.42f));
    }

    #undef RENDER_STATIC_LINE

    /* Scroll indicator */
    if (total_lines > visible_lines) {
        int bar_h = g_win_h - 2 * CODE_MARGIN_Y - LINE_H;
        float frac = (float)visible_lines / (float)total_lines;
        float pos  = (float)g_scroll / (float)total_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = g_win_h - CODE_MARGIN_Y - LINE_H
                      - (int)(bar_h * pos) - thumb_h;

        glEnable(GL_BLEND);
        glColor4f(0.50f, 0.50f, 0.65f, 0.35f);
        draw_quad((float)(panel_w - 6), (float)thumb_y,
                  5.0f, (float)thumb_h);
        glDisable(GL_BLEND);
    }

    /* Status bar */
    if (g_status_ttl > 0) {
        float alpha = g_status_ttl > 60 ? 1.0f : (float)g_status_ttl / 60.0f;
        glEnable(GL_BLEND);
        glColor4f(0.12f, 0.12f, 0.05f, 0.92f * alpha);
        draw_quad(0, 0, (float)panel_w, (float)(LINE_H + 6));
        glColor4f(1.0f, 0.85f, 0.20f, alpha);
        draw_string(CODE_MARGIN_X, 5, g_status, FONT_MONO);
        glDisable(GL_BLEND);
    }

    end_2d();
}

/* ========================================================================= */
/* Autocomplete popup                                                         */
/* ========================================================================= */

static void render_autocomplete(void) {
    if (g_ac_count < 1) return;

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int popup_x = g_cursor_px;
    int popup_y = g_cursor_py - LINE_H - 4;

    /* Calculate popup width from longest match */
    int max_w = 0;
    for (int i = 0; i < g_ac_count; i++) {
        int w = (int)strlen(g_ac_matches[i]) * FONT_W;
        if (w > max_w) max_w = w;
    }
    int popup_w = max_w + 16;
    int popup_h = g_ac_count * LINE_H + 6;

    /* Clamp to panel width */
    int panel_w = (int)(g_win_w * g_panel_frac);
    if (popup_x + popup_w > panel_w - 4)
        popup_x = panel_w - popup_w - 4;
    if (popup_x < 4) popup_x = 4;

    /* Background */
    glColor4f(0.08f, 0.08f, 0.15f, 0.95f);
    draw_quad((float)popup_x, (float)(popup_y - popup_h),
              (float)popup_w, (float)popup_h);

    /* Border */
    glColor4f(0.40f, 0.40f, 0.65f, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)popup_x, (float)(popup_y - popup_h));
    glVertex2f((float)(popup_x + popup_w), (float)(popup_y - popup_h));
    glVertex2f((float)(popup_x + popup_w), (float)popup_y);
    glVertex2f((float)popup_x, (float)popup_y);
    glEnd();

    /* Entries */
    int ey = popup_y - LINE_H + 1;
    for (int i = 0; i < g_ac_count; i++) {
        if (i == g_ac_sel) {
            /* Highlight selected */
            glColor4f(0.20f, 0.25f, 0.42f, 0.90f);
            draw_quad((float)(popup_x + 1), (float)(ey - 2),
                      (float)(popup_w - 2), (float)LINE_H);
            glColor3f(1.0f, 1.0f, 0.90f);
        } else {
            glColor3f(0.65f, 0.65f, 0.72f);
        }
        draw_string((float)(popup_x + 8), (float)ey,
                    g_ac_matches[i], FONT_MONO);
        ey -= LINE_H;
    }

    /* Hint text */
    glColor4f(0.45f, 0.45f, 0.55f, 0.70f);
    draw_string((float)(popup_x + 4),
                (float)(popup_y - popup_h - FONT_H - 2),
                "Tab to accept", FONT_SMALL);

    glDisable(GL_BLEND);
    end_2d();
}

/* ========================================================================= */
/* Help overlay                                                               */
/* ========================================================================= */

static void render_help(void) {
    if (!g_show_help) return;

    static const char *text[] = {
        "=== OpenGL REPL - Help ===",
        "Press F1 or Escape to close.  Up/Down or PgUp/PgDn to scroll.",
        "",
        "Supported Commands (type + ;):",
        "  glBegin(MODE)        GL_TRIANGLES, GL_TRIANGLE_STRIP, ...",
        "  glEnd()              End current primitive block",
        "  glVertex3f(x,y,z)    Specify a vertex position",
        "  glNormal3f(x,y,z)    Specify a vertex normal",
        "  glColor3f(r,g,b)     Specify vertex color",
        "  glColor4f(r,g,b,a)   Specify color with alpha",
        "  glEnable(CAP)        GL_DEPTH_TEST, GL_LIGHTING, ...",
        "  glDisable(CAP)       GL_COLOR_MATERIAL, GL_NORMALIZE",
        "  glShadeModel(MODE)   GL_SMOOTH, GL_FLAT",
        "",
        "Comments:",
        "  // text              Type directly to add a comment line",
        "  Ctrl+/               Toggle comment on current line",
        "",
        "Math Expressions (use anywhere floats are expected):",
        "  Constants:  PI, TAU          Functions: sin cos tan sqrt abs pow",
        "  Operators:  + - * / ( )      Also: min max floor ceil fmod",
        "  Example:    glVertex3f(cos(PI/4), sin(PI/4), 0)",
        "",
        "Variables (predefined: x, y, z, i, j, k, n, t):",
        "  x = 1.5;                    Assign a value to a variable",
        "  glVertex3f(x, y, z);        Use variables in expressions",
        "  Variables persist across commands and are saved/loaded",
        "",
        "For-Loops (stored as editable blocks, saved as C for-loops):",
        "  for(i, 0, 24) glVertex3f(cos(i*TAU/24), sin(i*TAU/24), 0);",
        "  for(i, 0, N) {              Multi-line block:",
        "    glNormal3f(...)              type body lines, end with }",
        "    glVertex3f(...)              or press Esc to exit",
        "  }",
        "  Nesting supported up to 4 levels (for spheres, tori, etc.)",
        "  Ctrl+S preserves loop structure in output.c (not expanded)",
        "",
        "Editing:",
        "  Up / Down            Navigate lines (scroll help when open)",
        "  Left / Right         Move cursor within line",
        "  Home / End           Jump to start / end of line",
        "  Type + ;             Commit line (edit existing or append new)",
        "  Enter                Insert new line (even in middle of list)",
        "  Tab / Enter          Accept autocomplete suggestion",
        "  Backspace            Delete character before cursor",
        "  Ctrl+/               Toggle comment on current line",
        "  Shift+Up/Down        Select multiple lines",
        "  Ctrl+C               Copy line/selection (for-loop on BEGIN)",
        "  Ctrl+X               Cut line/selection (for-loop on BEGIN)",
        "  Ctrl+V               Paste before current line",
        "  Ctrl+Z               Undo last command",
        "  Ctrl+D               Delete line at cursor",
        "  Ctrl+L               Clear all commands",
        "  Ctrl+S               Save to output.c",
        "  Ctrl+Q               Exit and save to temporary file",
        "  Escape               Clear input / exit insert / close help",
        "",
        "Camera:",
        "  Left-drag            Orbit",
        "  Right-drag           Pan",
        "  Scroll wheel         Zoom",
        "",
        "Toggles:",
        "  F1  Help overlay     F2  Wireframe mode",
        "  F3  Grid theme       F4  Axes theme",
        "  F5  Vertex numbers   F6  Normal vectors",
        "  F7  Command indices  F8  Vertex guides",
        "  F9  Auto-normals     F10 Light indicators",
        "  F11 Camera rotate",
        "",
        "  PgUp / PgDn          Scroll code panel (or help when open)",
        "",
        "Save / Load:",
        "  Ctrl+S saves the session to output.c",
        "  Reload a saved file:  ./sample output.c",
        "  (Commands between // Snippet start/end are imported)",
        "",
        NULL
    };

    /* Count total lines */
    int n_lines = 0;
    while (text[n_lines]) n_lines++;

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int hx = g_win_w / 6, hy = g_win_h / 12;
    int hw = g_win_w * 2 / 3, hh = g_win_h * 5 / 6;
    int pad_top = 32, pad_bot = 24;
    int content_h = hh - pad_top - pad_bot;
    int visible_lines = content_h / LINE_H;
    if (visible_lines < 1) visible_lines = 1;

    /* Clamp scroll */
    int max_scroll = n_lines - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (g_help_scroll > max_scroll) g_help_scroll = max_scroll;
    if (g_help_scroll < 0) g_help_scroll = 0;

    /* Background */
    glColor4f(0.03f, 0.03f, 0.06f, 0.94f);
    draw_quad((float)hx, (float)hy, (float)hw, (float)hh);

    /* Border */
    glColor4f(0.45f, 0.45f, 0.75f, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)hx, (float)hy);
    glVertex2f((float)(hx + hw), (float)hy);
    glVertex2f((float)(hx + hw), (float)(hy + hh));
    glVertex2f((float)hx, (float)(hy + hh));
    glEnd();

    /* Scissor clip to content area */
    glEnable(GL_SCISSOR_TEST);
    glScissor(hx + 1, hy + pad_bot, hw - 2, content_h);

    int tx = hx + 24;
    int ty_start = hy + hh - pad_top;

    for (int i = g_help_scroll; i < n_lines && i < g_help_scroll + visible_lines + 1; i++) {
        int ty = ty_start - (i - g_help_scroll) * LINE_H;
        if (ty < hy + pad_bot - LINE_H) break;

        if (text[i][0] == '=')
            glColor3f(0.80f, 0.80f, 1.00f);
        else if (text[i][0] == ' ' && text[i][1] == ' ')
            glColor3f(0.65f, 0.90f, 0.65f);
        else if (text[i][0] == '\0')
            continue;   /* skip blank lines (save space) — still scrolls past them */
        else
            glColor3f(0.75f, 0.75f, 0.80f);

        draw_string((float)tx, (float)ty, text[i], FONT_MONO);
    }

    glDisable(GL_SCISSOR_TEST);

    /* Scroll indicator (only if content overflows) */
    if (n_lines > visible_lines) {
        int bar_x = hx + hw - 10;
        int bar_top = hy + hh - pad_top;
        int bar_h = content_h;
        float frac = (float)visible_lines / (float)n_lines;
        float pos  = (float)g_help_scroll / (float)n_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = bar_top - (int)(bar_h * pos) - thumb_h;

        /* Track */
        glColor4f(0.20f, 0.20f, 0.35f, 0.40f);
        draw_quad((float)bar_x, (float)(bar_top - bar_h), 5.0f, (float)bar_h);

        /* Thumb */
        glColor4f(0.55f, 0.55f, 0.80f, 0.65f);
        draw_quad((float)bar_x, (float)thumb_y, 5.0f, (float)thumb_h);

        /* Scroll hint at bottom */
        if (g_help_scroll < max_scroll) {
            glColor4f(0.50f, 0.50f, 0.65f, 0.50f);
            char hint[32];
            snprintf(hint, sizeof(hint), "v %d more v",
                     n_lines - g_help_scroll - visible_lines);
            int hint_x = hx + (hw - (int)strlen(hint) * FONT_W) / 2;
            draw_string((float)hint_x, (float)(hy + 6), hint, FONT_SMALL);
        }
    }

    glDisable(GL_BLEND);
    end_2d();
}

/* ========================================================================= */
/* 3D scene helpers                                                           */
/* ========================================================================= */

static void draw_grid(void) {
    if (g_grid_theme == 0) return;

    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Nudge grid slightly below Y=0 to avoid z-fighting with axes */
    glPushMatrix();
    glTranslatef(0, -0.002f, 0);

    float breath = sinf(g_anim_time * 0.8f) * 0.5f + 0.5f; /* 0..1 */

    switch (g_grid_theme) {

    case 1: { /* Classic */
        float extent = 5.0f, step = 0.5f;
        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            float a = (fabsf(v) < 0.01f) ? 0.45f : 0.12f;
            glColor4f(0.50f, 0.50f, 0.60f, a);
            glVertex3f(v, 0, -extent); glVertex3f(v, 0, extent);
            glVertex3f(-extent, 0, v); glVertex3f(extent, 0, v);
        }
        glEnd();
        break;
    }

    case 2: { /* Fog — large grid, breathing fog density */
        float extent = 15.0f, step = 0.5f;
        float fog_density = 0.06f + breath * 0.04f;
        GLfloat fog_col[] = { 0.10f, 0.10f, 0.13f, 1.0f };
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, GL_EXP2);
        glFogfv(GL_FOG_COLOR, fog_col);
        glFogf(GL_FOG_DENSITY, fog_density);

        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            int is_major = (fabsf(fmodf(fabsf(v) + 0.01f, 2.0f)) < 0.1f);
            int is_origin = (fabsf(v) < 0.01f);
            float a = is_origin ? 0.55f : (is_major ? 0.25f : 0.10f);
            glColor4f(0.45f, 0.50f, 0.65f, a);
            glVertex3f(v, 0, -extent); glVertex3f(v, 0, extent);
            glVertex3f(-extent, 0, v); glVertex3f(extent, 0, v);
        }
        glEnd();

        glDisable(GL_FOG);
        break;
    }

    case 3: { /* Tron — cyan/blue glow, distance fade */
        float extent = 12.0f, step = 0.5f;
        float glow = 0.7f + breath * 0.3f;

        glLineWidth(1.0f);
        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            float dist = fabsf(v) / extent;
            float fade = (1.0f - dist * dist);
            if (fade < 0.0f) fade = 0.0f;
            int is_origin = (fabsf(v) < 0.01f);
            int is_major = (fabsf(fmodf(fabsf(v) + 0.01f, 2.0f)) < 0.1f);
            float base = is_origin ? 0.8f : (is_major ? 0.35f : 0.12f);
            float a = base * fade * glow;
            float r = 0.05f, g = is_origin ? 0.9f : 0.55f, b = 0.95f;
            glColor4f(r, g, b, a);
            /* Draw both directions with distance fade per-endpoint */
            glVertex3f(v, 0, -extent); glVertex3f(v, 0, extent);
            glVertex3f(-extent, 0, v); glVertex3f(extent, 0, v);
        }
        glEnd();

        /* Subtle glow line on axes */
        glLineWidth(2.0f);
        float ga = 0.25f * glow;
        glBegin(GL_LINES);
        glColor4f(0.0f, 0.8f, 1.0f, ga);
        glVertex3f(0, 0, -extent); glVertex3f(0, 0, extent);
        glVertex3f(-extent, 0, 0); glVertex3f(extent, 0, 0);
        glEnd();
        glLineWidth(1.0f);
        break;
    }

    case 4: { /* Ember — warm orange/red, pulsing ripple */
        float extent = 12.0f, step = 0.5f;

        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            float dist = fabsf(v) / extent;
            /* Ripple: wave traveling outward from center */
            float ripple = sinf(dist * 12.0f - g_anim_time * 2.5f);
            ripple = ripple * 0.5f + 0.5f; /* 0..1 */
            float fade = 1.0f - dist;
            if (fade < 0.0f) fade = 0.0f;
            int is_origin = (fabsf(v) < 0.01f);
            int is_major = (fabsf(fmodf(fabsf(v) + 0.01f, 2.0f)) < 0.1f);
            float base = is_origin ? 0.7f : (is_major ? 0.30f : 0.10f);
            float a = base * fade * (0.6f + ripple * 0.4f);
            float r = 0.95f, g = 0.35f + ripple * 0.25f, b = 0.05f;
            glColor4f(r, g, b, a);
            glVertex3f(v, 0, -extent); glVertex3f(v, 0, extent);
            glVertex3f(-extent, 0, v); glVertex3f(extent, 0, v);
        }
        glEnd();
        break;
    }

    case 5: { /* Faint — very subtle 0.5 reference lines */
        float extent = 5.0f, step = 0.5f;
        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            int is_origin = (fabsf(v) < 0.01f);
            int is_major = (fabsf(fmodf(fabsf(v) + 0.01f, 1.0f)) < 0.1f);
            float a = is_origin ? 0.18f : (is_major ? 0.07f : 0.03f);
            glColor4f(0.50f, 0.50f, 0.60f, a);
            glVertex3f(v, 0, -extent); glVertex3f(v, 0, extent);
            glVertex3f(-extent, 0, v); glVertex3f(extent, 0, v);
        }
        glEnd();
        break;
    }

    case 6: { /* Focus — fades rapidly around selected vertex */
        /* Update focus vertex from current or nearest vertex line */
        if (g_edit_line < g_num_cmds &&
            g_cmds[g_edit_line].valid &&
            g_cmds[g_edit_line].type == CMD_VERTEX3F) {
            g_focus_vtx[0] = g_cmds[g_edit_line].args[0];
            g_focus_vtx[1] = g_cmds[g_edit_line].args[1];
            g_focus_vtx[2] = g_cmds[g_edit_line].args[2];
            g_focus_vtx_valid = 1;
        } else if (!g_focus_vtx_valid) {
            /* Scan backwards from edit line for nearest vertex */
            for (int i = g_edit_line - 1; i >= 0; i--) {
                if (g_cmds[i].valid && g_cmds[i].type == CMD_VERTEX3F) {
                    g_focus_vtx[0] = g_cmds[i].args[0];
                    g_focus_vtx[1] = g_cmds[i].args[1];
                    g_focus_vtx[2] = g_cmds[i].args[2];
                    g_focus_vtx_valid = 1;
                    break;
                }
            }
        }

        float cx = g_focus_vtx[0], cz = g_focus_vtx[2];
        float extent = 8.0f, step = 0.5f;
        float radius = 3.0f;  /* fade-out radius */

        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            int is_origin = (fabsf(v) < 0.01f);
            int is_major = (fabsf(fmodf(fabsf(v) + 0.01f, 1.0f)) < 0.1f);
            float base = is_origin ? 0.40f : (is_major ? 0.18f : 0.06f);

            /* Vertical line at x=v: fade based on distance from cx */
            float dx = v - cx;
            float fx = 1.0f - (dx * dx) / (radius * radius);
            if (fx < 0.0f) fx = 0.0f;
            fx = fx * fx;  /* sharper falloff */
            if (fx > 0.001f) {
                glColor4f(0.50f, 0.55f, 0.70f, base * fx);
                /* Clamp line Z extent around focus */
                float z0 = cz - radius, z1 = cz + radius;
                if (z0 < -extent) z0 = -extent;
                if (z1 > extent) z1 = extent;
                glVertex3f(v, 0, z0); glVertex3f(v, 0, z1);
            }

            /* Horizontal line at z=v: fade based on distance from cz */
            float dz = v - cz;
            float fz = 1.0f - (dz * dz) / (radius * radius);
            if (fz < 0.0f) fz = 0.0f;
            fz = fz * fz;
            if (fz > 0.001f) {
                glColor4f(0.50f, 0.55f, 0.70f, base * fz);
                float x0 = cx - radius, x1 = cx + radius;
                if (x0 < -extent) x0 = -extent;
                if (x1 > extent) x1 = extent;
                glVertex3f(x0, 0, v); glVertex3f(x1, 0, v);
            }
        }
        glEnd();

        /* Crosshair at focus point */
        if (g_focus_vtx_valid) {
            glLineWidth(1.5f);
            glBegin(GL_LINES);
            glColor4f(0.80f, 0.85f, 0.95f, 0.25f);
            glVertex3f(cx - 0.3f, 0, cz);
            glVertex3f(cx + 0.3f, 0, cz);
            glVertex3f(cx, 0, cz - 0.3f);
            glVertex3f(cx, 0, cz + 0.3f);
            glEnd();
            glLineWidth(1.0f);
        }
        break;
    }

    default: break;
    }

    glPopMatrix();
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

/* Helper: draw an axis label at a 3D position */
static void draw_axis_label(float x, float y, float z, char ch,
                            float r, float g, float b) {
    glColor3f(r, g, b);
    glRasterPos3f(x, y, z);
    glutBitmapCharacter(FONT_MONO, ch);
}

static void draw_axes(void) {
    if (g_axes_theme == 0) return;

    glDisable(GL_LIGHTING);
//    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float breath = sinf(g_anim_time * 0.8f) * 0.5f + 0.5f; /* 0..1 */

    switch (g_axes_theme) {

    case 1: { /* Classic */
        float len = 2.0f;
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glColor3f(0.90f, 0.20f, 0.20f);
        glVertex3f(0, 0, 0); glVertex3f(len, 0, 0);
        glColor3f(0.20f, 0.90f, 0.20f);
        glVertex3f(0, 0, 0); glVertex3f(0, len, 0);
        glColor3f(0.20f, 0.20f, 0.90f);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, len);
        glEnd();
        glLineWidth(1.0f);

        draw_axis_label(2.15f, 0, 0, 'X', 0.90f, 0.30f, 0.30f);
        draw_axis_label(0, 2.15f, 0, 'Y', 0.30f, 0.90f, 0.30f);
        draw_axis_label(0, 0, 2.15f, 'Z', 0.30f, 0.30f, 0.90f);
        break;
    }

    case 2: { /* Pulse — dot traveling outward along each axis */
        float len = 3.0f;
        /* Solid dim axes */
        glLineWidth(1.5f);
        glBegin(GL_LINES);
        glColor4f(0.90f, 0.20f, 0.20f, 0.30f);
        glVertex3f(0, 0, 0); glVertex3f(len, 0, 0);
        glColor4f(0.20f, 0.90f, 0.20f, 0.30f);
        glVertex3f(0, 0, 0); glVertex3f(0, len, 0);
        glColor4f(0.20f, 0.20f, 0.90f, 0.30f);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, len);
        glEnd();
        glLineWidth(1.0f);

        /* Pulsing dot position (loops 0..1) */
        float t = fmodf(g_anim_time * 0.6f, 1.0f);
        float pos = t * len;
        float glow = sinf(t * (float)M_PI); /* bright in middle, dim at ends */
        glow = glow * 0.8f + 0.2f;

        glPointSize(8.0f);
        glBegin(GL_POINTS);
        glColor4f(1.0f, 0.3f, 0.3f, glow);
        glVertex3f(pos, 0, 0);
        glColor4f(0.3f, 1.0f, 0.3f, glow);
        glVertex3f(0, pos, 0);
        glColor4f(0.3f, 0.3f, 1.0f, glow);
        glVertex3f(0, 0, pos);
        glEnd();
        glPointSize(1.0f);

        /* Bright trail behind the dot */
        glLineWidth(3.0f);
        float trail = 0.6f;
        float t0 = pos - trail;
        if (t0 < 0) t0 = 0;
        glBegin(GL_LINES);
        glColor4f(1.0f, 0.3f, 0.3f, 0.05f);
        glVertex3f(t0, 0, 0);
        glColor4f(1.0f, 0.3f, 0.3f, glow * 0.7f);
        glVertex3f(pos, 0, 0);

        glColor4f(0.3f, 1.0f, 0.3f, 0.05f);
        glVertex3f(0, t0, 0);
        glColor4f(0.3f, 1.0f, 0.3f, glow * 0.7f);
        glVertex3f(0, pos, 0);

        glColor4f(0.3f, 0.3f, 1.0f, 0.05f);
        glVertex3f(0, 0, t0);
        glColor4f(0.3f, 0.3f, 1.0f, glow * 0.7f);
        glVertex3f(0, 0, pos);
        glEnd();
        glLineWidth(1.0f);

        draw_axis_label(len + 0.15f, 0, 0, 'X', 0.70f, 0.25f, 0.25f);
        draw_axis_label(0, len + 0.15f, 0, 'Y', 0.25f, 0.70f, 0.25f);
        draw_axis_label(0, 0, len + 0.15f, 'Z', 0.25f, 0.25f, 0.70f);
        break;
    }

    case 3: { /* Neon — bright glowing axes with breathing intensity */
        float len = 2.5f;
        float glow = 0.6f + breath * 0.4f;

        /* Outer glow (wide, dim) */
        glLineWidth(6.0f);
        glBegin(GL_LINES);
        glColor4f(1.0f, 0.1f, 0.1f, 0.12f * glow);
        glVertex3f(0, 0, 0); glVertex3f(len, 0, 0);
        glColor4f(0.1f, 1.0f, 0.1f, 0.12f * glow);
        glVertex3f(0, 0, 0); glVertex3f(0, len, 0);
        glColor4f(0.1f, 0.1f, 1.0f, 0.12f * glow);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, len);
        glEnd();

        /* Core (narrow, bright) */
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glColor4f(1.0f, 0.4f, 0.4f, 1.0f * glow);
        glVertex3f(0, 0, 0); glVertex3f(len, 0, 0);
        glColor4f(0.4f, 1.0f, 0.4f, 1.0f * glow);
        glVertex3f(0, 0, 0); glVertex3f(0, len, 0);
        glColor4f(0.4f, 0.4f, 1.0f, 1.0f * glow);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, len);
        glEnd();
        glLineWidth(1.0f);

        /* Bright tip dots */
        glPointSize(6.0f);
        glBegin(GL_POINTS);
        glColor4f(1.0f, 0.5f, 0.5f, glow);
        glVertex3f(len, 0, 0);
        glColor4f(0.5f, 1.0f, 0.5f, glow);
        glVertex3f(0, len, 0);
        glColor4f(0.5f, 0.5f, 1.0f, glow);
        glVertex3f(0, 0, len);
        glEnd();
        glPointSize(1.0f);

        float la = 0.5f + glow * 0.5f;
        draw_axis_label(len + 0.15f, 0, 0, 'X', 1.0f * la, 0.3f * la, 0.3f * la);
        draw_axis_label(0, len + 0.15f, 0, 'Y', 0.3f * la, 1.0f * la, 0.3f * la);
        draw_axis_label(0, 0, len + 0.15f, 'Z', 0.3f * la, 0.3f * la, 1.0f * la);
        break;
    }

    case 4: { /* Compass — positive and negative axes, dashed negative */
        float len = 2.5f;

        /* Positive axes (solid) */
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glColor4f(1.0f, 0.30f, 0.30f, 0.85f);
        glVertex3f(0, 0, 0); glVertex3f(len, 0, 0);
        glColor4f(0.30f, 1.0f, 0.30f, 0.85f);
        glVertex3f(0, 0, 0); glVertex3f(0, len, 0);
        glColor4f(0.30f, 0.30f, 1.0f, 0.85f);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, len);
        glEnd();

        /* Negative axes (stippled) */
        glEnable(GL_LINE_STIPPLE);
        glLineStipple(2, 0xAAAA);
        glBegin(GL_LINES);
        glColor4f(1.0f, 0.30f, 0.30f, 0.35f);
        glVertex3f(0, 0, 0); glVertex3f(-len, 0, 0);
        glColor4f(0.30f, 1.0f, 0.30f, 0.35f);
        glVertex3f(0, 0, 0); glVertex3f(0, -len, 0);
        glColor4f(0.30f, 0.30f, 1.0f, 0.35f);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, -len);
        glEnd();
        glDisable(GL_LINE_STIPPLE);
        glLineWidth(1.0f);

        /* Arrowheads at positive tips */
        glPointSize(7.0f);
        glBegin(GL_POINTS);
        glColor4f(1.0f, 0.4f, 0.4f, 0.9f);
        glVertex3f(len, 0, 0);
        glColor4f(0.4f, 1.0f, 0.4f, 0.9f);
        glVertex3f(0, len, 0);
        glColor4f(0.4f, 0.4f, 1.0f, 0.9f);
        glVertex3f(0, 0, len);
        glEnd();

        /* Small dots at negative tips */
        glPointSize(4.0f);
        glBegin(GL_POINTS);
        glColor4f(1.0f, 0.3f, 0.3f, 0.30f);
        glVertex3f(-len, 0, 0);
        glColor4f(0.3f, 1.0f, 0.3f, 0.30f);
        glVertex3f(0, -len, 0);
        glColor4f(0.3f, 0.3f, 1.0f, 0.30f);
        glVertex3f(0, 0, -len);
        glEnd();
        glPointSize(1.0f);

        /* Origin sphere-ish dot */
        glPointSize(5.0f);
        glBegin(GL_POINTS);
        glColor4f(0.9f, 0.9f, 0.9f, 0.6f);
        glVertex3f(0, 0, 0);
        glEnd();
        glPointSize(1.0f);

        draw_axis_label(len + 0.15f, 0, 0, 'X', 0.90f, 0.30f, 0.30f);
        draw_axis_label(0, len + 0.15f, 0, 'Y', 0.30f, 0.90f, 0.30f);
        draw_axis_label(0, 0, len + 0.15f, 'Z', 0.30f, 0.30f, 0.90f);
        draw_axis_label(-len - 0.15f, 0, 0, 'x', 0.55f, 0.25f, 0.25f);
        draw_axis_label(0, -len - 0.15f, 0, 'y', 0.25f, 0.55f, 0.25f);
        draw_axis_label(0, 0, -len - 0.15f, 'z', 0.25f, 0.25f, 0.55f);
        break;
    }

    default: break;
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
}

static void draw_vertex_numbers(void) {
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glColor3f(1.0f, 1.0f, 0.30f);

    /* Find which glBegin/glEnd block (in g_cmds) the cursor is in */
    int target_block = -1;
    {
        int block = -1;
        int in_block = 0;
        for (int i = 0; i < g_num_cmds; i++) {
            if (!g_cmds[i].valid) continue;
            if (g_cmds[i].type == CMD_BEGIN) {
                block++;
                in_block = 1;
            }
            if (in_block && i == g_edit_line) {
                target_block = block;
                break;
            }
            if (g_cmds[i].type == CMD_END) {
                if (i == g_edit_line) {
                    target_block = block;
                    break;
                }
                in_block = 0;
            }
        }
    }

    /* Draw vertex labels only for the target block in g_flat_cmds */
    int block = -1;
    int in_block = 0;
    int vn = 0;
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_BEGIN) {
            block++;
            in_block = 1;
            vn = 0;
        } else if (g_flat_cmds[i].type == CMD_END) {
            in_block = 0;
        } else if (g_flat_cmds[i].type == CMD_VERTEX3F) {
            if (in_block && block == target_block) {
                char label[16];
                snprintf(label, sizeof(label), " v%d", vn);
                glRasterPos3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                              g_flat_cmds[i].args[2]);
                for (const char *c = label; *c; c++)
                    glutBitmapCharacter(FONT_MONO, (unsigned char)*c);
            }
            vn++;
        }
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
}

static void draw_normal_vectors(void) {
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glColor3f(0.80f, 0.80f, 0.30f);
    float scale = 0.35f;
    float nx = 0, ny = 0, nz = 1;

    glBegin(GL_LINES);
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_NORMAL3F) {
            nx = g_flat_cmds[i].args[0]; ny = g_flat_cmds[i].args[1];
            nz = g_flat_cmds[i].args[2];
        } else if (g_flat_cmds[i].type == CMD_VERTEX3F) {
            float vx = g_flat_cmds[i].args[0], vy = g_flat_cmds[i].args[1],
                  vz = g_flat_cmds[i].args[2];
            glVertex3f(vx, vy, vz);
            glVertex3f(vx + nx * scale, vy + ny * scale, vz + nz * scale);
        }
    }
    glEnd();

    glPointSize(4.0f);
    glBegin(GL_POINTS);
    nx = 0; ny = 0; nz = 1;
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (g_flat_cmds[i].type == CMD_NORMAL3F) {
            nx = g_flat_cmds[i].args[0]; ny = g_flat_cmds[i].args[1];
            nz = g_flat_cmds[i].args[2];
        } else if (g_flat_cmds[i].type == CMD_VERTEX3F) {
            glVertex3f(g_flat_cmds[i].args[0] + nx * scale,
                       g_flat_cmds[i].args[1] + ny * scale,
                       g_flat_cmds[i].args[2] + nz * scale);
        }
    }
    glEnd();
    glPointSize(1.0f);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
}

/* ========================================================================= */
/* Vertex input guides — plane / line / point for partial glVertex3f args     */
/* ========================================================================= */

static void draw_vertex_guides(void) {
    if (!g_show_guides) return;

    /* Check current input for a partial glVertex3f( */
    if (strncmp(g_input, "glVertex3f(", 11) != 0 || g_input_len <= 11)
        return;

    float vals[3];
    int n = parse_exprs(g_input + 11, vals, 3, NULL, 0);
    if (n < 1) return;

    float sz = 3.0f;  /* half-size of guide geometry */

    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (n == 1) {
        /* One axis (x) — draw a YZ plane at x */
        glColor4f(0.3f, 0.6f, 1.0f, 0.15f);
        glBegin(GL_QUADS);
        glVertex3f(vals[0], -sz, -sz);
        glVertex3f(vals[0],  sz, -sz);
        glVertex3f(vals[0],  sz,  sz);
        glVertex3f(vals[0], -sz,  sz);
        glEnd();
        /* Outline */
        glColor4f(0.3f, 0.6f, 1.0f, 0.5f);
        glBegin(GL_LINE_LOOP);
        glVertex3f(vals[0], -sz, -sz);
        glVertex3f(vals[0],  sz, -sz);
        glVertex3f(vals[0],  sz,  sz);
        glVertex3f(vals[0], -sz,  sz);
        glEnd();
    } else if (n == 2) {
        /* Two axes (x, y) — draw a line parallel to Z */
        glColor4f(1.0f, 0.8f, 0.2f, 0.7f);
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glVertex3f(vals[0], vals[1], -sz);
        glVertex3f(vals[0], vals[1],  sz);
        glEnd();
        glLineWidth(1.0f);
    }

    if (n >= 3) {
        /* All three axes — draw a point */
        glColor4f(1.0f, 0.3f, 0.3f, 0.9f);
        glPointSize(8.0f);
        glBegin(GL_POINTS);
        glVertex3f(vals[0], vals[1], vals[2]);
        glEnd();
        glPointSize(1.0f);

        /* Show axis line for the component under the cursor */
        int paren_pos = 11; /* position after "glVertex3f(" */
        if (g_cursor_pos >= paren_pos) {
            int close = g_input_len;
            for (int ci = paren_pos; ci < g_input_len; ci++)
                if (g_input[ci] == ')') { close = ci; break; }
            if (g_cursor_pos <= close) {
                int component = 0;
                for (int ci = paren_pos; ci < g_cursor_pos; ci++)
                    if (g_input[ci] == ',') component++;
                if (component > 2) component = 2;

                glLineWidth(2.0f);
                glBegin(GL_LINES);
                switch (component) {
                case 0: /* x-axis */
                    glColor4f(0.9f, 0.2f, 0.2f, 0.7f);
                    glVertex3f(-sz, vals[1], vals[2]);
                    glVertex3f( sz, vals[1], vals[2]);
                    break;
                case 1: /* y-axis */
                    glColor4f(0.2f, 0.9f, 0.2f, 0.7f);
                    glVertex3f(vals[0], -sz, vals[2]);
                    glVertex3f(vals[0],  sz, vals[2]);
                    break;
                case 2: /* z-axis */
                    glColor4f(0.2f, 0.2f, 0.9f, 0.7f);
                    glVertex3f(vals[0], vals[1], -sz);
                    glVertex3f(vals[0], vals[1],  sz);
                    break;
                }
                glEnd();
                glLineWidth(1.0f);
            }
        }
    }

    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

/* ========================================================================= */
/* Normal edit guides — show doubled/halved component impact                  */
/* ========================================================================= */

static void draw_normal_guides(void) {
    if (!g_show_guides) return;

    /* Check current input for glNormal3f( */
    if (strncmp(g_input, "glNormal3f(", 11) != 0 || g_input_len <= 11)
        return;

    float vals[3];
    int n = parse_exprs(g_input + 11, vals, 3, NULL, 0);
    if (n < 3) return;

    /* Determine which component the cursor is on */
    int paren_pos = 11;
    if (g_cursor_pos < paren_pos) return;
    int close = g_input_len;
    for (int ci = paren_pos; ci < g_input_len; ci++)
        if (g_input[ci] == ')') { close = ci; break; }
    if (g_cursor_pos > close) return;

    int component = 0;
    for (int ci = paren_pos; ci < g_cursor_pos; ci++)
        if (g_input[ci] == ',') component++;
    if (component > 2) component = 2;

    /* Find the associated vertex — next CMD_VERTEX3F after current position */
    int search_start = (g_edit_line < g_num_cmds && !g_inserting)
                      ? g_edit_line + 1 : g_edit_line;
    float vx = 0, vy = 0, vz = 0;
    int found = 0;
    for (int i = search_start; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        if (g_cmds[i].type == CMD_VERTEX3F) {
            vx = g_cmds[i].args[0];
            vy = g_cmds[i].args[1];
            vz = g_cmds[i].args[2];
            found = 1;
            break;
        }
        if (g_cmds[i].type == CMD_END || g_cmds[i].type == CMD_BEGIN) break;
    }
    if (!found) return;

    /* Compute doubled and halved normals (re-normalized) */
    float doubled[3] = { vals[0], vals[1], vals[2] };
    float halved[3]  = { vals[0], vals[1], vals[2] };
    doubled[component] *= 2.0f;
    halved[component]  *= 0.5f;

    float dlen = sqrtf(doubled[0]*doubled[0] + doubled[1]*doubled[1]
                      + doubled[2]*doubled[2]);
    if (dlen > 1e-8f) { doubled[0]/=dlen; doubled[1]/=dlen; doubled[2]/=dlen; }
    float hlen = sqrtf(halved[0]*halved[0] + halved[1]*halved[1]
                      + halved[2]*halved[2]);
    if (hlen > 1e-8f) { halved[0]/=hlen; halved[1]/=hlen; halved[2]/=hlen; }

    float scale = 0.45f;

    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Current normal — thin white reference line */
    float clen = sqrtf(vals[0]*vals[0] + vals[1]*vals[1] + vals[2]*vals[2]);
    if (clen > 1e-8f) {
        float cn[3] = { vals[0]/clen, vals[1]/clen, vals[2]/clen };
        glColor4f(0.8f, 0.8f, 0.8f, 0.4f);
        glLineWidth(1.0f);
        glBegin(GL_LINES);
        glVertex3f(vx, vy, vz);
        glVertex3f(vx + cn[0]*scale, vy + cn[1]*scale, vz + cn[2]*scale);
        glEnd();
    }

    /* Doubled component — green stippled arrow */
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(1, 0xAAAA);
    glLineWidth(2.0f);

    glColor4f(0.2f, 0.95f, 0.2f, 0.75f);
    glBegin(GL_LINES);
    glVertex3f(vx, vy, vz);
    glVertex3f(vx + doubled[0]*scale, vy + doubled[1]*scale,
               vz + doubled[2]*scale);
    glEnd();

    /* Halved component — red stippled arrow */
    glColor4f(0.95f, 0.2f, 0.2f, 0.75f);
    glBegin(GL_LINES);
    glVertex3f(vx, vy, vz);
    glVertex3f(vx + halved[0]*scale, vy + halved[1]*scale,
               vz + halved[2]*scale);
    glEnd();

    glDisable(GL_LINE_STIPPLE);
    glLineWidth(1.0f);

    /* Dots at arrow tips */
    glPointSize(5.0f);
    glBegin(GL_POINTS);
    glColor4f(0.2f, 0.95f, 0.2f, 0.85f);
    glVertex3f(vx + doubled[0]*scale, vy + doubled[1]*scale,
               vz + doubled[2]*scale);
    glColor4f(0.95f, 0.2f, 0.2f, 0.85f);
    glVertex3f(vx + halved[0]*scale, vy + halved[1]*scale,
               vz + halved[2]*scale);
    glEnd();
    glPointSize(1.0f);

    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

/* ========================================================================= */
/* Scene lights — setup and visualization                                     */
/* ========================================================================= */

/* Set light properties (position/colors) only — enable/disable is driven
   by the user's REPL commands via execute_commands(). */
static void setup_lights(void) {
    /* Reset all lights to disabled; execute_commands() will enable them */
    for (int i = 0; i < MAX_LIGHTS; i++) {
        glDisable(g_lights[i].id);
        g_lights[i].enabled = 0;
        glLightfv(g_lights[i].id, GL_POSITION, g_lights[i].pos);
        glLightfv(g_lights[i].id, GL_DIFFUSE,  g_lights[i].diffuse);
        glLightfv(g_lights[i].id, GL_AMBIENT,  g_lights[i].ambient);
        glLightfv(g_lights[i].id, GL_SPECULAR, g_lights[i].specular);
    }
}

static void draw_lights(void) {
    if (!g_show_lights) return;

    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float breath = sinf(g_anim_time * 1.2f) * 0.5f + 0.5f;

    for (int i = 0; i < MAX_LIGHTS; i++) {
        float *d = g_lights[i].diffuse;
        float *p = g_lights[i].pos;
        int is_dir = (p[3] == 0.0f);
        int on = g_lights[i].enabled;

        /* Compute display position */
        float lx, ly, lz;
        if (is_dir) {
            float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
            if (len < 1e-6f) continue;
            lx = p[0] / len * 3.5f;
            ly = p[1] / len * 3.5f;
            lz = p[2] / len * 3.5f;
        } else {
            lx = p[0]; ly = p[1]; lz = p[2];
        }

        if (on) {
            /* === ENABLED: bright, glowing, animated === */
            float glow = 0.6f + breath * 0.4f;

            /* Outer glow */
            glPointSize(18.0f);
            glBegin(GL_POINTS);
            glColor4f(d[0], d[1], d[2], 0.15f * glow);
            glVertex3f(lx, ly, lz);
            glEnd();

            /* Inner core */
            glPointSize(8.0f);
            glBegin(GL_POINTS);
            glColor4f(d[0], d[1], d[2], 0.7f * glow);
            glVertex3f(lx, ly, lz);
            glEnd();

            /* White hot center */
            glPointSize(3.0f);
            glBegin(GL_POINTS);
            glColor4f(1.0f, 1.0f, 1.0f, 0.9f * glow);
            glVertex3f(lx, ly, lz);
            glEnd();

            /* Directional: ray toward origin */
            if (is_dir) {
                glEnable(GL_LINE_STIPPLE);
                glLineStipple(2, 0xAAAA);
                glLineWidth(1.0f);
                glBegin(GL_LINES);
                glColor4f(d[0], d[1], d[2], 0.35f * glow);
                glVertex3f(lx, ly, lz);
                glColor4f(d[0], d[1], d[2], 0.05f);
                glVertex3f(0, 0, 0);
                glEnd();
                glDisable(GL_LINE_STIPPLE);
            } else {
                /* Positional: radiating lines */
                float rlen = 0.25f + breath * 0.1f;
                glLineWidth(1.0f);
                glBegin(GL_LINES);
                float dirs[][3] = {
                    {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
                };
                for (int r = 0; r < 6; r++) {
                    glColor4f(d[0], d[1], d[2], 0.4f * glow);
                    glVertex3f(lx, ly, lz);
                    glColor4f(d[0], d[1], d[2], 0.0f);
                    glVertex3f(lx + dirs[r][0] * rlen,
                               ly + dirs[r][1] * rlen,
                               lz + dirs[r][2] * rlen);
                }
                glEnd();
            }

            /* Label */
            char label[8];
            snprintf(label, sizeof(label), " L%d", i);
            glColor4f(d[0] * 0.7f + 0.3f, d[1] * 0.7f + 0.3f,
                      d[2] * 0.7f + 0.3f, 0.8f);
            glRasterPos3f(lx, ly, lz);
            for (const char *c = label; *c; c++)
                glutBitmapCharacter(FONT_SMALL, (unsigned char)*c);

        } else {
            /* === DISABLED: dim grey marker with X === */

            /* Small grey dot */
            glPointSize(6.0f);
            glBegin(GL_POINTS);
            glColor4f(0.4f, 0.4f, 0.4f, 0.3f);
            glVertex3f(lx, ly, lz);
            glEnd();

            /* X cross through the light position */
            float xsz = 0.12f;
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(1, 0xAAAA);
            glLineWidth(1.0f);
            glBegin(GL_LINES);
            glColor4f(0.7f, 0.2f, 0.2f, 0.45f);
            glVertex3f(lx - xsz, ly - xsz, lz);
            glVertex3f(lx + xsz, ly + xsz, lz);
            glVertex3f(lx - xsz, ly + xsz, lz);
            glVertex3f(lx + xsz, ly - xsz, lz);
            glEnd();
            glDisable(GL_LINE_STIPPLE);

            /* Dimmed label */
            char label[16];
            snprintf(label, sizeof(label), " L%d off", i);
            glColor4f(0.5f, 0.3f, 0.3f, 0.45f);
            glRasterPos3f(lx, ly, lz);
            for (const char *c = label; *c; c++)
                glutBitmapCharacter(FONT_SMALL, (unsigned char)*c);
        }
    }

    glPointSize(1.0f);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
}

/* ========================================================================= */
/* 3D scene render (viewport offset to the right of the code panel)           */
/* ========================================================================= */

static void render_3d_scene(void) {
    int panel_w = (int)(g_win_w * g_panel_frac);
    int scene_w = g_win_w - panel_w;
    if (scene_w < 1) scene_w = 1;

    glViewport(panel_w, 0, scene_w, g_win_h);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)scene_w / (double)g_win_h, 0.1, 100.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glTranslatef(g_cam_px, g_cam_py, -g_cam_dist);
    glRotatef(g_cam_rx, 1, 0, 0);
    glRotatef(g_cam_ry, 0, 1, 0);

    setup_lights();

    GLfloat mspec[] = { 0.4f, 0.4f, 0.4f, 1.0f };
    GLfloat mshin[] = { 30.0f };
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);

    glColor3f(0.70f, 0.70f, 0.80f);

    draw_grid();
    draw_axes();

    if (g_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    execute_commands();

    if (g_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* Always-on wireframe + vertex dots overlay */
    glDisable(GL_LIGHTING);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(-1.0f, -1.0f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glColor3f(0.0f, 0.0f, 0.0f);
    {
        int in_begin = 0;
        for (int i = 0; i < g_num_flat_cmds; i++) {
            if (!g_flat_cmds[i].valid) continue;
            switch (g_flat_cmds[i].type) {
            case CMD_BEGIN:
                if (in_begin) glEnd();
                glBegin(g_flat_cmds[i].mode);
                in_begin = 1;
                break;
            case CMD_END:
                if (in_begin) { glEnd(); in_begin = 0; }
                break;
            case CMD_VERTEX3F:
                if (in_begin)
                    glVertex3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                               g_flat_cmds[i].args[2]);
                break;
            default: break;
            }
        }
        if (in_begin) glEnd();
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);

    /* Vertex dots */
    glPointSize(5.0f);
    glColor3f(0.0f, 0.0f, 0.0f);
    glBegin(GL_POINTS);
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (g_flat_cmds[i].valid && g_flat_cmds[i].type == CMD_VERTEX3F)
            glVertex3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                       g_flat_cmds[i].args[2]);
    }
    glEnd();
    glPointSize(1.0f);
    glEnable(GL_LIGHTING);

    draw_vertex_guides();
    draw_normal_guides();

    if (g_show_vnums)   draw_vertex_numbers();
    if (g_show_normals) draw_normal_vectors();
    draw_lights();
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
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* 3D scene in right portion */
    render_3d_scene();

    /* 2D overlays in full window coords */
    glViewport(0, 0, g_win_w, g_win_h);
    render_code_panel();
    render_autocomplete();
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

/* Find matching FOR_END for a FOR_BEGIN at index i */
static int find_for_end(int for_begin_idx) {
    int depth = 1;
    for (int j = for_begin_idx + 1; j < g_num_cmds; j++) {
        if (g_cmds[j].type == CMD_FOR_BEGIN) depth++;
        else if (g_cmds[j].type == CMD_FOR_END) {
            depth--;
            if (depth == 0) return j;
        }
    }
    return g_num_cmds;
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
    int fdepth = for_loop_depth_at(pos);
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
 * Try to handle '}' closing a for-loop block.
 * If there's already a pre-inserted FOR_END at the current position, skip past it.
 * Otherwise, if there's an unclosed FOR_BEGIN, insert FOR_END.
 */
static int try_commit_close_brace(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '}') return 0;

    int pos = g_inserting ? g_edit_line :
              (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);

    /* Check for unclosed FOR_BEGIN */
    if (for_loop_depth_at(pos) <= 0) return 0;

    /* If we're in insert mode right before a FOR_END, just exit insert mode */
    if (g_inserting && pos < g_num_cmds && g_cmds[pos].type == CMD_FOR_END) {
        g_edit_line = pos + 1;
        g_inserting = 0;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        load_line_to_input(g_edit_line);
        set_status("for-loop block closed");
        mark_normals_dirty();
        return 1;
    }

    /* Otherwise insert a FOR_END */
    int fdepth = for_loop_depth_at(pos) - 1;
    int bb_val = in_begin_block_at(pos);
    int ind_len = (bb_val ? 4 : 2) + fdepth * 2;
    char indent[32];
    if (ind_len > (int)sizeof(indent) - 1) ind_len = (int)sizeof(indent) - 1;
    memset(indent, ' ', ind_len);
    indent[ind_len] = '\0';

    GLCmd fe;
    memset(&fe, 0, sizeof(fe));
    fe.type = CMD_FOR_END;
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
    set_status("for-loop block closed");
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


    /* Escape */
    if (key == 27) {
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
                int fe = find_for_end(g_edit_line);
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
                int fe = find_for_end(start);
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
    if (key == 31 || (key == '/' && glutGetModifiers() & GLUT_ACTIVE_CTRL)) {
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
        /* Check for } closing a loop block */
        if (g_input_len > 0 && try_commit_close_brace()) {
            g_ac_count = 0;
            g_ac_ghost[0] = '\0';
            return;
        }
        /* Check for for-loop (single-line or block start) */
        if (g_input_len > 0 && try_commit_for_loop()) {
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
                        int fdepth = for_loop_depth_at(fpos);
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
            /* On existing line: if unmodified just advance; if modified, re-parse */
            int can_advance = 1;

            /* Check if input matches the existing line (unmodified) */
            int unmodified = 0;
            {
                const char *s = g_cmds[g_edit_line].source;
                while (*s && isspace((unsigned char)*s)) s++;
                int slen = (int)strlen(s);
                while (slen > 0 && (s[slen-1] == ';' ||
                       isspace((unsigned char)s[slen-1])))
                    slen--;
                if (slen == g_input_len &&
                    strncmp(g_input, s, slen) == 0)
                    unmodified = 1;
            }

            if (!unmodified && g_input_len > 0) {
                /* Input was modified — try to re-parse */
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
                        int fdepth = for_loop_depth_at(fpos);
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
            /* else: unmodified or empty — keep existing line as-is */

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
                        int fdepth = for_loop_depth_at(fpos);
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
                    int fdepth = for_loop_depth_at(fpos);
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
        g_show_indices = !g_show_indices;
        set_status(g_show_indices ? "Command indices ON" :
                   "Command indices OFF");
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

/* Handle left-click in the code panel: navigate to line + column */
static void handle_code_panel_click(int mx, int my) {
    /* Convert GLUT Y (top=0) to OpenGL Y (bottom=0) */
    int gl_y = g_win_h - my;

    /* Same layout constants as render_code_panel */
    int line_y_start = g_win_h - CODE_MARGIN_Y - LINE_H - LINE_H;
    int vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;
    if (vis < 0) return;   /* clicked in info bar */

    int n_hpre = 0;
    for (int i = 0; g_header_pre[i]; i++) n_hpre++;
    int n_hpost = 0;
    for (int i = 0; g_header_post[i]; i++) n_hpost++;
    int n_header = n_hpre + 3 + n_hpost;

    int doc_line = g_scroll + vis;
    int cmd_area = doc_line - n_header;

    /* Ignore clicks on header or footer */
    int n_cmd_area = g_num_cmds + (g_inserting ? 1 : 0) + 1;
    if (cmd_area < 0 || cmd_area >= n_cmd_area) return;

    /* Map cmd_area index to actual command index, accounting for insert line */
    int target;
    int on_insert_line = 0;
    if (g_inserting) {
        if (cmd_area < g_edit_line) {
            target = cmd_area;
        } else if (cmd_area == g_edit_line) {
            target = -1;
            on_insert_line = 1;
        } else {
            target = cmd_area - 1;
        }
    } else {
        target = cmd_area;
    }

    if (!on_insert_line) {
        if (target < 0) target = 0;
        if (target > g_num_cmds) target = g_num_cmds;
        navigate_to_line(target);
    }

    /* Compute cursor column from click X */
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int edit_idx = on_insert_line ? g_edit_line : target;
    int indent_chars = in_begin_block_at(
        edit_idx < g_num_cmds ? edit_idx : g_num_cmds) ? 4 : 2;
    int col = (mx - text_x - indent_chars * FONT_W + FONT_W / 2) / FONT_W;
    if (col < 0) col = 0;
    if (col > g_input_len) col = g_input_len;
    g_cursor_pos = col;

    g_cursor_on = 1;
    g_blink_tick = 0;
    g_ac_count = 0;
    g_ac_ghost[0] = '\0';
    clear_selection();
}

static void mouse_func(int button, int state, int x, int y) {
    /* Left-click in code panel: navigate to line + column */
    if (button == GLUT_LEFT_BUTTON && state == GLUT_DOWN) {
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
    } else {
        g_mouse_btn = -1;
    }

#ifdef USE_GLUT
    /* Apple GLUT reports scroll wheel as button 3/4 */
    if (button == 3 && state == GLUT_DOWN) {
        g_cam_dist -= 0.3f;
        if (g_cam_dist < 0.5f) g_cam_dist = 0.5f;
        glutPostRedisplay();
    } else if (button == 4 && state == GLUT_DOWN) {
        g_cam_dist += 0.3f;
        if (g_cam_dist > 50.0f) g_cam_dist = 50.0f;
        glutPostRedisplay();
    }
#endif
}

#ifndef USE_GLUT
/* FreeGLUT mouse wheel callback */
static void mousewheel_func(int wheel, int direction, int x, int y) {
    (void)wheel; (void)x; (void)y;
    if (direction > 0) {
        g_cam_dist -= 0.3f;
        if (g_cam_dist < 0.5f) g_cam_dist = 0.5f;
    } else {
        g_cam_dist += 0.3f;
        if (g_cam_dist > 50.0f) g_cam_dist = 50.0f;
    }
    glutPostRedisplay();
}
#endif

static void motion_func(int x, int y) {
    int dx = x - g_mouse_x;
    int dy = y - g_mouse_y;

    if (g_mouse_btn == GLUT_LEFT_BUTTON) {
        g_cam_ry += (float)dx * 0.5f;
        g_cam_rx += (float)dy * 0.5f;
        if (g_cam_rx >  89.0f) g_cam_rx =  89.0f;
        if (g_cam_rx < -89.0f) g_cam_rx = -89.0f;
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
/* Initialization                                                             */
/* ========================================================================= */

static void load_initial_commands(const char *import_file) {
    /* Try importing from file first */
    if (import_file && load_from_file(import_file)) {
        g_edit_line = g_num_cmds;
        return;
    }

    /* Fall back to default example */
    static const char *init_cmds[] = {
        "glEnable(GL_DEPTH_TEST);",
        "glEnable(GL_LIGHTING);",
        "glEnable(GL_COLOR_MATERIAL);",
        "glEnable(GL_NORMALIZE);",
        "glShadeModel(GL_SMOOTH);",
        "glEnable(GL_LIGHT3);",
        "glEnable(GL_LIGHT2);",
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

    for (int i = 0; init_cmds[i]; i++) {
        g_edit_line = g_num_cmds;
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        if (parse_command(init_cmds[i], &cmd))
            g_cmds[g_num_cmds++] = cmd;
    }

    g_edit_line = g_num_cmds;
    set_status("Ready - type GL commands, press ; to execute. F1 for help.");
}

static void init_gl(void) {
    glEnable(GL_MULTISAMPLE);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    GLfloat lm_amb[] = { 0.15f, 0.15f, 0.20f, 1.0f };
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
}

/* ========================================================================= */
/* Main                                                                       */
/* ========================================================================= */

int main(int argc, char **argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH | GLUT_MULTISAMPLE);
    glutInitWindowSize(g_win_w, g_win_h);
    glutCreateWindow("OpenGL REPL - Display List Dynamic Rendering");

    init_gl();
    init_predef_vars();
    load_initial_commands((argc > 1) ? argv[1] : NULL);

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutKeyboardFunc(keyboard_func);
    glutSpecialFunc(special_func);
    glutMouseFunc(mouse_func);
    glutMotionFunc(motion_func);
#ifndef USE_GLUT
    glutMouseWheelFunc(mousewheel_func);
#endif
    glutTimerFunc(16, timer_func, 0);

    glutMainLoop();
    return 0;
}
