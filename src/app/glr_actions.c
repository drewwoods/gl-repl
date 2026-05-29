/*
 * glr_actions.c -- Side-effecting editor actions and config dispatch.
 *
 * Input modules decide which key or menu row was activated. UI modules decide
 * what was clicked. This module owns the mutation that follows: config-row
 * cycling, F-key/Ctrl-key config shortcuts, startup config defaults, and menu
 * item actions that touch scenes, files, replay, audio, or presentation state.
 */
#include "app/glr_actions.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"                  /* DEFAULT_SCENE_FILE */
#include "app/glr_ctrl.h"            /* glr_ctrl_sync_ui_chrome */
#include "app/glr_ctrl_export.h"
#include "app/glr_state.h"           /* presentation/render storage */
#include "app/glr_camera.h"          /* camera focus-origin / reset (eased) */
#include "ui/app/layout.h"           /* CODE_PANEL_LAYOUT_* enum values */
#include "subsystems/color_picker/color_picker_state.h"
#include "app/glr_audio.h"
#include "repl/core.h"
#include "repl/core_internal.h"
#include "repl/examples.h"
#include "repl/scenes.h"
#include "app/glr_config.h"
#include "editor/input.h"
#include "editor/completion.h"
#include "keys.h"
#include "repl/help_text.h"
#include "repl/tutorials.h"
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "editor/help_session.h"
#include "repl/pipeline.h"
#include "repl/state_owners.h"
#include "ui/app/menu_bar.h"
#include "ui/support/memprof.h"
#include "ui/support/cpuprof.h"
#include "ui/app/state.h"
#include "editor/inline_file_prompt.h"
#include "editor/inline_rename.h"
#include "editor/undo.h"
#include "scene/themes.h"
#include "scene/lights.h"           /* scene_lights_apply_theme, scene_light_theme_names */

static const char *replay_mode_names[] = { "Polygon", "Vertex" };
static const char *backdrop_mode_names[SCENE_BACKDROP_COUNT] = {
    [SCENE_BACKDROP_OFF] = "Off",
    [SCENE_BACKDROP_CITYSCAPE] = "Cityscape",
    [SCENE_BACKDROP_STARS] = "Stars",
    [SCENE_BACKDROP_CITY_AND_STARS] = "City+Stars",
};
static const char *xform_guide_mode_names[SCENE_XFORM_GUIDE_COUNT] = {
    [SCENE_XFORM_GUIDE_OFF]   = "Off",
    [SCENE_XFORM_GUIDE_WORLD] = "World",
    [SCENE_XFORM_GUIDE_FRAME] = "Frame",
};
static const char *profile_panel_mode_names[] = { "Off", "On", "Details" };
static const char *memory_panel_mode_names[]  = { "Off", "On" };
static const char *code_panel_layout_names[] = {
    "Left", "Top", "Bottom", "Hidden"
};
static const char *grid_theme_names[GRID_THEME_COUNT] = {
    [GRID_THEME_OFF]     = "OFF",
    [GRID_THEME_CLASSIC] = "Classic",
    [GRID_THEME_FOG]     = "Fog",
    [GRID_THEME_TRON]    = "Tron",
    [GRID_THEME_EMBER]   = "Ember",
    [GRID_THEME_FAINT]   = "Faint",
    [GRID_THEME_FOCUS]   = "Focus",
    [GRID_THEME_OCEAN]   = "Ocean",
    [GRID_THEME_XZRULER] = "XZ Ruler",
    [GRID_THEME_PLANES]  = "Adaptive Planes",
    [GRID_THEME_RADAR]   = "Radar",
};
static const char *grid_major_names[GRID_MAJOR_COUNT] = {
    [GRID_MAJOR_1]  = "1",
    [GRID_MAJOR_2]  = "2",
    [GRID_MAJOR_5]  = "5",
    [GRID_MAJOR_10] = "10",
};
static const char *grid_extent_names[GRID_EXTENT_COUNT] = {
    [GRID_EXTENT_CLOSE] = "Close",
    [GRID_EXTENT_MID]   = "Mid",
    [GRID_EXTENT_FAR]   = "Far",
};
static const char *axes_theme_names[AXES_THEME_COUNT] = {
    [AXES_THEME_OFF]     = "OFF",
    [AXES_THEME_CLASSIC] = "Classic",
    [AXES_THEME_PULSE]   = "Pulse",
    [AXES_THEME_NEON]    = "Neon",
    [AXES_THEME_COMPASS] = "Compass",
    [AXES_THEME_GIZMO]   = "Gizmo",
    [AXES_THEME_RULER]   = "Ruler",
};
static char cfg_status_buf[REPL_STATUS_TEXT_MAX];

/* Unified audio cfg: two-state on/off toggle.
 * Indices:
 *   0 = Off  - paused
 *   1 = On   - playing, loop mode ALL (playlist, wrap forever)
 * Old 4-state ini values (1/2/3) are all remapped to On in
 * glr_actions_apply_defaults() via the > AUDIO_CFG_ALL clamp. */
