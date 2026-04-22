#ifndef REPL_STATE_H
#define REPL_STATE_H

#ifndef SAMPLE_H
#error "repl_state.h depends on sample.h; include sample.h first"
#endif

/* Runtime state ownership facade.
 *
 * The REPL still uses the historical g_* globals while the codebase is being
 * split apart.  These structs name the intended ownership boundaries and give
 * new code a typed way to ask for the live state without adding more ad hoc
 * extern declarations to sample.h.
 */

#define MAX_WORKSPACE_HEADER_LINES 48
#define WORKSPACE_HEADER_LINE_LEN  96

typedef struct {
    int     num_vars;
    ExprVar vars[MAX_EXPR_VARS];
} FlatCmdLocalVars;

/* View over an expanded command stream. The live view points at g_flat_cmds[]
 * and its per-command local-variable snapshots, but tests and future
 * replay/import paths can pass another flat stream without changing the
 * executor's control flow. */
typedef struct {
    const GLCmd      *cmds;
    FlatCmdLocalVars *local_vars;
    int               cmd_count;
} FlatProgramView;

typedef struct {
    const GLCmd      *source_cmds;
    int               source_cmd_count;
    GLCmd            *flat_cmds;
    FlatCmdLocalVars *flat_local_vars;
    int               flat_capacity;
    int               max_call_depth;
    int               visit_budget;
} ReplFlattenOptions;

typedef struct {
    int ok;
    int flat_cmd_count;
    int user_lighting_enabled;
    char status[128];
} ReplFlattenResult;

typedef struct {
    GLCmd            *cmds;
    int              *num_cmds;
    int               capacity;
    int              *normals_dirty;
    GLCmd            *flat_cmds;
    int              *num_flat_cmds;
    int              *flat_dirty;
    FlatCmdLocalVars *flat_local_vars;
} ReplCommandState;

typedef struct {
    char *input;
    int   input_capacity;
    int  *input_len;
    int  *cursor_pos;
    int  *edit_line;
    char *newline_buf;
    int   newline_capacity;
    int  *newline_len;
    int  *inserting;
    int  *sel_anchor;
    int  *sel_end;
    GLCmd *clipboard;
    int  *clipboard_count;
} ReplEditorState;

typedef struct {
    float *cam_rx;
    float *cam_ry;
    float *cam_dist;
    float *cam_tx;
    float *cam_ty;
    float *cam_tz;
    float *cam_motion_glow;
    int   *mouse_x;
    int   *mouse_y;
    int   *mouse_btn;
    int   *win_w;
    int   *win_h;
} ReplViewState;

typedef struct {
    int *show_help;
    int *help_tab;
    int *help_scroll;
    int *wireframe;
    int *grid_theme;
    int *grid_major_idx;
    int *grid_extent_idx;
    int *axes_theme;
    const float *grid_major_steps;
    const float *grid_extents;
    float *focus_vtx;
    int *focus_vtx_valid;
    int *show_vnums;
    int *show_normals;
    int *show_indices;
    int *wrap_at_comma;
    int *code_panel_layout;
    int *show_guides;
    int *xform_guide_mode;
    int *autonormal;
    int *show_lights;
    int *backdrop_mode;
    int *cam_rotate;
    int *example_idx;
    int *user_lighting_enabled;
    int *show_outlines;
    int *show_vpoints;
    int *highlight_current_poly;
    int *current_block_begin;
    int *current_block_end;
    int *ortho_mode;
} ReplPresentationState;

typedef struct {
    int   *replay_active;
    int   *replay_state;
    int   *replay_pc;
    int   *replay_mode;
    float *replay_speed;
    float *replay_accum;
    float *replay_fade_speed;
    int   *replay_src_line;
    int   *replay_total_flat;
    int   *replay_expand_args;
} ReplReplayRuntimeState;

