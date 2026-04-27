#include "imrepl_ctrl.h"

#include "repl_core_internal.h"
#include "repl_executor.h"
#include "repl_replay.h"
#include "repl_state.h"
#include "scene_render.h"
#include "ui_autocomplete_panel.h"
#include "ui_help_overlay.h"
#include "ui_menu_bar.h"
#include "ui_panels.h"
#include "ui_profile_panel.h"
#include "ui_variable_panel.h"
#include "prof.h"

void imrepl_ctrl_display_frame(void) {
    int saved_flat_count;
    float live_predef_vals[MAX_PREDEF_VARS] = { 0 };
    FlatProgramView flat_program = repl_state_flat_program_view();
    int g_num_flat_cmds = flat_program.cmd_count;
    const ReplReplayRuntimeState *replay = repl_state_replay();

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

    /* 3D scene - scene_render_3d_scene() handles optional accumulation-buffer AA */
    /* Reset subsection accumulators so timings across all AA samples sum up
     * correctly before the first (or only) scene_render_3d_scene() call. */
    for (ProfSection section_idx = PROF_SCENE_3D_SETUP; section_idx <= PROF_SCENE_3D_HUD; section_idx++)
        prof_accum_reset(section_idx);
    prof_begin(PROF_SCENE_3D);
    scene_render_3d_scene();
    prof_end(PROF_SCENE_3D);
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