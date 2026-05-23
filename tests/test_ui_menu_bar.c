#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "ui/app/menu_bar.h"
#include "app/glr_actions.h"
#include "app/glr_config.h"
#include "repl/core.h"
#include "repl/examples.h"
#include "repl/state_owners.h"
#include "subsystems/replay/replay.h"
#include "ui/app/state.h"
#include "ui/core/layout.h"
#include "ui/core/metrics.h"
#include "support/test_harness.h"
#include <ui/core/gl_2d.h>
#ifdef GL_STUBS
#include <GL/gl_stub_counts.h>
#endif
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT_EQ(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

static void reset_menu_bar_fixture(int window_w, int window_h) {
    glr_app_reset_all();
    ui_state_reset();
    ui_menu_bar_close();
    ui_state_viewport_set_size(window_w, window_h);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 1.0f;
}

static int menu_bar_center_my(void) {
    int cp_x, cp_y, cp_w, cp_h;
    int row_mid_y;

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    row_mid_y = cp_y + cp_h - CODE_MARGIN_Y - LINE_H + LINE_H / 2;
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
            if (ui_menu_bar_dropdown_item_hit(mx, my) == target_item) {
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

static void test_open_close_state(void) {
    reset_menu_bar_fixture(1000, 600);

    ASSERT_INT_EQ("init: no menu open", ui_menu_bar_open_menu_id(), -1);
    ASSERT_TRUE("init: dropdown not open", !ui_menu_bar_menu_dropdown_is_open());

    ui_menu_bar_set_open_menu(GLR_MENU_FILE, 0.0f);
    ASSERT_INT_EQ("open File menu", ui_menu_bar_open_menu_id(), GLR_MENU_FILE);
    ASSERT_TRUE("File dropdown open", ui_menu_bar_menu_dropdown_is_open());
    ASSERT_TRUE("example dropdown mirrors open menu", ui_menu_bar_example_dropdown_is_open());

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
    ASSERT_TRUE("hidden code panel suppresses example dropdown", !ui_menu_bar_example_dropdown_is_open());
}

static void test_top_level_hits(void) {
    int menu_mx;
    int pin_mx;
    int my;

    reset_menu_bar_fixture(1000, 600);
    menu_mx = file_menu_mx();
    pin_mx = replay_pin_mx();
    my = menu_bar_center_my();

    ASSERT_INT_EQ("hit File menu button", ui_menu_bar_menu_hit(menu_mx, my), GLR_MENU_FILE);
    ASSERT_INT_EQ("hit Replay pin", ui_menu_bar_pin_hit(pin_mx, my), UI_MENU_BAR_PIN_REPLAY);
    ASSERT_INT_EQ("menu miss below bar", ui_menu_bar_menu_hit(menu_mx, my + 120), -1);
    ASSERT_INT_EQ("pin miss in menu region", ui_menu_bar_pin_hit(menu_mx, my), -1);
}

static void test_dropdown_and_config_press(void) {
    int item_mx = -1;
    int item_my = -1;
    int tutorial_mx = -1;
    int tutorial_my = -1;
    int scene_tag_mx = -1;
    int scene_tag_my = -1;
    int cfg_row = -1;
    int cfg_mx = -1;
    int cfg_my = -1;
    int before;
    int expected;
    const GlrConfigItem *item;

    reset_menu_bar_fixture(1000, 600);

    ASSERT_TRUE("found New Scene item point",
                find_dropdown_item_point(GLR_MENU_FILE, GLR_FILE_ITEM_NEW_SCENE,
                                         &item_mx, &item_my));
    ASSERT_INT_EQ("hit New Scene item",
                  ui_menu_bar_dropdown_item_hit(item_mx, item_my),
                  GLR_FILE_ITEM_NEW_SCENE);

    ui_menu_bar_close();
    ASSERT_INT_EQ("hit when closed", ui_menu_bar_dropdown_item_hit(item_mx, item_my), -1);

    /* MENU_TUTORIALS top-level row 0 was a tutorial item before Phase B
     * (the flat dropdown); after Phase B it is the first tag row (tutorial
     * activation moved to per-tag flyouts via route_submenu_item_hit).
     * Hit-test still returns 0 either way — row index is row index — but
     * activation is now inert (mirrors the Scene tag-row guard below). */
    ASSERT_TRUE("found first tutorial tag row point",
                find_dropdown_item_point(GLR_MENU_TUTORIALS, 0,
                                         &tutorial_mx, &tutorial_my));
    ASSERT_INT_EQ("hit first tutorial tag row",
                  ui_menu_bar_dropdown_item_hit(tutorial_mx, tutorial_my),
                  0);
    ASSERT_INT_EQ("Tutorials tag row activation keeps menu open",
                  glr_action_menu_item_activate(GLR_MENU_TUTORIALS, 0), 0);

    ASSERT_TRUE("found first Scene tag row point",
                find_dropdown_item_point(GLR_MENU_SCENE, 1,
                                         &scene_tag_mx, &scene_tag_my));
    ASSERT_INT_EQ("hit first Scene tag row",
                  ui_menu_bar_dropdown_item_hit(scene_tag_mx, scene_tag_my),
                  1);
    ASSERT_INT_EQ("Scene tag row activation keeps menu open",
                  glr_action_menu_item_activate(GLR_MENU_SCENE, 1), 0);

    /* Config is now a section/flyout menu (plan Step 4). Top-level
     * rows are section parents, so right-press over the section list
     * is a deliberate no-op; backward-cycle moves into the open flyout
     * in Step 7 (covered by test_config_submenu_right_press there).
     * Positive section/flyout coverage is added in Step 9. */
    ui_menu_bar_set_open_menu(GLR_MENU_CONFIG, 0.0f);
    ASSERT_INT_EQ("config right-press over section list is a no-op",
                  ui_menu_bar_handle_config_right_press(cfg_mx, cfg_my), 0);
    ui_menu_bar_close();
    ASSERT_INT_EQ("right-press when closed",
                  ui_menu_bar_handle_config_right_press(cfg_mx, cfg_my), 0);
    (void)cfg_row; (void)item; (void)before; (void)expected;
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
    const char *lbl = glr_config_item_at(row)->label;
    ASSERT_TRUE("label updated to MSAAx4", strcmp(lbl, "MSAAx4") == 0);

    /* Test case 2: samples = 8 -> "MSAAx8" */
    g_gl_stub_samples = 8;
    glr_ctrl_init_gl();
    lbl = glr_config_item_at(row)->label;
    ASSERT_TRUE("label updated to MSAAx8", strcmp(lbl, "MSAAx8") == 0);

    /* Test case 3: samples = 0 -> "MSAA" (fallback) */
    g_gl_stub_samples = 0;
    glr_ctrl_init_gl();
    lbl = glr_config_item_at(row)->label;
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

#ifdef GL_STUBS
static void make_test_ui_snapshot(UiRenderSnapshot *snap) {
    memset(snap, 0, sizeof(*snap));
    snap->viewport = ui_state_viewport();
    snap->anim_time = 1.0f;
    snap->user_scene_active_idx = repl_active_user_scene();
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

    reset_menu_bar_fixture(1000, 600);
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
                ui_menu_bar_scene_example_submenu_rect_for_test(tag_idx,
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
                ui_menu_bar_scene_example_submenu_rect_for_test(tag_idx,
                                                                &sx, &sy,
                                                                &sw, &sh));
    ASSERT_TRUE("narrow submenu flips left of parent point", sx < tag_mx);
    ASSERT_TRUE("narrow submenu stays onscreen", sx + sw <= ui_state_viewport().window_w);
}

/* Step 9: positive Config-flyout coverage mirroring
 * test_scene_submenu_with_stubs — open Config, hover section 0, assert
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
     * its first row — a "### " HEADER — must be inert chrome, NOT a
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
    ui_menu_bar_render_search_overlay(&snap, 0, 400, 20);
    ASSERT_TRUE("search overlay renders placeholder", gl_stub_counts[GL_STUB_glRasterPos2f] > 0);

    snprintf(snap.search.query, sizeof(snap.search.query),
             "search-term-that-needs-truncation");
    snap.search.query_len = (int)strlen(snap.search.query);
    snap.search.cursor_pos = snap.search.query_len;
    snap.search.match_count = 0;
    snap.anim_time = 2.0f;
    gl_stub_counts_reset();
    ui_menu_bar_render_search_overlay(&snap, 0, 180, 20);
    ASSERT_TRUE("search overlay renders zero matches", gl_stub_counts[GL_STUB_glRasterPos2f] > 0);

    snap.search.match_count = 9;
    snap.search.hit_ordinal = 3;
    snap.code_panel.cursor_visible = 1;
    gl_stub_counts_reset();
    ui_menu_bar_render_search_overlay(&snap, 0, 180, 20);
    ASSERT_TRUE("search overlay renders cursor and count", gl_stub_counts[GL_STUB_glRectf] > 0);
}
#endif

/* Step 7 (Finding #3): right-press over a Config flyout item cycles
 * it backward; right-press over a section parent row / closed menu is
 * a no-op (never mis-cycles a g_cfg_items[] index). */
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
    ASSERT_INT_EQ("right-press on flyout item handled",
                  ui_menu_bar_handle_config_right_press(row_mx, row_my), 1);
    ASSERT_INT_EQ("right-press cycled the item backward",
                  glr_config_get(item->key), expected);

    /* Right-press over the section parent row itself (not the flyout)
     * is a no-op and changes nothing. */
    int v = glr_config_get(item->key);
    ASSERT_INT_EQ("right-press on section parent row is a no-op",
                  ui_menu_bar_handle_config_right_press(parent_mx,
                                                        parent_my), 0);
    ASSERT_INT_EQ("section parent right-press changed nothing",
                  glr_config_get(item->key), v);

    ui_menu_bar_close();
    ASSERT_INT_EQ("right-press when Config closed is a no-op",
                  ui_menu_bar_handle_config_right_press(row_mx, row_my), 0);
}

int main(void) {
    printf("--- ui_menu_bar tests ---\n");

    test_open_close_state();
    test_top_level_hits();
    test_dropdown_and_config_press();
    test_config_submenu_right_press();
#ifdef GL_STUBS
    test_msaa_label_dynamic();
#endif
    test_unified_hit_test();
#ifdef GL_STUBS
    test_scene_submenu_with_stubs();
    test_config_submenu_with_stubs();
    test_render_paths_with_stubs();
#endif

    printf("\n");
    return test_harness_report(&g_harness, "test_ui_menu_bar");
}
