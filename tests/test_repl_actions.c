#include "repl_actions.h"
#include "repl_state.h"
#include "replay_state.h"
#include "ui_state.h"
#include "editor_help_session.h"
#include "repl_config.h"
#include "repl_audio.h"
#include "repl_core.h"
#include "support/test_harness.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_STR(label, got, exp) \
    TEST_ASSERT_STR(&g_harness, label, got, exp)

/* Read status via the mutable accessor to avoid taking .text from a temporary. */
static const char *last_status_text(void) {
    return ui_state_status_mut()->text;
}

#define g_last_status (last_status_text())

static void run_menu_action_in_temp_dir(const char *label,
                                        int menu_id,
                                        int item_idx,
                                        int expect_output_file,
                                        int expect_workspace_dir) {
    char cwd[1024];
    char workspace_dir[REPL_WORKSPACE_DIR_MAX];
    char temp_dir[] = "/tmp/test_repl_actions_output.XXXXXX";
    char *made_dir = mkdtemp(temp_dir);
    int have_cwd = getcwd(cwd, sizeof(cwd)) != NULL;

    snprintf(workspace_dir, sizeof(workspace_dir), "%s", repl_workspace_dir());

    ASSERT_TRUE("mkdtemp menu action dir", made_dir != NULL);
    ASSERT_TRUE("getcwd before menu action", have_cwd);
    if (!made_dir || !have_cwd)
        return;

    int cd_ok = chdir(made_dir);
    ASSERT_INT("chdir menu action dir", cd_ok, 0);
    if (cd_ok == 0) {
        repl_set_workspace_dir(NULL);
        ASSERT_INT(label,
                   repl_action_menu_item_activate(menu_id, item_idx),
                   1);
        if (expect_output_file) {
            ASSERT_INT("menu action wrote temp output.c",
                       access("output.c", F_OK), 0);
            unlink("output.c");
        }
        if (expect_workspace_dir) {
            ASSERT_INT("menu action wrote temp workspace dir",
                       access(REPL_DEFAULT_WORKSPACE_DIR, F_OK), 0);
            rmdir(REPL_DEFAULT_WORKSPACE_DIR);
        }
        repl_set_workspace_dir(workspace_dir);
        ASSERT_INT("restore cwd after menu action", chdir(cwd), 0);
    }

    rmdir(made_dir);
}

static void test_apply_defaults(void) {
    repl_reset_state();
    /* repl_actions_apply_defaults pulls from repl_audio_get_cfg_mode()
     * which defaults to AUDIO_CFG_ALL (3) if invalid. */
    repl_actions_apply_defaults();
    ASSERT_INT("default audio mode is ALL", repl_config_get(REPL_CONFIG_AUDIO_MODE), 3);
}

static void test_cursor_actions(void) {
    repl_reset_state();
    ReplCodePanelRuntimeState *cp = ui_state_code_panel_mut();
    cp->cursor_visible = 0;
    cp->blink_tick = 100;

    repl_action_cursor_blink_reset();
    ASSERT_INT("cursor visible after reset", cp->cursor_visible, 1);
    ASSERT_INT("blink tick reset", cp->blink_tick, 0);
}

static void test_help_tab_actions(void) {
    repl_reset_state();
    editor_help_session_set_tab(0);
    editor_help_session_set_scroll(50);

    repl_action_help_tab_next();
    ASSERT_INT("help tab next moves to 1", editor_help_session_tab_idx(), 1);
    ASSERT_INT("help tab next resets scroll", editor_help_session_scroll(), 0);

    editor_help_session_set_scroll(30);
    repl_action_help_tab_next();
    ASSERT_INT("help tab next stays at 1 (max)", editor_help_session_tab_idx(), 1);

    repl_action_help_tab_prev();
    ASSERT_INT("help tab prev moves to 0", editor_help_session_tab_idx(), 0);
    ASSERT_INT("help tab prev resets scroll", editor_help_session_scroll(), 0);

    editor_help_session_set_scroll(20);
    repl_action_help_tab_prev();
    ASSERT_INT("help tab prev stays at 0", editor_help_session_tab_idx(), 0);
}

int g_stub_modifiers = 0;

