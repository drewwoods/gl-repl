#define _DEFAULT_SOURCE
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "ui/app/menu_bar.h"
#include "ui/app/view_mode_swatch.h"
#include "app/glr_audio.h"
#include "app/glr_actions.h"
#include "app/glr_config.h"
#include "app/glr_pointer_script.h"
#include "app/glr_workspaces.h"
#include "repl/scenes.h"
#include "repl/workspace_io.h"
#include "repl/examples.h"
#include "repl/example_loader.h"
#include "repl/tutorials.h"
#include "repl/state_owners.h"
#include "subsystems/replay/replay.h"
#include "ui/app/state.h"
#include "ui/app/layout.h"
#include "ui/core/metrics.h"
#include "ui/core/tabbed_overlay.h"
#include "ui/app/repl_code_panel.h"
#include "editor/help_session.h"
#include "editor/input.h"
#include "editor/state.h"
#include "support/test_harness.h"
#include <ui/core/gl_2d.h>
#ifdef GL_STUBS
#include <GL/gl_stub_counts.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT_EQ(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_STR_EQ(label, got, exp) \
    TEST_ASSERT_STR(&g_harness, label, got, exp)

static void reset_menu_bar_fixture(int window_w, int window_h) {
    glr_ctrl_reset_all();
    ui_state_reset();
    ui_menu_bar_close();
    ui_state_viewport_set_size(window_w, window_h);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 1.0f;
}

static void test_menu_bar_rect_helper(void) {
    int cp_x, cp_y, cp_w, cp_h;
    int bar_x, bar_y, bar_w, bar_h;

    reset_menu_bar_fixture(1000, 600);
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    ui_layout_menu_bar_rect(&bar_x, &bar_y, &bar_w, &bar_h);

    ASSERT_INT_EQ("menu bar x matches code panel", bar_x, cp_x);
    ASSERT_INT_EQ("menu bar y anchors to code panel top chrome", bar_y,
                  cp_y + cp_h - CODE_MARGIN_Y - LINE_H);
    ASSERT_INT_EQ("menu bar w matches code panel", bar_w, cp_w);
    ASSERT_INT_EQ("menu bar h is one line", bar_h, LINE_H);
}

static void test_reveal_workspace_enabled_state(void) {
    char temp_dir[] = "/tmp/test_reveal_workspace.XXXXXX";
    char *dir;
    WorkspaceManifest manifest;

    reset_menu_bar_fixture(1000, 600);
    repl_set_workspace_dir(NULL);
    ASSERT_INT_EQ("Reveal Workspace disabled while unbound",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_REVEAL_WORKSPACE), 0);

    dir = mkdtemp(temp_dir);
    ASSERT_TRUE("Reveal Workspace test directory created", dir != NULL);
    if (!dir) return;
    memset(&manifest, 0, sizeof(manifest));
    manifest.version = 1;
    snprintf(manifest.name, sizeof(manifest.name), "Reveal Test");
    ASSERT_TRUE("Reveal Workspace managed manifest written",
                workspace_io_manifest_write(dir, &manifest, NULL, 0));
    repl_set_workspace_dir(dir);
    ASSERT_INT_EQ("Reveal Workspace enabled for managed binding",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_REVEAL_WORKSPACE), 1);

    repl_set_workspace_dir(NULL);
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir,
                 WORKSPACE_IO_MANIFEST_FILE);
        unlink(path);
    }
    rmdir(dir);
}

static void test_scene_file_action_enabled_state(void) {
    char temp_dir[] = "/tmp/test_scene_file_actions.XXXXXX";
    char *dir;
    WorkspaceManifest manifest;

    reset_menu_bar_fixture(1000, 600);
    repl_set_workspace_dir(NULL);
    ASSERT_INT_EQ("Rename Scene disabled without a user scene",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_RENAME_SCENE), 0);
    ASSERT_INT_EQ("Delete Workspace Scene disabled while unbound",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_DELETE_SCENE), 0);

    repl_load_example(0);
    ASSERT_INT_EQ("Rename Scene disabled for an example",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_RENAME_SCENE), 0);
    ASSERT_INT_EQ("Delete Workspace Scene disabled for an example",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_DELETE_SCENE), 0);

    ASSERT_TRUE("user scene created for File action state test",
                repl_scenes_create_empty_user_scene() >= 0);
    ASSERT_INT_EQ("Rename Scene enabled for an unbound user scene",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_RENAME_SCENE), 1);
    ASSERT_INT_EQ("Delete Workspace Scene stays disabled while unbound",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_DELETE_SCENE), 0);

    dir = mkdtemp(temp_dir);
    ASSERT_TRUE("scene action test directory created", dir != NULL);
    if (!dir) return;
    memset(&manifest, 0, sizeof(manifest));
    manifest.version = 1;
    snprintf(manifest.name, sizeof(manifest.name), "Scene Actions");
    ASSERT_TRUE("scene action managed manifest written",
                workspace_io_manifest_write(dir, &manifest, NULL, 0));
    repl_set_workspace_dir(dir);
    ASSERT_INT_EQ("Rename Scene enabled for a workspace scene",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_RENAME_SCENE), 1);
    ASSERT_INT_EQ("Delete Workspace Scene enabled for a workspace scene",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_DELETE_SCENE), 1);

    tutorial_state_mut()->active = 1;
    ASSERT_INT_EQ("Rename Scene disabled during a tutorial",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_RENAME_SCENE), 0);
    ASSERT_INT_EQ("Delete Workspace Scene disabled during a tutorial",
        ui_menu_bar_menu_item_enabled_for_test(
            GLR_MENU_FILE, GLR_FILE_ITEM_DELETE_SCENE), 0);
    tutorial_state_mut()->active = 0;

    repl_set_workspace_dir(NULL);
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir,
                 WORKSPACE_IO_MANIFEST_FILE);
        unlink(path);
    }
    rmdir(dir);
}

static int menu_bar_center_my(void) {
    int bar_y, bar_h;
    int row_mid_y;

    ui_layout_menu_bar_rect(NULL, &bar_y, NULL, &bar_h);
    row_mid_y = bar_y + bar_h / 2;
    return ui_state_viewport().window_h - row_mid_y;
}

static int file_menu_mx(void) {
    int cp_x, cp_y, cp_w, cp_h;
    (void)cp_y;
    (void)cp_w;
    (void)cp_h;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    return cp_x + CODE_MARGIN_X + FONT_SMALL_W;
}

static int replay_pin_mx(void) {
    int cp_x, cp_y, cp_w, cp_h;
    int right_edge;

    (void)cp_y;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    right_edge = cp_x + cp_w - CODE_MARGIN_X;
    return right_edge - 10;
}

static int find_dropdown_item_point(int menu_id, int target_item,
                                    int *out_mx, int *out_my) {
    int win_w = ui_state_viewport().window_w;
    int win_h = ui_state_viewport().window_h;

    ui_menu_bar_set_open_menu(menu_id, 0.0f);
    for (int my = 0; my < win_h; my++) {
        for (int mx = 0; mx < win_w; mx++) {
            UiHit hit = ui_menu_bar_hit_test(mx, my);
            if (hit.kind == UI_HIT_MENU_ITEM && hit.item_idx == target_item) {
                if (out_mx)
                    *out_mx = mx;
                if (out_my)
                    *out_my = my;
                return 1;
            }
        }
    }

    return 0;
}

/* The File menu's workspace header names the binding the workspace rows below
 * it act on. Three things are pinned: the unbound wording (the state that is
 * otherwise invisible), that a bound manifest's name reaches the label, and
 * that the row stays inert - it is a header, not a command. */
static void test_workspace_header_row(void) {
    char temp_dir[] = "/tmp/test_workspace_header.XXXXXX";
    char *dir;
    WorkspaceManifest manifest;

    reset_menu_bar_fixture(1000, 600);
    repl_set_workspace_dir(NULL);
    glr_workspaces_refresh();
    ASSERT_STR_EQ("workspace header names the unbound state",
                  ui_menu_bar_menu_item_label_for_test(
                      GLR_MENU_FILE, GLR_FILE_ITEM_WORKSPACE_HDR),
                  "### WORKSPACE: (none)");

    dir = mkdtemp(temp_dir);
    ASSERT_TRUE("workspace header test directory created", dir != NULL);
    if (!dir) return;
    memset(&manifest, 0, sizeof(manifest));
    manifest.version = 1;
    snprintf(manifest.name, sizeof(manifest.name), "Header Test");
    ASSERT_TRUE("workspace header manifest written",
                workspace_io_manifest_write(dir, &manifest, NULL, 0));
    repl_set_workspace_dir(dir);
    ASSERT_STR_EQ("workspace header names the bound workspace",
                  ui_menu_bar_menu_item_label_for_test(
                      GLR_MENU_FILE, GLR_FILE_ITEM_WORKSPACE_HDR),
                  "### WORKSPACE: Header Test");

    /* Overlong names truncate instead of widening the whole File dropdown.
     * Rewriting the manifest under an unchanged binding also exercises the
     * memo's documented invalidation point. */
    snprintf(manifest.name, sizeof(manifest.name),
             "AbcdefghijklmnopqrstuvwxyzABCD");
    ASSERT_TRUE("workspace header long-name manifest written",
                workspace_io_manifest_write(dir, &manifest, NULL, 0));
    glr_workspaces_refresh();
    ASSERT_STR_EQ("workspace header truncates a long name",
                  ui_menu_bar_menu_item_label_for_test(
                      GLR_MENU_FILE, GLR_FILE_ITEM_WORKSPACE_HDR),
                  "### WORKSPACE: Abcdefghijklmnopqrstuvwx");

    ASSERT_INT_EQ("workspace header row is not clickable",
                  find_dropdown_item_point(GLR_MENU_FILE,
                                           GLR_FILE_ITEM_WORKSPACE_HDR,
                                           NULL, NULL), 0);
    ui_menu_bar_close();

    repl_set_workspace_dir(NULL);
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir,
                 WORKSPACE_IO_MANIFEST_FILE);
        unlink(path);
    }
    rmdir(dir);
    glr_workspaces_refresh();
}