#define AUDIO_CFG_PAUSE 0
#define AUDIO_CFG_ALL   1
static const char *audio_cfg_names[] = { "off", "on" };
static const char *syntax_hl_names[] = { "Off", "On", "On+Shadow" };
static const char *view_mode_names[] = { "3D", "2D" };
static const char *vertex_label_names[GLR_VERTEX_LABEL_COUNT] = {
    [GLR_VERTEX_LABEL_OFF]       = "Off",
    [GLR_VERTEX_LABEL_INDEX]     = "Index",
    [GLR_VERTEX_LABEL_INDEX_POS] = "Index+Pos",
};
/* Accum AA is one cycle that collapses on/off + jitter-sample count:
 * Off -> 2x -> 4x -> 8x -> 16x. The (enabled, samples) split behind it
 * is reconciled in glr_config.c (mirrors the audio cfg collapse). */
static const char *accum_aa_names[] = { "Off", "2x", "4x", "8x", "16x" };

/* Hidden session toggles — intentionally NOT rows in this table (no
 * menu entry, no keyboard-shortcut field here, no @cfg persistence).
 * They are session-only state flipped by a dedicated router handler,
 * mirroring the F1 help overlay. Listed here because g_cfg_items[] is
 * the table a reader scans for "what config exists"; these live
 * elsewhere on purpose:
 *
 *   Ctrl+N        post-processing filter cycle
 *                 -> glr_ctrl_router_handle_post_filter_key
 *                    (glr_ctrl.c); GlrPresentationState.post_filter_mode
 *   Ctrl+Shift+F  code-panel focus (hide boilerplate chrome)
 *                 -> glr_ctrl_router_handle_code_focus_key /
 *                    glr_ctrl_toggle_code_focus (glr_ctrl.c);
 *                    GlrPresentationState.code_focus; also surfaced as
 *                    the clickable statusbar "focus" keycap
 *                    (UI_HIT_CODE_FOCUS_TOGGLE) and the F1 help catalog
 *                    (src/repl/help_text.c).
 *
 * Both Ctrl-key codes are defined/annotated in keys.h. */
static const char *g_msaa_display_label = NULL;

#define CFG_ITEM(label, key_code, is_special, modifiers, key, state_count, state_names, section_header) \
    { label, key_code, is_special, modifiers, key, state_count, state_names, section_header, NULL }

#define CFG_ITEM_DISPLAY(label, key_code, is_special, modifiers, key, state_count, state_names, section_header, display_label_override) \
    { label, key_code, is_special, modifiers, key, state_count, state_names, section_header, display_label_override }

