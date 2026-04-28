#include "imrepl_ctrl.h"

#include "repl_core.h"
#include "repl_executor.h"
#include "repl_eval.h"
#include "repl_pipeline.h"
#include "repl_replay.h"
#include "repl_state.h"
#include "scene_render.h"
#include "ui_replay_hud.h"
#include "ui_autocomplete_panel.h"
#include "ui_help_overlay.h"
#include "ui_menu_bar.h"
#include "ui_panels.h"
#include "repl_layout.h"
#include "ui_profile_panel.h"
#include "ui_variable_panel.h"
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
    const ReplPresentationState *presentation = repl_state_presentation();
    const ReplVariableState *vars = repl_state_variables();
    const ReplEditorInputState *input = repl_state_editor_input();
    ReplPredefView predef = repl_eval_predef_view();

    SceneGuideSnapshot snapshot = {
        .show_guides = config->show_guides,
        .replaying = config->replaying,
        .xform_guide_mode = *presentation->xform_guide_mode,
        .user_lighting_enabled = config->user_lighting_enabled,
        .anim_time = *vars->anim_time,
        .input = input->input,
        .input_len = *input->input_len,
        .cursor_pos = *input->cursor_pos,
        .edit_line_idx = config->edit_line_idx,
        .inserting = repl_state_insert_mode(),
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
        .program = program
    });
}

/* Execute reset callback: clears fade context after last fade batch */
static void scene_execute_reset_adapter(void *user_data) {
    (void)user_data;
    repl_execute_set_fade_context(1.0f, 0);
}

static void imrepl_ctrl_build_scene_config(SceneRenderConfig *config) {
    const ReplRenderState *render = repl_state_render();
    const ReplReplayRuntimeState *replay = repl_state_replay();
    const ReplPresentationState *presentation = repl_state_presentation();
    const ReplCameraState *cam = repl_state_camera();
    const ReplFlatProgramState *flat = repl_state_flat_program();

    /* Refresh cursor block highlight before reading cursor state */
    repl_flatten_refresh_current_block_highlight();

    /* Existing fields (legacy, preserved) */
    repl_layout_scene_rect(&config->scene_x, &config->scene_y,
                           &config->scene_w, &config->scene_h);
    if (config->scene_w < 1) config->scene_w = 1;
    if (config->scene_h < 1) config->scene_h = 1;

    config->cam_dist = *cam->dist;
    config->cam_rx = *cam->rx;
    config->cam_ry = *cam->ry;
    config->cam_tx = *cam->tx;
    config->cam_ty = *cam->ty;
    config->cam_tz = *cam->tz;
    config->cam_motion_glow = *cam->motion_glow;
    config->multisample_enabled = *render->multisample_enabled;
    config->line_smooth_enabled = *render->line_smooth_enabled;
    config->wireframe = *presentation->wireframe;
    config->grid_theme = *presentation->grid_theme;
    config->grid_extent_idx = *presentation->grid_extent_idx;
    config->grid_major_idx = *presentation->grid_major_idx;
    config->axes_theme = *presentation->axes_theme;
    config->show_guides = *presentation->show_vertex_guides;
    config->show_vpoints = *presentation->show_vertex_points;
    config->show_vnums = *presentation->show_vertex_labels;
    config->show_normals = *presentation->show_normal_vectors;
    config->replaying = *replay->active;
    config->replay_mode = *replay->mode;
    config->replay_tess_preview = config->replaying &&
                                  config->replay_mode == REPLAY_MODE_VERTEX;
    config->replay_vertex_points = config->replay_tess_preview;
    config->show_current_poly = *presentation->highlight_current_poly && !config->replaying;
    imrepl_ctrl_build_replay_fade_plan(config);

    /* Alpha scale boost for dark backgrounds */
    float bg_lum = 0.2126f * render->clear_color[0]
                 + 0.7152f * render->clear_color[1]
                 + 0.0722f * render->clear_color[2];
    float as_val = (0.10f + 0.02f) / fmaxf(bg_lum + 0.02f, 1e-4f);
    config->alpha_scale = as_val < 1.0f ? 1.0f : (as_val > 3.0f ? 3.0f : as_val);

    /* New fields (push model) */
    config->execute_fn = scene_execute_adapter;
    config->execute_reset_fn = scene_execute_reset_adapter;
    config->execute_user_data = NULL;

    config->flat_program = repl_state_flat_program_view();
    config->anim_time = *repl_state_variables()->anim_time;

    config->viewport_w = *repl_state_viewport()->window_w;
    config->viewport_h = *repl_state_viewport()->window_h;

    config->use_accum = *render->use_accum;
    config->accum_aa_enabled = *render->accum_aa_enabled;
    config->accum_samples = *render->accum_samples;
    config->user_lighting_enabled = *flat->user_lighting_enabled;
    memcpy(config->lights, repl_state_render()->lights, sizeof(config->lights));
    config->show_light_indicators = *presentation->show_light_indicators;

    config->backdrop_mode = *presentation->backdrop_mode;
    config->show_vertex_outlines = *presentation->show_vertex_outlines;

        config->code_panel_layout = *presentation->code_panel_layout;
        config->replay_pc = *replay->pc;
        config->replay_total_cmds = *replay->total_flat_cmds;
    config->replay_state_val = *replay->state;
    config->replay_speed = *replay->speed;
    config->replay_expand_args = *replay->expand_args;

        memcpy(config->grid_major_steps, presentation->grid_major_steps,
            sizeof(config->grid_major_steps));
        memcpy(config->grid_extents, presentation->grid_extents,
            sizeof(config->grid_extents));

        config->cursor_block_begin_idx = *flat->current_block_begin_idx;
        config->cursor_block_end_idx = *flat->current_block_end_idx;
        config->cursor_block_source_line = *flat->current_block_source_line_idx;
    config->edit_line_idx = repl_state_edit_line();
        config->cursor_func_scope_mask = 0;
        config->cursor_call_src_cmd_idx = -1;
        config->focus = imrepl_ctrl_build_focus_vertex();
        config->guide_snapshot = imrepl_ctrl_build_guide_snapshot(config);
}

