#include "imrepl_ctrl.h"

#include <gl_includes.h>

#include "editor_input.h"
#include "repl_core.h"
#include "repl_executor.h"
#include "repl_eval.h"
#include "repl_pipeline.h"
#include "repl_replay.h"
#include "repl_replay_annotations.h"
#include "repl_state.h"
#include "scene_render.h"
#include "ui_color_picker.h"
#include "ui_editor.h"
#include "ui_replay_hud.h"
#include "ui_autocomplete_panel.h"
#include "ui_help_overlay.h"
#include "ui_menu_bar.h"
#include "ui_panels.h"
#include "ui_snapshot.h"
#include "ui_state.h"
#include "repl_clipboard.h"
#include "repl_code_panel_document.h"
#include "repl_export.h"
#include "repl_layout.h"
#include "repl_source_scope.h"
#include "ui_profile_panel.h"
#include "ui_variable_panel.h"
#include "variable_panel.h"
#include "replay_state.h"
#include "prof.h"

static int imrepl_ctrl_cmd_is_focus_vertex(const GLCmd *cmd) {
    return cmd->valid &&
           (cmd->type == CMD_VERTEX3F || cmd->type == CMD_TESS_VERTEX);
}

static SceneFocusVertex imrepl_ctrl_build_focus_vertex(void) {
    SceneFocusVertex focus = { .valid = 0 };
    int edit_line = repl_state_edit_line();

    if (edit_line >= 0 && edit_line < repl_state_document_count() &&
        imrepl_ctrl_cmd_is_focus_vertex(&repl_state_document_cmds_mut()[edit_line])) {
        focus.pos[0] = repl_state_document_cmds_mut()[edit_line].args[0];
        focus.pos[1] = repl_state_document_cmds_mut()[edit_line].args[1];
        focus.pos[2] = repl_state_document_cmds_mut()[edit_line].args[2];
        focus.valid = 1;
    } else {
        for (int i = edit_line - 1; i >= 0; i--) {
            if (imrepl_ctrl_cmd_is_focus_vertex(&repl_state_document_cmds_mut()[i])) {
                focus.pos[0] = repl_state_document_cmds_mut()[i].args[0];
                focus.pos[1] = repl_state_document_cmds_mut()[i].args[1];
                focus.pos[2] = repl_state_document_cmds_mut()[i].args[2];
                focus.valid = 1;
                break;
            }
        }
    }

    return focus;
}

static SceneGuideSnapshot imrepl_ctrl_build_guide_snapshot(const SceneRenderConfig *config) {
    ReplPresentationState presentation = repl_state_presentation();
    ReplVariableView vars = repl_state_variables();
    ReplEditorInputView input = editor_state_input();
    ReplPredefView predef = repl_eval_predef_view();

    SceneGuideSnapshot snapshot = {
        .show_guides = config->show_guides,
        .replaying = config->replaying,
        .xform_guide_mode = presentation.xform_guide_mode,
        .user_lighting_enabled = config->user_lighting_enabled,
        .anim_time = vars.anim_time,
        .input = input.input,
        .input_len = input.input_len,
        .cursor_pos = input.cursor_pos,
        .edit_line_idx = config->edit_line_idx,
        .inserting = editor_insert_mode(),
        .edit_line_committed_text = editor_buffer_line(config->edit_line_idx),
        .source_cmds = repl_state_document_cmds_mut(),
        .source_cmd_count = repl_state_document_count(),
        .flat_program = config->flat_program,
        .predef_vars = predef.vars,
        .predef_var_count = predef.count,
        .alpha_scale = config->alpha_scale,
    };
    return snapshot;
}

