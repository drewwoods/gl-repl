#include "repl_state.h"

#include "repl_clipboard.h"
#include "repl_command_store.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_eval.h"
#include "repl_source_scope.h"
#include "repl_state_compat.h"

/* Import/export helpers stay in repl_export.c for now; state exposes them. */
void refresh_workspace_header_lines(void);
int  parse_workspace_header_line(const char *line);

static ReplRuntimeState g_repl_state = {
    .document = {
        .cmds = g_cmds,
        .cmd_count = &g_num_cmds,
        .capacity = MAX_COMMANDS,
        .edit_line_idx = &g_edit_line,
        .normals_dirty = &g_normals_dirty,
    },
    .flat_program = {
        .cmds = g_flat_cmds,
        .local_vars = g_flat_cmd_local_vars,
        .cmd_count = &g_num_flat_cmds,
        .capacity = MAX_COMMANDS,
        .dirty = &g_flat_dirty,
        .user_lighting_enabled = &g_user_lighting_enabled,
        .current_block_begin_idx = &g_current_block_begin,
        .current_block_end_idx = &g_current_block_end,
        .current_block_source_line_idx = &g_current_block_line,
    },
    .variables = {
        .vars = g_predef_vars,
        .var_count = &g_num_predef_vars,
        .time_var_idx = &g_t_var_idx,
        .time_playing = &g_t_playing,
        .anim_time = &g_anim_time,
    },
    .editor_input = {
        .input = g_input,
        .input_capacity = MAX_INPUT_LEN,
        .input_len = &g_input_len,
        .cursor_pos = &g_cursor_pos,
        .edit_line_idx = &g_edit_line,
        .pending_newline = g_newline_buf,
        .pending_newline_capacity = MAX_INPUT_LEN,
        .pending_newline_len = &g_newline_len,
        .insert_mode = &g_inserting,
    },
    .selection = {
        .anchor_idx = &g_sel_anchor,
        .end_idx = &g_sel_end,
    },
    .clipboard = {
        .cmds = g_clipboard,
        .cmd_count = &g_clipboard_count,
    },
    .code_panel = {
        .panel_frac = &g_panel_frac,
        .resizing_panel = &g_resizing_panel,
        .scroll = &g_scroll,
        .scroll_follow_cursor = &g_scroll_follow_cursor,
        .cursor_visible = &g_cursor_on,
        .blink_tick = &g_blink_tick,
        .cursor_px = &g_cursor_px,
        .cursor_py = &g_cursor_py,
    },
    .help = {
        .visible = &g_show_help,
        .tab_idx = &g_help_tab,
        .scroll = &g_help_scroll,
    },
    .variable_panel = {
        .visible = &g_show_var_panel,
    },
    .variable_drag = {
        .var_idx = &g_drag_var,
        .log_mode = &g_drag_log_mode,
        .start_value = &g_drag_start_val,
        .start_x = &g_drag_start_x,
    },
    .profile_panel = {
        .mode = &g_show_profile_panel,
    },
    .status = {
        .text = g_status,
        .capacity = (int)sizeof(g_status),
        .ttl = &g_status_ttl,
    },
    .search = {
        .active = &g_search_active,
        .query = g_search_query,
        .query_capacity = MAX_INPUT_LEN,
        .query_len = &g_search_query_len,
        .cursor_pos = &g_search_cursor_pos,
        .hit_line_idx = &g_search_hit_line,
        .hit_char_idx = &g_search_hit_char,
        .hit_ordinal = &g_search_hit_ordinal,
        .match_count = &g_search_match_count,
    },
    .autocomplete = {
        .matches = g_ac_matches,
        .match_count = &g_ac_count,
        .selected_idx = &g_ac_sel,
        .ghost = g_ac_ghost,
        .ghost_capacity = MAX_LINE_LEN,
        .hint = g_ac_hint,
        .hint_capacity = MAX_LINE_LEN,
    },
    .camera = {
        .rx = &g_cam_rx,
        .ry = &g_cam_ry,
        .dist = &g_cam_dist,
        .tx = &g_cam_tx,
        .ty = &g_cam_ty,
        .tz = &g_cam_tz,
        .motion_glow = &g_cam_motion_glow,
        .auto_rotate = &g_cam_rotate,
    },
    .pointer = {
        .mouse_x = &g_mouse_x,
        .mouse_y = &g_mouse_y,
        .mouse_button = &g_mouse_btn,
    },
    .viewport = {
        .window_w = &g_win_w,
        .window_h = &g_win_h,
    },
    .presentation = {
        .wireframe = &g_wireframe,
        .grid_theme = &g_grid_theme,
        .grid_major_idx = &g_grid_major_idx,
        .grid_extent_idx = &g_grid_extent_idx,
        .axes_theme = &g_axes_theme,
        .show_vertex_labels = &g_show_vnums,
        .show_normal_vectors = &g_show_normals,
        .show_vertex_indices = &g_show_indices,
        .show_vertex_outlines = &g_show_outlines,
        .show_vertex_points = &g_show_vpoints,
        .show_vertex_guides = &g_show_guides,
        .xform_guide_mode = &g_xform_guide_mode,
        .autonormal = &g_autonormal,
        .show_light_indicators = &g_show_lights,
        .backdrop_mode = &g_backdrop_mode,
        .highlight_current_poly = &g_highlight_current_poly,
        .ortho_mode = &g_ortho_mode,
        .wrap_at_comma = &g_wrap_at_comma,
        .code_panel_layout = &g_code_panel_layout,
        .grid_major_steps = g_grid_major_steps,
        .grid_extents = g_grid_extents,
        .focus_vertex = g_focus_vtx,
        .focus_vertex_valid = &g_focus_vtx_valid,
    },
    .render = {
        .use_accum = &g_use_accum,
        .accum_aa_enabled = &g_accum_aa_enabled,
        .accum_samples = &g_accum_samples,
        .accum_jitter_x = &g_accum_jitter_x,
        .accum_jitter_y = &g_accum_jitter_y,
        .multisample_enabled = &g_multisample_enabled,
        .line_smooth_enabled = &g_line_smooth_enabled,
        .point_attenuation_enabled = &g_init_attenuate_points,
        .quadric = &g_quadric,
        .tess = &g_tess,
        .tess_verts = g_tess_verts,
        .tess_vert_count = &g_tess_vert_count,
        .lights = g_lights,
        .clear_color = g_clear_color,
    },
    .render_derived = {
        .focus_vertex = g_focus_vtx,
        .focus_vertex_valid = &g_focus_vtx_valid,
    },
    .replay = {
        .active = &g_replay_active,
        .state = &g_replay_state,
        .pc = &g_replay_pc,
        .mode = &g_replay_mode,
        .speed = &g_replay_speed,
        .accum = &g_replay_accum,
        .fade_speed = &g_replay_fade_speed,
        .src_line_idx = &g_replay_src_line,
        .total_flat_cmds = &g_replay_total_flat,
        .expand_args = &g_replay_expand_args,
    },
    .scenes = {
        .active_example_idx = &g_example_idx,
        .workspace_dir = g_workspace_dir,
        .workspace_dir_capacity = (int)sizeof(g_workspace_dir),
    },
    .import_export = {
        .workspace_header_lines = g_workspace_header_lines,
        .workspace_header_line_count = &g_workspace_header_line_count,
        .render_state_lines = g_render_state_lines,
        .cam_lines = g_cam_lines,
        .export_scene_name_hint = &g_export_scene_name_hint,
        .pending_scene_name = g_pending_scene_name,
        .pending_workspace_dir = g_pending_workspace_dir,
    },
};

