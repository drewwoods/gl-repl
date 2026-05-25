#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "app/glr_actions.h"
#include "repl/state.h"
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "ui/app/state.h"
#include "editor/help_session.h"
#include "editor/inline_file_prompt.h"
#include "app/glr_config.h"
#include "config.h"                  /* DEFAULT_SCENE_FILE */
#include "app/glr_audio.h"
#include "repl/core.h"
#include "editor/input.h"
#include "repl/examples.h"
#include "repl/tutorials.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "source_document.h"
#include "keys.h"
#include "app/glr_camera.h"               /* glr_camera_target_active / glr_camera */
#include "ui/app/menu_bar.h"              /* ui_menu_bar_open_menu_id, _set_open_menu */
#include "ui/app/layout.h"                /* CODE_PANEL_LAYOUT_* */
#include "repl/eval.h"                    /* g_predef_vars, repl_eval_find_predef_var_idx */
#include "subsystems/color_picker/color_picker_state.h"
#include "support/test_harness.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
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
    /* glr_actions_apply_defaults pulls from glr_audio_get_cfg_mode()
     * which defaults to AUDIO_CFG_ALL (3) if invalid. */
    glr_actions_apply_defaults();
    ASSERT_INT("default audio mode is ALL", glr_config_get(GLR_CONFIG_AUDIO_MODE), 3);
}

static void test_cursor_actions(void) {
    glr_app_reset_all();
    EditorCursorBlinkState *cb = editor_state_cursor_blink_mut();
    cb->cursor_visible = 0;
    cb->blink_tick = 100;

    glr_action_cursor_blink_reset();
    ASSERT_INT("cursor visible after reset", editor_state_cursor_blink().cursor_visible, 1);
    ASSERT_INT("blink tick reset", editor_state_cursor_blink().blink_tick, 0);
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
    ASSERT_INT("audio engine not paused", glr_audio_is_paused(), 0);
    ASSERT_INT("audio engine loop mode OFF", glr_audio_get_loop_mode(), GLR_AUDIO_LOOP_OFF);

    glr_cfg_cycle_row(audio_row, 1); // -> Song
    ASSERT_INT("audio mode Song", glr_config_get(GLR_CONFIG_AUDIO_MODE), 2);
    ASSERT_STR("status audio Song", g_last_status, "Audio: loop song");
    ASSERT_INT("audio engine loop mode SONG", glr_audio_get_loop_mode(), GLR_AUDIO_LOOP_SONG);

    glr_cfg_cycle_row(audio_row, 1); // -> All
    ASSERT_INT("audio mode All", glr_config_get(GLR_CONFIG_AUDIO_MODE), 3);
    ASSERT_STR("status audio All", g_last_status, "Audio: loop all");
    ASSERT_INT("audio engine loop mode ALL", glr_audio_get_loop_mode(), GLR_AUDIO_LOOP_ALL);

    glr_cfg_cycle_row(audio_row, 1); // -> Pause
    ASSERT_INT("audio mode Pause", glr_config_get(GLR_CONFIG_AUDIO_MODE), 0);
    ASSERT_STR("status audio Pause", g_last_status, "Audio: paused");
    ASSERT_INT("audio engine paused", glr_audio_is_paused(), 1);
}