typedef struct {
    float *panel_frac;
    int   *resizing_panel;
    int   *scroll;
    int   *scroll_follow_cursor;
    int   *cursor_on;
    int   *blink_tick;
    float *anim_time;
    int   *t_playing;
    int   *t_var_idx;
    int   *show_var_panel;
    int   *drag_var;
    int   *drag_log_mode;
    float *drag_start_val;
    int   *drag_start_x;
    int   *show_profile_panel;
    char  *status;
    int    status_capacity;
    int   *status_ttl;
    int   *search_active;
    char  *search_query;
    int    search_query_capacity;
    int   *search_query_len;
    int   *search_cursor_pos;
    int   *search_hit_line;
    int   *search_hit_char;
    int   *search_hit_ordinal;
    int   *search_match_count;
    const char **ac_matches;
    int   *ac_count;
    int   *ac_sel;
    char  *ac_ghost;
    int    ac_ghost_capacity;
    char  *ac_hint;
    int    ac_hint_capacity;
    int   *cursor_px;
    int   *cursor_py;
} ReplUiState;

typedef struct {
    int *use_accum;
    int *accum_aa_enabled;
    int *accum_samples;
    float *accum_jitter_x;
    float *accum_jitter_y;
    int *multisample_enabled;
    int *line_smooth_enabled;
    GLUquadric **quadric;
    GLUtesselator **tess;
    TessVertex *tess_verts;
    int *tess_vert_count;
    SceneLight *lights;
    float *clear_color;
} ReplRenderState;

typedef struct {
    char (*workspace_header_lines)[WORKSPACE_HEADER_LINE_LEN];
    int   *workspace_header_line_count;
    char  *scratch_buf;
    int    scratch_capacity;
    char (*render_state_lines)[64];
    char (*cam_lines)[96];
    const char **header_pre;
    const char **header_post;
    const char **footer_pre_init;
    const char **footer_post_init;
    const char **grid_names;
    const char **grid_major_names;
    const char **grid_extent_names;
    const char **axes_names;
    int *init_attenuate_points;
} ReplExportState;

ReplCommandState       repl_command_state_live(void);
ReplEditorState        repl_editor_state_live(void);
ReplViewState          repl_view_state_live(void);
ReplPresentationState  repl_presentation_state_live(void);
ReplReplayRuntimeState repl_replay_runtime_state_live(void);
ReplUiState            repl_ui_state_live(void);
ReplRenderState        repl_render_state_live(void);
ReplExportState        repl_export_state_live(void);

/* Command arrays */
extern GLCmd            g_cmds[MAX_COMMANDS];
extern int              g_num_cmds;
extern int              g_normals_dirty;
extern GLCmd            g_flat_cmds[MAX_COMMANDS];
extern int              g_num_flat_cmds;
extern int              g_flat_dirty;
extern FlatCmdLocalVars g_flat_cmd_local_vars[MAX_COMMANDS];

/* Editor */
extern char g_input[MAX_INPUT_LEN];
extern int  g_input_len;
extern int  g_cursor_pos;
extern int  g_edit_line;
extern char g_newline_buf[MAX_INPUT_LEN];
extern int  g_newline_len;
extern int  g_inserting;

/* Camera / window */
extern float g_cam_rx, g_cam_ry;
extern float g_cam_dist;
extern float g_cam_tx, g_cam_ty, g_cam_tz;
extern float g_cam_motion_glow;
extern int   g_mouse_x, g_mouse_y;
extern int   g_mouse_btn;
extern int   g_win_w, g_win_h;

/* Code panel, cursor, and animation */
extern float g_panel_frac;
extern int   g_resizing_panel;
extern int   g_scroll;
extern int   g_scroll_follow_cursor;
extern int   g_cursor_on;
extern int   g_blink_tick;
extern float g_anim_time;
extern int   g_t_playing;
extern int   g_t_var_idx;

/* Accumulation buffer */
extern int   g_use_accum;
extern int   g_accum_aa_enabled;
extern int   g_accum_samples;
extern float g_accum_jitter_x;
extern float g_accum_jitter_y;
extern int   g_multisample_enabled;
extern int   g_line_smooth_enabled;

