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
 * Controls:
 *   Type + ;       Execute / commit line
 *   Enter          Insert new line (works in middle of list)
 *   Up/Down        Navigate between command lines
 *   Left/Right     Move cursor within input line
 *   Home/End       Jump to start/end of input line
 *   Backspace      Delete character before cursor
 *   Ctrl+Z         Undo last command
 *   Ctrl+D         Delete line at cursor
 *   Ctrl+L         Clear all commands
 *   Ctrl+S         Save to output.c
 *   Escape         Clear input / exit insert mode / close help
 *   Tab            Accept autocomplete suggestion
 *   Left-drag      Orbit camera
 *   Right-drag     Pan camera
 *   Middle-drag    Zoom
 *   F1-F8          Toggle overlays (help/wire/grid/axes/vnums/normals/indices/guides)
 *   PgUp/PgDn      Scroll code panel
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

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

#define MAX_COMMANDS    512
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
} GLCmd;

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
    NULL
};

static const char *g_header_pre[] = {
    "#include <gl_includes.h>",
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
    "void init() {",
    "  glEnable(GL_LIGHT0);",
    "}",
    "",
    "int main(int argc, char **argv) {",
    "  glutInit(&argc, argv);",
    "  glutInitDisplayMode(GLUT_DOUBLE|GLUT_RGB|GLUT_DEPTH);",
    "  glutInitWindowSize(800, 600);",
    "  glutCreateWindow(\"OpenGL REPL\");",
    "  init();",
    "  glutDisplayFunc(display);",
    "  glutReshapeFunc(reshape);",
    "  glutMainLoop();",
    "  return 0;",
    "}",
    NULL
};

/* ========================================================================= */
/* Global state                                                               */
/* ========================================================================= */

static GLCmd  g_cmds[MAX_COMMANDS];
static int    g_num_cmds = 0;

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

/* Toggles */
static int    g_show_help    = 0;
static int    g_wireframe    = 0;
static int    g_show_grid    = 1;
static int    g_show_axes    = 1;
static int    g_show_vnums   = 0;
static int    g_show_normals = 0;
static int    g_show_indices = 0;
static int    g_show_guides  = 1;

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
    for (int i = 0; i < g_num_cmds; i++)
        if (g_cmds[i].valid && g_cmds[i].type == CMD_VERTEX3F) n++;
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

/* Save the current session as a standalone C source file */
static void save_output(void) {
    FILE *f = fopen("output.c", "w");
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
    for (int i = 0; i < g_num_cmds; i++)
        fprintf(f, "%s\n", g_cmds[i].source);
    if (in_begin_block())
        fprintf(f, "  glEnd();\n");
    for (int i = 0; g_footer[i]; i++)
        fprintf(f, "%s\n", g_footer[i]);

    fclose(f);

    char msg[128];
    snprintf(msg, sizeof(msg), "Saved to output.c (%d commands)", g_num_cmds);
    set_status(msg);
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

static int parse_floats(const char *s, float *out, int max) {
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        char *end;
        out[n] = strtof(p, &end);
        if (end == p) break;
        n++;
        p = end;
    }
    return n;
}

