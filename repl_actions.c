/*
 * repl_actions.c -- Side-effecting editor actions and config dispatch.
 *
 * Input modules decide which key or menu row was activated. UI modules decide
 * what was clicked. This module owns the mutation that follows: config-row
 * cycling, F-key/Ctrl-key config shortcuts, startup config defaults, and menu
 * item actions that touch scenes, files, replay, audio, or presentation state.
 */
#include "sample.h"
#include "repl_actions.h"
#include "repl_audio.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_config.h"
#include "repl_keys.h"
#include "repl_state.h"
#include "ui_panels.h"
#include "repl_inline_rename.h"

static const char *replay_mode_names[] = { "Polygon", "Vertex" };
static const char *backdrop_mode_names[] = { "Off", "Cityscape" };
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

const ReplConfigItem g_cfg_items[] = {
    { "### RENDERING",     0, 0,  REPL_CONFIG_NONE,               0, NULL,                 1 },
    { "MSAA",              KEY_CTRL_U, 0, REPL_CONFIG_MSAA,       2, NULL,                 0 },
    { "Line smooth",       0, 0,  REPL_CONFIG_LINE_SMOOTH,        2, NULL,                 0 },
    { "Accum AA",          0, 0,  REPL_CONFIG_ACCUM_AA,           2, NULL,                 0 },
    { "Wireframe",         GLUT_KEY_F2, 1, REPL_CONFIG_WIREFRAME, 2, NULL,                 0 },
    { "Point attenuation",  0, 0,  REPL_CONFIG_POINT_ATTENUATION,  2, NULL,                 0 },
    { "---",               0, 0,  REPL_CONFIG_NONE,               0, NULL,                 1 },
    { "### TIME & REPLAY",  0, 0,  REPL_CONFIG_NONE,               0, NULL,                 1 },
    { "Auto time",         KEY_CTRL_T, 0, REPL_CONFIG_AUTO_TIME,   2, NULL,                 0 },
    { "Replay",            KEY_CTRL_R, 0, REPL_CONFIG_REPLAY,      2, NULL,                 0 },
    { "Replay mode",       0, 0,  REPL_CONFIG_REPLAY_MODE,         2, replay_mode_names,    0 },
    { "Replay expand",     0, 0,  REPL_CONFIG_REPLAY_EXPAND,       2, NULL,                 0 },
    { "---",               0, 0,  REPL_CONFIG_NONE,               0, NULL,                 1 },
    { "### OVERLAYS & SCENE", 0, 0, REPL_CONFIG_NONE,              0, NULL,                 1 },
    { "Grid",              GLUT_KEY_F3, 1, REPL_CONFIG_GRID_THEME,  GRID_THEME_COUNT, grid_theme_names, 0 },
    { "Grid major",        KEY_CTRL_O, 0, REPL_CONFIG_GRID_MAJOR,  GRID_MAJOR_COUNT, grid_major_names, 0 },
    { "Grid extent",       0, 0,  REPL_CONFIG_GRID_EXTENT,         GRID_EXTENT_COUNT, grid_extent_names, 0 },
    { "Axes",              GLUT_KEY_F4, 1, REPL_CONFIG_AXES_THEME,  AXES_THEME_COUNT, axes_theme_names,    0 },
    { "Vertex guides",     GLUT_KEY_F8, 1, REPL_CONFIG_VERTEX_GUIDES, 2, NULL,             0 },
    { "Xform guide mode",  0, 0,  REPL_CONFIG_XFORM_GUIDE_MODE,     2, xform_guide_mode_names, 0 },
    { "Light indicators",  GLUT_KEY_F10, 1, REPL_CONFIG_LIGHT_INDICATORS, 2, NULL,        0 },
    { "Poly highlight",    0, 0,  REPL_CONFIG_POLY_HIGHLIGHT,       2, NULL,                 0 },
    { "Backdrop",          0, 0,  REPL_CONFIG_BACKDROP,             2, backdrop_mode_names, 0 },
    { "Camera rotate",     GLUT_KEY_F11, 1, REPL_CONFIG_CAMERA_ROTATE, 2, NULL,            0 },
    { "Auto-normals",      GLUT_KEY_F9, 1, REPL_CONFIG_AUTO_NORMALS, 2, NULL,              0 },
    { "---",               0, 0,  REPL_CONFIG_NONE,               0, NULL,                 1 },
    { "### GEOMETRY",      0, 0,  REPL_CONFIG_NONE,               0, NULL,                 1 },
    { "Vertex labels",     GLUT_KEY_F5, 1, REPL_CONFIG_VERTEX_LABELS, 2, NULL,             0 },
    { "Normal vectors",    GLUT_KEY_F6, 1, REPL_CONFIG_NORMAL_VECTORS, 2, NULL,            0 },
    { "Vertex outlines",   GLUT_KEY_F7, 1, REPL_CONFIG_VERTEX_OUTLINES, 2, NULL,           0 },
    { "Vertex points",     0, 0,  REPL_CONFIG_VERTEX_POINTS,        2, NULL,                 0 },
    { "---",               0, 0,  REPL_CONFIG_NONE,               0, NULL,                 1 },
    { "### INTERFACE",     0, 0,  REPL_CONFIG_NONE,               0, NULL,                 1 },
    { "Variable panel",    0, 0,  REPL_CONFIG_VARIABLE_PANEL,      2, NULL,                 0 },
    { "CPU profile",       KEY_CTRL_W, 0, REPL_CONFIG_CPU_PROFILE,  PROFILE_PANEL_MODE_COUNT, profile_panel_mode_names, 0 },
    { "Code panel",        KEY_CTRL_B, 0, REPL_CONFIG_CODE_PANEL_LAYOUT, CODE_PANEL_LAYOUT_COUNT, code_panel_layout_names, 0 },
    { "Wrap at commas",    0, 0,  REPL_CONFIG_WRAP_AT_COMMA,       2, NULL,                 0 },
    { "---",               0, 0,  REPL_CONFIG_NONE,               0, NULL,                 1 },
    { "### AUDIO",         0, 0,  REPL_CONFIG_NONE,               0, NULL,                 1 },
    { "Audio",             0, 0,  REPL_CONFIG_AUDIO_MODE,          4, audio_cfg_names,      0 },
};

