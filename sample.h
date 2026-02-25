/*
 * sample.h — Shared types, macros, extern globals, and utility declarations
 *
 * Common header for the OpenGL REPL split across sample.c, scene_render.c,
 * and ui_panels.c.
 */
#ifndef SAMPLE_H
#define SAMPLE_H

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

#define MAX_ACCUM_SAMPLES 16
#define ACCUM_STEP_COUNT  5
#define GRID_THEME_COUNT  7
#define AXES_THEME_COUNT  5
#define NUM_EXAMPLES      7
#define MAX_LIGHTS        4
#define TESS_VERT_BUF_SIZE 256

typedef struct {
    GLdouble pos[3];
    GLdouble normal[3]; /* per-vertex normal, default (0,0,1) */
    GLdouble color[4];  /* per-vertex RGBA,   default (1,1,1,1) */
} TessVertex;

/* ========================================================================= */
/* Types                                                                      */
/* ========================================================================= */

typedef enum {
    CMD_BEGIN, CMD_END,
    CMD_VERTEX3F, CMD_VERTEX2F,
    CMD_NORMAL3F,
    CMD_COLOR3F, CMD_COLOR4F,
    CMD_ENABLE, CMD_DISABLE,
    CMD_SHADE_MODEL,
    CMD_TRANSLATE3F,
    CMD_SCALEF,
    CMD_ROTATEF,
    CMD_PUSH_MATRIX,
    CMD_POP_MATRIX,
    CMD_COLOR_MATERIAL,
    CMD_LIGHT_MODEL_I,
    CMD_FRONT_FACE,
    CMD_FOR_BEGIN, CMD_FOR_END,
    CMD_FUNC_DEF, CMD_FUNC_END, CMD_CALL,
    CMD_IF_BEGIN, CMD_IF_END,
    CMD_COMMENT,
    CMD_VAR_ASSIGN,
    CMD_LABEL, CMD_GOTO,
    CMD_GLU_SPHERE, CMD_GLU_CYLINDER, CMD_GLU_DISK, CMD_GLU_PARTIAL_DISK,
    CMD_GLUT_TORUS,
    CMD_TESS_BEGIN_POLYGON,
    CMD_TESS_BEGIN_CONTOUR,
    CMD_TESS_END,
    CMD_TESS_NORMAL,
    CMD_TESS_COLOR,
    CMD_TESS_VERTEX,
    CMD_TYPE_COUNT
} CmdType;

typedef struct {
    const char *name;
    GLenum      value;
} EnumEntry;

typedef struct {
    CmdType  type;
    GLenum   mode;
    float    args[8];
    int      num_args;
    char     source[MAX_LINE_LEN];
    int      valid;
    int      is_auto;
    int      has_vars;
    int      src_cmd_idx;
} GLCmd;

typedef struct {
    GLenum   id;         /* GL_LIGHT0 .. GL_LIGHT3 */
    int      enabled;
    float    pos[4];     /* xyz + w (0=directional, 1=positional) */
    float    diffuse[4];
    float    ambient[4];
    float    specular[4];
} SceneLight;

typedef struct {
    const char  *label;
    const char  *key_hint;
    int         *value;
    int          n_states;    /* 2 = ON/OFF toggle; >2 = cycle */
    const char **state_names; /* NULL -> display "OFF"/"ON" */
} CfgItem;

/* ========================================================================= */
/* Transform command helpers (used by sample.c and scene_render.c)           */
/* ========================================================================= */

/* Returns non-zero if t is a matrix transform command */
static inline int is_transform_cmd(CmdType t) {
    return (t == CMD_TRANSLATE3F || t == CMD_SCALEF  || t == CMD_ROTATEF ||
            t == CMD_PUSH_MATRIX  || t == CMD_POP_MATRIX);
}

/* Execute the GL call for a transform command (must not be inside glBegin/glEnd) */
static inline void apply_transform_cmd(const GLCmd *cmd) {
    switch (cmd->type) {
    case CMD_TRANSLATE3F: glTranslatef(cmd->args[0], cmd->args[1], cmd->args[2]); break;
    case CMD_SCALEF:      glScalef    (cmd->args[0], cmd->args[1], cmd->args[2]); break;
    case CMD_ROTATEF:     glRotatef   (cmd->args[0], cmd->args[1],
                                       cmd->args[2], cmd->args[3]); break;
    case CMD_PUSH_MATRIX: glPushMatrix(); break;
    case CMD_POP_MATRIX:  glPopMatrix();  break;
    default: break;
    }
}

/* ========================================================================= */
/* Extern globals                                                             */
/* ========================================================================= */