static void test_open_close_state(void) {
    reset_menu_bar_fixture(1000, 600);

    ASSERT_INT_EQ("init: no menu open", ui_menu_bar_open_menu_id(), -1);
    ASSERT_TRUE("init: dropdown not open", !ui_menu_bar_menu_dropdown_is_open());

    ui_menu_bar_set_open_menu(GLR_MENU_FILE, 0.0f);
    ASSERT_INT_EQ("open File menu", ui_menu_bar_open_menu_id(), GLR_MENU_FILE);
    ASSERT_TRUE("File dropdown open", ui_menu_bar_menu_dropdown_is_open());
    ASSERT_TRUE("example dropdown mirrors open menu", ui_menu_bar_menu_dropdown_is_open());

    ui_menu_bar_set_open_menu(GLR_MENU_SCENE, 0.0f);
    ASSERT_INT_EQ("switch to Scene menu", ui_menu_bar_open_menu_id(), GLR_MENU_SCENE);

    ui_menu_bar_open_config(0.0f);
    ASSERT_INT_EQ("open Config menu via helper", ui_menu_bar_open_menu_id(), GLR_MENU_CONFIG);

    ui_menu_bar_open_config(0.0f);
    ASSERT_INT_EQ("open Config again closes it", ui_menu_bar_open_menu_id(), -1);

    ui_menu_bar_set_open_menu(999, 0.0f);
    ASSERT_INT_EQ("invalid menu ID closes", ui_menu_bar_open_menu_id(), -1);

    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN; glr_ctrl_sync_ui_chrome();
    ui_menu_bar_set_open_menu(GLR_MENU_FILE, 0.0f);
    ASSERT_TRUE("hidden code panel suppresses dropdown", !ui_menu_bar_menu_dropdown_is_open());
    ASSERT_TRUE("hidden code panel suppresses example dropdown", !ui_menu_bar_menu_dropdown_is_open());
}

static void test_top_level_hits(void) {
    int menu_mx;
    int pin_mx;
    int my;
    int cp_x, cp_y, cp_w, cp_h;
    int right_edge, replay_w, view_mode_w, swatch_x, swatch_mx;

    reset_menu_bar_fixture(1000, 600);
    menu_mx = file_menu_mx();
    pin_mx = replay_pin_mx();
    my = menu_bar_center_my();

    UiHit h_menu = ui_menu_bar_hit_test(menu_mx, my);
    ASSERT_INT_EQ("hit File menu button", h_menu.kind == UI_HIT_MENU_BUTTON ? h_menu.cmd_idx : -1, GLR_MENU_FILE);

    UiHit h_pin = ui_menu_bar_hit_test(pin_mx, my);
    ASSERT_INT_EQ("hit Replay pin", h_pin.kind == UI_HIT_PIN_BUTTON ? h_pin.item_idx : -1, UI_MENU_BAR_PIN_REPLAY);

    /* Test hit-testing the new View Mode pin (PIN_VIEW_MODE) */
    view_mode_w = ui_view_mode_swatch_label_width();
    replay_w = (int)strlen("Replaying") * FONT_SMALL_W + 12 + 22;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    right_edge = cp_x + cp_w - CODE_MARGIN_X;
    swatch_x = right_edge - replay_w - view_mode_w;
    swatch_mx = swatch_x + view_mode_w / 2;

    UiHit h_swatch = ui_menu_bar_hit_test(swatch_mx, my);
    ASSERT_INT_EQ("hit View Mode pin kind", h_swatch.kind, UI_HIT_PIN_BUTTON);
    ASSERT_INT_EQ("hit View Mode pin item_idx", h_swatch.item_idx, UI_MENU_BAR_PIN_VIEW_MODE);

    UiHit h_menu_miss = ui_menu_bar_hit_test(menu_mx, my + 120);
    ASSERT_INT_EQ("menu miss below bar", h_menu_miss.kind == UI_HIT_MENU_BUTTON ? h_menu_miss.cmd_idx : -1, -1);

    UiHit h_pin_miss = ui_menu_bar_hit_test(menu_mx, my);
    ASSERT_INT_EQ("pin miss in menu region", h_pin_miss.kind == UI_HIT_PIN_BUTTON ? h_pin_miss.item_idx : -1, -1);
}

static void test_dropdown_and_config_press(void) {
    int item_mx = -1;
    int item_my = -1;
    int tutorial_mx = -1;
    int tutorial_my = -1;
    int scene_tag_mx = -1;
    int scene_tag_my = -1;

    reset_menu_bar_fixture(1000, 600);

    ASSERT_TRUE("found New Scene item point",
                find_dropdown_item_point(GLR_MENU_FILE, GLR_FILE_ITEM_NEW_SCENE,
                                         &item_mx, &item_my));
    UiHit h_new_scene = ui_menu_bar_hit_test(item_mx, item_my);
    ASSERT_INT_EQ("hit New Scene item",
                  h_new_scene.kind == UI_HIT_MENU_ITEM ? h_new_scene.item_idx : -1,
                  GLR_FILE_ITEM_NEW_SCENE);

    ASSERT_TRUE("found Quit item point",
                find_dropdown_item_point(GLR_MENU_FILE, GLR_FILE_ITEM_QUIT,
                                         &item_mx, &item_my));
    UiHit h_quit = ui_menu_bar_hit_test(item_mx, item_my);
    ASSERT_INT_EQ("hit Quit item",
                  h_quit.kind == UI_HIT_MENU_ITEM ? h_quit.item_idx : -1,
                  GLR_FILE_ITEM_QUIT);

    ui_menu_bar_close();
    UiHit h_closed = ui_menu_bar_hit_test(item_mx, item_my);
    ASSERT_INT_EQ("hit when closed", h_closed.kind == UI_HIT_MENU_ITEM ? h_closed.item_idx : -1, -1);

    /* MENU_TUTORIALS top-level row 0 is the first tag row (tutorial
     * activation moved to per-tag flyouts via route_submenu_item_hit).
     * Hit-test still returns 0 either way - row index is row index - but
     * activation is now inert (mirrors the Scene tag-row guard below). */
    ASSERT_TRUE("found first tutorial tag row point",
                find_dropdown_item_point(GLR_MENU_TUTORIALS, 0,
                                         &tutorial_mx, &tutorial_my));
    UiHit h_tut = ui_menu_bar_hit_test(tutorial_mx, tutorial_my);
    ASSERT_INT_EQ("hit first tutorial tag row",
                  h_tut.kind == UI_HIT_MENU_ITEM ? h_tut.item_idx : -1,
                  0);
    ASSERT_INT_EQ("Tutorials tag row activation keeps menu open",
                  glr_action_menu_item_activate(GLR_MENU_TUTORIALS, 0), 0);

    ASSERT_TRUE("found first Scene tag row point",
                find_dropdown_item_point(GLR_MENU_SCENE, 1,
                                         &scene_tag_mx, &scene_tag_my));
    UiHit h_scene = ui_menu_bar_hit_test(scene_tag_mx, scene_tag_my);
    ASSERT_INT_EQ("hit first Scene tag row",
                  h_scene.kind == UI_HIT_MENU_ITEM ? h_scene.item_idx : -1,
                  1);
    ASSERT_INT_EQ("Scene tag row activation keeps menu open",
                  glr_action_menu_item_activate(GLR_MENU_SCENE, 1), 0);
}

static void test_unified_hit_test(void) {
    int menu_mx;
    int pin_mx;
    int my;
    int item_mx = -1;
    int item_my = -1;
    UiHit h;

    reset_menu_bar_fixture(1000, 600);
    menu_mx = file_menu_mx();
    pin_mx = replay_pin_mx();
    my = menu_bar_center_my();

    h = ui_menu_bar_hit_test(menu_mx, my + 120);
    ASSERT_INT_EQ("hit_test: miss kind", h.kind, UI_HIT_NONE);

    h = ui_menu_bar_hit_test(menu_mx, my);
    ASSERT_INT_EQ("hit_test: menu button kind", h.kind, UI_HIT_MENU_BUTTON);
    ASSERT_INT_EQ("hit_test: menu button id", h.cmd_idx, GLR_MENU_FILE);

    ASSERT_TRUE("found dropdown point for hit_test",
                find_dropdown_item_point(GLR_MENU_FILE, GLR_FILE_ITEM_NEW_SCENE,
                                         &item_mx, &item_my));
    h = ui_menu_bar_hit_test(item_mx, item_my);
    ASSERT_INT_EQ("hit_test: dropdown item kind", h.kind, UI_HIT_MENU_ITEM);
    ASSERT_INT_EQ("hit_test: dropdown item menu_id", h.cmd_idx, GLR_MENU_FILE);
    ASSERT_INT_EQ("hit_test: dropdown item idx", h.item_idx, GLR_FILE_ITEM_NEW_SCENE);

    ASSERT_TRUE("found Scene tag point for hit_test",
                find_dropdown_item_point(GLR_MENU_SCENE, 1,
                                         &item_mx, &item_my));
    h = ui_menu_bar_hit_test(item_mx, item_my);
    ASSERT_INT_EQ("hit_test: Scene tag kind", h.kind, UI_HIT_MENU_ITEM);
    ASSERT_INT_EQ("hit_test: Scene tag menu_id", h.cmd_idx, GLR_MENU_SCENE);
    ASSERT_INT_EQ("hit_test: Scene tag row", h.item_idx, 1);

    ui_menu_bar_close();
    h = ui_menu_bar_hit_test(pin_mx, my);
    ASSERT_INT_EQ("hit_test: pin kind", h.kind, UI_HIT_PIN_BUTTON);
    ASSERT_INT_EQ("hit_test: pin id", h.item_idx, UI_MENU_BAR_PIN_REPLAY);
}