static void ensure_t_var_idx(void) {
    if (g_t_var_idx >= 0 && g_t_var_idx < g_num_predef_vars &&
        strcmp(g_predef_vars[g_t_var_idx].name, "t") == 0)
        return;
    g_t_var_idx = find_predef_var_idx("t");
}

static void reset_time_state(void) {
    g_anim_time = 0.0f;
    init_predef_vars();
    ensure_t_var_idx();
}

const ReplDocumentState *repl_state_document(void) {
    return &g_repl_state.document;
}

ReplDocumentState *repl_state_document_mut(void) {
    return &g_repl_state.document;
}

void repl_state_document_reset(void) {
    ReplCommandStore store = repl_command_store_live();
    repl_command_store_load(&store, NULL, 0, 0);
}

const ReplFlatProgramState *repl_state_flat_program(void) {
    return &g_repl_state.flat_program;
}

ReplFlatProgramState *repl_state_flat_program_mut(void) {
    return &g_repl_state.flat_program;
}

void repl_state_flat_program_reset(void) {
    g_num_flat_cmds = 0;
    g_flat_dirty = 1;
    g_user_lighting_enabled = 0;
    g_current_block_begin = -1;
    g_current_block_end = -1;
    g_current_block_line = -1;
}

void repl_state_mark_flat_dirty(void) {
    g_flat_dirty = 1;
}

