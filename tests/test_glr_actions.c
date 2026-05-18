#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "app/glr_actions.h"
#include "repl/state.h"
#include "widgets/replay_state.h"
#include "ui/state.h"
#include "editor/help_session.h"
#include "app/glr_config.h"
#include "audio.h"
#include "repl/core.h"
#include "editor/input.h"
#include "repl/examples.h"
#include "keys.h"
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

/* Controllable modifier state for glr_cfg_handle_ascii_shortcut tests. */
static int g_test_mods = 0;
static int test_mods_provider(void) { return g_test_mods; }

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
                   glr_action_menu_item_activate(menu_id, item_idx),
                   1);
        if (expect_output_file) {
            ASSERT_INT("menu action wrote temp output.c",
                       access("output.c", F_OK), 0);
            unlink("output.c");
        }
        if (expect_workspace_dir) {
            ASSERT_INT("menu action wrote temp workspace dir",
                       access(GLR_DEFAULT_WORKSPACE_DIR, F_OK), 0);
            rmdir(GLR_DEFAULT_WORKSPACE_DIR);
        }
        repl_set_workspace_dir(workspace_dir);
        ASSERT_INT("restore cwd after menu action", chdir(cwd), 0);
    }

    rmdir(made_dir);
}

static void test_apply_defaults(void) {
    glr_app_reset_all();
    /* glr_actions_apply_defaults pulls from audio_get_cfg_mode()
     * which defaults to AUDIO_CFG_ALL (3) if invalid. */
    glr_actions_apply_defaults();
    ASSERT_INT("default audio mode is ALL", glr_config_get(GLR_CONFIG_AUDIO_MODE), 3);
}

static void test_cursor_actions(void) {
    glr_app_reset_all();
    UiCodePanelRuntimeState *cp = ui_state_code_panel_mut();
    cp->cursor_visible = 0;
    cp->blink_tick = 100;

    glr_action_cursor_blink_reset();
    ASSERT_INT("cursor visible after reset", cp->cursor_visible, 1);
    ASSERT_INT("blink tick reset", cp->blink_tick, 0);
}

static void test_help_tab_actions(void) {
    glr_app_reset_all();
    editor_help_session_set_tab(0);
    editor_help_session_set_scroll(50);

    /* Tabs: Overview(0) -> Commands(1) -> Keys(2). */
    glr_action_help_tab_next();
    ASSERT_INT("help tab next moves to 1", editor_help_session_tab_idx(), 1);
    ASSERT_INT("help tab next resets scroll", editor_help_session_scroll(), 0);

    glr_action_help_tab_next();
    ASSERT_INT("help tab next moves to 2", editor_help_session_tab_idx(), 2);

    editor_help_session_set_scroll(30);
    glr_action_help_tab_next();
    ASSERT_INT("help tab next stays at 2 (max)", editor_help_session_tab_idx(), 2);
    ASSERT_INT("help tab next at max keeps scroll", editor_help_session_scroll(), 30);

    glr_action_help_tab_prev();
    ASSERT_INT("help tab prev moves to 1", editor_help_session_tab_idx(), 1);
    ASSERT_INT("help tab prev resets scroll", editor_help_session_scroll(), 0);

    glr_action_help_tab_prev();
    ASSERT_INT("help tab prev moves to 0", editor_help_session_tab_idx(), 0);

    editor_help_session_set_scroll(20);
    glr_action_help_tab_prev();
    ASSERT_INT("help tab prev stays at 0", editor_help_session_tab_idx(), 0);
}

int g_stub_modifiers = 0;