#ifdef GL_STUBS
static void test_msaa_label_dynamic(void) {
    extern int g_gl_stub_samples;

    /* Test case 1: samples = 4 -> "MSAAx4" */
    g_gl_stub_samples = 4;
    glr_ctrl_init_gl();

    int row = -1;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && item->key == GLR_CONFIG_MSAA) {
            row = i;
            break;
        }
    }
    ASSERT_TRUE("found MSAA config item", row >= 0);
    const char *lbl = glr_config_item_display_label(glr_config_item_at(row));
    ASSERT_TRUE("label updated to MSAAx4", strcmp(lbl, "MSAAx4") == 0);

    /* Test case 2: samples = 8 -> "MSAAx8" */
    g_gl_stub_samples = 8;
    glr_ctrl_init_gl();
    lbl = glr_config_item_display_label(glr_config_item_at(row));
    ASSERT_TRUE("label updated to MSAAx8", strcmp(lbl, "MSAAx8") == 0);

    /* Test case 3: samples = 0 -> "MSAA" (fallback) */
    g_gl_stub_samples = 0;
    glr_ctrl_init_gl();
    lbl = glr_config_item_display_label(glr_config_item_at(row));
    ASSERT_TRUE("label fallback to MSAA", strcmp(lbl, "MSAA") == 0);
}
#endif

/* Pure geometry (no GL): a click point centered on submenu row
 * `ordinal`. Used by both stub-gated render tests and the non-stub
 * Config right-press test, so it lives outside the GL_STUBS guard. */
static int submenu_row_point(int sx, int sy, int sw, int sh, int ordinal,
                             int *out_mx, int *out_my) {
    int ry;
    if (ordinal < 0)
        return 0;
    ry = sy + sh - 4 - ordinal * LINE_H - LINE_H / 2;
    if (out_mx)
        *out_mx = sx + sw / 2;
    if (out_my)
        *out_my = ui_state_viewport().window_h - ry;
    return 1;
}

static void install_audio_menu_playlist(void) {
    GlrAudioTrackSpec tracks[] = {
        { "assets/alpha.mp3", "Assets", NULL },
        { "assets/beta.mp3", "Assets", "Beta Song" },
        { "bundle/gamma.mp3", "Bundled", NULL },
    };
    ASSERT_INT_EQ("audio menu fixture playlist",
                  glr_audio_set_playlist_specs(tracks, 3), 3);
}

static void test_audio_menu_flyout_hits(void) {
    int group_mx = -1, group_my = -1;
    int play_mx = -1, play_my = -1;
    int loop_mx = -1, loop_my = -1;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    int row_mx = -1, row_my = -1;
    const char *shortcut;
    UiHit hit;

    reset_menu_bar_fixture(1000, 600);
    install_audio_menu_playlist();

    ASSERT_TRUE("found first Audio group row",
                find_dropdown_item_point(GLR_MENU_AUDIO, 0,
                                         &group_mx, &group_my));
    hit = ui_menu_bar_hit_test(group_mx, group_my);
    ASSERT_INT_EQ("audio group row hit kind", hit.kind, UI_HIT_MENU_ITEM);
    ASSERT_INT_EQ("audio group row hit menu", hit.cmd_idx, GLR_MENU_AUDIO);
    ASSERT_INT_EQ("audio group row hit idx", hit.item_idx, 0);
    ASSERT_INT_EQ("Audio group row activation keeps menu open",
                  glr_action_menu_item_activate(GLR_MENU_AUDIO, 0), 0);

    ASSERT_TRUE("audio hover opens group flyout",
                ui_menu_bar_update_pointer_hover(group_mx, group_my, 0.0f));
    ASSERT_TRUE("audio flyout rect",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_AUDIO, 0,
                                                  &sx, &sy, &sw, &sh));
    ASSERT_TRUE("audio submenu row point",
                submenu_row_point(sx, sy, sw, sh, 1, &row_mx, &row_my));
    hit = ui_menu_bar_hit_test(row_mx, row_my);
    ASSERT_INT_EQ("audio submenu row kind", hit.kind, UI_HIT_SUBMENU_ITEM);
    ASSERT_INT_EQ("audio submenu carries menu_id", hit.cmd_idx, GLR_MENU_AUDIO);
    ASSERT_INT_EQ("audio submenu carries track index", hit.item_idx, 1);
    ASSERT_INT_EQ("audio submenu carries ordinal", hit.line_idx, 1);

    ASSERT_TRUE("found Audio Play row",
                find_dropdown_item_point(GLR_MENU_AUDIO,
                                         2 + GLR_AUDIO_OFF_PLAY,
                                         &play_mx, &play_my));
    hit = ui_menu_bar_hit_test(play_mx, play_my);
    ASSERT_INT_EQ("audio Play row hit", hit.item_idx,
                  2 + GLR_AUDIO_OFF_PLAY);
    shortcut = ui_menu_bar_menu_item_shortcut_for_test(GLR_MENU_AUDIO,
                                                       2 + GLR_AUDIO_OFF_PLAY);
    ASSERT_STR_EQ("audio Play shortcut", shortcut ? shortcut : "(null)",
                  "Ctrl+Shift+A");

    int back_mx = -1;
    int back_my = -1;
    ASSERT_TRUE("found Audio Back row",
                find_dropdown_item_point(GLR_MENU_AUDIO,
                                         2 + GLR_AUDIO_OFF_BACK10,
                                         &back_mx, &back_my));
    hit = ui_menu_bar_hit_test(back_mx, back_my);
    ASSERT_INT_EQ("audio Back row hit", hit.item_idx,
                  2 + GLR_AUDIO_OFF_BACK10);

    int fwd_mx = -1;
    int fwd_my = -1;
    ASSERT_TRUE("found Audio Forward row",
                find_dropdown_item_point(GLR_MENU_AUDIO,
                                         2 + GLR_AUDIO_OFF_FWD10,
                                         &fwd_mx, &fwd_my));
    hit = ui_menu_bar_hit_test(fwd_mx, fwd_my);
    ASSERT_INT_EQ("audio Forward row hit", hit.item_idx,
                  2 + GLR_AUDIO_OFF_FWD10);

    shortcut = ui_menu_bar_menu_item_shortcut_for_test(GLR_MENU_AUDIO,
                                                       2 + GLR_AUDIO_OFF_NEXT);
    ASSERT_STR_EQ("audio Next shortcut", shortcut ? shortcut : "(null)",
                  "Ctrl+Right");
    shortcut = ui_menu_bar_menu_item_shortcut_for_test(GLR_MENU_AUDIO,
                                                       2 + GLR_AUDIO_OFF_PREV);
    ASSERT_STR_EQ("audio Previous shortcut", shortcut ? shortcut : "(null)",
                  "Ctrl+Left");

    ASSERT_TRUE("found Audio Loop row",
                find_dropdown_item_point(GLR_MENU_AUDIO,
                                         2 + GLR_AUDIO_OFF_LOOP,
                                         &loop_mx, &loop_my));
    hit = ui_menu_bar_hit_test(loop_mx, loop_my);
    ASSERT_INT_EQ("audio Loop row hit", hit.item_idx,
                  2 + GLR_AUDIO_OFF_LOOP);
}

#ifdef GL_STUBS
static void make_test_ui_snapshot(UiRenderSnapshot *snap) {
    memset(snap, 0, sizeof(*snap));
    snap->viewport = ui_state_viewport();
    snap->anim_time = 1.0f;
    snap->user_scene_active_idx = repl_active_user_scene();
}

/* The Scene menu's "Next" / "Previous" cycle rows carry the F12 /
 * Shift+F12 shortcuts (moved off the "### EXAMPLES" header), hit-test as
 * plain menu items, and - being leaf actions - have no flyout. */
static void test_scene_menu_cycle_rows(void) {
    int tag_count;
    int next_row, prev_row;
    const char *shortcut;
    int mx = -1, my = -1;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    UiHit hit;

    reset_menu_bar_fixture(1000, 600);
    tag_count = repl_example_visible_tag_count();
    next_row = tag_count + GLR_SCENE_OFF_NEXT;
    prev_row = tag_count + GLR_SCENE_OFF_PREV;

    shortcut = ui_menu_bar_menu_item_shortcut_for_test(GLR_MENU_SCENE, next_row);
    ASSERT_STR_EQ("scene Next shortcut", shortcut ? shortcut : "(null)", "F12");
    shortcut = ui_menu_bar_menu_item_shortcut_for_test(GLR_MENU_SCENE, prev_row);
    ASSERT_STR_EQ("scene Previous shortcut", shortcut ? shortcut : "(null)",
                  "Shift+F12");

    ASSERT_TRUE("found Scene Next row",
                find_dropdown_item_point(GLR_MENU_SCENE, next_row, &mx, &my));
    hit = ui_menu_bar_hit_test(mx, my);
    ASSERT_INT_EQ("scene Next row hit kind", hit.kind, UI_HIT_MENU_ITEM);
    ASSERT_INT_EQ("scene Next row hit menu", hit.cmd_idx, GLR_MENU_SCENE);
    ASSERT_INT_EQ("scene Next row hit idx", hit.item_idx, next_row);

    ASSERT_TRUE("found Scene Previous row",
                find_dropdown_item_point(GLR_MENU_SCENE, prev_row, &mx, &my));
    hit = ui_menu_bar_hit_test(mx, my);
    ASSERT_INT_EQ("scene Previous row hit idx", hit.item_idx, prev_row);

    ASSERT_TRUE("scene Next row has no flyout",
                !ui_menu_bar_submenu_rect_for_test(GLR_MENU_SCENE, next_row,
                                                   &sx, &sy, &sw, &sh));
    ASSERT_TRUE("scene Previous row has no flyout",
                !ui_menu_bar_submenu_rect_for_test(GLR_MENU_SCENE, prev_row,
                                                   &sx, &sy, &sw, &sh));

    /* The "---" dividers bracketing Next/Previous are inert chrome: they
     * produce no MENU_ITEM hit (find_dropdown_item_point returns 0). */
    ASSERT_TRUE("scene top divider is inert",
                !find_dropdown_item_point(GLR_MENU_SCENE,
                                          tag_count + GLR_SCENE_OFF_SEP_TOP,
                                          &mx, &my));
    ASSERT_TRUE("scene bottom divider is inert",
                !find_dropdown_item_point(GLR_MENU_SCENE,
                                          tag_count + GLR_SCENE_OFF_SEP_BOT,
                                          &mx, &my));
}

