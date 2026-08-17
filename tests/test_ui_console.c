/*
 * test_ui_console.c - Unit tests for Console UI overlay panel.
 */
#ifdef GL_STUBS
#include "ui/support/console.h"
#include "ui/core/gl_2d.h"
#include "support/test_harness.h"
#include <GL/gl_stub_counts.h>
#endif

#include <stdio.h>
#include <string.h>

#ifdef GL_STUBS
static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)    TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, exp) TEST_ASSERT_INT(&g_harness, label, got, exp)
#define ASSERT_STR(label, got, exp) TEST_ASSERT_STR(&g_harness, label, got, exp)

enum { WIN_W = 1000, WIN_H = 800 };

static void test_panel_size(void) {
    int w = 0, h = 0;
    ui_console_panel_size(0, &w, &h);
    ASSERT_INT("w for 0 lines", w, UI_CONSOLE_PANEL_W);
    ASSERT_INT("h for 0 lines (min 1 line)", h, UI_CONSOLE_HEADER_H + UI_CONSOLE_PAD * 2 + UI_CONSOLE_LINE_H);

    ui_console_panel_size(5, &w, &h);
    ASSERT_INT("h for 5 lines", h, UI_CONSOLE_HEADER_H + UI_CONSOLE_PAD * 2 + 5 * UI_CONSOLE_LINE_H);

    int max_h = 0;
    ui_console_panel_size(100, &max_h, &max_h);
    ASSERT_INT("h capped at 16 lines", max_h, UI_CONSOLE_HEADER_H + UI_CONSOLE_PAD * 2 + 16 * UI_CONSOLE_LINE_H);
}

static void test_panel_hit_test(void) {
    ConsoleLine lines[2];
    snprintf(lines[0].text, sizeof(lines[0].text), "line 1");
    snprintf(lines[1].text, sizeof(lines[1].text), "line 2");

    ConsoleView cview;
    cview.open = 1;
    cview.count = 2;
    cview.total_count = 2;
    cview.overflow_count = 0;
    cview.lines = lines;

    UiConsolePanelView view;
    memset(&view, 0, sizeof(view));
    view.window_w = WIN_W;
    view.window_h = WIN_H;
    view.visible = 1;
    view.panel_x = 600;
    view.panel_y = 100;
    view.console = cview;

    int panel_w, panel_h;
    ui_console_panel_size(2, &panel_w, &panel_h);

    /* Point outside: (100, 100) */
    ASSERT_INT("outside is NONE", ui_console_panel_hit_test(&view, 100, 100), UI_CONSOLE_HIT_NONE);

    /* Point inside panel body: GLUT coords:
     * panel_x = 600..600+panel_w
     * panel_y = 100 in GL (bottom-up) -> GLUT y in [WIN_H - 100 - panel_h, WIN_H - 100] = [700 - panel_h, 700] */
    int inside_x = 650;
    int inside_y = WIN_H - 100 - panel_h / 2;
    ASSERT_INT("inside panel body is PANEL", ui_console_panel_hit_test(&view, inside_x, inside_y), UI_CONSOLE_HIT_PANEL);

    /* Close button [x] is near header right edge:
     * Header GLUT y in [WIN_H - (panel_y + panel_h), WIN_H - (panel_y + panel_h) + UI_CONSOLE_HEADER_H]
     * Header right edge x ~ 600 + panel_w - UI_CONSOLE_PAD */
    int close_y = WIN_H - (100 + panel_h) + 5;
    int close_x = 600 + panel_w - UI_CONSOLE_PAD - 5;
    ASSERT_INT("close button is CLOSE", ui_console_panel_hit_test(&view, close_x, close_y), UI_CONSOLE_HIT_CLOSE);

    /* Invisible view returns NONE */
    view.visible = 0;
    ASSERT_INT("invisible view is NONE", ui_console_panel_hit_test(&view, inside_x, inside_y), UI_CONSOLE_HIT_NONE);
}

static void test_panel_render(void) {
    ConsoleLine lines[3];
    snprintf(lines[0].text, sizeof(lines[0].text), "Hello world");
    snprintf(lines[1].text, sizeof(lines[1].text), "  depth 1 message");
    snprintf(lines[2].text, sizeof(lines[2].text), "    result = 123.45");

    ConsoleView cview;
    cview.open = 1;
    cview.count = 3;
    cview.total_count = 3;
    cview.overflow_count = 0;
    cview.lines = lines;

    UiConsolePanelView view;
    memset(&view, 0, sizeof(view));
    view.window_w = WIN_W;
    view.window_h = WIN_H;
    view.visible = 1;
    view.panel_x = 600;
    view.panel_y = 100;
    view.console = cview;

    /* Render should run cleanly with GL stubs */
    ui_console_panel_render(&view);
    ASSERT_TRUE("render with data succeeded", 1);

    /* Empty state */
    cview.count = 0;
    view.console = cview;
    ui_console_panel_render(&view);
    ASSERT_TRUE("render empty state succeeded", 1);

    /* Overflow state: the panel walks min(count, 16) entries of lines[]. */
    {
        ConsoleLine overflow_lines[16];
        memset(overflow_lines, 0, sizeof(overflow_lines));
        for (int i = 0; i < 16; i++) {
            snprintf(overflow_lines[i].text, sizeof(overflow_lines[i].text),
                     "overflow line %d", i);
        }
        cview.count = 16;
        cview.total_count = 41;
        cview.overflow_count = 25;
        cview.lines = overflow_lines;
        view.console = cview;
        ui_console_panel_render(&view);
        ASSERT_TRUE("render overflow state succeeded", 1);
    }
}

int main(void) {
    test_panel_size();
    test_panel_hit_test();
    test_panel_render();

    return test_harness_report(&g_harness, "test_ui_console");
}
#else
int main(void) {
    return 0;
}
#endif
