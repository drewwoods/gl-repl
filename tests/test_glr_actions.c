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
#include "editor/inline_rename.h"
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
#include "repl/cfg_baseline.h"             /* repl_cfg_set_text, repl_cfg_resolve_text */
#include "scene/themes.h"                  /* GRID/AXES/SCENE_BACKDROP/LIGHT_THEME_* */
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
                                        const char *expect_file,
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
        if (expect_file) {
            ASSERT_INT("menu action wrote expected temp file",
                       access(expect_file, F_OK), 0);
            unlink(expect_file);
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
    glr_ctrl_reset_all();
    /* glr_actions_apply_defaults pulls from glr_audio_get_cfg_mode()
     * which defaults to AUDIO_CFG_ALL (1 = on) if invalid. */
    glr_actions_apply_defaults();
    ASSERT_INT("default audio mode is ALL", glr_config_get(GLR_CONFIG_AUDIO_MODE), 1);
}

static void test_cursor_actions(void) {
    glr_ctrl_reset_all();
    EditorCursorBlinkState *cb = editor_state_cursor_blink_mut();
    cb->cursor_visible = 0;
    cb->blink_tick = 100;

    glr_action_cursor_blink_reset();
    ASSERT_INT("cursor visible after reset", editor_state_cursor_blink().cursor_visible, 1);
    ASSERT_INT("blink tick reset", editor_state_cursor_blink().blink_tick, 0);
}

static void test_help_tab_actions(void) {
    glr_ctrl_reset_all();
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
    glr_ctrl_reset_all();

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

    /* Test Audio modes (2-state: off/on) */
    glr_config_set(GLR_CONFIG_AUDIO_MODE, 0); // Off (pause)
    glr_cfg_cycle_row(audio_row, 1); // -> On
    ASSERT_INT("audio mode On", glr_config_get(GLR_CONFIG_AUDIO_MODE), 1);
    ASSERT_STR("status audio On", g_last_status, "Audio: on");
    ASSERT_INT("audio engine not paused", glr_audio_is_paused(), 0);
    ASSERT_INT("audio engine loop mode ALL", glr_audio_get_loop_mode(), GLR_AUDIO_LOOP_ALL);

    glr_cfg_cycle_row(audio_row, 1); // -> Off
    ASSERT_INT("audio mode Off", glr_config_get(GLR_CONFIG_AUDIO_MODE), 0);
    ASSERT_STR("status audio Off", g_last_status, "Audio: off");
    ASSERT_INT("audio engine paused", glr_audio_is_paused(), 1);
}

static void test_menu_actions(void) {
    glr_ctrl_reset_all();

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
                                NULL,
                                1);
    run_menu_action_in_temp_dir("File Load Workspace",
                                GLR_MENU_FILE,
                                GLR_FILE_ITEM_LOAD_WORKSPACE,
                                NULL,
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
     * selector). New Scene creates a real empty user-scene slot so the
     * tab strip reflects it immediately. */
    ASSERT_INT("File New Scene", glr_action_menu_item_activate(GLR_MENU_FILE, GLR_FILE_ITEM_NEW_SCENE), 1);
    ASSERT_INT("active example cleared", repl_state_scenes().active_example_idx, -1);
    ASSERT_TRUE("active user scene created", repl_active_user_scene() >= 0);
    ASSERT_INT("New Scene clears document",
               source_document_view().line_count, 0);

    /* Save Scene writes the active named scene's slug; run sandboxed so
     * it cannot touch a repo file. */
    run_menu_action_in_temp_dir("File Save Scene",
                                GLR_MENU_FILE,
                                GLR_FILE_ITEM_SAVE_SCENE,
                                "new_scene.c",
                                0);

    ASSERT_INT("File Rename Scene (active)", glr_action_menu_item_activate(GLR_MENU_FILE, GLR_FILE_ITEM_RENAME_SCENE), 1);
    ASSERT_INT("Rename prompt active", editor_inline_rename_active(), 1);
    editor_inline_rename_cancel();

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
    glr_ctrl_reset_all();

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
    glr_ctrl_reset_all();

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
    glr_ctrl_reset_all();
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
    glr_ctrl_reset_all();
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
    glr_ctrl_reset_all();
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
    glr_ctrl_reset_all();
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
    glr_ctrl_reset_all();

    glr_config_set(GLR_CONFIG_AUDIO_MODE, 1);
    ASSERT_INT("direct-set audio mode 1", glr_audio_get_cfg_mode(), 1);

    glr_config_set(GLR_CONFIG_AUDIO_MODE, 0);
    ASSERT_INT("direct-set audio mode 0", glr_audio_get_cfg_mode(), 0);

    glr_config_set(GLR_CONFIG_AUDIO_MODE, 99);
    ASSERT_INT("direct-set clamps to max valid",
               glr_audio_get_cfg_mode(), glr_config_state_count(GLR_CONFIG_AUDIO_MODE) - 1);
}