static void test_scene_submenu_with_stubs(void) {
    UiRenderSnapshot snap;
    UiHit hit;
    int tag_idx;
    int example_idx;
    int tag_mx = -1;
    int tag_my = -1;
    int sx = 0;
    int sy = 0;
    int sw = 0;
    int sh = 0;
    int sub_mx = -1;
    int sub_my = -1;
    unsigned long long base_raster_calls;
    unsigned long long hover_raster_calls;

    /* Tall fixture: the per-row render assertion below needs the whole tag
     * flyout onscreen - 600px clamps it once the catalog grows past ~32
     * entries plus group subheading rows. */
    reset_menu_bar_fixture(1000, 1000);
    make_test_ui_snapshot(&snap);
    tag_idx = repl_example_visible_tag_at(0);
    ASSERT_TRUE("first visible example tag exists", tag_idx >= 0);
    ASSERT_TRUE("found first Scene tag hover point",
                find_dropdown_item_point(GLR_MENU_SCENE, 1, &tag_mx, &tag_my));

    snap.pointer.mouse_x = ui_state_viewport().window_w - 1;
    snap.pointer.mouse_y = ui_state_viewport().window_h - 1;
    gl_stub_counts_reset();
    ui_menu_bar_render_example_dropdown(&snap);
    base_raster_calls = gl_stub_counts[GL_STUB_glRasterPos2f];

    ASSERT_TRUE("Scene tag hover update opens submenu",
                ui_menu_bar_update_pointer_hover(tag_mx, tag_my, snap.anim_time));
    ASSERT_TRUE("Scene submenu rect available after hover update",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_SCENE,
                                                  ui_menu_bar_scene_parent_row_for_tag(tag_idx),
                                                  &sx, &sy,
                                                  &sw, &sh));

    example_idx = repl_example_index_for_tag(tag_idx, 0);
    ASSERT_TRUE("first submenu example exists", example_idx >= 0);

    /* Walk submenu ordinals to find the first ITEM row. Subheading
     * HEADER chrome rows may precede the first example (e.g. "### Basics"
     * before "Lit cube" in the All flyout); skip them so this test
     * stays robust to catalog grouping changes. */
    int first_item_ord = -1;
    for (int o = 0; o < 64; o++) {
        int mx, my;
        if (!submenu_row_point(sx, sy, sw, sh, o, &mx, &my))
            break;
        UiHit probe = ui_menu_bar_hit_test(mx, my);
        if (probe.kind == UI_HIT_SUBMENU_ITEM) {
            first_item_ord = o;
            sub_mx = mx;
            sub_my = my;
            break;
        }
    }
    ASSERT_TRUE("found first submenu ITEM row", first_item_ord >= 0);

    hit = ui_menu_bar_hit_test(sub_mx, sub_my);
    ASSERT_INT_EQ("hover update makes submenu hittable",
                  hit.kind, UI_HIT_SUBMENU_ITEM);
    ASSERT_INT_EQ("hit_test: submenu carries menu_id",
                  hit.cmd_idx, GLR_MENU_SCENE);
    ASSERT_INT_EQ("hit_test: submenu example", hit.item_idx, example_idx);
    ASSERT_INT_EQ("hit_test: submenu ordinal",
                  hit.line_idx, first_item_ord);

    snap.pointer.mouse_x = tag_mx;
    snap.pointer.mouse_y = tag_my;
    gl_stub_counts_reset();
    ui_menu_bar_render_example_dropdown(&snap);
    hover_raster_calls = gl_stub_counts[GL_STUB_glRasterPos2f];
    ASSERT_TRUE("Scene tag hover renders submenu rows",
                hover_raster_calls >=
                base_raster_calls +
                (unsigned long long)repl_example_count_for_tag(tag_idx));

    snap.pointer.mouse_x = sub_mx;
    snap.pointer.mouse_y = sub_my;
    snap.scenes.active_example_idx = example_idx;
    gl_stub_counts_reset();
    ui_menu_bar_render_example_dropdown(&snap);
    ASSERT_TRUE("Scene submenu active example render path draws",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0);

    reset_menu_bar_fixture(420, 600);
    tag_idx = repl_example_visible_tag_at(0);
    ASSERT_TRUE("found narrow Scene tag point",
                find_dropdown_item_point(GLR_MENU_SCENE, 1, &tag_mx, &tag_my));
    ASSERT_TRUE("narrow submenu rect available",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_SCENE,
                                                  ui_menu_bar_scene_parent_row_for_tag(tag_idx),
                                                  &sx, &sy,
                                                  &sw, &sh));
    ASSERT_TRUE("narrow submenu flips left of parent point", sx < tag_mx);
    ASSERT_TRUE("narrow submenu stays onscreen", sx + sw <= ui_state_viewport().window_w);
}

/* Step 9: positive Config-flyout coverage mirroring
 * test_scene_submenu_with_stubs - open Config, hover section 0, assert
 * the flyout rect/hit/render, and that the "All" flyout keeps its
 * "### "/"---" chrome inert (Finding #4). */
static void test_config_submenu_with_stubs(void) {
    UiRenderSnapshot snap;
    UiHit hit;
    int parent_mx = -1, parent_my = -1;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    int row_mx = -1, row_my = -1;
    int start = 0, count = 0;
    unsigned long long base_raster, hover_raster;

    reset_menu_bar_fixture(1000, 600);
    make_test_ui_snapshot(&snap);

    ASSERT_TRUE("found Config section-0 parent point",
                find_dropdown_item_point(GLR_MENU_CONFIG, 0,
                                         &parent_mx, &parent_my));

    snap.pointer.mouse_x = ui_state_viewport().window_w - 1;
    snap.pointer.mouse_y = ui_state_viewport().window_h - 1;
    gl_stub_counts_reset();
    ui_menu_bar_render_example_dropdown(&snap);
    base_raster = gl_stub_counts[GL_STUB_glRasterPos2f];

    ASSERT_TRUE("Config section hover opens flyout",
                ui_menu_bar_update_pointer_hover(parent_mx, parent_my,
                                                 snap.anim_time));
    ASSERT_TRUE("Config flyout rect available after hover",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_CONFIG, 0,
                                                  &sx, &sy, &sw, &sh));
    ASSERT_TRUE("section 0 range resolves",
                glr_config_section_range(0, &start, &count) && count > 0);

    ASSERT_TRUE("Config flyout row 0 point computed",
                submenu_row_point(sx, sy, sw, sh, 0, &row_mx, &row_my));
    hit = ui_menu_bar_hit_test(row_mx, row_my);
    ASSERT_INT_EQ("Config flyout row is a submenu hit",
                  hit.kind, UI_HIT_SUBMENU_ITEM);
    ASSERT_INT_EQ("Config flyout hit carries menu_id",
                  hit.cmd_idx, GLR_MENU_CONFIG);
    ASSERT_INT_EQ("Config flyout hit carries absolute g_cfg_items idx",
                  hit.item_idx, start);
    ASSERT_INT_EQ("Config flyout hit ordinal", hit.line_idx, 0);

    snap.pointer.mouse_x = parent_mx;
    snap.pointer.mouse_y = parent_my;
    gl_stub_counts_reset();
    ui_menu_bar_render_example_dropdown(&snap);
    hover_raster = gl_stub_counts[GL_STUB_glRasterPos2f];
    ASSERT_TRUE("Config section hover renders flyout rows",
                hover_raster > base_raster);

    /* "All" flyout (last parent row) spans the whole table; a point on
     * its first row - a "### " HEADER - must be inert chrome, NOT a
     * submenu hit. */
    {
        int all_row = glr_config_section_count();  /* synthetic All */
        int amx = -1, amy = -1, asx = 0, asy = 0, asw = 0, ash = 0;
        int hdr_mx = -1, hdr_my = -1;
        UiHit ah;

        ASSERT_TRUE("found All parent point",
                    find_dropdown_item_point(GLR_MENU_CONFIG, all_row,
                                             &amx, &amy));
        ui_menu_bar_update_pointer_hover(amx, amy, snap.anim_time);
        ASSERT_TRUE("All flyout rect available",
                    ui_menu_bar_submenu_rect_for_test(GLR_MENU_CONFIG,
                                                      all_row,
                                                      &asx, &asy,
                                                      &asw, &ash));
        ASSERT_INT_EQ("All flyout row 0 is a HEADER",
                      glr_config_row_kind(0), GLR_CFG_ROW_HEADER);
        ASSERT_TRUE("All flyout header point computed",
                    submenu_row_point(asx, asy, asw, ash, 0,
                                      &hdr_mx, &hdr_my));
        ah = ui_menu_bar_hit_test(hdr_mx, hdr_my);
        /* A header inside the flyout must be *consumed inertly*, not
         * merely "not a submenu hit": returning UI_HIT_NONE would let
         * the click fall through to the tabs/code/scene drawn beneath
         * the flyout (overlay-precedence). */
        ASSERT_INT_EQ("All flyout header is inert chrome",
                      ah.kind, UI_HIT_CODE_PANEL_CHROME);
    }
}