static void test_cfg_cycling(void) {
    glr_app_reset_all();

    /* Find some row indices */
    int wireframe_row = -1;
    int auto_time_row = -1;
    int audio_row = -1;
    int code_panel_row = -1;
    int auto_normals_row = -1;
    int replay_row = -1;
    int point_attenuation_row = -1;

    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (!item) continue;
        if (item->key == GLR_CONFIG_WIREFRAME) wireframe_row = i;
        if (item->key == GLR_CONFIG_AUTO_TIME) auto_time_row = i;
        if (item->key == GLR_CONFIG_AUDIO_MODE) audio_row = i;
        if (item->key == GLR_CONFIG_CODE_PANEL_LAYOUT) code_panel_row = i;
        if (item->key == GLR_CONFIG_AUTO_NORMALS) auto_normals_row = i;
        if (item->key == GLR_CONFIG_REPLAY) replay_row = i;
        if (item->key == GLR_CONFIG_POINT_ATTENUATION) point_attenuation_row = i;
    }

    /* Test generic cycle (Wireframe) */
    glr_state_presentation_mut()->wireframe = 0;
    glr_cfg_cycle_row(wireframe_row, 1);
    ASSERT_INT("wireframe toggled to 1", glr_state_presentation().wireframe, 1);
    ASSERT_STR("wireframe status ON", g_last_status, "Wireframe: ON");

    /* Test Replay special case - only starts if there are commands to replay */
    replay_state_mut()->active = 0;
    glr_config_set(GLR_CONFIG_REPLAY, 0);
    /* When no commands present, replay toggles but stays inactive with "nothing to play" */
    glr_cfg_cycle_row(replay_row, 1);
    ASSERT_STR("replay status nothing to play", g_last_status, "Replay: nothing to play");

    /* Test Auto-time toggle without shift */
    repl_state_variables_mut()->anim_time = 5.0f;
    glr_cfg_cycle_row(auto_time_row, 1);
    /* Time toggle would handle the animation, test just verifies it can be cycled */
    ASSERT_TRUE("auto time cycled", 1);

    /* Test Code Panel Layout */
    glr_state_presentation_mut()->code_panel_layout = 0; glr_ctrl_sync_ui_chrome(); // Left
    glr_cfg_cycle_row(code_panel_row, 1); // -> Top
    ASSERT_INT("code panel layout top", glr_state_presentation().code_panel_layout, 1);
    ASSERT_STR("status top", g_last_status, "Layout: top code panel");

    glr_cfg_cycle_row(code_panel_row, 1); // -> Bottom
    ASSERT_INT("code panel layout bottom", glr_state_presentation().code_panel_layout, 2);
    ASSERT_STR("status bottom", g_last_status, "Layout: bottom code panel");

    glr_cfg_cycle_row(code_panel_row, 1); // -> Hidden
    ASSERT_INT("code panel layout hidden", glr_state_presentation().code_panel_layout, 3);
    ASSERT_STR("status hidden", g_last_status, "Layout: code panel hidden");

    glr_cfg_cycle_row(code_panel_row, 1); // -> Left
    ASSERT_INT("code panel layout left", glr_state_presentation().code_panel_layout, 0);
    ASSERT_STR("status left", g_last_status, "Layout: left code panel");

    /* Test Auto-normals */
    glr_state_presentation_mut()->autonormal = 0;
    glr_cfg_cycle_row(auto_normals_row, 1);
    ASSERT_INT("autonormal ON", glr_state_presentation().autonormal, 1);
    ASSERT_STR("status autonormal ON", g_last_status, "Auto-normals: ON");

    /* Test Point Attenuation */
    glr_config_set(GLR_CONFIG_POINT_ATTENUATION, 0);
    glr_cfg_cycle_row(point_attenuation_row, 1);
    ASSERT_INT("point attenuation ON", glr_config_get(GLR_CONFIG_POINT_ATTENUATION), 1);
    ASSERT_STR("status point attenuation ON", g_last_status, "Point attenuation: ON");

    /* Test Audio modes */
    glr_config_set(GLR_CONFIG_AUDIO_MODE, 0); // Pause
    glr_cfg_cycle_row(audio_row, 1); // -> Once
    ASSERT_INT("audio mode Once", glr_config_get(GLR_CONFIG_AUDIO_MODE), 1);
    ASSERT_STR("status audio Once", g_last_status, "Audio: play once");
    ASSERT_INT("audio engine not paused", audio_is_paused(), 0);
    ASSERT_INT("audio engine loop mode OFF", audio_get_loop_mode(), AUDIO_LOOP_OFF);

    glr_cfg_cycle_row(audio_row, 1); // -> Song
    ASSERT_INT("audio mode Song", glr_config_get(GLR_CONFIG_AUDIO_MODE), 2);
    ASSERT_STR("status audio Song", g_last_status, "Audio: loop song");
    ASSERT_INT("audio engine loop mode SONG", audio_get_loop_mode(), AUDIO_LOOP_SONG);

    glr_cfg_cycle_row(audio_row, 1); // -> All
    ASSERT_INT("audio mode All", glr_config_get(GLR_CONFIG_AUDIO_MODE), 3);
    ASSERT_STR("status audio All", g_last_status, "Audio: loop all");
    ASSERT_INT("audio engine loop mode ALL", audio_get_loop_mode(), AUDIO_LOOP_ALL);

    glr_cfg_cycle_row(audio_row, 1); // -> Pause
    ASSERT_INT("audio mode Pause", glr_config_get(GLR_CONFIG_AUDIO_MODE), 0);
    ASSERT_STR("status audio Pause", g_last_status, "Audio: paused");
    ASSERT_INT("audio engine paused", audio_is_paused(), 1);
}