void repl_state_mark_normals_dirty(void) {
    g_normals_dirty = 1;
    g_flat_dirty = 1;
    depth_cache_invalidate();
}

FlatProgramView repl_state_flat_program_view(void) {
    return repl_flat_program_view_live();
}

const ReplVariableState *repl_state_variables(void) {
    return &g_repl_state.variables;
}

ReplVariableState *repl_state_variables_mut(void) {
    return &g_repl_state.variables;
}

void repl_state_variables_reset(void) {
    init_predef_vars();
    g_t_var_idx = find_predef_var_idx("t");
}

void repl_state_time_advance(float dt) {
    if (dt <= 0.0f)
        return;

    ensure_t_var_idx();
    g_anim_time += dt;
    if (g_t_playing && g_t_var_idx >= 0) {
        g_predef_vars[g_t_var_idx].value += dt;
        g_flat_dirty = 1;
    }
}

void repl_state_time_reset_to_zero(void) {
    ensure_t_var_idx();
    if (g_t_var_idx < 0)
        return;

    g_predef_vars[g_t_var_idx].value = 0.0f;
    g_flat_dirty = 1;
}

const ReplEditorInputState *repl_state_editor_input(void) {
    return &g_repl_state.editor_input;
}

ReplEditorInputState *repl_state_editor_input_mut(void) {
    return &g_repl_state.editor_input;
}

void repl_state_editor_input_reset(void) {
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    g_newline_buf[0] = '\0';
    g_newline_len = 0;
    g_inserting = 0;
}

const ReplSelectionState *repl_state_selection(void) {
    return &g_repl_state.selection;
}

ReplSelectionState *repl_state_selection_mut(void) {
    return &g_repl_state.selection;
}

void repl_state_selection_clear(void) {
    clear_selection();
}

const ReplClipboardState *repl_state_clipboard(void) {
    return &g_repl_state.clipboard;
}

ReplClipboardState *repl_state_clipboard_mut(void) {
    return &g_repl_state.clipboard;
}

void repl_state_clipboard_clear(void) {
    g_clipboard_count = 0;
}

const ReplCodePanelRuntimeState *repl_state_code_panel(void) {
    return &g_repl_state.code_panel;
}

ReplCodePanelRuntimeState *repl_state_code_panel_mut(void) {
    return &g_repl_state.code_panel;
}

void repl_state_code_panel_reset(void) {
    g_scroll = 0;
    g_scroll_follow_cursor = 0;
    g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
    g_resizing_panel = 0;
}

const ReplHelpState *repl_state_help(void) {
    return &g_repl_state.help;
}

ReplHelpState *repl_state_help_mut(void) {
    return &g_repl_state.help;
}

void repl_state_help_reset(void) {
    g_show_help = 0;
    g_help_tab = 0;
    g_help_scroll = 0;
}

const ReplVariablePanelState *repl_state_variable_panel(void) {
    return &g_repl_state.variable_panel;
}

ReplVariablePanelState *repl_state_variable_panel_mut(void) {
    return &g_repl_state.variable_panel;
}

const ReplVariableDragState *repl_state_variable_drag(void) {
    return &g_repl_state.variable_drag;
}

ReplVariableDragState *repl_state_variable_drag_mut(void) {
    return &g_repl_state.variable_drag;
}

void repl_state_variable_drag_reset(void) {
    g_drag_var = -1;
    g_drag_log_mode = 0;
    g_drag_start_val = 0.0f;
    g_drag_start_x = 0;
}

const ReplProfilePanelState *repl_state_profile_panel(void) {
    return &g_repl_state.profile_panel;
}

ReplProfilePanelState *repl_state_profile_panel_mut(void) {
    return &g_repl_state.profile_panel;
}

const ReplStatusState *repl_state_status(void) {
    return &g_repl_state.status;
}