static void test_tutorial_submenu_with_stubs(void) {
    UiRenderSnapshot snap;
    UiHit hit;
    int tag_idx;
    int tutorial_idx;
    int tag_mx = -1;
    int tag_my = -1;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    int sub_mx = -1, sub_my = -1;
    unsigned long long base_raster_calls;
    unsigned long long hover_raster_calls;

    reset_menu_bar_fixture(1000, 600);
    make_test_ui_snapshot(&snap);
    snap.tutorial.visible_tag_count = repl_tutorial_visible_tag_count();

    tag_idx = repl_tutorial_visible_tag_at(0);
    ASSERT_TRUE("first visible tutorial tag exists", tag_idx >= 0);
    ASSERT_TRUE("found first Tutorial tag hover point",
                find_dropdown_item_point(GLR_MENU_TUTORIALS, 0, &tag_mx, &tag_my));

    snap.pointer.mouse_x = ui_state_viewport().window_w - 1;
    snap.pointer.mouse_y = ui_state_viewport().window_h - 1;
    gl_stub_counts_reset();
    ui_menu_bar_render_example_dropdown(&snap);
    base_raster_calls = gl_stub_counts[GL_STUB_glRasterPos2f];

    ASSERT_TRUE("Tutorial tag hover update opens submenu",
                ui_menu_bar_update_pointer_hover(tag_mx, tag_my, snap.anim_time));
    ASSERT_TRUE("Tutorial submenu rect available after hover update",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_TUTORIALS,
                                                   ui_menu_bar_tutorial_parent_row_for_tag(tag_idx),
                                                   &sx, &sy, &sw, &sh));

    tutorial_idx = repl_tutorial_index_for_tag(tag_idx, 0);
    ASSERT_TRUE("first submenu tutorial exists", tutorial_idx >= 0);

    /* Walk submenu ordinals to find the first ITEM row. */
    int first_item_ord = -1;
    for (int o = 0; o < 64; o++) {
        int mx, my;
        if (!submenu_row_point(sx, sy, sw, sh, o, &mx, &my))
            break;
        UiHit probe = ui_menu_bar_hit_test(mx, my);
        if (probe.kind == UI_HIT_SUBMENU_ITEM) {
            first_item_ord = o;
            sub_mx = mx;
            sub_my = my;
            break;
        }
    }
    ASSERT_TRUE("found first Tutorial submenu ITEM row", first_item_ord >= 0);

    hit = ui_menu_bar_hit_test(sub_mx, sub_my);
    ASSERT_INT_EQ("hover update makes Tutorial submenu hittable",
                  hit.kind, UI_HIT_SUBMENU_ITEM);
    ASSERT_INT_EQ("hit_test: Tutorial submenu carries menu_id",
                  hit.cmd_idx, GLR_MENU_TUTORIALS);
    ASSERT_INT_EQ("hit_test: Tutorial submenu tutorial index",
                  hit.item_idx, tutorial_idx);
    ASSERT_INT_EQ("hit_test: Tutorial submenu ordinal",
                  hit.line_idx, first_item_ord);

    snap.pointer.mouse_x = tag_mx;
    snap.pointer.mouse_y = tag_my;
    gl_stub_counts_reset();
    ui_menu_bar_render_example_dropdown(&snap);
    hover_raster_calls = gl_stub_counts[GL_STUB_glRasterPos2f];
    ASSERT_TRUE("Tutorial tag hover renders submenu rows",
                hover_raster_calls >=
                base_raster_calls +
                (unsigned long long)repl_tutorial_count_for_tag(tag_idx));

    snap.pointer.mouse_x = sub_mx;
    snap.pointer.mouse_y = sub_my;
    snap.tutorial.active = 1;
    snap.tutorial.tutorial_idx = tutorial_idx;
    gl_stub_counts_reset();
    ui_menu_bar_render_example_dropdown(&snap);
    ASSERT_TRUE("Tutorial submenu active tutorial render path draws",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0);
}

/* Audit #3 (Bug, fd70b4e) regression: ui_menu_bar_render_example_dropdown
 * must be free of side-effects on g_menu_item_hover / g_submenu_*. The
 * pre-fix code called ui_menu_bar_update_pointer_hover() from inside the
 * render path, double-mutating hover state with whatever the snapshot's
 * pointer.{mouse_x,mouse_y} happened to be. Probe by seeding hover at A,
 * rendering with snap.pointer at a different point B, then re-seeding at
 * A - the return value (changed?) is the observable signal. */
static void test_render_does_not_mutate_hover(void) {
    UiRenderSnapshot snap;
    int item_a_mx = -1, item_a_my = -1;
    int item_b_mx = -1, item_b_my = -1;
    int reseed_changed;

    reset_menu_bar_fixture(1000, 600);
    make_test_ui_snapshot(&snap);

    /* Pick two distinct dropdown items so seed-A vs render-pointer-B
     * cannot accidentally coincide. */
    ASSERT_TRUE("found file item A point",
                find_dropdown_item_point(GLR_MENU_FILE, GLR_FILE_ITEM_NEW_SCENE,
                                         &item_a_mx, &item_a_my));
    ASSERT_TRUE("found file item B point",
                find_dropdown_item_point(GLR_MENU_FILE, GLR_FILE_ITEM_SAVE_SCENE,
                                         &item_b_mx, &item_b_my));
    ASSERT_TRUE("A and B distinct rows",
                item_a_my != item_b_my);

    /* Seed: hover at A. First call changes state (returns 1); second is
     * stable (returns 0). */
    ASSERT_TRUE("seed hover at A returns changed",
                ui_menu_bar_update_pointer_hover(item_a_mx, item_a_my,
                                                  snap.anim_time));
    ASSERT_TRUE("re-seed at A returns unchanged",
                !ui_menu_bar_update_pointer_hover(item_a_mx, item_a_my,
                                                   snap.anim_time));

    /* Render with snap.pointer pointing at B. A pure render leaves
     * hover at A; a mutating render writes hover=B. */
    snap.pointer.mouse_x = item_b_mx;
    snap.pointer.mouse_y = item_b_my;
    ui_menu_bar_render_example_dropdown(&snap);

    /* Re-seed at A. If render kept hover at A, this returns 0 (no
     * change). If render mutated to B, going B->A returns 1. */
    reseed_changed = ui_menu_bar_update_pointer_hover(item_a_mx, item_a_my,
                                                       snap.anim_time);
    ASSERT_TRUE("render did not mutate hover (re-seed unchanged)",
                !reseed_changed);
}

static void test_render_paths_with_stubs(void) {
    UiRenderSnapshot snap;
    int item_mx = -1;
    int item_my = -1;

    reset_menu_bar_fixture(1000, 600);
    make_test_ui_snapshot(&snap);

    gl_stub_counts_reset();
    ui_menu_bar_render(&snap);
    ASSERT_TRUE("menu bar render draws background", gl_stub_counts[GL_STUB_glRectf] > 0);

    snap.replay.active = 1;
    snap.replay.state = REPLAY_PLAYING;
    gl_stub_counts_reset();
    ui_menu_bar_render(&snap);
    ASSERT_TRUE("menu bar render covers replay playing", gl_stub_counts[GL_STUB_glRectf] > 0);

    snap.replay.state = REPLAY_DONE;
    gl_stub_counts_reset();
    ui_menu_bar_render(&snap);
    ASSERT_TRUE("menu bar render covers replay done", gl_stub_counts[GL_STUB_glRectf] > 0);

    ASSERT_TRUE("found file dropdown point for render",
                find_dropdown_item_point(GLR_MENU_FILE, GLR_FILE_ITEM_NEW_SCENE,
                                         &item_mx, &item_my));
    snap.pointer.mouse_x = item_mx;
    snap.pointer.mouse_y = item_my;
    gl_stub_counts_reset();
    ui_menu_bar_render_example_dropdown(&snap);
    ASSERT_TRUE("file dropdown renders text", gl_stub_counts[GL_STUB_glRasterPos2f] > 0);

    ui_menu_bar_set_open_menu(GLR_MENU_SCENE, 0.0f);
    snap.scenes.active_example_idx = 0;
    gl_stub_counts_reset();
    ui_menu_bar_render_example_dropdown(&snap);
    ASSERT_TRUE("scene dropdown renders rows", gl_stub_counts[GL_STUB_glRasterPos2f] > 0);

    ui_menu_bar_set_open_menu(GLR_MENU_CONFIG, 0.0f);
    gl_stub_counts_reset();
    ui_menu_bar_render_example_dropdown(&snap);
    ASSERT_TRUE("config dropdown renders rows", gl_stub_counts[GL_STUB_glRasterPos2f] > 0);

    ui_menu_bar_note_search_opened(0.0f);
    snap.search.active = 1;
    gl_stub_counts_reset();
    ui_menu_bar_render_search_overlay(&snap);
    ASSERT_TRUE("search overlay renders placeholder", gl_stub_counts[GL_STUB_glRasterPos2f] > 0);

    snprintf(snap.search.query, sizeof(snap.search.query),
             "search-term-that-needs-truncation");
    snap.search.query_len = (int)strlen(snap.search.query);
    snap.search.cursor_pos = snap.search.query_len;
    snap.search.match_count = 0;
    snap.anim_time = 2.0f;
    gl_stub_counts_reset();
    ui_menu_bar_render_search_overlay(&snap);
    ASSERT_TRUE("search overlay renders zero matches", gl_stub_counts[GL_STUB_glRasterPos2f] > 0);

    snap.search.match_count = 9;
    snap.search.hit_ordinal = 3;
    snap.cursor_blink.cursor_visible = 1;
    gl_stub_counts_reset();
    ui_menu_bar_render_search_overlay(&snap);
    ASSERT_TRUE("search overlay renders cursor and count", gl_stub_counts[GL_STUB_glRectf] > 0);
}
#endif

