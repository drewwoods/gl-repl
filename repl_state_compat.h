#ifndef REPL_STATE_COMPAT_H
#define REPL_STATE_COMPAT_H

#include "repl_executor.h"
#include "repl_config.h"
#include "repl_state.h"

/* Compatibility facade for the pre-phase-2 global state layout.
 * New code should eventually move to the typed runtime APIs in repl_state.h,
 * but some modules still rely on these legacy compatibility names. */

#ifndef REPL_STATE_IMPLEMENTATION
#define g_use_accum                (*repl_state_render()->use_accum)
#define g_accum_aa_enabled         (*repl_state_render()->accum_aa_enabled)
#define g_accum_samples            (*repl_state_render()->accum_samples)
#define g_accum_jitter_x           (*repl_state_render()->accum_jitter_x)
#define g_accum_jitter_y           (*repl_state_render()->accum_jitter_y)
#define g_multisample_enabled      (*repl_state_render()->multisample_enabled)
#define g_line_smooth_enabled      (*repl_state_render()->line_smooth_enabled)
#define g_init_attenuate_points    (*repl_state_render()->point_attenuation_enabled)
#define g_quadric                  (*repl_state_render()->quadric)
#define g_tess                     (*repl_state_render()->tess)
#define g_tess_verts               (repl_state_render()->tess_verts)
#define g_tess_vert_count          (*repl_state_render()->tess_vert_count)
#define g_lights                   (repl_state_render()->lights)
#define g_clear_color              (repl_state_render()->clear_color)

#define g_anim_time              (*repl_state_variables()->anim_time)
#define g_t_playing              (*repl_state_variables()->time_playing)
#define g_t_var_idx              (*repl_state_variables()->time_var_idx)

#define g_panel_frac              (*repl_state_code_panel()->panel_frac)
#define g_resizing_panel          (*repl_state_code_panel()->resizing_panel)
#define g_scroll                  (*repl_state_code_panel()->scroll)
#define g_scroll_follow_cursor    (*repl_state_code_panel()->scroll_follow_cursor)
#define g_cursor_on               (*repl_state_code_panel()->cursor_visible)
#define g_blink_tick              (*repl_state_code_panel()->blink_tick)
#define g_cursor_px               (*repl_state_code_panel()->cursor_px)
#define g_cursor_py               (*repl_state_code_panel()->cursor_py)

#define g_show_help               (*repl_state_help()->visible)
#define g_help_tab                (*repl_state_help()->tab_idx)
#define g_help_scroll             (*repl_state_help()->scroll)

#define g_show_var_panel          (*repl_state_variable_panel()->visible)

#define g_drag_var                (*repl_state_variable_drag()->var_idx)
#define g_drag_log_mode           (*repl_state_variable_drag()->log_mode)
#define g_drag_start_val          (*repl_state_variable_drag()->start_value)
#define g_drag_start_x            (*repl_state_variable_drag()->start_x)

#define g_show_profile_panel       (*repl_state_profile_panel()->mode)

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

void refresh_workspace_header_lines(void);
int  parse_workspace_header_line(const char *line);

extern const char  *g_grid_names[];
extern const char  *g_grid_major_names[GRID_MAJOR_COUNT];
extern const char  *g_grid_extent_names[GRID_EXTENT_COUNT];
extern const char  *g_axes_names[];

#endif /* REPL_STATE_COMPAT_H */
