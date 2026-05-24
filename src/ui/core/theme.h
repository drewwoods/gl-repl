/*
 * theme.h - Centralized UI color palette.
 *
 * Single source of truth for 2D UI chrome colors. Replaces scattered
 * glColor3f/4f literals with semantic tokens resolved against an active
 * theme row. Header-only (mirrors gl_2d.h): all UI translation units
 * already reach sibling src/ui headers, and `make check-c99`
 * syntax-checks this transitively through $(SRCS).
 *
 * Three buckets of color (see plans/active/UI-Color-Theming-Infrastructure.md):
 *   1. Theme token    -> ui_clr(UI_TOK_*) here.
 *   2. Named constant  -> a local #define / static const at the use site
 *                         (fixed, non-theme one-offs; not table slots).
 *   3. Left as-is      -> computed/data palettes that must NOT follow the
 *                         theme: color_picker.c HSV math,
 *                         repl_code_panel.c k_category_colors[] /
 *                         k_syntax_shade, profile_panel.c FPS gauge
 *                         (red must stay red), text_panel.c k_clr_*
 *                         editor sub-palette.
 *
 * Neutral chrome tokens are identical across every theme row; only the
 * three accent-derived tokens vary. Swap schemes by changing
 * g_ui_theme (the design-rework bundle defines warm/cyan/amber/violet/
 * green/mono; green is the project's selected scheme).
 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "gl_includes.h"
#include "c_compat.h"   /* STATIC_ASSERT (C99/C11 portable) */
#include "config.h"     /* UI_THEME_DEFAULT (compile-time scheme select) */

typedef float UiRgba[4];

typedef enum {
    /* Neutral chrome - theme-stable (identical in every row). */
    UI_TOK_SURFACE = 0,            /* menubar / tab strip bg     #1d1d1d */
    UI_TOK_RAISED,                 /* dropdown / overlay panel   #222222 */
    UI_TOK_SUNKEN,                 /* sunken field bg            #141414 */
    UI_TOK_BORDER,                 /* panel border               #3a3a3a */
    UI_TOK_DIVIDER,                /* separator / rule           #333333 */
    UI_TOK_MENU_LABEL_HOVER_BG,    /* top-bar label hover (gray) #2a2a2a */
    UI_TOK_MENU_LABEL_ACTIVE_BG,   /* pressed/"hot" label        #262626 */
    UI_TOK_TEXT_PRIMARY,           /* default text               #d8d8d8 */
    UI_TOK_TEXT_ON_HILITE,         /* text over hover/selection  #ffffff */
    UI_TOK_TEXT_SECTION,           /* dropdown section header     #7a8494 */
    UI_TOK_TEXT_MUTED,             /* shortcuts / hints / counts  #888888 */
    UI_TOK_TEXT_PLACEHOLDER,       /* placeholder / "search..."   #7a7a7a */
    UI_TOK_CARET,                  /* text / search caret               */
    UI_TOK_STATUS_OK,              /* semantic success            #70c070 */
    UI_TOK_STATUS_WARN,            /* semantic warning            #e0a040 */
    UI_TOK_STATUS_ERR,             /* semantic error              #c9442e */

    /* Accent-derived - varies per theme row. */
    UI_TOK_ACCENT,                 /* primary accent (green #6fb36f)     */
    UI_TOK_DROPDOWN_ITEM_HOVER_BG, /* dropdown/submenu item hover band   */
    UI_TOK_ACCENT_GLOW_BG,         /* HUD / control accent band          */

    UI_TOK_COUNT
} UiThemeToken;

typedef enum {
    UI_THEME_GREEN = 0,            /* project's selected scheme (active) */
    UI_THEME_WARM,
    UI_THEME_CYAN,
    UI_THEME_AMBER,
    UI_THEME_VIOLET,
    UI_THEME_MONO,
    UI_THEME_COUNT
} UiTheme;

/* The 16 neutral columns, shared verbatim by every theme row so a new
 * scheme only fills the 3 accent tokens below it. */
#define UI_THEME_NEUTRAL_COLUMNS \
    [UI_TOK_SURFACE]              = { 0.114f, 0.114f, 0.114f, 1.0f }, \
    [UI_TOK_RAISED]               = { 0.133f, 0.133f, 0.133f, 1.0f }, \
    [UI_TOK_SUNKEN]               = { 0.078f, 0.078f, 0.078f, 1.0f }, \
    [UI_TOK_BORDER]               = { 0.227f, 0.227f, 0.227f, 1.0f }, \
    [UI_TOK_DIVIDER]              = { 0.200f, 0.200f, 0.200f, 1.0f }, \
    [UI_TOK_MENU_LABEL_HOVER_BG]  = { 0.165f, 0.165f, 0.165f, 1.0f }, \
    [UI_TOK_MENU_LABEL_ACTIVE_BG] = { 0.149f, 0.149f, 0.149f, 1.0f }, \
    [UI_TOK_TEXT_PRIMARY]         = { 0.847f, 0.847f, 0.847f, 1.0f }, \
    [UI_TOK_TEXT_ON_HILITE]       = { 1.000f, 1.000f, 1.000f, 1.0f }, \
    [UI_TOK_TEXT_SECTION]         = { 0.478f, 0.518f, 0.580f, 1.0f }, \
    [UI_TOK_TEXT_MUTED]           = { 0.533f, 0.533f, 0.533f, 1.0f }, \
    [UI_TOK_TEXT_PLACEHOLDER]     = { 0.478f, 0.478f, 0.478f, 1.0f }, \
    [UI_TOK_CARET]                = { 0.950f, 0.800f, 0.240f, 1.0f }, \
    [UI_TOK_STATUS_OK]            = { 0.439f, 0.753f, 0.439f, 1.0f }, \
    [UI_TOK_STATUS_WARN]          = { 0.878f, 0.627f, 0.251f, 1.0f }, \
    [UI_TOK_STATUS_ERR]           = { 0.788f, 0.267f, 0.180f, 1.0f }

extern const UiRgba g_ui_theme_table[UI_THEME_COUNT][UI_TOK_COUNT];
extern int g_ui_theme;

static inline const float *ui_rgba(UiThemeToken t) {
    return g_ui_theme_table[g_ui_theme][t];
}

/* Set the current GL color from a token (alpha = token's own). */
static inline void ui_clr(UiThemeToken t) {
    const float *c = ui_rgba(t);
    glColor4f(c[0], c[1], c[2], c[3]);
}

/* Set the current GL color from a token with an explicit alpha
 * (multiplies the token alpha) - matches the dominant
 * glColor4f(r, g, b, alpha) / (r, g, b, 0.98f * alpha) call shape. */
static inline void ui_clr_a(UiThemeToken t, float a) {
    const float *c = ui_rgba(t);
    glColor4f(c[0], c[1], c[2], c[3] * a);
}

static inline void ui_theme_select(UiTheme th) { g_ui_theme = (int)th; }
static inline UiTheme ui_theme_active(void) { return (UiTheme)g_ui_theme; }

STATIC_ASSERT(UI_TOK_COUNT == 19,
              "UiThemeToken count changed - update g_ui_theme_table and test_ui_theme");
STATIC_ASSERT(UI_THEME_COUNT == 6,
              "UiTheme row count changed - update g_ui_theme_table and test_ui_theme");
STATIC_ASSERT(UI_THEME_DEFAULT >= 0 && UI_THEME_DEFAULT < UI_THEME_COUNT,
              "UI_THEME_DEFAULT (config.h) out of range for the UiTheme table");

#endif /* UI_THEME_H */