static void imrepl_ctrl_build_replay_fade_plan(SceneRenderConfig *config) {
    ReplayFadeBatchView fade_batches;
    int batch_count;

    memset(&config->replay_fade_plan, 0, sizeof(config->replay_fade_plan));
    config->replay_has_fades = 0;
    config->replay_base_limit = 0;

    if (!config->replaying)
        return;

    repl_replay_copy_baseline_predef_values(config->replay_fade_plan.baseline_predef_vals,
                                            MAX_PREDEF_VARS);

    config->replay_has_fades = repl_replay_has_active_fades();
    if (!config->replay_has_fades)
        return;

    config->replay_base_limit = repl_replay_fill_base_limit();
    fade_batches = repl_replay_fade_batches_view();
    batch_count = repl_replay_compute_fade_skip_limits(config->replay_fade_plan.skip_limits,
                                                       REPLAY_FADE_BATCH_MAX);
    if (batch_count > REPLAY_FADE_BATCH_MAX)
        batch_count = REPLAY_FADE_BATCH_MAX;

    config->replay_fade_plan.batch_count = batch_count;
    for (int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
        const ReplayFadeBatch *batch = &fade_batches.batches[batch_idx];
        config->replay_fade_plan.batches[batch_idx] = *batch;
        config->replay_fade_plan.batch_alpha[batch_idx] = repl_replay_batch_alpha(batch);
    }
}

static void imrepl_ctrl_push_highlights(void) {
    editor_state_highlights_clear();

    int doc_count = repl_state_document_count();
    int edit_line = repl_state_edit_line();
    int insert_mode = editor_insert_mode();

    if (!insert_mode && edit_line >= 0 && edit_line < doc_count) {
        const GLCmd *cmd = repl_state_document_cmd_at(edit_line);
        if (cmd && cmd->valid) {
            int norm_idx = repl_find_feeding_normal_cmd(edit_line);
            int color_idx = repl_find_feeding_color_cmd(edit_line);
            if (norm_idx >= 0)
                editor_state_highlights_append(norm_idx, -1, -1,
                                                    HIGHLIGHT_FEEDING_NORMAL);
            if (color_idx >= 0)
                editor_state_highlights_append(color_idx, -1, -1,
                                                    HIGHLIGHT_FEEDING_COLOR);
        }
    }

    ReplReplayRuntimeState replay = replay_state_view();
    if (replay.active && replay.src_line_idx >= 0)
        editor_state_highlights_append(replay.src_line_idx, -1, -1,
                                            HIGHLIGHT_REPLAY_PC);
}

static void imrepl_ctrl_push_color_transformers(void) {
    editor_state_transformers_clear();
    int doc_count = repl_state_document_count();
    for (int i = 0; i < doc_count; i++) {
        if (!ui_color_picker_can_edit_cmd(i))
            continue;
        const GLCmd *cmd = repl_state_document_cmd_at(i);
        if (!cmd)
            continue;
        int has_alpha = (cmd->type == CMD_COLOR4F ||
                         cmd->type == CMD_TESS_COLOR ||
                         cmd->type == CMD_CLEAR_COLOR);
        EditorTransformer t = {
            .line_idx = i,
            .char_start = -1,
            .char_end = -1,
            .kind = TRANSFORMER_COLOR_PICKER,
            .state.color = {
                .r = cmd->args[0],
                .g = cmd->args[1],
                .b = cmd->args[2],
                .a = has_alpha ? cmd->args[3] : 1.0f,
                .has_alpha = has_alpha,
                .is_clear = (cmd->type == CMD_CLEAR_COLOR),
            },
        };
        if (!editor_state_transformers_append(&t))
            break;
    }
}

static void imrepl_ctrl_apply_input_effects(ReplInputDispatchEffects effects) {
    if (effects.set_cursor)
        glutSetCursor(effects.cursor);
    if (effects.request_redraw)
        glutPostRedisplay();
    if (effects.schedule_timer)
        glutTimerFunc(effects.timer_millis, imrepl_ctrl_timer, effects.timer_value);
}

/* ========================================================================= */
/* Scene config builder (push model)                                          */
/* ========================================================================= */

