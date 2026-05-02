#define REPL_STATE_IMPLEMENTATION
#include "repl_state.h"

#include "editor_state.h"
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

/* Forward decls for ui_state_* entry points referenced from this
 * translation unit. check-controller-boundaries forbids repl_*.c from
 * including ui_state.h, so the symbols are declared here directly.
 *
 * Phase 1 commit 8 routes legacy `repl_state_*` slice accessors
 * (status / help / variable_panel / profile_panel / viewport /
 * pointer) through one-line forwarders defined below; those
 * forwarders call into the ui_state_* canonical API. Defining the
 * forwarders here (not in ui_state.c) keeps `check-state-boundaries`
 * happy: the guard forbids ui_*.c from calling `repl_state_*_mut()`,
 * so the forwarders that *do* call those names live on the repl_state
 * side instead. */
void                    ui_state_reset(void);
ReplStatusState         ui_state_status(void);
ReplStatusState        *ui_state_status_mut(void);
void                    ui_state_status_set(const char *message);
void                    ui_state_status_clear(void);
void                    ui_state_status_tick(void);
ReplHelpState           ui_state_help(void);
ReplHelpState          *ui_state_help_mut(void);
void                    ui_state_help_reset(void);
ReplVariablePanelState  ui_state_variable_panel(void);
ReplVariablePanelState *ui_state_variable_panel_mut(void);
ReplProfilePanelState   ui_state_profile_panel(void);
ReplProfilePanelState  *ui_state_profile_panel_mut(void);
ReplViewportState       ui_state_viewport(void);
ReplViewportState      *ui_state_viewport_mut(void);
void                    ui_state_viewport_set_size(int window_w, int window_h);
ReplPointerState        ui_state_pointer(void);
ReplPointerState       *ui_state_pointer_mut(void);
void                    ui_state_pointer_set(int mouse_x, int mouse_y, int mouse_button);
void                    ui_state_pointer_set_pos(int mouse_x, int mouse_y);
void                    ui_state_pointer_set_button(int mouse_button);

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
/* g_input / g_cursor_pos / g_newline_buf / g_newline_len / g_inserting
 * macros removed (Phase 1 commit 5). The editor_input slice now lives
 * on g_editor_state.input in editor_state.c, where the dependent
 * convenience getters (editor_input_text, _cursor_pos,
 * _insert_mode, _pending_newline_*, etc.) are also implemented. */
/* g_clipboard / g_clipboard_count / g_sel_anchor / g_sel_end macros
 * removed (Phase 1 commit 6). The selection + clipboard slices live
 * on g_editor_state.{selection,clipboard} in editor_state.c. */
#define g_anim_time                 (g_repl_state.variables.anim_time)
#define g_t_playing                 (g_repl_state.variables.time_playing)
#define g_t_var_idx                 (g_repl_state.variables.time_var_idx)
#define g_panel_frac                (g_repl_state.code_panel.panel_frac)
#define g_resizing_panel            (g_repl_state.code_panel.resizing_panel)
/* g_scroll / g_scroll_follow_cursor macros removed (Phase 1 commit 11);
 * scroll lives on g_editor_state.scroll in editor_state.c. */
#define g_cursor_on                 (g_repl_state.code_panel.cursor_visible)
#define g_blink_tick                (g_repl_state.code_panel.blink_tick)
/* g_show_help / g_help_tab / g_help_scroll / g_show_var_panel macros
 * removed (Phase 1 commit 8); the help and variable_panel slices live
 * on g_ui_state.{help,variable_panel} in ui_state.c. */
/* g_drag_* macros removed (Phase 1 commit 9); variable_drag lives on
 * g_editor_state.variable_drag in editor_state.c. */
/* g_show_profile_panel macro removed (Phase 1 commit 8); profile_panel
 * lives on g_ui_state.profile_panel in ui_state.c. */
#define g_cam_rx                    (g_repl_state.camera.rx)
#define g_cam_ry                    (g_repl_state.camera.ry)
#define g_cam_dist                  (g_repl_state.camera.dist)
#define g_cam_tx                    (g_repl_state.camera.tx)
#define g_cam_ty                    (g_repl_state.camera.ty)
#define g_cam_tz                    (g_repl_state.camera.tz)
#define g_cam_motion_glow           (g_repl_state.camera.motion_glow)
/* g_mouse_* and g_win_* macros removed (Phase 1 commit 8); pointer and
 * viewport slices live on g_ui_state.{pointer,viewport} in ui_state.c. */
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
/* g_status / g_status_ttl macros removed (Phase 1 commit 8); status
 * lives on g_ui_state.status in ui_state.c. */
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

/* Editor-input + editor-buffer accessors moved to editor_state.c
 * (Phase 1 commits 4-5). Use editor_state_input / _mut / _reset for
 * the input slice, editor_state_buffer / _mut for the whole-buffer
 * struct, and editor_buffer_* for slice-level line text. */

