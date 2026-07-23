#include "ui/core/theme.h"

const UiRgba g_ui_theme_table[UI_THEME_COUNT][UI_TOK_COUNT] = {
    /* GREEN - the project's selected scheme. DROPDOWN_ITEM_HOVER_BG is
     * the hue-shifted twin of the old #2e4a6e blue (Issue 1 fix):
     * same luminance/saturation, hue rotated blue -> green. */
    [UI_THEME_GREEN] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.435f, 0.702f, 0.435f, 1.0f }, /* #6fb36fff */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.180f, 0.431f, 0.290f, 1.0f }, /* #2e6e4aff */
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.188f, 0.298f, 0.220f, 1.0f }, /* #304c38ff */
        [UI_TOK_ACCENT_ALT]             = { 0.950f, 0.780f, 0.300f, 1.0f }, /* warm amber #f2c74dff */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.550f, 0.450f, 0.200f, 1.0f }, /* dim amber  #8c7333ff */
    },
    /* The remaining rows reuse the same neutral chrome; only the accent,
     * a dark accent-tinted hover band, and the HUD glow band differ
     * (--accent-h values from docs/plans/done/design-rework). */
    [UI_THEME_WARM] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.851f, 0.424f, 0.310f, 1.0f }, /* #d96c4fff */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.431f, 0.212f, 0.155f, 1.0f }, /* #6e3628ff */
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.302f, 0.157f, 0.118f, 1.0f }, /* #4d281eff */
        [UI_TOK_ACCENT_ALT]             = { 0.400f, 0.750f, 0.850f, 1.0f }, /* cool sky   #66bfd9ff */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.240f, 0.420f, 0.500f, 1.0f }, /* dim sky    #3d6b80ff */
    },
    [UI_THEME_CYAN] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.353f, 0.690f, 0.761f, 1.0f }, /* #5ab0c2ff */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.180f, 0.345f, 0.380f, 1.0f }, /* #2e5861ff */
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.129f, 0.255f, 0.278f, 1.0f }, /* #214147ff */
        [UI_TOK_ACCENT_ALT]             = { 0.920f, 0.700f, 0.350f, 1.0f }, /* warm gold  #ebb359ff */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.520f, 0.400f, 0.200f, 1.0f }, /* dim gold   #856633ff */
    },
    [UI_THEME_AMBER] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.878f, 0.631f, 0.227f, 1.0f }, /* #e0a13aff */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.439f, 0.316f, 0.114f, 1.0f }, /* #70511dff */
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.310f, 0.227f, 0.090f, 1.0f }, /* #4f3a17ff */
        [UI_TOK_ACCENT_ALT]             = { 0.500f, 0.700f, 0.900f, 1.0f }, /* soft azure #80b3e6ff */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.300f, 0.400f, 0.520f, 1.0f }, /* dim azure  #4d6685ff */
    },
    [UI_THEME_VIOLET] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.663f, 0.518f, 0.831f, 1.0f }, /* #a984d4ff */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.331f, 0.259f, 0.416f, 1.0f }, /* #54426aff */
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.239f, 0.188f, 0.302f, 1.0f }, /* #3d304dff */
        [UI_TOK_ACCENT_ALT]             = { 0.850f, 0.750f, 0.350f, 1.0f }, /* warm gold  #d9bf59ff */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.480f, 0.420f, 0.200f, 1.0f }, /* dim gold   #7a6b33ff */
    },
    [UI_THEME_MONO] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.800f, 0.800f, 0.800f, 1.0f }, /* #ccccccff */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.290f, 0.290f, 0.290f, 1.0f }, /* #4a4a4aff */
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.220f, 0.220f, 0.220f, 1.0f }, /* #383838ff */
        [UI_TOK_ACCENT_ALT]             = { 0.700f, 0.700f, 0.700f, 1.0f }, /* lighter    #b3b3b3ff */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.400f, 0.400f, 0.400f, 1.0f }, /* dim gray   #666666ff */
    },
    /* NEON - lifted straight from examples/scenes/glr-logo.glr's cube
     * interior faces: cyan glColor3f(0.25, 0.675, 1) on +Z/-Z and magenta
     * glColor3f(0.95, 0.25, 1) on +Y/-Y. Hover/glow bands follow the
     * same 0.5x / 0.36x scale-down used by the other accent-derived rows. */
    [UI_THEME_NEON] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.250f, 0.675f, 1.000f, 1.0f }, /* logo cyan    #40acffff */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.125f, 0.338f, 0.500f, 1.0f }, /* #205680ff */
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.090f, 0.243f, 0.360f, 1.0f }, /* #173e5cff */
        [UI_TOK_ACCENT_ALT]             = { 0.950f, 0.250f, 1.000f, 1.0f }, /* logo magenta #f240ffff */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.551f, 0.145f, 0.580f, 1.0f }, /* dim magenta  #8d2594ff */
    },
};

int g_ui_theme = UI_THEME_DEFAULT;