/* Execute callback adapter: forwards to repl_execute_program() */
static void scene_execute_adapter(float alpha_scale,
                                  int skip_geom_before_pc,
                                  int flat_cmd_count,
                                  FlatProgramView program,
                                  void *user_data) {
    (void)user_data;
    repl_execute_set_fade_context(alpha_scale, skip_geom_before_pc);
    repl_execute_program(&(ReplExecutionOptions){
        .flat_cmd_count = flat_cmd_count,
        .program = program,
        .text = editor_buffer_view(),
    });
}

/* Execute reset callback: clears fade context after last fade batch */
static void scene_execute_reset_adapter(void *user_data) {
    (void)user_data;
    repl_execute_set_fade_context(1.0f, 0);
}

static void imrepl_ctrl_build_scene_config(SceneRenderConfig *config) {
    ReplRenderState render = repl_state_render();
    ReplReplayRuntimeState replay = replay_state_view();
    ReplPresentationState presentation = repl_state_presentation();
    ReplCameraState cam = ui_state_camera();
    const float *grid_major_steps = repl_state_grid_major_steps();
    const float *grid_extents = repl_state_grid_extents();
    float bg_lum;
    float as_val;

    /* Refresh cursor block highlight before reading cursor state */
    repl_flatten_refresh_current_block_highlight();

    /* --- Execute hook --- */
    config->execute_fn = scene_execute_adapter;
    config->execute_user_data = NULL;
    config->execute_reset_fn = scene_execute_reset_adapter;

    /* --- Flat program --- */
    config->flat_program = repl_state_flat_program_view();

    /* --- Animation --- */
    config->anim_time = repl_state_variables().anim_time;

    /* --- Viewport and scene rectangle --- */
    config->viewport_w = ui_state_viewport().window_w;
    config->viewport_h = ui_state_viewport().window_h;
    repl_layout_scene_rect(&config->scene_x, &config->scene_y,
                           &config->scene_w, &config->scene_h);
    if (config->scene_w < 1) config->scene_w = 1;
    if (config->scene_h < 1) config->scene_h = 1;

    /* --- Camera --- */
    config->cam_dist = cam.dist;
    config->cam_rx = cam.rx;
    config->cam_ry = cam.ry;
    config->cam_tx = cam.tx;
    config->cam_ty = cam.ty;
    config->cam_tz = cam.tz;
    config->cam_motion_glow = cam.motion_glow;

    /* --- Rendering quality --- */
    config->multisample_enabled = render.multisample_enabled;
    config->line_smooth_enabled = render.line_smooth_enabled;
    config->use_accum = render.use_accum;
    config->accum_aa_enabled = render.accum_aa_enabled;
    config->accum_samples = render.accum_samples;

    /* --- Lighting --- */
    config->user_lighting_enabled = repl_state_flat_program_user_lighting_enabled();
    memcpy(config->lights, render.lights, sizeof(config->lights));
    config->show_light_indicators = presentation.show_light_indicators;

    /* --- Environment --- */
    config->backdrop_mode = presentation.backdrop_mode;
    config->wireframe = presentation.wireframe;

    /* --- Grid and axes --- */
    config->grid_theme = presentation.grid_theme;
    config->grid_extent_idx = presentation.grid_extent_idx;
    config->grid_major_idx = presentation.grid_major_idx;
    config->axes_theme = presentation.axes_theme;
    memcpy(config->grid_major_steps, grid_major_steps,
           sizeof(config->grid_major_steps));
    memcpy(config->grid_extents, grid_extents,
           sizeof(config->grid_extents));

    /* --- 3D overlay flags --- */
    config->show_guides = presentation.show_vertex_guides;
    config->show_vpoints = presentation.show_vertex_points;
    config->show_vnums = presentation.show_vertex_labels;
    config->show_normals = presentation.show_normal_vectors;
    config->show_vertex_outlines = presentation.show_vertex_outlines;
    config->replaying = replay.active;
    config->replay_mode = replay.mode;
    config->replay_tess_preview = config->replaying &&
                                  config->replay_mode == REPLAY_MODE_VERTEX;
    config->replay_vertex_points = config->replay_tess_preview;
    config->show_current_poly = presentation.highlight_current_poly && !config->replaying;

    /* --- Cursor / editor block overlay --- */
    config->cursor_block_begin_idx = repl_state_flat_program_current_block_begin();
    config->cursor_block_end_idx = repl_state_flat_program_current_block_end();
    config->cursor_block_source_line =
        repl_state_flat_program_current_block_source_line();
    config->edit_line_idx = repl_state_edit_line();
    config->cursor_func_scope_mask = 0;
    config->cursor_call_src_cmd_idx = -1;

    /* --- Focus and guide snapshots --- */
    config->focus = imrepl_ctrl_build_focus_vertex();

    /* Alpha scale boost for dark backgrounds */
        bg_lum = 0.2126f * render.clear_color[0]
            + 0.7152f * render.clear_color[1]
            + 0.0722f * render.clear_color[2];
    as_val = (0.10f + 0.02f) / fmaxf(bg_lum + 0.02f, 1e-4f);
    config->alpha_scale = as_val < 1.0f ? 1.0f : (as_val > 3.0f ? 3.0f : as_val);

    config->guide_snapshot = imrepl_ctrl_build_guide_snapshot(config);

    /* --- Replay --- */
    imrepl_ctrl_build_replay_fade_plan(config);
}