const GlrConfigItem g_cfg_items[] = {
    CFG_ITEM("### RENDERING",     0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM_DISPLAY("MSAA",      KEY_CTRL_U, 0, 0, GLR_CONFIG_MSAA,       2, NULL,                 0, &g_msaa_display_label),
    CFG_ITEM("Line smooth",       0, 0, 0, GLR_CONFIG_LINE_SMOOTH,        2, NULL,                 0),
    CFG_ITEM("Accum AA",          GLUT_KEY_F2, 1, 0, GLR_CONFIG_ACCUM_AA,  5, accum_aa_names,       0),
    CFG_ITEM("Wireframe",         KEY_CTRL_G, 0, 0, GLR_CONFIG_WIREFRAME,  2, NULL,                0),
    CFG_ITEM("Point attenuation", 0, 0, 0, GLR_CONFIG_POINT_ATTENUATION,  2, NULL,                 0),
    CFG_ITEM("---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM("### TIME & REPLAY", 0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM("Auto time",         KEY_CTRL_T, 0, 0, GLR_CONFIG_AUTO_TIME,  2, NULL,                0),
    CFG_ITEM("Replay",            KEY_CTRL_R, 0, 0, GLR_CONFIG_REPLAY,     2, NULL,                0),
    CFG_ITEM("Replay mode",       0, 0, 0, GLR_CONFIG_REPLAY_MODE,         2, replay_mode_names,    0),
    CFG_ITEM("Replay expand",     0, 0, 0, GLR_CONFIG_REPLAY_EXPAND,       2, NULL,                 0),
    CFG_ITEM("---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM("### OVERLAYS & SCENE", 0, 0, 0, GLR_CONFIG_NONE,            0, NULL,                 1),
    CFG_ITEM("Grid",              GLUT_KEY_F3, 1, 0, GLR_CONFIG_GRID_THEME, GRID_THEME_COUNT, grid_theme_names, 0),
    CFG_ITEM("Grid major",        KEY_CTRL_O, 0, 0, GLR_CONFIG_GRID_MAJOR, GRID_MAJOR_COUNT, grid_major_names, 0),
    CFG_ITEM("Grid extent",       GLUT_KEY_F7, 1, 0, GLR_CONFIG_GRID_EXTENT, GRID_EXTENT_COUNT, grid_extent_names, 0),
    CFG_ITEM("Axes",              GLUT_KEY_F4, 1, 0, GLR_CONFIG_AXES_THEME, AXES_THEME_COUNT, axes_theme_names, 0),
    CFG_ITEM("Xform guides",      GLUT_KEY_F8, 1, 0, GLR_CONFIG_XFORM_GUIDE_MODE,
             SCENE_XFORM_GUIDE_COUNT, xform_guide_mode_names, 0),
    CFG_ITEM("Light indicators",  KEY_CTRL_L, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_LIGHT_INDICATORS, 2, NULL, 0),
    CFG_ITEM("Light theme",       GLUT_KEY_F9, 1, 0, GLR_CONFIG_LIGHT_THEME, LIGHT_THEME_COUNT, scene_light_theme_names, 0),
    CFG_ITEM("Backdrop",          GLUT_KEY_F6, 1, 0, GLR_CONFIG_BACKDROP,
             SCENE_BACKDROP_COUNT, backdrop_mode_names, 0),
    CFG_ITEM("Auto-normals",      0, 0, 0, GLR_CONFIG_AUTO_NORMALS, 2, NULL,             0),
    CFG_ITEM("---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM("### CAMERA",        0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM("View mode",         KEY_CTRL_V, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_ORTHO_MODE,   2, view_mode_names, 0),
    CFG_ITEM("Camera rotate",     KEY_CTRL_R, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_CAMERA_ROTATE, 2, NULL,          0),
    CFG_ITEM("Focus origin",      KEY_CTRL_O, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_FOCUS_ORIGIN, 0, NULL, 0),
    CFG_ITEM("Reset camera",      KEY_CTRL_C, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_RESET_CAMERA, 0, NULL, 0),
    CFG_ITEM("---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM("### GEOMETRY",      0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM("Vertex labels",     GLUT_KEY_F5, 1, 0, GLR_CONFIG_VERTEX_LABELS, GLR_VERTEX_LABEL_COUNT, vertex_label_names, 0),
    CFG_ITEM("Normal vectors",    KEY_CTRL_N, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_NORMAL_VECTORS, 2, NULL, 0),
    CFG_ITEM("Vertex outlines",   KEY_CTRL_E, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_VERTEX_OUTLINES, 2, NULL, 0),
    CFG_ITEM("Vertex points",     0, 0, 0, GLR_CONFIG_VERTEX_POINTS,        2, NULL,                 0),
    CFG_ITEM("Poly highlight",    0, 0, 0, GLR_CONFIG_POLY_HIGHLIGHT,       2, NULL,                 0),
    CFG_ITEM("---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM("### INTERFACE",     0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM("Variable panel",    KEY_CTRL_P, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_VARIABLE_PANEL, 2, NULL, 0),
    CFG_ITEM("CPU profile",       KEY_CTRL_W, 0, 0, GLR_CONFIG_CPU_PROFILE, PROFILE_PANEL_MODE_COUNT, profile_panel_mode_names, 0),
    /* Ctrl+Shift+W mirrors CPU profile's Ctrl+W. The two-pass ascii
     * shortcut dispatcher in glr_cfg_handle_ascii_shortcut prefers
     * Shift-requiring rows when Shift is held, so plain Ctrl+W still
     * routes to CPU profile while Ctrl+Shift+W cycles this row. */
    CFG_ITEM("Memory profile",    KEY_CTRL_W, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_MEMORY_PROFILE, MEMORY_PANEL_MODE_COUNT, memory_panel_mode_names, 0),
    CFG_ITEM("Code panel",        KEY_CTRL_B, 0, 0, GLR_CONFIG_CODE_PANEL_LAYOUT, CODE_PANEL_LAYOUT_COUNT, code_panel_layout_names, 0),
    CFG_ITEM("Wrap at commas",    0, 0, 0, GLR_CONFIG_WRAP_AT_COMMA,       2, NULL,                 0),
    CFG_ITEM("Syntax highlight",  GLUT_KEY_F10, 1, 0, GLR_CONFIG_SYNTAX_HIGHLIGHT, 3, syntax_hl_names, 0),
    CFG_ITEM("---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM("### AUDIO",         0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1),
    CFG_ITEM("Audio",             KEY_CTRL_A, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_AUDIO_MODE, 2, audio_cfg_names, 0),
};

const int CFG_ITEM_COUNT = (int)(sizeof(g_cfg_items) / sizeof(g_cfg_items[0]));

#undef CFG_ITEM_DISPLAY
#undef CFG_ITEM

/* ---- Export-config bridge --------------------------------------------- *
 *
 * Lets src/repl/export.c emit/parse @cfg header lines without touching
 * glr_config_* directly; src/repl/scenes.c uses the same bridge for
 * per-scene cfg snapshots. (Originally step 4 of
 * feature/decouple-repl-from-gl-repl-alt.md.) */

#include "repl/cfg_baseline.h"

/* Subset of cfg keys saved per-scene: presentation toggles plus
 * camera_rotate. The controller owns this knowledge so src/repl/scenes.c
 * stays neutral. (Mirrors the keys covered by the pre-step-4
 * k_scene_cfg_keys list.) */
static int cfg_key_in_scene_subset(GlrConfigKey key) {
    switch (key) {
    case GLR_CONFIG_WIREFRAME:
    case GLR_CONFIG_GRID_THEME:
    case GLR_CONFIG_GRID_MAJOR:
    case GLR_CONFIG_GRID_EXTENT:
    case GLR_CONFIG_AXES_THEME:
    case GLR_CONFIG_VERTEX_LABELS:
    case GLR_CONFIG_NORMAL_VECTORS:
    case GLR_CONFIG_VERTEX_OUTLINES:
    case GLR_CONFIG_VERTEX_POINTS:
    case GLR_CONFIG_XFORM_GUIDE_MODE:
    case GLR_CONFIG_LIGHT_INDICATORS:
    case GLR_CONFIG_LIGHT_THEME:
    case GLR_CONFIG_BACKDROP:
    case GLR_CONFIG_ORTHO_MODE:
    case GLR_CONFIG_CAMERA_ROTATE:
    case GLR_CONFIG_VARIABLE_PANEL:
        return 1;
    default:
        return 0;
    }
}

static void glr_export_cfg_fill_all(ReplConfigBag *cfg) {
    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int i = 0; i < n; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE) continue;
        if (item->state_count <= 0) continue; /* action row: nothing to persist */
        repl_config_bag_set_int(cfg, glr_config_item_slug(item),
                                glr_config_get(item->key));
    }
}

static void glr_export_cfg_fill_scene_subset(ReplConfigBag *cfg) {
    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int i = 0; i < n; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE) continue;
        if (!cfg_key_in_scene_subset(item->key))                  continue;
        repl_config_bag_set_int(cfg, glr_config_item_slug(item),
                                glr_config_get(item->key));
    }
}

static void glr_export_cfg_normalize_legacy_alias(const char **slug, int *val,
                                                  char *slug_buf,
                                                  size_t slug_buf_sz) {
    if (!slug || !*slug || !val || !slug_buf || slug_buf_sz == 0)
        return;

    if (strcmp(*slug, "top_code_panel") == 0) {
        snprintf(slug_buf, slug_buf_sz, "%s", "code_panel");
        *slug = slug_buf;
        *val = *val ? CODE_PANEL_LAYOUT_TOP : CODE_PANEL_LAYOUT_LEFT;
    }
}

static const GlrConfigItem *glr_export_cfg_find_item_by_slug(const char *slug) {
    if (!slug)
        return NULL;

    int dummy_val = 1;
    char normalized_slug[REPL_CFG_KEY_MAX];
    glr_export_cfg_normalize_legacy_alias(&slug, &dummy_val,
                                          normalized_slug,
                                          sizeof(normalized_slug));

    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int i = 0; i < n; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE)
            continue;
        if (strcmp(glr_config_item_slug(item), slug) == 0)
            return item;
    }
    return NULL;
}

/* Symbolic-name → enum-value tables. Built-in catalogs (examples,
 * tutorials) record values like "GRID_THEME_RADAR" rather than the raw
 * `10`; this lookup resolves them at apply time so reordering the
 * underlying enum (src/scene/themes.h) doesn't silently shift which
 * value the catalog selects.
 *
 * The slug→table map below covers every enum-valued slug the catalogs
 * actually use today (grid / axes / backdrop). Other enum-shaped slugs
 * — replay, code_panel_layout, vertex_label, etc. — stay integer-only
 * in their saved form because no catalog literal carries them
 * symbolically. Add a table here if a new catalog needs symbolic
 * support for one of those slugs.
 *
 * Each table is keyed by enum index so STATIC_ASSERT can pin
 * length-against-count, catching "new enum value but missed the
 * table" at build time. */
/* The string tables share their entry list with the enum definitions
 * in src/scene/themes.h via X-macros, so adding a theme/backdrop there
 * adds its symbol here automatically. */
static const char *cfg_grid_theme_symbols[GRID_THEME_COUNT] = {
#define GRID_THEME_SYMBOL_ENTRY(name) [GRID_THEME_##name] = "GRID_THEME_" #name,
    GRID_THEME_LIST(GRID_THEME_SYMBOL_ENTRY)
#undef GRID_THEME_SYMBOL_ENTRY
};
static const char *cfg_axes_theme_symbols[AXES_THEME_COUNT] = {
#define AXES_THEME_SYMBOL_ENTRY(name) [AXES_THEME_##name] = "AXES_THEME_" #name,
    AXES_THEME_LIST(AXES_THEME_SYMBOL_ENTRY)
#undef AXES_THEME_SYMBOL_ENTRY
};
static const char *cfg_backdrop_mode_symbols[SCENE_BACKDROP_COUNT] = {
#define SCENE_BACKDROP_SYMBOL_ENTRY(name) [SCENE_BACKDROP_##name] = "SCENE_BACKDROP_" #name,
    SCENE_BACKDROP_LIST(SCENE_BACKDROP_SYMBOL_ENTRY)
#undef SCENE_BACKDROP_SYMBOL_ENTRY
};

static int glr_cfg_symbol_table_lookup(const char *value_name,
                                       const char *const *table,
                                       int count, int *out_value) {
    if (!value_name || !table || !out_value) return 0;
    for (int i = 0; i < count; i++) {
        if (table[i] && strcmp(value_name, table[i]) == 0) {
            *out_value = i;
            return 1;
        }
    }
    return 0;
}

static int glr_export_cfg_resolve_text(const char *slug,
                                       const char *value_name,
                                       int *out_value) {
    if (!slug || !value_name || !out_value) return 0;
    if (strcmp(slug, "grid") == 0)
        return glr_cfg_symbol_table_lookup(value_name,
                                           cfg_grid_theme_symbols,
                                           GRID_THEME_COUNT, out_value);
    if (strcmp(slug, "axes") == 0)
        return glr_cfg_symbol_table_lookup(value_name,
                                           cfg_axes_theme_symbols,
                                           AXES_THEME_COUNT, out_value);
    if (strcmp(slug, "backdrop") == 0)
        return glr_cfg_symbol_table_lookup(value_name,
                                           cfg_backdrop_mode_symbols,
                                           SCENE_BACKDROP_COUNT, out_value);
    return 0;
}

/* True iff `s` is a clean integer literal (`-?[0-9]+` plus trailing
 * whitespace). The fallback path below uses this to decide whether
 * strtol's "0 on no conversion" is meaningful: an identifier-shaped
 * string like "GRID_THEME_RADRA" (a typo'd enum name) is NOT a
 * legacy integer-form value — strtol would silently land it at 0,
 * collapsing every misspelled symbol onto the first enum entry. */
static int glr_export_cfg_value_is_integer_literal(const char *s) {
    if (!s || !*s) return 0;
    if (*s == '+' || *s == '-') s++;
    if (!isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*s)) s++;
    while (*s && isspace((unsigned char)*s)) s++;
    return *s == '\0';
}