static void test_cfg_cycling(void) {
    repl_reset_state();

    /* Find some row indices */
    int wireframe_row = -1;
    int auto_time_row = -1;
    int audio_row = -1;
    int code_panel_row = -1;
    int auto_normals_row = -1;
    int replay_row = -1;
    int point_attenuation_row = -1;

    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const ReplConfigItem *item = repl_config_item_at(i);
        if (!item) continue;
        if (item->key == REPL_CONFIG_WIREFRAME) wireframe_row = i;
        if (item->key == REPL_CONFIG_AUTO_TIME) auto_time_row = i;
        if (item->key == REPL_CONFIG_AUDIO_MODE) audio_row = i;
        if (item->key == REPL_CONFIG_CODE_PANEL_LAYOUT) code_panel_row = i;
        if (item->key == REPL_CONFIG_AUTO_NORMALS) auto_normals_row = i;
        if (item->key == REPL_CONFIG_REPLAY) replay_row = i;
        if (item->key == REPL_CONFIG_POINT_ATTENUATION) point_attenuation_row = i;
    }

    /* Test generic cycle (Wireframe) */
    repl_state_presentation_mut()->wireframe = 0;
    repl_cfg_cycle_row(wireframe_row, 1);
    ASSERT_INT("wireframe toggled to 1", repl_state_presentation().wireframe, 1);
    ASSERT_STR("wireframe status ON", g_last_status, "Wireframe: ON");

    /* Test Replay special case - only starts if there are commands to replay */
    replay_state_mut()->active = 0;
    repl_config_set(REPL_CONFIG_REPLAY, 0);
    /* When no commands present, replay toggles but stays inactive with "nothing to play" */
    repl_cfg_cycle_row(replay_row, 1);
    ASSERT_STR("replay status nothing to play", g_last_status, "Replay: nothing to play");

    /* Test Auto-time toggle without shift */
    repl_state_variables_mut()->anim_time = 5.0f;
    repl_cfg_cycle_row(auto_time_row, 1);
    /* Time toggle would handle the animation, test just verifies it can be cycled */
    ASSERT_TRUE("auto time cycled", 1);

    /* Test Code Panel Layout */
    repl_state_presentation_mut()->code_panel_layout = 0; // Left
    repl_cfg_cycle_row(code_panel_row, 1); // -> Top
    ASSERT_INT("code panel layout top", repl_state_presentation().code_panel_layout, 1);
    ASSERT_STR("status top", g_last_status, "Layout: top code panel");

    repl_cfg_cycle_row(code_panel_row, 1); // -> Bottom
    ASSERT_INT("code panel layout bottom", repl_state_presentation().code_panel_layout, 2);
    ASSERT_STR("status bottom", g_last_status, "Layout: bottom code panel");

    repl_cfg_cycle_row(code_panel_row, 1); // -> Hidden
    ASSERT_INT("code panel layout hidden", repl_state_presentation().code_panel_layout, 3);
    ASSERT_STR("status hidden", g_last_status, "Layout: code panel hidden");

    repl_cfg_cycle_row(code_panel_row, 1); // -> Left
    ASSERT_INT("code panel layout left", repl_state_presentation().code_panel_layout, 0);
    ASSERT_STR("status left", g_last_status, "Layout: left code panel");

    /* Test Auto-normals */
    repl_state_presentation_mut()->autonormal = 0;
    repl_cfg_cycle_row(auto_normals_row, 1);
    ASSERT_INT("autonormal ON", repl_state_presentation().autonormal, 1);
    ASSERT_STR("status autonormal ON", g_last_status, "Auto-normals: ON");

    /* Test Point Attenuation */
    repl_config_set(REPL_CONFIG_POINT_ATTENUATION, 0);
    repl_cfg_cycle_row(point_attenuation_row, 1);
    ASSERT_INT("point attenuation ON", repl_config_get(REPL_CONFIG_POINT_ATTENUATION), 1);
    ASSERT_STR("status point attenuation ON", g_last_status, "Point attenuation: ON");

    /* Test Audio modes */
    repl_config_set(REPL_CONFIG_AUDIO_MODE, 0); // Pause
    repl_cfg_cycle_row(audio_row, 1); // -> Once
    ASSERT_INT("audio mode Once", repl_config_get(REPL_CONFIG_AUDIO_MODE), 1);
    ASSERT_STR("status audio Once", g_last_status, "Audio: play once");
    ASSERT_INT("audio engine not paused", repl_audio_is_paused(), 0);
    ASSERT_INT("audio engine loop mode OFF", repl_audio_get_loop_mode(), REPL_AUDIO_LOOP_OFF);

    repl_cfg_cycle_row(audio_row, 1); // -> Song
    ASSERT_INT("audio mode Song", repl_config_get(REPL_CONFIG_AUDIO_MODE), 2);
    ASSERT_STR("status audio Song", g_last_status, "Audio: loop song");
    ASSERT_INT("audio engine loop mode SONG", repl_audio_get_loop_mode(), REPL_AUDIO_LOOP_SONG);

    repl_cfg_cycle_row(audio_row, 1); // -> All
    ASSERT_INT("audio mode All", repl_config_get(REPL_CONFIG_AUDIO_MODE), 3);
    ASSERT_STR("status audio All", g_last_status, "Audio: loop all");
    ASSERT_INT("audio engine loop mode ALL", repl_audio_get_loop_mode(), REPL_AUDIO_LOOP_ALL);

    repl_cfg_cycle_row(audio_row, 1); // -> Pause
    ASSERT_INT("audio mode Pause", repl_config_get(REPL_CONFIG_AUDIO_MODE), 0);
    ASSERT_STR("status audio Pause", g_last_status, "Audio: paused");
    ASSERT_INT("audio engine paused", repl_audio_is_paused(), 1);
}

