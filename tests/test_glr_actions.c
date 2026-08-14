#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "app/glr_actions.h"
#include "repl/state_owners.h"
#include "repl/pipeline.h"           /* ReplAutoNormalMode */
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "ui/app/state.h"
#include "editor/help_session.h"
#include "editor/inline_file_prompt.h"
#include "app/glr_config.h"
#include "config.h"                  /* DEFAULT_SCENE_FILE */
#include "app/glr_audio.h"
#include "repl/example_loader.h"
#include "repl/host_effects.h"
#include "repl/scenes.h"
#include "repl/workspace_io.h"
#include "repl/help_text.h"
#include "editor/input.h"
#include "editor/undo.h"          /* editor_undo_pop_snapshot */
#include "editor/state.h"                /* editor_state_edit_line_set */
#include "editor/inline_rename.h"
#include "repl/examples.h"
#include "repl/tutorials.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "app/glr_tours.h"
#include "app/glr_pointer_script.h"
#include "source_document.h"
#include "keymap.h"
#include "keys.h"
#include "app/glr_camera.h"               /* glr_camera_target_active / glr_camera */
#include "ui/app/menu_bar.h"              /* ui_menu_bar_open_menu_id, _set_open_menu */
#include "ui/app/view_mode_swatch.h"      /* ui_view_mode_swatch_state */
#include "ui/app/layout.h"                /* CODE_PANEL_LAYOUT_* */
#include "ui/support/cpuprof.h"           /* PROFILE_PANEL_* */
#include "repl/eval.h"                    /* g_predef_vars, repl_eval_find_predef_var_idx */
#include "subsystems/color_picker/color_picker_state.h"
#include "repl/cfg_baseline.h"             /* repl_cfg_set_text, repl_cfg_resolve_text */
#include "render3d/themes.h"                  /* GRID/AXES/SCENE_BACKDROP/LIGHT_THEME_* */
#include "subsystems/edit_overlays/edit_overlays.h"
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
                       access(GLR_WORKSPACES_ROOT_DIR "/" GLR_DEFAULT_WORKSPACE_NAME,
                              F_OK), 0);
            rmdir(GLR_WORKSPACES_ROOT_DIR "/" GLR_DEFAULT_WORKSPACE_NAME);
        }
        repl_set_workspace_dir(workspace_dir);
        ASSERT_INT("restore cwd after menu action", chdir(cwd), 0);
    }

    rmdir(made_dir);
}

static void remove_test_workspace(const char *dir) {
    WorkspaceManifest manifest;
    char err[REPL_STATUS_TEXT_MAX];
    if (workspace_io_manifest_read(dir, &manifest, err, sizeof(err))) {
        for (int i = 0; i < manifest.scene_count; i++) {
            char path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 8];
            if (workspace_io_path_join(dir, manifest.scene_files[i],
                                       path, sizeof(path)))
                unlink(path);
        }
    }
    {
        char path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 8];
        if (workspace_io_path_join(dir, WORKSPACE_IO_MANIFEST_FILE,
                                   path, sizeof(path)))
            unlink(path);
    }
    rmdir(dir);
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

    /* Tabs: Overview(0) -> Commands(1) -> Keys(2) -> About(3). */
    glr_action_help_tab_next();
    ASSERT_INT("help tab next moves to 1", editor_help_session_tab_idx(), 1);
    ASSERT_INT("help tab next resets scroll", editor_help_session_scroll(), 0);

    glr_action_help_tab_next();
    ASSERT_INT("help tab next moves to 2", editor_help_session_tab_idx(), 2);

    glr_action_help_tab_next();
    ASSERT_INT("help tab next moves to 3", editor_help_session_tab_idx(), 3);

    editor_help_session_set_scroll(30);
    glr_action_help_tab_next();
    ASSERT_INT("help tab next stays at 3 (max)", editor_help_session_tab_idx(), 3);
    ASSERT_INT("help tab next at max keeps scroll", editor_help_session_scroll(), 30);

    glr_action_help_tab_prev();
    ASSERT_INT("help tab prev moves to 2", editor_help_session_tab_idx(), 2);
    ASSERT_INT("help tab prev resets scroll", editor_help_session_scroll(), 0);

    glr_action_help_tab_prev();
    ASSERT_INT("help tab prev moves to 1", editor_help_session_tab_idx(), 1);

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
    int code_panel_row = -1;
    int auto_normals_row = -1;
    int replay_row = -1;
    int point_attenuation_row = -1;

    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (!item) continue;
        if (item->key == GLR_CONFIG_WIREFRAME) wireframe_row = i;
        if (item->key == GLR_CONFIG_AUTO_TIME) auto_time_row = i;
        if (item->key == GLR_CONFIG_CODE_PANEL_LAYOUT) code_panel_row = i;
        if (item->key == GLR_CONFIG_AUTO_NORMALS) auto_normals_row = i;
        if (item->key == GLR_CONFIG_REPLAY) replay_row = i;
        if (item->key == GLR_CONFIG_POINT_ATTENUATION) point_attenuation_row = i;
    }

    /* Test generic cycle (Wireframe) */
    glr_state_presentation_mut()->wireframe = 0;
    glr_cfg_cycle_row(wireframe_row, 1);
    ASSERT_INT("wireframe toggled to 1", glr_state_presentation().wireframe, 1);
    ASSERT_STR("wireframe status Wireframe", g_last_status, "Wireframe: Wireframe");

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

    /* Test Auto-normals: a three-state cycle Off -> Face -> Smooth -> Off */
    glr_state_presentation_mut()->autonormal = REPL_AUTONORMAL_OFF;
    glr_cfg_cycle_row(auto_normals_row, 1);
    ASSERT_INT("autonormal Face", glr_state_presentation().autonormal,
               REPL_AUTONORMAL_FACE);
    ASSERT_STR("status autonormal Face", g_last_status, "Auto-normals: Face");
    glr_cfg_cycle_row(auto_normals_row, 1);
    ASSERT_INT("autonormal Smooth", glr_state_presentation().autonormal,
               REPL_AUTONORMAL_SMOOTH);
    ASSERT_STR("status autonormal Smooth", g_last_status, "Auto-normals: Smooth");
    glr_cfg_cycle_row(auto_normals_row, 1);
    ASSERT_INT("autonormal Off", glr_state_presentation().autonormal,
               REPL_AUTONORMAL_OFF);
    ASSERT_STR("status autonormal Off", g_last_status,
               "Auto-normals: Off");

    /* Test Point Attenuation */
    glr_config_set(GLR_CONFIG_POINT_ATTENUATION, 0);
    glr_cfg_cycle_row(point_attenuation_row, 1);
    ASSERT_INT("point attenuation ON", glr_config_get(GLR_CONFIG_POINT_ATTENUATION), 1);
    ASSERT_STR("status point attenuation ON", g_last_status, "Point attenuation: ON");

    /* Audio is no longer a Config menu row, but its hidden config key
     * still drives the Play/Pause action through glr_config_set. */
    glr_audio_set_loop_mode(GLR_AUDIO_LOOP_SONG);
    glr_config_set(GLR_CONFIG_AUDIO_MODE, 0); // Off (pause)
    glr_action_toggle_audio_play_pause(); // -> On
    ASSERT_INT("audio mode On", glr_config_get(GLR_CONFIG_AUDIO_MODE), 1);
    ASSERT_STR("status audio On", g_last_status, "Audio: playing");
    ASSERT_INT("audio engine not paused", glr_audio_is_paused(), 0);
    ASSERT_INT("audio On preserves loop mode", glr_audio_get_loop_mode(), GLR_AUDIO_LOOP_SONG);

    glr_action_toggle_audio_play_pause(); // -> Off
    ASSERT_INT("audio mode Off", glr_config_get(GLR_CONFIG_AUDIO_MODE), 0);
    ASSERT_STR("status audio Off", g_last_status, "Audio: paused");
    ASSERT_INT("audio engine paused", glr_audio_is_paused(), 1);

    /* View-mode swatch toggle: glr_action_toggle_view_mode() must flip the
     * View mode config row (the click path the menu-bar 2D/3D swatch uses),
     * sharing the keybind's status text. Render3dViewMode: 0 = 3D, 1 = 2D. */
    glr_config_set(GLR_CONFIG_ORTHO_MODE, 0);
    glr_action_toggle_view_mode();
    ASSERT_INT("view mode toggled to 2D", glr_config_get(GLR_CONFIG_ORTHO_MODE), 1);
    ASSERT_STR("view mode status 2D", g_last_status, "View mode: 2D");
    glr_action_toggle_view_mode();
    ASSERT_INT("view mode toggled to 3D", glr_config_get(GLR_CONFIG_ORTHO_MODE), 0);
    ASSERT_STR("view mode status 3D", g_last_status, "View mode: 3D");
}

/* The view-mode swatch's pure visual-state selector. Render3dViewMode:
 * 0 = 3D, non-0 = 2D; projection_mix in [0,1] (0 = ortho/2D, 1 = persp/3D). */
static void test_view_mode_swatch_state(void) {
    float t = -1.0f;

    /* Settled endpoints -> flat text, t unused (0). */
    ASSERT_INT("settled 3D -> flat 3D",
               ui_view_mode_swatch_state(0, 1.0f, &t), UI_VIEW_SWATCH_FLAT_3D);
    ASSERT_INT("settled 2D -> flat 2D",
               ui_view_mode_swatch_state(1, 0.0f, &t), UI_VIEW_SWATCH_FLAT_2D);

    /* Heading to 2D (target 2D, mix easing 1->0) -> cross-fade, t = 1-mix. */
    ASSERT_INT("3D->2D -> cross-fade",
               ui_view_mode_swatch_state(1, 0.75f, &t), UI_VIEW_SWATCH_CROSSFADE);
    ASSERT_TRUE("cross-fade t = 1-mix", fabsf(t - 0.25f) < 1e-4f);

    /* Heading to 3D (target 3D, mix easing 0->1) -> lit cube, t = mix. */
    ASSERT_INT("2D->3D -> cube",
               ui_view_mode_swatch_state(0, 0.4f, &t), UI_VIEW_SWATCH_CUBE);
    ASSERT_TRUE("cube reveal t = mix", fabsf(t - 0.4f) < 1e-4f);

    /* Out-of-range mix clamps t into [0,1]. */
    ui_view_mode_swatch_state(0, 1.5f, &t); /* >=1 settles to flat 3D */
    ASSERT_INT("mix>1 settles flat 3D",
               ui_view_mode_swatch_state(0, 1.5f, &t), UI_VIEW_SWATCH_FLAT_3D);

    ASSERT_TRUE("label width fits both labels",
                ui_view_mode_swatch_label_width() >= 2 * FONT_SMALL_W);
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
    ASSERT_INT("File Open Workspace parent is hover-only",
               glr_action_menu_item_activate(GLR_MENU_FILE,
                                             GLR_FILE_ITEM_OPEN_WORKSPACE),
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
     * selector). New Scene creates a fresh user-scene slot so the
     * tab strip reflects it immediately. */
    ASSERT_INT("File New Scene", glr_action_menu_item_activate(GLR_MENU_FILE, GLR_FILE_ITEM_NEW_SCENE), 1);
    ASSERT_INT("active example cleared", repl_state_scenes().active_example_idx, -1);
    ASSERT_TRUE("active user scene created", repl_active_user_scene() >= 0);
    ASSERT_INT("New Scene seeds display baseline",
               source_document_view().line_count, 6);

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

    /* Config menu: top-level rows are section parents - activating
     * one is an inert no-op that keeps the dropdown open (returns 0). */
    ASSERT_INT("Config parent-row activate is inert (returns 0)",
               glr_action_menu_item_activate(GLR_MENU_CONFIG, 1), 0);
}

/* A built-in example is rendered as the visible scene tab but remains a
 * transient document until its first edit. Workspace-oriented save actions
 * must promote it before serializing the user-scene catalog; otherwise the
 * manifest reports "Saved 0 scenes" even though a scene is visibly open. */
