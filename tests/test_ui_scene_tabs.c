/*
 * test_ui_scene_tabs.c - scene tab strip: derivation, geometry/hit,
 * band_h lockstep, and double-click rename routing.
 *
 * Core test (runs under `make test`); mirrors the controller-seeded
 * fixture of test_repl_code_panel_document.c.
 */
#include "app/glr_actions.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "editor/inline_rename.h"
#include "repl/example_loader.h"
#include "repl/host_effects.h"
#include "repl/scenes.h"
#include "repl/workspace_io.h"
#include "app/glr_workspaces.h"
#include "ui/app/hit.h"
#include "ui/app/layout.h"
#include "ui/app/menu_bar.h"
#include "ui/core/metrics.h"
#include "ui/app/panels.h"
#include "ui/app/scene_tabs.h"
#include "ui/app/snapshot.h"
#include "ui/app/state.h"
#include "ui/core/text_panel.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)   TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT_EQ(label, g, e) TEST_ASSERT_INT(&g_harness, label, g, e)
#define ASSERT_STR(label, g, e)    TEST_ASSERT_STR(&g_harness, label, g, e)

static unsigned int g_fake_ms;
static unsigned int fake_clock(void) { return g_fake_ms; }

static void reset_fixture(void) {
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1000, 600);
    ui_state_code_panel_mut()->panel_frac = 0.6f;
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
}

/* Bare reset leaves no occupied slot (plan §Verification: derivation
 * cases must seed explicitly). Occupy + activate one user slot the same
 * way the editor tests do. */
static void seed_user_scene(void) {
    repl_load_example(0);
    repl_promote_transient_if_needed();
}

/* Band outer rect, computed exactly as scene_tabs_rects() does. */
static void band_rect(int *cp_x, int *cp_w, int *by, int *bh) {
    int x, w, menu_by;
    ui_layout_code_panel_rect(&x, NULL, &w, NULL);
    *cp_x = x;
    *cp_w = w;
    *bh = TAB_STRIP_H;
    ui_layout_menu_bar_rect(NULL, &menu_by, NULL, NULL);
    *by = menu_by - TAB_STRIP_H;
}

static void test_derivation(void) {
    UiRenderSnapshot snap;

    /* Decision #6 / §3: bare reset (no occupied slot, neither active) ->
     * zero tabs, zero band, no synthetic tab. */
    reset_fixture();
    glr_ctrl_build_ui_snapshot(&snap);
    ASSERT_INT_EQ("bare reset -> 0 tabs", snap.scene_tabs.count, 0);
    ASSERT_INT_EQ("bare reset -> active_idx -1",
                  snap.scene_tabs.active_idx, -1);
    ASSERT_INT_EQ("bare reset -> band_h 0",
                  ui_scene_tabs_band_h(&snap), 0);

    /* One occupied + active user slot, no example: 1 active user tab. */
    reset_fixture();
    seed_user_scene();
    glr_ctrl_build_ui_snapshot(&snap);
    ASSERT_TRUE("seeded has >=1 tab", snap.scene_tabs.count >= 1);
    ASSERT_TRUE("seeded active tab valid",
                snap.scene_tabs.active_idx >= 0 &&
                snap.scene_tabs.active_idx < snap.scene_tabs.count);
    ASSERT_TRUE("seeded active tab is a user tab",
                snap.scene_tabs.tabs[snap.scene_tabs.active_idx].kind ==
                UI_SCENE_TAB_USER);
    {
        int has_example = 0;
        for (int i = 0; i < snap.scene_tabs.count; i++)
            if (snap.scene_tabs.tabs[i].kind == UI_SCENE_TAB_EXAMPLE)
                has_example = 1;
        ASSERT_TRUE("seeded has no example tab", !has_example);
    }

    /* Load an example while a user slot is occupied -> an example tab
     * appears and is the active tab. */
    repl_load_example(0);
    glr_ctrl_build_ui_snapshot(&snap);
    ASSERT_TRUE("example loaded -> >=2 tabs", snap.scene_tabs.count >= 2);
    ASSERT_TRUE("example tab is active",
                snap.scene_tabs.active_idx >= 0 &&
                snap.scene_tabs.tabs[snap.scene_tabs.active_idx].kind ==
                UI_SCENE_TAB_EXAMPLE);
    {
        int has_user = 0;
        for (int i = 0; i < snap.scene_tabs.count; i++)
            if (snap.scene_tabs.tabs[i].kind == UI_SCENE_TAB_USER)
                has_user = 1;
        ASSERT_TRUE("user tab still listed under active example", has_user);
    }
}