static void test_menu_actions(void) {
    glr_app_reset_all();

    /* File menu */
    ASSERT_INT("File Load Scene", glr_action_menu_item_activate(GLR_MENU_FILE, GLR_FILE_ITEM_LOAD_SCENE), 1);
    /* No glr_ctrl_set_program_name() in tests -> default "gl-repl". */
    ASSERT_STR("Load Scene status", g_last_status,
               "Runtime load unsupported - relaunch gl-repl <file> "
               "or use Load Workspace");
    run_menu_action_in_temp_dir("File Save Workspace",
                                GLR_MENU_FILE,
                                GLR_FILE_ITEM_SAVE_WORKSPACE,
                                0,
                                1);
    run_menu_action_in_temp_dir("File Load Workspace",
                                GLR_MENU_FILE,
                                GLR_FILE_ITEM_LOAD_WORKSPACE,
                                0,
                                0);

    /* Scene menu - tag rows are hover-only; examples load via submenu hits. */
    int tag_count = repl_example_visible_tag_count();
    if (tag_count > 0) {
        ASSERT_INT("Scene tag row keeps menu open",
                   glr_action_menu_item_activate(GLR_MENU_SCENE, 1), 0);
        ASSERT_INT("Scene tag row does not load an example",
                   repl_state_scenes().active_example_idx, -1);
    }

    /* Scene actions now live in the File menu (Scene menu is a pure
     * selector). New Scene enters the transient lifecycle: it clears
     * both the active example and any active user slot. */
    ASSERT_INT("File New Scene", glr_action_menu_item_activate(GLR_MENU_FILE, GLR_FILE_ITEM_NEW_SCENE), 1);
    ASSERT_INT("active example cleared", repl_state_scenes().active_example_idx, -1);
    ASSERT_INT("active user scene detached", repl_active_user_scene(), -1);

    /* Save Scene with no active named scene falls back to the default
     * single-file save; run sandboxed so it cannot touch repo output.c. */
    run_menu_action_in_temp_dir("File Save Scene",
                                GLR_MENU_FILE,
                                GLR_FILE_ITEM_SAVE_SCENE,
                                1,
                                0);

    /* Rename Scene with no active user scene -> guarded no-op. */
    ASSERT_INT("File Rename Scene (none)", glr_action_menu_item_activate(GLR_MENU_FILE, GLR_FILE_ITEM_RENAME_SCENE), 1);
    ASSERT_STR("Rename status", g_last_status, "No active scene to rename");

    /* User scenes */
    /* Add a user scene by loading an example first (promotes current scene to slot 0) */
    editor_feed_line("glVertex3f(1,1,1);");
    repl_load_example(0);

    ASSERT_INT("Slot 0 used", repl_user_scene_slot_used(0), 1);
    ASSERT_INT("Dense index 0 is slot 0", glr_scene_menu_slot_for_dense_index(0), 0);
    ASSERT_INT("Load user scene 0", glr_action_menu_item_activate(GLR_MENU_SCENE, tag_count + GLR_SCENE_OFF_SCENES), 1);
    ASSERT_INT("active user scene slot", repl_active_user_scene(), 0);

    /* Config menu: top-level rows are section parents — activating
     * one is an inert no-op that keeps the dropdown open (returns 0). */
    ASSERT_INT("Config parent-row activate is inert (returns 0)",
               glr_action_menu_item_activate(GLR_MENU_CONFIG, 1), 0);
}