static int parse_command(const char *line, GLCmd *cmd) {
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
        cmd->num_args = parse_floats(args, cmd->args, 3);
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
        cmd->num_args = parse_floats(args, cmd->args, 3);
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
        cmd->num_args = parse_floats(args, cmd->args, 3);
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
        cmd->num_args = parse_floats(args, cmd->args, 4);
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

/* ========================================================================= */
/* Command execution                                                          */
/* ========================================================================= */

static void execute_commands(void) {
    int in_begin = 0;
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        switch (g_cmds[i].type) {
        case CMD_BEGIN:
            if (in_begin) glEnd();
            glBegin(g_cmds[i].mode);
            in_begin = 1;
            break;
        case CMD_END:
            if (in_begin) { glEnd(); in_begin = 0; }
            break;
        case CMD_VERTEX3F:
            if (in_begin)
                glVertex3f(g_cmds[i].args[0], g_cmds[i].args[1],
                           g_cmds[i].args[2]);
            break;
        case CMD_NORMAL3F:
            glNormal3f(g_cmds[i].args[0], g_cmds[i].args[1],
                       g_cmds[i].args[2]);
            break;
        case CMD_COLOR3F:
            glColor3f(g_cmds[i].args[0], g_cmds[i].args[1],
                      g_cmds[i].args[2]);
            break;
        case CMD_COLOR4F:
            glColor4f(g_cmds[i].args[0], g_cmds[i].args[1],
                      g_cmds[i].args[2], g_cmds[i].args[3]);
            break;
        case CMD_ENABLE:
            glEnable(g_cmds[i].mode);
            break;
        case CMD_DISABLE:
            glDisable(g_cmds[i].mode);
            break;
        case CMD_SHADE_MODEL:
            glShadeModel(g_cmds[i].mode);
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
    default:           glColor3f(0.70f, 0.70f, 0.70f); break;
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
        "",
        "Supported Commands (type + ;):",
        "  glBegin(MODE)        GL_TRIANGLES, GL_TRIANGLE_STRIP, ...",
        "  glEnd()              End current primitive block",
        "  glVertex3f(x,y,z)   Specify a vertex position",
        "  glNormal3f(x,y,z)   Specify a vertex normal",
        "  glColor3f(r,g,b)    Specify vertex color",
        "  glColor4f(r,g,b,a)  Specify color with alpha",
        "",
        "Editing:",
        "  Up / Down            Navigate command lines",
        "  Left / Right         Move cursor within line",
        "  Home / End           Jump to start / end of line",
        "  Type + ;             Commit line (edit existing or append new)",
        "  Enter                Insert new line (even in middle of list)",
        "  Tab                  Accept autocomplete suggestion",
        "  Backspace            Delete character before cursor",
        "  Ctrl+Z               Undo last command",
        "  Ctrl+D               Delete line at cursor",
        "  Ctrl+L               Clear all commands",
        "  Ctrl+S               Save to output.c",
        "  Escape               Clear input / exit insert / close help",
        "",
        "Camera:",
        "  Left-drag            Orbit",
        "  Right-drag           Pan",
        "  Middle-drag Y        Zoom",
        "",
        "Toggles:",
        "  F1  Help overlay     F2  Wireframe mode",
        "  F3  Grid             F4  Axes",
        "  F5  Vertex numbers   F6  Normal vectors",
        "  F7  Command indices  F8  Vertex guides",
        "  PgUp / PgDn          Scroll code panel",
        "",
        "Press F1 or Escape to close.",
        NULL
    };

    begin_2d();
    glEnable(GL_BLEND);

    int hx = g_win_w / 5, hy = g_win_h / 8;
    int hw = g_win_w * 3 / 5, hh = g_win_h * 3 / 4;

    glColor4f(0.03f, 0.03f, 0.06f, 0.92f);
    draw_quad((float)hx, (float)hy, (float)hw, (float)hh);

    glColor4f(0.45f, 0.45f, 0.75f, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)hx, (float)hy);
    glVertex2f((float)(hx + hw), (float)hy);
    glVertex2f((float)(hx + hw), (float)(hy + hh));
    glVertex2f((float)hx, (float)(hy + hh));
    glEnd();

    int tx = hx + 24, ty = hy + hh - 32;
    for (int i = 0; text[i]; i++) {
        if (text[i][0] == '=')
            glColor3f(0.80f, 0.80f, 1.00f);
        else if (text[i][0] == ' ' && text[i][1] == ' ')
            glColor3f(0.65f, 0.90f, 0.65f);
        else
            glColor3f(0.75f, 0.75f, 0.80f);

        draw_string((float)tx, (float)ty, text[i], FONT_MONO);
        ty -= LINE_H;
    }

    glDisable(GL_BLEND);
    end_2d();
}

/* ========================================================================= */
/* 3D scene helpers                                                           */
/* ========================================================================= */

static void draw_grid(void) {
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float extent = 5.0f, step = 0.5f;
    glBegin(GL_LINES);
    for (float v = -extent; v <= extent + 0.01f; v += step) {
        float a = (fabsf(v) < 0.01f) ? 0.45f : 0.12f;
        glColor4f(0.50f, 0.50f, 0.60f, a);
        glVertex3f(v, -extent, 0); glVertex3f(v, extent, 0);
        glVertex3f(-extent, v, 0); glVertex3f(extent, v, 0);
    }
    glEnd();

    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

static void draw_axes(void) {
    glDisable(GL_LIGHTING);
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    glColor3f(0.90f, 0.20f, 0.20f);
    glVertex3f(0, 0, 0); glVertex3f(2, 0, 0);
    glColor3f(0.20f, 0.90f, 0.20f);
    glVertex3f(0, 0, 0); glVertex3f(0, 2, 0);
    glColor3f(0.20f, 0.20f, 0.90f);
    glVertex3f(0, 0, 0); glVertex3f(0, 0, 2);
    glEnd();
    glLineWidth(1.0f);

    glColor3f(0.90f, 0.30f, 0.30f);
    glRasterPos3f(2.15f, 0, 0); glutBitmapCharacter(FONT_MONO, 'X');
    glColor3f(0.30f, 0.90f, 0.30f);
    glRasterPos3f(0, 2.15f, 0); glutBitmapCharacter(FONT_MONO, 'Y');
    glColor3f(0.30f, 0.30f, 0.90f);
    glRasterPos3f(0, 0, 2.15f); glutBitmapCharacter(FONT_MONO, 'Z');

    glEnable(GL_LIGHTING);
}

static void draw_vertex_numbers(void) {
    glDisable(GL_LIGHTING);
    glColor3f(1.0f, 1.0f, 0.30f);

    int vn = 0;
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid || g_cmds[i].type != CMD_VERTEX3F) continue;
        char label[16];
        snprintf(label, sizeof(label), " v%d", vn);
        glRasterPos3f(g_cmds[i].args[0], g_cmds[i].args[1],
                      g_cmds[i].args[2]);
        for (const char *c = label; *c; c++)
            glutBitmapCharacter(FONT_MONO, (unsigned char)*c);
        vn++;
    }

    glEnable(GL_LIGHTING);
}

