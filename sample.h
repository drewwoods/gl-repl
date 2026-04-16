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

#define MAX_COMMANDS    (2*4096)
#define MAX_LINE_LEN    256
#define MAX_INPUT_LEN   1024
#define MAX_AC_MATCHES  10

#define FONT_MONO       GLUT_BITMAP_9_BY_15
#define FONT_SMALL      GLUT_BITMAP_8_BY_13
#define FONT_W          9
#define FONT_H          15
#define FONT_SMALL_W    8
#define FONT_SMALL_H    13
#define LINE_H          18
#define CODE_MARGIN_X   10
#define CODE_MARGIN_Y   8

#define MAX_ACCUM_SAMPLES 16
#define ACCUM_STEP_COUNT  5
#define GRID_THEME_COUNT  10
#define AXES_THEME_COUNT  6

/* Default values for runtime-configurable state.
 * Used at both the variable definition site and in repl_reset_state() so the
 * two cannot drift.  Add a new entry here whenever a global's default appears
 * in more than one place. */
#define CFG_DEFAULT_MULTISAMPLE       1
#define CFG_DEFAULT_LINE_SMOOTH       0
#define CFG_DEFAULT_ATTENUATE_POINTS  1
#define CFG_DEFAULT_WRAP_AT_COMMA     1
#define CFG_DEFAULT_LAYOUT_VERTICAL   0
#define CFG_DEFAULT_PANEL_FRAC        0.42f

/* Grid major tick spacing. Values live in g_grid_major_steps[] and
 * must match this enum order. */
typedef enum {
    GRID_MAJOR_1  = 0,
    GRID_MAJOR_2,
    GRID_MAJOR_5,
    GRID_MAJOR_10,
    GRID_MAJOR_COUNT
} GridMajorIdx;

/* Grid half-extent from origin along each axis. Values live in
 * g_grid_extents[] and must match this enum order. */
typedef enum {
    GRID_EXTENT_CLOSE = 0,
    GRID_EXTENT_MID,
    GRID_EXTENT_FAR,
    GRID_EXTENT_COUNT
} GridExtentIdx;
#define MAX_LIGHTS        4
/* Shared by live scene overlays and generated output.c outline passes. */
#define REPL_OUTLINE_POLYGON_OFFSET_FACTOR (-0.01f)
#define REPL_OUTLINE_POLYGON_OFFSET_UNITS  (-100.0f)
#define TESS_VERT_BUF_SIZE 256
#define CAM_LINE_COUNT 4
#define RENDER_STATE_LINE_COUNT 2

/*
 * Replay HUD layout shared by scene_render.c and ui_panels.c.
 * Keep replay HUD drawing and variable-panel replay lift anchored to this
 * single geometry contract.
 */
#define REPLAY_HUD_MARGIN_X      18
#define REPLAY_HUD_MARGIN_Y      18
#define REPLAY_HUD_MIN_WIDTH     220
#define REPLAY_HUD_HEIGHT        56
#define REPLAY_HUD_PROGRESS_Y    36
#define REPLAY_HUD_PROGRESS_H     8
#define REPLAY_HUD_TEXT_PAD_X    10
#define REPLAY_HUD_TEXT_LINE1_Y  18
#define REPLAY_HUD_TEXT_LINE2_Y   4
#define REPLAY_HUD_BOTTOM_Y (REPLAY_HUD_MARGIN_Y + REPLAY_HUD_HEIGHT)

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
    CMD_VAR_DECLARE,
    CMD_LABEL, CMD_GOTO,
    CMD_GLU_SPHERE, CMD_GLU_CYLINDER, CMD_GLU_DISK, CMD_GLU_PARTIAL_DISK,
    CMD_GLUT_TORUS,
    CMD_TESS_BEGIN_POLYGON,
    CMD_TESS_BEGIN_CONTOUR,
    CMD_TESS_END,
    CMD_TESS_NORMAL,
    CMD_TESS_COLOR,
    CMD_TESS_VERTEX,
    CMD_MATERIALF,
    CMD_POINT_SIZE,
    CMD_POINT_PARAMETER_FV,
    CMD_BLEND_FUNC,
    CMD_CLEAR_COLOR,
    CMD_TYPE_COUNT
} CmdType;

typedef enum {
    REPLAY_OFF = 0,
    REPLAY_PLAYING,
    REPLAY_PAUSED,
    REPLAY_DONE
} ReplayState;

typedef enum {
    REPLAY_MODE_POLYGON = 0,
    REPLAY_MODE_VERTEX
} ReplayMode;

typedef enum {
    PROFILE_PANEL_OFF = 0,
    PROFILE_PANEL_ON,
    PROFILE_PANEL_DETAILS,
    PROFILE_PANEL_MODE_COUNT
} ProfilePanelMode;

typedef struct {
    const char *name;
    GLenum      value;
} EnumEntry;

#define MAX_FUNC_HINT_PARAMS 10

