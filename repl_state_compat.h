#ifndef REPL_STATE_COMPAT_H
#define REPL_STATE_COMPAT_H

#include "repl_executor.h"
#include "repl_config.h"
#include "repl_state.h"

/* Compatibility facade for the pre-phase-2 global state layout.
 * New code should eventually move to the typed runtime APIs in repl_state.h,
 * but the existing modules still rely on these bundle structs and externs. */

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

ReplEditorState        repl_editor_state_live(void);
ReplViewState          repl_view_state_live(void);



extern float g_panel_frac;
extern int   g_resizing_panel;
extern int   g_scroll;
extern int   g_scroll_follow_cursor;
extern int   g_cursor_on;
extern int   g_blink_tick;
extern float g_anim_time;
extern int   g_t_playing;
extern int   g_t_var_idx;

extern int   g_use_accum;
extern int   g_accum_aa_enabled;
extern int   g_accum_samples;
extern float g_accum_jitter_x;
extern float g_accum_jitter_y;
extern int   g_multisample_enabled;
extern int   g_line_smooth_enabled;

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
extern int         g_example_idx;
extern int         g_show_outlines;
extern int         g_show_vpoints;
extern int         g_highlight_current_poly;
extern int         g_ortho_mode;

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

extern int   g_show_var_panel;
extern int   g_drag_var;
extern int   g_drag_log_mode;
extern float g_drag_start_val;
extern int   g_drag_start_x;
extern int   g_show_profile_panel;
extern char  g_status[256];
extern int   g_status_ttl;

extern GLUquadric    *g_quadric;
extern GLUtesselator *g_tess;
extern TessVertex     g_tess_verts[TESS_VERT_BUF_SIZE];
extern int            g_tess_vert_count;
extern SceneLight     g_lights[MAX_LIGHTS];
extern float          g_clear_color[4];

extern int  g_search_active;
extern char g_search_query[MAX_INPUT_LEN];
extern int  g_search_query_len;
extern int  g_search_cursor_pos;
extern int  g_search_hit_line;
extern int  g_search_hit_char;
extern int  g_search_hit_ordinal;
extern int  g_search_match_count;

extern const char *g_ac_matches[MAX_AC_MATCHES];
extern int         g_ac_count;
extern int         g_ac_sel;
extern char        g_ac_ghost[MAX_LINE_LEN];
extern char        g_ac_hint[MAX_LINE_LEN];
extern int         g_cursor_px;
extern int         g_cursor_py;

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

#endif /* REPL_STATE_COMPAT_H */
