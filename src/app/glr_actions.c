/*
 * repl_actions.c -- Side-effecting editor actions and config dispatch.
 *
 * Input modules decide which key or menu row was activated. UI modules decide
 * what was clicked. This module owns the mutation that follows: config-row
 * cycling, F-key/Ctrl-key config shortcuts, startup config defaults, and menu
 * item actions that touch scenes, files, replay, audio, or presentation state.
 */
#include "app/glr_actions.h"
#include "app/glr_ctrl.h"            /* glr_ctrl_sync_ui_chrome */
#include "app/glr_state.h"           /* presentation/render storage (step 7a) */
#include "app/glr_camera.h"          /* camera focus-origin / reset (eased) */
#include "ui/layout.h"           /* CODE_PANEL_LAYOUT_* enum values */
#include "widgets/color_picker_state.h"
#include "audio.h"
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
#include "widgets/replay.h"
#include "widgets/replay_state.h"
#include "widgets/tutorial.h"
#include "widgets/tutorial_state.h"
#include "editor/help_session.h"
#include "repl/pipeline.h"
#include "repl/state_owners.h"
#include "ui/menu_bar.h"
#include "ui/profile_panel.h"
#include "ui/state.h"
#include "editor/inline_rename.h"
#include "editor/undo.h"

static const char *replay_mode_names[] = { "Polygon", "Vertex" };
static const char *backdrop_mode_names[] = { "Off", "Cityscape", "Stars", "City+Stars" };
static const char *xform_guide_mode_names[] = { "World", "Frame" };
static const char *profile_panel_mode_names[] = { "Off", "On", "Details" };
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
static char cfg_status_buf[256];

/* Unified audio cfg: collapses mute + loop mode into one cycling menu entry.
 * Indices:
 *   0 = Pause   - paused, loop mode untouched
 *   1 = Once    - playing, loop mode OFF  (playlist plays through)
 *   2 = Song    - playing, loop mode SONG (repeat current track)
 *   3 = All     - playing, loop mode ALL  (playlist, wrap forever)
 * Default 3 matches repl_audio.c's LOOP_ALL default with volume on. */