/* Presentation toggles */
extern int         g_show_help;
extern int         g_help_tab;
extern int         g_help_scroll;
extern int         g_wireframe;
extern int         g_grid_theme;
extern int         g_grid_major_idx;
extern int         g_grid_extent_idx;
extern int         g_axes_theme;
extern const float g_grid_major_steps[GRID_MAJOR_COUNT];
extern const float g_grid_extents[GRID_EXTENT_COUNT];
extern float       g_focus_vtx[3];
extern int         g_focus_vtx_valid;
extern int         g_show_vnums;
extern int         g_show_normals;
extern int         g_show_indices;
extern int         g_wrap_at_comma;
extern int         g_code_panel_layout;
extern int         g_show_guides;
extern int         g_xform_guide_mode;
extern int         g_autonormal;
extern int         g_show_lights;
extern int         g_backdrop_mode;
extern int         g_cam_rotate;
extern int         g_example_idx;
extern int         g_user_lighting_enabled;
extern int         g_show_outlines;
extern int         g_show_vpoints;
extern int         g_highlight_current_poly;
extern int         g_current_block_begin;
extern int         g_current_block_end;
extern int         g_ortho_mode;

/* Replay */
extern int   g_replay_active;
extern int   g_replay_state;
extern int   g_replay_pc;
extern int   g_replay_mode;
extern float g_replay_speed;
extern float g_replay_accum;
extern float g_replay_fade_speed;
extern int   g_replay_src_line;
extern int   g_replay_total_flat;
extern int   g_replay_expand_args;

/* UI panels and status */
extern int   g_show_var_panel;
extern int   g_drag_var;
extern int   g_drag_log_mode;
extern float g_drag_start_val;
extern int   g_drag_start_x;
extern int   g_show_profile_panel;
extern char  g_status[256];
extern int   g_status_ttl;

/* Render resources */
extern GLUquadric    *g_quadric;
extern GLUtesselator *g_tess;
extern TessVertex     g_tess_verts[TESS_VERT_BUF_SIZE];
extern int            g_tess_vert_count;
extern SceneLight     g_lights[MAX_LIGHTS];

/* Max brightness (V in HSV) allowed for glClearColor channels.
 * Since max(r,g,b) == V, capping V caps all channels. */
#define CP_CLEAR_MAX_V 0.1f
extern float g_clear_color[4];

/* Search */
extern int  g_search_active;
extern char g_search_query[MAX_INPUT_LEN];
extern int  g_search_query_len;
extern int  g_search_cursor_pos;
extern int  g_search_hit_line;
extern int  g_search_hit_char;
extern int  g_search_hit_ordinal;
extern int  g_search_match_count;

/* Autocomplete */
extern const char *g_ac_matches[MAX_AC_MATCHES];
extern int         g_ac_count;
extern int         g_ac_sel;
extern char        g_ac_ghost[MAX_LINE_LEN];
extern char        g_ac_hint[MAX_LINE_LEN];
extern int         g_cursor_px;
extern int         g_cursor_py;

/* Selection and clipboard */
extern int   g_sel_anchor;
extern int   g_sel_end;
extern GLCmd g_clipboard[MAX_COMMANDS];
extern int   g_clipboard_count;

/* Scratch and workspace/export state */
extern char g_scratch_buf[256];

extern char g_workspace_header_lines[MAX_WORKSPACE_HEADER_LINES][WORKSPACE_HEADER_LINE_LEN];
extern int  g_workspace_header_line_count;
void refresh_workspace_header_lines(void);
int  parse_workspace_header_line(const char *line);

extern char        g_render_state_lines[RENDER_STATE_LINE_COUNT][64];
extern char        g_cam_lines[CAM_LINE_COUNT][96];
extern const char *g_header_pre[];
extern const char *g_header_post[];
extern const char *g_footer_pre_init[];
extern const char *g_footer_post_init[];
extern const char *g_grid_names[];
extern const char *g_grid_major_names[GRID_MAJOR_COUNT];
extern const char *g_grid_extent_names[GRID_EXTENT_COUNT];
extern const char *g_axes_names[];
extern int         g_init_attenuate_points;

/* Config items */
extern CfgItem g_cfg_items[];
extern const int CFG_ITEM_COUNT;

#endif /* REPL_STATE_H */