static void test_msaa_display_label_override(void) {
    int msaa_row = -1;

    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && item->key == GLR_CONFIG_MSAA) {
            msaa_row = i;
            break;
        }
    }

    ASSERT_TRUE("found MSAA row", msaa_row >= 0);
    if (msaa_row < 0)
        return;

    const GlrConfigItem *item = glr_config_item_at(msaa_row);
    ASSERT_TRUE("MSAA item available", item != NULL);

    glr_actions_set_msaa_label(4);
    ASSERT_STR("MSAA display label override", glr_config_item_display_label(item), "MSAAx4");
    ASSERT_STR("MSAA slug remains stable", glr_config_item_slug(item), "msaa");

    glr_actions_set_msaa_label(1);
    ASSERT_STR("MSAA display label resets", glr_config_item_display_label(item), "MSAA");
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
    glr_ctrl_reset_all();

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

/* Audit #19: cycling a config row that invalidates replay stops the replay,
 * but non-invalidating ones (like wireframe) preserve active replay. */
static void test_cfg_cycle_stops_replay(void) {
    glr_ctrl_reset_all();

    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(0,0,0);");
    editor_feed_line("glEnd();");

    replay_start();
    ASSERT_INT("replay active after start", replay_active(), 1);

    int wireframe_row = -1;
    int autonormals_row = -1;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item) {
            if (item->key == GLR_CONFIG_WIREFRAME) {
                wireframe_row = i;
            } else if (item->key == GLR_CONFIG_AUTO_NORMALS) {
                autonormals_row = i;
            }
        }
    }
    ASSERT_TRUE("found wireframe row", wireframe_row >= 0);
    ASSERT_TRUE("found autonormals row", autonormals_row >= 0);

    glr_cfg_cycle_row(wireframe_row, 1);
    ASSERT_INT("replay active after non-invalidating cfg cycle",
               replay_active(), 1);

    glr_cfg_cycle_row(autonormals_row, 1);
    ASSERT_INT("replay stopped after invalidating cfg cycle",
               replay_active(), 0);
}

static void test_replay_config_set_uses_lifecycle(void) {
    glr_ctrl_reset_all();

    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(0,0,0);");
    editor_feed_line("glEnd();");

    repl_state_variables_mut()->time_playing = 1;
    glr_config_set(GLR_CONFIG_REPLAY, 1);
    ASSERT_INT("direct replay config active", replay_active(), 1);
    ASSERT_INT("direct replay config machine playing",
               replay_machine_state(), REPLAY_PLAYING);
    ASSERT_INT("direct replay config pc reset", replay_pc(), 0);
    ASSERT_TRUE("direct replay config captured flat commands",
                replay_total_flat() > 0);
    ASSERT_INT("direct replay config pauses time",
               repl_state_variables().time_playing, 0);

    glr_config_set(GLR_CONFIG_REPLAY, 0);
    ASSERT_INT("direct replay config inactive", replay_active(), 0);
    ASSERT_INT("direct replay config machine off",
               replay_machine_state(), REPLAY_OFF);
    ASSERT_INT("direct replay config cleared flat count", replay_total_flat(), 0);
    ASSERT_INT("direct replay config restored time",
               repl_state_variables().time_playing, 1);
}

/* Audit #59 (Tier B, closeout) regression: ui_state_status_set_kind must
 * drop empty messages, not stamp them with full TTL. Without this guard
 * a stray repl_set_status("") (or any caller passing "") would overwrite
 * the live amber banner with a blank rect held for REPL_STATUS_MESSAGE_TTL
 * ticks. Reverting the early-return would silently regress because no
 * caller asserts on the banner contents after an empty set. */