static void test_geometry_hit(void) {
    UiRenderSnapshot snap;
    int cp_x, cp_w, by, bh;
    int win_h;
    UiHit h;

    reset_fixture();
    seed_user_scene();
    glr_ctrl_build_ui_snapshot(&snap);
    win_h = snap.viewport.window_h;

    ASSERT_INT_EQ("band_h is TAB_STRIP_H with tabs",
                  ui_scene_tabs_band_h(&snap), TAB_STRIP_H);

    band_rect(&cp_x, &cp_w, &by, &bh);

    /* The band now leads with the workspace chip, so the left margin is the
     * chip and tab 0 sits to its right. Scan rightward from the chip's end
     * for tab 0 rather than recomputing the private chip metrics - the
     * contract under test is the breadcrumb ORDER, not the exact gap. */
    {
        int chip_x = 0, chip_w = 0;
        int found_tab_x = -1;
        int mx;

        ASSERT_TRUE("chip is shown alongside tabs",
                    ui_scene_tabs_chip_rect(&snap, &chip_x, NULL, &chip_w,
                                            NULL));
        h = ui_scene_tabs_hit_test(&snap, chip_x + 2,
                                   win_h - (by + bh / 2));
        ASSERT_TRUE("left-margin click -> WORKSPACE_CHIP",
                    h.kind == UI_HIT_CODE_PANEL_WORKSPACE_CHIP);
        ASSERT_INT_EQ("chip starts at the band's left margin", chip_x,
                      cp_x + CODE_MARGIN_X);

        for (mx = chip_x + chip_w; mx < cp_x + cp_w; mx++) {
            h = ui_scene_tabs_hit_test(&snap, mx, win_h - (by + bh / 2));
            if (h.kind == UI_HIT_CODE_PANEL_TAB) {
                found_tab_x = mx;
                break;
            }
        }
        ASSERT_TRUE("tab 0 is reachable right of the chip", found_tab_x >= 0);
        ASSERT_INT_EQ("first-tab item_idx", h.item_idx, 0);
    }

    /* Far right of a wide panel: in-band but off every (short) tab. */
    h = ui_scene_tabs_hit_test(&snap, cp_x + cp_w - 2,
                               win_h - (by + bh / 2));
    ASSERT_TRUE("in-band off-tab -> CHROME (inert)",
                h.kind == UI_HIT_CODE_PANEL_CHROME);

    /* Just below the band (into the code rows) -> not ours. */
    h = ui_scene_tabs_hit_test(&snap, cp_x + CODE_MARGIN_X + 2,
                               win_h - (by - 4));
    ASSERT_TRUE("below band -> NONE", h.kind == UI_HIT_NONE);
}

static void test_band_h_lockstep(void) {
    for (int H = 120; H <= 1200; H += 37) {
        int base = ui_text_panel_visible_lines_for_height(H, 22, 0);
        int with = ui_text_panel_visible_lines_for_height(H, 22,
                                                          TAB_STRIP_H);
        if (base <= 1)
            continue;  /* clamp floor (see plan §3) - delta not meaningful */
        ASSERT_INT_EQ("TAB_STRIP_H drops visible rows by exactly one",
                      with, base - 1);
    }
}

/* The leading workspace chip: it must render in BOTH binding states (an
 * unbound collection is exactly what it exists to make visible), name a bound
 * workspace, and lead to the Open Workspace flyout on click. */