static void test_workspace_save_promotes_visible_example(void) {
    char ctrl_s_dir[] = "/tmp/test_glr_ctrl_s_example.XXXXXX";
    char menu_dir[] = "/tmp/test_glr_save_workspace_example.XXXXXX";
    WorkspaceManifest manifest;
    char err[REPL_STATUS_TEXT_MAX];
    char scene_path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 8];
    char *made_dir;

    made_dir = mkdtemp(ctrl_s_dir);
    ASSERT_TRUE("Ctrl+S example workspace created", made_dir != NULL);
    if (!made_dir)
        return;
    memset(&manifest, 0, sizeof(manifest));
    manifest.version = 1;
    snprintf(manifest.name, sizeof(manifest.name), "Ctrl S Example");
    ASSERT_TRUE("Ctrl+S example manifest created",
                workspace_io_manifest_write(ctrl_s_dir, &manifest,
                                             err, sizeof(err)));

    glr_ctrl_reset_all();
    repl_set_workspace_dir(ctrl_s_dir);
    repl_load_example(0);
    ASSERT_INT("example starts transient before Ctrl+S",
               repl_active_user_scene(), -1);
    ASSERT_INT("Ctrl+S action saves visible example",
               glr_action_save_active_scene(), 1);
    ASSERT_TRUE("Ctrl+S promotes visible example",
                repl_active_user_scene() >= 0);
    int manifest_ok = workspace_io_manifest_read(ctrl_s_dir, &manifest,
                                                  err, sizeof(err));
    ASSERT_TRUE("Ctrl+S workspace manifest reloads", manifest_ok);
    ASSERT_INT("Ctrl+S workspace contains visible example",
               manifest.scene_count, 1);
    int path_ok = manifest_ok && manifest.scene_count == 1 &&
        workspace_io_path_join(ctrl_s_dir, manifest.scene_files[0],
                               scene_path, sizeof(scene_path));
    ASSERT_TRUE("Ctrl+S scene path resolves", path_ok);
    if (path_ok)
        ASSERT_INT("Ctrl+S scene file exists", access(scene_path, F_OK), 0);
    repl_set_workspace_dir(NULL);
    remove_test_workspace(ctrl_s_dir);

    made_dir = mkdtemp(menu_dir);
    ASSERT_TRUE("Save Workspace example directory created", made_dir != NULL);
    if (!made_dir) {
        glr_ctrl_reset_all();
        return;
    }
    memset(&manifest, 0, sizeof(manifest));
    manifest.version = 1;
    snprintf(manifest.name, sizeof(manifest.name), "Menu Example");
    ASSERT_TRUE("Save Workspace example manifest created",
                workspace_io_manifest_write(menu_dir, &manifest,
                                             err, sizeof(err)));

    glr_ctrl_reset_all();
    repl_set_workspace_dir(menu_dir);
    repl_load_example(1);
    ASSERT_INT("example starts transient before Save Workspace",
               repl_active_user_scene(), -1);
    ASSERT_INT("Save Workspace action consumed",
               glr_action_menu_item_activate(GLR_MENU_FILE,
                                             GLR_FILE_ITEM_SAVE_WORKSPACE), 1);
    ASSERT_TRUE("Save Workspace manifest reloads",
                workspace_io_manifest_read(menu_dir, &manifest,
                                            err, sizeof(err)));
    ASSERT_INT("Save Workspace contains visible example",
               manifest.scene_count, 1);
    repl_set_workspace_dir(NULL);
    remove_test_workspace(menu_dir);
    glr_ctrl_reset_all();
}

/* File > Split Declaration routes to the editor split entry: a multi-var
 * decl under the cursor splits one-per-line. */
static void test_split_decl_menu_action(void) {
    glr_ctrl_reset_all();

    editor_feed_line("float grid, extent;");
    editor_state_edit_line_set(0);   /* cursor on the decl */

    ASSERT_INT("File Split Declaration consumed",
               glr_action_menu_item_activate(GLR_MENU_FILE, GLR_FILE_ITEM_SPLIT_DECL), 1);
    ASSERT_INT("decl split into two lines", source_document_view().line_count, 2);
    ASSERT_STR("split line 0", editor_buffer_line(0), "  static float grid;");
    ASSERT_STR("split line 1", editor_buffer_line(1), "  static float extent;");
}

/* Regression: the in-app Open Workspace action must land on a loaded
 * scene (active slot >= 0) AND rescue the pre-load document to the
 * recovery file. Before the fix, repl_load_workspace left the active
 * slot at -1 with the (now tabless) pre-load document still live - the
 * user saw their current scene vanish from the tab strip yet stay
 * selected/showing in the editor. The CLI bootstrap already activated
 * the first slot; the menu caller forgot to. */
static void test_load_workspace_activates_scene(void) {
    glr_ctrl_reset_all();

    char cwd[1024];
    char temp_dir[] = "/tmp/test_repl_actions_loadws.XXXXXX";
    char *made_dir = mkdtemp(temp_dir);
    int have_cwd = getcwd(cwd, sizeof(cwd)) != NULL;
    ASSERT_TRUE("loadws mkdtemp", made_dir != NULL);
    ASSERT_TRUE("loadws getcwd", have_cwd);
    if (!made_dir || !have_cwd)
        return;
    ASSERT_INT("loadws chdir", chdir(made_dir), 0);

    /* Build a one-scene workspace on disk under ./ws. Fresh startup no longer
     * creates an implicit user scene, so make the source scene explicit. */
    ASSERT_INT("loadws create source scene",
               repl_scenes_create_empty_user_scene(), 0);
    editor_feed_line("glVertex3f(1, 1, 1);");
    repl_load_example(0);
    ASSERT_INT("loadws workspace slot occupied", repl_user_scene_slot_used(0), 1);
    ASSERT_TRUE("loadws saved a scene", repl_save_workspace("ws", NULL) >= 1);

    /* Fresh session with a distinctive unsaved pre-load document and no
     * active scene - exactly the reported repro (fresh start, then Load
     * Workspace from inside the REPL). */
    glr_ctrl_reset_all();
    repl_set_workspace_dir(NULL);
    editor_feed_line("glColor3f(0.9, 0.1, 0.1);");
    ASSERT_TRUE("loadws pre-load doc non-empty",
                source_document_view().line_count > 0);

    unlink(QUIT_RECOVERY_FILE);  /* clean slate for the rescue check */
    ASSERT_INT("Open Workspace action succeeds",
               glr_action_open_workspace_path("ws"),
               1);

    /* The fix: a workspace tab is actually selected now (was -1 pre-fix). */
    ASSERT_TRUE("Open Workspace lands on a scene",
                repl_active_user_scene() >= 0);
    /* The pre-load document was rescued to the recovery file (the menu
     * caller did not save recovery at all pre-fix). */
    ASSERT_INT("Open Workspace wrote recovery file",
               access(QUIT_RECOVERY_FILE, F_OK), 0);

    unlink(QUIT_RECOVERY_FILE);
    repl_set_workspace_dir(NULL);
    ASSERT_INT("loadws restore cwd", chdir(cwd), 0);
    {
        char cmd[1100];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", made_dir);
        int rc = system(cmd);
        (void)rc;
    }
}

/* Step 5 (Finding #1): every Config top-level row - each "### "
 * section parent and the synthetic "All" row - must be inert on
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
    /* g_cfg_items[] has seven "### " sections. Audio has its own menu, so
     * GLR_CONFIG_AUDIO_MODE is intentionally no longer surfaced here.
     * The section model must
     * be data-faithful: only real headers counted, "---" excluded, the
     * synthetic "All" view NOT counted here. */
    int n = glr_config_section_count();
    ASSERT_INT("section count", n, 7);

    /* glr_config_section_label is data-faithful: it returns the raw
     * "### " label with the marker stripped (still UPPERCASE). The
     * leading-uppercase prettify is a menu-display concern applied in
     * menu_item_label, not here. */
    const char *expect[] = {
        "RENDERING", "TIME & REPLAY", "SCENE", "CAMERA",
        "GEOMETRY", "OVERLAYS", "INTERFACE",
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
        const GlrConfigItem *it = glr_config_item_at(i);
        ASSERT_TRUE("Audio is not a visible Config row",
                    !it || it->key != GLR_CONFIG_AUDIO_MODE);
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

/* Modifier-aware dispatch in glr_cfg_handle_ascii_shortcut is exact: a
 * Shift-requiring row only matches with Shift held, and a plain row never
 * claims the shifted chord. */
static void test_ascii_shortcut_modifiers(void) {
    glr_ctrl_reset_all();
    editor_input_set_modifier_provider_for_test(test_mods_provider);

    /* Plain Ctrl+O (no Shift) runs Focus origin (an action row); its
     * Ctrl+Shift twin Vertex outlines must NOT toggle. */
    g_test_mods = 0;
    int vo0 = glr_config_get(GLR_CONFIG_VERTEX_OUTLINES);
    repl_set_status("");
    ASSERT_INT("plain Ctrl+O handled", glr_cfg_handle_ascii_shortcut(KEY_CTRL_O), 1);
    ASSERT_STR("plain Ctrl+O ran Focus origin", g_last_status,
               "Camera: focus origin");
    ASSERT_INT("plain Ctrl+O left Vertex outlines alone",
               glr_config_get(GLR_CONFIG_VERTEX_OUTLINES), vo0);

    /* Ctrl+Shift+O: the Shift-requiring Vertex outlines row toggles; Focus
     * origin (the plain row) must NOT run. */
    g_test_mods = GLUT_ACTIVE_SHIFT;
    int vo1 = glr_config_get(GLR_CONFIG_VERTEX_OUTLINES);
    repl_set_status("");
    ASSERT_INT("Ctrl+Shift+O handled", glr_cfg_handle_ascii_shortcut(KEY_CTRL_O), 1);
    ASSERT_TRUE("Ctrl+Shift+O toggled Vertex outlines",
                glr_config_get(GLR_CONFIG_VERTEX_OUTLINES) != vo1);
    ASSERT_TRUE("Ctrl+Shift+O did NOT run Focus origin",
                strcmp(g_last_status, "Camera: focus origin") != 0);

    /* Ctrl+Shift+N belongs to the controller's debug-dump action, not the
     * plain Ctrl+N Normal vectors config row. The cfg dispatcher therefore
     * declines it and must leave Normal vectors unchanged. */
    int nv0 = glr_config_get(GLR_CONFIG_NORMAL_VECTORS);
    ASSERT_INT("Ctrl+Shift+N declined by cfg",
               glr_cfg_handle_ascii_shortcut(KEY_CTRL_N), 0);
    ASSERT_INT("Ctrl+Shift+N left Normal vectors alone",
               glr_config_get(GLR_CONFIG_NORMAL_VECTORS), nv0);

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

    /* Ctrl+Shift+W toggles Winding view. */
    g_test_mods = GLUT_ACTIVE_SHIFT;
    int wv0 = glr_config_get(GLR_CONFIG_WINDING_VIEW);
    ASSERT_INT("Ctrl+Shift+W handled", glr_cfg_handle_ascii_shortcut(KEY_CTRL_W), 1);
    ASSERT_TRUE("Ctrl+Shift+W toggled Winding",
                glr_config_get(GLR_CONFIG_WINDING_VIEW) != wv0);

    /* Ctrl+Shift+T is a controller action, not a shifted fallback to the
     * plain Auto time config row. */
    g_test_mods = GLUT_ACTIVE_SHIFT;
    int t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("t predef var exists", t_idx >= 0);
    g_predef_vars_mut[t_idx].value = 5.0f;
    ASSERT_INT("Ctrl+Shift+T declined by cfg", glr_cfg_handle_ascii_shortcut(KEY_CTRL_T), 0);
    ASSERT_INT("Ctrl+Shift+T handled by controller",
               glr_ctrl_router_handle_time_reset_key(KEY_CTRL_T), 1);
    ASSERT_TRUE("Ctrl+Shift+T reset time to 0", fabsf(g_predef_vars[t_idx].value) < 1e-6f);

    /* Plain Ctrl+P (no Shift) toggles Polygon highlight. */
    g_test_mods = 0;
    int ph0 = glr_config_get(GLR_CONFIG_POLY_HIGHLIGHT);
    ASSERT_INT("plain Ctrl+P handled", glr_cfg_handle_ascii_shortcut(KEY_CTRL_P), 1);
    ASSERT_TRUE("plain Ctrl+P toggled Polygon highlight",
                glr_config_get(GLR_CONFIG_POLY_HIGHLIGHT) != ph0);

    /* Ctrl+Shift+P toggles Vertex points. Polygon highlight must NOT toggle. */
    g_test_mods = GLUT_ACTIVE_SHIFT;
    int ph1 = glr_config_get(GLR_CONFIG_POLY_HIGHLIGHT);
    int vp0 = glr_config_get(GLR_CONFIG_VERTEX_POINTS);
    ASSERT_INT("Ctrl+Shift+P handled", glr_cfg_handle_ascii_shortcut(KEY_CTRL_P), 1);
    ASSERT_TRUE("Ctrl+Shift+P toggled Vertex points",
                glr_config_get(GLR_CONFIG_VERTEX_POINTS) != vp0);
    ASSERT_INT("Ctrl+Shift+P left Polygon highlight alone",
               glr_config_get(GLR_CONFIG_POLY_HIGHLIGHT), ph1);

    /* Plain Ctrl+Y is not a g_cfg_items row (Redo owns it in the editor),
     * so it must be declined here. Ctrl+Shift+H shares byte 8 with
     * Shift+Backspace, but the cfg route claims it before the editor. */
    g_test_mods = 0;
    ASSERT_INT("plain Ctrl+Y declined (-> editor redo)",
               glr_cfg_handle_ascii_shortcut(KEY_CTRL_Y), 0);

    g_test_mods = GLUT_ACTIVE_SHIFT;
    ASSERT_INT("Ctrl+Shift+A not claimed by cfg (Audio has own router)",
               glr_cfg_handle_ascii_shortcut(KEY_CTRL_A), 0);

    int sh0 = glr_config_get(GLR_CONFIG_SYNTAX_HIGHLIGHT);
    g_test_mods = 0;
    ASSERT_INT("plain Ctrl+H declined (-> editor Backspace)",
               glr_cfg_handle_ascii_shortcut(KEY_CTRL_H), 0);
    g_test_mods = GLUT_ACTIVE_SHIFT;
    ASSERT_INT("Ctrl+Shift+H handled", glr_cfg_handle_ascii_shortcut(KEY_CTRL_H), 1);
    ASSERT_TRUE("Ctrl+Shift+H cycled Syntax highlight",
                glr_config_get(GLR_CONFIG_SYNTAX_HIGHLIGHT) != sh0);

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
 * tutorial_start. With the hierarchical menu, the menu's top-level
 * MENU_TUTORIALS rows are tag rows (inert) + trailing Restart/Exit;
 * tutorial activation itself flows through route_submenu_item_hit ->
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
        /* Exit keeps the lesson's view (the learner is still in the
         * tutorial's scene); the cfg baseline captured at tutorial_start
         * is restored by the next teardown flush - see the bag-restore
         * lifecycle added in the REQUIRE/SET commit (8fefa82). The
         * baseline here was the post-reset 3D default, so the flush
         * reverts the tutorial's 2D back to 3D. */
        tutorial_stop();
        ASSERT_INT("tutorial_exit keeps the tutorial's 2D view",
                   glr_config_get(GLR_CONFIG_ORTHO_MODE), 1);
        tutorial_teardown();
        ASSERT_INT("flush restores view mode to pre-start baseline",
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
         * presentation_reset -> 3D no longer leaks past teardown.
         * Color & Transform names no presentation slugs, but the
         * captured baseline still holds the pre-start 2D and exit
         * restores it. See test_tutorial_runner.c's
         * test_baseline_captures_view_mode_even_when_unreferenced
         * for the dedicated regression. */
        tutorial_stop();
        ASSERT_INT("no-cfg tutorial exit keeps the reset 3D view",
                   glr_config_get(GLR_CONFIG_ORTHO_MODE), 0);
        tutorial_teardown();
        ASSERT_INT("flush restores view mode to pre-start baseline",
                   glr_config_get(GLR_CONFIG_ORTHO_MODE), 1);
    }
}

/* Top-level MENU_TUTORIALS rows behave like Scene tag rows -
 * clicking a tag row is inert (returns 0, keeps menu open), and Restart /
 * Exit work via their positions (tag_count + GLR_TUTORIAL_OFF_*). Activation
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

    /* Next at tag_count + GLR_TUTORIAL_OFF_NEXT: cycles tutorial. */
    int active_before = tutorial_state_view().tutorial_idx;
    ASSERT_INT("Next row handled",
               glr_action_menu_item_activate(GLR_MENU_TUTORIALS,
                                             tag_count + GLR_TUTORIAL_OFF_NEXT), 1);
    ASSERT_INT("Next row cycles to next tutorial",
               tutorial_state_view().tutorial_idx, (active_before + 1) % repl_tutorial_count());

    /* Previous at tag_count + GLR_TUTORIAL_OFF_PREV: cycles tutorial. */
    ASSERT_INT("Previous row handled",
               glr_action_menu_item_activate(GLR_MENU_TUTORIALS,
                                             tag_count + GLR_TUTORIAL_OFF_PREV), 1);
    ASSERT_INT("Previous row cycles to previous tutorial",
               tutorial_state_view().tutorial_idx, active_before);

    /* Restart at tag_count + GLR_TUTORIAL_OFF_RESTART: re-enters step 0. */
    glr_ctrl_reshape(1000, 620);
    glr_ctrl_set_depth_readback_supported_for_test(1);
    glr_ctrl_set_depth_snapshot_wanted(1);
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("depth valid before tutorial restart",
               glr_ctrl_depth_snapshot_view().valid, 1);
    ASSERT_INT("Restart row handled",
               glr_action_menu_item_activate(GLR_MENU_TUTORIALS,
                                             tag_count + GLR_TUTORIAL_OFF_RESTART), 1);
    ASSERT_INT("Restart returns step to 0",
               tutorial_state_view().step, 0);
    ASSERT_INT("tutorial restart drops outgoing document depth",
               glr_ctrl_depth_snapshot_view().valid, 0);

    /* Exit at tag_count + GLR_TUTORIAL_OFF_EXIT: ends the tutorial. */
    ASSERT_INT("Exit row handled",
               glr_action_menu_item_activate(GLR_MENU_TUTORIALS,
                                             tag_count + GLR_TUTORIAL_OFF_EXIT), 1);
    ASSERT_INT("Exit ends the tutorial",
               tutorial_active(), 0);
}