/* Command arrays */
extern GLCmd  g_cmds[MAX_COMMANDS];
extern int    g_num_cmds;
extern int    g_normals_dirty;
extern GLCmd  g_flat_cmds[MAX_COMMANDS];
extern int    g_num_flat_cmds;
extern int    g_flat_dirty;

/* Editor */
extern char   g_input[MAX_INPUT_LEN];
extern int    g_input_len;
extern int    g_cursor_pos;
extern int    g_edit_line;
extern char   g_newline_buf[MAX_INPUT_LEN];
extern int    g_newline_len;
extern int    g_inserting;

/* Camera */
extern float  g_cam_rx, g_cam_ry;
extern float  g_cam_dist;
extern float  g_cam_px, g_cam_py;
extern int    g_mouse_x, g_mouse_y;
extern int    g_mouse_btn;

/* Window */
extern int    g_win_w, g_win_h;

/* Code panel */
extern float  g_panel_frac;
extern int    g_scroll;
extern int    g_scroll_follow_cursor;

/* Accumulation buffer */
extern int    g_use_accum;
extern int    g_accum_aa_enabled;
extern int    g_accum_samples;
extern float  g_accum_jitter_x;
extern float  g_accum_jitter_y;

/* Cursor blink */
extern int    g_cursor_on;
extern int    g_blink_tick;

/* Animation */
extern float  g_anim_time;
extern int    g_t_playing;
extern int    g_t_var_idx;

/* Toggles */
extern int    g_show_help;
extern int    g_help_scroll;
extern int    g_wireframe;
extern int    g_grid_theme;
extern int    g_axes_theme;
extern float  g_focus_vtx[3];
extern int    g_focus_vtx_valid;
extern int    g_show_vnums;
extern int    g_show_normals;
extern int    g_show_indices;
extern int    g_show_guides;
extern int    g_autonormal;
extern int    g_show_lights;
extern int    g_cam_rotate;
extern int    g_example_idx;
extern int    g_user_lighting_enabled;
extern int    g_show_outlines;
extern int    g_highlight_current_poly;
extern int    g_current_block_begin;
extern int    g_current_block_end;
extern int    g_ortho_mode;

/* Variable slider panel */
extern int    g_show_var_panel;
extern int    g_drag_var;
extern float  g_drag_start_val;
extern int    g_drag_start_x;

/* Configuration menu */
extern int    g_show_config;
extern int    g_config_hover;

/* GLU quadric & tessellator */
extern GLUquadric    *g_quadric;
extern GLUtesselator *g_tess;
extern TessVertex     g_tess_verts[TESS_VERT_BUF_SIZE];
extern int            g_tess_vert_count;

/* Lights */
extern SceneLight g_lights[MAX_LIGHTS];

/* Status bar */
extern char   g_status[256];
extern int    g_status_ttl;

/* Autocomplete */
extern const char *g_ac_matches[MAX_AC_MATCHES];
extern int    g_ac_count;
extern int    g_ac_sel;
extern char   g_ac_ghost[MAX_LINE_LEN];
extern int    g_cursor_px;
extern int    g_cursor_py;

/* Selection */
extern int    g_sel_anchor;
extern int    g_sel_end;

/* Clipboard */
extern GLCmd  g_clipboard[MAX_COMMANDS];
extern int    g_clipboard_count;

/* Lookup tables */
extern const EnumEntry g_begin_modes[];
extern const EnumEntry g_enable_caps[];
extern const EnumEntry g_shade_models[];
extern const char *g_func_completions[];
extern const char *g_header_pre[];
extern char        g_lookat[3][128];
extern const char *g_header_post[];
extern const char *g_footer[];
extern const char *g_grid_names[];
extern const char *g_axes_names[];

/* Config items */
extern CfgItem g_cfg_items[];
extern const int CFG_ITEM_COUNT;

/* ========================================================================= */
/* Shared utility functions                                                   */
/* ========================================================================= */

void draw_string(float x, float y, const char *s, void *font);
void draw_quad(float x, float y, float w, float h);
void begin_2d(void);
void end_2d(void);

void set_status(const char *msg);
const char *mode_name(GLenum mode);
int  in_begin_block_at(int line_idx);
int  in_begin_block(void);
int  cmd_indent_chars(int pos);
GLenum current_begin_mode(void);
int  count_vertices(void);
void mark_normals_dirty(void);

void clear_selection(void);
int  sel_active(void);
int  sel_lo(void);
int  sel_hi(void);

void navigate_to_line(int target);
void execute_commands(void);
void flatten_commands(void);
void recompute_autonormals(void);
void update_lookat_strings(void);

#endif /* SAMPLE_H */