#define AUDIO_CFG_PAUSE 0
#define AUDIO_CFG_ONCE  1
#define AUDIO_CFG_SONG  2
#define AUDIO_CFG_ALL   3
static const char *audio_cfg_names[] = { "Pause", "Once", "Song", "All" };
static const char *syntax_hl_names[] = { "Off", "On", "On+Bold" };
static const char *view_mode_names[] = { "3D", "2D" };
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
GlrConfigItem g_cfg_items[] = {
    /* { label, key_code, is_special, modifiers, key, state_count, state_names, section_header } */
    { "### RENDERING",     0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "MSAA",              KEY_CTRL_U, 0, 0, GLR_CONFIG_MSAA,       2, NULL,                 0 },
    { "Line smooth",       0, 0, 0, GLR_CONFIG_LINE_SMOOTH,        2, NULL,                 0 },
    { "Accum AA",          0, 0, 0, GLR_CONFIG_ACCUM_AA,           5, accum_aa_names,       0 },
    { "Wireframe",         GLUT_KEY_F2, 1, 0, GLR_CONFIG_WIREFRAME, 2, NULL,                0 },
    { "Point attenuation", 0, 0, 0, GLR_CONFIG_POINT_ATTENUATION,  2, NULL,                 0 },
    { "---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "### TIME & REPLAY", 0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "Auto time",         KEY_CTRL_T, 0, 0, GLR_CONFIG_AUTO_TIME,  2, NULL,                0 },
    { "Replay",            KEY_CTRL_R, 0, 0, GLR_CONFIG_REPLAY,     2, NULL,                0 },
    { "Replay mode",       0, 0, 0, GLR_CONFIG_REPLAY_MODE,         2, replay_mode_names,    0 },
    { "Replay expand",     0, 0, 0, GLR_CONFIG_REPLAY_EXPAND,       2, NULL,                 0 },
    { "---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "### OVERLAYS & SCENE", 0, 0, 0, GLR_CONFIG_NONE,            0, NULL,                 1 },
    { "Grid",              GLUT_KEY_F3, 1, 0, GLR_CONFIG_GRID_THEME, GRID_THEME_COUNT, grid_theme_names, 0 },
    { "Grid major",        KEY_CTRL_O, 0, 0, GLR_CONFIG_GRID_MAJOR, GRID_MAJOR_COUNT, grid_major_names, 0 },
    { "Grid extent",       0, 0, 0, GLR_CONFIG_GRID_EXTENT,         GRID_EXTENT_COUNT, grid_extent_names, 0 },
    { "Axes",              GLUT_KEY_F4, 1, 0, GLR_CONFIG_AXES_THEME, AXES_THEME_COUNT, axes_theme_names, 0 },
    { "Xform guides",      GLUT_KEY_F8, 1, 0, GLR_CONFIG_XFORM_GUIDES, 2, NULL,            0 },
    { "Xform guide mode",  0, 0, 0, GLR_CONFIG_XFORM_GUIDE_MODE,     2, xform_guide_mode_names, 0 },
    { "Light indicators",  GLUT_KEY_F10, 1, 0, GLR_CONFIG_LIGHT_INDICATORS, 2, NULL,       0 },
    { "Backdrop",          0, 0, 0, GLR_CONFIG_BACKDROP,             4, backdrop_mode_names, 0 },
    { "Auto-normals",      GLUT_KEY_F9, 1, 0, GLR_CONFIG_AUTO_NORMALS, 2, NULL,             0 },
    { "---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "### CAMERA",        0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "View mode",         KEY_CTRL_V, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_ORTHO_MODE,   2, view_mode_names, 0 },
    { "Camera rotate",     GLUT_KEY_F11, 1, 0, GLR_CONFIG_CAMERA_ROTATE, 2, NULL,          0 },
    { "Focus origin",      KEY_CTRL_O, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_FOCUS_ORIGIN, 0, NULL, 0 },
    { "Reset camera",      KEY_CTRL_C, 0, GLUT_ACTIVE_SHIFT, GLR_CONFIG_RESET_CAMERA, 0, NULL, 0 },
    { "---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "### GEOMETRY",      0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "Vertex labels",     GLUT_KEY_F5, 1, 0, GLR_CONFIG_VERTEX_LABELS, 2, NULL,            0 },
    { "Normal vectors",    GLUT_KEY_F6, 1, 0, GLR_CONFIG_NORMAL_VECTORS, 2, NULL,           0 },
    { "Vertex outlines",   GLUT_KEY_F7, 1, 0, GLR_CONFIG_VERTEX_OUTLINES, 2, NULL,          0 },
    { "Vertex points",     0, 0, 0, GLR_CONFIG_VERTEX_POINTS,        2, NULL,                 0 },
    { "Poly highlight",    0, 0, 0, GLR_CONFIG_POLY_HIGHLIGHT,       2, NULL,                 0 },
    { "---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "### INTERFACE",     0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "Variable panel",    0, 0, 0, GLR_CONFIG_VARIABLE_PANEL,      2, NULL,                 0 },
    { "CPU profile",       KEY_CTRL_W, 0, 0, GLR_CONFIG_CPU_PROFILE, PROFILE_PANEL_MODE_COUNT, profile_panel_mode_names, 0 },
    { "Code panel",        KEY_CTRL_B, 0, 0, GLR_CONFIG_CODE_PANEL_LAYOUT, CODE_PANEL_LAYOUT_COUNT, code_panel_layout_names, 0 },
    { "Wrap at commas",    0, 0, 0, GLR_CONFIG_WRAP_AT_COMMA,       2, NULL,                 0 },
    { "Syntax highlight",  0, 0, 0, GLR_CONFIG_SYNTAX_HIGHLIGHT,    3, syntax_hl_names,      0 },
    { "---",               0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "### AUDIO",         0, 0, 0, GLR_CONFIG_NONE,               0, NULL,                 1 },
    { "Audio",             0, 0, 0, GLR_CONFIG_AUDIO_MODE,          4, audio_cfg_names,      0 },
};

const int CFG_ITEM_COUNT = (int)(sizeof(g_cfg_items) / sizeof(g_cfg_items[0]));

/* ---- Export-config bridge (step 4 of the decouple plan) ---------------- */

#include "repl/export.h"

static void cfg_slug_from_label(const char *label, char *out, size_t out_sz) {
    size_t out_idx = 0;
    for (size_t i = 0; label[i] && out_idx + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)label[i];
        if (c == ' ' || c == '\t' || c == '-' || c == '/') out[out_idx++] = '_';
        else if (isalnum(c))                               out[out_idx++] = (char)tolower(c);
        else if (c == '_')                                 out[out_idx++] = '_';
    }
    if (out_sz > 0) out[out_idx] = '\0';
}

/* Subset of cfg keys saved per-scene (mirrors what the pre-step-4
 * k_scene_cfg_keys list covered, including camera_rotate). The
 * subset includes presentation toggles plus camera_rotate; the
 * controller owns this knowledge so src/repl/scenes.c stays neutral. */
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
    case GLR_CONFIG_XFORM_GUIDES:
    case GLR_CONFIG_XFORM_GUIDE_MODE:
    case GLR_CONFIG_LIGHT_INDICATORS:
    case GLR_CONFIG_BACKDROP:
    case GLR_CONFIG_CAMERA_ROTATE:
        return 1;
    default:
        return 0;
    }
}

static void glr_export_cfg_fill_all(ReplExportConfig *cfg) {
    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int i = 0; i < n; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE) continue;
        if (item->state_count <= 0) continue; /* action row: nothing to persist */
        char slug[REPL_EXPORT_CFG_KEY_MAX];
        cfg_slug_from_label(item->label, slug, sizeof(slug));
        repl_export_config_set_int(cfg, slug, glr_config_get(item->key));
    }
}