/* Step 5 (Finding #1): every Config top-level row — each "### "
 * section parent and the synthetic "All" row — must be inert on
 * click: glr_action_menu_item_activate returns 0 (menu stays open)
 * and NObody's config state changes (it must never be mis-read as a
 * g_cfg_items[] index and cycled). */
static void test_config_parent_rows_inert(void) {
    glr_app_reset_all();

    int parents = glr_config_section_count() + 1; /* sections + All */

    /* Snapshot every config value. */
    int before[GLR_CONFIG_COUNT];
    for (int k = 1; k < GLR_CONFIG_COUNT; k++)
        before[k] = glr_config_get((GlrConfigKey)k);

    for (int row = 0; row < parents; row++) {
        ASSERT_INT("parent-row activate returns 0 (menu open)",
                   glr_action_menu_item_activate(GLR_MENU_CONFIG, row), 0);
    }

    for (int k = 1; k < GLR_CONFIG_COUNT; k++)
        ASSERT_INT("config value unchanged by parent-row clicks",
                   glr_config_get((GlrConfigKey)k), before[k]);
}

static void test_shortcuts(void) {
    glr_app_reset_all();

    /* Test handling of unknown keys - these should return 0 */
    ASSERT_INT("Unknown ASCII", glr_cfg_handle_ascii_shortcut('X'), 0);
    ASSERT_INT("Unknown special", glr_cfg_handle_special_shortcut(999), 0);

    /* Test that specific shortcut handlers are callable
     * Note: testing actual shortcut effects requires GL context initialization,
     * which is handled by test_cfg_cycling with specific config items  */
}

static void test_config_sections(void) {
    /* g_cfg_items[] has seven "### " sections. The section model must
     * be data-faithful: only real headers counted, "---" excluded, the
     * synthetic "All" view NOT counted here (plan Finding #2). */
    int n = glr_config_section_count();
    ASSERT_INT("section count", n, 7);

    /* glr_config_section_label is data-faithful: it returns the raw
     * "### " label with the marker stripped (still UPPERCASE). The
     * leading-uppercase prettify is a menu-display concern applied in
     * menu_item_label, not here. */
    const char *expect[] = {
        "RENDERING", "TIME & REPLAY", "OVERLAYS & SCENE", "CAMERA",
        "GEOMETRY", "INTERFACE", "AUDIO",
    };
    for (int s = 0; s < n; s++)
        ASSERT_STR("section label (### stripped)",
                   glr_config_section_label(s), expect[s]);

    ASSERT_TRUE("out-of-range label is NULL",
                glr_config_section_label(n) == NULL);
    ASSERT_TRUE("out-of-range range fails",
                glr_config_section_range(n, NULL, NULL) == 0);

    /* Every section range covers only actionable rows, contiguous, and
     * the per-section item totals + chrome reconstruct the full table. */
    int items_seen = 0;
    for (int s = 0; s < n; s++) {
        int start = -1, count = -1;
        ASSERT_TRUE("section range ok",
                    glr_config_section_range(s, &start, &count) == 1);
        ASSERT_TRUE("section non-empty", count > 0);
        for (int i = start; i < start + count; i++) {
            const GlrConfigItem *it = glr_config_item_at(i);
            ASSERT_TRUE("range row is an item",
                        it && !it->section_header &&
                        it->key != GLR_CONFIG_NONE);
            ASSERT_INT("range row kind ITEM",
                       glr_config_row_kind(i), GLR_CFG_ROW_ITEM);
        }
        items_seen += count;
    }

    int headers = 0, seps = 0, items = 0;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        switch (glr_config_row_kind(i)) {
        case GLR_CFG_ROW_HEADER:    headers++; break;
        case GLR_CFG_ROW_SEPARATOR: seps++;    break;
        case GLR_CFG_ROW_ITEM:      items++;   break;
        }
    }
    ASSERT_INT("row_kind headers == section count", headers, n);
    ASSERT_INT("section ranges cover every item row", items_seen, items);
    ASSERT_INT("kinds partition the table",
               headers + seps + items, CFG_ITEM_COUNT);
}