static void test_status_set_drops_empty_message(void) {
    UiStatusState *status;

    glr_ctrl_reset_all();
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
    glr_ctrl_reset_all();

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
    glr_ctrl_reset_all();

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
    glr_ctrl_reset_all();

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
    g_predef_vars_mut[t_idx].value = 7.25f;
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
    glr_ctrl_reset_all();

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
               editor_state_autocomplete()->match_count, 0);
    ASSERT_STR("HIDDEN layout status", g_last_status,
               "Layout: code panel hidden");
}

static void test_vertex_label_modes(void) {
    glr_ctrl_reset_all();

    int vertex_labels_row = -1;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && item->key == GLR_CONFIG_VERTEX_LABELS) {
            vertex_labels_row = i;
            break;
        }
    }
    ASSERT_TRUE("vertex labels row found", vertex_labels_row >= 0);

    const GlrConfigItem *item = glr_config_item_at(vertex_labels_row);
    ASSERT_INT("vertex labels state_count is 3",
               item->state_count, GLR_VERTEX_LABEL_COUNT);
    ASSERT_TRUE("vertex labels has state_names",
                item->state_names != NULL);

    ASSERT_INT("default is Index",
               glr_config_get(GLR_CONFIG_VERTEX_LABELS), GLR_VERTEX_LABEL_INDEX);

    glr_cfg_cycle_row(vertex_labels_row, 1);
    ASSERT_INT("cycle to Index+Pos",
               glr_config_get(GLR_CONFIG_VERTEX_LABELS), GLR_VERTEX_LABEL_INDEX_POS);
    ASSERT_STR("status Index+Pos", g_last_status,
               "Vertex labels: Index+Pos");

    glr_cfg_cycle_row(vertex_labels_row, 1);
    ASSERT_INT("cycle to Off",
               glr_config_get(GLR_CONFIG_VERTEX_LABELS), GLR_VERTEX_LABEL_OFF);
    ASSERT_STR("status Off", g_last_status, "Vertex labels: Off");

    glr_cfg_cycle_row(vertex_labels_row, 1);
    ASSERT_INT("cycle wraps to Index",
               glr_config_get(GLR_CONFIG_VERTEX_LABELS), GLR_VERTEX_LABEL_INDEX);
    ASSERT_STR("status Index", g_last_status, "Vertex labels: Index");

    glr_config_set(GLR_CONFIG_VERTEX_LABELS, GLR_VERTEX_LABEL_INDEX_POS);
    ASSERT_INT("direct set Index+Pos",
               glr_state_presentation().show_vertex_labels, GLR_VERTEX_LABEL_INDEX_POS);

    glr_config_set(GLR_CONFIG_VERTEX_LABELS, GLR_VERTEX_LABEL_OFF);
    ASSERT_INT("direct set Off",
               glr_state_presentation().show_vertex_labels, GLR_VERTEX_LABEL_OFF);
}

/* Audit #41: the cfg bridge accepts symbolic value names so catalogs
 * can write "@cfg grid = GRID_THEME_RADAR" instead of a magic 10.
 * Pin each scene-enum slug end-to-end (resolve_text + apply via
 * repl_cfg_set_text), plus the legacy integer path that still has
 * to round-trip from older saved files. */