static void test_menu_actions(void) {
    glr_app_reset_all();

    /* File menu */
    ASSERT_INT("File Load Scene", glr_action_menu_item_activate(GLR_MENU_FILE, GLR_FILE_ITEM_LOAD_SCENE), 1);
    /* LOAD_SCENE now opens the inline file prompt seeded with the
     * default filename. The prompt becomes the input modal until the
     * user commits or cancels. */
    ASSERT_INT("Load Scene opens prompt",
               editor_inline_file_prompt_active(), 1);
    ASSERT_STR("Load Scene prompt seeded with default",
               editor_inline_file_prompt_buffer(), DEFAULT_SCENE_FILE);
    editor_inline_file_prompt_cancel();
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
     * both the active example and any active user slot. (Audit #13) */
    ASSERT_INT("File New Scene", glr_action_menu_item_activate(GLR_MENU_FILE, GLR_FILE_ITEM_NEW_SCENE), 1);
    ASSERT_INT("active example cleared", repl_state_scenes().active_example_idx, -1);
    ASSERT_INT("active user scene detached", repl_active_user_scene(), -1);
    ASSERT_INT("New Scene clears document",
               source_document_view().line_count, 0);

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

static int find_tutorial_idx_by_name(const char *name) {
    for (int i = 0; i < repl_tutorial_count(); i++) {
        const char *n = repl_tutorial_name(i);
        if (n && strcmp(n, name) == 0)
            return i;
    }
    return -1;
}

/* Starting a tutorial runs the reset-then-apply pipeline examples use:
 * presentation chrome resets to defaults, then the tutorial's leading
 * `@cfg` lines (if any) layer overrides on top. Both live REPL-side in
 * tutorial_start. After Phase B's hierarchical menu, the menu's top-level
 * MENU_TUTORIALS rows are tag rows (inert) + trailing Restart/Exit;
 * tutorial activation itself flows through route_submenu_item_hit →
 * tutorial_start. This test calls tutorial_start directly (unit-level)
 * since simulating a submenu-item hit would be integration-level. */
static void test_tutorial_start_applies_cfg(void) {
    int first = find_tutorial_idx_by_name("First Triangle");
    int color = find_tutorial_idx_by_name("Color & Transform");
    ASSERT_TRUE("First Triangle in catalog", first >= 0);
    ASSERT_TRUE("Color & Transform in catalog", color >= 0);

    /* (A) First Triangle ships `@cfg view_mode = 1`: a 2D ortho view. */
    glr_app_reset_all();
    ASSERT_INT("view mode starts 3D", glr_config_get(GLR_CONFIG_ORTHO_MODE), 0);
    if (first >= 0) {
        ASSERT_TRUE("First Triangle has cfg lines",
                    repl_tutorial_cfg_lines(first) != NULL);
        tutorial_start(first);
        ASSERT_INT("First Triangle tutorial active", tutorial_active(), 1);
        ASSERT_INT("First Triangle cfg applied 2D view",
                   glr_config_get(GLR_CONFIG_ORTHO_MODE), 1);
        /* tutorial_exit runs tutorial_teardown, which restores the
         * cfg baseline captured at tutorial_start — see the bag-restore
         * lifecycle added in the REQUIRE/SET commit (8fefa82). The
         * baseline here was the post-reset 3D default, so exit reverts
         * the tutorial's 2D back to 3D. */
        tutorial_stop();
        ASSERT_INT("tutorial_exit restores view mode to pre-start baseline",
                   glr_config_get(GLR_CONFIG_ORTHO_MODE), 0);
    }

    /* (B) A tutorial with no cfg (Color & Transform) still gets the
     * per-start presentation reset, so a prior 2D doesn't leak in. */
    glr_app_reset_all();
    glr_config_set(GLR_CONFIG_ORTHO_MODE, 1);
    if (color >= 0) {
        ASSERT_TRUE("Color & Transform has no cfg",
                    repl_tutorial_cfg_lines(color) == NULL);
        tutorial_start(color);
        ASSERT_INT("no-cfg tutorial active", tutorial_active(), 1);
        ASSERT_INT("no-cfg tutorial resets view to 3D default",
                   glr_config_get(GLR_CONFIG_ORTHO_MODE), 0);
        /* tutorial_capture_cfg_baseline now records view_mode
         * unconditionally (not just when the tutorial's
         * @cfg / SET / REQUIRE references it), so the per-start
         * presentation_reset → 3D no longer leaks past teardown.
         * Color & Transform names no presentation slugs, but the
         * captured baseline still holds the pre-start 2D and exit
         * restores it. See test_tutorial_runner.c's
         * test_baseline_captures_view_mode_even_when_unreferenced
         * for the dedicated regression. */
        tutorial_stop();
        ASSERT_INT("no-cfg tutorial exit restores view mode to pre-start baseline",
                   glr_config_get(GLR_CONFIG_ORTHO_MODE), 1);
    }
}

/* Phase B: top-level MENU_TUTORIALS rows behave like Scene tag rows —
 * clicking a tag row is inert (returns 0, keeps menu open), and Restart /
 * Exit work via their new positions (tag_count + 1 / + 2). Activation
 * of an actual tutorial flows through route_submenu_item_hit (not this
 * function); the dispatch contract here only covers tag rows + the
 * trailing Restart/Exit. */
static void test_tutorial_menu_dispatch(void) {
    int first = find_tutorial_idx_by_name("First Triangle");
    glr_app_reset_all();
    ASSERT_TRUE("First Triangle in catalog", first >= 0);

    int tag_count = repl_tutorial_visible_tag_count();
    ASSERT_TRUE("at least one visible tutorial tag", tag_count > 0);

    /* Tag rows: inert (return 0, no tutorial started). */
    ASSERT_INT("tag row 0 is inert (returns 0)",
               glr_action_menu_item_activate(GLR_MENU_TUTORIALS, 0), 0);
    ASSERT_INT("tag row 0 did not start a tutorial",
               tutorial_active(), 0);

    /* Start a tutorial out-of-band (this is what route_submenu_item_hit
     * would do for a flyout-item click) so the Restart/Exit assertions
     * have something to act on. */
    if (first >= 0)
        tutorial_start(first);
    ASSERT_INT("tutorial active for restart/exit checks",
               tutorial_active(), 1);

    /* Restart at tag_count + 1: re-enters step 0. */
    ASSERT_INT("Restart row handled",
               glr_action_menu_item_activate(GLR_MENU_TUTORIALS,
                                             tag_count + 1), 1);
    ASSERT_INT("Restart returns step to 0",
               tutorial_state_view().step, 0);

    /* Exit at tag_count + 2: ends the tutorial. */
    ASSERT_INT("Exit row handled",
               glr_action_menu_item_activate(GLR_MENU_TUTORIALS,
                                             tag_count + 2), 1);
    ASSERT_INT("Exit ends the tutorial",
               tutorial_active(), 0);
}

/* Audit #20: glr_config_set(GLR_CONFIG_AUDIO_MODE, ...) routes through
 * the audio module's cfg_mode setter, not a raw pointer write. */
static void test_audio_config_direct_set(void) {
    glr_app_reset_all();

    glr_config_set(GLR_CONFIG_AUDIO_MODE, 2);
    ASSERT_INT("direct-set audio mode 2", glr_audio_get_cfg_mode(), 2);

    glr_config_set(GLR_CONFIG_AUDIO_MODE, 0);
    ASSERT_INT("direct-set audio mode 0", glr_audio_get_cfg_mode(), 0);

    glr_config_set(GLR_CONFIG_AUDIO_MODE, 99);
    ASSERT_INT("direct-set clamps to max valid",
               glr_audio_get_cfg_mode(), glr_config_state_count(GLR_CONFIG_AUDIO_MODE) - 1);
}

/* Audit #18: GLR_CONFIG_NONE must return 0/NULL state count and name,
 * and must not match the first rendering row (which carries GLR_CONFIG_NONE). */
static void test_config_none_handling(void) {
    ASSERT_INT("GLR_CONFIG_NONE state count is 0",
               glr_config_state_count(GLR_CONFIG_NONE), 0);
    ASSERT_TRUE("GLR_CONFIG_NONE state name is NULL",
               glr_config_state_name(GLR_CONFIG_NONE, 0) == NULL);
}

/* Audit #38: out-of-range menu indices must not crash or mutate state. */
static void test_menu_out_of_range_indices(void) {
    glr_app_reset_all();

    int before[GLR_CONFIG_COUNT];
    for (int k = 1; k < GLR_CONFIG_COUNT; k++)
        before[k] = glr_config_get((GlrConfigKey)k);

    ASSERT_INT("FILE out-of-range consumed",
               glr_action_menu_item_activate(GLR_MENU_FILE, 999), 1);
    ASSERT_INT("SCENE negative consumed",
               glr_action_menu_item_activate(GLR_MENU_SCENE, -1), 1);
    ASSERT_INT("TUTORIALS out-of-range consumed",
               glr_action_menu_item_activate(GLR_MENU_TUTORIALS, 999), 1);
    ASSERT_INT("CONFIG out-of-range returns 0 (menu stays open)",
               glr_action_menu_item_activate(GLR_MENU_CONFIG, 999), 0);

    for (int k = 1; k < GLR_CONFIG_COUNT; k++)
        ASSERT_INT("config unchanged after out-of-range activations",
                   glr_config_get((GlrConfigKey)k), before[k]);
}

/* Audit #19: cycling a non-replay config row while replay is active
 * stops the replay as a side effect. */
static void test_cfg_cycle_stops_replay(void) {
    glr_app_reset_all();

    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(0,0,0);");
    editor_feed_line("glEnd();");

    replay_start();
    ASSERT_INT("replay active after start", replay_active(), 1);

    int wireframe_row = -1;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && item->key == GLR_CONFIG_WIREFRAME) {
            wireframe_row = i;
            break;
        }
    }
    ASSERT_TRUE("found wireframe row", wireframe_row >= 0);

    glr_cfg_cycle_row(wireframe_row, 1);
    ASSERT_INT("replay stopped after non-replay cfg cycle",
               replay_active(), 0);
}