const int CFG_ITEM_COUNT = (int)(sizeof(g_cfg_items) / sizeof(g_cfg_items[0]));

static void apply_audio_cfg_mode(int mode) {
    switch (mode) {
    case AUDIO_CFG_PAUSE:
        repl_audio_set_paused(1);
        break;
    case AUDIO_CFG_ONCE:
        repl_audio_set_paused(0);
        repl_audio_set_loop_mode(REPL_AUDIO_LOOP_OFF);
        break;
    case AUDIO_CFG_SONG:
        repl_audio_set_paused(0);
        repl_audio_set_loop_mode(REPL_AUDIO_LOOP_SONG);
        break;
    case AUDIO_CFG_ALL:
    default:
        repl_audio_set_paused(0);
        repl_audio_set_loop_mode(REPL_AUDIO_LOOP_ALL);
        break;
    }
}

int repl_scene_menu_slot_for_dense_index(int scene_idx) {
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

void repl_cfg_cycle_row(int row, int delta) {
    const ReplConfigItem *item = repl_config_item_at(row);

    if (!item || item->section_header)
        return;

    /* Replay is special-cased: its cfg toggle kicks off/ends the replay
     * machinery rather than flipping the int directly. Both directions
     * collapse to "toggle". */
    if (item->key == REPL_CONFIG_REPLAY) {
        if (repl_config_get(REPL_CONFIG_REPLAY)) {
            replay_stop();
            set_status("Replay: off");
        } else {
            replay_start();
        }
        return;
    }

    if (item->key == REPL_CONFIG_AUTO_TIME) {
        if (glutGetModifiers() & GLUT_ACTIVE_SHIFT) {
            repl_reset_time_to_zero();
            set_status(*repl_state_variables()->time_playing ? "Time: reset to 0"
                                                             : "Time: reset to 0 (paused)");
            return;
        }
    }

    if (*repl_state_replay()->active)
        replay_stop();

    int v = repl_config_cycle(item->key, delta);

    if (item->key == REPL_CONFIG_CODE_PANEL_LAYOUT) {
        *repl_state_code_panel()->panel_frac = 0.3f;
        if (*repl_state_presentation()->code_panel_layout == CODE_PANEL_LAYOUT_TOP) {
            set_status("Layout: top code panel");
        } else if (*repl_state_presentation()->code_panel_layout == CODE_PANEL_LAYOUT_BOTTOM) {
            set_status("Layout: bottom code panel");
        } else if (*repl_state_presentation()->code_panel_layout == CODE_PANEL_LAYOUT_HIDDEN) {
            ui_panels_close_menus();
            clear_autocomplete_state();
            set_status("Layout: code panel hidden");
        } else {
            set_status("Layout: left code panel");
        }
    } else if (item->key == REPL_CONFIG_AUTO_NORMALS) {
        if (*repl_state_presentation()->autonormal) {
            mark_normals_dirty();
            set_status("Auto-normals: ON");
        } else {
            set_status("Auto-normals: OFF (existing normals kept)");
        }
    } else if (item->key == REPL_CONFIG_POINT_ATTENUATION) {
        apply_init_bootstrap();
        set_status(repl_config_get(REPL_CONFIG_POINT_ATTENUATION) ? "Point attenuation: ON"
                                                                  : "Point attenuation: OFF");
    } else if (item->key == REPL_CONFIG_AUDIO_MODE) {
        int mode = repl_config_get(REPL_CONFIG_AUDIO_MODE);
        apply_audio_cfg_mode(mode);
        static const char *labels[] = {
            "Audio: paused",
            "Audio: play once",
            "Audio: loop song",
            "Audio: loop all",
        };
        set_status(labels[mode]);
    } else if (item->state_names) {
        snprintf(cfg_status_buf, sizeof(cfg_status_buf), "%s: %s",
                 item->label, repl_config_state_name(item->key, v));
        set_status(cfg_status_buf);
    } else if (item->state_count == 2) {
        snprintf(cfg_status_buf, sizeof(cfg_status_buf), "%s: %s",
                 item->label, v ? "ON" : "OFF");
        set_status(cfg_status_buf);
    }
}

int repl_cfg_handle_ascii_shortcut(unsigned char key) {
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const ReplConfigItem *item = repl_config_item_at(i);
        if (item && !item->section_header &&
            !item->is_special &&
            item->key_code > 0 &&
            item->key_code < 32 &&
            item->key_code == key) {
            repl_cfg_cycle_row(i, 1);
            return 1;
        }
    }
    return 0;
}

