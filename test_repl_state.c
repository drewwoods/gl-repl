#include "repl_state.h"

#include <stdio.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s] (line %d)\n", label, __LINE__); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    g_run++; \
    if ((got) == (exp)) g_pass++; \
    else printf("FAIL [%s] got %d, expected %d (line %d)\n", \
                label, (int)(got), (int)(exp), __LINE__); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    g_run++; \
    if (strcmp((got), (exp)) == 0) g_pass++; \
    else printf("FAIL [%s] got \"%s\", expected \"%s\" (line %d)\n", \
                label, (got), (exp), __LINE__); \
} while (0)

static void populate_runtime_snapshot_fixture(const char *scene_hint) {
    char err[128];
    int foo_idx;
    GLCmd *doc_cmds;
    GLCmd *flat_cmds;
    FlatCmdLocalVars *flat_locals;
    ReplClipboardState *clipboard;
    ReplCodePanelRuntimeState *code_panel;
    ReplStatusState *status;
    ReplAutocompleteState *ac;
    ReplVariableDragState *drag;
    ReplImportExportState *io;
    ReplSceneRuntimeState *scenes;
    ReplPresentationState *presentation;
    ReplRenderState *render;
    ReplReplayRuntimeState *replay;
    ReplFlatProgramState *flat_program;

    repl_state_input_set_text("glVertex3f(1, 2, 3)");
    repl_state_cursor_pos_set(6);
    repl_state_document_count_set(2);
    repl_state_edit_line_set(1);
    doc_cmds = repl_state_document_cmds_mut();
    doc_cmds[0].type = CMD_BEGIN;
    doc_cmds[0].valid = 1;
    snprintf(doc_cmds[0].source, sizeof(doc_cmds[0].source), "%s",
             "  glBegin(GL_TRIANGLES);");
    doc_cmds[1].type = CMD_END;
    doc_cmds[1].valid = 1;
    snprintf(doc_cmds[1].source, sizeof(doc_cmds[1].source), "%s",
             "  glEnd();");

    repl_state_flat_program_set_count(1);
    flat_program = repl_state_flat_program_mut();
    flat_cmds = repl_state_flat_program_cmds_mut();
    flat_locals = repl_state_flat_program_local_vars_mut();
    flat_cmds[0].type = CMD_VERTEX3F;
    flat_cmds[0].valid = 1;
    flat_cmds[0].src_cmd_idx = 7;
    flat_locals[0].num_vars = 1;
    snprintf(flat_locals[0].vars[0].name, sizeof(flat_locals[0].vars[0].name),
             "%s", "i");
    flat_locals[0].vars[0].value = 3.0f;
    *flat_program->dirty = 0;
    *flat_program->user_lighting_enabled = 1;
    repl_state_flat_program_set_current_block(2, 5, 4);

    repl_state_selection_set(4, 7);
    clipboard = repl_state_clipboard_mut();
    clipboard->cmd_count = 1;
    clipboard->cmds[0].type = CMD_COLOR3F;

    repl_state_help_mut()->visible = 1;
    repl_state_help_mut()->tab_idx = 1;
    repl_state_help_mut()->scroll = 3;
    code_panel = repl_state_code_panel_mut();
    *code_panel->panel_frac = 0.61f;
    *code_panel->resizing_panel = 1;
    *code_panel->scroll = 9;
    *code_panel->scroll_follow_cursor = 1;
    *code_panel->cursor_visible = 0;
    *code_panel->blink_tick = 12;
    *code_panel->cursor_px = 123;
    *code_panel->cursor_py = 456;
    repl_state_variable_panel_mut()->visible = 0;

    drag = repl_state_variable_drag_mut();
    drag->var_idx = 3;
    drag->log_mode = 1;
    drag->start_value = 2.5f;
    drag->start_x = 17;
    repl_state_profile_panel_mut()->mode = PROFILE_PANEL_DETAILS;

    status = repl_state_status_mut();
    snprintf(status->text, sizeof(status->text), "%s", "state snapshot");
    status->ttl = 17;

    repl_state_search_mut()->active = 1;
    snprintf(repl_state_search_mut()->query,
             sizeof(repl_state_search_mut()->query),
             "%s", "vertex");
    repl_state_search_mut()->query_len = 6;
    repl_state_search_mut()->cursor_pos = 2;
    repl_state_search_mut()->hit_line_idx = 5;
    repl_state_search_mut()->hit_char_idx = 8;
    repl_state_search_mut()->hit_ordinal = 2;
    repl_state_search_mut()->match_count = 4;

    ac = repl_state_autocomplete_mut();
    ac->matches[0] = "glVertex3f";
    ac->matches[1] = "glVertex2f";
    ac->insert_matches[0] = "glVertex3f(";
    ac->insert_matches[1] = "glVertex2f(";
    ac->match_count = 2;
    ac->selected_idx = 1;
    snprintf(ac->ghost, sizeof(ac->ghost), "%s", "ghost text");
    snprintf(ac->hint, sizeof(ac->hint), "%s", "hint text");

    *repl_state_variables_mut()->time_playing = 0;
    *repl_state_variables_mut()->anim_time = 1.25f;
    foo_idx = repl_eval_find_predef_var_idx("foo");
    if (foo_idx < 0)
        repl_eval_declare_predef_var("foo", err, sizeof(err));
    foo_idx = repl_eval_find_predef_var_idx("foo");
    ASSERT_TRUE("foo var declared", foo_idx >= 0);
    if (foo_idx >= 0)
        g_predef_vars[foo_idx].value = 42.0f;

    repl_state_camera_set(11.0f, 22.0f, 7.5f, 0.5f, -0.25f, 1.75f, 0.9f);
    repl_state_camera_mut()->auto_rotate = 1;
    repl_state_pointer_set(321, 654, 2);
    repl_state_viewport_set_size(1440, 900);

    presentation = repl_state_presentation_mut();
    *presentation->wireframe = 1;
    *presentation->grid_theme = GRID_THEME_TRON;
    *presentation->grid_major_idx = GRID_MAJOR_10;
    *presentation->grid_extent_idx = GRID_EXTENT_CLOSE;
    *presentation->axes_theme = AXES_THEME_NEON;
    *presentation->show_normal_vectors = 1;
    *presentation->show_vertex_indices = 0;
    *presentation->show_vertex_points = 0;
    *presentation->show_vertex_guides = 0;
    *presentation->show_light_indicators = 0;
    *presentation->backdrop_mode = 1;
    *presentation->highlight_current_poly = 0;
    *presentation->ortho_mode = 1;
    *presentation->wrap_at_comma = 0;
    *presentation->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM;
    presentation->focus_vertex[0] = 1.5f;
    presentation->focus_vertex[1] = -2.0f;
    presentation->focus_vertex[2] = 0.25f;
    *presentation->focus_vertex_valid = 1;

    render = repl_state_render_mut();
    *render->use_accum = 0;
    *render->accum_aa_enabled = 0;
    *render->accum_samples = 8;
    *render->accum_jitter_x = 0.25f;
    *render->accum_jitter_y = -0.125f;
    *render->multisample_enabled = 0;
    *render->line_smooth_enabled = 1;
    *render->point_attenuation_enabled = 0;
    render->lights[1].enabled = 0;
    render->lights[2].pos[0] = 7.5f;
    render->clear_color[0] = 0.20f;
    render->clear_color[1] = 0.25f;
    render->clear_color[2] = 0.30f;
    render->clear_color[3] = 1.0f;

    replay = repl_state_replay_mut();
    *replay->active = 1;
    *replay->state = REPLAY_PAUSED;
    *replay->pc = 9;
    *replay->mode = REPLAY_MODE_POLYGON;
    *replay->speed = 12.5f;
    *replay->accum = 0.75f;
    *replay->fade_speed = 6.0f;
    *replay->src_line_idx = 7;
    *replay->total_flat_cmds = 13;
    *replay->expand_args = 0;

    scenes = repl_state_scenes_mut();
    scenes->active_example_idx = 3;
    repl_state_workspace_set_dir("/tmp/repl-state-stage1");
    io = repl_state_import_export_mut();
    io->export_scene_name_hint = scene_hint;
    snprintf(io->pending_scene_name, sizeof(io->pending_scene_name), "%s",
             "pending scene");
    snprintf(io->pending_workspace_dir, sizeof(io->pending_workspace_dir), "%s",
             "/tmp/pending-workspace");
}

