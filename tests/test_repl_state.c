#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "repl/state.h"
#include "editor/state.h"
#include "ui/app/state.h"
#include "app/glr_camera.h"
#include "ui/app/profile_panel.h"
#include "ui/core/layout.h"            /* CODE_PANEL_LAYOUT_* enum values */
#include "subsystems/variable_panel/variable_panel_state.h"
#include "subsystems/replay/replay.h"               /* REPLAY_PAUSED, REPLAY_MODE_* enums */
#include "subsystems/replay/replay_state.h"
#include "editor/help_session.h"
#include "app/glr_defaults.h"    /* CFG_DEFAULT_* macros */

#include "support/test_harness.h"
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    TEST_ASSERT_INT(&g_harness, label, got, exp); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    TEST_ASSERT_STR(&g_harness, label, got, exp); \
} while (0)

static void populate_runtime_snapshot_fixture(const char *scene_hint) {
    char err[128];
    int foo_idx;
    GLCmd *doc_cmds;
    GLCmd *flat_cmds;
    FlatCmdLocalVars *flat_locals;
    EditorClipboardState *clipboard;
    UiCodePanelRuntimeState *code_panel;
    UiStatusState *status;
    EditorAutocompleteState *ac;
    EditorVariableDragState *drag;
    ReplImportExportState *io;
    ReplSceneRuntimeState *scenes;
    GlrPresentationState *presentation;
    GlrRenderState *glr_render;
    ReplRenderState *render;
    ReplReplayRuntimeState *replay;
    ReplFlatProgramState *flat_program;

    editor_input_set_text("glVertex3f(1, 2, 3)");
    editor_cursor_pos_set(6);
    repl_state_document_count_set(2);
    editor_state_edit_line_set(1);
    doc_cmds = repl_state_document_cmds_mut();
    doc_cmds[0].type = CMD_BEGIN;
    doc_cmds[0].valid = 1;
    editor_buffer_set_line(0, "  glBegin(GL_TRIANGLES);");
    doc_cmds[1].type = CMD_END;
    doc_cmds[1].valid = 1;
    editor_buffer_set_line(1, "  glEnd();");

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
    flat_program->dirty = 0;
    flat_program->user_lighting_enabled = 1;
    repl_state_flat_program_set_current_block(2, 5, 4);

    editor_state_selection_set(4, 7);
    clipboard = editor_state_clipboard_mut();
    clipboard->line_count = 1;
    snprintf(clipboard->lines[0], MAX_LINE_LEN, "glColor3f(1, 0, 0);");

    ui_state_help_mut()->visible = 1;
    editor_help_session_set_tab(1);
    editor_help_session_set_scroll(3);
    code_panel = ui_state_code_panel_mut();
    code_panel->panel_frac = 0.61f;
    code_panel->resizing_panel = 1;
    editor_scroll_set(9);
    editor_scroll_follow_cursor_set(1);
    code_panel->cursor_visible = 0;
    code_panel->blink_tick = 12;
    variable_panel_view_mut()->visible = 0;

    drag = variable_panel_drag_mut();
    drag->var_idx = 3;
    drag->log_mode = 1;
    drag->start_value = 2.5f;
    drag->start_x = 17;
    ui_state_profile_panel_mut()->mode = PROFILE_PANEL_DETAILS;

    status = ui_state_status_mut();
    snprintf(status->text, sizeof(status->text), "%s", "state snapshot");
    status->ttl = 17;

    editor_state_search_mut()->active = 1;
    snprintf(editor_state_search_mut()->query,
             sizeof(editor_state_search_mut()->query),
             "%s", "vertex");
    editor_state_search_mut()->query_len = 6;
    editor_state_search_mut()->cursor_pos = 2;
    editor_state_search_mut()->hit_line_idx = 5;
    editor_state_search_mut()->hit_char_idx = 8;
    editor_state_search_mut()->hit_ordinal = 2;
    editor_state_search_mut()->match_count = 4;

    ac = editor_state_autocomplete_mut();
    ac->matches[0] = "glVertex3f";
    ac->matches[1] = "glVertex2f";
    ac->insert_matches[0] = "glVertex3f(";
    ac->insert_matches[1] = "glVertex2f(";
    ac->match_count = 2;
    ac->selected_idx = 1;
    snprintf(ac->ghost, sizeof(ac->ghost), "%s", "ghost text");
    snprintf(ac->hint, sizeof(ac->hint), "%s", "hint text");

    repl_state_variables_mut()->time_playing = 0;
    repl_state_variables_mut()->anim_time = 1.25f;
    foo_idx = repl_eval_find_predef_var_idx("foo");
    if (foo_idx < 0)
        repl_eval_declare_predef_var("foo", err, sizeof(err));
    foo_idx = repl_eval_find_predef_var_idx("foo");
    ASSERT_TRUE("foo var declared", foo_idx >= 0);
    if (foo_idx >= 0)
        g_predef_vars[foo_idx].value = 42.0f;

    glr_camera_set(11.0f, 22.0f, 7.5f, 0.5f, -0.25f, 1.75f, 0.9f);
    glr_camera_mut()->auto_rotate = 1;
    ui_state_pointer_set(321, 654, 2);
    ui_state_viewport_set_size(1440, 900);

    presentation = glr_state_presentation_mut();
    presentation->wireframe = 1;
    presentation->grid_theme = GRID_THEME_TRON;
    presentation->grid_major_idx = GRID_MAJOR_10;
    presentation->grid_extent_idx = GRID_EXTENT_CLOSE;
    presentation->axes_theme = AXES_THEME_NEON;
    presentation->show_normal_vectors = 1;
    presentation->show_vertex_indices = 0;
    presentation->show_vertex_points = 0;
    presentation->show_vertex_guides = 0;
    presentation->show_light_indicators = 0;
    presentation->backdrop_mode = 1;
    presentation->highlight_current_poly = 0;
    presentation->ortho_mode = 1;
    presentation->wrap_at_comma = 0;
    presentation->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM;
    /* focus_vertex storage was deleted in step 7a — no live readers
     * existed; the per-frame snapshot is computed from the document
     * inside glr_ctrl. */

    glr_render = glr_state_render_mut();
    glr_render->use_accum = 0;
    glr_render->accum_aa_enabled = 0;
    glr_render->accum_samples = 8;
    glr_render->accum_jitter_x = 0.25f;
    glr_render->accum_jitter_y = -0.125f;
    glr_render->multisample_enabled = 0;
    glr_render->line_smooth_enabled = 1;
    glr_render->point_attenuation_enabled = 0;

    render = repl_state_render_mut();
    render->lights[1].enabled = 0;
    render->lights[2].pos[0] = 7.5f;
    render->clear_color[0] = 0.20f;
    render->clear_color[1] = 0.25f;
    render->clear_color[2] = 0.30f;
    render->clear_color[3] = 1.0f;

    replay = replay_state_mut();
    replay->active = 1;
    replay->state = REPLAY_PAUSED;
    replay->pc = 9;
    replay->mode = REPLAY_MODE_POLYGON;
    replay->speed = 12.5f;
    replay->accum = 0.75f;
    replay->fade_speed = 6.0f;
    replay->src_line_idx = 7;
    replay->total_flat_cmds = 13;
    replay->expand_args = 0;

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
    static GlrState glr_snap;
    static GlrState glr_round_trip;
    static GlrCameraState camera_snap;
    static GlrCameraState camera_round_trip;
    static EditorState editor_snap;
    static EditorState editor_round_trip;
    static UiState ui_snap;
    static UiState ui_round_trip;
    static VariablePanelState varpanel_snap;
    static VariablePanelState varpanel_round_trip;
    static ReplReplayRuntimeState replay_snap;
    static ReplReplayRuntimeState replay_round_trip;
    static EditorHelpSession help_snap;
    static EditorHelpSession help_round_trip;
    static const char *scene_hint = "Captured Scene";
    int foo_idx;

    glr_app_reset_all();
    populate_runtime_snapshot_fixture(scene_hint);
    repl_state_capture(&snapshot);
    glr_state_capture(&glr_snap);
    glr_camera_capture(&camera_snap);
    editor_state_capture(&editor_snap);
    ui_state_capture(&ui_snap);
    variable_panel_state_capture(&varpanel_snap);
    replay_state_capture(&replay_snap);
    editor_help_session_capture(&help_snap);
    glr_app_reset_all();
    repl_state_restore(&snapshot);
    glr_state_restore(&glr_snap);
    glr_camera_restore(&camera_snap);
    editor_state_restore(&editor_snap);
    ui_state_restore(&ui_snap);
    variable_panel_state_restore(&varpanel_snap);
    replay_state_restore(&replay_snap);
    editor_help_session_restore(&help_snap);
    repl_state_capture(&round_trip);
    glr_state_capture(&glr_round_trip);
    glr_camera_capture(&camera_round_trip);
    editor_state_capture(&editor_round_trip);
    ui_state_capture(&ui_round_trip);
    variable_panel_state_capture(&varpanel_round_trip);
    replay_state_capture(&replay_round_trip);
    editor_help_session_capture(&help_round_trip);

    ASSERT_STR("input restored",
               editor_input_text(),
               "glVertex3f(1, 2, 3)");
    ASSERT_INT("cursor restored", editor_cursor_pos(), 6);
    ASSERT_INT("document count restored", repl_state_document_count(), 2);
    ASSERT_INT("edit line restored", editor_state_edit_line(), 1);
    ASSERT_INT("document cmd type restored", repl_state_document_cmds()[0].type, CMD_BEGIN);
    ASSERT_STR("document cmd source restored",
               editor_buffer_line(0),
               "  glBegin(GL_TRIANGLES);");
    ASSERT_INT("flat count restored", repl_state_flat_program_count(), 1);
    ASSERT_INT("flat cmd type restored", repl_state_flat_program_cmds()[0].type, CMD_VERTEX3F);
    ASSERT_INT("flat local var count restored",
               repl_state_flat_program_local_vars_mut()[0].num_vars, 1);
    ASSERT_INT("flat lighting restored",
               repl_state_flat_program_user_lighting_enabled(), 1);
    ASSERT_INT("selection anchor restored", editor_state_selection().anchor_idx, 4);
    ASSERT_INT("selection end restored", editor_state_selection().end_idx, 7);
    ASSERT_INT("clipboard count restored", editor_state_clipboard().line_count, 1);
    ASSERT_TRUE("clipboard line restored",
                strcmp(editor_state_clipboard().lines[0], "glColor3f(1, 0, 0);") == 0);
    ASSERT_INT("help restored", ui_state_help().visible, 1);
    ASSERT_INT("help tab restored", editor_help_session_tab_idx(), 1);
    ASSERT_INT("help scroll restored", editor_help_session_scroll(), 3);
    ASSERT_TRUE("code panel frac restored", ui_state_code_panel().panel_frac == 0.61f);
    ASSERT_INT("code panel resizing restored", ui_state_code_panel().resizing_panel, 1);
    ASSERT_INT("code panel scroll restored", editor_scroll(), 9);
    ASSERT_INT("code panel follow restored",
               editor_scroll_follow_cursor(), 1);
    ASSERT_INT("code panel cursor visible restored",
               ui_state_code_panel().cursor_visible, 0);
    ASSERT_INT("code panel blink restored", ui_state_code_panel().blink_tick, 12);
    ASSERT_INT("variable panel restored", variable_panel_view().visible, 0);
    ASSERT_INT("variable drag idx restored", variable_panel_drag().var_idx, 3);
    ASSERT_INT("variable drag log restored", variable_panel_drag().log_mode, 1);
    ASSERT_TRUE("variable drag value restored",
                variable_panel_drag().start_value == 2.5f);
    ASSERT_INT("variable drag x restored", variable_panel_drag().start_x, 17);
    ASSERT_INT("profile panel restored",
               ui_state_profile_panel().mode, PROFILE_PANEL_DETAILS);
    {
        UiStatusState status = ui_state_status();
        EditorSearchState search = editor_state_search();
        EditorAutocompleteState autocomplete = editor_state_autocomplete();

        ASSERT_STR("status text restored", status.text, "state snapshot");
        ASSERT_INT("status ttl restored", status.ttl, 17);
        ASSERT_INT("search active restored", search.active, 1);
        ASSERT_STR("search query restored", search.query, "vertex");
        ASSERT_INT("search cursor restored", search.cursor_pos, 2);
        ASSERT_INT("search hit line restored", search.hit_line_idx, 5);
        ASSERT_INT("search hit char restored", search.hit_char_idx, 8);
        ASSERT_INT("search ordinal restored", search.hit_ordinal, 2);
        ASSERT_INT("search count restored", search.match_count, 4);
        ASSERT_INT("autocomplete count restored", autocomplete.match_count, 2);
        ASSERT_INT("autocomplete selection restored",
                   autocomplete.selected_idx, 1);
        ASSERT_STR("autocomplete ghost restored", autocomplete.ghost, "ghost text");
        ASSERT_STR("autocomplete hint restored", autocomplete.hint, "hint text");
        ASSERT_STR("autocomplete match restored",
                   autocomplete.matches[0], "glVertex3f");
        ASSERT_STR("autocomplete insert restored",
                   autocomplete.insert_matches[1], "glVertex2f(");
    }
    ASSERT_INT("time playing restored", repl_state_variables().time_playing, 0);
    ASSERT_TRUE("anim time restored", repl_state_variables().anim_time == 1.25f);
    ASSERT_TRUE("camera rx restored", glr_camera().rx == 11.0f);
    ASSERT_TRUE("camera ry restored", glr_camera().ry == 22.0f);
    ASSERT_TRUE("camera dist restored", glr_camera().dist == 7.5f);
    ASSERT_TRUE("camera tx restored", glr_camera().tx == 0.5f);
    ASSERT_TRUE("camera ty restored", glr_camera().ty == -0.25f);
    ASSERT_TRUE("camera tz restored", glr_camera().tz == 1.75f);
    ASSERT_TRUE("camera glow restored", glr_camera().motion_glow == 0.9f);
    ASSERT_INT("camera rotate restored", glr_camera().auto_rotate, 1);
    ASSERT_INT("pointer x restored", ui_state_pointer().mouse_x, 321);
    ASSERT_INT("pointer y restored", ui_state_pointer().mouse_y, 654);
    ASSERT_INT("pointer button restored", ui_state_pointer().mouse_button, 2);
    ASSERT_INT("viewport width restored", ui_state_viewport().window_w, 1440);
    ASSERT_INT("viewport height restored", ui_state_viewport().window_h, 900);
    ASSERT_INT("presentation wireframe restored", glr_state_presentation().wireframe, 1);
    ASSERT_INT("presentation grid restored", glr_state_presentation().grid_theme, GRID_THEME_TRON);
    ASSERT_INT("presentation layout restored",
               glr_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_BOTTOM);
    /* focus_vertex storage was deleted in step 7a; per-frame compute
     * lives on glr_ctrl. */
    ASSERT_INT("render use accum restored", glr_state_render().use_accum, 0);
    ASSERT_INT("render accum aa restored", glr_state_render().accum_aa_enabled, 0);
    ASSERT_INT("render samples restored", glr_state_render().accum_samples, 8);
    ASSERT_TRUE("render jitter restored", glr_state_render().accum_jitter_x == 0.25f);
    ASSERT_INT("render light enabled restored", repl_state_render().lights[1].enabled, 0);
    ASSERT_TRUE("render clear color restored",
                repl_state_render().clear_color[2] == 0.30f);
    ASSERT_INT("replay active restored", replay_state_view().active, 1);
    ASSERT_INT("replay state restored", replay_state_view().state, REPLAY_PAUSED);
    ASSERT_INT("replay mode restored", replay_state_view().mode, REPLAY_MODE_POLYGON);
    ASSERT_INT("replay pc restored", replay_state_view().pc, 9);
    ASSERT_TRUE("replay speed restored", replay_state_view().speed == 12.5f);
    ASSERT_INT("replay src line restored", replay_state_view().src_line_idx, 7);
    ASSERT_INT("replay expand restored", replay_state_view().expand_args, 0);
    ASSERT_INT("active example restored", repl_state_scenes().active_example_idx, 3);
    ASSERT_STR("workspace restored",
               repl_state_workspace_dir(),
               "/tmp/repl-state-stage1");
    ASSERT_STR("pending scene restored",
               repl_state_import_export().pending_scene_name,
               "pending scene");
    ASSERT_STR("pending workspace restored",
               repl_state_import_export().pending_workspace_dir,
               "/tmp/pending-workspace");
    ASSERT_STR("scene hint restored",
               repl_state_import_export().export_scene_name_hint,
               scene_hint);
    ASSERT_TRUE("workspace header count restored",
                repl_state_import_export().workspace_header_line_count >= 3);
    ASSERT_STR("workspace header banner restored",
               repl_state_import_export().workspace_header_lines[0],
               "// @workspace: REPL state (auto-saved)");
    ASSERT_STR("workspace header scene restored",
               repl_state_import_export().workspace_header_lines[1],
               "// @scene-name Captured Scene");
    ASSERT_STR("workspace header dir restored",
               repl_state_import_export().workspace_header_lines[2],
               "// @workspace-dir /tmp/repl-state-stage1");

    foo_idx = repl_eval_find_predef_var_idx("foo");
    ASSERT_TRUE("foo var restored", foo_idx >= 0);
    if (foo_idx >= 0)
        ASSERT_TRUE("foo value restored", g_predef_vars[foo_idx].value == 42.0f);
    ASSERT_TRUE("runtime snapshot round trip",
                memcmp(&snapshot, &round_trip, sizeof(snapshot)) == 0);
    ASSERT_TRUE("glr snapshot round trip",
                memcmp(&glr_snap, &glr_round_trip, sizeof(glr_snap)) == 0);
    ASSERT_TRUE("camera snapshot round trip",
                memcmp(&camera_snap, &camera_round_trip, sizeof(camera_snap)) == 0);
    ASSERT_TRUE("editor snapshot round trip",
                memcmp(&editor_snap, &editor_round_trip, sizeof(editor_snap)) == 0);
    ASSERT_TRUE("ui snapshot round trip",
                memcmp(&ui_snap, &ui_round_trip, sizeof(ui_snap)) == 0);
}