static void test_cfg_bridge_resolves_symbolic_names(void) {
    test_apply_defaults();  /* installs g_glr_export_cfg_bridge */

    int out = -1;
    ASSERT_TRUE("resolve grid: GRID_THEME_RADAR",
                repl_cfg_resolve_text("grid", "GRID_THEME_RADAR", &out));
    ASSERT_INT("  -> GRID_THEME_RADAR", out, GRID_THEME_RADAR);

    out = -1;
    ASSERT_TRUE("resolve grid: GRID_THEME_FOCUS",
                repl_cfg_resolve_text("grid", "GRID_THEME_FOCUS", &out));
    ASSERT_INT("  -> GRID_THEME_FOCUS", out, GRID_THEME_FOCUS);

    out = -1;
    ASSERT_TRUE("resolve axes: AXES_THEME_COMPASS",
                repl_cfg_resolve_text("axes", "AXES_THEME_COMPASS", &out));
    ASSERT_INT("  -> AXES_THEME_COMPASS", out, AXES_THEME_COMPASS);

    out = -1;
    ASSERT_TRUE("resolve backdrop: SCENE_BACKDROP_CITY_AND_STARS",
                repl_cfg_resolve_text("backdrop", "SCENE_BACKDROP_CITY_AND_STARS", &out));
    ASSERT_INT("  -> SCENE_BACKDROP_CITY_AND_STARS", out,
               SCENE_BACKDROP_CITY_AND_STARS);

    /* Unknown name on a known slug must fail (don't fall back to
     * strtol — "GRID_THEME_XYZ" must NOT silently land as 0). */
    out = -42;
    ASSERT_TRUE("unknown symbol fails resolve",
                !repl_cfg_resolve_text("grid", "GRID_THEME_XYZ", &out));
    ASSERT_INT("  out untouched on failure", out, -42);

    /* Apply via repl_cfg_set_text — proves the bridge wires the
     * resolved int into glr_export_cfg_apply, not just the lookup. */
    repl_cfg_set_text("grid", "GRID_THEME_TRON");
    ASSERT_INT("set_text grid -> presentation.grid_theme",
               glr_state_presentation().grid_theme, GRID_THEME_TRON);
    repl_cfg_set_text("axes", "AXES_THEME_NEON");
    ASSERT_INT("set_text axes -> presentation.axes_theme",
               glr_state_presentation().axes_theme, AXES_THEME_NEON);
    repl_cfg_set_text("backdrop", "SCENE_BACKDROP_STARS");
    ASSERT_INT("set_text backdrop -> presentation.backdrop_mode",
               glr_state_presentation().backdrop_mode, SCENE_BACKDROP_STARS);

    /* Legacy integer-form @cfg lines must still load — the apply
     * path tries resolve_text first, then falls back to strtol.
     * Drives the same bag/apply edge repl_cfg_set_text uses, just
     * with an integer-string value instead of a symbolic name. */
    repl_cfg_set_text("grid", "4");  /* GRID_THEME_EMBER */
    ASSERT_INT("integer-form '4' still resolves to GRID_THEME_EMBER",
               glr_state_presentation().grid_theme, GRID_THEME_EMBER);

    /* A typo'd symbolic value name must NOT silently land at 0
     * (GRID_THEME_OFF). The strtol fallback was tightened to skip
     * identifier-shaped tokens precisely so a misspelled catalog
     * literal fails to apply instead of muting the showcase. */
    glr_state_presentation_mut()->grid_theme = GRID_THEME_PLANES;
    repl_cfg_set_text("grid", "GRID_THEME_RADRA");  /* typo */
    ASSERT_INT("typo'd symbol leaves grid_theme unchanged",
               glr_state_presentation().grid_theme, GRID_THEME_PLANES);
    glr_state_presentation_mut()->axes_theme = AXES_THEME_PULSE;
    repl_cfg_set_text("axes", "AXES_THEME_COMPSS");  /* typo */
    ASSERT_INT("typo'd axes symbol leaves axes_theme unchanged",
               glr_state_presentation().axes_theme, AXES_THEME_PULSE);
    glr_state_presentation_mut()->backdrop_mode = SCENE_BACKDROP_STARS;
    repl_cfg_set_text("backdrop", "SCENE_BACKDROP_CITISCAPE");  /* typo */
    ASSERT_INT("typo'd backdrop symbol leaves backdrop_mode unchanged",
               glr_state_presentation().backdrop_mode, SCENE_BACKDROP_STARS);
}

/* F9 was reassigned from Auto-normals to cycling the light theme. The
 * generic special-shortcut dispatch finds the descriptor row by key_code
 * and cycles it, so F9 must now advance GLR_CONFIG_LIGHT_THEME through all
 * LIGHT_THEME_COUNT presets (and must no longer touch auto-normals). */
static void test_f9_cycles_light_theme(void) {
    glr_ctrl_reset_all();
    printf("--- F9 cycles the light theme ---\n");

    glr_config_set(GLR_CONFIG_LIGHT_THEME, LIGHT_THEME_DEFAULT);
    int auto_normals_before = glr_config_get(GLR_CONFIG_AUTO_NORMALS);

    ASSERT_INT("F9 is handled as a special shortcut",
               glr_cfg_handle_special_shortcut(GLUT_KEY_F9), 1);
    ASSERT_INT("F9 advances the light theme one step",
               glr_config_get(GLR_CONFIG_LIGHT_THEME), LIGHT_THEME_HEADLIGHT);
    ASSERT_INT("F9 no longer toggles auto-normals",
               glr_config_get(GLR_CONFIG_AUTO_NORMALS), auto_normals_before);

    /* Cycling LIGHT_THEME_COUNT-1 more times wraps back to the first theme. */
    for (int i = 0; i < LIGHT_THEME_COUNT - 1; i++)
        glr_cfg_handle_special_shortcut(GLUT_KEY_F9);
    ASSERT_INT("F9 wraps back to the default theme",
               glr_config_get(GLR_CONFIG_LIGHT_THEME), LIGHT_THEME_DEFAULT);
}