/* Resolve a bag value to its integer form. Tries the symbolic
 * resolver first (so catalog literals like "GRID_THEME_RADAR" land at
 * the right enum value); falls back to strtol ONLY for clean integer
 * literals so legacy integer-form saved files keep loading. Returns
 * 1 on success and writes the int into *out_value; returns 0 for an
 * identifier-shaped value that doesn't resolve (the apply caller
 * then drops the row instead of silently landing it at 0). */
static int glr_export_cfg_resolve_value(const char *slug,
                                        const char *value_text,
                                        int *out_value) {
    if (!out_value) return 0;
    if (glr_export_cfg_resolve_text(slug, value_text, out_value))
        return 1;
    if (!glr_export_cfg_value_is_integer_literal(value_text))
        return 0;
    *out_value = (int)strtol(value_text, NULL, 10);
    return 1;
}

static void glr_export_cfg_apply(const ReplConfigBag *cfg) {
    if (!cfg) return;
    for (int idx = 0; idx < cfg->count; idx++) {
        const char *slug  = cfg->items[idx].key;
        const char *value = cfg->items[idx].value;
        int val;
        if (!glr_export_cfg_resolve_value(slug, value, &val)) {
            /* Unresolved symbolic value — drop the row rather than
             * silently land it at 0 (GRID_THEME_OFF / AXES_THEME_OFF
             * etc.). The pre-bridge parse_cfg path did the
             * equivalent via strtol; tightening it here is what
             * makes catalog typos fail loud instead of muting the
             * showcase. */
            fprintf(stderr,
                    "repl_cfg: dropping '%s = %s' (unknown symbolic value)\n",
                    slug, value);
            continue;
        }
        if (strcmp(slug, "top_code_panel") == 0) {
            val = val ? CODE_PANEL_LAYOUT_TOP : CODE_PANEL_LAYOUT_LEFT;
        }
        const GlrConfigItem *item = glr_export_cfg_find_item_by_slug(slug);
        if (item)
            glr_config_set(item->key, val);
        /* Unknown slugs silently ignored — same behaviour as the pre-bridge
         * parse_cfg path: drop unrecognised cfg keys. */
    }
}