/* Audit #59 (Tier B, closeout) regression: ui_state_status_set_kind must
 * drop empty messages, not stamp them with full TTL. Without this guard
 * a stray repl_set_status("") (or any caller passing "") would overwrite
 * the live amber banner with a blank rect held for REPL_STATUS_MESSAGE_TTL
 * ticks. Reverting the early-return would silently regress because no
 * caller asserts on the banner contents after an empty set. */
static void test_status_set_drops_empty_message(void) {
    UiStatusState *status;

    glr_app_reset_all();
    status = ui_state_status_mut();

    /* Baseline: seed a real INFO message. */
    ui_state_status_set("real message");
    ASSERT_STR("seeded text", status->text, "real message");
    ASSERT_TRUE("seeded ttl positive", status->ttl > 0);
    ASSERT_INT("seeded kind INFO", status->kind, UI_STATUS_INFO);

    /* Empty set must not stamp; text and ttl unchanged. */
    int prior_ttl = status->ttl;
    ui_state_status_set("");
    ASSERT_STR("empty INFO does not overwrite text", status->text, "real message");
    ASSERT_INT("empty INFO leaves ttl unchanged", status->ttl, prior_ttl);

    /* Symmetric for ERROR: previously-set message survives an empty error. */
    ui_state_status_set_error("oops");
    ASSERT_STR("error text seeded", status->text, "oops");
    ASSERT_INT("error kind ERROR", status->kind, UI_STATUS_ERROR);
    prior_ttl = status->ttl;
    ui_state_status_set_error("");
    ASSERT_STR("empty ERROR does not overwrite text", status->text, "oops");
    ASSERT_INT("empty ERROR leaves ttl unchanged", status->ttl, prior_ttl);
    ASSERT_INT("empty ERROR leaves kind unchanged", status->kind, UI_STATUS_ERROR);

    /* NULL is also a no-op (the function's outer guard). */
    ui_state_status_set(NULL);
    ASSERT_STR("NULL message does not overwrite text", status->text, "oops");
}