/* #52/#53: pin Config section labels (raw and display) so the
 * pre-computed title-case path stays correct. */
static void test_config_section_labels(void) {
    static const char *expected_raw[] = {
        "RENDERING",
        "TIME & REPLAY",
        "SCENE",
        "CAMERA",
        "GEOMETRY",
        "OVERLAYS",
        "INTERFACE",
    };
    static const char *expected_display[] = {
        "Rendering",
        "Time & replay",
        "Scene",
        "Camera",
        "Geometry",
        "Overlays",
        "Interface",
    };
    int n = glr_config_section_count();
    ASSERT_INT_EQ("section count", n, 7);
    for (int i = 0; i < n && i < 7; i++) {
        const char *label = glr_config_section_label(i);
        ASSERT_TRUE("section label not null", label != NULL);
        if (label)
            ASSERT_TRUE(expected_raw[i], strcmp(label, expected_raw[i]) == 0);
        const char *disp = glr_config_section_display_label(i);
        ASSERT_TRUE("display label not null", disp != NULL);
        if (disp)
            ASSERT_TRUE(expected_display[i],
                        strcmp(disp, expected_display[i]) == 0);
    }
    ASSERT_TRUE("past-end section label is null",
                glr_config_section_label(n) == NULL);
    ASSERT_TRUE("past-end display label is null",
                glr_config_section_display_label(n) == NULL);
}

#ifdef GL_STUBS
/* #50/#51 regression: pin dropdown geometry values so the magic-constant
 * refactor doesn't silently change dropdown sizing or position. */
static void test_dropdown_geometry_pinned(void) {
    int sx, sy, sw, sh;
    int parent_mx = -1, parent_my = -1;

    reset_menu_bar_fixture(1000, 600);

    ui_menu_bar_set_open_menu(GLR_MENU_FILE, 0.0f);
    ASSERT_TRUE("File dropdown geometry exists",
                find_dropdown_item_point(GLR_MENU_FILE, GLR_FILE_ITEM_NEW_SCENE,
                                         &parent_mx, &parent_my));

    UiHit h = ui_menu_bar_hit_test(parent_mx, parent_my);
    ASSERT_INT_EQ("File dropdown item hit kind", h.kind, UI_HIT_MENU_ITEM);

    ui_menu_bar_set_open_menu(GLR_MENU_CONFIG, 0.0f);
    ASSERT_TRUE("Config section-0 parent point found",
                find_dropdown_item_point(GLR_MENU_CONFIG, 0,
                                         &parent_mx, &parent_my));
    ASSERT_TRUE("Config section hover opens flyout",
                ui_menu_bar_update_pointer_hover(parent_mx, parent_my, 0.0f));

    ASSERT_TRUE("Config flyout rect after hover",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_CONFIG, 0,
                                                  &sx, &sy, &sw, &sh));
    ASSERT_TRUE("flyout width positive", sw > 0);
    ASSERT_TRUE("flyout height positive", sh > 0);
    ASSERT_TRUE("flyout x non-negative", sx >= 0);

    int start = 0, count = 0;
    ASSERT_TRUE("section 0 range resolves",
                glr_config_section_range(0, &start, &count) && count > 0);
    ASSERT_TRUE("flyout height fits section items",
                sh >= count * LINE_H);
}

/* Audit #50/#51 (Tier C) cache-correctness regression: the menu dropdown
 * rect is now cached keyed on (open_menu, window_size). Verify the cache
 * returns stable values within the same open menu, and invalidates when
 * the window size changes. The pre-cache test only asserted shape (>0).
 * Cache regressions would silently serve stale rects after resize. */
static void test_dropdown_geometry_cache_invariants(void) {
    int sx_a1, sy_a1, sw_a1, sh_a1;
    int sx_a2, sy_a2, sw_a2, sh_a2;
    int sx_a3, sy_a3, sw_a3, sh_a3;
    int parent_mx = -1, parent_my = -1;
    int start = 0, count = 0;

    reset_menu_bar_fixture(1000, 600);

    /* Open Config + hover section 0 to open its flyout. */
    ASSERT_TRUE("Config section-0 parent point found (1000x600)",
                find_dropdown_item_point(GLR_MENU_CONFIG, 0,
                                         &parent_mx, &parent_my));
    ASSERT_TRUE("Config section hover opens flyout (1000x600)",
                ui_menu_bar_update_pointer_hover(parent_mx, parent_my, 0.0f));
    ASSERT_TRUE("flyout rect (1000x600)",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_CONFIG, 0,
                                                  &sx_a1, &sy_a1, &sw_a1, &sh_a1));

    /* Tight pin: flyout height equals section's item count x LINE_H plus
     * the dropdown padding constants. Use the exact value the cache
     * returns the first time so a stale-cache regression would surface
     * a mismatch on subsequent calls. */
    ASSERT_TRUE("section 0 range resolves",
                glr_config_section_range(0, &start, &count) && count > 0);

    /* Second call: cache hit should return identical rect. */
    ASSERT_TRUE("flyout rect (1000x600) second call",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_CONFIG, 0,
                                                  &sx_a2, &sy_a2, &sw_a2, &sh_a2));
    ASSERT_INT_EQ("flyout x stable across calls", sx_a2, sx_a1);
    ASSERT_INT_EQ("flyout y stable across calls", sy_a2, sy_a1);
    ASSERT_INT_EQ("flyout w stable across calls", sw_a2, sw_a1);
    ASSERT_INT_EQ("flyout h stable across calls", sh_a2, sh_a1);

    /* Resize window: dropdown_rect cache is keyed on (menu, win_w, win_h)
     * and submenu_rect's geometry derives from the dropdown rect's x/y.
     * A regression that ignores the window-size key would serve A1's
     * rect even at 600x400, so y (top-anchored from win_h) would not
     * match the new viewport. */
    ui_state_viewport_set_size(600, 400);
    /* Re-hover at the new (rebuilt) parent point so the cache refreshes
     * its parent-row mapping under the new viewport. */
    ASSERT_TRUE("Config section-0 parent point found (600x400)",
                find_dropdown_item_point(GLR_MENU_CONFIG, 0,
                                         &parent_mx, &parent_my));
    ASSERT_TRUE("Config section hover opens flyout (600x400)",
                ui_menu_bar_update_pointer_hover(parent_mx, parent_my, 0.0f));
    ASSERT_TRUE("flyout rect (600x400)",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_CONFIG, 0,
                                                  &sx_a3, &sy_a3, &sw_a3, &sh_a3));
    ASSERT_TRUE("flyout y differs after window resize",
                sy_a3 != sy_a1);
}

/* #47 regression: pin that Scene tag rows (hover-only) stay inert when
 * activated, and that clicks inside an open dropdown on non-actionable
 * regions don't fall through. This guards the two UI_HIT_CODE_PANEL_CHROME
 * blocks in hit_test. */
static void test_dropdown_inert_chrome_hit(void) {
    int tag_mx = -1, tag_my = -1;

    reset_menu_bar_fixture(1000, 600);

    ASSERT_TRUE("found Scene tag row 1 point",
                find_dropdown_item_point(GLR_MENU_SCENE, 1,
                                         &tag_mx, &tag_my));

    UiHit h = ui_menu_bar_hit_test(tag_mx, tag_my);
    ASSERT_INT_EQ("Scene tag row is MENU_ITEM",
                  h.kind, UI_HIT_MENU_ITEM);
    ASSERT_INT_EQ("Scene tag row has correct item_idx",
                  h.item_idx, 1);
    ASSERT_INT_EQ("Scene tag row activation is inert (returns 0)",
                  glr_action_menu_item_activate(GLR_MENU_SCENE, 1), 0);
}
#endif

/* Step 7 (Finding #3), now routed through the controller: a right-press
 * over a Config flyout item backward-cycles it. Production resolves the
 * press with the canonical hit-test (route_right_press in
 * glr_ctrl_router.c -> ui_panels_hit_test -> ui_menu_bar_hit_test ->
 * submenu_hit_test) and cycles ONLY on a UI_HIT_SUBMENU_ITEM hit with
 * cmd_idx == GLR_MENU_CONFIG. submenu_hit_test resolves actionable ITEM
 * rows only - chrome ("### "/"---" in the "All" flyout) is skipped via
 * submenu_row_kind, and section/All parent rows own no submenu row - so
 * a right-press over the section list (or a closed menu) never
 * mis-cycles a g_cfg_items[] index. This test drives the same filter
 * against the hit-test. */