static void test_workspace_chip(void) {
    char temp_dir[] = "/tmp/test_chip_workspace.XXXXXX";
    char *dir;
    WorkspaceManifest manifest;
    UiRenderSnapshot snap;
    UiMenuBarRuntimeSnapshot menu;
    UiHit h = ui_hit_none();
    int chip_w = 0;

    reset_fixture();
    seed_user_scene();
    repl_set_workspace_dir(NULL);
    glr_workspaces_refresh();
    glr_ctrl_build_ui_snapshot(&snap);
    ASSERT_STR("chip name is empty while unbound", snap.workspace.name, "");
    ASSERT_TRUE("chip still renders while unbound",
                ui_scene_tabs_chip_rect(&snap, NULL, NULL, &chip_w, NULL));
    ASSERT_TRUE("unbound chip has real width", chip_w > 0);

    dir = mkdtemp(temp_dir);
    ASSERT_TRUE("chip test directory created", dir != NULL);
    if (!dir) return;
    memset(&manifest, 0, sizeof(manifest));
    manifest.version = 1;
    snprintf(manifest.name, sizeof(manifest.name), "Chip Test");
    ASSERT_TRUE("chip test manifest written",
                workspace_io_manifest_write(dir, &manifest, NULL, 0));
    repl_set_workspace_dir(dir);
    glr_workspaces_refresh();
    glr_ctrl_build_ui_snapshot(&snap);
    ASSERT_STR("chip names the bound workspace", snap.workspace.name,
               "Chip Test");

    /* Click leads to the workspace list rather than switching anything.
     * Web build exempt: menu_bar.c's menu_visible() hides MENU_FILE under
     * __EMSCRIPTEN__ (the shell supplies its own New/Open chrome), so there is
     * no File menu for the chip to open and no flyout to expand. */
#if !defined(__EMSCRIPTEN__)
    ui_menu_bar_close();
    h.kind = UI_HIT_CODE_PANEL_WORKSPACE_CHIP;
    glr_ctrl_router_handle_code_panel_hit(h, 0, 0);
    ui_menu_bar_runtime_capture(&menu);
    ASSERT_INT_EQ("chip click opens the File menu", menu.open_menu,
                  GLR_MENU_FILE);
    ASSERT_INT_EQ("chip click expands the Open Workspace flyout",
                  menu.submenu_parent_row, GLR_FILE_ITEM_OPEN_WORKSPACE);
    ASSERT_INT_EQ("flyout belongs to the File menu", menu.submenu_menu_id,
                  GLR_MENU_FILE);
    ASSERT_INT_EQ("flyout parent is hovered so motion cannot tear it down",
                  menu.item_hover, GLR_FILE_ITEM_OPEN_WORKSPACE);

    /* Second click on the chip toggles the same flyout closed. */
    glr_ctrl_router_handle_code_panel_hit(h, 0, 0);
    ui_menu_bar_runtime_capture(&menu);
    ASSERT_INT_EQ("second chip click closes the menu", menu.open_menu, -1);
#endif

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

static void test_double_click_rename(void) {
    UiRenderSnapshot snap;
    UiHit h = ui_hit_none();

    /* Active user tab: single click does not rename; a second
     * click on the same tab within the window opens inline rename. */
    reset_fixture();
    seed_user_scene();
    glr_ctrl_build_ui_snapshot(&snap);
    glr_ctrl_router_set_double_click_clock_for_test(fake_clock);
    g_fake_ms = 5000;

    h.kind = UI_HIT_CODE_PANEL_TAB;
    h.item_idx = snap.scene_tabs.active_idx;  /* the active user tab */

    glr_ctrl_router_handle_code_panel_hit(h, 0, 0);
    ASSERT_TRUE("single click does not rename",
                !editor_inline_rename_active());

    g_fake_ms += 100;  /* well within DOUBLE_CLICK_MS */
    glr_ctrl_router_handle_code_panel_hit(h, 0, 0);
    ASSERT_TRUE("double click on user tab opens rename",
                editor_inline_rename_active());

    /* Example tab: double-click switches/consumes but never renames.
     * (glr_ctrl_reset_all does not cancel an active inline rename, so
     * clear the prior subtest's rename explicitly.) */
    editor_inline_rename_cancel();
    reset_fixture();
    seed_user_scene();
    repl_load_example(0);
    ASSERT_TRUE("rename inactive before example double-click",
                !editor_inline_rename_active());
    glr_ctrl_build_ui_snapshot(&snap);
    glr_ctrl_router_set_double_click_clock_for_test(fake_clock);
    g_fake_ms = 9000;
    h.kind = UI_HIT_CODE_PANEL_TAB;
    h.item_idx = snap.scene_tabs.active_idx;  /* the active example tab */
    glr_ctrl_router_handle_code_panel_hit(h, 0, 0);
    g_fake_ms += 100;
    glr_ctrl_router_handle_code_panel_hit(h, 0, 0);
    ASSERT_TRUE("double click on example tab does not rename",
                !editor_inline_rename_active());

    glr_ctrl_router_set_double_click_clock_for_test(NULL);
}

/* Regression: an open Scene/Config dropdown overlaps the tab band. A
 * click on a non-actionable dropdown row (### header / --- separator)
 * must be consumed by the menu layer (CHROME), NOT fall through to a
 * scene tab underneath. ui_menu_bar_dropdown_item_hit() returns -1 for
 * such rows, so without the full-rect consume the click switched scenes
 * beneath the dropdown (overlay-precedence violation). */
static void test_dropdown_over_band_consumes(void) {
    UiRenderSnapshot snap;
    int cp_x, cp_w;
    int win_h, menu_by, tab_by, menu_my, my_click, cfg_mx;

    reset_fixture();
    seed_user_scene();
    repl_load_example(0);
    glr_ctrl_build_ui_snapshot(&snap);

    win_h = snap.viewport.window_h;
    ui_layout_code_panel_rect(&cp_x, NULL, &cp_w, NULL);
    ui_layout_menu_bar_rect(NULL, &menu_by, NULL, NULL);
    tab_by  = menu_by - TAB_STRIP_H;
    menu_my = win_h - (menu_by + LINE_H / 2);

    /* Locate the Config menu button column (its dropdown is left-aligned
     * there and overlaps the tab band's top row). */
    cfg_mx = -1;
    for (int x = cp_x + CODE_MARGIN_X; x < cp_x + cp_w; x++) {
        UiHit hit = ui_menu_bar_hit_test(x, menu_my);
        if (hit.kind == UI_HIT_MENU_BUTTON && hit.cmd_idx == GLR_MENU_CONFIG) {
            cfg_mx = x + 2;
            break;
        }
    }
    ASSERT_TRUE("found Config menu button", cfg_mx >= 0);

    my_click = win_h - (tab_by + TAB_STRIP_H / 2);  /* dropdown row 0 */

    /* Positive control: dropdown closed -> the band point is reachable
     * as a normal tab-strip hit (we did not over-suppress). */
    ui_menu_bar_close();
    {
        UiHit ok = ui_panels_hit_test(&snap, cfg_mx, my_click, 0);
        ASSERT_TRUE("closed dropdown: band still reachable",
                    ok.kind == UI_HIT_CODE_PANEL_TAB ||
                    ok.kind == UI_HIT_CODE_PANEL_CHROME);
    }

    /* Config is now a section/flyout menu (plan Step 4): row 0 is the
     * "RENDERING" section *parent* row, not a literal "### " header.
     * A click on it is classified as UI_HIT_MENU_ITEM and consumed by
     * the menu layer (Config's activate keeps the dropdown open), so it
     * still does NOT fall through to a scene tab underneath - the
     * overlay-precedence invariant this regression guards. The
     * general "section rows are inert" guarantee + dedicated coverage
     * lands in Step 5. */
    ui_menu_bar_set_open_menu(GLR_MENU_CONFIG, 0.0f);
    ASSERT_TRUE("Config dropdown open",
                ui_menu_bar_menu_dropdown_is_open());

    {
        UiHit mh = ui_menu_bar_hit_test(cfg_mx, my_click);
        ASSERT_TRUE("menu hit-test claims the section row",
                    mh.kind == UI_HIT_MENU_ITEM);
    }
    {
        int active_before = repl_active_user_scene();
        UiHit hit = ui_panels_hit_test(&snap, cfg_mx, my_click, 0);
        int consumed;

        ASSERT_TRUE("dropdown over band -> MENU_ITEM, not TAB",
                    hit.kind == UI_HIT_MENU_ITEM);

        consumed = glr_ctrl_router_handle_code_panel_hit(hit, cfg_mx,
                                                         my_click);
        ASSERT_TRUE("routed click consumed", consumed != 0);
        ASSERT_INT_EQ("active scene unchanged underneath",
                      repl_active_user_scene(), active_before);
        ASSERT_TRUE("no rename triggered underneath",
                    !editor_inline_rename_active());
    }
}

/* The inline rename prompt owns its own display state (snapshot rename
 * view), independent of the shared transient status line: it must not
 * auto-hide, and a competing repl_set_status() must not overwrite it. */
static void test_rename_prompt_owns_its_state(void) {
    UiRenderSnapshot snap;
    char prompt_before[UI_SCENE_TAB_NAME_MAX];

    reset_fixture();
    seed_user_scene();
    editor_inline_rename_cancel();

    ASSERT_TRUE("rename begins", editor_inline_rename_begin(0) != 0);
    glr_ctrl_build_ui_snapshot(&snap);
    ASSERT_TRUE("snapshot reports rename active", snap.rename_active);
    ASSERT_TRUE("rename text is the seeded name",
                snap.rename_text[0] != '\0');
    snprintf(prompt_before, sizeof(prompt_before), "%s", snap.rename_text);

    /* A competing status message (e.g. an audio track change) mid-rename
     * must NOT clobber the rename prompt - the reported bug. */
    repl_set_status("Now playing: some track");
    glr_ctrl_build_ui_snapshot(&snap);
    ASSERT_TRUE("rename still active after competing status",
                snap.rename_active);
    ASSERT_STR("rename prompt not overwritten by status",
               snap.rename_text, prompt_before);

    /* Not TTL-driven: survives far more than 240 frames. */
    for (int i = 0; i < 1000; i++)
        glr_ctrl_tick();
    glr_ctrl_build_ui_snapshot(&snap);
    ASSERT_TRUE("prompt persists across many ticks", snap.rename_active);
    ASSERT_STR("prompt text intact across ticks",
               snap.rename_text, prompt_before);

    editor_inline_rename_cancel();
    glr_ctrl_build_ui_snapshot(&snap);
    ASSERT_TRUE("rename view clears on cancel", !snap.rename_active);
    ASSERT_INT_EQ("rename text empty on cancel",
                  (int)snap.rename_text[0], 0);
}

/* Inline rename is a hard modal for keys but not the mouse; clicking
 * anywhere in the code panel must cancel the in-progress rename (Esc-
 * equivalent) so keystrokes don't keep feeding a hidden rename buffer. */
static void test_click_away_cancels_rename(void) {
    UiHit click = ui_hit_none();

    reset_fixture();
    seed_user_scene();
    editor_inline_rename_cancel();

    ASSERT_TRUE("rename begins", editor_inline_rename_begin(0) != 0);
    ASSERT_TRUE("rename active before click",
                editor_inline_rename_active());

    /* Any code-panel press routes through this entry point. */
    click.kind = UI_HIT_CODE_PANEL_CHROME;
    glr_ctrl_router_handle_code_panel_hit(click, 10, 10);
    ASSERT_TRUE("click away cancels rename",
                !editor_inline_rename_active());
}

int main(void) {
    printf("--- ui_scene_tabs tests ---\n");
    test_derivation();
    test_geometry_hit();
    test_band_h_lockstep();
    test_workspace_chip();
    test_double_click_rename();
    test_dropdown_over_band_consumes();
    test_rename_prompt_owns_its_state();
    test_click_away_cancels_rename();
    return test_harness_report(&g_harness, "ui_scene_tabs");
}