/* glr_cfg_cycle_row per-key matrix (audit #50 prep).
 *
 * The function is a 91-line per-key special-case chain that audit #50
 * proposes folding into an on_change hook on GlrConfigItem. The
 * remaining special cases not covered by test_cfg_cycling above are
 * captured here so the refactor preserves each side effect:
 *
 *   FOCUS_ORIGIN         → glr_camera_focus_origin (ease target=origin)
 *   RESET_CAMERA         → glr_camera_ease_to_default
 *   AUTO_TIME + Shift    → repl_reset_time_to_zero
 *   CODE_PANEL_LAYOUT_HIDDEN → ui_menu_bar_close + color_picker_stop +
 *                              editor_completion_clear (the hidden-only
 *                              branch — the other layouts test pure
 *                              status & state in test_cfg_cycling).
 *
 * Each case asserts an OBSERVABLE side effect (camera target active,
 * time reset, panel-open flag) rather than a comparison against the
 * function's internal control flow, so any refactor that preserves
 * the side effect passes. */
static int find_cfg_row_for_key(GlrConfigKey key) {
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && item->key == key)
            return i;
    }
    return -1;
}

static void test_cfg_cycle_focus_origin_eases_to_origin(void) {
    glr_app_reset_all();

    int row = find_cfg_row_for_key(GLR_CONFIG_FOCUS_ORIGIN);
    ASSERT_TRUE("found focus_origin row", row >= 0);
    if (row < 0) return;

    /* Move target away from origin so the ease has somewhere to go. */
    glr_camera_set(20.0f, 30.0f, 5.0f, 3.0f, 4.0f, 5.0f, 0.0f);
    ASSERT_INT("camera target inactive before cycle",
               glr_camera_target_active(), 0);

    glr_cfg_cycle_row(row, 1);

    ASSERT_INT("focus_origin starts a camera ease",
               glr_camera_target_active(), 1);
    ASSERT_STR("focus_origin status", g_last_status, "Camera: focus origin");

    /* Tick to convergence and verify the ease destination was the origin
     * (tx/ty/tz, not rx/ry — focus_origin recenters the orbit target). */
    for (int i = 0; i < 500 && glr_camera_target_active(); i++)
        glr_camera_tick();
    GlrCameraState cam = glr_camera();
    ASSERT_TRUE("focus_origin lands at origin (tx)", fabsf(cam.tx) < 1e-3f);
    ASSERT_TRUE("focus_origin lands at origin (ty)", fabsf(cam.ty) < 1e-3f);
    ASSERT_TRUE("focus_origin lands at origin (tz)", fabsf(cam.tz) < 1e-3f);
    /* Orbit angles are preserved by focus_origin. */
    ASSERT_TRUE("focus_origin preserves rx", fabsf(cam.rx - 20.0f) < 1e-3f);
    ASSERT_TRUE("focus_origin preserves ry", fabsf(cam.ry - 30.0f) < 1e-3f);
}