int repl_cfg_handle_special_shortcut(int key) {
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const ReplConfigItem *item = repl_config_item_at(i);
        if (item && !item->section_header &&
            item->is_special && item->key_code == key) {
            repl_cfg_cycle_row(i, 1);
            return 1;
        }
    }
    return 0;
}

int repl_action_menu_item_activate(int menu_id, int item_idx) {
    if (menu_id == REPL_MENU_FILE) {
        if (item_idx == REPL_FILE_ITEM_EXPORT) {
            repl_save_default_output();
            return 1;
        }
        if (item_idx == REPL_FILE_ITEM_IMPORT) {
            set_status("Import not implemented yet");
            return 1;
        }
        if (item_idx == REPL_FILE_ITEM_SAVE_WORKSPACE) {
            const char *dir = repl_workspace_dir();
            if (!dir || !dir[0])
                dir = REPL_DEFAULT_WORKSPACE_DIR;
            repl_save_workspace(dir);
            return 1;
        }
        if (item_idx == REPL_FILE_ITEM_LOAD_WORKSPACE) {
            const char *dir = repl_workspace_dir();
            if (!dir || !dir[0])
                dir = REPL_DEFAULT_WORKSPACE_DIR;
            repl_load_workspace(dir);
            return 1;
        }
    } else if (menu_id == REPL_MENU_SCENE) {
        int example_count = repl_example_count();
        if (item_idx >= 1 && item_idx <= example_count) {
            repl_load_example(item_idx - 1);
            return 1;
        }
        if (item_idx == example_count + REPL_SCENE_OFF_NEW) {
            int *active_example_idx = repl_state_scenes_mut()->active_example_idx;
            if (*active_example_idx >= 0)
                *active_example_idx = -1;
            repl_clear_all_cmds();
            return 1;
        }
        if (item_idx == example_count + REPL_SCENE_OFF_SAVE) {
            repl_save_default_output();
            return 1;
        }
        if (item_idx == example_count + REPL_SCENE_OFF_RENAME) {
            int slot = repl_active_user_scene();
            if (slot < 0) {
                set_status("No active scene to rename");
                return 1;
            }
            repl_inline_rename_begin(slot);
            return 1;
        }

        int scene_idx = item_idx - (example_count + REPL_SCENE_OFF_SCENES);
        if (scene_idx >= 0 && scene_idx < repl_user_scene_count()) {
            int slot = repl_scene_menu_slot_for_dense_index(scene_idx);
            if (slot >= 0) {
                repl_load_user_scene_idx(slot);
                return 1;
            }
        }
        return 1;
    } else if (menu_id == REPL_MENU_CONFIG) {
        if (repl_config_item_at(item_idx) &&
            !repl_config_item_at(item_idx)->section_header) {
            repl_cfg_cycle_row(item_idx, 1);
        }
        return 0;
    }

    return 1;
}

void repl_actions_apply_defaults(void) {
    /* Restore the audio mode persisted from the previous session.
     * repl_audio_play_playlist() calls load_state() which stores the cfg_mode
     * in the audio module; pull it here so the UI config and the actual audio
     * engine agree before the first frame. */
    int saved_mode = repl_audio_get_cfg_mode();
    if (saved_mode < AUDIO_CFG_PAUSE || saved_mode > AUDIO_CFG_ALL)
        saved_mode = AUDIO_CFG_ALL;
    apply_audio_cfg_mode(saved_mode);
    repl_audio_set_cfg_mode(saved_mode);
}