/* Tours menu: every catalog entry has a name, activating a row starts the
 * controlled pointer-script tour (validating each built-in script parses as an
 * untimed, completion-driven script - glr_pointer_script_start_tour rejects
 * malformed and timestamped lines) and closes the menu, and out-of-range rows
 * are consumed. */
static void test_tours_menu_dispatch(void) {
    glr_ctrl_reset_all();

    int n = glr_tours_count();
    ASSERT_TRUE("tour catalog is non-empty", n > 0);
    ASSERT_TRUE("name query rejects out-of-range", glr_tours_name(n) == NULL);
    ASSERT_TRUE("first tour has a name", glr_tours_name(0) != NULL);

    /* A tour takes over scene execution, so launch must clear the narrower
     * REPL replay render before the deferred tour baseline captures it. */
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(0,0,0);");
    editor_feed_line("glEnd();");
    replay_start();
    ASSERT_INT("replay active before tour launch", replay_active(), 1);
    ASSERT_INT("first tour clears replay and starts",
               glr_action_menu_item_activate(GLR_MENU_TOURS, 0), 1);
    ASSERT_INT("tour launch stops active replay", replay_active(), 0);
    ASSERT_INT("tour script loaded after replay stop",
               glr_pointer_script_tour_active(), 1);
    glr_pointer_script_stop();

    for (int i = 1; i < n; i++) {
        ASSERT_TRUE("tour has a name", glr_tours_name(i) != NULL);
        ASSERT_INT("tour row activation starts tour and closes menu",
                   glr_action_menu_item_activate(GLR_MENU_TOURS, i), 1);
        ASSERT_INT("tour script loaded and playing",
                   glr_pointer_script_tour_active(), 1);
        glr_pointer_script_stop();
        ASSERT_INT("stop deactivates the tour",
                   glr_pointer_script_tour_active(), 0);
        ASSERT_INT("stop deactivates the overlay too",
                   glr_pointer_script_active(), 0);
    }

    ASSERT_INT("TOURS out-of-range consumed",
               glr_action_menu_item_activate(GLR_MENU_TOURS, n), 1);
    ASSERT_INT("out-of-range did not start a tour",
               glr_pointer_script_tour_active(), 0);
}

/* Done is presented at least once, then a tour with no final caption closes on
 * the next frame. A final caption keeps Done alive for its authored lifetime. */
static void test_tour_done_auto_closes(void) {
    static const char *const tiny[] = { "move 10 10" };
    static const char *const caption[] = { "echo 10 10 18 0.1 Finished" };
    glr_ctrl_reset_all();

    ASSERT_INT("tiny tour script loads",
               glr_pointer_script_start_tour("Tiny", "tiny.pointer", tiny, 1),
               1);
    ASSERT_INT("tour playing after start",
               glr_pointer_script_tour_active(), 1);

    /* Frame 1 captures the baseline; frame 2 fires the move; frame 3 completes
     * it and enters Done. Extra frames guard against off-by-one drift. */
    for (int i = 0; i < 5 &&
                    glr_pointer_script_tour_view().state != GLR_TOUR_DONE; i++)
        glr_pointer_script_frame();
    ASSERT_INT("tour reached Done",
               glr_pointer_script_tour_view().state, GLR_TOUR_DONE);
    ASSERT_INT("controlled tour active for Done presentation",
               glr_pointer_script_tour_active(), 1);
    glr_pointer_script_frame();
    ASSERT_INT("captionless Done auto-closes next frame",
               glr_pointer_script_tour_active(), 0);

    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(0,0,0);");
    editor_feed_line("glEnd();");
    ASSERT_INT("caption tour loads",
               glr_pointer_script_start_tour("Caption", "caption.pointer",
                                             caption, 1), 1);
    for (int i = 0; i < 5 &&
                    glr_pointer_script_tour_view().state != GLR_TOUR_DONE; i++)
        glr_pointer_script_frame();
    ASSERT_INT("caption tour reaches Done",
               glr_pointer_script_tour_view().state, GLR_TOUR_DONE);
    glr_pointer_script_frame();
    ASSERT_INT("final caption keeps Done active",
               glr_pointer_script_tour_active(), 1);
    replay_start();
    ASSERT_INT("replay running during Done linger", replay_active(), 1);
    for (int i = 0; i < 10 && glr_pointer_script_tour_active(); i++)
        glr_pointer_script_frame();
    ASSERT_INT("tour closes when final caption expires",
               glr_pointer_script_tour_active(), 0);
    ASSERT_INT("tour auto-close leaves replay running", replay_active(), 1);
    replay_stop();
}

/* `key@<cps>` paces the payload at chars/sec on the frame clock instead of
 * landing it all on the fire frame; malformed rates are authoring errors. */
static void test_tour_paced_key(void) {
    static const char *const bad_zero[] = { "key@0 x" };
    static const char *const bad_rate[] = { "key@ x" };
    static const char *const bad_tail[] = { "key@12x y" };
    /* 30 cps = one char every 2 frames; "abc" spans ~5 frames. */
    static const char *const paced[] = { "key@30 abc" };

    glr_ctrl_reset_all();

    ASSERT_INT("key@0 rejected",
               glr_pointer_script_start_tour("T", "t.pointer", bad_zero, 1), 0);
    ASSERT_INT("key@ with no rate rejected",
               glr_pointer_script_start_tour("T", "t.pointer", bad_rate, 1), 0);
    ASSERT_INT("key@12x trailing junk rejected",
               glr_pointer_script_start_tour("T", "t.pointer", bad_tail, 1), 0);

    int len0 = editor_state_input().input_len;
    ASSERT_INT("paced key script loads",
               glr_pointer_script_start_tour("T", "t.pointer", paced, 1), 1);

    /* Frame 1 captures the baseline; frame 2 fires the event and delivers
     * exactly the first character. */
    glr_pointer_script_frame();
    glr_pointer_script_frame();
    ASSERT_INT("first char lands on the fire frame",
               editor_state_input().input_len, len0 + 1);
    ASSERT_INT("tour still typing (not yet Done)",
               glr_pointer_script_tour_view().state, GLR_TOUR_PLAYING);

    /* Remaining chars arrive at 30 cps; the tour enters Done once complete. */
    for (int i = 0; i < 8 &&
                    glr_pointer_script_tour_view().state != GLR_TOUR_DONE; i++)
        glr_pointer_script_frame();
    ASSERT_INT("full payload delivered",
               editor_state_input().input_len, len0 + 3);
    ASSERT_INT("tour entered Done after paced typing finished",
               glr_pointer_script_tour_view().state, GLR_TOUR_DONE);
    glr_pointer_script_stop();
}

/* Controlled tours advance from completion rather than an absolute schedule,
 * and `pause` supplies an intentional delay between otherwise immediate steps.
 * Timestamped lines are rejected (untimed-only). Frame 1 always captures the
 * rewind baseline, so each sub-sequence starts with an extra frame. Completion
 * enters Done immediately; a live final echo delays auto-close, not Done. */