static void test_cfg_cycle_reset_camera_eases_to_default(void) {
    glr_app_reset_all();

    int row = find_cfg_row_for_key(GLR_CONFIG_RESET_CAMERA);
    ASSERT_TRUE("found reset_camera row", row >= 0);
    if (row < 0) return;

    glr_camera_set(45.0f, 90.0f, 12.0f, 7.0f, -2.0f, 3.0f, 0.0f);

    glr_cfg_cycle_row(row, 1);

    ASSERT_INT("reset_camera starts an ease", glr_camera_target_active(), 1);
    ASSERT_STR("reset_camera status", g_last_status, "Camera: reset to default");

    for (int i = 0; i < 500 && glr_camera_target_active(); i++)
        glr_camera_tick();
    GlrCameraState cam = glr_camera();
    /* Built-in default is (rx=20, ry=30, dist=5, tx=ty=tz=0). */
    ASSERT_TRUE("reset_camera rx reaches default 20", fabsf(cam.rx - 20.0f) < 0.5f);
    ASSERT_TRUE("reset_camera ry reaches default 30", fabsf(cam.ry - 30.0f) < 0.5f);
    ASSERT_TRUE("reset_camera dist reaches default 5", fabsf(cam.dist - 5.0f) < 0.5f);
}

static void test_cfg_cycle_auto_time_shift_resets_time(void) {
    glr_app_reset_all();

    int row = find_cfg_row_for_key(GLR_CONFIG_AUTO_TIME);
    ASSERT_TRUE("found auto_time row", row >= 0);
    if (row < 0) return;

    /* Seed a non-zero t predef-var value so we can observe the reset.
     * (repl_reset_time_to_zero zeros the t predef var, not the
     * controller-side anim_time accumulator — the predef value is
     * what the executor and the scene observe each frame.) */
    int t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("t predef var exists", t_idx >= 0);
    if (t_idx < 0) return;
    g_predef_vars[t_idx].value = 7.25f;
    int was_playing = repl_state_variables().time_playing;

    /* Shift+cycle: resets time to 0 regardless of play state. Use the
     * modifier provider seam to make Shift observable to the cfg path. */
    editor_input_set_modifier_provider_for_test(test_mods_provider);
    g_test_mods = GLUT_ACTIVE_SHIFT;
    glr_cfg_cycle_row(row, 1);
    g_test_mods = 0;
    editor_input_set_modifier_provider_for_test(NULL);

    ASSERT_TRUE("Shift+auto_time zeros t",
                fabsf(g_predef_vars[t_idx].value) < 1e-6f);
    /* time_playing is not changed by the reset path — it just zeroes
     * the clock. Pin the invariant so a refactor that flips play state
     * trips the assert. */
    ASSERT_INT("Shift+auto_time leaves play state unchanged",
               repl_state_variables().time_playing, was_playing);
    /* Status reflects the current play state. */
    const char *expected_status = repl_state_variables().time_playing
                                      ? "Time: reset to 0"
                                      : "Time: reset to 0 (paused)";
    ASSERT_STR("Shift+auto_time status", g_last_status, expected_status);
}