static void draw_normal_vectors(void) {
    glDisable(GL_LIGHTING);
    glColor3f(0.30f, 0.80f, 1.00f);
    float scale = 0.35f;
    float nx = 0, ny = 0, nz = 1;

    glBegin(GL_LINES);
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        if (g_cmds[i].type == CMD_NORMAL3F) {
            nx = g_cmds[i].args[0]; ny = g_cmds[i].args[1];
            nz = g_cmds[i].args[2];
        } else if (g_cmds[i].type == CMD_VERTEX3F) {
            float vx = g_cmds[i].args[0], vy = g_cmds[i].args[1],
                  vz = g_cmds[i].args[2];
            glVertex3f(vx, vy, vz);
            glVertex3f(vx + nx * scale, vy + ny * scale, vz + nz * scale);
        }
    }
    glEnd();

    glPointSize(4.0f);
    glBegin(GL_POINTS);
    nx = 0; ny = 0; nz = 1;
    for (int i = 0; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        if (g_cmds[i].type == CMD_NORMAL3F) {
            nx = g_cmds[i].args[0]; ny = g_cmds[i].args[1];
            nz = g_cmds[i].args[2];
        } else if (g_cmds[i].type == CMD_VERTEX3F) {
            glVertex3f(g_cmds[i].args[0] + nx * scale,
                       g_cmds[i].args[1] + ny * scale,
                       g_cmds[i].args[2] + nz * scale);
        }
    }
    glEnd();
    glPointSize(1.0f);

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
    int n = parse_floats(g_input + 11, vals, 3);
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
    }

    glDisable(GL_BLEND);
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

    GLfloat lpos[] = { 2.0f, 4.0f, 5.0f, 0.0f };
    GLfloat lamb[] = { 0.20f, 0.20f, 0.25f, 1.0f };
    GLfloat ldif[] = { 0.80f, 0.80f, 0.75f, 1.0f };
    GLfloat lspc[] = { 1.0f, 1.0f, 0.95f, 1.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glLightfv(GL_LIGHT0, GL_AMBIENT,  lamb);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  ldif);
    glLightfv(GL_LIGHT0, GL_SPECULAR, lspc);

    GLfloat mspec[] = { 0.4f, 0.4f, 0.4f, 1.0f };
    GLfloat mshin[] = { 30.0f };
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);

    glColor3f(0.70f, 0.70f, 0.80f);

    if (g_show_grid) draw_grid();
    if (g_show_axes) draw_axes();

    if (g_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    execute_commands();

    if (g_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    draw_vertex_guides();

    if (g_show_vnums)   draw_vertex_numbers();
    if (g_show_normals) draw_normal_vectors();
}

/* ========================================================================= */
/* GLUT callbacks                                                             */
/* ========================================================================= */

static void display_func(void) {
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

static void keyboard_func(unsigned char key, int x, int y) {
    (void)x; (void)y;

    /* Reset cursor blink on any input */
    g_cursor_on = 1;
    g_blink_tick = 0;

    /* Escape */
    if (key == 27) {
        if (g_show_help) {
            g_show_help = 0;
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
        glutPostRedisplay();
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
            set_status("Undo: removed last command");
        } else {
            set_status("Nothing to undo");
        }
        glutPostRedisplay();
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
            set_status("Line deleted");
        }
        glutPostRedisplay();
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
        set_status("All commands cleared");
        glutPostRedisplay();
        return;
    }

    /* Ctrl+S: save to output.c */
    if (key == 19) {
        save_output();
        glutPostRedisplay();
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
        glutPostRedisplay();
        return;
    }

    /* Tab: accept autocomplete */
    if (key == '\t') {
        if (g_ac_count > 0) {
            accept_autocomplete();
            update_autocomplete();
        }
        glutPostRedisplay();
        return;
    }

    /* Enter: insert new line */
    if (key == '\r' || key == '\n') {
        if (g_inserting) {
            /* Already in insert mode */
            if (g_input_len > 0) {
                /* Commit insertion, stay in insert mode */
                GLCmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                if (parse_command(g_input, &cmd) && g_num_cmds < MAX_COMMANDS) {
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
            /* On existing line: commit any changes, then enter insert mode */
            int can_advance = 1;
            if (g_input_len > 0) {
                GLCmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                if (parse_command(g_input, &cmd)) {
                    g_cmds[g_edit_line] = cmd;
                                    } else {
                    can_advance = 0;
                }
            }
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
                if (parse_command(g_input, &cmd) && g_num_cmds < MAX_COMMANDS) {
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
        glutPostRedisplay();
        return;
    }

    /* Semicolon: parse and commit */
    if (key == ';') {
        if (g_input_len > 0) {
            GLCmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            if (parse_command(g_input, &cmd)) {
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
        glutPostRedisplay();
        return;
    }

    /* Printable character: insert at cursor position */
    if (key >= 32 && key < 127 && g_input_len < MAX_INPUT_LEN - 2) {
        memmove(&g_input[g_cursor_pos + 1], &g_input[g_cursor_pos],
                g_input_len - g_cursor_pos + 1);
        g_input[g_cursor_pos] = (char)key;
        g_input_len++;
        g_cursor_pos++;
        update_autocomplete();
        glutPostRedisplay();
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
        } else {
            navigate_to_line(g_edit_line - 1);
        }
        break;

    case GLUT_KEY_DOWN:
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
        } else {
            navigate_to_line(g_edit_line + 1);
        }
        break;

    /* Toggle keys */
    case GLUT_KEY_F1:
        g_show_help = !g_show_help; break;
    case GLUT_KEY_F2:
        g_wireframe = !g_wireframe;
        set_status(g_wireframe ? "Wireframe ON" : "Wireframe OFF"); break;
    case GLUT_KEY_F3:
        g_show_grid = !g_show_grid;
        set_status(g_show_grid ? "Grid ON" : "Grid OFF"); break;
    case GLUT_KEY_F4:
        g_show_axes = !g_show_axes;
        set_status(g_show_axes ? "Axes ON" : "Axes OFF"); break;
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
        break;

    /* Scroll */
    case GLUT_KEY_PAGE_UP:   g_scroll -= 5; break;
    case GLUT_KEY_PAGE_DOWN: g_scroll += 5; break;
    default: break;
    }

    glutPostRedisplay();
}

static void mouse_func(int button, int state, int x, int y) {
    if (state == GLUT_DOWN) {
        g_mouse_btn = button;
        g_mouse_x = x;
        g_mouse_y = y;
    } else {
        g_mouse_btn = -1;
    }

    /* Scroll wheel */
    if (button == 3 && state == GLUT_DOWN) {
        g_cam_dist -= 0.3f;
        if (g_cam_dist < 0.5f) g_cam_dist = 0.5f;
        glutPostRedisplay();
    } else if (button == 4 && state == GLUT_DOWN) {
        g_cam_dist += 0.3f;
        if (g_cam_dist > 50.0f) g_cam_dist = 50.0f;
        glutPostRedisplay();
    }
}

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
    glutPostRedisplay();
}

static void timer_func(int value) {
    (void)value;

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

static void load_initial_commands(void) {
    static const char *init_cmds[] = {
        "glEnable(GL_DEPTH_TEST);",
        "glEnable(GL_LIGHTING);",
        "glEnable(GL_COLOR_MATERIAL);",
        "glShadeModel(GL_SMOOTH);",
        "glBegin(GL_TRIANGLE_STRIP);",
        "glNormal3f(-0.5, -0.5, 0.2);",
        "glVertex3f(-1.0, -1.0, -0.5);",
        "glNormal3f(-0.5, 0.5, 1.0);",
        "glVertex3f(-1.0, 1.0, -0.5);",
        "glNormal3f(0.0, -0.5, 1.0);",
        "glVertex3f(0.0, -1.0, -0.5);",
        NULL
    };

    for (int i = 0; init_cmds[i]; i++) {
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        if (parse_command(init_cmds[i], &cmd))
            g_cmds[g_num_cmds++] = cmd;
    }

    g_edit_line = g_num_cmds;  /* cursor on new line */
        set_status("Ready - type GL commands, press ; to execute. F1 for help.");
}

static void init_gl(void) {
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_MULTISAMPLE);

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
    load_initial_commands();

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutKeyboardFunc(keyboard_func);
    glutSpecialFunc(special_func);
    glutMouseFunc(mouse_func);
    glutMotionFunc(motion_func);
    glutTimerFunc(16, timer_func, 0);

    glutMainLoop();
    return 0;
}