void imrepl_ctrl_display_frame(void) {
    int saved_flat_count;
    float live_predef_vals[MAX_PREDEF_VARS] = { 0 };
    FlatProgramView flat_program = repl_state_flat_program_view();
    int g_num_flat_cmds = flat_program.cmd_count;
    const ReplReplayRuntimeState *replay = repl_state_replay();
    SceneRenderConfig scene_config;

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

    saved_flat_count = g_num_flat_cmds;
    repl_copy_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    if (*replay->active)
        repl_state_flat_program_set_count(repl_replay_prepare_frame(saved_flat_count));

    update_render_state_strings();
    update_cam_lines();
    imrepl_ctrl_build_scene_config(&scene_config);

    /* 3D scene - scene_render_3d_scene() handles optional accumulation-buffer AA */
    /* Reset subsection accumulators so timings across all AA samples sum up
     * correctly before the first (or only) scene_render_3d_scene() call. */
    for (ProfSection section_idx = PROF_SCENE_3D_SETUP; section_idx <= PROF_SCENE_3D_HUD; section_idx++)
        prof_accum_reset(section_idx);
    prof_begin(PROF_SCENE_3D);
    scene_render_3d_scene(&scene_config);
    prof_end(PROF_SCENE_3D);

    if (scene_config.replaying) {
        UiReplayHudState replay_hud_state = {
            .scene_x = scene_config.scene_x,
            .scene_y = scene_config.scene_y,
            .scene_w = scene_config.scene_w,
            .scene_h = scene_config.scene_h,
            .viewport_w = scene_config.viewport_w,
            .viewport_h = scene_config.viewport_h,
            .code_panel_layout = scene_config.code_panel_layout,
            .replay_mode = scene_config.replay_mode,
            .replay_pc = scene_config.replay_pc,
            .replay_total_cmds = scene_config.replay_total_cmds,
            .replay_state_val = scene_config.replay_state_val,
            .replay_speed = scene_config.replay_speed,
            .replay_expand_args = scene_config.replay_expand_args,
            .replaying = scene_config.replaying,
        };
        ui_replay_hud_render(&replay_hud_state);
    }

    /* Commit the accumulated subsection totals now that all AA samples are done. */
    for (ProfSection section_idx = PROF_SCENE_3D_SETUP; section_idx <= PROF_SCENE_3D_HUD; section_idx++)
        prof_accum_commit(section_idx);

    prof_begin(PROF_CODE_PANEL);
    ui_panels_render_code_panel();
    prof_end(PROF_CODE_PANEL);

    prof_begin(PROF_UI_PANELS);
    ui_autocomplete_panel_render();
    ui_menu_bar_render_example_dropdown();
    ui_variable_panel_render();
    ui_panels_render_scene_status();
    ui_help_overlay_render();
    prof_end(PROF_UI_PANELS);

    ui_profile_panel_render();

    repl_state_flat_program_set_count(saved_flat_count);
    repl_restore_predef_values(live_predef_vals, MAX_PREDEF_VARS);

    prof_end(PROF_FRAME_TOTAL);
}

void imrepl_ctrl_reshape(int w, int h) {
    if (h < 1) h = 1;
    repl_state_viewport_set_size(w, h);
}

void imrepl_ctrl_init_gl(void) {
    ensure_init_bootstrap_ready();
    scene_render_init_gl();
    repl_executor_init_resources();
    apply_init_bootstrap();
}

void imrepl_ctrl_keyboard(unsigned char key, int x, int y) {
    repl_keyboard_func(key, x, y);
}

void imrepl_ctrl_special(int key, int x, int y) {
    repl_special_func(key, x, y);
}

void imrepl_ctrl_mouse(int button, int state, int x, int y) {
    repl_mouse_func(button, state, x, y);
}

void imrepl_ctrl_motion(int x, int y) {
    repl_motion_func(x, y);
}

void imrepl_ctrl_passive_motion(int x, int y) {
    repl_passive_motion_func(x, y);
}

void imrepl_ctrl_mousewheel(int wheel, int direction, int x, int y) {
    repl_mousewheel_func(wheel, direction, x, y);
}

void imrepl_ctrl_timer(int value) {
    repl_timer_func(value);
}