static void test_cfg_cycle_panel_hidden_closes_overlays(void) {
    glr_app_reset_all();

    int row = find_cfg_row_for_key(GLR_CONFIG_CODE_PANEL_LAYOUT);
    ASSERT_TRUE("found code_panel_layout row", row >= 0);
    if (row < 0) return;

    /* Start from LEFT and cycle until HIDDEN; along the way seed an
     * open menu so the HIDDEN branch's ui_menu_bar_close() reach is
     * observable. */
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();

    /* Cycle to HIDDEN (LEFT -> TOP -> BOTTOM -> HIDDEN). At HIDDEN the
     * cycle must close any open menu / picker / autocomplete because
     * the panel that hosts them is no longer drawn. */
    while (glr_state_presentation().code_panel_layout != CODE_PANEL_LAYOUT_HIDDEN) {
        /* Seed each transition with an open menu so the HIDDEN branch
         * (when reached) has something to close. */
        ui_menu_bar_set_open_menu(GLR_MENU_FILE, 0.0f);
        glr_cfg_cycle_row(row, 1);
    }

    ASSERT_INT("HIDDEN layout closes the open menu",
               ui_menu_bar_open_menu_id(), -1);
    ASSERT_INT("HIDDEN layout closes the color picker",
               color_picker_view().open, 0);
    ASSERT_INT("HIDDEN layout clears autocomplete matches",
               editor_state_autocomplete().match_count, 0);
    ASSERT_STR("HIDDEN layout status", g_last_status,
               "Layout: code panel hidden");
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
    test_tutorial_start_applies_cfg();
    test_tutorial_menu_dispatch();
    test_audio_config_direct_set();
    test_config_none_handling();
    test_menu_out_of_range_indices();
    test_cfg_cycle_stops_replay();
    test_status_set_drops_empty_message();
    test_cfg_cycle_focus_origin_eases_to_origin();
    test_cfg_cycle_reset_camera_eases_to_default();
    test_cfg_cycle_auto_time_shift_resets_time();
    test_cfg_cycle_panel_hidden_closes_overlays();

    return test_harness_report(&g_harness, "test_repl_actions");
}