static void test_capture_restore_round_trip(void) {
    static ReplRuntimeState snapshot;
    static ReplRuntimeState round_trip;
    static const char *scene_hint = "Captured Scene";
    int foo_idx;

    repl_state_init_defaults();
    populate_runtime_snapshot_fixture(scene_hint);
    repl_state_capture(&snapshot);
    repl_state_reset_all();
    repl_state_restore(&snapshot);
    repl_state_capture(&round_trip);

    ASSERT_STR("input restored",
               repl_state_input_text(),
               "glVertex3f(1, 2, 3)");
    ASSERT_INT("cursor restored", repl_state_cursor_pos(), 6);
    ASSERT_INT("document count restored", repl_state_document_count(), 2);
    ASSERT_INT("edit line restored", repl_state_edit_line(), 1);
    ASSERT_INT("document cmd type restored", repl_state_document_cmds()[0].type, CMD_BEGIN);
    ASSERT_STR("document cmd source restored",
               repl_state_document_cmds()[0].source,
               "  glBegin(GL_TRIANGLES);");
    ASSERT_INT("flat count restored", repl_state_flat_program_count(), 1);
    ASSERT_INT("flat cmd type restored", repl_state_flat_program_cmds()[0].type, CMD_VERTEX3F);
    ASSERT_INT("flat local var count restored",
               repl_state_flat_program_local_vars_mut()[0].num_vars, 1);
    ASSERT_INT("flat lighting restored",
               repl_state_flat_program_user_lighting_enabled(), 1);
    ASSERT_INT("selection anchor restored", repl_state_selection()->anchor_idx, 4);
    ASSERT_INT("selection end restored", repl_state_selection()->end_idx, 7);
    ASSERT_INT("clipboard count restored", repl_state_clipboard()->cmd_count, 1);
    ASSERT_INT("clipboard cmd restored", repl_state_clipboard()->cmds[0].type, CMD_COLOR3F);
    ASSERT_INT("help restored", repl_state_help().visible, 1);
    ASSERT_INT("help tab restored", repl_state_help().tab_idx, 1);
    ASSERT_INT("help scroll restored", repl_state_help().scroll, 3);
    ASSERT_TRUE("code panel frac restored", *repl_state_code_panel()->panel_frac == 0.61f);
    ASSERT_INT("code panel resizing restored", *repl_state_code_panel()->resizing_panel, 1);
    ASSERT_INT("code panel scroll restored", *repl_state_code_panel()->scroll, 9);
    ASSERT_INT("code panel follow restored",
               *repl_state_code_panel()->scroll_follow_cursor, 1);
    ASSERT_INT("code panel cursor visible restored",
               *repl_state_code_panel()->cursor_visible, 0);
    ASSERT_INT("code panel blink restored", *repl_state_code_panel()->blink_tick, 12);
    ASSERT_INT("code panel cursor x restored", *repl_state_code_panel()->cursor_px, 123);
    ASSERT_INT("code panel cursor y restored", *repl_state_code_panel()->cursor_py, 456);
    ASSERT_INT("variable panel restored", repl_state_variable_panel()->visible, 0);
    ASSERT_INT("variable drag idx restored", repl_state_variable_drag()->var_idx, 3);
    ASSERT_INT("variable drag log restored", repl_state_variable_drag()->log_mode, 1);
    ASSERT_TRUE("variable drag value restored",
                repl_state_variable_drag()->start_value == 2.5f);
    ASSERT_INT("variable drag x restored", repl_state_variable_drag()->start_x, 17);
    ASSERT_INT("profile panel restored",
               repl_state_profile_panel()->mode, PROFILE_PANEL_DETAILS);
    ASSERT_STR("status text restored", repl_state_status()->text, "state snapshot");
    ASSERT_INT("status ttl restored", repl_state_status()->ttl, 17);
    ASSERT_INT("search active restored", repl_state_search()->active, 1);
    ASSERT_STR("search query restored", repl_state_search()->query, "vertex");
    ASSERT_INT("search cursor restored", repl_state_search()->cursor_pos, 2);
    ASSERT_INT("search hit line restored", repl_state_search()->hit_line_idx, 5);
    ASSERT_INT("search hit char restored", repl_state_search()->hit_char_idx, 8);
    ASSERT_INT("search ordinal restored", repl_state_search()->hit_ordinal, 2);
    ASSERT_INT("search count restored", repl_state_search()->match_count, 4);
    ASSERT_INT("autocomplete count restored", repl_state_autocomplete()->match_count, 2);
    ASSERT_INT("autocomplete selection restored",
               repl_state_autocomplete()->selected_idx, 1);
    ASSERT_STR("autocomplete ghost restored", repl_state_autocomplete()->ghost, "ghost text");
    ASSERT_STR("autocomplete hint restored", repl_state_autocomplete()->hint, "hint text");
    ASSERT_STR("autocomplete match restored",
               repl_state_autocomplete()->matches[0], "glVertex3f");
    ASSERT_STR("autocomplete insert restored",
               repl_state_autocomplete()->insert_matches[1], "glVertex2f(");
    ASSERT_INT("time playing restored", *repl_state_variables()->time_playing, 0);
    ASSERT_TRUE("anim time restored", *repl_state_variables()->anim_time == 1.25f);
    ASSERT_TRUE("camera rx restored", repl_state_camera()->rx == 11.0f);
    ASSERT_TRUE("camera ry restored", repl_state_camera()->ry == 22.0f);
    ASSERT_TRUE("camera dist restored", repl_state_camera()->dist == 7.5f);
    ASSERT_TRUE("camera tx restored", repl_state_camera()->tx == 0.5f);
    ASSERT_TRUE("camera ty restored", repl_state_camera()->ty == -0.25f);
    ASSERT_TRUE("camera tz restored", repl_state_camera()->tz == 1.75f);
    ASSERT_TRUE("camera glow restored", repl_state_camera()->motion_glow == 0.9f);
    ASSERT_INT("camera rotate restored", repl_state_camera()->auto_rotate, 1);
    ASSERT_INT("pointer x restored", repl_state_pointer()->mouse_x, 321);
    ASSERT_INT("pointer y restored", repl_state_pointer()->mouse_y, 654);
    ASSERT_INT("pointer button restored", repl_state_pointer()->mouse_button, 2);
    ASSERT_INT("viewport width restored", repl_state_viewport()->window_w, 1440);
    ASSERT_INT("viewport height restored", repl_state_viewport()->window_h, 900);
    ASSERT_INT("presentation wireframe restored", *repl_state_presentation()->wireframe, 1);
    ASSERT_INT("presentation grid restored", *repl_state_presentation()->grid_theme, GRID_THEME_TRON);
    ASSERT_INT("presentation layout restored",
               *repl_state_presentation()->code_panel_layout, CODE_PANEL_LAYOUT_BOTTOM);
    ASSERT_INT("presentation focus valid restored",
               *repl_state_presentation()->focus_vertex_valid, 1);
    ASSERT_TRUE("presentation focus x restored",
                repl_state_presentation()->focus_vertex[0] == 1.5f);
    ASSERT_INT("render use accum restored", *repl_state_render()->use_accum, 0);
    ASSERT_INT("render accum aa restored", *repl_state_render()->accum_aa_enabled, 0);
    ASSERT_INT("render samples restored", *repl_state_render()->accum_samples, 8);
    ASSERT_TRUE("render jitter restored", *repl_state_render()->accum_jitter_x == 0.25f);
    ASSERT_INT("render light enabled restored", repl_state_render()->lights[1].enabled, 0);
    ASSERT_TRUE("render clear color restored",
                repl_state_render()->clear_color[2] == 0.30f);
    ASSERT_INT("replay active restored", *repl_state_replay()->active, 1);
    ASSERT_INT("replay state restored", *repl_state_replay()->state, REPLAY_PAUSED);
    ASSERT_INT("replay mode restored", *repl_state_replay()->mode, REPLAY_MODE_POLYGON);
    ASSERT_INT("replay pc restored", *repl_state_replay()->pc, 9);
    ASSERT_TRUE("replay speed restored", *repl_state_replay()->speed == 12.5f);
    ASSERT_INT("replay src line restored", *repl_state_replay()->src_line_idx, 7);
    ASSERT_INT("replay expand restored", *repl_state_replay()->expand_args, 0);
    ASSERT_INT("active example restored", repl_state_scenes()->active_example_idx, 3);
    ASSERT_STR("workspace restored",
               repl_state_workspace_dir(),
               "/tmp/repl-state-stage1");
    ASSERT_STR("pending scene restored",
               repl_state_import_export()->pending_scene_name,
               "pending scene");
    ASSERT_STR("pending workspace restored",
               repl_state_import_export()->pending_workspace_dir,
               "/tmp/pending-workspace");
    ASSERT_STR("scene hint restored",
               repl_state_import_export()->export_scene_name_hint,
               scene_hint);
    ASSERT_TRUE("workspace header count restored",
                repl_state_import_export()->workspace_header_line_count >= 3);
    ASSERT_STR("workspace header banner restored",
               repl_state_import_export()->workspace_header_lines[0],
               "// @workspace: REPL state (auto-saved)");
    ASSERT_STR("workspace header scene restored",
               repl_state_import_export()->workspace_header_lines[1],
               "// @scene-name Captured Scene");
    ASSERT_STR("workspace header dir restored",
               repl_state_import_export()->workspace_header_lines[2],
               "// @workspace-dir /tmp/repl-state-stage1");

    foo_idx = repl_eval_find_predef_var_idx("foo");
    ASSERT_TRUE("foo var restored", foo_idx >= 0);
    if (foo_idx >= 0)
        ASSERT_TRUE("foo value restored", g_predef_vars[foo_idx].value == 42.0f);
    ASSERT_TRUE("runtime snapshot round trip",
                memcmp(&snapshot, &round_trip, sizeof(snapshot)) == 0);
}