static void test_config_submenu_right_press(void) {
    int parent_mx = -1, parent_my = -1;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    int row_mx = -1, row_my = -1;
    int start = 0, count = 0;

    reset_menu_bar_fixture(1000, 600);

    /* Open Config and hover section row 0 ("RENDERING") so its flyout
     * opens. */
    ASSERT_TRUE("found Config section-0 parent point",
                find_dropdown_item_point(GLR_MENU_CONFIG, 0,
                                         &parent_mx, &parent_my));
    ui_menu_bar_update_pointer_hover(parent_mx, parent_my, 0.0f);

    ASSERT_TRUE("section 0 range resolves",
                glr_config_section_range(0, &start, &count) && count > 0);
    ASSERT_TRUE("Config flyout rect available",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_CONFIG, 0,
                                                  &sx, &sy, &sw, &sh));

    const GlrConfigItem *item = glr_config_item_at(start); /* ordinal 0 */
    ASSERT_TRUE("flyout item 0 is a real config item",
                item && !item->section_header &&
                item->key != GLR_CONFIG_NONE);

    int sc = item->state_count;
    int before = glr_config_get(item->key);
    int expected = (before - 1 + sc) % sc;

    ASSERT_TRUE("flyout row 0 point computed",
                submenu_row_point(sx, sy, sw, sh, 0, &row_mx, &row_my));
    UiHit hit = ui_menu_bar_hit_test(row_mx, row_my);
    ASSERT_INT_EQ("right-press on flyout item kind",
                  hit.kind, UI_HIT_SUBMENU_ITEM);
    ASSERT_INT_EQ("right-press on flyout item menu id",
                  hit.cmd_idx, GLR_MENU_CONFIG);
    ASSERT_INT_EQ("right-press on flyout item index",
                  hit.item_idx, start);
    if (hit.kind == UI_HIT_SUBMENU_ITEM && hit.cmd_idx == GLR_MENU_CONFIG &&
        hit.item_idx >= 0) {
        glr_cfg_cycle_row(hit.item_idx, -1);
    }
    ASSERT_INT_EQ("right-press cycled the item backward",
                  glr_config_get(item->key), expected);

    /* The section parent row itself (not the flyout) resolves to a
     * dropdown MENU_ITEM, not a SUBMENU_ITEM, so the production filter
     * never cycles from it. */
    UiHit parent_hit = ui_menu_bar_hit_test(parent_mx, parent_my);
    ASSERT_TRUE("right-press on section parent row resolves no flyout item",
                parent_hit.kind != UI_HIT_SUBMENU_ITEM);

    ui_menu_bar_close();
    UiHit closed_hit = ui_menu_bar_hit_test(row_mx, row_my);
    ASSERT_TRUE("right-press when Config closed resolves no flyout item",
                closed_hit.kind != UI_HIT_SUBMENU_ITEM);
}

/* Audit #4 cache-correctness regression: submenu_rect caches its result
 * keyed on (menu_id, parent_row, win_w, win_h). The cache hit path
 * bypasses the menu_dropdown_rect call that would otherwise enforce
 * ui_menu_bar_panel_visible(). If the panel becomes invisible while
 * the cache is warm, the next submenu_rect call must return 0 - not
 * the stale cached rect - because callers (hit-test, render) gate on
 * the return code, not the rect contents. */
static void test_submenu_cache_invalidates_when_panel_hidden(void) {
    int sx, sy, sw, sh;
    int parent_mx = -1, parent_my = -1;

    reset_menu_bar_fixture(1000, 600);

    /* Open Config, hover the section-0 parent row to open its flyout,
     * then call submenu_rect once to warm the cache. */
    ASSERT_TRUE("Config section-0 parent point found",
                find_dropdown_item_point(GLR_MENU_CONFIG, 0,
                                         &parent_mx, &parent_my));
    ASSERT_TRUE("Config section hover opens flyout",
                ui_menu_bar_update_pointer_hover(parent_mx, parent_my, 0.0f));
    ASSERT_TRUE("submenu_rect succeeds while panel visible",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_CONFIG, 0,
                                                  &sx, &sy, &sw, &sh));

    /* Hide the code panel without closing the menu. ui_menu_bar_panel_visible()
     * now returns 0 (rect collapses to zero width/height). The cache key
     * (menu_id, parent_row, win_w, win_h) hasn't changed, so a stale
     * cache hit would still report success. */
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
    glr_ctrl_sync_ui_chrome();

    ASSERT_INT_EQ("submenu_rect fails when panel becomes invisible",
                  ui_menu_bar_submenu_rect_for_test(GLR_MENU_CONFIG, 0,
                                                    &sx, &sy, &sw, &sh),
                  0);
}

/* Long-flyout scrolling: the Config "All" flyout (~47 rows) is taller
 * than a small viewport, so submenu_rect clamps its height and the mouse
 * wheel pages through the hidden rows. submenu_row_point() addresses rows
 * by VISIBLE position (0 = topmost on-screen row), so a fixed screen point
 * maps to a higher absolute ordinal as the flyout scrolls. Pure
 * geometry/hit-test - runs in both stub and non-stub builds. */
static void test_config_all_flyout_scroll(void) {
    int sx, sy, sw, sh;
    int parent_mx = -1, parent_my = -1;
    int all_row = glr_config_section_count();   /* synthetic All parent */
    int visible_rows, max_scroll, mx = -1, my = -1;

    reset_menu_bar_fixture(1000, 600);

    ASSERT_TRUE("All parent point found",
                find_dropdown_item_point(GLR_MENU_CONFIG, all_row,
                                         &parent_mx, &parent_my));
    ASSERT_TRUE("hover opens All flyout",
                ui_menu_bar_update_pointer_hover(parent_mx, parent_my, 0.0f));
    ASSERT_TRUE("All flyout rect",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_CONFIG, all_row,
                                                  &sx, &sy, &sw, &sh));

    /* Clamped: fits the 600px viewport AND is genuinely shorter than the
     * full 47-row natural height (proves the overflow path engaged). */
    ASSERT_TRUE("All flyout height fits viewport", sh <= 600);
    ASSERT_TRUE("All flyout height clamped below natural height",
                sh < CFG_ITEM_COUNT * LINE_H);

    visible_rows = (sh - 2 * DROPDOWN_PAD_Y) / LINE_H;
    max_scroll   = CFG_ITEM_COUNT - visible_rows;
    ASSERT_TRUE("flyout overflows (rows hidden)", max_scroll > 0);

    ASSERT_TRUE("point on visible row 1",
                submenu_row_point(sx, sy, sw, sh, 1, &mx, &my));

    /* A wheel event outside the flyout is NOT consumed (it falls through
     * to code panel / camera). Pick a column outside the flyout rect. */
    {
        int out_mx = (sx >= 5) ? sx - 5 : sx + sw + 5;
        ASSERT_INT_EQ("wheel outside flyout not consumed",
                      ui_menu_bar_handle_wheel_scroll(out_mx, my, 1), 0);
    }

    /* Scroll past the bottom: consumed, clamps at max. The bottom-most
     * visible row now maps to the last absolute row (CFG_ITEM_COUNT-1). */
    ASSERT_INT_EQ("wheel over flyout consumed",
                  ui_menu_bar_handle_wheel_scroll(mx, my, 1000), 1);
    {
        int bmx = -1, bmy = -1;
        UiHit h;
        ASSERT_TRUE("point on last visible row",
                    submenu_row_point(sx, sy, sw, sh, visible_rows - 1,
                                      &bmx, &bmy));
        h = ui_menu_bar_hit_test(bmx, bmy);
        ASSERT_INT_EQ("bottom row is a submenu item", h.kind,
                      UI_HIT_SUBMENU_ITEM);
        ASSERT_INT_EQ("bottom row is the last config item",
                      h.item_idx, CFG_ITEM_COUNT - 1);
    }

    /* Scroll past the top: clamps at 0. Visible row 1 maps to absolute
     * ordinal 1 again (the second All row). */
    ui_menu_bar_handle_wheel_scroll(mx, my, -1000);
    {
        UiHit h = ui_menu_bar_hit_test(mx, my);
        ASSERT_INT_EQ("top: visible row 1 is a submenu item", h.kind,
                      UI_HIT_SUBMENU_ITEM);
        ASSERT_INT_EQ("top: visible row 1 maps to absolute ordinal 1",
                      h.line_idx, 1);
    }

    /* A bounded scroll shifts the mapping by exactly the delta: from the
     * top, +3 makes the same point show absolute ordinal 4 (both are item
     * rows in the leading RENDERING section). */
    ui_menu_bar_handle_wheel_scroll(mx, my, 3);
    {
        UiHit h = ui_menu_bar_hit_test(mx, my);
        ASSERT_INT_EQ("after +3 the row is still an item", h.kind,
                      UI_HIT_SUBMENU_ITEM);
        ASSERT_INT_EQ("after +3 the mapping shifts by 3", h.line_idx, 4);
    }
}

/* A flyout that fits the viewport must not clamp or scroll, but a wheel
 * event over it is still consumed (the open menu is modal over the code
 * panel behind it). */
static void test_config_short_flyout_consumes_but_no_scroll(void) {
    int sx, sy, sw, sh;
    int parent_mx = -1, parent_my = -1;
    int start = 0, count = 0, mx = -1, my = -1;
    UiHit before, after;

    reset_menu_bar_fixture(1000, 600);

    ASSERT_TRUE("section-0 parent point",
                find_dropdown_item_point(GLR_MENU_CONFIG, 0,
                                         &parent_mx, &parent_my));
    ASSERT_TRUE("hover opens section-0 flyout",
                ui_menu_bar_update_pointer_hover(parent_mx, parent_my, 0.0f));
    ASSERT_TRUE("section-0 flyout rect",
                ui_menu_bar_submenu_rect_for_test(GLR_MENU_CONFIG, 0,
                                                  &sx, &sy, &sw, &sh));

    ASSERT_TRUE("section 0 range",
                glr_config_section_range(0, &start, &count) && count > 0);
    ASSERT_TRUE("short flyout keeps full (unclamped) height",
                sh >= count * LINE_H);

    ASSERT_TRUE("point on section-0 row 0",
                submenu_row_point(sx, sy, sw, sh, 0, &mx, &my));
    before = ui_menu_bar_hit_test(mx, my);
    ASSERT_INT_EQ("short flyout row 0 is an item", before.kind,
                  UI_HIT_SUBMENU_ITEM);

    ASSERT_INT_EQ("wheel over short flyout is still consumed",
                  ui_menu_bar_handle_wheel_scroll(mx, my, 5), 1);
    after = ui_menu_bar_hit_test(mx, my);
    ASSERT_INT_EQ("short flyout did not scroll (kind)",
                  after.kind, before.kind);
    ASSERT_INT_EQ("short flyout did not scroll (ordinal)",
                  after.line_idx, before.line_idx);
}

