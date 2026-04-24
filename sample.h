/*
 * sample.h — Shared types, macros, extern globals, and utility declarations
 *
 * Common header for the OpenGL REPL split across sample.c, scene_render.c,
 * scene_grid.c, scene_axes.c, and ui_panels.c.
 */
#ifndef SAMPLE_H
#define SAMPLE_H

#include <gl_includes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>

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
#define MAX_WORKSPACE_HEADER_LINES 48
#define WORKSPACE_HEADER_LINE_LEN   96

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
/* Grid themes. The label tables in repl_actions.c, scene_grid.c, and the
 * custom render paths in scene_render.c must stay in sync with this enum. */
typedef enum {
    GRID_THEME_OFF = 0,
    GRID_THEME_CLASSIC,
    GRID_THEME_FOG,
    GRID_THEME_TRON,
    GRID_THEME_EMBER,
    GRID_THEME_FAINT,
    GRID_THEME_FOCUS,
    GRID_THEME_OCEAN,
    GRID_THEME_XZRULER,
    GRID_THEME_PLANES,
    GRID_THEME_COUNT
} GridTheme;

/* Axes themes. The label table in repl_actions.c and scene_axes.c must stay in
 * sync with this enum. */
typedef enum {
    AXES_THEME_OFF = 0,
    AXES_THEME_CLASSIC,
    AXES_THEME_PULSE,
    AXES_THEME_NEON,
    AXES_THEME_COMPASS,
    AXES_THEME_GIZMO,
    AXES_THEME_COUNT
} AxesTheme;

typedef enum {
    CODE_PANEL_LAYOUT_LEFT = 0,
    CODE_PANEL_LAYOUT_TOP,
    CODE_PANEL_LAYOUT_BOTTOM,
    CODE_PANEL_LAYOUT_HIDDEN,
    CODE_PANEL_LAYOUT_COUNT
} CodePanelLayout;

/* Default values for runtime-configurable state.
 * Used at both the variable definition site and in repl_reset_state() so the
 * two cannot drift.  Add a new entry here whenever a global's default appears
 * in more than one place. */
#define CFG_DEFAULT_MULTISAMPLE       1
#define CFG_DEFAULT_LINE_SMOOTH       0
#define CFG_DEFAULT_ATTENUATE_POINTS  1
#define CFG_DEFAULT_WRAP_AT_COMMA     1
#define CFG_DEFAULT_CODE_PANEL_LAYOUT CODE_PANEL_LAYOUT_LEFT
#define CFG_DEFAULT_PANEL_FRAC        0.45f

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

/* Default values for scene-presentation state that examples are allowed to
 * override via leading metadata. Keep these aligned with the global
 * definitions in repl_core.c and reuse them from reset helpers/tests to avoid
 * drift. */
#define CFG_DEFAULT_WIREFRAME         0
#define CFG_DEFAULT_GRID_THEME        8
#define CFG_DEFAULT_GRID_MAJOR_IDX    GRID_MAJOR_1
#define CFG_DEFAULT_GRID_EXTENT_IDX   GRID_EXTENT_FAR
#define CFG_DEFAULT_AXES_THEME        0
#define CFG_DEFAULT_VERTEX_LABELS     1
#define CFG_DEFAULT_VERTEX_INDICES    1
#define CFG_DEFAULT_NORMAL_VECTORS    0
#define CFG_DEFAULT_VERTEX_OUTLINES   1
#define CFG_DEFAULT_VERTEX_POINTS     1
#define CFG_DEFAULT_VERTEX_GUIDES     1
#define CFG_DEFAULT_XFORM_GUIDE_MODE  0
#define CFG_DEFAULT_LIGHT_INDICATORS  1
#define CFG_DEFAULT_BACKDROP_MODE     0
#define CFG_DEFAULT_CAMERA_ROTATE     0

#define MAX_LIGHTS        4
/* Shared by live scene overlays and generated output.c outline passes. */
#define REPL_OUTLINE_POLYGON_OFFSET_FACTOR (-0.01f)
#define REPL_OUTLINE_POLYGON_OFFSET_UNITS  (-100.0f)
#define TESS_VERT_BUF_SIZE 256
#define CAM_LINE_COUNT 4
#define RENDER_STATE_LINE_COUNT 3

/*
 * Replay HUD layout shared by scene_render.c and ui_panels.c.
 * Keep replay HUD drawing and variable-panel replay lift anchored to this
 * single geometry contract.
 */
#define REPLAY_HUD_MARGIN_X      18
#define REPLAY_HUD_MARGIN_Y      18
#define REPLAY_HUD_MIN_WIDTH     220
#define REPLAY_HUD_HEIGHT        56
/* y positions measured from hud_y (bottom edge), top-to-bottom:
 *   line1 (status)   @ 36 — icon row, above progress
 *   progress groove  @ 22
 *   line2 (kbd)      @  4 */
#define REPLAY_HUD_PROGRESS_Y    22
#define REPLAY_HUD_PROGRESS_H     6
#define REPLAY_HUD_TEXT_PAD_X    10
#define REPLAY_HUD_TEXT_LINE1_Y  36
#define REPLAY_HUD_TEXT_LINE2_Y   4
#define REPLAY_HUD_BOTTOM_Y (REPLAY_HUD_MARGIN_Y + REPLAY_HUD_HEIGHT)

/* Height of the amber status strip along the bottom of the scene — used by
 * both ui_panels.c (var panel lift, code panel statusbar) and scene_render.c
 * (replay HUD lift) so the HUD clears the strip. */
#define STATUSBAR_H 22

/* Shared UI accent palette — kept here so menubar (ui_panels.c) and HUD
 * (scene_render.c) use identical values.  #6fb36f is the design-bundle
 * green used for the Replay button, progress fill, and active example. */
#define UI_ACCENT_GREEN_R 0.435f
#define UI_ACCENT_GREEN_G 0.702f
#define UI_ACCENT_GREEN_B 0.435f

/* Max brightness (V in HSV) allowed for glClearColor channels.
 * Since max(r,g,b) == V, capping V caps all channels. */
#define CP_CLEAR_MAX_V 0.1f

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
    CMD_DEPTH_MASK,
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

#include "repl_config.h"
#include "repl_flatten.h"
#include "repl_executor.h"
#include "repl_state.h"
#include "repl_source_scope.h"

int  init_section_line_count(void);
void init_section_line(int i, char *buf, size_t n);

void repl_state_autocomplete_clear(void);

static inline void clear_autocomplete_state(void) {
    repl_state_autocomplete_clear();
}

/* ========================================================================= */
/* Shared utility functions                                                   */
/* ========================================================================= */

void draw_string(float x, float y, const char *s, void *font);
void draw_quad(float x, float y, float w, float h);
void begin_2d(void);
void end_2d(void);

void set_status(const char *msg);
const char *mode_name(GLenum mode);
GLenum current_begin_mode(void);
int  count_vertices(void);
void mark_normals_dirty(void);

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

#ifdef NO_POINT_PARAMETER
/* Approximate glPointParameterfv distance attenuation by scaling every
 * glPointSize call by 5/cam_dist.  Defined before the macro so this body
 * still resolves to the real GL function rather than looping back. */
static inline void _repl_point_size(GLfloat sz) {
    float cam_dist = *repl_state_camera()->dist;
    glPointSize(cam_dist > 0.0f ? sz * (2.0f / (0.5 * cam_dist)) : sz);
}
#define glPointSize _repl_point_size
#endif

#endif /* SAMPLE_H */