/* ========================================================================= */
/* UI snapshot builder (push model)                                           */
/* ========================================================================= */

static void imrepl_ctrl_build_ui_snapshot(UiRenderSnapshot *snap) {
    memset(snap, 0, sizeof(*snap));

    /* Refresh derived workspace header lines so the import/export view
     * reflects the current frame before rendering reads it. */
    repl_state_refresh_workspace_header_lines();

    snap->viewport       = ui_state_viewport();
    snap->presentation   = repl_state_presentation();
    snap->code_panel     = ui_state_code_panel();
    snap->help           = ui_state_help();
    snap->variable_panel = variable_panel_view();
    snap->profile_panel  = ui_state_profile_panel();
    snap->status         = ui_state_status();
    snap->search         = editor_state_search();
    snap->autocomplete   = editor_state_autocomplete();
    snap->camera         = ui_state_camera();
    snap->pointer        = ui_state_pointer();
    snap->render         = repl_state_render();
    snap->replay         = replay_state_view();
    snap->scenes         = repl_state_scenes();
    snap->variable_drag  = variable_panel_drag();
    snap->selection      = editor_state_selection();
    snap->scroll         = editor_state_scroll();

    snap->variables      = repl_state_variables();
    snap->editor_input   = editor_state_input();
    snap->import_export  = repl_state_import_export();
    snap->flat_program   = repl_state_flat_program_view();
    snap->predef         = repl_eval_predef_view();

    snap->document_cmds       = repl_state_document_cmds();
    snap->document_count      = repl_state_document_count();
    snap->edit_line           = repl_state_edit_line();
    snap->normals_dirty       = repl_state_normals_dirty();

    snap->insert_mode         = snap->editor_input.insert_mode;
    snap->cursor_pos          = snap->editor_input.cursor_pos;
    snap->input_len           = snap->editor_input.input_len;
    snap->pending_newline_len = snap->editor_input.pending_newline_len;
    snap->flat_program_count  = snap->flat_program.cmd_count;

    snap->grid_major_steps    = repl_state_grid_major_steps();
    snap->grid_extents        = repl_state_grid_extents();

    snap->user_scene_count        = repl_user_scene_count();
    snap->user_scene_active_idx   = repl_active_user_scene();
    for (int i = 0; i < MAX_USER_SCENES; i++) {
        snap->user_scene_slot_used[i] = repl_user_scene_slot_used(i);
        snap->user_scene_names[i]     = repl_user_scene_name(i);
    }

    snap->workspace_dir = repl_state_workspace_dir();
    snap->editor_transformers = editor_state_transformers();
    snap->editor_highlights = editor_state_highlights();
    snap->editor_virtual_lines = editor_state_virtual_lines();

    /* Selection range materialized once for the per-row code-panel branch. */
    snap->selection_active = repl_clipboard_sel_active();
    snap->selection_lo     = snap->selection_active ? repl_clipboard_sel_lo() : -1;
    snap->selection_hi     = snap->selection_active ? repl_clipboard_sel_hi() : -1;

    /* Indent + statusbar metadata so the render path does not call back
     * into repl_source_scope_* / repl_code_panel_document_* per row. */
    snap->active_indent_chars   = repl_code_panel_document_active_indent_chars();
    snap->trailing_indent_chars = repl_source_scope_cmd_indent_chars(snap->document_count);
    snap->in_begin_block        = repl_source_scope_in_begin_block();
    snap->current_begin_mode    = current_begin_mode();
}