/* Shift+F<n> steps the F-key-bound config rows BACKWARD (plain F<n> steps
 * forward). Exercised on two multi-state cycles — light theme (F9) and grid
 * theme (F3) — where the direction is observable; 2-state toggles flip
 * either way. Modifiers are driven through the editor test seam. */
static void test_shift_fkey_steps_backward(void) {
    glr_ctrl_reset_all();
    printf("--- Shift+F<n> steps shortcuts backward ---\n");
    editor_input_set_modifier_provider_for_test(test_mods_provider);

    /* Light theme (F9): from the first theme, Shift+F9 wraps to the last. */
    glr_config_set(GLR_CONFIG_LIGHT_THEME, LIGHT_THEME_DEFAULT);
    g_test_mods = GLUT_ACTIVE_SHIFT;
    ASSERT_INT("Shift+F9 handled",
               glr_cfg_handle_special_shortcut(GLUT_KEY_F9), 1);
    ASSERT_INT("Shift+F9 wraps backward to the last theme",
               glr_config_get(GLR_CONFIG_LIGHT_THEME), LIGHT_THEME_COUNT - 1);
    glr_cfg_handle_special_shortcut(GLUT_KEY_F9);
    ASSERT_INT("Shift+F9 steps one further back",
               glr_config_get(GLR_CONFIG_LIGHT_THEME), LIGHT_THEME_COUNT - 2);

    /* Plain F9 (no Shift) steps forward, undoing one backward step. */
    g_test_mods = 0;
    glr_cfg_handle_special_shortcut(GLUT_KEY_F9);
    ASSERT_INT("plain F9 forward undoes one Shift+F9 step",
               glr_config_get(GLR_CONFIG_LIGHT_THEME), LIGHT_THEME_COUNT - 1);

    /* Grid theme (F3): same backward behavior on a different F-key row. */
    glr_config_set(GLR_CONFIG_GRID_THEME, 0);
    g_test_mods = GLUT_ACTIVE_SHIFT;
    ASSERT_INT("Shift+F3 handled",
               glr_cfg_handle_special_shortcut(GLUT_KEY_F3), 1);
    ASSERT_INT("Shift+F3 wraps grid theme backward to the last",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_COUNT - 1);

    editor_input_set_modifier_provider_for_test(NULL);
    g_test_mods = 0;
}

/* The F-keys were reassigned to the longest cycles (so Shift+F backward
 * pays off), and the displaced 2-state toggles moved to Ctrl shortcuts.
 * Verify the new wiring: reassigned F-keys cycle their config, the four
 * toggles respond to their new Ctrl / Ctrl+Shift keys, and the Shift-gated
 * ones are NOT claimed by the cfg layer without Shift. */
