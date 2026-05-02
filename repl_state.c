#define REPL_STATE_IMPLEMENTATION
#include "repl_state.h"

#include "repl_command_store.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_pipeline.h"
#include "repl_eval.h"
#include "repl_source_scope.h"
#undef REPL_STATE_IMPLEMENTATION

/* Import/export helpers stay in repl_export.c for now; state exposes them. */
void refresh_workspace_header_lines(void);
int  parse_workspace_header_line(const char *line);
void update_render_state_strings(void);
void update_cam_lines(void);

static const float g_grid_major_steps[GRID_MAJOR_COUNT] = {
    [GRID_MAJOR_1]  = 1.0f,
    [GRID_MAJOR_2]  = 2.0f,
    [GRID_MAJOR_5]  = 5.0f,
    [GRID_MAJOR_10] = 10.0f,
};
static const float g_grid_extents[GRID_EXTENT_COUNT] = {
    [GRID_EXTENT_CLOSE] = 5.0f,
    [GRID_EXTENT_MID]   = 25.0f,
    [GRID_EXTENT_FAR]   = 100.0f,
};
static const ReplRuntimeState g_repl_state_defaults = {
#include "repl_state_defaults.inc"
};

static ReplRuntimeState g_repl_state;

#define g_cmds                      (g_repl_state.document.cmds)
#define g_num_cmds                  (g_repl_state.document.cmd_count)
#define g_edit_line                 (g_repl_state.document.edit_line_idx)
#define g_normals_dirty             (g_repl_state.document.normals_dirty)
#define g_flat_cmds                 (g_repl_state.flat_program.cmds)
#define g_flat_cmd_local_vars       (g_repl_state.flat_program.local_vars)
#define g_num_flat_cmds             (g_repl_state.flat_program.cmd_count)
#define g_flat_dirty                (g_repl_state.flat_program.dirty)
#define g_user_lighting_enabled     (g_repl_state.flat_program.user_lighting_enabled)
#define g_current_block_begin       (g_repl_state.flat_program.current_block_begin_idx)
#define g_current_block_end         (g_repl_state.flat_program.current_block_end_idx)
#define g_current_block_line        (g_repl_state.flat_program.current_block_source_line_idx)
#define g_input                     (g_repl_state.editor_input.input)
#define g_input_len                 (g_repl_state.editor_input.input_len)
#define g_cursor_pos                (g_repl_state.editor_input.cursor_pos)
#define g_newline_buf               (g_repl_state.editor_input.pending_newline)
#define g_newline_len               (g_repl_state.editor_input.pending_newline_len)
#define g_inserting                 (g_repl_state.editor_input.insert_mode)
#define g_clipboard                 (g_repl_state.clipboard.cmds)
#define g_clipboard_count           (g_repl_state.clipboard.cmd_count)
#define g_sel_anchor                (g_repl_state.selection.anchor_idx)
#define g_sel_end                   (g_repl_state.selection.end_idx)
#define g_anim_time                 (g_repl_state.variables.anim_time)
#define g_t_playing                 (g_repl_state.variables.time_playing)
#define g_t_var_idx                 (g_repl_state.variables.time_var_idx)
#define g_panel_frac                (g_repl_state.code_panel.panel_frac)
#define g_resizing_panel            (g_repl_state.code_panel.resizing_panel)
#define g_scroll                    (g_repl_state.code_panel.scroll)
#define g_scroll_follow_cursor      (g_repl_state.code_panel.scroll_follow_cursor)
#define g_cursor_on                 (g_repl_state.code_panel.cursor_visible)
#define g_blink_tick                (g_repl_state.code_panel.blink_tick)
#define g_show_help                 (g_repl_state.help.visible)
#define g_help_tab                  (g_repl_state.help.tab_idx)
#define g_help_scroll               (g_repl_state.help.scroll)
#define g_show_var_panel            (g_repl_state.variable_panel.visible)
#define g_drag_var                  (g_repl_state.variable_drag.var_idx)
#define g_drag_log_mode             (g_repl_state.variable_drag.log_mode)
#define g_drag_start_val            (g_repl_state.variable_drag.start_value)
#define g_drag_start_x              (g_repl_state.variable_drag.start_x)
#define g_show_profile_panel        (g_repl_state.profile_panel.mode)
#define g_cam_rx                    (g_repl_state.camera.rx)
#define g_cam_ry                    (g_repl_state.camera.ry)
#define g_cam_dist                  (g_repl_state.camera.dist)
#define g_cam_tx                    (g_repl_state.camera.tx)
#define g_cam_ty                    (g_repl_state.camera.ty)
#define g_cam_tz                    (g_repl_state.camera.tz)
#define g_cam_motion_glow           (g_repl_state.camera.motion_glow)
#define g_mouse_x                   (g_repl_state.pointer.mouse_x)
#define g_mouse_y                   (g_repl_state.pointer.mouse_y)
#define g_mouse_btn                 (g_repl_state.pointer.mouse_button)
#define g_win_w                     (g_repl_state.viewport.window_w)
#define g_win_h                     (g_repl_state.viewport.window_h)
#define g_wireframe                 (g_repl_state.presentation.wireframe)
#define g_grid_theme                (g_repl_state.presentation.grid_theme)
#define g_grid_major_idx            (g_repl_state.presentation.grid_major_idx)
#define g_grid_extent_idx           (g_repl_state.presentation.grid_extent_idx)
#define g_focus_vtx                 (g_repl_state.presentation.focus_vertex)
#define g_focus_vtx_valid           (g_repl_state.presentation.focus_vertex_valid)
#define g_axes_theme                (g_repl_state.presentation.axes_theme)
#define g_show_vnums                (g_repl_state.presentation.show_vertex_labels)
#define g_show_normals              (g_repl_state.presentation.show_normal_vectors)
#define g_show_indices              (g_repl_state.presentation.show_vertex_indices)
#define g_wrap_at_comma             (g_repl_state.presentation.wrap_at_comma)
#define g_code_panel_layout         (g_repl_state.presentation.code_panel_layout)
#define g_show_guides               (g_repl_state.presentation.show_vertex_guides)
#define g_xform_guide_mode          (g_repl_state.presentation.xform_guide_mode)
#define g_autonormal                (g_repl_state.presentation.autonormal)
#define g_show_lights               (g_repl_state.presentation.show_light_indicators)
#define g_backdrop_mode             (g_repl_state.presentation.backdrop_mode)
#define g_cam_rotate                (g_repl_state.camera.auto_rotate)
#define g_show_outlines             (g_repl_state.presentation.show_vertex_outlines)
#define g_show_vpoints              (g_repl_state.presentation.show_vertex_points)
#define g_highlight_current_poly    (g_repl_state.presentation.highlight_current_poly)
#define g_ortho_mode                (g_repl_state.presentation.ortho_mode)
#define g_cursor_px                 (g_repl_state.code_panel.cursor_px)
#define g_cursor_py                 (g_repl_state.code_panel.cursor_py)
#define g_use_accum                 (g_repl_state.render.use_accum)
#define g_accum_aa_enabled          (g_repl_state.render.accum_aa_enabled)
#define g_accum_samples             (g_repl_state.render.accum_samples)
#define g_accum_jitter_x            (g_repl_state.render.accum_jitter_x)
#define g_accum_jitter_y            (g_repl_state.render.accum_jitter_y)
#define g_multisample_enabled       (g_repl_state.render.multisample_enabled)
#define g_line_smooth_enabled       (g_repl_state.render.line_smooth_enabled)
#define g_init_attenuate_points     (g_repl_state.render.point_attenuation_enabled)
#define g_lights                    (g_repl_state.render.lights)
#define g_clear_color               (g_repl_state.render.clear_color)
#define g_status                    (g_repl_state.status.text)
#define g_status_ttl                (g_repl_state.status.ttl)
#define g_search_active             (g_repl_state.search.active)
#define g_search_query              (g_repl_state.search.query)
#define g_search_query_len          (g_repl_state.search.query_len)
#define g_search_cursor_pos         (g_repl_state.search.cursor_pos)
#define g_search_hit_line           (g_repl_state.search.hit_line_idx)
#define g_search_hit_char           (g_repl_state.search.hit_char_idx)
#define g_search_hit_ordinal        (g_repl_state.search.hit_ordinal)
#define g_search_match_count        (g_repl_state.search.match_count)
#define g_ac_matches                (g_repl_state.autocomplete.matches)
#define g_ac_insert_matches         (g_repl_state.autocomplete.insert_matches)
#define g_ac_count                  (g_repl_state.autocomplete.match_count)
#define g_ac_sel                    (g_repl_state.autocomplete.selected_idx)
#define g_ac_ghost                  (g_repl_state.autocomplete.ghost)
#define g_ac_hint                   (g_repl_state.autocomplete.hint)
#define g_replay_active             (g_repl_state.replay.active)
#define g_replay_state              (g_repl_state.replay.state)
#define g_replay_pc                 (g_repl_state.replay.pc)
#define g_replay_mode               (g_repl_state.replay.mode)
#define g_replay_speed              (g_repl_state.replay.speed)
#define g_replay_accum              (g_repl_state.replay.accum)
#define g_replay_fade_speed         (g_repl_state.replay.fade_speed)
#define g_replay_src_line           (g_repl_state.replay.src_line_idx)
#define g_replay_total_flat         (g_repl_state.replay.total_flat_cmds)
#define g_replay_expand_args        (g_repl_state.replay.expand_args)
#define g_example_idx               (g_repl_state.scenes.active_example_idx)
#define g_workspace_dir             (g_repl_state.scenes.workspace_dir)
#define g_workspace_header_lines    (g_repl_state.import_export.workspace_header_lines)
#define g_workspace_header_line_count (g_repl_state.import_export.workspace_header_line_count)
#define g_render_state_lines        (g_repl_state.import_export.render_state_lines)
#define g_cam_lines                 (g_repl_state.import_export.cam_lines)
#define g_export_scene_name_hint    (g_repl_state.import_export.export_scene_name_hint)
#define g_pending_scene_name        (g_repl_state.import_export.pending_scene_name)
#define g_pending_workspace_dir     (g_repl_state.import_export.pending_workspace_dir)