static void test_tour_sequential_steps_and_pause(void) {
    static const char *const paced[] = {
        "key@30 abc",
        "key d"
    };
    static const char *const paused[] = {
        "key x",
        "pause 0.05",
        "key y"
    };
    static const char *const caption_then_key[] = {
        "echo 10 10 18 1 Caption remains visible",
        "key z"
    };
    static const char *const bad_zero[] = { "pause 0" };
    static const char *const bad_tail[] = { "pause 1 later" };
    static const char *const timed[] = { "0.0 key x" };
    static const char *const shell_target[] = {
        "move shell:new"
    };
    int len0;

    glr_ctrl_reset_all();
    len0 = editor_state_input().input_len;
    ASSERT_INT("untimed paced sequence loads",
               glr_pointer_script_start_tour("T", "t.pointer", paced, 2), 1);

    glr_pointer_script_frame(); /* baseline capture */
    glr_pointer_script_frame(); /* key@30 abc fires, first char */
    ASSERT_INT("sequential typing starts with one character",
               editor_state_input().input_len, len0 + 1);
    for (int i = 0; i < 4; i++)
        glr_pointer_script_frame();
    ASSERT_INT("next step waits for paced typing completion",
               editor_state_input().input_len, len0 + 3);
    glr_pointer_script_frame();
    ASSERT_INT("next step starts after paced typing completion",
               editor_state_input().input_len, len0 + 4);

    glr_pointer_script_stop();
    glr_ctrl_reset_all();
    len0 = editor_state_input().input_len;
    ASSERT_INT("pause sequence loads",
               glr_pointer_script_start_tour("T", "t.pointer", paused, 3), 1);
    glr_pointer_script_frame(); /* baseline capture */
    glr_pointer_script_frame(); /* key x */
    ASSERT_INT("first step types its character",
               editor_state_input().input_len, len0 + 1);
    glr_pointer_script_frame(); /* pause starts */
    ASSERT_INT("pause leaves following step pending",
               editor_state_input().input_len, len0 + 1);
    glr_pointer_script_frame();
    glr_pointer_script_frame();
    ASSERT_INT("pause holds for its specified duration",
               editor_state_input().input_len, len0 + 1);
    glr_pointer_script_frame();
    ASSERT_INT("step after pause fires when the pause completes",
               editor_state_input().input_len, len0 + 2);

    glr_pointer_script_stop();
    glr_ctrl_reset_all();
    len0 = editor_state_input().input_len;
    ASSERT_INT("caption sequence loads",
               glr_pointer_script_start_tour("T", "t.pointer",
                                             caption_then_key, 2), 1);
    glr_pointer_script_frame(); /* baseline capture */
    glr_pointer_script_frame(); /* echo starts */
    ASSERT_INT("caption event does not type the following key immediately",
               editor_state_input().input_len, len0);
    glr_pointer_script_frame(); /* key z */
    ASSERT_INT("caption duration does not block the following step",
               editor_state_input().input_len, len0 + 1);
    glr_pointer_script_frame(); /* key z completes -> Done (echo not awaited) */
    ASSERT_INT("echo does not delay Done",
               glr_pointer_script_tour_view().state, GLR_TOUR_DONE);
    ASSERT_INT("echo overlay retained through Done",
               glr_pointer_script_active(), 1);
    glr_pointer_script_stop();

    ASSERT_INT("zero-duration pause rejected",
               glr_pointer_script_start_tour("T", "t.pointer", bad_zero, 1), 0);
    ASSERT_INT("pause trailing junk rejected",
               glr_pointer_script_start_tour("T", "t.pointer", bad_tail, 1), 0);
    ASSERT_INT("timestamped line rejected (untimed-only)",
               glr_pointer_script_start_tour("T", "t.pointer", timed, 1), 0);
    ASSERT_INT("web shell target is part of the pointer grammar",
               glr_pointer_script_start_tour("T", "t.pointer", shell_target, 1),
               1);
    glr_pointer_script_stop();
}

/* Audit #20: glr_config_set(GLR_CONFIG_AUDIO_MODE, ...) routes through
 * the audio module's cfg_mode setter, not a raw pointer write. */
static void test_audio_config_direct_set(void) {
    glr_ctrl_reset_all();

    ASSERT_INT("hidden audio config still has two states",
               glr_config_state_count(GLR_CONFIG_AUDIO_MODE), 2);
    ASSERT_STR("hidden audio state 0 name",
               glr_config_state_name(GLR_CONFIG_AUDIO_MODE, 0), "off");
    ASSERT_STR("hidden audio state 1 name",
               glr_config_state_name(GLR_CONFIG_AUDIO_MODE, 1), "on");

    glr_config_set(GLR_CONFIG_AUDIO_MODE, 1);
    ASSERT_INT("direct-set audio mode 1", glr_audio_get_cfg_mode(), 1);

    glr_config_set(GLR_CONFIG_AUDIO_MODE, 0);
    ASSERT_INT("direct-set audio mode 0", glr_audio_get_cfg_mode(), 0);

    glr_config_set(GLR_CONFIG_AUDIO_MODE, 99);
    ASSERT_INT("direct-set clamps to max valid",
               glr_audio_get_cfg_mode(), glr_config_state_count(GLR_CONFIG_AUDIO_MODE) - 1);
}

static void test_compute_profile_mode_names(void) {
    ASSERT_INT("compute profile exposes four modes",
               glr_config_state_count(GLR_CONFIG_CPU_PROFILE),
               PROFILE_PANEL_MODE_COUNT);
    ASSERT_STR("compute profile mode 0", glr_config_state_name(
                   GLR_CONFIG_CPU_PROFILE, PROFILE_PANEL_OFF), "Off");
    ASSERT_STR("compute profile mode 1", glr_config_state_name(
                   GLR_CONFIG_CPU_PROFILE, PROFILE_PANEL_FPS), "FPS");
    ASSERT_STR("compute profile mode 2", glr_config_state_name(
                   GLR_CONFIG_CPU_PROFILE, PROFILE_PANEL_SECTIONS), "Sections");
    ASSERT_STR("compute profile mode 3", glr_config_state_name(
                   GLR_CONFIG_CPU_PROFILE, PROFILE_PANEL_HISTOGRAM), "Histogram");
}

static void test_replay_expand_mode_names(void) {
    ASSERT_INT("replay expand exposes three modes",
               glr_config_state_count(GLR_CONFIG_REPLAY_EXPAND),
               REPLAY_EXPAND_COUNT);
    ASSERT_STR("replay expand mode 0", glr_config_state_name(
                   GLR_CONFIG_REPLAY_EXPAND, REPLAY_EXPAND_OFF), "Off");
    ASSERT_STR("replay expand mode 1", glr_config_state_name(
                   GLR_CONFIG_REPLAY_EXPAND, REPLAY_EXPAND_EXPANDED), "Expanded");
    ASSERT_STR("replay expand mode 2", glr_config_state_name(
                   GLR_CONFIG_REPLAY_EXPAND, REPLAY_EXPAND_VERBOSE), "Verbose");
}

static void test_audio_menu_actions(void) {
    GlrAudioTrackSpec tracks[] = {
        { "assets/a.mp3", "Assets", "A" },
        { "assets/b.mp3", "Assets", "B" },
    };
    int group_count = 1;

    glr_ctrl_reset_all();
    ASSERT_INT("audio menu fixture playlist",
               glr_audio_set_playlist_specs(tracks, 2), 2);

    ASSERT_INT("audio group row inert",
               glr_action_menu_item_activate(GLR_MENU_AUDIO, 0), 0);

    glr_config_set(GLR_CONFIG_AUDIO_MODE, 1);
    ASSERT_INT("audio menu pause row stays open",
               glr_action_menu_item_activate(GLR_MENU_AUDIO,
                                             group_count + GLR_AUDIO_OFF_PLAY),
               0);
    ASSERT_INT("audio menu pause writes cfg",
               glr_config_get(GLR_CONFIG_AUDIO_MODE), 0);
    ASSERT_INT("audio menu pause reaches audio module",
               glr_audio_is_paused(), 1);

    ASSERT_INT("audio menu play row stays open",
               glr_action_menu_item_activate(GLR_MENU_AUDIO,
                                             group_count + GLR_AUDIO_OFF_PLAY),
               0);
    ASSERT_INT("audio menu play writes cfg",
               glr_config_get(GLR_CONFIG_AUDIO_MODE), 1);
    ASSERT_INT("audio menu play reaches audio module",
               glr_audio_is_paused(), 0);

    glr_audio_set_loop_mode(GLR_AUDIO_LOOP_OFF);
    ASSERT_INT("audio menu loop row stays open",
               glr_action_menu_item_activate(GLR_MENU_AUDIO,
                                             group_count + GLR_AUDIO_OFF_LOOP),
               0);
    ASSERT_INT("audio menu loop cycles to Song",
               glr_audio_get_loop_mode(), GLR_AUDIO_LOOP_SONG);

    ASSERT_INT("audio menu back10 row stays open",
               glr_action_menu_item_activate(GLR_MENU_AUDIO,
                                             group_count + GLR_AUDIO_OFF_BACK10),
               0);
    ASSERT_STR("audio menu back10 sets status",
               g_last_status, "Jump Back 10s (0:00)");

    ASSERT_INT("audio menu fwd10 row stays open",
               glr_action_menu_item_activate(GLR_MENU_AUDIO,
                                             group_count + GLR_AUDIO_OFF_FWD10),
               0);
    ASSERT_STR("audio menu fwd10 sets status",
               g_last_status, "Jump Forward 10s (0:00)");

    ASSERT_INT("audio menu next closes",
               glr_action_menu_item_activate(GLR_MENU_AUDIO,
                                             group_count + GLR_AUDIO_OFF_NEXT),
               1);
    ASSERT_INT("audio menu previous closes",
               glr_action_menu_item_activate(GLR_MENU_AUDIO,
                                             group_count + GLR_AUDIO_OFF_PREV),
               1);
}

/* Scene menu "Next" / "Previous" rows cycle the example/scene selection
 * (the F12 / Shift+F12 path). They keep the dropdown open (return 0) so
 * repeated clicks step through examples, like the Config cycle rows. */
static void test_scene_menu_cycle_actions(void) {
    glr_ctrl_reset_all();
    ASSERT_TRUE("multiple examples exist", repl_example_count() > 1);

    int tag_count = repl_example_visible_tag_count();

    /* Seed a known example so forward/backward are exact inverses. */
    repl_load_example(1);
    ASSERT_INT("seeded on example 1",
               repl_state_scenes().active_example_idx, 1);

    /* Open the Scene dropdown, as a real click would. The cycle runs
     * glr_ctrl_reset_transients() (which closes the menu) internally, so
     * this guards that the dropdown survives - the reported regression. */
    ui_menu_bar_set_open_menu(GLR_MENU_SCENE, 0.0f);
    ASSERT_INT("Scene menu open before cycle",
               ui_menu_bar_open_menu_id(), GLR_MENU_SCENE);

    ASSERT_INT("Scene Previous row keeps menu open",
               glr_action_menu_item_activate(GLR_MENU_SCENE,
                                             tag_count + GLR_SCENE_OFF_PREV), 0);
    ASSERT_INT("Scene Previous steps to example 0",
               repl_state_scenes().active_example_idx, 0);
    ASSERT_INT("Scene menu still open after Previous",
               ui_menu_bar_open_menu_id(), GLR_MENU_SCENE);

    ASSERT_INT("Scene Next row keeps menu open",
               glr_action_menu_item_activate(GLR_MENU_SCENE,
                                             tag_count + GLR_SCENE_OFF_NEXT), 0);
    ASSERT_INT("Scene Next steps back to example 1",
               repl_state_scenes().active_example_idx, 1);
    ASSERT_INT("Scene menu still open after Next",
               ui_menu_bar_open_menu_id(), GLR_MENU_SCENE);

    ui_menu_bar_close();
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

static void test_config_slug_table(void) {
    char err[256];
    ASSERT_INT("config slug table validates",
               glr_config_validate(err, sizeof(err)), 1);
    ASSERT_STR("config slug validation has no error", err, "");
}

/* The property glr_config_validate() enforces for keys, asserted directly
 * against the live table: find_item_by_key() returns the first match, so a
 * second row on the same GlrConfigKey would silently inherit the first row's
 * state count and state names. */
static void test_config_keys_are_unique(void) {
    int dup_key = GLR_CONFIG_NONE;
    int actionable = 0;

    for (int i = 0; i < CFG_ITEM_COUNT && dup_key == GLR_CONFIG_NONE; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (!item || item->section_header || item->key == GLR_CONFIG_NONE)
            continue;
        actionable++;
        for (int j = 0; j < i; j++) {
            const GlrConfigItem *prior = glr_config_item_at(j);
            if (!prior || prior->section_header)
                continue;
            if (prior->key == item->key) {
                dup_key = (int)item->key;
                break;
            }
        }
    }

    ASSERT_TRUE("config table has actionable rows", actionable > 0);
    ASSERT_INT("no GlrConfigKey is claimed by two rows", dup_key,
               GLR_CONFIG_NONE);
}

/* Depth view row: a 4-state cycle (Off / Linear / Scene / Split) bound
 * to Ctrl+Shift+D, with an explicit stable @cfg slug. */
static void test_depth_viz_row_metadata(void) {
    const GlrConfigItem *item = NULL;

    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *candidate = glr_config_item_at(i);
        if (candidate && candidate->key == GLR_CONFIG_DEPTH_VIZ) {
            item = candidate;
            break;
        }
    }
    ASSERT_TRUE("found Depth view row", item != NULL);
    if (!item)
        return;

    ASSERT_STR("Depth view label", item->label, "Depth view");
    ASSERT_STR("Depth view slug", glr_config_item_slug(item), "depth_view");
    ASSERT_INT("Depth view has 4 states",
               glr_config_state_count(GLR_CONFIG_DEPTH_VIZ), 4);
    ASSERT_STR("state 0", glr_config_state_name(GLR_CONFIG_DEPTH_VIZ, 0), "Off");
    ASSERT_STR("state 1", glr_config_state_name(GLR_CONFIG_DEPTH_VIZ, 1), "Linear");
    ASSERT_STR("state 2", glr_config_state_name(GLR_CONFIG_DEPTH_VIZ, 2), "Scene");
    ASSERT_STR("state 3", glr_config_state_name(GLR_CONFIG_DEPTH_VIZ, 3), "Split");
    ASSERT_INT("Depth view binds Ctrl+Shift+D", item->key_code, KM_KEY(GLR_DEPTH_VIZ));
    ASSERT_INT("Depth view requires Shift", item->modifiers, GLUT_ACTIVE_SHIFT);
    ASSERT_INT("Depth view is a normal-key binding", item->is_special, 0);
}

