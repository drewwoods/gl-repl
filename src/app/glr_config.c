#include "app/glr_config.h"
#include <string.h>
#include "app/glr_audio.h"
#include "app/glr_camera.h"
#include "app/glr_state.h"           /* presentation + render storage */
#include "repl/state_owners.h"
#include "ui/app/state_types.h"
#include "subsystems/variable_panel/variable_panel_state.h"
#include "subsystems/tutorial/tutorial.h"        /* tutorial_notify_state_changed */

/* Camera, profile_panel slices live on UiState; variable_panel
 * visibility lives on the variable_panel peer; replay state lives
 * on the replay peer. repl_*.c is not allowed to include ui_state.h
 * per check-controller-boundaries, so the relevant accessors are
 * forward-declared inline. */
GlrCameraState         *glr_camera_mut(void);
UiProfilePanelState   *ui_state_profile_panel_mut(void);
UiVariablePanelState  *variable_panel_view_mut(void);
ReplReplayRuntimeState  *replay_state_mut(void);

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Accum AA is presented as a single Off->2x->4x->8x->16x cycle that
 * drives the two underlying render fields (accum_aa_enabled +
 * accum_samples). Index 0 == Off; k_accum_steps[0] is unused.
 * Mirrors the audio-cfg collapse: config_value_ptr() returns NULL for
 * this key and glr_config_get/set special-case it. */
#define ACCUM_AA_STATE_COUNT 5
static const int k_accum_steps[ACCUM_AA_STATE_COUNT] = { 0, 2, 4, 8, 16 };

static int accum_aa_get_cycle(void) {
    if (!glr_state_render_mut()->accum_aa_enabled ||
        glr_state_render_mut()->accum_samples <= 1)
        return 0;
    int samples = glr_state_render_mut()->accum_samples;
    for (int i = 1; i < ACCUM_AA_STATE_COUNT; i++)
        if (samples <= k_accum_steps[i])
            return i;
    return ACCUM_AA_STATE_COUNT - 1;
}

static void accum_aa_set_cycle(int value) {
    if (value <= 0) {
        glr_state_render_mut()->accum_aa_enabled = 0;
        return;
    }
    if (value >= ACCUM_AA_STATE_COUNT)
        value = ACCUM_AA_STATE_COUNT - 1;
    glr_state_render_mut()->accum_aa_enabled = 1;
    glr_state_render_mut()->accum_samples = k_accum_steps[value];
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
    case GLR_CONFIG_ACCUM_AA:            return NULL; /* cycle: see accum_aa_*_cycle */
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
    case GLR_CONFIG_ORTHO_MODE:          return &glr_state_presentation_mut()->ortho_mode;
    case GLR_CONFIG_CAMERA_ROTATE:       return &glr_camera_mut()->auto_rotate;
    case GLR_CONFIG_FOCUS_ORIGIN:        return NULL; /* action row: no backing state */
    case GLR_CONFIG_RESET_CAMERA:        return NULL; /* action row: no backing state */
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
        return glr_audio_get_cfg_mode();
    if (key == GLR_CONFIG_ACCUM_AA)
        return accum_aa_get_cycle();

    int *value = config_value_ptr(key);
    return value ? *value : 0;
}

/* First g_cfg_items[] row whose key matches, or NULL. Section-header
 * rows carry GLR_CONFIG_NONE so they never match a real key; callers
 * still guard ->section_header (their miss return differs). */
static const GlrConfigItem *find_item_by_key(GlrConfigKey key) {
    for (int item_idx = 0; item_idx < CFG_ITEM_COUNT; item_idx++)
        if (g_cfg_items[item_idx].key == key)
            return &g_cfg_items[item_idx];
    return NULL;
}

int glr_config_state_count(GlrConfigKey key) {
    const GlrConfigItem *item = find_item_by_key(key);
    if (!item || item->section_header)
        return 0;
    return item->state_count;
}

const char *glr_config_state_name(GlrConfigKey key, int value) {
    const GlrConfigItem *item = find_item_by_key(key);
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
        glr_audio_set_cfg_mode(value);
    } else if (key == GLR_CONFIG_ACCUM_AA) {
        accum_aa_set_cycle(value);
    } else {
        int *target = config_value_ptr(key);
        if (!target)
            return;  /* unknown key — nothing changed, no notify */
        *target = value;
    }

    /* REQUIRE-step listener — slug-scoped and inactive-checked, so this
     * is a no-op except when a tutorial is waiting on this key's slug.
     * Placing the hook in the lowest-level setter catches every write
     * path (glr_cfg_cycle_row's normal AND early-return branches, the
     * direct accum-AA set in glr_ctrl.c's Ctrl+=/- handler, and the
     * config-bridge's apply during @cfg / example / workspace load). */
    tutorial_notify_state_changed();
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

/* ---- Section model over g_cfg_items[] -------------------------------- */

static int row_is_section_header(int idx) {
    const GlrConfigItem *it = &g_cfg_items[idx];
    return it->label && strncmp(it->label, "### ", 4) == 0;
}

GlrConfigRowKind glr_config_row_kind(int idx) {
    if (idx < 0 || idx >= CFG_ITEM_COUNT)
        return GLR_CFG_ROW_ITEM;
    if (row_is_section_header(idx))
        return GLR_CFG_ROW_HEADER;
    if (g_cfg_items[idx].section_header)
        return GLR_CFG_ROW_SEPARATOR;   /* "---" (or any non-"### " chrome) */
    return GLR_CFG_ROW_ITEM;
}

int glr_config_section_count(void) {
    int n = 0;
    for (int i = 0; i < CFG_ITEM_COUNT; i++)
        if (row_is_section_header(i))
            n++;
    return n;
}

/* Index in g_cfg_items[] of the Nth "### " header, or -1. */
static int section_header_index(int section) {
    int seen = 0;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        if (row_is_section_header(i)) {
            if (seen == section)
                return i;
            seen++;
        }
    }
    return -1;
}

const char *glr_config_section_label(int section) {
    int h = section_header_index(section);
    if (h < 0)
        return NULL;
    return g_cfg_items[h].label + 4;   /* strip the "### " marker */
}

int glr_config_section_range(int section, int *start, int *count) {
    int h = section_header_index(section);
    if (h < 0)
        return 0;

    int s = h + 1;
    int e = s;
    while (e < CFG_ITEM_COUNT && !g_cfg_items[e].section_header)
        e++;

    if (start) *start = s;
    if (count) *count = e - s;
    return 1;
}