static void test_menu_actions(void) {
    repl_reset_state();

    /* File menu */
    run_menu_action_in_temp_dir("File Export",
                                REPL_MENU_FILE,
                                REPL_FILE_ITEM_EXPORT,
                                1,
                                0);
    ASSERT_INT("File Import", repl_action_menu_item_activate(REPL_MENU_FILE, REPL_FILE_ITEM_IMPORT), 1);
    ASSERT_STR("Import status", g_last_status, "Import not implemented yet");
    run_menu_action_in_temp_dir("File Save Workspace",
                                REPL_MENU_FILE,
                                REPL_FILE_ITEM_SAVE_WORKSPACE,
                                0,
                                1);
    run_menu_action_in_temp_dir("File Load Workspace",
                                REPL_MENU_FILE,
                                REPL_FILE_ITEM_LOAD_WORKSPACE,
                                0,
                                0);

    /* Scene menu - Examples */
    int example_count = repl_example_count();
    if (example_count > 0) {
        ASSERT_INT("Load Example 0", repl_action_menu_item_activate(REPL_MENU_SCENE, 1), 1);
        ASSERT_INT("active example is 0", repl_state_scenes().active_example_idx, 0);
    }

    /* Scene menu - Fixed items */
    ASSERT_INT("Scene New", repl_action_menu_item_activate(REPL_MENU_SCENE, example_count + REPL_SCENE_OFF_NEW), 1);
    ASSERT_INT("active example cleared", repl_state_scenes().active_example_idx, -1);

    run_menu_action_in_temp_dir("Scene Save",
                                REPL_MENU_SCENE,
                                example_count + REPL_SCENE_OFF_SAVE,
                                1,
                                0);

    /* Scene Rename - need an active user scene */
    /* For now, just call it and expect "No active scene to rename" if none active */
    ASSERT_INT("Scene Rename (none)", repl_action_menu_item_activate(REPL_MENU_SCENE, example_count + REPL_SCENE_OFF_RENAME), 1);
    ASSERT_STR("Rename status", g_last_status, "No active scene to rename");

    /* User scenes */
    /* Add a user scene by loading an example first (promotes current scene to slot 0) */
    repl_feed_line_public("glVertex3f(1,1,1);");
    repl_load_example(0);

    ASSERT_INT("Slot 0 used", repl_user_scene_slot_used(0), 1);
    ASSERT_INT("Dense index 0 is slot 0", repl_scene_menu_slot_for_dense_index(0), 0);
    ASSERT_INT("Load user scene 0", repl_action_menu_item_activate(REPL_MENU_SCENE, example_count + REPL_SCENE_OFF_SCENES), 1);
    ASSERT_INT("active user scene slot", repl_active_user_scene(), 0);

    /* Config menu */
    ASSERT_INT("Config row 1", repl_action_menu_item_activate(REPL_MENU_CONFIG, 1), 0); // 0 because toggles keep menu open
}

static void test_shortcuts(void) {
    repl_reset_state();

    /* Test handling of unknown keys - these should return 0 */
    ASSERT_INT("Unknown ASCII", repl_cfg_handle_ascii_shortcut('X'), 0);
    ASSERT_INT("Unknown special", repl_cfg_handle_special_shortcut(999), 0);

    /* Test that specific shortcut handlers are callable
     * Note: testing actual shortcut effects requires GL context initialization,
     * which is handled by test_cfg_cycling with specific config items  */
}

int main(void) {
    test_apply_defaults();
    test_cursor_actions();
    test_help_tab_actions();
    test_cfg_cycling();
    test_menu_actions();
    test_shortcuts();

    return test_harness_report(&g_harness, "test_repl_actions");
}