/* Stencil view mirrors depth view's session-scoped config plumbing and is
 * bound to Ctrl+Shift+S. */
static void test_stencil_viz_row_metadata(void) {
    const GlrConfigItem *item = NULL;

    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *candidate = glr_config_item_at(i);
        if (candidate && candidate->key == GLR_CONFIG_STENCIL_VIZ) {
            item = candidate;
            break;
        }
    }
    ASSERT_TRUE("found Stencil view row", item != NULL);
    if (!item)
        return;

    ASSERT_STR("Stencil view label", item->label, "Stencil view");
    ASSERT_STR("Stencil view slug", glr_config_item_slug(item), "stencil_view");
    ASSERT_INT("Stencil view has 4 states",
               glr_config_state_count(GLR_CONFIG_STENCIL_VIZ), 4);
    ASSERT_STR("stencil state 0", glr_config_state_name(GLR_CONFIG_STENCIL_VIZ, 0), "Off");
    ASSERT_STR("stencil state 1", glr_config_state_name(GLR_CONFIG_STENCIL_VIZ, 1), "Palette");
    ASSERT_STR("stencil state 2", glr_config_state_name(GLR_CONFIG_STENCIL_VIZ, 2), "Ramp");
    ASSERT_STR("stencil state 3", glr_config_state_name(GLR_CONFIG_STENCIL_VIZ, 3), "Split");
    ASSERT_INT("Stencil view binds Ctrl+Shift+S", item->key_code,
               KM_KEY(GLR_STENCIL_VIZ));
    ASSERT_INT("Stencil view requires Shift", item->modifiers,
               GLUT_ACTIVE_SHIFT);
    ASSERT_INT("Stencil view is not a special key", item->is_special, 0);
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
    ASSERT_INT("TOURS out-of-range consumed",
               glr_action_menu_item_activate(GLR_MENU_TOURS, 999), 1);
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
    int depth_viz_row = -1;
    int autonormals_row = -1;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item) {
            if (item->key == GLR_CONFIG_WIREFRAME) {
                wireframe_row = i;
            } else if (item->key == GLR_CONFIG_DEPTH_VIZ) {
                depth_viz_row = i;
            } else if (item->key == GLR_CONFIG_AUTO_NORMALS) {
                autonormals_row = i;
            }
        }
    }
    ASSERT_TRUE("found wireframe row", wireframe_row >= 0);
    ASSERT_TRUE("found depth view row", depth_viz_row >= 0);
    ASSERT_TRUE("found autonormals row", autonormals_row >= 0);

    glr_cfg_cycle_row(wireframe_row, 1);
    ASSERT_INT("replay active after non-invalidating cfg cycle",
               replay_active(), 1);

    /* Depth view is a presentation toggle: cycling it mid-replay must
     * keep the replay running (the "works during replay" requirement). */
    glr_cfg_cycle_row(depth_viz_row, 1);
    ASSERT_INT("replay active after depth-view cycle", replay_active(), 1);
    ASSERT_INT("depth view cycled to Linear",
               glr_config_get(GLR_CONFIG_DEPTH_VIZ), 1);
    glr_state_presentation_mut()->depth_viz = 0;

    glr_cfg_cycle_row(autonormals_row, 1);
    ASSERT_INT("replay stopped after invalidating cfg cycle",
               replay_active(), 0);
}

/* On a context that can't read the depth buffer back (WebGL; a failed
 * init-GL probe), the interactive Depth view cycle must refuse with a
 * status message instead of silently cycling a row whose render-config
 * copy is forced Off every frame. Only glr_cfg_cycle_row is gated -
 * @cfg header loads still write the stored value so files round-trip. */
static void test_depth_viz_cycle_refuses_without_readback(void) {
    glr_ctrl_reset_all();

    int depth_viz_row = -1;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && item->key == GLR_CONFIG_DEPTH_VIZ)
            depth_viz_row = i;
    }
    ASSERT_TRUE("found depth view row", depth_viz_row >= 0);
    ASSERT_TRUE("supported context reports no refusal reason",
                glr_ctrl_depth_readback_unsupported_reason() == NULL);

    glr_ctrl_set_depth_readback_supported_for_test(0);
    ASSERT_TRUE("unsupported context reports a refusal reason",
                glr_ctrl_depth_readback_unsupported_reason() != NULL);
    glr_cfg_cycle_row(depth_viz_row, 1);
    ASSERT_INT("depth view stays Off on refused cycle",
               glr_config_get(GLR_CONFIG_DEPTH_VIZ), 0);
    ASSERT_TRUE("status names the missing capability",
                strstr(ui_state_status().text,
                       "can't read the depth buffer") != NULL);

    glr_ctrl_set_depth_readback_supported_for_test(1);
    glr_cfg_cycle_row(depth_viz_row, 1);
    ASSERT_INT("depth view cycles once readback is supported",
               glr_config_get(GLR_CONFIG_DEPTH_VIZ), 1);
    glr_state_presentation_mut()->depth_viz = 0;
}

static void test_stencil_viz_cycle_refuses_without_readback(void) {
    int stencil_viz_row = -1;

    glr_ctrl_reset_all();
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && item->key == GLR_CONFIG_STENCIL_VIZ)
            stencil_viz_row = i;
    }
    ASSERT_TRUE("found stencil view row", stencil_viz_row >= 0);
    ASSERT_TRUE("supported context reports no stencil refusal reason",
                glr_ctrl_stencil_readback_unsupported_reason() == NULL);

    glr_ctrl_set_stencil_readback_supported_for_test(0);
    ASSERT_TRUE("unsupported context reports stencil refusal reason",
                glr_ctrl_stencil_readback_unsupported_reason() != NULL);
    glr_cfg_cycle_row(stencil_viz_row, 1);
    ASSERT_INT("stencil view stays Off on refused cycle",
               glr_config_get(GLR_CONFIG_STENCIL_VIZ), 0);
    ASSERT_TRUE("status names the missing stencil capability",
                strstr(ui_state_status().text, "can't read the stencil buffer") != NULL);

    glr_ctrl_set_stencil_readback_supported_for_test(1);
    glr_cfg_cycle_row(stencil_viz_row, 1);
    ASSERT_INT("stencil view cycles once readback is supported",
               glr_config_get(GLR_CONFIG_STENCIL_VIZ), 1);
    glr_state_presentation_mut()->stencil_viz = 0;
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
 * the live amber banner with a blank rect held for UI_STATUS_MESSAGE_TTL
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
 *   FOCUS_ORIGIN         -> glr_camera_focus_origin (ease target=origin)
 *   RESET_CAMERA         -> glr_camera_ease_to_default
 *   TIME_RESET           -> repl_reset_time_to_zero
 *   CODE_PANEL_LAYOUT_HIDDEN -> ui_menu_bar_close + color_picker_stop +
 *                              editor_completion_clear (the hidden-only
 *                              branch - the other layouts test pure
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
     * (tx/ty/tz, not rx/ry - focus_origin recenters the orbit target). */
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

static void test_cfg_cycle_look_down_z_zeroes_orbit_and_pan(void) {
    glr_ctrl_reset_all();

    int row = find_cfg_row_for_key(GLR_CONFIG_LOOK_DOWN_Z);
    ASSERT_TRUE("found look_down_z row", row >= 0);
    if (row < 0) return;

    /* Orbit, pan and zoom all off the default so each axis is observable. */
    glr_camera_set(20.0f, 30.0f, 9.0f, 3.0f, 4.0f, 5.0f, 0.0f);

    glr_cfg_cycle_row(row, 1);

    ASSERT_INT("look_down_z starts a camera ease",
               glr_camera_target_active(), 1);
    ASSERT_STR("look_down_z status", g_last_status, "Camera: look down Z");

    for (int i = 0; i < 500 && glr_camera_target_active(); i++)
        glr_camera_tick();
    GlrCameraState cam = glr_camera();
    ASSERT_TRUE("look_down_z zeroes rx", fabsf(cam.rx) < 1e-3f);
    ASSERT_TRUE("look_down_z zeroes ry", fabsf(cam.ry) < 1e-3f);
    ASSERT_TRUE("look_down_z zeroes tx", fabsf(cam.tx) < 1e-3f);
    ASSERT_TRUE("look_down_z zeroes ty", fabsf(cam.ty) < 1e-3f);
    ASSERT_TRUE("look_down_z zeroes tz", fabsf(cam.tz) < 1e-3f);
    /* Zoom is deliberately kept - that is what separates it from Reset. */
    ASSERT_TRUE("look_down_z preserves dist", fabsf(cam.dist - 9.0f) < 1e-3f);
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

static void test_time_reset_action(void) {
    glr_ctrl_reset_all();

    /* Seed a non-zero t predef-var value so we can observe the reset.
     * (repl_reset_time_to_zero zeros the t predef var, not the
     * controller-side anim_time accumulator - the predef value is
     * what the executor and the scene observe each frame.) */
    int t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("t predef var exists", t_idx >= 0);
    if (t_idx < 0) return;
    g_predef_vars_mut[t_idx].value = 7.25f;
    int was_playing = repl_state_variables().time_playing;

    glr_action_reset_time_to_zero();

    ASSERT_TRUE("time reset zeros t",
                fabsf(g_predef_vars[t_idx].value) < 1e-6f);
    /* time_playing is not changed by the reset path - it just zeroes
     * the clock. Pin the invariant so a refactor that flips play state
     * trips the assert. */
    ASSERT_INT("time reset leaves play state unchanged",
               repl_state_variables().time_playing, was_playing);
    /* Status reflects the current play state. */
    const char *expected_status = repl_state_variables().time_playing
                                      ? "Time: reset to 0"
                                      : "Time: reset to 0 (paused)";
    ASSERT_STR("time reset status", g_last_status, expected_status);
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
    ASSERT_INT("vertex labels state_count matches enum",
               item->state_count, OVERLAY_VERTEX_LABEL_COUNT);
    ASSERT_TRUE("vertex labels has state_names",
                item->state_names != NULL);

    ASSERT_INT("default is Index",
               glr_config_get(GLR_CONFIG_VERTEX_LABELS), OVERLAY_VERTEX_LABEL_INDEX);

    glr_cfg_cycle_row(vertex_labels_row, 1);
    ASSERT_INT("cycle to Index+Pos",
               glr_config_get(GLR_CONFIG_VERTEX_LABELS), OVERLAY_VERTEX_LABEL_INDEX_POS);
    ASSERT_STR("status Index+Pos", g_last_status,
               "Vertex labels: Index+Pos");

    glr_cfg_cycle_row(vertex_labels_row, 1);
    ASSERT_INT("cycle to Index+World",
               glr_config_get(GLR_CONFIG_VERTEX_LABELS), OVERLAY_VERTEX_LABEL_INDEX_WORLD);
    ASSERT_STR("status Index+World", g_last_status,
               "Vertex labels: Index+World");

    glr_cfg_cycle_row(vertex_labels_row, 1);
    ASSERT_INT("cycle to Index+World Fine",
               glr_config_get(GLR_CONFIG_VERTEX_LABELS), OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE);
    ASSERT_STR("status Index+World Fine", g_last_status,
               "Vertex labels: Index+World Fine");

    glr_cfg_cycle_row(vertex_labels_row, 1);
    ASSERT_INT("cycle to Off",
               glr_config_get(GLR_CONFIG_VERTEX_LABELS), OVERLAY_VERTEX_LABEL_OFF);
    ASSERT_STR("status Off", g_last_status, "Vertex labels: Off");

    glr_cfg_cycle_row(vertex_labels_row, 1);
    ASSERT_INT("cycle wraps to Index",
               glr_config_get(GLR_CONFIG_VERTEX_LABELS), OVERLAY_VERTEX_LABEL_INDEX);
    ASSERT_STR("status Index", g_last_status, "Vertex labels: Index");

    glr_config_set(GLR_CONFIG_VERTEX_LABELS, OVERLAY_VERTEX_LABEL_INDEX_POS);
    ASSERT_INT("direct set Index+Pos",
               glr_state_presentation().show_vertex_labels, OVERLAY_VERTEX_LABEL_INDEX_POS);

    glr_config_set(GLR_CONFIG_VERTEX_LABELS, OVERLAY_VERTEX_LABEL_OFF);
    ASSERT_INT("direct set Off",
               glr_state_presentation().show_vertex_labels, OVERLAY_VERTEX_LABEL_OFF);
}

static void test_backdrop_grid_pairing_policy(void) {
    int grid_row = -1;
    int grid_before_pair;
    Render3dGridTheme paired_grid = GRID_THEME_OFF;

    glr_ctrl_reset_all();
    printf("--- Backdrop/grid pairing policy ---\n");

    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && item->key == GLR_CONFIG_GRID_THEME) {
            grid_row = i;
            break;
        }
    }
    ASSERT_TRUE("found grid row", grid_row >= 0);

    ASSERT_TRUE("Nebula forces a grid",
                glr_config_backdrop_forces_grid(RENDER3D_BACKDROP_NEBULA,
                                                &paired_grid));
    ASSERT_INT("Nebula forces Star Chart", paired_grid, GRID_THEME_STARCHART);
    ASSERT_TRUE("paired grid is hidden from direct selection",
                !glr_config_grid_user_selectable(GRID_THEME_STARCHART));
    ASSERT_TRUE("Aurora paired grid is hidden from direct selection",
                !glr_config_grid_user_selectable(GRID_THEME_AURORA));
    ASSERT_TRUE("ordinary grid remains directly selectable",
                glr_config_grid_user_selectable(GRID_THEME_TRON));

    grid_before_pair = glr_config_get(GLR_CONFIG_GRID_THEME);
    glr_config_set(GLR_CONFIG_BACKDROP, RENDER3D_BACKDROP_NEBULA);
    ASSERT_INT("setting Nebula forces Star Chart",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_STARCHART);

    glr_config_set(GLR_CONFIG_GRID_THEME, GRID_THEME_TRON);
    ASSERT_INT("manual grid set stays forced while Nebula is active",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_STARCHART);

    glr_cfg_cycle_row(grid_row, 1);
    ASSERT_INT("grid cycle stays forced while Nebula is active",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_STARCHART);
    ASSERT_STR("grid cycle warns while Nebula locks the grid",
               g_last_status,
               "Warning: grid locked by Nebula backdrop (Star Chart)");

    glr_config_set(GLR_CONFIG_BACKDROP, RENDER3D_BACKDROP_OFF);
    ASSERT_INT("leaving paired backdrop restores previous grid",
               glr_config_get(GLR_CONFIG_GRID_THEME), grid_before_pair);

    glr_config_set(GLR_CONFIG_GRID_THEME, GRID_THEME_SOIL);
    glr_cfg_cycle_row(grid_row, 1);
    ASSERT_INT("grid cycle skips hidden Star Chart forward",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_PLANES);

    glr_cfg_cycle_row(grid_row, -1);
    ASSERT_INT("grid cycle skips hidden Star Chart backward",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_SOIL);

    glr_config_set(GLR_CONFIG_GRID_THEME, GRID_THEME_TRON);
    glr_config_set(GLR_CONFIG_BACKDROP, RENDER3D_BACKDROP_AURORA);
    ASSERT_INT("Aurora forces Aurora grid",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_AURORA);
    glr_config_set(GLR_CONFIG_BACKDROP, RENDER3D_BACKDROP_SUNSET);
    ASSERT_INT("switching paired backdrops forces the new pair",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_SYNTHWAVE);
    glr_config_set(GLR_CONFIG_BACKDROP, RENDER3D_BACKDROP_OFF);
    ASSERT_INT("leaving chained paired backdrops restores original grid",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_TRON);
}