static int glr_export_cfg_get_int(const char *slug, int fallback) {
    const GlrConfigItem *item = glr_export_cfg_find_item_by_slug(slug);
    if (item)
        return glr_config_get(item->key);
    return fallback;
}

static int glr_export_cfg_is_known(const char *slug) {
    return glr_export_cfg_find_item_by_slug(slug) ? 1 : 0;
}

static int glr_export_cfg_slug_is_scene_subset(const char *slug) {
    const GlrConfigItem *item = glr_export_cfg_find_item_by_slug(slug);
    return item && cfg_key_in_scene_subset(item->key) ? 1 : 0;
}

const ReplConfigBridge g_glr_export_cfg_bridge = {
    .fill_all          = glr_export_cfg_fill_all,
    .fill_scene_subset = glr_export_cfg_fill_scene_subset,
    .apply             = glr_export_cfg_apply,
    .get_int           = glr_export_cfg_get_int,
    .is_known          = glr_export_cfg_is_known,
    .slug_is_scene_subset = glr_export_cfg_slug_is_scene_subset,
    .resolve_text      = glr_export_cfg_resolve_text,
};

void glr_actions_install_export_cfg_bridge(void) {
    repl_config_install_bridge(&g_glr_export_cfg_bridge);
}

void glr_actions_set_msaa_label(int samples) {
    static char msaa_label[32];
    if (samples > 1) {
        snprintf(msaa_label, sizeof(msaa_label), "MSAAx%d", samples);
    } else {
        snprintf(msaa_label, sizeof(msaa_label), "MSAA");
    }
    g_msaa_display_label = msaa_label;
}

