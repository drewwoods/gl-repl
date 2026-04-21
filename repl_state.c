#include "sample.h"

ReplCommandState repl_command_state_live(void) {
    ReplCommandState state = {
        g_cmds,
        &g_num_cmds,
        MAX_COMMANDS,
        &g_normals_dirty,
        g_flat_cmds,
        &g_num_flat_cmds,
        &g_flat_dirty,
        g_flat_cmd_local_vars
    };
    return state;
}

ReplEditorState repl_editor_state_live(void) {
    ReplEditorState state = {
        g_input,
        MAX_INPUT_LEN,
        &g_input_len,
        &g_cursor_pos,
        &g_edit_line,
        g_newline_buf,
        MAX_INPUT_LEN,
        &g_newline_len,
        &g_inserting,
        &g_sel_anchor,
        &g_sel_end,
        g_clipboard,
        &g_clipboard_count
    };
    return state;
}

ReplViewState repl_view_state_live(void) {
    ReplViewState state = {
        &g_cam_rx,
        &g_cam_ry,
        &g_cam_dist,
        &g_cam_tx,
        &g_cam_ty,
        &g_cam_tz,
        &g_cam_motion_glow,
        &g_mouse_x,
        &g_mouse_y,
        &g_mouse_btn,
        &g_win_w,
        &g_win_h
    };
    return state;
}

ReplPresentationState repl_presentation_state_live(void) {
    ReplPresentationState state = {
        &g_show_help,
        &g_help_tab,
        &g_help_scroll,
        &g_wireframe,
        &g_grid_theme,
        &g_grid_major_idx,
        &g_grid_extent_idx,
        &g_axes_theme,
        g_grid_major_steps,
        g_grid_extents,
        g_focus_vtx,
        &g_focus_vtx_valid,
        &g_show_vnums,
        &g_show_normals,
        &g_show_indices,
        &g_wrap_at_comma,
        &g_code_panel_layout,
        &g_show_guides,
        &g_xform_guide_mode,
        &g_autonormal,
        &g_show_lights,
        &g_backdrop_mode,
        &g_cam_rotate,
        &g_example_idx,
        &g_user_lighting_enabled,
        &g_show_outlines,
        &g_show_vpoints,
        &g_highlight_current_poly,
        &g_current_block_begin,
        &g_current_block_end,
        &g_ortho_mode
    };
    return state;
}

ReplReplayRuntimeState repl_replay_runtime_state_live(void) {
    ReplReplayRuntimeState state = {
        &g_replay_active,
        &g_replay_state,
        &g_replay_pc,
        &g_replay_mode,
        &g_replay_speed,
        &g_replay_accum,
        &g_replay_fade_speed,
        &g_replay_src_line,
        &g_replay_total_flat,
        &g_replay_expand_args
    };
    return state;
}

ReplUiState repl_ui_state_live(void) {
    ReplUiState state = {
        &g_panel_frac,
        &g_resizing_panel,
        &g_scroll,
        &g_scroll_follow_cursor,
        &g_cursor_on,
        &g_blink_tick,
        &g_anim_time,
        &g_t_playing,
        &g_t_var_idx,
        &g_show_var_panel,
        &g_drag_var,
        &g_drag_log_mode,
        &g_drag_start_val,
        &g_drag_start_x,
        &g_show_profile_panel,
        g_status,
        (int)sizeof(g_status),
        &g_status_ttl,
        &g_search_active,
        g_search_query,
        MAX_INPUT_LEN,
        &g_search_query_len,
        &g_search_cursor_pos,
        &g_search_hit_line,
        &g_search_hit_char,
        &g_search_hit_ordinal,
        &g_search_match_count,
        g_ac_matches,
        &g_ac_count,
        &g_ac_sel,
        g_ac_ghost,
        MAX_LINE_LEN,
        g_ac_hint,
        MAX_LINE_LEN,
        &g_cursor_px,
        &g_cursor_py
    };
    return state;
}

ReplRenderState repl_render_state_live(void) {
    ReplRenderState state = {
        &g_use_accum,
        &g_accum_aa_enabled,
        &g_accum_samples,
        &g_accum_jitter_x,
        &g_accum_jitter_y,
        &g_multisample_enabled,
        &g_line_smooth_enabled,
        &g_quadric,
        &g_tess,
        g_tess_verts,
        &g_tess_vert_count,
        g_lights,
        g_clear_color
    };
    return state;
}

ReplExportState repl_export_state_live(void) {
    ReplExportState state = {
        g_workspace_header_lines,
        &g_workspace_header_line_count,
        g_scratch_buf,
        (int)sizeof(g_scratch_buf),
        g_render_state_lines,
        g_cam_lines,
        g_header_pre,
        g_header_post,
        g_footer_pre_init,
        g_footer_post_init,
        g_grid_names,
        g_grid_major_names,
        g_grid_extent_names,
        g_axes_names,
        &g_init_attenuate_points
    };
    return state;
}