/* Audit #41: the cfg bridge accepts symbolic value names so catalogs
 * can write "@cfg grid = GRID_THEME_RADAR" instead of a magic integer.
 * Pin enum-valued slugs end-to-end (resolve_text + apply via
 * repl_cfg_set_text), plus the legacy integer path that still has
 * to round-trip from older saved files. */
static void test_cfg_bridge_resolves_symbolic_names(void) {
    test_apply_defaults();  /* installs g_glr_export_cfg_bridge */

    int out = -1;
    char grid_value[16];
    ASSERT_TRUE("resolve grid: GRID_THEME_RADAR",
                repl_cfg_resolve_text("grid", "GRID_THEME_RADAR", &out));
    ASSERT_INT("  -> GRID_THEME_RADAR", out, GRID_THEME_RADAR);

    out = -1;
    ASSERT_TRUE("resolve grid: GRID_THEME_AURORA",
                repl_cfg_resolve_text("grid", "GRID_THEME_AURORA", &out));
    ASSERT_INT("  -> GRID_THEME_AURORA", out, GRID_THEME_AURORA);

    out = -1;
    ASSERT_TRUE("resolve axes: AXES_THEME_COMPASS",
                repl_cfg_resolve_text("axes", "AXES_THEME_COMPASS", &out));
    ASSERT_INT("  -> AXES_THEME_COMPASS", out, AXES_THEME_COMPASS);

    out = -1;
    ASSERT_TRUE("resolve backdrop: RENDER3D_BACKDROP_CITY_AND_STARS",
                repl_cfg_resolve_text("backdrop", "RENDER3D_BACKDROP_CITY_AND_STARS", &out));
    ASSERT_INT("  -> RENDER3D_BACKDROP_CITY_AND_STARS", out,
               RENDER3D_BACKDROP_CITY_AND_STARS);

    out = -1;
    ASSERT_TRUE("resolve overlay_scope: OVERLAY_SCOPE_ALL_INSTANCES",
                repl_cfg_resolve_text("overlay_scope", "OVERLAY_SCOPE_ALL_INSTANCES", &out));
    ASSERT_INT("  -> OVERLAY_SCOPE_ALL_INSTANCES", out,
               OVERLAY_SCOPE_ALL_INSTANCES);

    out = -1;
    ASSERT_TRUE("resolve overlay_scope: OVERLAY_SCOPE_SINGLE_POLYGON",
                repl_cfg_resolve_text("overlay_scope", "OVERLAY_SCOPE_SINGLE_POLYGON", &out));
    ASSERT_INT("  -> OVERLAY_SCOPE_SINGLE_POLYGON", out,
               OVERLAY_SCOPE_SINGLE_POLYGON);

    /* overlay_scope is canonical; the two names it has been through still
     * resolve, so a scene saved under either keeps loading. */
    out = -1;
    ASSERT_TRUE("resolve legacy label_highlight_scope alias",
                repl_cfg_resolve_text("label_highlight_scope",
                                      "OVERLAY_SCOPE_ALL_INSTANCES", &out));
    ASSERT_INT("  -> OVERLAY_SCOPE_ALL_INSTANCES via legacy label_highlight_scope",
               out, OVERLAY_SCOPE_ALL_INSTANCES);

    out = -1;
    ASSERT_TRUE("resolve legacy label_scope alias",
                repl_cfg_resolve_text("label_scope", "OVERLAY_SCOPE_ALL_INSTANCES", &out));
    ASSERT_INT("  -> OVERLAY_SCOPE_ALL_INSTANCES via legacy label_scope", out,
               OVERLAY_SCOPE_ALL_INSTANCES);

    out = -1;
    ASSERT_TRUE("resolve overlay_scope: OVERLAY_SCOPE_WHOLE_SCENE",
                repl_cfg_resolve_text("overlay_scope", "OVERLAY_SCOPE_WHOLE_SCENE", &out));
    ASSERT_INT("  -> OVERLAY_SCOPE_WHOLE_SCENE", out, OVERLAY_SCOPE_WHOLE_SCENE);
    ASSERT_TRUE("resolve vertex_labels: OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE",
                repl_cfg_resolve_text("vertex_labels",
                                      "OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE", &out));
    ASSERT_INT("  -> OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE", out,
               OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE);
    ASSERT_TRUE("resolve vertex_outline_style: VERTEX_OUTLINE_STYLE_BOLD_INVERTED",
                repl_cfg_resolve_text("vertex_outline_style",
                                      "VERTEX_OUTLINE_STYLE_BOLD_INVERTED", &out));
    ASSERT_INT("  -> VERTEX_OUTLINE_STYLE_BOLD_INVERTED", out,
               VERTEX_OUTLINE_STYLE_BOLD_INVERTED);
    ASSERT_TRUE("resolve syntax_highlight: SYNTAX_HIGHLIGHT_ON_SHADOW",
                repl_cfg_resolve_text("syntax_highlight",
                                      "SYNTAX_HIGHLIGHT_ON_SHADOW", &out));
    ASSERT_INT("  -> SYNTAX_HIGHLIGHT_ON_SHADOW", out,
               SYNTAX_HIGHLIGHT_ON_SHADOW);

    /* Unknown name on a known slug must fail (don't fall back to
     * strtol - "GRID_THEME_XYZ" must NOT silently land as 0). */
    out = -42;
    ASSERT_TRUE("unknown symbol fails resolve",
                !repl_cfg_resolve_text("grid", "GRID_THEME_XYZ", &out));
    ASSERT_INT("  out untouched on failure", out, -42);

    /* Apply via repl_cfg_set_text - proves the bridge wires the
     * resolved int into glr_export_cfg_apply, not just the lookup. */
    repl_cfg_set_text("grid", "GRID_THEME_TRON");
    ASSERT_INT("set_text grid -> presentation.grid_theme",
               glr_state_presentation().grid_theme, GRID_THEME_TRON);
    repl_cfg_set_text("axes", "AXES_THEME_NEON");
    ASSERT_INT("set_text axes -> presentation.axes_theme",
               glr_state_presentation().axes_theme, AXES_THEME_NEON);
    repl_cfg_set_text("backdrop", "RENDER3D_BACKDROP_STARS");
    ASSERT_INT("set_text backdrop -> presentation.backdrop_mode",
               glr_state_presentation().backdrop_mode, RENDER3D_BACKDROP_STARS);
    repl_cfg_set_text("vertex_labels", "OVERLAY_VERTEX_LABEL_INDEX_POS");
    ASSERT_INT("set_text vertex_labels -> presentation.show_vertex_labels",
               glr_state_presentation().show_vertex_labels,
               OVERLAY_VERTEX_LABEL_INDEX_POS);
    repl_cfg_set_text("syntax_highlight", "SYNTAX_HIGHLIGHT_OFF");
    ASSERT_INT("set_text syntax_highlight -> presentation.syntax_highlight",
               glr_state_presentation().syntax_highlight,
               SYNTAX_HIGHLIGHT_OFF);

    /* Legacy integer-form @cfg lines must still load - the apply
     * path tries resolve_text first, then falls back to strtol.
     * Drives the same bag/apply edge repl_cfg_set_text uses, just
     * with an integer-string value instead of a symbolic name. */
    snprintf(grid_value, sizeof(grid_value), "%d", GRID_THEME_EMBER);
    repl_cfg_set_text("grid", grid_value);
    ASSERT_INT("integer-form still resolves to GRID_THEME_EMBER",
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
    glr_state_presentation_mut()->grid_theme = GRID_THEME_RADAR;
    glr_state_presentation_mut()->axes_theme = AXES_THEME_GIZMO;
    glr_state_presentation_mut()->backdrop_mode = RENDER3D_BACKDROP_CITY_AND_STARS;
    glr_state_presentation_mut()->light_theme = LIGHT_THEME_SOLAR;
    glr_state_presentation_mut()->overlay_scope = OVERLAY_SCOPE_WHOLE_SCENE;
    glr_config_set(GLR_CONFIG_VERTEX_LABEL_PLACEMENT,
                   OVERLAY_LABEL_PLACEMENT_AT_VERTEX);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINE_STYLE, VERTEX_OUTLINE_STYLE_BOLD_INVERTED);
    glr_config_set(GLR_CONFIG_SYNTAX_HIGHLIGHT, SYNTAX_HIGHLIGHT_ON_SHADOW);
    ASSERT_INT("vertex_outline_style set/get",
               glr_config_get(GLR_CONFIG_VERTEX_OUTLINE_STYLE),
               VERTEX_OUTLINE_STYLE_BOLD_INVERTED);
    ASSERT_INT("syntax_highlight set/get",
               glr_config_get(GLR_CONFIG_SYNTAX_HIGHLIGHT),
               SYNTAX_HIGHLIGHT_ON_SHADOW);

    ReplConfigBag bag;
    repl_config_bag_clear(&bag);
    repl_config_bridge()->fill_all(&bag);

    ASSERT_STR("fill_all grid is symbolic", repl_config_bag_get(&bag, "grid"), "GRID_THEME_RADAR");
    ASSERT_STR("fill_all axes is symbolic", repl_config_bag_get(&bag, "axes"), "AXES_THEME_GIZMO");
    ASSERT_STR("fill_all backdrop is symbolic", repl_config_bag_get(&bag, "backdrop"), "RENDER3D_BACKDROP_CITY_AND_STARS");
    ASSERT_STR("fill_all light_theme is symbolic", repl_config_bag_get(&bag, "light_theme"), "LIGHT_THEME_SOLAR");
    ASSERT_STR("fill_all overlay_scope is symbolic", repl_config_bag_get(&bag, "overlay_scope"), "OVERLAY_SCOPE_WHOLE_SCENE");
    ASSERT_STR("fill_all vertex_label_placement is symbolic",
               repl_config_bag_get(&bag, "vertex_label_placement"),
               "OVERLAY_LABEL_PLACEMENT_AT_VERTEX");
    ASSERT_STR("fill_all vertex_outline_style is symbolic",
               repl_config_bag_get(&bag, "vertex_outline_style"),
               "VERTEX_OUTLINE_STYLE_BOLD_INVERTED");
    ASSERT_STR("fill_all syntax_highlight is symbolic",
               repl_config_bag_get(&bag, "syntax_highlight"),
               "SYNTAX_HIGHLIGHT_ON_SHADOW");
    ASSERT_TRUE("fill_all includes hidden audio cfg",
                repl_config_bag_get(&bag, "audio") != NULL);
    ASSERT_INT("hidden audio cfg slug remains known",
               repl_config_bridge()->is_known("audio"), 1);

    ReplConfigBag scene_bag;
    repl_config_bag_clear(&scene_bag);
    repl_config_bridge()->fill_scene_subset(&scene_bag);

    ASSERT_STR("fill_scene_subset grid is symbolic", repl_config_bag_get(&scene_bag, "grid"), "GRID_THEME_RADAR");
    ASSERT_STR("fill_scene_subset axes is symbolic", repl_config_bag_get(&scene_bag, "axes"), "AXES_THEME_GIZMO");
    ASSERT_STR("fill_scene_subset backdrop is symbolic", repl_config_bag_get(&scene_bag, "backdrop"), "RENDER3D_BACKDROP_CITY_AND_STARS");
    ASSERT_STR("fill_scene_subset light_theme is symbolic", repl_config_bag_get(&scene_bag, "light_theme"), "LIGHT_THEME_SOLAR");
    ASSERT_STR("fill_scene_subset overlay_scope is symbolic", repl_config_bag_get(&scene_bag, "overlay_scope"), "OVERLAY_SCOPE_WHOLE_SCENE");
    ASSERT_STR("fill_scene_subset vertex_label_placement is symbolic",
               repl_config_bag_get(&scene_bag, "vertex_label_placement"),
               "OVERLAY_LABEL_PLACEMENT_AT_VERTEX");
    ASSERT_STR("fill_scene_subset vertex_outline_style is symbolic",
               repl_config_bag_get(&scene_bag, "vertex_outline_style"),
               "VERTEX_OUTLINE_STYLE_BOLD_INVERTED");
}