static void glr_export_cfg_fill_scene_subset(ReplExportConfig *cfg) {
    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int i = 0; i < n; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE) continue;
        if (!cfg_key_in_scene_subset(item->key))                  continue;
        char slug[REPL_EXPORT_CFG_KEY_MAX];
        cfg_slug_from_label(item->label, slug, sizeof(slug));
        repl_export_config_set_int(cfg, slug, glr_config_get(item->key));
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

static void glr_export_cfg_apply(const ReplExportConfig *cfg) {
    if (!cfg) return;
    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int idx = 0; idx < cfg->count; idx++) {
        const char *slug = cfg->items[idx].key;
        int val = (int)strtol(cfg->items[idx].value, NULL, 10);
        char normalized_slug[REPL_EXPORT_CFG_KEY_MAX];
        glr_export_cfg_normalize_legacy_alias(&slug, &val,
                                              normalized_slug,
                                              sizeof(normalized_slug));
        for (int i = 0; i < n; i++) {
            const GlrConfigItem *item = &items[i];
            if (item->section_header || item->key == GLR_CONFIG_NONE) continue;
            char item_slug[REPL_EXPORT_CFG_KEY_MAX];
            cfg_slug_from_label(item->label, item_slug, sizeof(item_slug));
            if (strcmp(item_slug, slug) == 0) {
                glr_config_set(item->key, val);
                break;
            }
        }
        /* Unknown slugs silently ignored — same behaviour as the pre-bridge
         * parse_cfg path: drop unrecognised cfg keys. */
    }
}

static int glr_export_cfg_get_int(const char *slug, int fallback) {
    if (!slug) return fallback;
    int n = 0;
    const GlrConfigItem *items = glr_config_items(&n);
    for (int i = 0; i < n; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE) continue;
        char item_slug[REPL_EXPORT_CFG_KEY_MAX];
        cfg_slug_from_label(item->label, item_slug, sizeof(item_slug));
        if (strcmp(item_slug, slug) == 0)
            return glr_config_get(item->key);
    }
    return fallback;
}

const ReplExportConfigBridge g_glr_export_cfg_bridge = {
    .fill_all          = glr_export_cfg_fill_all,
    .fill_scene_subset = glr_export_cfg_fill_scene_subset,
    .apply             = glr_export_cfg_apply,
    .get_int           = glr_export_cfg_get_int,
};

void glr_actions_install_export_cfg_bridge(void) {
    repl_export_install_config_bridge(&g_glr_export_cfg_bridge);
}