ExprVar *repl_state_predef_vars_storage(int *capacity, int **count_ptr) {
    if (capacity)
        *capacity = MAX_PREDEF_VARS;
    if (count_ptr)
        *count_ptr = &g_repl_state.variables.predef_var_count;
    return g_repl_state.variables.predef_vars;
}

static void repl_state_bind_eval_predef_storage(void) {
    repl_eval_bind_predef_storage(g_repl_state.variables.predef_vars,
                                  &g_repl_state.variables.predef_var_count);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
static void repl_state_bind_eval_predef_storage_at_load(void) {
    repl_state_bind_eval_predef_storage();
}
#endif

static void ensure_t_var_idx(void) {
    if (g_t_var_idx >= 0 && g_t_var_idx < g_num_predef_vars &&
        strcmp(g_predef_vars[g_t_var_idx].name, "t") == 0)
        return;
    g_t_var_idx = repl_eval_find_predef_var_idx("t");
}

static void reset_time_state(void) {
    g_anim_time = 0.0f;
    repl_eval_init_predef_vars();
    ensure_t_var_idx();
}

ReplDocumentState *repl_state_document_mut(void) {
    return &g_repl_state.document;
}

const GLCmd *repl_state_document_cmds(void) {
    return g_repl_state.document.cmds;
}

GLCmd *repl_state_document_cmds_mut(void) {
    return g_repl_state.document.cmds;
}

const GLCmd *repl_state_document_cmd_at(int cmd_idx) {
    if (cmd_idx < 0 || cmd_idx >= g_num_cmds)
        return NULL;
    return &g_cmds[cmd_idx];
}

GLCmd *repl_state_document_cmd_at_mut(int cmd_idx) {
    if (cmd_idx < 0 || cmd_idx >= g_num_cmds)
        return NULL;
    return &g_cmds[cmd_idx];
}

int repl_state_document_count(void) {
    return g_num_cmds;
}

void repl_state_document_count_set(int cmd_count) {
    g_num_cmds = cmd_count;
}

int repl_state_document_capacity(void) {
    return MAX_COMMANDS;
}

int repl_state_edit_line(void) {
    return g_edit_line;
}

void repl_state_edit_line_set(int edit_line_idx) {
    g_edit_line = edit_line_idx;
    repl_state_edit_line_clamp();
}

void repl_state_edit_line_clamp(void) {
    if (g_edit_line < 0)
        g_edit_line = 0;
    if (g_edit_line > g_num_cmds)
        g_edit_line = g_num_cmds;
}

int repl_state_normals_dirty(void) {
    return g_normals_dirty;
}

void repl_state_normals_dirty_clear(void) {
    g_normals_dirty = 0;
}

void repl_state_document_reset(void) {
    ReplCommandStore store = repl_command_store_live();
    repl_command_store_load(&store, NULL, 0, 0);
}

ReplFlatProgramState *repl_state_flat_program_mut(void) {
    return &g_repl_state.flat_program;
}

const GLCmd *repl_state_flat_program_cmds(void) {
    return g_repl_state.flat_program.cmds;
}

GLCmd *repl_state_flat_program_cmds_mut(void) {
    return g_repl_state.flat_program.cmds;
}

FlatCmdLocalVars *repl_state_flat_program_local_vars_mut(void) {
    return g_repl_state.flat_program.local_vars;
}

int repl_state_flat_program_count(void) {
    return g_num_flat_cmds;
}

void repl_state_flat_program_set_count(int cmd_count) {
    if (cmd_count < 0)
        cmd_count = 0;
    if (cmd_count > MAX_COMMANDS)
        cmd_count = MAX_COMMANDS;
    g_num_flat_cmds = cmd_count;
}

int repl_state_flat_program_dirty(void) {
    return g_flat_dirty;
}

void repl_state_flat_program_clear_dirty(void) {
    g_flat_dirty = 0;
}

int repl_state_flat_program_user_lighting_enabled(void) {
    return g_user_lighting_enabled;
}

int repl_state_flat_program_current_block_begin(void) {
    return g_current_block_begin;
}

int repl_state_flat_program_current_block_end(void) {
    return g_current_block_end;
}

int repl_state_flat_program_current_block_source_line(void) {
    return g_current_block_line;
}

void repl_state_flat_program_set_user_lighting_enabled(int enabled) {
    g_user_lighting_enabled = enabled ? 1 : 0;
}

void repl_state_flat_program_set_current_block(int begin_idx, int end_idx,
                                               int source_line_idx) {
    g_current_block_begin = begin_idx;
    g_current_block_end = end_idx;
    g_current_block_line = source_line_idx;
}

void repl_state_flat_program_clear_current_block(void) {
    repl_state_flat_program_set_current_block(-1, -1, -1);
}

void repl_state_flat_program_reset(void) {
    g_num_flat_cmds = 0;
    g_flat_dirty = 1;
    g_user_lighting_enabled = 0;
    repl_state_flat_program_clear_current_block();
}

void repl_state_mark_flat_dirty(void) {
    g_flat_dirty = 1;
}

void repl_state_mark_normals_dirty(void) {
    g_normals_dirty = 1;
    g_flat_dirty = 1;
    repl_source_scope_depth_cache_invalidate();
}

FlatProgramView repl_state_flat_program_view(void) {
    FlatProgramView view = {
        .cmds = g_repl_state.flat_program.cmds,
        .local_vars = g_repl_state.flat_program.local_vars,
        .cmd_count = g_repl_state.flat_program.cmd_count,
    };
    return view;
}

ReplVariableView repl_state_variables(void) {
    return (ReplVariableView){
        .vars = g_predef_vars,
        .var_count = g_num_predef_vars,
        .time_var_idx = g_t_var_idx,
        .time_playing = g_t_playing,
        .anim_time = g_anim_time,
    };
}

ReplVariableState *repl_state_variables_mut(void) {
    return &g_repl_state.variables;
}

void repl_state_variables_reset(void) {
    repl_state_bind_eval_predef_storage();
    g_repl_state.variables = g_repl_state_defaults.variables;
    repl_eval_init_predef_vars();
    g_t_var_idx = repl_eval_find_predef_var_idx("t");
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

ReplEditorInputView repl_state_editor_input(void) {
    return (ReplEditorInputView){
        .input = g_input,
        .input_capacity = MAX_INPUT_LEN,
        .input_len = g_input_len,
        .cursor_pos = g_cursor_pos,
        .edit_line_idx = g_edit_line,
        .pending_newline = g_newline_buf,
        .pending_newline_capacity = MAX_INPUT_LEN,
        .pending_newline_len = g_newline_len,
        .insert_mode = g_inserting,
    };
}

ReplEditorInputState *repl_state_editor_input_mut(void) {
    return &g_repl_state.editor_input;
}

/* Editor-owns-text spike: per-line text buffer accessors. */
const ReplEditorBuffer *repl_state_editor_buffer(void) {
    return &g_repl_state.editor_buffer;
}

ReplEditorBuffer *repl_state_editor_buffer_mut(void) {
    return &g_repl_state.editor_buffer;
}

const char *repl_state_editor_buffer_line(int idx) {
    if (idx < 0 || idx >= MAX_COMMANDS)
        return "";
    return g_repl_state.editor_buffer.lines[idx];
}

void repl_state_editor_buffer_set_line(int idx, const char *text) {
    if (idx < 0 || idx >= MAX_COMMANDS)
        return;
    if (!text) text = "";
    char *dst = g_repl_state.editor_buffer.lines[idx];
    int n = (int)strlen(text);
    if (n >= MAX_LINE_LEN) n = MAX_LINE_LEN - 1;
    memcpy(dst, text, (size_t)n);
    dst[n] = '\0';
}

void repl_state_editor_buffer_set_count(int count) {
    if (count < 0) count = 0;
    if (count > MAX_COMMANDS) count = MAX_COMMANDS;
    g_repl_state.editor_buffer.line_count = count;
}

int repl_state_editor_buffer_count(void) {
    return g_repl_state.editor_buffer.line_count;
}

const EditorTransformerList *repl_state_editor_transformers(void) {
    return &g_repl_state.editor_transformers;
}

void repl_state_editor_transformers_clear(void) {
    g_repl_state.editor_transformers.count = 0;
}

int repl_state_editor_transformers_append(const EditorTransformer *transformer) {
    EditorTransformerList *list = &g_repl_state.editor_transformers;
    if (!transformer || list->count >= MAX_TRANSFORMERS)
        return 0;
    list->items[list->count++] = *transformer;
    return 1;
}

void repl_state_editor_input_reset(void) {
    repl_state_input_clear();
    repl_state_pending_newline_clear();
    g_inserting = 0;
}

const char *repl_state_input_text(void) {
    return g_input;
}

char *repl_state_input_buffer_mut(void) {
    return g_input;
}

int repl_state_input_len(void) {
    return g_input_len;
}

void repl_state_input_len_set(int input_len) {
    if (input_len < 0)
        input_len = 0;
    if (input_len >= MAX_INPUT_LEN)
        input_len = MAX_INPUT_LEN - 1;
    g_input_len = input_len;
    g_input[g_input_len] = '\0';
    repl_state_cursor_pos_set(g_cursor_pos);
}

void repl_state_input_set_text(const char *text) {
    repl_copy_string_fits(g_input, MAX_INPUT_LEN, text ? text : "");
    repl_state_input_len_set((int)strlen(g_input));
    repl_state_cursor_pos_set(g_input_len);
}

void repl_state_input_clear(void) {
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
}

int repl_state_cursor_pos(void) {
    return g_cursor_pos;
}

void repl_state_cursor_pos_set(int cursor_pos) {
    if (cursor_pos < 0)
        cursor_pos = 0;
    if (cursor_pos > g_input_len)
        cursor_pos = g_input_len;
    g_cursor_pos = cursor_pos;
}

int repl_state_insert_mode(void) {
    return g_inserting;
}

void repl_state_insert_mode_set(int insert_mode) {
    g_inserting = insert_mode ? 1 : 0;
}

char *repl_state_pending_newline_buffer_mut(void) {
    return g_newline_buf;
}

int repl_state_pending_newline_len(void) {
    return g_newline_len;
}

void repl_state_pending_newline_len_set(int newline_len) {
    if (newline_len < 0)
        newline_len = 0;
    if (newline_len >= MAX_INPUT_LEN)
        newline_len = MAX_INPUT_LEN - 1;
    g_newline_len = newline_len;
    g_newline_buf[g_newline_len] = '\0';
}

void repl_state_pending_newline_set_text(const char *text) {
    repl_copy_string_fits(g_newline_buf, MAX_INPUT_LEN, text ? text : "");
    repl_state_pending_newline_len_set((int)strlen(g_newline_buf));
}

void repl_state_pending_newline_clear(void) {
    g_newline_buf[0] = '\0';
    g_newline_len = 0;
}

ReplSelectionState repl_state_selection(void) {
    return g_repl_state.selection;
}

ReplSelectionState *repl_state_selection_mut(void) {
    return &g_repl_state.selection;
}

void repl_state_selection_clear(void) {
    g_sel_anchor = -1;
    g_sel_end = -1;
}

int repl_state_selection_anchor(void) {
    return g_sel_anchor;
}

int repl_state_selection_end_idx(void) {
    return g_sel_end;
}

void repl_state_selection_set(int anchor_idx, int end_idx) {
    g_sel_anchor = anchor_idx;
    g_sel_end = end_idx;
}

ReplClipboardState repl_state_clipboard(void) {
    return g_repl_state.clipboard;
}

ReplClipboardState *repl_state_clipboard_mut(void) {
    return &g_repl_state.clipboard;
}

void repl_state_clipboard_clear(void) {
    g_clipboard_count = 0;
}

GLCmd *repl_state_clipboard_cmds_mut(void) {
    return g_clipboard;
}

int repl_state_clipboard_count(void) {
    return g_clipboard_count;
}

void repl_state_clipboard_count_set(int cmd_count) {
    if (cmd_count < 0)
        cmd_count = 0;
    if (cmd_count > MAX_COMMANDS)
        cmd_count = MAX_COMMANDS;
    g_clipboard_count = cmd_count;
}

ReplCodePanelRuntimeState repl_state_code_panel(void) {
    return g_repl_state.code_panel;
}

ReplCodePanelRuntimeState *repl_state_code_panel_mut(void) {
    return &g_repl_state.code_panel;
}

void repl_state_code_panel_reset(void) {
    g_repl_state.code_panel = g_repl_state_defaults.code_panel;
}

ReplHelpState repl_state_help(void) {
    return g_repl_state.help;
}

ReplHelpState *repl_state_help_mut(void) {
    return &g_repl_state.help;
}

void repl_state_help_reset(void) {
    g_repl_state.help = g_repl_state_defaults.help;
}

ReplVariablePanelState repl_state_variable_panel(void) {
    return g_repl_state.variable_panel;
}

ReplVariablePanelState *repl_state_variable_panel_mut(void) {
    return &g_repl_state.variable_panel;
}

ReplVariableDragState repl_state_variable_drag(void) {
    return g_repl_state.variable_drag;
}

ReplVariableDragState *repl_state_variable_drag_mut(void) {
    return &g_repl_state.variable_drag;
}

void repl_state_variable_drag_reset(void) {
    g_repl_state.variable_drag = g_repl_state_defaults.variable_drag;
}

ReplProfilePanelState repl_state_profile_panel(void) {
    return g_repl_state.profile_panel;
}

ReplProfilePanelState *repl_state_profile_panel_mut(void) {
    return &g_repl_state.profile_panel;
}

ReplStatusState repl_state_status(void) {
    return g_repl_state.status;
}

ReplStatusState *repl_state_status_mut(void) {
    return &g_repl_state.status;
}

void repl_state_status_set(const char *message) {
    ReplStatusState *status = repl_state_status_mut();
    if (!message)
        message = "";
    strncpy(status->text, message, sizeof(status->text) - 1);
    status->text[sizeof(status->text) - 1] = '\0';
    status->ttl = 240;
}

void repl_state_status_clear(void) {
    ReplStatusState *status = repl_state_status_mut();
    status->text[0] = '\0';
    status->ttl = 0;
}

void repl_state_status_tick(void) {
    ReplStatusState *status = repl_state_status_mut();
    if (status->ttl > 0)
        status->ttl--;
}

ReplSearchState repl_state_search(void) {
    return g_repl_state.search;
}

ReplSearchState *repl_state_search_mut(void) {
    return &g_repl_state.search;
}

void repl_state_search_clear(void) {
    g_repl_state.search = g_repl_state_defaults.search;
}

ReplAutocompleteState repl_state_autocomplete(void) {
    return g_repl_state.autocomplete;
}

ReplAutocompleteState *repl_state_autocomplete_mut(void) {
    return &g_repl_state.autocomplete;
}

void repl_state_autocomplete_clear(void) {
    g_repl_state.autocomplete = g_repl_state_defaults.autocomplete;
}

ReplCameraState repl_state_camera(void) {
    return g_repl_state.camera;
}

ReplCameraState *repl_state_camera_mut(void) {
    return &g_repl_state.camera;
}

ReplCameraState repl_state_camera_snapshot(void) {
    return g_repl_state.camera;
}

void repl_state_camera_set(float rx, float ry, float dist,
                           float tx, float ty, float tz,
                           float motion_glow) {
    g_cam_rx = rx;
    g_cam_ry = ry;
    g_cam_dist = dist;
    g_cam_tx = tx;
    g_cam_ty = ty;
    g_cam_tz = tz;
    g_cam_motion_glow = motion_glow;
}

void repl_state_camera_set_orbit(float rx, float ry) {
    g_cam_rx = rx;
    g_cam_ry = ry;
}

void repl_state_camera_set_pan(float tx, float ty, float tz) {
    g_cam_tx = tx;
    g_cam_ty = ty;
    g_cam_tz = tz;
}

void repl_state_camera_set_distance(float dist) {
    g_cam_dist = dist;
}

void repl_state_camera_set_motion_glow(float motion_glow) {
    g_cam_motion_glow = motion_glow;
}

void repl_state_camera_reset_default(void) {
    g_repl_state.camera = g_repl_state_defaults.camera;
}

ReplPointerState repl_state_pointer(void) {
    return g_repl_state.pointer;
}

ReplPointerState *repl_state_pointer_mut(void) {
    return &g_repl_state.pointer;
}

void repl_state_pointer_set(int mouse_x, int mouse_y, int mouse_button) {
    g_mouse_x = mouse_x;
    g_mouse_y = mouse_y;
    g_mouse_btn = mouse_button;
}

void repl_state_pointer_set_pos(int mouse_x, int mouse_y) {
    g_mouse_x = mouse_x;
    g_mouse_y = mouse_y;
}

void repl_state_pointer_set_button(int mouse_button) {
    g_mouse_btn = mouse_button;
}

ReplViewportState repl_state_viewport(void) {
    return g_repl_state.viewport;
}

ReplViewportState *repl_state_viewport_mut(void) {
    return &g_repl_state.viewport;
}

void repl_state_viewport_set_size(int window_w, int window_h) {
    g_win_w = window_w;
    g_win_h = window_h;
}

ReplPresentationState repl_state_presentation(void) {
    return g_repl_state.presentation;
}

ReplPresentationState *repl_state_presentation_mut(void) {
    return &g_repl_state.presentation;
}

const float *repl_state_grid_major_steps(void) {
    return g_grid_major_steps;
}

const float *repl_state_grid_extents(void) {
    return g_grid_extents;
}


void repl_state_presentation_reset_defaults(void) {
    g_repl_state.presentation = g_repl_state_defaults.presentation;
    g_cam_rotate = g_repl_state_defaults.camera.auto_rotate;
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

ReplRenderState repl_state_render(void) {
    return g_repl_state.render;
}

ReplRenderState *repl_state_render_mut(void) {
    return &g_repl_state.render;
}

void repl_state_render_reset_defaults(void) {
    g_repl_state.render = g_repl_state_defaults.render;
}

ReplReplayRuntimeState repl_state_replay(void) {
    return g_repl_state.replay;
}

ReplReplayRuntimeState *repl_state_replay_mut(void) {
    return &g_repl_state.replay;
}

void repl_state_replay_reset(void) {
    g_repl_state.replay = g_repl_state_defaults.replay;
}

ReplSceneRuntimeState repl_state_scenes(void) {
    return g_repl_state.scenes;
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

ReplImportExportView repl_state_import_export(void) {
    return (ReplImportExportView){
        .workspace_header_lines = g_repl_state.import_export.workspace_header_lines,
        .workspace_header_line_count = g_repl_state.import_export.workspace_header_line_count,
        .render_state_lines = g_repl_state.import_export.render_state_lines,
        .cam_lines = g_repl_state.import_export.cam_lines,
        .export_scene_name_hint = g_repl_state.import_export.export_scene_name_hint,
        .pending_scene_name = g_repl_state.import_export.pending_scene_name,
        .pending_workspace_dir = g_repl_state.import_export.pending_workspace_dir,
    };
}

ReplImportExportState *repl_state_import_export_mut(void) {
    return &g_repl_state.import_export;
}

void repl_state_capture(ReplRuntimeState *snapshot) {
    if (!snapshot)
        return;

    repl_state_bind_eval_predef_storage();
    refresh_workspace_header_lines();
    update_render_state_strings();
    update_cam_lines();
    *snapshot = g_repl_state;
}

void repl_state_restore(const ReplRuntimeState *snapshot) {
    if (!snapshot)
        return;

    g_repl_state = *snapshot;
    repl_state_bind_eval_predef_storage();
    ensure_t_var_idx();
    repl_source_scope_depth_cache_invalidate();
}

void repl_state_import_export_reset(void) {
    g_repl_state.import_export = g_repl_state_defaults.import_export;
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
    g_repl_state = g_repl_state_defaults;
    repl_state_bind_eval_predef_storage();
    repl_scenes_reset();
    reset_time_state();
    repl_editor_reset_transients();
    refresh_workspace_header_lines();
    update_render_state_strings();
    update_cam_lines();
    repl_source_scope_depth_cache_invalidate();
    repl_state_mark_flat_dirty();
    repl_state_mark_normals_dirty();
}