void glr_actions_apply_audio_cfg_mode(int mode) {
    if (mode == AUDIO_CFG_PAUSE) {
        glr_audio_set_paused(1);
    } else {
        glr_audio_set_paused(0);
        glr_audio_set_loop_mode(GLR_AUDIO_LOOP_ALL);
    }
}

int glr_scene_menu_slot_for_dense_index(int scene_idx) {
    int seen = 0;
    for (int slot = 0; slot < MAX_USER_SCENES; slot++) {
        if (!repl_user_scene_slot_used(slot))
            continue;
        if (seen == scene_idx)
            return slot;
        seen++;
    }
    return -1;
}

/* Shared scene-load sequences. The Scene menu and the scene tab strip
 * both switch scenes; keep the load sequence (and its load-bearing
 * subtleties) in one place rather than duplicating the statements.
 * Neither helper self-no-ops on "already active" — the Scene menu
 * always reloads, so callers that want a no-op (the tab router) check
 * before calling. */
void glr_scene_load_example(int example_idx) {
    glr_ctrl_reset_transients();
    editor_undo_note_wholesale_replacement();
    editor_state_edit_line_set(repl_load_example(example_idx));
}

void glr_scene_load_user_slot(int slot) {
    glr_ctrl_reset_transients();
    editor_undo_note_wholesale_replacement();
    glr_camera_clear_scene_default();
    if (repl_load_user_scene_idx(slot))
        editor_load_line_to_input(editor_state_edit_line());
}

void glr_cfg_cycle_row(int row, int delta) {
    const GlrConfigItem *item = glr_config_item_at(row);

    if (!item || item->section_header)
        return;

    /* Replay is special-cased: its cfg toggle kicks off/ends the replay
     * machinery rather than flipping the int directly. Both directions
     * collapse to "toggle". */
    if (item->key == GLR_CONFIG_REPLAY) {
        int turn_on = !glr_config_get(GLR_CONFIG_REPLAY);
        glr_config_set(GLR_CONFIG_REPLAY, turn_on);
        if (!turn_on)
            repl_set_status("Replay: off");
        return;
    }

    /* Camera action rows: momentary, no state to cycle. Both directions
     * collapse to "do it". Easing keeps the move smooth, not jarring. */
    if (item->key == GLR_CONFIG_FOCUS_ORIGIN) {
        glr_camera_focus_origin();
        repl_set_status("Camera: focus origin");
        return;
    }
    if (item->key == GLR_CONFIG_RESET_CAMERA) {
        glr_camera_ease_to_default();
        repl_set_status("Camera: reset to default");
        return;
    }

    if (item->key == GLR_CONFIG_AUTO_TIME) {
        if (editor_input_active_modifiers() & GLUT_ACTIVE_SHIFT) {
            repl_reset_time_to_zero();
            repl_set_status(repl_state_variables().time_playing ? "Time: reset to 0"
                                                           : "Time: reset to 0 (paused)");
            return;
        }
    }

    if (item->key == GLR_CONFIG_AUTO_NORMALS || item->key == GLR_CONFIG_POINT_ATTENUATION) {
        if (replay_active())
            replay_stop();
    }

    int new_value = glr_config_cycle(item->key, delta);
    glr_ctrl_sync_ui_chrome();  /* refresh ui_state.code_panel mirrors */

    if (item->key == GLR_CONFIG_CODE_PANEL_LAYOUT) {
        ui_state_code_panel_mut()->panel_frac = CFG_DEFAULT_PANEL_FRAC;
        GlrPresentationState p = glr_state_presentation();
        if (p.code_panel_layout == CODE_PANEL_LAYOUT_TOP) {
            repl_set_status("Layout: top code panel");
        } else if (p.code_panel_layout == CODE_PANEL_LAYOUT_BOTTOM) {
            repl_set_status("Layout: bottom code panel");
        } else if (p.code_panel_layout == CODE_PANEL_LAYOUT_HIDDEN) {
            ui_menu_bar_close();
            color_picker_stop();
            editor_completion_clear();
            repl_set_status("Layout: code panel hidden");
        } else {
            repl_set_status("Layout: left code panel");
        }
    } else if (item->key == GLR_CONFIG_AUTO_NORMALS) {
        if (glr_state_presentation().autonormal) {
            repl_mark_source_dirty();
            repl_set_status("Auto-normals: ON");
        } else {
            repl_set_status("Auto-normals: OFF (existing normals kept)");
        }
    } else if (item->key == GLR_CONFIG_POINT_ATTENUATION) {
        repl_apply_init_bootstrap();
        repl_set_status(glr_config_get(GLR_CONFIG_POINT_ATTENUATION) ? "Point attenuation: ON"
                                                                  : "Point attenuation: OFF");

    } else if (item->key == GLR_CONFIG_LIGHT_THEME) {
        /* scene_lights_apply_theme + eye-space init already ran inside
         * glr_config_set above (so @cfg-driven theme loads get the
         * same treatment). The cycle handler just needs to re-apply
         * the init bootstrap so the exporter / code-panel light lines
         * pick up the new positions and colors. */
        repl_apply_init_bootstrap();
        snprintf(cfg_status_buf, sizeof(cfg_status_buf), "%s: %s",
                 glr_config_item_display_label(item),
                 glr_config_state_name(item->key, new_value));
        repl_set_status(cfg_status_buf);
    } else if (item->state_names) {
        snprintf(cfg_status_buf, sizeof(cfg_status_buf), "%s: %s",
                 glr_config_item_display_label(item),
                 glr_config_state_name(item->key, new_value));
        repl_set_status(cfg_status_buf);
    } else if (item->state_count == 2) {
        snprintf(cfg_status_buf, sizeof(cfg_status_buf), "%s: %s",
                 glr_config_item_display_label(item),
                 new_value ? "ON" : "OFF");
        repl_set_status(cfg_status_buf);
    }
}