static void apply_audio_cfg_mode(int mode) {
    switch (mode) {
    case AUDIO_CFG_PAUSE:
        audio_set_paused(1);
        break;
    case AUDIO_CFG_ONCE:
        audio_set_paused(0);
        audio_set_loop_mode(AUDIO_LOOP_OFF);
        break;
    case AUDIO_CFG_SONG:
        audio_set_paused(0);
        audio_set_loop_mode(AUDIO_LOOP_SONG);
        break;
    case AUDIO_CFG_ALL:
    default:
        audio_set_paused(0);
        audio_set_loop_mode(AUDIO_LOOP_ALL);
        break;
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
    /* Clear editor / camera / menu / picker / code-panel-drag transients
     * so the new scene starts from a clean controller state. */
    editor_reset_transients();
    editor_undo_clear();
    repl_load_example(example_idx);
}

void glr_scene_load_user_slot(int slot) {
    editor_undo_clear();
    if (repl_load_user_scene_idx(slot))
        editor_load_line_to_input(repl_state_edit_line());
}

void glr_cfg_cycle_row(int row, int delta) {
    const GlrConfigItem *item = glr_config_item_at(row);

    if (!item || item->section_header)
        return;

    /* Replay is special-cased: its cfg toggle kicks off/ends the replay
     * machinery rather than flipping the int directly. Both directions
     * collapse to "toggle". */
    if (item->key == GLR_CONFIG_REPLAY) {
        if (glr_config_get(GLR_CONFIG_REPLAY)) {
            replay_stop();
            repl_set_status("Replay: off");
        } else {
            replay_start();
        }
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

    if (replay_active())
        replay_stop();

    int new_value = glr_config_cycle(item->key, delta);
    glr_ctrl_sync_ui_chrome();  /* refresh ui_state.code_panel mirrors */

    if (item->key == GLR_CONFIG_CODE_PANEL_LAYOUT) {
        ui_state_code_panel_mut()->panel_frac = CFG_DEFAULT_PANEL_FRAC;
        if (glr_state_presentation().code_panel_layout == CODE_PANEL_LAYOUT_TOP) {
            repl_set_status("Layout: top code panel");
        } else if (glr_state_presentation().code_panel_layout == CODE_PANEL_LAYOUT_BOTTOM) {
            repl_set_status("Layout: bottom code panel");
        } else if (glr_state_presentation().code_panel_layout == CODE_PANEL_LAYOUT_HIDDEN) {
            ui_menu_bar_close();
            color_picker_close();
            editor_completion_clear();
            repl_set_status("Layout: code panel hidden");
        } else {
            repl_set_status("Layout: left code panel");
        }
    } else if (item->key == GLR_CONFIG_AUTO_NORMALS) {
        if (glr_state_presentation().autonormal) {
            repl_mark_normals_dirty();
            repl_set_status("Auto-normals: ON");
        } else {
            repl_set_status("Auto-normals: OFF (existing normals kept)");
        }
    } else if (item->key == GLR_CONFIG_POINT_ATTENUATION) {
        repl_apply_init_bootstrap();
        repl_set_status(glr_config_get(GLR_CONFIG_POINT_ATTENUATION) ? "Point attenuation: ON"
                                                                  : "Point attenuation: OFF");
    } else if (item->key == GLR_CONFIG_AUDIO_MODE) {
        int mode = glr_config_get(GLR_CONFIG_AUDIO_MODE);
        apply_audio_cfg_mode(mode);
        static const char *labels[] = {
            "Audio: paused",
            "Audio: play once",
            "Audio: loop song",
            "Audio: loop all",
        };
        repl_set_status(labels[mode]);
    } else if (item->state_names) {
        snprintf(cfg_status_buf, sizeof(cfg_status_buf), "%s: %s",
                 item->label, glr_config_state_name(item->key, new_value));
        repl_set_status(cfg_status_buf);
    } else if (item->state_count == 2) {
        snprintf(cfg_status_buf, sizeof(cfg_status_buf), "%s: %s",
                 item->label, new_value ? "ON" : "OFF");
        repl_set_status(cfg_status_buf);
    }
}

void glr_action_cursor_blink_reset(void) {
    UiCodePanelRuntimeState *cp = ui_state_code_panel_mut();

    cp->cursor_visible = 1;
    cp->blink_tick = 0;
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
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && !item->section_header &&
            item->is_special && item->key_code == key) {
            glr_cfg_cycle_row(i, 1);
            return 1;
        }
    }
    return 0;
}