static void test_reset_all_restores_default_runtime(void) {
    static ReplRuntimeState defaults;
    static ReplRuntimeState reset_state;

    glr_app_reset_all();
    repl_state_capture(&defaults);

    populate_runtime_snapshot_fixture("Reset Scene");
    glr_app_reset_all();
    repl_state_capture(&reset_state);

    ASSERT_TRUE("reset_all restores default snapshot",
                memcmp(&defaults, &reset_state, sizeof(defaults)) == 0);
    ASSERT_INT("reset_all help hidden", ui_state_help().visible, 0);
    ASSERT_INT("reset_all panel scroll", editor_scroll(), 0);
    ASSERT_INT("reset_all replay mode", replay_state_view().mode, REPLAY_MODE_VERTEX);
    ASSERT_INT("reset_all replay expand", replay_state_view().expand_args, 1);
    ASSERT_STR("reset_all workspace dir", repl_state_workspace_dir(), "");
    ASSERT_INT("reset_all workspace header count",
               repl_state_import_export().workspace_header_line_count,
               defaults.import_export.workspace_header_line_count);
    ASSERT_STR("reset_all workspace header banner",
               repl_state_import_export().workspace_header_lines[0],
               "// @workspace: REPL state (auto-saved)");
}

/* Regression (feedback P3): the per-line override list cap must
 * cover the full document size — otherwise layout (which reads the
 * snapshot list) and render (formerly recomputing live) drift past
 * the cap, breaking wrap-row counts / scroll / hit-testing.
 *
 * Push 600 distinct overrides; verify all 600 are retrievable. The
 * pre-fix cap of 512 silently dropped entries 512..599; this test
 * fails on that build (override_for(513) returns NULL) and passes
 * after MAX_LINE_OVERRIDES bumps to MAX_COMMANDS. */
