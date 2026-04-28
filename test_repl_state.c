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

static void test_capture_restore_round_trip(void) {
    char err[128];
    static ReplRuntimeState snapshot;
    static const char *scene_hint = "Captured Scene";
    int foo_idx;
    ReplClipboardState *clipboard;
    ReplStatusState *status;
    ReplAutocompleteState *ac;
    ReplVariableDragState *drag;
    ReplImportExportState *io;
    ReplSceneRuntimeState *scenes;

    repl_state_init_defaults();
    repl_state_input_set_text("glVertex3f(1, 2, 3)");
    repl_state_cursor_pos_set(6);
    repl_state_document_count_set(2);
    repl_state_edit_line_set(1);
    repl_state_selection_set(4, 7);
    clipboard = repl_state_clipboard_mut();
    clipboard->cmd_count = 1;
    clipboard->cmds[0].type = CMD_COLOR3F;
    repl_state_help_mut()->visible = 1;
    repl_state_help_mut()->tab_idx = 1;
    repl_state_help_mut()->scroll = 3;
    *repl_state_code_panel_mut()->scroll = 9;
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
    repl_state_camera_set(11.0f, 22.0f, 7.5f, 0.5f, -0.25f, 1.75f, 0.9f);
    repl_state_camera_mut()->auto_rotate = 1;
    repl_state_pointer_set(321, 654, 2);
    repl_state_viewport_set_size(1440, 900);
    scenes = repl_state_scenes_mut();
    scenes->active_example_idx = 3;
    repl_state_workspace_set_dir("/tmp/repl-state-stage1");
    io = repl_state_import_export_mut();
    io->export_scene_name_hint = scene_hint;
    snprintf(io->pending_scene_name, sizeof(io->pending_scene_name), "%s",
             "pending scene");
    snprintf(io->pending_workspace_dir, sizeof(io->pending_workspace_dir), "%s",
             "/tmp/pending-workspace");

    repl_eval_declare_predef_var("foo", err, sizeof(err));
    foo_idx = repl_eval_find_predef_var_idx("foo");
    ASSERT_TRUE("foo var declared", foo_idx >= 0);
    if (foo_idx >= 0)
        g_predef_vars[foo_idx].value = 42.0f;

    repl_state_capture(&snapshot);

    repl_state_input_clear();
    repl_state_document_count_set(0);
    repl_state_edit_line_set(0);
    repl_state_selection_clear();
    repl_state_clipboard_clear();
    repl_state_help_mut()->visible = 0;
    repl_state_help_mut()->tab_idx = 0;
    repl_state_help_mut()->scroll = 0;
    *repl_state_code_panel_mut()->scroll = 0;
    repl_state_variable_panel_mut()->visible = 1;
    repl_state_variable_drag_reset();
    repl_state_profile_panel_mut()->mode = PROFILE_PANEL_OFF;
    repl_state_status_clear();
    repl_state_search_clear();
    repl_state_autocomplete_clear();
    *repl_state_variables_mut()->time_playing = 1;
    repl_state_camera_set(99.0f, 88.0f, 3.0f, 9.0f, 8.0f, 7.0f, 0.1f);
    repl_state_camera_mut()->auto_rotate = 0;
    repl_state_pointer_set(0, 0, -1);
    repl_state_viewport_set_size(800, 600);
    repl_state_scenes_mut()->active_example_idx = -1;
    repl_state_workspace_set_dir("/tmp/repl-state-mutated");
    repl_state_import_export_reset();
    if (foo_idx >= 0)
        g_predef_vars[foo_idx].value = -1.0f;

    repl_state_restore(&snapshot);

    ASSERT_STR("input restored",
               repl_state_input_text(),
               "glVertex3f(1, 2, 3)");
    ASSERT_INT("cursor restored", repl_state_cursor_pos(), 6);
    ASSERT_INT("document count restored", repl_state_document_count(), 2);
    ASSERT_INT("edit line restored", repl_state_edit_line(), 1);
    ASSERT_INT("selection anchor restored", repl_state_selection()->anchor_idx, 4);
    ASSERT_INT("selection end restored", repl_state_selection()->end_idx, 7);
    ASSERT_INT("clipboard count restored", repl_state_clipboard()->cmd_count, 1);
    ASSERT_INT("clipboard cmd restored", repl_state_clipboard()->cmds[0].type, CMD_COLOR3F);
    ASSERT_INT("help restored", repl_state_help()->visible, 1);
    ASSERT_INT("help tab restored", repl_state_help()->tab_idx, 1);
    ASSERT_INT("help scroll restored", repl_state_help()->scroll, 3);
    ASSERT_INT("code panel scroll restored", *repl_state_code_panel()->scroll, 9);
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
}

int main(void) {
    printf("--- repl_state tests ---\n");
    test_capture_restore_round_trip();
    printf("%d / %d tests passed\n", g_pass, g_run);
    return g_pass == g_run ? 0 : 1;
}