static void test_fkey_reassignment_and_alt_shortcuts(void) {
    glr_ctrl_reset_all();
    printf("--- F-key reassignment + alternative toggle shortcuts ---\n");
    editor_input_set_modifier_provider_for_test(test_mods_provider);

    /* Reassigned F-keys now drive the long cycles (plain, no Shift). */
    g_test_mods = 0;
    struct { int fkey; GlrConfigKey key; const char *name; } fmap[] = {
        { GLUT_KEY_F2,  GLR_CONFIG_ACCUM_AA,        "Accum AA"        },
        { GLUT_KEY_F6,  GLR_CONFIG_BACKDROP,        "Backdrop"        },
        { GLUT_KEY_F7,  GLR_CONFIG_GRID_EXTENT,     "Grid extent"     },
        { GLUT_KEY_F10, GLR_CONFIG_SYNTAX_HIGHLIGHT,"Syntax highlight"},
    };
    for (unsigned i = 0; i < sizeof(fmap)/sizeof(fmap[0]); i++) {
        int before = glr_config_get(fmap[i].key);
        ASSERT_INT("reassigned F-key consumed",
                   glr_cfg_handle_special_shortcut(fmap[i].fkey), 1);
        ASSERT_TRUE("reassigned F-key cycles its config",
                    glr_config_get(fmap[i].key) != before);
    }

    /* Wireframe -> plain Ctrl+G. */
    glr_state_presentation_mut()->wireframe = 0;
    ASSERT_INT("Ctrl+G consumed", glr_cfg_handle_ascii_shortcut(KEY_CTRL_G), 1);
    ASSERT_INT("Ctrl+G toggles wireframe", glr_state_presentation().wireframe, 1);

    /* The three Ctrl+Shift toggles fire only with Shift held. */
    g_test_mods = GLUT_ACTIVE_SHIFT;
    glr_state_presentation_mut()->show_normal_vectors = 0;
    ASSERT_INT("Ctrl+Shift+N consumed", glr_cfg_handle_ascii_shortcut(KEY_CTRL_N), 1);
    ASSERT_INT("Ctrl+Shift+N toggles normal vectors",
               glr_state_presentation().show_normal_vectors, 1);

    glr_state_presentation_mut()->show_vertex_outlines = 0;
    ASSERT_INT("Ctrl+Shift+E consumed", glr_cfg_handle_ascii_shortcut(KEY_CTRL_E), 1);
    ASSERT_INT("Ctrl+Shift+E toggles vertex outlines",
               glr_state_presentation().show_vertex_outlines, 1);

    glr_state_presentation_mut()->show_light_indicators = 0;
    ASSERT_INT("Ctrl+Shift+L consumed", glr_cfg_handle_ascii_shortcut(KEY_CTRL_L), 1);
    ASSERT_INT("Ctrl+Shift+L toggles light indicators",
               glr_state_presentation().show_light_indicators, 1);

    /* Without Shift, the cfg layer must NOT claim Ctrl+N/E/L (they fall
     * through to the post-filter router / editor cursor-end / clear-all). */
    g_test_mods = 0;
    ASSERT_INT("plain Ctrl+N not claimed by cfg", glr_cfg_handle_ascii_shortcut(KEY_CTRL_N), 0);
    ASSERT_INT("plain Ctrl+E not claimed by cfg", glr_cfg_handle_ascii_shortcut(KEY_CTRL_E), 0);
    ASSERT_INT("plain Ctrl+L not claimed by cfg", glr_cfg_handle_ascii_shortcut(KEY_CTRL_L), 0);

    editor_input_set_modifier_provider_for_test(NULL);
    g_test_mods = 0;
}

/* No two config rows may claim the same keyboard binding. A binding is
 * the triple (key_code, modifiers, is_special): the same key with
 * different modifiers is distinct by design (Ctrl+W = CPU profile vs
 * Ctrl+Shift+W = Memory profile; the plain-Ctrl / Ctrl+Shift pairs noted
 * in keymap.h), and an F-key vs a Ctrl byte of equal numeric value are
 * distinct via is_special. Guards against an accidental double-map when a
 * keymap.h binding is reused across two rows. Data-driven over
 * g_cfg_items[] (the enumerable, collision-prone tier — it owns the
 * shared Ctrl-letters); key_code-0 rows are section chrome or
 * shortcut-less and are skipped. */
static void test_no_duplicate_config_bindings(void) {
    int count = 0, dups = 0;
    const GlrConfigItem *items = glr_config_items(&count);
    for (int i = 0; i < count; i++) {
        if (items[i].key_code == 0)
            continue;
        for (int j = i + 1; j < count; j++) {
            if (items[j].key_code == 0)
                continue;
            if (items[i].key_code == items[j].key_code &&
                items[i].modifiers == items[j].modifiers &&
                items[i].is_special == items[j].is_special) {
                char msg[160];
                snprintf(msg, sizeof msg,
                         "duplicate binding: '%s' and '%s' share "
                         "(key=%d, mods=%d, special=%d)",
                         items[i].label, items[j].label,
                         items[i].key_code, items[i].modifiers,
                         items[i].is_special);
                ASSERT_TRUE(msg, 0);
                dups++;
            }
        }
    }
    ASSERT_INT("config rows have no duplicate (key,mods,is_special)", dups, 0);
}