typedef struct {
    const char *insert_text;
    const char *display_text;
    int         param_count;
    const char *params[MAX_FUNC_HINT_PARAMS];
} FuncCompletion;

typedef struct {
    CmdType  type;
    GLenum   mode;
    float    args[8];
    int      num_args;              /* Number of meaningful entries in args[] */
    char     source[MAX_LINE_LEN];  /* Normalized source text shown in the editor */
    int      valid;                 /* Deleted commands remain allocated but skipped */
    int      is_auto;               /* Auto-generated helper, e.g. synthesized normals */
    int      has_vars;              /* Source must be preserved/re-evaluated from text */
    char     var_names[MAX_NAMES_PER_DECL][16];
    int      var_decl_count;        /* Number of names in a CMD_VAR_DECLARE line */
    int      src_cmd_idx;           /* Owning source command for flat->source mapping */
    int      call_src_cmd_idx;      /* Immediate call site that expanded this command */
    int      root_call_src_cmd_idx; /* Outermost call site in nested expansion */
    unsigned int func_scope_mask;   /* Function scopes active when command was flattened */
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

static inline void apply_tracked_transform_cmd(const GLCmd *cmd, int *matrix_depth) {
    switch (cmd->type) {
    case CMD_PUSH_MATRIX:
        glPushMatrix();
        if (matrix_depth) (*matrix_depth)++;
        break;
    case CMD_POP_MATRIX:
        if (!matrix_depth || *matrix_depth > 0) {
            glPopMatrix();
            if (matrix_depth) (*matrix_depth)--;
        }
        break;
    default:
        apply_transform_cmd(cmd);
        break;
    }
}

static inline void unwind_tracked_transform_stack(int *matrix_depth) {
    if (!matrix_depth)
        return;
    while (*matrix_depth > 0) {
        glPopMatrix();
        (*matrix_depth)--;
    }
}

/* ========================================================================= */
/* Extern globals                                                             */
/* ========================================================================= */

/* TODO: Globals are definitions are scattered across multiple files, should be consolidated into a
 * single globals.c or similar. */

/* Per-flat-command local variable snapshot (loop vars at time of flattening) */
typedef struct {
    int     num_vars;
    ExprVar vars[MAX_EXPR_VARS];
} FlatCmdLocalVars;

/* Command arrays */
extern GLCmd           g_cmds[MAX_COMMANDS];
extern int             g_num_cmds;
extern int             g_normals_dirty;
extern GLCmd           g_flat_cmds[MAX_COMMANDS];
extern int             g_num_flat_cmds;
extern int             g_flat_dirty;
extern FlatCmdLocalVars g_flat_cmd_local_vars[MAX_COMMANDS];

/* Editor */
extern char   g_input[MAX_INPUT_LEN];
extern int    g_input_len;
extern int    g_cursor_pos;
extern int    g_edit_line;
extern char   g_newline_buf[MAX_INPUT_LEN];
extern int    g_newline_len;
extern int    g_inserting;

/* Camera — orbit target (world-space pivot) + yaw/pitch/distance */
extern float  g_cam_rx, g_cam_ry;
extern float  g_cam_dist;
extern float  g_cam_tx, g_cam_ty, g_cam_tz;
extern float  g_cam_motion_glow;
extern int    g_mouse_x, g_mouse_y;
extern int    g_mouse_btn;

/* Window */
extern int    g_win_w, g_win_h;

/* Code panel */
extern float  g_panel_frac;
extern int    g_resizing_panel;
extern int    g_scroll;
extern int    g_scroll_follow_cursor;

/* Accumulation buffer */
extern int    g_use_accum;
extern int    g_accum_aa_enabled;
extern int    g_accum_samples;
extern float  g_accum_jitter_x;
extern float  g_accum_jitter_y;
extern int    g_multisample_enabled;
extern int    g_line_smooth_enabled;

/* Cursor blink */
extern int    g_cursor_on;
extern int    g_blink_tick;

/* Animation */
extern float  g_anim_time;
extern int    g_t_playing;
extern int    g_t_var_idx;

/* Toggles */
extern int    g_show_help;
extern int    g_help_tab;
extern int    g_help_scroll;
extern int    g_wireframe;
extern int    g_grid_theme;
extern int    g_grid_major_idx;  /* index into g_grid_major_steps[] */
extern int    g_grid_extent_idx; /* index into g_grid_extents[] */
extern int    g_axes_theme;
extern const float g_grid_major_steps[GRID_MAJOR_COUNT];
extern const float g_grid_extents[GRID_EXTENT_COUNT];
extern float  g_focus_vtx[3];
extern int    g_focus_vtx_valid;
extern int    g_show_vnums;
extern int    g_show_normals;
extern int    g_show_indices;
extern int    g_wrap_at_comma;
extern int    g_layout_vertical;
extern int    g_show_guides;
extern int    g_autonormal;
extern int    g_show_lights;
extern int    g_backdrop_mode;
extern int    g_cam_rotate;
extern int    g_example_idx;
extern int    g_user_lighting_enabled;
extern int    g_show_outlines;
extern int    g_show_vpoints;
extern int    g_highlight_current_poly;
extern int    g_current_block_begin;
extern int    g_current_block_end;
extern int    g_ortho_mode;

/* Replay */
extern int    g_replay_active;
extern int    g_replay_state;
extern int    g_replay_pc;
extern int    g_replay_mode;
extern float  g_replay_speed;
extern float  g_replay_accum;
extern float  g_replay_fade_alpha;
extern float  g_replay_fade_speed;
extern int    g_replay_fade_begin;
extern int    g_replay_fade_end;
extern int    g_replay_src_line;
extern int    g_replay_total_flat;

/* Variable slider panel */
extern int    g_show_var_panel;
extern int    g_drag_var;
extern int    g_drag_log_mode;   /* 0=linear (LMB), 1=logarithmic (RMB) */
extern float  g_drag_start_val;
extern int    g_drag_start_x;

/* Configuration menu */
extern int    g_show_config;
extern int    g_config_hover;

/* CPU profile panel */
extern int    g_show_profile_panel;

/* GLU quadric & tessellator */
extern GLUquadric    *g_quadric;
extern GLUtesselator *g_tess;
extern TessVertex     g_tess_verts[TESS_VERT_BUF_SIZE];
extern int            g_tess_vert_count;

/* Lights */
extern SceneLight g_lights[MAX_LIGHTS];

/* Max brightness (V in HSV) allowed for glClearColor channels.
 * Since max(r,g,b) == V, capping V caps all channels. */
#define CP_CLEAR_MAX_V  0.1f

/* Clear color (user-settable via glClearColor command) */
extern float  g_clear_color[4];

/* Status bar */
extern char   g_status[256];
extern int    g_status_ttl;

/* Search */
extern int    g_search_active;
extern char   g_search_query[MAX_INPUT_LEN];
extern int    g_search_query_len;
extern int    g_search_cursor_pos;
extern int    g_search_hit_line;
extern int    g_search_hit_char;
extern int    g_search_hit_ordinal;
extern int    g_search_match_count;

/* Autocomplete */
extern const char *g_ac_matches[MAX_AC_MATCHES];
extern int    g_ac_count;
extern int    g_ac_sel;
extern char   g_ac_ghost[MAX_LINE_LEN];
extern char   g_ac_hint[MAX_LINE_LEN];
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
extern const FuncCompletion g_func_completions[];
extern const char *g_header_pre[];

/* Scratch buffer */
extern char   g_scratch_buf[256];

/* Workspace state header — saved as comments at top of exported files, parsed
 * at launch to restore variable values and config toggles, and displayed as
 * the topmost block in the code panel. */
#define MAX_WORKSPACE_HEADER_LINES 48
#define WORKSPACE_HEADER_LINE_LEN  96
extern char g_workspace_header_lines[MAX_WORKSPACE_HEADER_LINES][WORKSPACE_HEADER_LINE_LEN];
extern int  g_workspace_header_line_count;
void refresh_workspace_header_lines(void);
int  parse_workspace_header_line(const char *line);

extern char        g_render_state_lines[RENDER_STATE_LINE_COUNT][64];
extern char        g_cam_lines[CAM_LINE_COUNT][96];
extern const char *g_header_post[];
extern const char *g_footer_pre_init[];
extern const char *g_footer_post_init[];
extern const char *g_grid_names[];
extern const char *g_grid_major_names[GRID_MAJOR_COUNT];
extern const char *g_grid_extent_names[GRID_EXTENT_COUNT];
extern const char *g_axes_names[];
extern int         g_init_attenuate_points;

int  init_section_line_count(void);
void init_section_line(int i, char *buf, size_t n);

static inline void clear_autocomplete_state(void) {
    g_ac_count = 0;
    g_ac_sel = 0;
    g_ac_ghost[0] = '\0';
    g_ac_hint[0] = '\0';
}

/* Config items */
extern CfgItem g_cfg_items[];
extern const int CFG_ITEM_COUNT;
void repl_editor_apply_defaults(void);

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

int  repl_search_row_count(void);
const char *repl_search_row_text(int row_idx);
int  repl_search_row_for_cmd_index(int cmd_idx);
int  repl_search_find_next_in_text(const char *text, const char *query,
                                   int start_pos);
int  repl_search_find_prev_in_text(const char *text, const char *query,
                                   int start_pos);

void navigate_to_line(int target);
void execute_commands(void);
void execute_replay_fade_batches(void);
void flatten_commands(void);
void replay_start(void);
void replay_stop(void);
void replay_advance(void);
int  replay_exec_limit(void);
int  replay_has_active_fades(void);
int  replay_fill_base_limit(void);
void recompute_autonormals(void);
void update_cam_lines(void);

#endif /* SAMPLE_H */