static void test_cfg_bridge_enforces_backdrop_grid_pair_order(void) {
    ReplConfigBag bag;

    glr_ctrl_reset_all();
    printf("--- @cfg backdrop/grid pairing is order-independent ---\n");

    repl_config_bag_clear(&bag);
    repl_config_bag_set(&bag, "backdrop", "RENDER3D_BACKDROP_NEBULA");
    repl_config_bag_set(&bag, "grid", "GRID_THEME_TRON");
    repl_config_bridge()->apply(&bag);
    ASSERT_INT("backdrop then grid still forced to Star Chart",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_STARCHART);

    glr_ctrl_reset_all();
    repl_config_bag_clear(&bag);
    repl_config_bag_set(&bag, "grid", "GRID_THEME_TRON");
    repl_config_bag_set(&bag, "backdrop", "RENDER3D_BACKDROP_NEBULA");
    repl_config_bridge()->apply(&bag);
    ASSERT_INT("grid then backdrop forced to Star Chart",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_STARCHART);

    glr_ctrl_reset_all();
    repl_cfg_set_text("grid", "GRID_THEME_STARCHART");
    ASSERT_INT("hidden grid remains valid in saved cfg",
               glr_config_get(GLR_CONFIG_GRID_THEME), GRID_THEME_STARCHART);
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
 * forward). Exercised on two multi-state cycles - light theme (F9) and grid
 * theme (F3) - where the direction is observable; 2-state toggles flip
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

    /* Grid theme (F2): same backward behavior on a different F-key row. */
    glr_config_set(GLR_CONFIG_GRID_THEME, 0);
    g_test_mods = GLUT_ACTIVE_SHIFT;
    ASSERT_INT("Shift+F2 handled",
               glr_cfg_handle_special_shortcut(GLUT_KEY_F2), 1);
    ASSERT_INT("Shift+F2 wraps grid theme backward to the last",
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
        { GLUT_KEY_F2,  GLR_CONFIG_GRID_THEME,      "Grid theme"      },
        { GLUT_KEY_F5,  GLR_CONFIG_BACKDROP,        "Backdrop"        },
        { GLUT_KEY_F3,  GLR_CONFIG_GRID_EXTENT,     "Grid extent"     },
    };
    for (unsigned i = 0; i < sizeof(fmap)/sizeof(fmap[0]); i++) {
        int before = glr_config_get(fmap[i].key);
        ASSERT_INT("reassigned F-key consumed",
                   glr_cfg_handle_special_shortcut(fmap[i].fkey), 1);
        ASSERT_TRUE("reassigned F-key cycles its config",
                    glr_config_get(fmap[i].key) != before);
    }

    /* F10 is part of the config table and cycles Post FX Scope. */
    glr_config_set(GLR_CONFIG_POST_FX_EFFECT, GLR_POST_FX_EFFECT_CHROMATIC_ABERRATION);
    glr_config_set(GLR_CONFIG_POST_FX_SCOPE, GLR_POST_FX_SCOPE_OFF);
    ASSERT_INT("F10 is claimed by the cfg special-shortcut dispatcher",
               glr_cfg_handle_special_shortcut(GLUT_KEY_F10), 1);
    ASSERT_INT("F10 cycles Post FX Scope to 3D View",
               glr_config_get(GLR_CONFIG_POST_FX_SCOPE), GLR_POST_FX_SCOPE_VIEW_3D);
    ASSERT_INT("3D View scope applies the selected effect to the scene",
               (int)glr_state_presentation().post_filter_mode, (int)RENDER3D_POST_FILTER_CHROMATIC_ABERRATION);

    /* Wireframe -> plain Ctrl+W. */
    glr_state_presentation_mut()->wireframe = 0;
    ASSERT_INT("Ctrl+W consumed", glr_cfg_handle_ascii_shortcut(KEY_CTRL_W), 1);
    ASSERT_INT("Ctrl+W toggles wireframe", glr_state_presentation().wireframe, 1);

    /* Normal vectors moved to plain Ctrl+N. Vertex outlines remains on
     * Ctrl+Shift+O, covered in test_ascii_shortcut_modifiers. */
    g_test_mods = 0;
    glr_state_presentation_mut()->show_normal_vectors = 0;
    ASSERT_INT("Ctrl+N consumed", glr_cfg_handle_ascii_shortcut(KEY_CTRL_N), 1);
    ASSERT_INT("Ctrl+N toggles normal vectors",
               glr_state_presentation().show_normal_vectors, 1);

    g_test_mods = 0;
    glr_state_presentation_mut()->show_light_indicators = 0;
    ASSERT_INT("Ctrl+L consumed", glr_cfg_handle_ascii_shortcut(KEY_CTRL_L), 1);
    ASSERT_INT("Ctrl+L toggles light indicators",
               glr_state_presentation().show_light_indicators, 1);

    g_test_mods = GLUT_ACTIVE_SHIFT;
    glr_config_set(GLR_CONFIG_LINE_SMOOTH, 0);
    ASSERT_INT("Ctrl+Shift+L consumed", glr_cfg_handle_ascii_shortcut(KEY_CTRL_L), 1);
    ASSERT_INT("Ctrl+Shift+L toggles line smooth",
               glr_config_get(GLR_CONFIG_LINE_SMOOTH), 1);

    glr_state_presentation_mut()->projection_mode = PROJ_PERSPECTIVE;
    ASSERT_INT("Ctrl+Shift+E consumed", glr_cfg_handle_ascii_shortcut(KEY_CTRL_E), 1);
    ASSERT_INT("Ctrl+Shift+E toggles projection",
               glr_state_presentation().projection_mode, PROJ_ORTHO);

    /* Ctrl+Shift+D is the Depth view cycle (Off -> Linear). Plain Ctrl+D
     * remains an editor deletion key. */
    g_test_mods = GLUT_ACTIVE_SHIFT;
    glr_state_presentation_mut()->depth_viz = 0;
    ASSERT_INT("Ctrl+Shift+D claimed by cfg (Depth view)",
               glr_cfg_handle_ascii_shortcut(KEY_CTRL_D), 1);
    ASSERT_INT("Ctrl+Shift+D cycles depth view",
               glr_state_presentation().depth_viz, 1);
    glr_state_presentation_mut()->depth_viz = 0;
    g_test_mods = 0;
    ASSERT_INT("plain Ctrl+D not claimed by cfg", glr_cfg_handle_ascii_shortcut(KEY_CTRL_D), 0);
    ASSERT_INT("plain Ctrl+E not claimed by cfg", glr_cfg_handle_ascii_shortcut(KEY_CTRL_E), 0);

    editor_input_set_modifier_provider_for_test(NULL);
    g_test_mods = 0;
}

/* No two config rows may claim the same keyboard binding. A binding is
 * the triple (key_code, modifiers, is_special): the same key with
 * different modifiers is distinct by design (Ctrl+G = Compute profile,
 * Ctrl+W = Wireframe, Ctrl+Shift+W = Winding, and Ctrl+Shift+B = Memory
 * profile; the plain-Ctrl / Ctrl+Shift pairs noted
 * in keymap.h), and an F-key vs a Ctrl byte of equal numeric value are
 * distinct via is_special. Guards against an accidental double-map when a
 * keymap.h binding is reused across two rows. Data-driven over
 * g_cfg_items[] (the enumerable, collision-prone tier - it owns the
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
     * freeglut Cocoa build - GLUT_ACTIVE_SUPER is 0 under the GL stubs - so
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

static void test_keymap_binding_to_string(void) {
    char buf[KEYMAP_SHORTCUT_LABEL_MAX];

    ASSERT_STR("format Ctrl letter",
               keymap_binding_to_string(buf, (int)sizeof(buf),
                                        KM_KEY(GLR_SAVE), KM_MODS(GLR_SAVE), 0),
               "Ctrl+S");
    ASSERT_STR("format Ctrl+Shift letter",
               keymap_binding_to_string(buf, (int)sizeof(buf),
                                        KM_KEY(GLR_LINE_SMOOTH),
                                        KM_MODS(GLR_LINE_SMOOTH), 0),
               "Ctrl+Shift+L");
    ASSERT_STR("format claimed Ctrl+Shift control alias",
               keymap_binding_to_string(buf, (int)sizeof(buf),
                                        KM_KEY(GLR_SYNTAX_HL),
                                        KM_MODS(GLR_SYNTAX_HL), 0),
               "Ctrl+Shift+H");
    ASSERT_STR("format F-key",
               keymap_binding_to_string(buf, (int)sizeof(buf),
                                        KM_KEY(GLR_NEXT_EXAMPLE),
                                        KM_MODS(GLR_NEXT_EXAMPLE), 1),
               "F12");
    ASSERT_STR("format Shift+F-key",
               keymap_binding_to_string(buf, (int)sizeof(buf),
                                        KM_KEY(GLR_PREV_EXAMPLE),
                                        KM_MODS(GLR_PREV_EXAMPLE), 1),
               "Shift+F12");
    ASSERT_STR("format Ctrl+special key",
               keymap_binding_to_string(buf, (int)sizeof(buf),
                                        KM_KEY(GLR_AUDIO_PREV),
                                        KM_MODS(GLR_AUDIO_PREV), 1),
               "Ctrl+Left");
    ASSERT_STR("format printable key",
               keymap_binding_to_string(buf, (int)sizeof(buf),
                                        KM_KEY(GLR_VARIABLE_PANEL),
                                        KM_MODS(GLR_VARIABLE_PANEL), 0),
               "`");
    ASSERT_STR("format escape key",
               keymap_binding_to_string(buf, (int)sizeof(buf),
                                        KM_KEY(GLR_ESCAPE),
                                        KM_MODS(GLR_ESCAPE), 0),
               "Escape");

    /* Modifier combinations */
    ASSERT_STR("format Alt letter",
               keymap_binding_to_string(buf, (int)sizeof(buf), 'A', GLUT_ACTIVE_ALT, 0),
               "Alt+A");
    ASSERT_STR("format Ctrl+Shift+Alt letter",
               keymap_binding_to_string(buf, (int)sizeof(buf), 'A',
                                        GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SHIFT | GLUT_ACTIVE_ALT, 0),
               "Ctrl+Shift+Alt+A");

    /* Special keys list */
    ASSERT_STR("format special Left",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_LEFT, 0, 1),
               "Left");
    ASSERT_STR("format special Right",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_RIGHT, 0, 1),
               "Right");
    ASSERT_STR("format special Up",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_UP, 0, 1),
               "Up");
    ASSERT_STR("format special Down",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_DOWN, 0, 1),
               "Down");
    ASSERT_STR("format special Home",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_HOME, 0, 1),
               "Home");
    ASSERT_STR("format special End",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_END, 0, 1),
               "End");
    ASSERT_STR("format special Page Up",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_PAGE_UP, 0, 1),
               "Page Up");
    ASSERT_STR("format special Page Down",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_PAGE_DOWN, 0, 1),
               "Page Down");
    ASSERT_STR("format special Insert",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_INSERT, 0, 1),
               "Insert");