void glr_action_cursor_blink_reset(void) {
    EditorCursorBlinkState *cb = editor_state_cursor_blink_mut();

    cb->cursor_visible = 1;
    cb->blink_tick = 0;
}

/* Highest valid help tab index, derived from the actual content so
 * adding/removing a tab in help_text.c needs no edit here. */
static int glr_help_max_tab_idx(void) {
    const ReplHelpContent *help = repl_help_text_build();
    if (!help || help->tab_count <= 0)
        return 0;
    return help->tab_count - 1;
}

void glr_action_help_tab_next(void) {
    int tab = editor_help_session_tab_idx();
    if (tab < glr_help_max_tab_idx()) {
        editor_help_session_set_tab(tab + 1);
        editor_help_session_set_scroll(0);
    }
}

void glr_action_help_tab_prev(void) {
    int tab = editor_help_session_tab_idx();
    if (tab > 0) {
        editor_help_session_set_tab(tab - 1);
        editor_help_session_set_scroll(0);
    }
}

/* Ctrl+<key> shortcut dispatch. GLUT delivers Ctrl+letter as the same
 * control code with or without Shift, so Shift is read from the live
 * modifier state and matched in two passes:
 *
 *   Pass A (only when Shift is held): prefer a row that *requires*
 *     Shift (modifiers & GLUT_ACTIVE_SHIFT) — e.g. Ctrl+Shift+V/O/C.
 *   Pass B (always): fall back to the modifier-agnostic row
 *     (modifiers == 0). This keeps the deliberate quirk where one
 *     modifiers==0 row answers both forms — Ctrl+T toggles time and
 *     Ctrl+Shift+T resets it, both via the single Auto time row whose
 *     handler then inspects Shift itself.
 *
 * So a Shift row shadows the plain row only when Shift is actually
 * down; plain Ctrl+V still falls through to the editor (paste) because
 * no modifiers==0 row claims it. The descriptor table is the single
 * source of truth — no separate router. */
static int cfg_match_row(unsigned char key, int want_shift) {
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (!item || item->section_header || item->is_special)
            continue;
        if (item->key_code <= 0 || item->key_code >= 32 ||
            item->key_code != key)
            continue;
        int row_shift = (item->modifiers & GLUT_ACTIVE_SHIFT) != 0;
        if (row_shift != want_shift)
            continue;
        glr_cfg_cycle_row(i, 1);
        return 1;
    }
    return 0;
}

int glr_cfg_handle_ascii_shortcut(unsigned char key) {
    int shift = (editor_input_active_modifiers() & GLUT_ACTIVE_SHIFT) != 0;
    if (shift && cfg_match_row(key, 1)) /* pass A: Shift-requiring row */
        return 1;
    return cfg_match_row(key, 0);       /* pass B: modifier-agnostic row */
}

int glr_cfg_handle_special_shortcut(int key) {
    /* F<n> steps the bound row forward; Shift+F<n> steps it backward.
     * For 2-state toggles the direction is immaterial (both flip); for
     * the multi-state cycles (grid / axes / vertex labels / xform
     * guides / light theme) Shift reverses the wrap. No F-key row uses
     * the descriptor's `modifiers` field, so Shift is free to mean
     * "backward" here. Modifiers are read in the actions layer, matching
     * glr_cfg_handle_ascii_shortcut. */
    int delta = (editor_input_active_modifiers() & GLUT_ACTIVE_SHIFT) ? -1 : 1;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && !item->section_header &&
            item->is_special && item->key_code == key) {
            glr_cfg_cycle_row(i, delta);
            return 1;
        }
    }
    return 0;
}