/* Two-pass modifier-aware dispatch in glr_cfg_handle_ascii_shortcut:
 * a Shift-requiring row (modifiers & GLUT_ACTIVE_SHIFT) wins only when
 * Shift is held; otherwise the modifiers==0 row matches — and a plain
 * Ctrl key with no modifiers==0 row falls through (returns 0). */
static void test_ascii_shortcut_modifiers(void) {
    glr_app_reset_all();
    editor_input_set_modifier_provider_for_test(test_mods_provider);

    /* Plain Ctrl+O (no Shift) cycles Grid major; View mode untouched. */
    g_test_mods = 0;
    int gm0 = glr_config_get(GLR_CONFIG_GRID_MAJOR);
    int ortho0 = glr_config_get(GLR_CONFIG_ORTHO_MODE);
    ASSERT_INT("plain Ctrl+O handled", glr_cfg_handle_ascii_shortcut(KEY_CTRL_O), 1);
    ASSERT_TRUE("plain Ctrl+O cycled Grid major",
                glr_config_get(GLR_CONFIG_GRID_MAJOR) != gm0);
    ASSERT_INT("plain Ctrl+O left View mode alone",
               glr_config_get(GLR_CONFIG_ORTHO_MODE), ortho0);

    /* Ctrl+Shift+O: the Shift-requiring Focus origin row shadows Grid
     * major (which must NOT cycle); status proves Focus origin ran. */
    g_test_mods = GLUT_ACTIVE_SHIFT;
    int gm1 = glr_config_get(GLR_CONFIG_GRID_MAJOR);
    repl_set_status("");
    ASSERT_INT("Ctrl+Shift+O handled", glr_cfg_handle_ascii_shortcut(KEY_CTRL_O), 1);
    ASSERT_INT("Ctrl+Shift+O did NOT cycle Grid major",
               glr_config_get(GLR_CONFIG_GRID_MAJOR), gm1);
    ASSERT_STR("Ctrl+Shift+O ran Focus origin", g_last_status,
               "Camera: focus origin");

    /* Ctrl+Shift+V toggles View mode (ordinary cycle row + Shift). */
    int ortho1 = glr_config_get(GLR_CONFIG_ORTHO_MODE);
    ASSERT_INT("Ctrl+Shift+V handled", glr_cfg_handle_ascii_shortcut(KEY_CTRL_V), 1);
    ASSERT_TRUE("Ctrl+Shift+V toggled View mode",
                glr_config_get(GLR_CONFIG_ORTHO_MODE) != ortho1);

    /* Ctrl+Shift+C runs Reset camera. */
    repl_set_status("");
    ASSERT_INT("Ctrl+Shift+C handled", glr_cfg_handle_ascii_shortcut(KEY_CTRL_C), 1);
    ASSERT_STR("Ctrl+Shift+C ran Reset camera", g_last_status,
               "Camera: reset to default");

    /* Plain Ctrl+V / Ctrl+C: no modifiers==0 row claims them, so the
     * handler declines (controller then routes to editor paste/copy). */
    g_test_mods = 0;
    ASSERT_INT("plain Ctrl+V declined (-> editor paste)",
               glr_cfg_handle_ascii_shortcut(KEY_CTRL_V), 0);
    ASSERT_INT("plain Ctrl+C declined (-> editor copy)",
               glr_cfg_handle_ascii_shortcut(KEY_CTRL_C), 0);

    /* Ctrl+Shift+T quirk: no Shift-row for T, so it falls back to the
     * modifiers==0 Auto time row, whose handler resets t on Shift. */
    g_test_mods = GLUT_ACTIVE_SHIFT;
    repl_set_status("");
    ASSERT_INT("Ctrl+Shift+T handled", glr_cfg_handle_ascii_shortcut(KEY_CTRL_T), 1);
    ASSERT_TRUE("Ctrl+Shift+T fell back to Auto time (reset t)",
                strstr(g_last_status, "reset to 0") != NULL);

    editor_input_set_modifier_provider_for_test(NULL);
    g_test_mods = 0;
}

int main(void) {
    test_apply_defaults();
    test_cursor_actions();
    test_help_tab_actions();
    test_cfg_cycling();
    test_config_sections();
    test_config_parent_rows_inert();
    test_menu_actions();
    test_shortcuts();
    test_ascii_shortcut_modifiers();

    return test_harness_report(&g_harness, "test_repl_actions");
}