void repl_status_set(const char *message) {
    if (!message)
        message = "";
    strncpy(g_status, message, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    g_status_ttl = 240;
}

void repl_status_clear(void) {
    g_status[0] = '\0';
    g_status_ttl = 0;
}

void repl_status_tick(void) {
    if (g_status_ttl > 0)
        g_status_ttl--;
}

const ReplSearchState *repl_state_search(void) {
    return &g_repl_state.search;
}

ReplSearchState *repl_state_search_mut(void) {
    return &g_repl_state.search;
}

void repl_state_search_clear(void) {
    search_clear_all();
}

const ReplAutocompleteState *repl_state_autocomplete(void) {
    return &g_repl_state.autocomplete;
}

ReplAutocompleteState *repl_state_autocomplete_mut(void) {
    return &g_repl_state.autocomplete;
}

void repl_state_autocomplete_clear(void) {
    g_ac_count = 0;
    g_ac_sel = 0;
    g_ac_ghost[0] = '\0';
    g_ac_hint[0] = '\0';
}

const ReplCameraState *repl_state_camera(void) {
    return &g_repl_state.camera;
}

ReplCameraState *repl_state_camera_mut(void) {
    return &g_repl_state.camera;
}

ReplCameraState repl_state_camera_snapshot(void) {
    return g_repl_state.camera;
}

void repl_state_camera_reset_default(void) {
    g_cam_rx = 20.0f;
    g_cam_ry = 30.0f;
    g_cam_dist = 5.0f;
    g_cam_tx = 0.0f;
    g_cam_ty = 0.0f;
    g_cam_tz = 0.0f;
    g_cam_motion_glow = 0.0f;
}

const ReplPointerState *repl_state_pointer(void) {
    return &g_repl_state.pointer;
}

ReplPointerState *repl_state_pointer_mut(void) {
    return &g_repl_state.pointer;
}

const ReplViewportState *repl_state_viewport(void) {
    return &g_repl_state.viewport;
}

ReplViewportState *repl_state_viewport_mut(void) {
    return &g_repl_state.viewport;
}

const ReplPresentationState *repl_state_presentation(void) {
    return &g_repl_state.presentation;
}

ReplPresentationState *repl_state_presentation_mut(void) {
    return &g_repl_state.presentation;
}

ReplPresentationState repl_state_presentation_snapshot(void) {
    return g_repl_state.presentation;
}

void repl_state_presentation_reset_defaults(void) {
    g_wireframe = CFG_DEFAULT_WIREFRAME;
    g_grid_theme = CFG_DEFAULT_GRID_THEME;
    g_grid_major_idx = CFG_DEFAULT_GRID_MAJOR_IDX;
    g_grid_extent_idx = CFG_DEFAULT_GRID_EXTENT_IDX;
    g_axes_theme = CFG_DEFAULT_AXES_THEME;
    g_show_vnums = CFG_DEFAULT_VERTEX_LABELS;
    g_show_indices = CFG_DEFAULT_VERTEX_INDICES;
    g_show_normals = CFG_DEFAULT_NORMAL_VECTORS;
    g_show_outlines = CFG_DEFAULT_VERTEX_OUTLINES;
    g_show_vpoints = CFG_DEFAULT_VERTEX_POINTS;
    g_show_guides = CFG_DEFAULT_VERTEX_GUIDES;
    g_xform_guide_mode = CFG_DEFAULT_XFORM_GUIDE_MODE;
    g_show_lights = CFG_DEFAULT_LIGHT_INDICATORS;
    g_backdrop_mode = CFG_DEFAULT_BACKDROP_MODE;
    g_cam_rotate = CFG_DEFAULT_CAMERA_ROTATE;
    g_highlight_current_poly = 1;
    g_ortho_mode = 0;
    g_wrap_at_comma = CFG_DEFAULT_WRAP_AT_COMMA;
    g_code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
}

void repl_state_presentation_reset_example_defaults(void) {
    g_wireframe = CFG_DEFAULT_WIREFRAME;
    g_grid_theme = CFG_DEFAULT_GRID_THEME;
    g_grid_major_idx = CFG_DEFAULT_GRID_MAJOR_IDX;
    g_grid_extent_idx = CFG_DEFAULT_GRID_EXTENT_IDX;
    g_axes_theme = CFG_DEFAULT_AXES_THEME;
    g_show_vnums = CFG_DEFAULT_VERTEX_LABELS;
    g_show_indices = CFG_DEFAULT_VERTEX_INDICES;
    g_show_normals = CFG_DEFAULT_NORMAL_VECTORS;
    g_show_outlines = CFG_DEFAULT_VERTEX_OUTLINES;
    g_show_vpoints = CFG_DEFAULT_VERTEX_POINTS;
    g_show_guides = CFG_DEFAULT_VERTEX_GUIDES;
    g_xform_guide_mode = CFG_DEFAULT_XFORM_GUIDE_MODE;
    g_show_lights = CFG_DEFAULT_LIGHT_INDICATORS;
    g_backdrop_mode = CFG_DEFAULT_BACKDROP_MODE;
    g_cam_rotate = CFG_DEFAULT_CAMERA_ROTATE;
}

const ReplRenderState *repl_state_render(void) {
    return &g_repl_state.render;
}

ReplRenderState *repl_state_render_mut(void) {
    return &g_repl_state.render;
}

void repl_state_render_reset_defaults(void) {
    g_multisample_enabled = CFG_DEFAULT_MULTISAMPLE;
    g_line_smooth_enabled = CFG_DEFAULT_LINE_SMOOTH;
    g_init_attenuate_points = CFG_DEFAULT_ATTENUATE_POINTS;
    g_clear_color[0] = 0.10f;
    g_clear_color[1] = 0.10f;
    g_clear_color[2] = 0.13f;
    g_clear_color[3] = 1.0f;
}

void repl_state_render_init_resources(void) {
    /*
     * Resource creation still lives in repl_core.c's GL bootstrap path.
     * The state facade exposes this hook so the ownership boundary is explicit
     * before the storage migration lands.
     */
}

void repl_state_render_destroy_resources(void) {
    /*
     * Resource teardown still lives with the GL bootstrap owner for now.
     * This is a placeholder hook for the future render-resource split.
     */
}

const ReplRenderDerivedState *repl_state_render_derived(void) {
    return &g_repl_state.render_derived;
}

ReplRenderDerivedState *repl_state_render_derived_mut(void) {
    return &g_repl_state.render_derived;
}

const ReplReplayRuntimeState *repl_state_replay(void) {
    return &g_repl_state.replay;
}

ReplReplayRuntimeState *repl_state_replay_mut(void) {
    return &g_repl_state.replay;
}

void repl_state_replay_reset(void) {
    g_replay_active = 0;
    g_replay_state = REPLAY_OFF;
    g_replay_pc = 0;
    g_replay_mode = REPLAY_MODE_POLYGON;
    g_replay_speed = 1.0f;
    g_replay_accum = 0.0f;
    g_replay_fade_speed = 0.0f;
    g_replay_src_line = 0;
    g_replay_total_flat = 0;
    g_replay_expand_args = 0;
}

const ReplSceneRuntimeState *repl_state_scenes(void) {
    return &g_repl_state.scenes;
}

ReplSceneRuntimeState *repl_state_scenes_mut(void) {
    return &g_repl_state.scenes;
}

void repl_state_workspace_set_dir(const char *dir) {
    repl_set_workspace_dir(dir);
}

const char *repl_state_workspace_dir(void) {
    return repl_workspace_dir();
}

const ReplImportExportState *repl_state_import_export(void) {
    return &g_repl_state.import_export;
}

ReplImportExportState *repl_state_import_export_mut(void) {
    return &g_repl_state.import_export;
}

void repl_state_import_export_reset(void) {
    memset(g_workspace_header_lines, 0, sizeof(g_workspace_header_lines));
    g_workspace_header_line_count = 0;
    memset(g_render_state_lines, 0, sizeof(g_render_state_lines));
    memset(g_cam_lines, 0, sizeof(g_cam_lines));
    g_export_scene_name_hint = NULL;
    g_pending_scene_name[0] = '\0';
    g_pending_workspace_dir[0] = '\0';
}

void repl_state_refresh_workspace_header_lines(void) {
    refresh_workspace_header_lines();
}

int repl_state_parse_workspace_header_line(const char *line) {
    return parse_workspace_header_line(line);
}

void repl_state_init_defaults(void) {
    repl_state_reset_all();
}

void repl_state_reset_all(void) {
    repl_state_document_reset();
    repl_state_flat_program_reset();
    repl_state_editor_input_reset();
    repl_scenes_reset();
    g_example_idx = -1;
    repl_state_code_panel_reset();
    repl_state_render_reset_defaults();
    g_wrap_at_comma = CFG_DEFAULT_WRAP_AT_COMMA;
    g_code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    reset_time_state();
    repl_state_selection_clear();
    repl_editor_reset_transients();
    repl_state_autocomplete_clear();
    repl_state_search_clear();
    repl_state_import_export_reset();
    update_render_state_strings();
    depth_cache_invalidate();
    repl_state_mark_flat_dirty();
    repl_state_mark_normals_dirty();
}

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