int glr_action_menu_item_activate(int menu_id, int item_idx) {
    switch (menu_id) {
    case GLR_MENU_FILE:
        switch (item_idx) {
        case GLR_FILE_ITEM_NEW_SCENE:
            repl_scenes_enter_transient_scene();
            repl_scenes_reset_for_transient();
            editor_reset_for_new_scene();
            editor_undo_note_wholesale_replacement();
            glr_camera_clear_scene_default();
            return 1;
        case GLR_FILE_ITEM_SAVE_SCENE: {
            ReplExportLayout layout;
            glr_ctrl_fill_export_layout(&layout);
            repl_save_active_scene(&layout);
            return 1;
        }
        case GLR_FILE_ITEM_LOAD_SCENE:
            editor_inline_file_prompt_begin(DEFAULT_SCENE_FILE);
            return 1;
        case GLR_FILE_ITEM_RENAME_SCENE: {
            int slot = repl_active_user_scene();
            if (slot < 0) {
                repl_set_status_error("No active scene to rename");
                return 1;
            }
            editor_inline_rename_begin(slot);
            return 1;
        }
        case GLR_FILE_ITEM_SAVE_WORKSPACE: {
            const char *dir = repl_workspace_dir();
            if (!dir || !dir[0])
                dir = GLR_DEFAULT_WORKSPACE_DIR;
            ReplExportLayout layout;
            glr_ctrl_fill_export_layout(&layout);
            repl_save_workspace(dir, &layout);
            return 1;
        }
        case GLR_FILE_ITEM_LOAD_WORKSPACE: {
            const char *dir = repl_workspace_dir();
            if (!dir || !dir[0])
                dir = GLR_DEFAULT_WORKSPACE_DIR;
            glr_camera_clear_scene_default();
            int n = repl_load_workspace(dir);
            if (n >= 0)
                editor_undo_note_wholesale_replacement();
            return 1;
        }
        case GLR_FILE_ITEM_SCENE_SEP:
            return 1;
        default:
            /* Out-of-range or unknown file menu item. Return 1 (consumed)
             * to keep backward compatibility with existing tests. */
            return 1;
        }

    case GLR_MENU_SCENE: {
        int tag_count = repl_example_visible_tag_count();
        if (item_idx >= 1 && item_idx <= tag_count)
            return 0;

        int scene_idx = item_idx - (tag_count + GLR_SCENE_OFF_SCENES);
        if (scene_idx >= 0 && scene_idx < repl_user_scene_count()) {
            int slot = glr_scene_menu_slot_for_dense_index(scene_idx);
            if (slot >= 0) {
                glr_scene_load_user_slot(slot);
                return 1;
            }
        }
        /* Returns 1 (consumed) for out-of-range, header row, and negative indexes,
         * matching test expectations. */
        return 1;
    }

    case GLR_MENU_TUTORIALS: {
        /* Top-level Tutorials rows after the Phase-B hierarchical menu:
         *   [0..t-1]   tag rows (inert hover-only, like Scene tag rows
         *              — the actual tutorial flyout-item activation
         *              flows through route_submenu_item_hit which
         *              dispatches directly to tutorial_start, NOT
         *              through this function).
         *   [t]        "---" (chrome row, filtered before activation).
         *   [t+1]      "Restart Tutorial" (only when tutorial_active).
         *   [t+2]      "Exit Tutorial"    (only when tutorial_active). */
        int tag_count = repl_tutorial_visible_tag_count();
        if (item_idx < tag_count)
            return 0;   /* tag row → keep menu open, no action */
        if (tutorial_active() && item_idx == tag_count + GLR_TUTORIAL_OFF_RESTART) {
            TutorialRuntimeState tutorial = tutorial_state_view();
            if (tutorial.tutorial_idx >= 0)
                tutorial_start(tutorial.tutorial_idx);
            return 1;
        }
        if (tutorial_active() && item_idx == tag_count + GLR_TUTORIAL_OFF_EXIT) {
            tutorial_stop();
            return 1;
        }
        /* Returns 1 (consumed) for out-of-range or separators, matching test expectations. */
        return 1;
    }

    case GLR_MENU_CONFIG:
        /* Config top-level rows are section / "All" PARENT rows: they
         * hover-open a flyout, and a click on the parent itself is
         * inert (mirrors the MENU_SCENE tag-row guard above). `item_idx`
         * here is a section parent row, NOT a g_cfg_items[] index, so
         * it must never be cycled. Leaf config-item activation arrives
         * via UI_HIT_SUBMENU_ITEM and is dispatched straight to
         * glr_cfg_cycle_row() on the absolute g_cfg_items[] index
         * (route_submenu_item_hit) — it never reaches this branch.
         * Return 0 so the dropdown stays open, matching the old
         * per-toggle feel. */
        (void)item_idx;
        return 0;

    default:
        return 1;
    }
}

void glr_actions_apply_defaults(void) {
    /* Restore the audio mode persisted from the previous session.
     * glr_audio_play_playlist() calls load_state() which stores the cfg_mode
     * in the audio module; pull it here so the UI config and the actual audio
     * engine agree before the first frame. */
    int saved_mode = glr_audio_get_cfg_mode();
    if (saved_mode < AUDIO_CFG_PAUSE || saved_mode > AUDIO_CFG_ALL)
        saved_mode = AUDIO_CFG_ALL;
    glr_actions_apply_audio_cfg_mode(saved_mode);
    glr_audio_set_cfg_mode(saved_mode);
}
