/*
 * test_ui_theme.c - Palette integrity + Issue-1 regression for ui/theme.h.
 *
 * Header-only target: links no project objects (mirrors
 * test_repl_code_panel_layout). The STATIC_ASSERTs in theme.h already
 * guard the enum/row counts at compile time; this exercises the data.
 */
#include "ui/theme.h"
#include "support/test_harness.h"

#include <stdio.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)  TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_F(label, got, exp) TEST_ASSERT_FLOAT_DEFAULT(&g_harness, label, got, exp)

/* The old blue that DROPDOWN_ITEM_HOVER_BG replaced (Issue 1). */
static const float k_old_blue[3] = { 0.180f, 0.290f, 0.431f };

int main(void) {
    /* 1. Every token in every row is initialized. A designated-init gap
     * leaves the slot zero-filled; every real token carries alpha 1.0,
     * so alpha == 0 reliably flags a missing entry. */
    for (int th = 0; th < UI_THEME_COUNT; th++)
        for (int t = 0; t < UI_TOK_COUNT; t++)
            ASSERT_TRUE("no zeroed/uninitialized token slot",
                        g_ui_theme_table[th][t][3] > 0.0f);

    /* 2. Neutral chrome tokens are identical across every theme row
     * (UI_TOK_SURFACE .. UI_TOK_STATUS_ERR is the theme-stable band). */
    for (int t = UI_TOK_SURFACE; t <= UI_TOK_STATUS_ERR; t++)
        for (int th = 1; th < UI_THEME_COUNT; th++)
            for (int k = 0; k < 4; k++)
                ASSERT_F("neutral token stable across themes",
                         g_ui_theme_table[th][t][k],
                         g_ui_theme_table[UI_THEME_GREEN][t][k]);

    /* 3. Back-compat: GREEN accent == #6fb36f (the retired
     * UI_ACCENT_GREEN_* values), so accent migration is a visual no-op. */
    {
        const float *a = g_ui_theme_table[UI_THEME_GREEN][UI_TOK_ACCENT];
        ASSERT_F("green accent R == 0.435", a[0], 0.435f);
        ASSERT_F("green accent G == 0.702", a[1], 0.702f);
        ASSERT_F("green accent B == 0.435", a[2], 0.435f);
    }

    /* 4. Issue-1 regression: the dropdown/submenu hover is no longer the
     * old blue and is unmistakably green (green channel dominates). */
    {
        const float *h = g_ui_theme_table[UI_THEME_GREEN][UI_TOK_DROPDOWN_ITEM_HOVER_BG];
        int is_old_blue = (h[0] == k_old_blue[0] &&
                           h[1] == k_old_blue[1] &&
                           h[2] == k_old_blue[2]);
        ASSERT_TRUE("dropdown hover is not the old #2e4a6e blue", !is_old_blue);
        ASSERT_TRUE("dropdown hover is green-dominant (g>r && g>b)",
                    h[1] > h[0] && h[1] > h[2]);
    }

    /* 5. Accent rows are actually distinct (table not all-green). */
    ASSERT_TRUE("WARM accent differs from GREEN accent",
                g_ui_theme_table[UI_THEME_WARM][UI_TOK_ACCENT][0] !=
                g_ui_theme_table[UI_THEME_GREEN][UI_TOK_ACCENT][0]);

    /* 6. ui_theme_select / ui_theme_active round-trip, and ui_rgba
     * reflects the active row. */
    {
        /* Tracks UI_THEME_DEFAULT (config.h, build-overridable) rather
         * than hardcoding GREEN, so a -DUI_THEME_DEFAULT=N build does
         * not trip this. The GREEN-specific table assertions above are
         * intentionally fixed (they lock the GREEN row's values). */
        ASSERT_TRUE("default active theme tracks UI_THEME_DEFAULT",
                    ui_theme_active() == (UiTheme)UI_THEME_DEFAULT);
        ui_theme_select(UI_THEME_WARM);
        ASSERT_TRUE("active theme is WARM after select",
                    ui_theme_active() == UI_THEME_WARM);
        ASSERT_F("ui_rgba follows active theme",
                 ui_rgba(UI_TOK_ACCENT)[0],
                 g_ui_theme_table[UI_THEME_WARM][UI_TOK_ACCENT][0]);
        ui_theme_select(UI_THEME_GREEN);
        ASSERT_TRUE("active theme restored to GREEN",
                    ui_theme_active() == UI_THEME_GREEN);
    }

    return test_harness_report(&g_harness, "test_ui_theme");
}