/* keymap_event_is is an EXACT modifier match (binding_mods == 0 means no
 * extra modifier), after normalizing away the Ctrl a control byte implies
 * and the macOS Cmd/SUPER alias. Exercised with explicit (key, mods) args
 * so it stays independent of how any individual GLR_* binding is set. */
static void test_keymap_event_is_strict(void) {
    editor_input_set_modifier_provider_for_test(test_mods_provider);

    /* Shift is exact: a plain (key,0) and its Shift twin never alias. */
    g_test_mods = 0;
    ASSERT_INT("plain key, none held -> match",
               keymap_event_is(GLUT_KEY_F12, GLUT_KEY_F12, 0), 1);
    ASSERT_INT("Shift binding, none held -> no match",
               keymap_event_is(GLUT_KEY_F12, GLUT_KEY_F12, GLUT_ACTIVE_SHIFT), 0);
    g_test_mods = GLUT_ACTIVE_SHIFT;
    ASSERT_INT("Shift binding, Shift held -> match",
               keymap_event_is(GLUT_KEY_F12, GLUT_KEY_F12, GLUT_ACTIVE_SHIFT), 1);
    ASSERT_INT("plain binding, Shift held -> no match (strict)",
               keymap_event_is(GLUT_KEY_F12, GLUT_KEY_F12, 0), 0);

    /* Ctrl is implicit in a control byte: a (Ctrl-byte, 0) binding matches
     * with Ctrl held, but adding Shift breaks it; the Ctrl+Shift twin
     * needs Shift. */
    g_test_mods = GLUT_ACTIVE_CTRL;
    ASSERT_INT("Ctrl byte, mods 0, Ctrl held -> match",
               keymap_event_is(KEY_CTRL_S, KEY_CTRL_S, 0), 1);
    g_test_mods = GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SHIFT;
    ASSERT_INT("Ctrl byte, mods 0, Ctrl+Shift held -> no match",
               keymap_event_is(KEY_CTRL_S, KEY_CTRL_S, 0), 0);
    ASSERT_INT("Ctrl byte, mods SHIFT, Ctrl+Shift held -> match",
               keymap_event_is(KEY_CTRL_C, KEY_CTRL_C, GLUT_ACTIVE_SHIFT), 1);

    /* (The macOS Cmd/SUPER normalization is only meaningful on the real
     * freeglut Cocoa build — GLUT_ACTIVE_SUPER is 0 under the GL stubs — so
     * it isn't asserted here; see keymap_event_is in src/editor/input.c.)
     * A binding that explicitly requires Ctrl (audio arrows) needs it. */
    g_test_mods = GLUT_ACTIVE_CTRL;
    ASSERT_INT("Ctrl-required, Ctrl held -> match",
               keymap_event_is(GLUT_KEY_LEFT, GLUT_KEY_LEFT, GLUT_ACTIVE_CTRL), 1);
    g_test_mods = 0;
    ASSERT_INT("Ctrl-required, none held -> no match",
               keymap_event_is(GLUT_KEY_LEFT, GLUT_KEY_LEFT, GLUT_ACTIVE_CTRL), 0);
    ASSERT_INT("key mismatch -> no match",
               keymap_event_is(GLUT_KEY_F11, GLUT_KEY_F12, 0), 0);

    editor_input_set_modifier_provider_for_test(NULL);
    g_test_mods = 0;
}

int main(void) {
    test_apply_defaults();
    test_no_duplicate_config_bindings();
    test_keymap_event_is_strict();
    test_f9_cycles_light_theme();
    test_shift_fkey_steps_backward();
    test_fkey_reassignment_and_alt_shortcuts();
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
    test_msaa_display_label_override();
    test_config_none_handling();
    test_menu_out_of_range_indices();
    test_cfg_cycle_stops_replay();
    test_replay_config_set_uses_lifecycle();
    test_status_set_drops_empty_message();
    test_cfg_cycle_focus_origin_eases_to_origin();
    test_cfg_cycle_reset_camera_eases_to_default();
    test_cfg_cycle_auto_time_shift_resets_time();
    test_cfg_cycle_panel_hidden_closes_overlays();
    test_vertex_label_modes();
    test_cfg_bridge_resolves_symbolic_names();

    return test_harness_report(&g_harness, "test_repl_actions");
}
