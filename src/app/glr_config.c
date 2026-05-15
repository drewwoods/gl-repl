#include "app/glr_config.h"
#include "audio.h"
#include "app/glr_camera.h"
#include "app/glr_state.h"           /* presentation + render storage (step 7a) */
#include "repl/state_owners.h"
#include "ui/state_types.h"

/* Camera, profile_panel slices live on UiState; variable_panel
 * visibility lives on the variable_panel peer; replay state lives
 * on the replay peer. repl_*.c is not allowed to include ui_state.h
 * per check-controller-boundaries, so the relevant accessors are
 * forward-declared inline. */
ReplCameraState         *glr_camera_mut(void);
ReplProfilePanelState   *ui_state_profile_panel_mut(void);
ReplVariablePanelState  *variable_panel_view_mut(void);
ReplReplayRuntimeState  *replay_state_mut(void);

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

const GlrConfigItem *glr_config_items(int *count) {
    if (count)
        *count = CFG_ITEM_COUNT;
    return g_cfg_items;
}

const GlrConfigItem *glr_config_item_at(int idx) {
    if (idx < 0 || idx >= CFG_ITEM_COUNT)
        return NULL;
    return &g_cfg_items[idx];
}

static int *config_value_ptr(GlrConfigKey key) {
    switch (key) {
    case GLR_CONFIG_MSAA:                return &glr_state_render_mut()->multisample_enabled;
    case GLR_CONFIG_LINE_SMOOTH:         return &glr_state_render_mut()->line_smooth_enabled;
    case GLR_CONFIG_ACCUM_AA:            return &glr_state_render_mut()->accum_aa_enabled;
    case GLR_CONFIG_WIREFRAME:           return &glr_state_presentation_mut()->wireframe;
    case GLR_CONFIG_POINT_ATTENUATION:   return &glr_state_render_mut()->point_attenuation_enabled;
    case GLR_CONFIG_AUTO_TIME:           return &repl_state_variables_mut()->time_playing;
    case GLR_CONFIG_REPLAY:              return &replay_state_mut()->active;
    case GLR_CONFIG_REPLAY_MODE:         return &replay_state_mut()->mode;
    case GLR_CONFIG_REPLAY_EXPAND:       return &replay_state_mut()->expand_args;
    case GLR_CONFIG_GRID_THEME:          return &glr_state_presentation_mut()->grid_theme;
    case GLR_CONFIG_GRID_MAJOR:          return &glr_state_presentation_mut()->grid_major_idx;
    case GLR_CONFIG_GRID_EXTENT:         return &glr_state_presentation_mut()->grid_extent_idx;
    case GLR_CONFIG_AXES_THEME:          return &glr_state_presentation_mut()->axes_theme;
    case GLR_CONFIG_XFORM_GUIDES:       return &glr_state_presentation_mut()->show_vertex_guides;
    case GLR_CONFIG_XFORM_GUIDE_MODE:    return &glr_state_presentation_mut()->xform_guide_mode;
    case GLR_CONFIG_LIGHT_INDICATORS:    return &glr_state_presentation_mut()->show_light_indicators;
    case GLR_CONFIG_POLY_HIGHLIGHT:      return &glr_state_presentation_mut()->highlight_current_poly;
    case GLR_CONFIG_BACKDROP:            return &glr_state_presentation_mut()->backdrop_mode;
    case GLR_CONFIG_CAMERA_ROTATE:       return &glr_camera_mut()->auto_rotate;
    case GLR_CONFIG_AUTO_NORMALS:        return &glr_state_presentation_mut()->autonormal;
    case GLR_CONFIG_VERTEX_LABELS:       return &glr_state_presentation_mut()->show_vertex_labels;
    case GLR_CONFIG_NORMAL_VECTORS:      return &glr_state_presentation_mut()->show_normal_vectors;
    case GLR_CONFIG_VERTEX_OUTLINES:     return &glr_state_presentation_mut()->show_vertex_outlines;
    case GLR_CONFIG_VERTEX_POINTS:       return &glr_state_presentation_mut()->show_vertex_points;
    case GLR_CONFIG_VARIABLE_PANEL:      return &variable_panel_view_mut()->visible;
    case GLR_CONFIG_CPU_PROFILE:         return &ui_state_profile_panel_mut()->mode;
    case GLR_CONFIG_CODE_PANEL_LAYOUT:   return &glr_state_presentation_mut()->code_panel_layout;
    case GLR_CONFIG_WRAP_AT_COMMA:       return &glr_state_presentation_mut()->wrap_at_comma;
    case GLR_CONFIG_AUDIO_MODE:          return NULL; /* audio module owns this one */
    case GLR_CONFIG_SYNTAX_HIGHLIGHT:    return &glr_state_presentation_mut()->syntax_highlight;
    case GLR_CONFIG_NONE:
    case GLR_CONFIG_COUNT:
    default:
        return NULL;
    }
}

int glr_config_get(GlrConfigKey key) {
    if (key == GLR_CONFIG_AUDIO_MODE)
        return audio_get_cfg_mode();

    int *value = config_value_ptr(key);
    return value ? *value : 0;
}

int glr_config_state_count(GlrConfigKey key) {
    const GlrConfigItem *item = NULL;
    for (int item_idx = 0; item_idx < CFG_ITEM_COUNT; item_idx++) {
        if (g_cfg_items[item_idx].key == key) {
            item = &g_cfg_items[item_idx];
            break;
        }
    }
    if (!item || item->section_header)
        return 0;
    return item->state_count;
}

const char *glr_config_state_name(GlrConfigKey key, int value) {
    const GlrConfigItem *item = NULL;
    for (int item_idx = 0; item_idx < CFG_ITEM_COUNT; item_idx++) {
        if (g_cfg_items[item_idx].key == key) {
            item = &g_cfg_items[item_idx];
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

void glr_config_set(GlrConfigKey key, int value) {
    int state_count = glr_config_state_count(key);
    if (state_count > 0)
        value = clamp_int(value, 0, state_count - 1);

    if (key == GLR_CONFIG_AUDIO_MODE) {
        audio_set_cfg_mode(value);
        return;
    }

    int *target = config_value_ptr(key);
    if (!target)
        return;

    *target = value;
}

int glr_config_cycle(GlrConfigKey key, int delta) {
    int count = glr_config_state_count(key);
    if (count <= 0)
        return glr_config_get(key);

    int value = glr_config_get(key);
    value = (value + delta) % count;
    if (value < 0)
        value += count;
    glr_config_set(key, value);
    return value;
}