void imrepl_ctrl_display_frame(void) {
    int saved_flat_count;
    float live_predef_vals[MAX_PREDEF_VARS] = { 0 };
    FlatProgramView flat_program = repl_state_flat_program_view();
    int g_num_flat_cmds = flat_program.cmd_count;
    ReplReplayRuntimeState replay = replay_state_view();
    SceneRenderConfig scene_config;
    UiRenderSnapshot ui_snap;

    prof_frame_tick();
    prof_begin(PROF_FRAME_TOTAL);

    if (repl_state_normals_dirty()) {
        recompute_autonormals();
        repl_state_normals_dirty_clear();
    }
    if (repl_state_flat_program_dirty()) {
        prof_begin(PROF_FLATTEN);
        flatten_commands();
        repl_state_flat_program_clear_dirty();
        prof_end(PROF_FLATTEN);
        flat_program = repl_state_flat_program_view();
        g_num_flat_cmds = flat_program.cmd_count;
    }

    /* Snapshot production: every per-frame list/snapshot the UI consumes
     * is built here so timing of the producer side is visible in profile.
     * The aggregate PROF_SNAPSHOT section sums the four sub-phases plus
     * the scene-config and ui-snapshot builders below. */
    prof_begin(PROF_SNAPSHOT);

    prof_begin(PROF_SNAPSHOT_TRANSFORMERS);
    imrepl_ctrl_push_color_transformers();
    prof_end(PROF_SNAPSHOT_TRANSFORMERS);

    prof_begin(PROF_SNAPSHOT_HIGHLIGHTS);
    imrepl_ctrl_push_highlights();
    prof_end(PROF_SNAPSHOT_HIGHLIGHTS);

    /* Prepare replay annotations + push the virtual-line list.
     * Layout also calls prepare(), so this stays idempotent. */
    prof_begin(PROF_SNAPSHOT_VIRTUAL_LINES);
    repl_replay_annotations_prepare(editor_buffer_view());
    prof_end(PROF_SNAPSHOT_VIRTUAL_LINES);

    saved_flat_count = g_num_flat_cmds;
    repl_copy_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    if (replay.active)
        repl_state_flat_program_set_count(repl_replay_prepare_frame(saved_flat_count));

    update_render_state_strings();
    update_cam_lines();

    prof_begin(PROF_SNAPSHOT_SCENE_CONFIG);
    imrepl_ctrl_build_scene_config(&scene_config);
    prof_end(PROF_SNAPSHOT_SCENE_CONFIG);

    prof_begin(PROF_SNAPSHOT_UI);
    imrepl_ctrl_build_ui_snapshot(&ui_snap);
    prof_end(PROF_SNAPSHOT_UI);

    prof_end(PROF_SNAPSHOT);

    /* 3D scene - scene_render_3d_scene() handles optional accumulation-buffer AA */
    /* Reset subsection accumulators so timings across all AA samples sum up
     * correctly before the first (or only) scene_render_3d_scene() call. */
    for (ProfSection section_idx = PROF_SCENE_3D_SETUP; section_idx <= PROF_SCENE_3D_HUD; section_idx++)
        prof_accum_reset(section_idx);
    prof_begin(PROF_SCENE_3D);
    scene_render_3d_scene(&scene_config);
    prof_end(PROF_SCENE_3D);

    if (scene_config.replaying) {
        ReplPresentationState presentation = repl_state_presentation();
        UiReplayHudState replay_hud_state = {
            .scene_x = scene_config.scene_x,
            .scene_y = scene_config.scene_y,
            .scene_w = scene_config.scene_w,
            .scene_h = scene_config.scene_h,
            .viewport_w = scene_config.viewport_w,
            .viewport_h = scene_config.viewport_h,
            .code_panel_layout = presentation.code_panel_layout,
            .replay_mode = scene_config.replay_mode,
            .replay_pc = replay.pc,
            .replay_total_cmds = replay.total_flat_cmds,
            .replay_state_val = replay.state,
            .replay_speed = replay.speed,
            .replay_expand_args = replay.expand_args,
            .replaying = scene_config.replaying,
        };
        ui_replay_hud_render(&replay_hud_state);
    }

    /* Commit the accumulated subsection totals now that all AA samples are done. */
    for (ProfSection section_idx = PROF_SCENE_3D_SETUP; section_idx <= PROF_SCENE_3D_HUD; section_idx++)
        prof_accum_commit(section_idx);

    prof_begin(PROF_CODE_PANEL);
    ui_panels_render_code_panel(&ui_snap);
    prof_end(PROF_CODE_PANEL);

    prof_begin(PROF_UI_PANELS);
    ui_autocomplete_panel_render(&ui_snap);
    ui_menu_bar_render_example_dropdown(&ui_snap);
    ui_variable_panel_render(&ui_snap);
    ui_panels_render_scene_status(&ui_snap);
    ui_help_overlay_render(&ui_snap);
    prof_end(PROF_UI_PANELS);

    ui_profile_panel_render(&ui_snap);

    repl_state_flat_program_set_count(saved_flat_count);
    repl_restore_predef_values(live_predef_vals, MAX_PREDEF_VARS);

    prof_end(PROF_FRAME_TOTAL);
}