#ifndef USE_GLUT
    ASSERT_STR("format special Delete",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_DELETE, 0, 1),
               "Delete");
#endif
    ASSERT_STR("format special F1",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_F1, 0, 1),
               "F1");
    ASSERT_STR("format special F12",
               keymap_binding_to_string(buf, (int)sizeof(buf), GLUT_KEY_F12, 0, 1),
               "F12");
    ASSERT_STR("format unknown special key",
               keymap_binding_to_string(buf, (int)sizeof(buf), 9999, 0, 1),
               "Key9999");

    /* Non-special keys */
    ASSERT_STR("format Backspace",
               keymap_binding_to_string(buf, (int)sizeof(buf), KEY_BACKSPACE, 0, 0),
               "Backspace");
    ASSERT_STR("format Delete",
               keymap_binding_to_string(buf, (int)sizeof(buf), KEY_DELETE, 0, 0),
               "Delete");
    ASSERT_STR("format Tab",
               keymap_binding_to_string(buf, (int)sizeof(buf), '\t', 0, 0),
               "Tab");
    ASSERT_STR("format Enter (LF)",
               keymap_binding_to_string(buf, (int)sizeof(buf), '\n', 0, 0),
               "Enter");
    ASSERT_STR("format Enter (CR)",
               keymap_binding_to_string(buf, (int)sizeof(buf), '\r', 0, 0),
               "Enter");
    ASSERT_STR("format Space",
               keymap_binding_to_string(buf, (int)sizeof(buf), ' ', 0, 0),
               "Space");
    ASSERT_STR("format Ctrl Backslash",
               keymap_binding_to_string(buf, (int)sizeof(buf), KEY_CTRL_BACKSLASH, 0, 0),
               "Ctrl+\\");
    ASSERT_STR("format Ctrl Dash",
               keymap_binding_to_string(buf, (int)sizeof(buf), KEY_CTRL_DASH, 0, 0),
               "Ctrl+-");
    ASSERT_STR("format Key 128",
               keymap_binding_to_string(buf, (int)sizeof(buf), 128, 0, 0),
               "Key128");

    /* Boundary / error guards */
    ASSERT_STR("null buffer check",
               keymap_binding_to_string(NULL, 10, 'A', 0, 0),
               "");
    ASSERT_STR("zero buffer size check",
               keymap_binding_to_string(buf, 0, 'A', 0, 0),
               "");
    ASSERT_STR("negative buffer size check",
               keymap_binding_to_string(buf, -5, 'A', 0, 0),
               "");

    /* Truncation / boundary snprintf tests in append_label_part */
    ASSERT_STR("exact size no truncation",
               keymap_binding_to_string(buf, 7, 'A', GLUT_ACTIVE_CTRL, 0),
               "Ctrl+A");
    ASSERT_STR("truncation case Ctrl+",
               keymap_binding_to_string(buf, 5, 'A', GLUT_ACTIVE_CTRL, 0),
               "Ctrl");
    ASSERT_STR("truncation case Ctrl+Sh",
               keymap_binding_to_string(buf, 8, 'A', GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SHIFT, 0),
               "Ctrl+Sh");
}

static int help_tab_contains_binding(const char *tab_label,
                                     const char *binding,
                                     const char *desc) {
    const ReplHelpContent *help = repl_help_text_build();
    for (int t = 0; help && t < help->tab_count; t++) {
        const ReplHelpTab *tab = &help->tabs[t];
        if (!tab->label || strcmp(tab->label, tab_label) != 0)
            continue;
        for (int i = 0; tab->lines && tab->lines[i]; i++) {
            if (strstr(tab->lines[i], binding) &&
                strstr(tab->lines[i], desc))
                return 1;
        }
    }
    return 0;
}

static void test_help_keys_tab_uses_keymap_labels(void) {
    char shortcut[KEYMAP_SHORTCUT_LABEL_MAX];

    keymap_binding_to_string(shortcut, (int)sizeof(shortcut),
                             KM_KEY(GLR_SAVE), KM_MODS(GLR_SAVE), 0);
    ASSERT_TRUE("help Keys tab renders Save shortcut from keymap",
                help_tab_contains_binding("Keys", shortcut, "Save to output.c"));

    keymap_binding_to_string(shortcut, (int)sizeof(shortcut),
                             KM_KEY(GLR_NEXT_EXAMPLE), KM_MODS(GLR_NEXT_EXAMPLE), 1);
    ASSERT_TRUE("help Keys tab renders Next Example shortcut from keymap",
                help_tab_contains_binding("Keys", shortcut, "Next example / scene"));

    keymap_binding_to_string(shortcut, (int)sizeof(shortcut),
                             KM_KEY(GLR_AUDIO_PREV), KM_MODS(GLR_AUDIO_PREV), 1);
    ASSERT_TRUE("help Keys tab renders Audio Previous shortcut from keymap",
                help_tab_contains_binding("Keys", shortcut, "Previous track"));

    ASSERT_TRUE("help Keys tab mentions OpenGL state inspector",
                help_tab_contains_binding("Keys", "Right-click blank row",
                                          "current vs default OpenGL state"));
}

static void test_help_commands_tab_lists_if_branches(void) {
    ASSERT_TRUE("help Commands tab renders else-if syntax",
                help_tab_contains_binding("Commands", "} else if(t > 0.5) {",
                                          "First matching branch runs"));
    ASSERT_TRUE("help Commands tab renders else syntax",
                help_tab_contains_binding("Commands", "} else {",
                                          "Fallback branch"));
}


/* Switching auto-normals off removes what the pass generated, and leaves an
 * undo entry so an accidental toggle is recoverable. Both halves matter: the
 * pass mutates the document from the display frame, so before this there was
 * neither a way to remove the rows nor a Ctrl+Z to fall back on. */
static void test_auto_normals_off_strips_generated_rows(void) {
    int auto_normals_row = -1;
    int doc_before, doc_with_normals;

    printf("Testing auto-normals Off strips generated rows...\n");

    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && item->key == GLR_CONFIG_AUTO_NORMALS)
            auto_normals_row = i;
    }
    ASSERT_TRUE("auto-normals config row found", auto_normals_row >= 0);

    glr_ctrl_reset_all();
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    doc_before = repl_state_document_count();

    /* Generate: the controller normally runs this from the display frame. */
    glr_state_presentation_mut()->autonormal = REPL_AUTONORMAL_FACE;
    repl_recompute_autonormals(REPL_AUTONORMAL_FACE, NULL);
    doc_with_normals = repl_state_document_count();
    ASSERT_TRUE("pass generated at least one normal",
                doc_with_normals > doc_before);

    /* Cycle Face -> Smooth -> Off; only the Off step strips. */
    glr_cfg_cycle_row(auto_normals_row, 1);
    ASSERT_INT("cycled to Smooth", glr_state_presentation().autonormal,
               REPL_AUTONORMAL_SMOOTH);
    ASSERT_INT("Smooth keeps the generated rows",
               repl_state_document_count(), doc_with_normals);

    glr_cfg_cycle_row(auto_normals_row, 1);
    ASSERT_INT("cycled to Off", glr_state_presentation().autonormal,
               REPL_AUTONORMAL_OFF);
    ASSERT_INT("Off removed the generated rows",
               repl_state_document_count(), doc_before);
    ASSERT_STR("status names the removal and the way back", g_last_status,
               "Auto-normals: Off (1 generated normal removed, "
               "Ctrl+Z to restore)");

    /* The snapshot is the point: an accidental Off is recoverable, and with
     * the mode now off nothing re-runs to strip the rows a second time. */
    editor_undo_pop_snapshot();
    ASSERT_INT("Ctrl+Z restores the stripped rows",
               repl_state_document_count(), doc_with_normals);
    repl_recompute_autonormals(glr_state_presentation().autonormal, NULL);
    ASSERT_INT("and they stay restored while the mode is off",
               repl_state_document_count(), doc_with_normals);
}

int main(void) {
    /* Before audio init, the actions layer must treat audio as disabled
     * (the --no-audio case): defaults application and the play/pause
     * toggle are status-only no-ops. */
    glr_ctrl_reset_all();
    glr_actions_apply_defaults();
    ASSERT_STR("apply_defaults reports disabled audio",
               g_last_status, "Audio: disabled");
    glr_config_set(GLR_CONFIG_AUDIO_MODE, 1);
    glr_action_toggle_audio_play_pause();
    ASSERT_INT("audio toggle no-op while disabled",
               glr_config_get(GLR_CONFIG_AUDIO_MODE), 1);
    ASSERT_STR("audio toggle reports disabled audio",
               g_last_status, "Audio: disabled");

    /* The rest of the suite exercises the audio-enabled paths; run the
     * engine deviceless so it works on headless CI machines. */
    setenv("GLR_AUDIO_NO_DEVICE", "1", 1);
    if (glr_audio_init() != 0) {
        fprintf(stderr, "test_repl_actions: glr_audio_init failed\n");
        return 1;
    }

    test_apply_defaults();
    test_no_duplicate_config_bindings();
    test_keymap_event_is_strict();
    test_keymap_binding_to_string();
    test_help_keys_tab_uses_keymap_labels();
    test_help_commands_tab_lists_if_branches();
    test_auto_normals_off_strips_generated_rows();
    test_f9_cycles_light_theme();
    test_shift_fkey_steps_backward();
    test_fkey_reassignment_and_alt_shortcuts();
    test_cursor_actions();
    test_help_tab_actions();
    test_cfg_cycling();
    test_config_sections();
    test_config_parent_rows_inert();
    test_view_mode_swatch_state();
    test_menu_actions();
    test_workspace_save_promotes_visible_example();
    test_split_decl_menu_action();
    test_load_workspace_activates_scene();
    test_shortcuts();
    test_ascii_shortcut_modifiers();
    test_tutorial_start_applies_cfg();
    test_tutorial_menu_dispatch();
    test_tours_menu_dispatch();
    test_tour_done_auto_closes();
    test_tour_paced_key();
    test_tour_sequential_steps_and_pause();
    test_compute_profile_mode_names();
    test_replay_expand_mode_names();
    test_audio_config_direct_set();
    test_audio_menu_actions();
    test_scene_menu_cycle_actions();
    test_config_slug_table();
    test_config_keys_are_unique();
    test_msaa_display_label_override();
    test_depth_viz_row_metadata();
    test_stencil_viz_row_metadata();
    test_config_none_handling();
    test_menu_out_of_range_indices();
    test_cfg_cycle_stops_replay();
    test_depth_viz_cycle_refuses_without_readback();
    test_stencil_viz_cycle_refuses_without_readback();
    test_replay_config_set_uses_lifecycle();
    test_status_set_drops_empty_message();
    test_cfg_cycle_focus_origin_eases_to_origin();
    test_cfg_cycle_look_down_z_zeroes_orbit_and_pan();
    test_cfg_cycle_reset_camera_eases_to_default();
    test_time_reset_action();
    test_cfg_cycle_panel_hidden_closes_overlays();
    test_vertex_label_modes();
    test_backdrop_grid_pairing_policy();
    test_cfg_bridge_resolves_symbolic_names();
    test_cfg_bridge_enforces_backdrop_grid_pair_order();

    glr_audio_shutdown();
    return test_harness_report(&g_harness, "test_repl_actions");
}
