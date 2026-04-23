#include "repl_config.h"

#include "repl_audio.h"
#include "repl_state_compat.h"

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

const ReplConfigItem *repl_config_items(int *count) {
    if (count)
        *count = CFG_ITEM_COUNT;
    return g_cfg_items;
}

const ReplConfigItem *repl_config_item_at(int idx) {
    if (idx < 0 || idx >= CFG_ITEM_COUNT)
        return NULL;
    return &g_cfg_items[idx];
}

static int *config_value_ptr(ReplConfigKey key) {
    switch (key) {
    case REPL_CONFIG_MSAA:                return &g_multisample_enabled;
    case REPL_CONFIG_LINE_SMOOTH:         return &g_line_smooth_enabled;
    case REPL_CONFIG_ACCUM_AA:            return &g_accum_aa_enabled;
    case REPL_CONFIG_WIREFRAME:           return &g_wireframe;
    case REPL_CONFIG_POINT_ATTENUATION:   return &g_init_attenuate_points;
    case REPL_CONFIG_AUTO_TIME:           return &g_t_playing;
    case REPL_CONFIG_REPLAY:              return &g_replay_active;
    case REPL_CONFIG_REPLAY_MODE:         return &g_replay_mode;
    case REPL_CONFIG_REPLAY_EXPAND:       return &g_replay_expand_args;
    case REPL_CONFIG_GRID_THEME:          return &g_grid_theme;
    case REPL_CONFIG_GRID_MAJOR:          return &g_grid_major_idx;
    case REPL_CONFIG_GRID_EXTENT:         return &g_grid_extent_idx;
    case REPL_CONFIG_AXES_THEME:          return &g_axes_theme;
    case REPL_CONFIG_VERTEX_GUIDES:       return &g_show_guides;
    case REPL_CONFIG_XFORM_GUIDE_MODE:    return &g_xform_guide_mode;
    case REPL_CONFIG_LIGHT_INDICATORS:    return &g_show_lights;
    case REPL_CONFIG_POLY_HIGHLIGHT:      return &g_highlight_current_poly;
    case REPL_CONFIG_BACKDROP:            return &g_backdrop_mode;
    case REPL_CONFIG_CAMERA_ROTATE:       return repl_state_camera_mut()->auto_rotate;
    case REPL_CONFIG_AUTO_NORMALS:        return &g_autonormal;
    case REPL_CONFIG_VERTEX_LABELS:       return &g_show_vnums;
    case REPL_CONFIG_NORMAL_VECTORS:      return &g_show_normals;
    case REPL_CONFIG_VERTEX_OUTLINES:     return &g_show_outlines;
    case REPL_CONFIG_VERTEX_POINTS:       return &g_show_vpoints;
    case REPL_CONFIG_VARIABLE_PANEL:      return &g_show_var_panel;
    case REPL_CONFIG_CPU_PROFILE:         return &g_show_profile_panel;
    case REPL_CONFIG_CODE_PANEL_LAYOUT:   return &g_code_panel_layout;
    case REPL_CONFIG_WRAP_AT_COMMA:       return &g_wrap_at_comma;
    case REPL_CONFIG_AUDIO_MODE:          return NULL; /* audio module owns this one */
    case REPL_CONFIG_NONE:
    case REPL_CONFIG_COUNT:
    default:
        return NULL;
    }
}

int repl_config_get(ReplConfigKey key) {
    if (key == REPL_CONFIG_AUDIO_MODE)
        return repl_audio_get_cfg_mode();

    int *value = config_value_ptr(key);
    return value ? *value : 0;
}

int repl_config_state_count(ReplConfigKey key) {
    const ReplConfigItem *item = NULL;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        if (g_cfg_items[i].key == key) {
            item = &g_cfg_items[i];
            break;
        }
    }
    if (!item || item->section_header)
        return 0;
    return item->state_count;
}

const char *repl_config_state_name(ReplConfigKey key, int value) {
    const ReplConfigItem *item = NULL;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        if (g_cfg_items[i].key == key) {
            item = &g_cfg_items[i];
            break;
        }
    }
    if (!item || item->section_header)
        return NULL;

    if (item->state_names && value >= 0 && value < item->state_count)
        return item->state_names[value];
    if (item->state_count == 2)
        return value ? "ON" : "OFF";
    return NULL;
}

void repl_config_set(ReplConfigKey key, int value) {
    int state_count = repl_config_state_count(key);
    if (state_count > 0)
        value = clamp_int(value, 0, state_count - 1);

    if (key == REPL_CONFIG_AUDIO_MODE) {
        repl_audio_set_cfg_mode(value);
        return;
    }

    int *target = config_value_ptr(key);
    if (!target)
        return;

    *target = value;
}

int repl_config_cycle(ReplConfigKey key, int delta) {
    int count = repl_config_state_count(key);
    if (count <= 0)
        return repl_config_get(key);

    int value = repl_config_get(key);
    value = (value + delta) % count;
    if (value < 0)
        value += count;
    repl_config_set(key, value);
    return value;
}