static void test_reset_all_restores_default_runtime(void) {
    static ReplRuntimeState defaults;
    static ReplRuntimeState reset_state;

    repl_state_init_defaults();
    repl_state_capture(&defaults);

    populate_runtime_snapshot_fixture("Reset Scene");
    repl_state_reset_all();
    repl_state_capture(&reset_state);

    ASSERT_TRUE("reset_all restores default snapshot",
                memcmp(&defaults, &reset_state, sizeof(defaults)) == 0);
    ASSERT_INT("reset_all help hidden", repl_state_help().visible, 0);
    ASSERT_INT("reset_all panel scroll", *repl_state_code_panel()->scroll, 0);
    ASSERT_INT("reset_all replay mode", *repl_state_replay()->mode, REPLAY_MODE_VERTEX);
    ASSERT_INT("reset_all replay expand", *repl_state_replay()->expand_args, 1);
    ASSERT_STR("reset_all workspace dir", repl_state_workspace_dir(), "");
    ASSERT_INT("reset_all workspace header count",
               repl_state_import_export()->workspace_header_line_count,
               defaults.import_export.workspace_header_line_count);
    ASSERT_STR("reset_all workspace header banner",
               repl_state_import_export()->workspace_header_lines[0],
               "// @workspace: REPL state (auto-saved)");
}

int main(void) {
    printf("--- repl_state tests ---\n");
    test_capture_restore_round_trip();
    test_reset_all_restores_default_runtime();
    printf("%d / %d tests passed\n", g_pass, g_run);
    return g_pass == g_run ? 0 : 1;
}