static void test_line_override_cap_covers_busy_replay(void) {
    const int N = 600;
    char buf[64];
    int append_failures = 0;

    editor_state_line_overrides_clear();
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof(buf), "// override-row %d", i);
        if (!editor_state_line_overrides_append(i, buf))
            append_failures++;
    }
    ASSERT_INT("override list: all 600 appends succeed",
               append_failures, 0);
    ASSERT_INT("override list: count tracks total",
               editor_state_line_overrides()->count, N);

    /* Past the old 512 cap. Pre-fix this returns NULL because the
     * append at 513 silently failed; post-fix the cap covers the
     * whole document so the lookup succeeds. Guard against NULL
     * before strcmp so the test fails cleanly on pre-fix instead
     * of segfaulting in ASSERT_STR. */
    {
        const char *got = editor_state_line_override_for(513);
        snprintf(buf, sizeof(buf), "// override-row %d", 513);
        ASSERT_TRUE("override-for(513) returns the pushed text past old cap",
                    got != NULL);
        ASSERT_STR("override-for(513) text matches",
                   got ? got : "<null>", buf);
    }

    /* Sanity: lookups for an entry that was never pushed return NULL. */
    ASSERT_TRUE("override-for(out-of-range) returns NULL",
                editor_state_line_override_for(N + 50) == NULL);

    editor_state_line_overrides_clear();
}

int main(void) {
    printf("--- repl_state tests ---\n");
    test_capture_restore_round_trip();
    test_reset_all_restores_default_runtime();
    test_line_override_cap_covers_busy_replay();
    printf("%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return g_harness.passed == g_harness.run ? 0 : 1;
}