int glr_action_menu_item_activate(int menu_id, int item_idx) {
    if (menu_id == GLR_MENU_FILE) {
        if (item_idx == GLR_FILE_ITEM_LOAD_SCENE) {
            {
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "Runtime load unsupported - relaunch %s <file> "
                         "or use Load Workspace",
                         glr_ctrl_program_name());
                repl_set_status(msg);
            }
            return 1;
        }
        if (item_idx == GLR_FILE_ITEM_SAVE_WORKSPACE) {
            const char *dir = repl_workspace_dir();
            if (!dir || !dir[0])
                dir = GLR_DEFAULT_WORKSPACE_DIR;
            ReplExportLayout layout;
            glr_ctrl_fill_export_layout(&layout);
            repl_save_workspace(dir, &layout);
            return 1;
        }
        if (item_idx == GLR_FILE_ITEM_LOAD_WORKSPACE) {
            const char *dir = repl_workspace_dir();
            if (!dir || !dir[0])
                dir = GLR_DEFAULT_WORKSPACE_DIR;
            /* Wholesale REPL state replacement — drop the undo ring so
             * a post-load Ctrl+Z can't pull a snapshot from the
             * previous workspace into the new one. */
            editor_undo_clear();
            repl_load_workspace(dir);
            return 1;
        }
        if (item_idx == GLR_FILE_ITEM_NEW_SCENE) {
            /* Not the old Scene-New body verbatim: it left the active
             * user slot attached, so the new Save Scene would overwrite
             * that slot's <slug>.c with the emptied buffer. Detach the
             * scene fully (transient lifecycle) and drop the undo ring
             * (wholesale document replacement, same as F12 / load). */
            ReplSceneRuntimeState *scenes = repl_state_scenes_mut();
            if (scenes->active_example_idx >= 0)
                scenes->active_example_idx = -1;
            repl_scenes_enter_transient_scene();
            repl_scenes_reset_for_transient();
            editor_clear_all_cmds();
            editor_undo_clear();
            return 1;
        }
        if (item_idx == GLR_FILE_ITEM_SAVE_SCENE) {
            ReplExportLayout layout;
            glr_ctrl_fill_export_layout(&layout);
            repl_save_active_scene(&layout);
            return 1;
        }
        if (item_idx == GLR_FILE_ITEM_RENAME_SCENE) {
            int slot = repl_active_user_scene();
            if (slot < 0) {
                repl_set_status("No active scene to rename");
                return 1;
            }
            editor_inline_rename_begin(slot);
            return 1;
        }
        /* GLR_FILE_ITEM_SCENE_SEP is a non-actionable "---" row (the
         * dropdown hit-test never returns it); no case needed. */
    } else if (menu_id == GLR_MENU_SCENE) {
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
        return 1;
    } else if (menu_id == GLR_MENU_TUTORIALS) {
        int tutorial_count = repl_tutorial_count();
        if (item_idx < tutorial_count) {
            tutorial_start(item_idx);
            return 1;
        }
        if (tutorial_active() && item_idx == tutorial_count + 1) {
            TutorialRuntimeState tutorial = tutorial_state_view();
            if (tutorial.tutorial_idx >= 0)
                tutorial_start(tutorial.tutorial_idx);
            return 1;
        }
        if (tutorial_active() && item_idx == tutorial_count + 2) {
            tutorial_exit();
            return 1;
        }
        return 1;
    } else if (menu_id == GLR_MENU_CONFIG) {
        /* Config top-level rows are section / "All" PARENT rows: they
         * hover-open a flyout, and a click on the parent itself is
         * inert (mirrors the MENU_SCENE tag-row guard above — plan
         * Finding #1). `item_idx` here is a section parent row, NOT a
         * g_cfg_items[] index, so it must never be cycled. Leaf
         * config-item activation arrives via UI_HIT_SUBMENU_ITEM and
         * is dispatched straight to glr_cfg_cycle_row() on the
         * absolute g_cfg_items[] index (route_submenu_item_hit,
         * Step 6) — it never reaches this branch. Return 0 so the
         * dropdown stays open, matching the old per-toggle feel. */
        (void)item_idx;
        return 0;
    }

    return 1;
}

void glr_actions_apply_defaults(void) {
    /* Restore the audio mode persisted from the previous session.
     * audio_play_playlist() calls load_state() which stores the cfg_mode
     * in the audio module; pull it here so the UI config and the actual audio
     * engine agree before the first frame. */
    int saved_mode = audio_get_cfg_mode();
    if (saved_mode < AUDIO_CFG_PAUSE || saved_mode > AUDIO_CFG_ALL)
        saved_mode = AUDIO_CFG_ALL;
    apply_audio_cfg_mode(saved_mode);
    audio_set_cfg_mode(saved_mode);
}