/* Symbolic pointer-script targets (menu:/item:/subenter:/sub:/pin:/scene:)
 * must resolve to points the real hit-test classifies as the named element
 * - and must keep doing so at a different window size, since resolution
 * against the live layout is the whole point. */
static void test_symbolic_targets_resolve_to_hits(void) {
    static const int sizes[][2] = { { 1200, 800 }, { 1600, 1000 } };

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int mx = -1, my = -1;
        UiHit hit;

        reset_menu_bar_fixture(sizes[s][0], sizes[s][1]);
        /* The fixture parks panel_frac at 1.0 (no scene rect); scene:
         * targets need a real viewport slice like the running app has. */
        ui_state_code_panel_mut()->panel_frac = 0.42f;

        /* Menu button + pin resolve with no menu open. */
        ASSERT_TRUE("menu:scene resolves",
                    glr_pointer_script_resolve_target("menu:scene", &mx, &my));
        hit = ui_menu_bar_hit_test(mx, my);
        ASSERT_INT_EQ("menu:scene lands on the Scene button",
                      hit.kind == UI_HIT_MENU_BUTTON ? hit.cmd_idx : -1,
                      GLR_MENU_SCENE);

        ASSERT_TRUE("pin:replay resolves",
                    glr_pointer_script_resolve_target("pin:replay", &mx, &my));
        hit = ui_menu_bar_hit_test(mx, my);
        ASSERT_INT_EQ("pin:replay lands on the Replay pin",
                      hit.kind == UI_HIT_PIN_BUTTON ? hit.item_idx : -1,
                      UI_MENU_BAR_PIN_REPLAY);

        /* Row targets need their menu open. */
        ASSERT_TRUE("item: fails with no menu open",
                    !glr_pointer_script_resolve_target("item:new_scene",
                                                       &mx, &my));
        ui_menu_bar_set_open_menu(GLR_MENU_FILE, 0.0f);
        ASSERT_TRUE("item:new_scene resolves in the open File menu",
                    glr_pointer_script_resolve_target("item:new_scene",
                                                      &mx, &my));
        hit = ui_menu_bar_hit_test(mx, my);
        ASSERT_INT_EQ("item:new_scene lands on the New Scene row",
                      hit.kind == UI_HIT_MENU_ITEM ? hit.item_idx : -1,
                      GLR_FILE_ITEM_NEW_SCENE);

        /* Flyout targets: parent row hover opens the flyout for real,
         * then the sub: point must classify as that flyout row. */
        ui_menu_bar_set_open_menu(GLR_MENU_CONFIG, 0.0f);
        ASSERT_TRUE("item:overlays resolves in the open Config menu",
                    glr_pointer_script_resolve_target("item:overlays",
                                                      &mx, &my));
        ui_menu_bar_update_pointer_hover(mx, my, 0.0f);
        ASSERT_TRUE("subenter:overlays resolves",
                    glr_pointer_script_resolve_target("subenter:overlays",
                                                      &mx, &my));
        ASSERT_TRUE("sub:overlays:vertex_points resolves",
                    glr_pointer_script_resolve_target(
                        "sub:overlays:vertex_points", &mx, &my));
        hit = ui_menu_bar_hit_test(mx, my);
        ASSERT_TRUE("sub:overlays:vertex_points is a flyout-row hit",
                    hit.kind == UI_HIT_SUBMENU_ITEM &&
                    hit.cmd_idx == GLR_MENU_CONFIG);
        if (hit.kind == UI_HIT_SUBMENU_ITEM) {
            const GlrConfigItem *item = glr_config_item_at(hit.item_idx);
            ASSERT_STR_EQ("flyout row is the Vertex points item",
                          item && item->label ? item->label : "(none)",
                          "Vertex points");
        }
        ui_menu_bar_close();

        /* Scene-fraction target sits inside the scene rect. */
        ASSERT_TRUE("scene:0.5,0.5 resolves",
                    glr_pointer_script_resolve_target("scene:0.5,0.5",
                                                      &mx, &my));
        {
            int sx, sy, sw, sh;
            int gl_y;
            ui_layout_scene_rect(&sx, &sy, &sw, &sh);
            gl_y = ui_state_viewport().window_h - my;
            ASSERT_TRUE("scene point inside the scene rect",
                        mx >= sx && mx < sx + sw &&
                        gl_y >= sy && gl_y < sy + sh);
        }

        /* Unknown names fail rather than aim somewhere. */
        ASSERT_TRUE("unknown menu label fails",
                    !glr_pointer_script_resolve_target("menu:bogus",
                                                       &mx, &my));
        ASSERT_TRUE("unknown flyout row fails",
                    !glr_pointer_script_resolve_target("sub:overlays:bogus",
                                                       &mx, &my));
    }
}

/* helptab: and code: resolve against transient surfaces - the modal help
 * overlay and the code panel's visible rows - so both must fail closed when
 * the surface is absent and land on the real hit-test when it is up. */
static void test_help_and_code_targets_resolve_to_hits(void) {
    int mx = -1, my = -1;

    reset_menu_bar_fixture(1200, 800);
    ui_state_code_panel_mut()->panel_frac = 0.42f;

    ASSERT_TRUE("helptab: fails with the overlay closed",
                !glr_pointer_script_resolve_target("helptab:commands",
                                                   &mx, &my));
    glr_ctrl_toggle_help();
    ASSERT_TRUE("helptab:commands resolves with the overlay open",
                glr_pointer_script_resolve_target("helptab:commands",
                                                  &mx, &my));
    {
        UiOverlayState st = {
            0, editor_help_session_view().tab_idx,
            editor_help_session_view().scroll,
            ui_state_viewport().window_w, ui_state_viewport().window_h,
            glr_ctrl_help_overlay_content()
        };
        UiOverlayHit hit;
        st.visible = 1;
        hit = ui_tabbed_overlay_hit_test(&st, mx, my);
        ASSERT_INT_EQ("helptab:commands lands on a tab", (int)hit.kind,
                      (int)UI_OVERLAY_HIT_TAB);
        ASSERT_STR_EQ("...and it is the Commands tab",
                      hit.kind == UI_OVERLAY_HIT_TAB
                          ? st.content->tabs[hit.tab].label : "(none)",
                      "Commands");
    }
    ASSERT_TRUE("unknown tab label fails",
                !glr_pointer_script_resolve_target("helptab:bogus",
                                                   &mx, &my));
    glr_ctrl_close_help();

    ASSERT_TRUE("code: fails with an empty document",
                !glr_pointer_script_resolve_target("code:glcolor3f",
                                                   &mx, &my));
    editor_feed_line("glColor3f(1, 0.5, 0.2);");
    ASSERT_TRUE("code:glcolor3f resolves once the row exists",
                glr_pointer_script_resolve_target("code:glcolor3f",
                                                  &mx, &my));
    {
        UiRenderSnapshot snap;
        UiHit hit;
        glr_ctrl_build_ui_snapshot(&snap);
        hit = ui_repl_code_panel_hit_test(&snap, mx, my);
        ASSERT_INT_EQ("code: point is a code-text hit", (int)hit.kind,
                      (int)UI_HIT_CODE_TEXT);
        ASSERT_STR_EQ("...on the glColor3f row",
                      hit.kind == UI_HIT_CODE_TEXT
                          ? editor_buffer_line(hit.line_idx) : "(none)",
                      "  glColor3f(1, 0.5, 0.2);");   /* canonical indent */
    }
    ASSERT_TRUE("unmatched command text fails",
                !glr_pointer_script_resolve_target("code:glbogusf",
                                                   &mx, &my));
}

int main(void) {
    printf("--- ui_menu_bar tests ---\n");

    test_menu_bar_rect_helper();
    test_reveal_workspace_enabled_state();
    test_scene_file_action_enabled_state();
    test_workspace_header_row();
    test_open_close_state();
    test_top_level_hits();
    test_dropdown_and_config_press();
    test_audio_menu_flyout_hits();
    test_config_submenu_right_press();
    test_config_section_labels();
    test_config_all_flyout_scroll();
    test_config_short_flyout_consumes_but_no_scroll();
    test_symbolic_targets_resolve_to_hits();
    test_help_and_code_targets_resolve_to_hits();
#ifdef GL_STUBS
    test_msaa_label_dynamic();
    test_dropdown_geometry_pinned();
    test_dropdown_geometry_cache_invariants();
    test_submenu_cache_invalidates_when_panel_hidden();
    test_dropdown_inert_chrome_hit();
#endif
    test_unified_hit_test();
#ifdef GL_STUBS
    test_scene_menu_cycle_rows();
    test_scene_submenu_with_stubs();
    test_config_submenu_with_stubs();
    test_tutorial_submenu_with_stubs();
    test_render_does_not_mutate_hover();
    test_render_paths_with_stubs();
#endif

    printf("\n");
    return test_harness_report(&g_harness, "test_ui_menu_bar");
}
