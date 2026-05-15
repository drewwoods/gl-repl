/*
 * test_ui_scene_tabs.c - scene tab strip: derivation, geometry/hit,
 * band_h lockstep, and double-click rename routing.
 *
 * Core test (runs under `make test`); mirrors the controller-seeded
 * fixture of test_repl_code_panel_document.c.
 */
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "editor/inline_rename.h"
#include "repl/core.h"
#include "repl/scenes.h"
#include "ui/hit.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "ui/scene_tabs.h"
#include "ui/snapshot.h"
#include "ui/state.h"
#include "ui/text_panel.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)   TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT_EQ(label, g, e) TEST_ASSERT_INT(&g_harness, label, g, e)

static unsigned int g_fake_ms;
static unsigned int fake_clock(void) { return g_fake_ms; }

static void reset_fixture(void) {
    glr_app_reset_all();
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
    repl_promote_example_if_needed();
}

/* Band outer rect, computed exactly as scene_tabs_rects() does. */
static void band_rect(int *cp_x, int *cp_w, int *by, int *bh) {
    int x, y, w, h;
    ui_layout_code_panel_rect(&x, &y, &w, &h);
    *cp_x = x;
    *cp_w = w;
    *bh = TAB_STRIP_H;
    *by = (y + h) - CODE_MARGIN_Y - LINE_H - TAB_STRIP_H;
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

    /* Just inside the first tab (>= TAB_MIN_W from the left margin). */
    h = ui_scene_tabs_hit_test(&snap,
                               cp_x + CODE_MARGIN_X + 2,
                               win_h - (by + bh / 2));
    ASSERT_TRUE("first-tab click -> TAB", h.kind == UI_HIT_CODE_PANEL_TAB);
    ASSERT_INT_EQ("first-tab item_idx", h.item_idx, 0);

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
    int flags = UI_TEXT_PANEL_CHROME_STATUSBAR;
    for (int H = 120; H <= 1200; H += 37) {
        int base = ui_text_panel_visible_lines_for_height(H, flags, 0);
        int with = ui_text_panel_visible_lines_for_height(H, flags,
                                                          TAB_STRIP_H);
        if (base <= 1)
            continue;  /* clamp floor (see plan §3) — delta not meaningful */
        ASSERT_INT_EQ("TAB_STRIP_H drops visible rows by exactly one",
                      with, base - 1);
    }
}

static void test_double_click_rename(void) {
    UiRenderSnapshot snap;
    UiHit h = ui_hit_none();

    /* Active user (home) tab: single click does not rename; a second
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
     * (glr_app_reset_all does not cancel an active inline rename, so
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

int main(void) {
    printf("--- ui_scene_tabs tests ---\n");
    test_derivation();
    test_geometry_hit();
    test_band_h_lockstep();
    test_double_click_rename();
    return test_harness_report(&g_harness, "ui_scene_tabs");
}