/* Editor overlay snapshot list accessors (transformers / highlights /
 * virtual_lines) moved to editor_state.c (Phase 1 commit 9). Use
 * editor_state_transformers / _highlights / _virtual_lines. */

/* editor_state_input_reset and the editor_input convenience getters
 * (input_text / input_len / cursor_pos / insert_mode / pending_newline_*)
 * moved to editor_state.c (Phase 1 commit 5). The editor_state_input
 * struct accessor and the new editor_state_input_reset entry point
 * live there too. */

/* Selection + clipboard accessors moved to editor_state.c
 * (Phase 1 commit 6). Use editor_state_selection / _clipboard. */

ReplCodePanelRuntimeState repl_state_code_panel(void) {
    return g_repl_state.code_panel;
}

ReplCodePanelRuntimeState *repl_state_code_panel_mut(void) {
    return &g_repl_state.code_panel;
}

void repl_state_code_panel_reset(void) {
    g_repl_state.code_panel = g_repl_state_defaults.code_panel;
}

/* Help / variable_panel / profile_panel / status / pointer / viewport
 * accessors moved to ui_state.c (Phase 1 commit 8). The legacy
 * repl_state_* names remain alive as one-line forwarders defined
 * there, so existing callers link without including ui_state.h. */

/* editor_state_variable_drag accessors moved to editor_state.c
 * (Phase 1 commit 9). Use editor_state_variable_drag / _mut / _reset. */

/* Search + autocomplete accessors moved to editor_state.c (Phase 1
 * commit 7). Use editor_state_search / _autocomplete.
 * Status / help / variable_panel / profile_panel accessors moved to
 * ui_state.c (Phase 1 commit 8); legacy repl_state_* names are
 * forwarders defined there. */

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

/* Pointer + viewport accessors moved to ui_state.c (Phase 1 commit 8);
 * legacy repl_state_* names are forwarders defined there. */

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
    /* Phase 1 scaffold (commit 3): drain the new EditorState and UiState
     * singletons too so every test reset clears all three structs. The
     * structs are placeholders until commits 4-7 move slices in. */
    editor_state_reset();
    ui_state_reset();
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

/* EDITOR_OWNERSHIP_TODO(phase-4): delete this entire forwarder block
 * once `UiAction` dispatch eliminates the direct-mutation call sites
 * that need the legacy `repl_state_*` names. Each new forwarder added
 * here grows the budget tracked by
 * scripts/check-editor-ownership-budget.sh; the ratchet only allows
 * the count to decrease. New UI slice migrations should NOT add to
 * this block — instead they should let callers migrate to the
 * canonical `ui_state_*` API directly (extending the controller-
 * boundaries allowlist where required).
 *
 * Phase 1 commit 8: legacy `repl_state_*` UI-slice accessors as
 * forwarders into ui_state.c. They keep callers in non-allowlisted
 * repl_*.c files linkable without forcing them to include ui_state.h. */

ReplStatusState repl_state_status(void) {
    return ui_state_status();
}

ReplStatusState *repl_state_status_mut(void) {
    return ui_state_status_mut();
}

void repl_state_status_set(const char *message) {
    ui_state_status_set(message);
}

void repl_state_status_clear(void) {
    ui_state_status_clear();
}

void repl_state_status_tick(void) {
    ui_state_status_tick();
}

ReplHelpState repl_state_help(void) {
    return ui_state_help();
}

ReplHelpState *repl_state_help_mut(void) {
    return ui_state_help_mut();
}

void repl_state_help_reset(void) {
    ui_state_help_reset();
}

ReplVariablePanelState repl_state_variable_panel(void) {
    return ui_state_variable_panel();
}

ReplVariablePanelState *repl_state_variable_panel_mut(void) {
    return ui_state_variable_panel_mut();
}

ReplProfilePanelState repl_state_profile_panel(void) {
    return ui_state_profile_panel();
}

ReplProfilePanelState *repl_state_profile_panel_mut(void) {
    return ui_state_profile_panel_mut();
}

ReplViewportState repl_state_viewport(void) {
    return ui_state_viewport();
}

ReplViewportState *repl_state_viewport_mut(void) {
    return ui_state_viewport_mut();
}

void repl_state_viewport_set_size(int window_w, int window_h) {
    ui_state_viewport_set_size(window_w, window_h);
}

ReplPointerState repl_state_pointer(void) {
    return ui_state_pointer();
}

ReplPointerState *repl_state_pointer_mut(void) {
    return ui_state_pointer_mut();
}

void repl_state_pointer_set(int mouse_x, int mouse_y, int mouse_button) {
    ui_state_pointer_set(mouse_x, mouse_y, mouse_button);
}

void repl_state_pointer_set_pos(int mouse_x, int mouse_y) {
    ui_state_pointer_set_pos(mouse_x, mouse_y);
}

void repl_state_pointer_set_button(int mouse_button) {
    ui_state_pointer_set_button(mouse_button);
}
