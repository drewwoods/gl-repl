#include "glr_state.h"
#include "glr_ctrl.h"
#include "ui/menu_bar.h"
#include "glr_actions.h"
#include "glr_config.h"
#include "repl/core.h"
#include "repl/state_owners.h"
#include "replay.h"
#include "ui/state.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "support/test_harness.h"
#include <gl_2d.h>
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

static int find_first_config_action_point(int *out_row, int *out_mx, int *out_my) {
    int win_w = ui_state_viewport().window_w;
    int win_h = ui_state_viewport().window_h;

    ui_menu_bar_set_open_menu(GLR_MENU_CONFIG, 0.0f);
    for (int my = 0; my < win_h; my++) {
        for (int mx = 0; mx < win_w; mx++) {
            int row = ui_menu_bar_dropdown_item_hit(mx, my);
            const GlrConfigItem *item;

            if (row < 0)
                continue;
            item = glr_config_item_at(row);
            if (!item || item->section_header || item->key == GLR_CONFIG_NONE)
                continue;

            if (out_row)
                *out_row = row;
            if (out_mx)
                *out_mx = mx;
            if (out_my)
                *out_my = my;
            return 1;
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
    ASSERT_INT_EQ("hit Replay pin", ui_menu_bar_pin_hit(pin_mx, my), REPL_MENU_BAR_PIN_REPLAY);
    ASSERT_INT_EQ("menu miss below bar", ui_menu_bar_menu_hit(menu_mx, my + 120), -1);
    ASSERT_INT_EQ("pin miss in menu region", ui_menu_bar_pin_hit(menu_mx, my), -1);
}

static void test_dropdown_and_config_press(void) {
    int item_mx = -1;
    int item_my = -1;
    int cfg_row = -1;
    int cfg_mx = -1;
    int cfg_my = -1;
    int before;
    int expected;
    const GlrConfigItem *item;

    reset_menu_bar_fixture(1000, 600);

    ASSERT_TRUE("found Export item point",
                find_dropdown_item_point(GLR_MENU_FILE, REPL_FILE_ITEM_EXPORT,
                                         &item_mx, &item_my));
    ASSERT_INT_EQ("hit Export item",
                  ui_menu_bar_dropdown_item_hit(item_mx, item_my),
                  REPL_FILE_ITEM_EXPORT);

    ui_menu_bar_close();
    ASSERT_INT_EQ("hit when closed", ui_menu_bar_dropdown_item_hit(item_mx, item_my), -1);

    ASSERT_TRUE("found config action point",
                find_first_config_action_point(&cfg_row, &cfg_mx, &cfg_my));
    item = glr_config_item_at(cfg_row);
    before = glr_config_get(item->key);
    expected = before - 1;
    if (expected < 0)
        expected = item->state_count - 1;

    ASSERT_INT_EQ("handled config right-press",
                  ui_menu_bar_handle_config_right_press(cfg_mx, cfg_my), 1);
    ASSERT_INT_EQ("config right-press cycles backward",
                  glr_config_get(item->key), expected);
    ASSERT_INT_EQ("right-press miss while open", ui_menu_bar_handle_config_right_press(0, 0), 0);

    ui_menu_bar_close();
    ASSERT_INT_EQ("right-press when closed", ui_menu_bar_handle_config_right_press(cfg_mx, cfg_my), 0);
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
                find_dropdown_item_point(GLR_MENU_FILE, REPL_FILE_ITEM_EXPORT,
                                         &item_mx, &item_my));
    h = ui_menu_bar_hit_test(item_mx, item_my);
    ASSERT_INT_EQ("hit_test: dropdown item kind", h.kind, UI_HIT_MENU_ITEM);
    ASSERT_INT_EQ("hit_test: dropdown item menu_id", h.cmd_idx, GLR_MENU_FILE);
    ASSERT_INT_EQ("hit_test: dropdown item idx", h.item_idx, REPL_FILE_ITEM_EXPORT);

    ui_menu_bar_close();
    h = ui_menu_bar_hit_test(pin_mx, my);
    ASSERT_INT_EQ("hit_test: pin kind", h.kind, UI_HIT_PIN_BUTTON);
    ASSERT_INT_EQ("hit_test: pin id", h.item_idx, REPL_MENU_BAR_PIN_REPLAY);
}

#ifdef GL_STUBS
static void make_test_ui_snapshot(UiRenderSnapshot *snap) {
    memset(snap, 0, sizeof(*snap));
    snap->viewport = ui_state_viewport();
    snap->anim_time = 1.0f;
    snap->user_scene_active_idx = repl_active_user_scene();
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
                find_dropdown_item_point(GLR_MENU_FILE, REPL_FILE_ITEM_EXPORT,
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

int main(void) {
    printf("--- ui_menu_bar tests ---\n");

    test_open_close_state();
    test_top_level_hits();
    test_dropdown_and_config_press();
    test_unified_hit_test();
#ifdef GL_STUBS
    test_render_paths_with_stubs();
#endif

    printf("\n");
    return test_harness_report(&g_harness, "test_ui_menu_bar");
}