void imrepl_ctrl_reshape(int w, int h) {
    if (h < 1) h = 1;
    ui_state_viewport_set_size(w, h);
}

void imrepl_ctrl_init_gl(void) {
    repl_state_init_defaults();
    ensure_init_bootstrap_ready();
    scene_render_init_gl();
    repl_executor_init_resources();
    apply_init_bootstrap();
}

void imrepl_ctrl_bootstrap_repl(const char *input_file) {
    repl_eval_init_predef_vars();
    for (int i = 0; i < g_num_predef_vars; i++) {
        if (strcmp(g_predef_vars[i].name, "t") == 0) {
            repl_state_variables_mut()->time_var_idx = i;
            break;
        }
    }
    repl_load_initial_commands(input_file);
}

void imrepl_ctrl_set_accum(int enabled) {
    repl_state_render_mut()->use_accum = enabled ? 1 : 0;
}

void imrepl_ctrl_keyboard(unsigned char key, int x, int y) {
    imrepl_ctrl_apply_input_effects(editor_handle_key(key, x, y));
}

void imrepl_ctrl_special(int key, int x, int y) {
    imrepl_ctrl_apply_input_effects(editor_handle_special(key, x, y));
}

void imrepl_ctrl_mouse(int button, int state, int x, int y) {
    imrepl_ctrl_apply_input_effects(editor_handle_mouse(button, state, x, y));
}

void imrepl_ctrl_motion(int x, int y) {
    imrepl_ctrl_apply_input_effects(editor_handle_motion(x, y));
}

void imrepl_ctrl_passive_motion(int x, int y) {
    imrepl_ctrl_apply_input_effects(editor_handle_passive_motion(x, y));
}

void imrepl_ctrl_mousewheel(int wheel, int direction, int x, int y) {
    imrepl_ctrl_apply_input_effects(repl_mousewheel_func(wheel, direction, x, y));
}

void imrepl_ctrl_timer(int value) {
    imrepl_ctrl_apply_input_effects(repl_timer_func(value));
}
