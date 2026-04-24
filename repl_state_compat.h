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

extern int   g_show_var_panel;
extern int   g_drag_var;
extern int   g_drag_log_mode;
extern float g_drag_start_val;
extern int   g_drag_start_x;
extern int   g_show_profile_panel;

#ifndef REPL_STATE_IMPLEMENTATION
#define g_status                  (repl_state_status()->text)
#define g_status_ttl              (*repl_state_status()->ttl)

#define g_search_active           (*repl_state_search()->active)
#define g_search_query            (repl_state_search()->query)
#define g_search_query_len        (*repl_state_search()->query_len)
#define g_search_cursor_pos       (*repl_state_search()->cursor_pos)
#define g_search_hit_line         (*repl_state_search()->hit_line_idx)
#define g_search_hit_char         (*repl_state_search()->hit_char_idx)
#define g_search_hit_ordinal      (*repl_state_search()->hit_ordinal)
#define g_search_match_count      (*repl_state_search()->match_count)

#define g_ac_matches              (repl_state_autocomplete()->matches)
#define g_ac_insert_matches       (repl_state_autocomplete()->insert_matches)
#define g_ac_count                (*repl_state_autocomplete()->match_count)
#define g_ac_sel                  (*repl_state_autocomplete()->selected_idx)
#define g_ac_ghost                (repl_state_autocomplete()->ghost)
#define g_ac_hint                 (repl_state_autocomplete()->hint)
#endif

extern GLUquadric    *g_quadric;
extern GLUtesselator *g_tess;
extern TessVertex     g_tess_verts[TESS_VERT_BUF_SIZE];
extern int            g_tess_vert_count;
extern SceneLight     g_lights[MAX_LIGHTS];
extern float          g_clear_color[4];

extern int            g_cursor_px;
extern int            g_cursor_py;

extern int g_init_attenuate_points;

void refresh_workspace_header_lines(void);
int  parse_workspace_header_line(const char *line);

extern char         g_scratch_buf[256];
extern const char  *g_header_pre[];
extern const char  *g_header_post[];
extern const char  *g_footer_pre_init[];
extern const char  *g_footer_post_init[];
extern const char  *g_grid_names[];
extern const char  *g_grid_major_names[GRID_MAJOR_COUNT];
extern const char  *g_grid_extent_names[GRID_EXTENT_COUNT];
extern const char  *g_axes_names[];

#endif /* REPL_STATE_COMPAT_H */
