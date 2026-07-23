#include "ui/core/theme.h"

const UiRgba g_ui_theme_table[UI_THEME_COUNT][UI_TOK_COUNT] = {
    /* GREEN - the project's selected scheme. DROPDOWN_ITEM_HOVER_BG is
     * the hue-shifted twin of the old #2e4a6e blue (Issue 1 fix):
     * same luminance/saturation, hue rotated blue -> green. */
    [UI_THEME_GREEN] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.435f, 0.702f, 0.435f, 1.0f }, /* #6fb36f */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.180f, 0.431f, 0.290f, 1.0f }, /* #2e6e4a */
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.188f, 0.298f, 0.220f, 1.0f }, /* #304c38 */
        [UI_TOK_ACCENT_ALT]             = { 0.950f, 0.780f, 0.300f, 1.0f }, /* warm amber */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.550f, 0.450f, 0.200f, 1.0f }, /* dim amber  */
    },
    /* The remaining rows reuse the same neutral chrome; only the accent,
     * a dark accent-tinted hover band, and the HUD glow band differ
     * (--accent-h values from docs/plans/done/design-rework). */
    [UI_THEME_WARM] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.851f, 0.424f, 0.310f, 1.0f }, /* #d96c4f */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.431f, 0.212f, 0.155f, 1.0f },
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.302f, 0.157f, 0.118f, 1.0f },
        [UI_TOK_ACCENT_ALT]             = { 0.400f, 0.750f, 0.850f, 1.0f }, /* cool sky   */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.240f, 0.420f, 0.500f, 1.0f }, /* dim sky    */
    },
    [UI_THEME_CYAN] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.353f, 0.690f, 0.761f, 1.0f }, /* #5ab0c2 */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.180f, 0.345f, 0.380f, 1.0f },
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.129f, 0.255f, 0.278f, 1.0f },
        [UI_TOK_ACCENT_ALT]             = { 0.920f, 0.700f, 0.350f, 1.0f }, /* warm gold  */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.520f, 0.400f, 0.200f, 1.0f }, /* dim gold   */
    },
    [UI_THEME_AMBER] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.878f, 0.631f, 0.227f, 1.0f }, /* #e0a13a */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.439f, 0.316f, 0.114f, 1.0f },
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.310f, 0.227f, 0.090f, 1.0f },
        [UI_TOK_ACCENT_ALT]             = { 0.500f, 0.700f, 0.900f, 1.0f }, /* soft azure */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.300f, 0.400f, 0.520f, 1.0f }, /* dim azure  */
    },
    [UI_THEME_VIOLET] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.663f, 0.518f, 0.831f, 1.0f }, /* #a984d4 */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.331f, 0.259f, 0.416f, 1.0f },
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.239f, 0.188f, 0.302f, 1.0f },
        [UI_TOK_ACCENT_ALT]             = { 0.850f, 0.750f, 0.350f, 1.0f }, /* warm gold  */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.480f, 0.420f, 0.200f, 1.0f }, /* dim gold   */
    },
    [UI_THEME_MONO] = {
        UI_THEME_NEUTRAL_COLUMNS,
        [UI_TOK_ACCENT]                 = { 0.800f, 0.800f, 0.800f, 1.0f }, /* #cccccc */
        [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = { 0.290f, 0.290f, 0.290f, 1.0f },
        [UI_TOK_ACCENT_GLOW_BG]         = { 0.220f, 0.220f, 0.220f, 1.0f },
        [UI_TOK_ACCENT_ALT]             = { 0.700f, 0.700f, 0.700f, 1.0f }, /* lighter    */
        [UI_TOK_ACCENT_ALT_DIM]         = { 0.400f, 0.400f, 0.400f, 1.0f }, /* dim gray   */
    },
};

int g_ui_theme = UI_THEME_DEFAULT;
