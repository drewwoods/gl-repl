/*
 * glr_state.c -- Storage and accessors for app-frame presentation/render
 * state.
 *
 * The fields here used to live on `ReplRuntimeState.{presentation,render}`.
 * Workspace-header read/write already routed through the cfg bridge
 * (`ReplExportConfigBridge`) and per-scene snapshots through the same
 * opaque bag, so the storage relocation is mechanical: the bridge keeps
 * using `glr_config_get/set`, which now points at this struct via
 * `glr_config.c::config_value_ptr`.
 *
 * Defaults come from `glr_defaults.h` (CFG_DEFAULT_*) which already
 * documents itself as controller-side scene/presentation defaults.
 *
 * `src/repl/state.c` no longer owns these fields and no longer references
 * `glr_camera`; the `auto_rotate` reset moved to the camera bridge's
 * `apply` callback (driven via the bridge during scene-cfg restore).
 *
 * (Cfg-bridge routing landed as step 4 of the decouple plan;
 * this storage relocation is step 7a of
 * feature/decouple-repl-from-gl-repl-alt.md.)
 */
#include "app/glr_state.h"

#include <string.h>

#include "app/glr_defaults.h"    /* CFG_DEFAULT_* */
#include "scene/themes.h"    /* GRID_MAJOR_*, GRID_EXTENT_*, SceneGridTheme defaults */
#include "scene/postprocess_filter.h" /* SCENE_POST_FILTER_OFF */
#include "ui/core/layout.h"       /* CODE_PANEL_LAYOUT_* */

static const float g_grid_major_steps[GRID_MAJOR_COUNT] = {
    [GRID_MAJOR_1]  = 1.0f,
    [GRID_MAJOR_2]  = 2.0f,
    [GRID_MAJOR_5]  = 5.0f,
    [GRID_MAJOR_10] = 10.0f,
};
static const float g_grid_extents[GRID_EXTENT_COUNT] = {
    [GRID_EXTENT_CLOSE] = 5.0f,
    [GRID_EXTENT_MID]   = 25.0f,
    [GRID_EXTENT_FAR]   = 100.0f,
};

static const GlrState g_glr_state_defaults = {
    .presentation = {
        .wireframe              = CFG_DEFAULT_WIREFRAME,
        .grid_theme             = CFG_DEFAULT_GRID_THEME,
        .grid_major_idx         = CFG_DEFAULT_GRID_MAJOR_IDX,
        .grid_extent_idx        = CFG_DEFAULT_GRID_EXTENT_IDX,
        .axes_theme             = CFG_DEFAULT_AXES_THEME,
        .show_vertex_labels     = CFG_DEFAULT_VERTEX_LABELS,
        .show_normal_vectors    = CFG_DEFAULT_NORMAL_VECTORS,
        .show_vertex_indices    = CFG_DEFAULT_VERTEX_INDICES,
        .show_vertex_outlines   = CFG_DEFAULT_VERTEX_OUTLINES,
        .show_vertex_points     = CFG_DEFAULT_VERTEX_POINTS,
        .show_vertex_guides     = CFG_DEFAULT_VERTEX_GUIDES,
        .xform_guide_mode       = CFG_DEFAULT_XFORM_GUIDE_MODE,
        .autonormal             = 0,
        .show_light_indicators  = CFG_DEFAULT_LIGHT_INDICATORS,
        .backdrop_mode          = CFG_DEFAULT_BACKDROP_MODE,
        .post_filter_mode       = SCENE_POST_FILTER_OFF,
        .highlight_current_poly = 1,
        .ortho_mode             = CFG_DEFAULT_ORTHO_MODE,
        .wrap_at_comma          = CFG_DEFAULT_WRAP_AT_COMMA,
        .code_panel_layout      = CFG_DEFAULT_CODE_PANEL_LAYOUT,
        .syntax_highlight       = CFG_DEFAULT_SYNTAX_HIGHLIGHT,
        .code_focus             = CFG_DEFAULT_CODE_FOCUS,
    },
    .render = {
        .use_accum                 = 1,
        .accum_aa_enabled          = 1,
        .accum_samples             = 2,
        .accum_jitter_x            = 0.0f,
        .accum_jitter_y            = 0.0f,
        .multisample_enabled       = CFG_DEFAULT_MULTISAMPLE,
        .msaa_samples              = 0,
        .line_smooth_enabled       = CFG_DEFAULT_LINE_SMOOTH,
        .point_attenuation_enabled = CFG_DEFAULT_ATTENUATE_POINTS,
    },
};

static GlrState g_glr_state;

GlrPresentationState glr_state_presentation(void) {
    return g_glr_state.presentation;
}

GlrPresentationState *glr_state_presentation_mut(void) {
    return &g_glr_state.presentation;
}

GlrRenderState glr_state_render(void) {
    return g_glr_state.render;
}

GlrRenderState *glr_state_render_mut(void) {
    return &g_glr_state.render;
}

void glr_state_presentation_reset_defaults(void) {
    g_glr_state.presentation = g_glr_state_defaults.presentation;
}

void glr_state_presentation_reset_example_defaults(void) {
    /* Reset the scene-bound subset plus ortho_mode. The scene-subset
     * roster mirrors the cfg-bridge `fill_scene_subset` whitelist in
     * glr_actions.c; ortho_mode is reset alongside it (rather than in
     * the subset proper) so the 2D/3D view doesn't leak across example
     * loads in the F12 cycle — an example whose @cfg omits `view_mode`
     * gets the default 3D, not whatever the prior example set.
     * Fields outside this reset (autonormal, code_panel_layout,
     * wrap_at_comma, highlight_current_poly) keep their current values
     * across example switches. */
    GlrPresentationState *p = &g_glr_state.presentation;
    p->wireframe             = CFG_DEFAULT_WIREFRAME;
    p->grid_theme            = CFG_DEFAULT_GRID_THEME;
    p->grid_major_idx        = CFG_DEFAULT_GRID_MAJOR_IDX;
    p->grid_extent_idx       = CFG_DEFAULT_GRID_EXTENT_IDX;
    p->axes_theme            = CFG_DEFAULT_AXES_THEME;
    p->show_vertex_labels    = CFG_DEFAULT_VERTEX_LABELS;
    p->show_vertex_indices   = CFG_DEFAULT_VERTEX_INDICES;
    p->show_normal_vectors   = CFG_DEFAULT_NORMAL_VECTORS;
    p->show_vertex_outlines  = CFG_DEFAULT_VERTEX_OUTLINES;
    p->show_vertex_points    = CFG_DEFAULT_VERTEX_POINTS;
    p->show_vertex_guides    = CFG_DEFAULT_VERTEX_GUIDES;
    p->xform_guide_mode      = CFG_DEFAULT_XFORM_GUIDE_MODE;
    p->show_light_indicators = CFG_DEFAULT_LIGHT_INDICATORS;
    p->backdrop_mode         = CFG_DEFAULT_BACKDROP_MODE;
    p->ortho_mode            = CFG_DEFAULT_ORTHO_MODE;
}

void glr_state_render_reset_defaults(void) {
    g_glr_state.render = g_glr_state_defaults.render;
}

void glr_state_capture(GlrState *snapshot) {
    if (!snapshot) return;
    *snapshot = g_glr_state;
}

void glr_state_restore(const GlrState *snapshot) {
    if (!snapshot) return;
    g_glr_state = *snapshot;
}

const float *glr_state_grid_major_steps(void) {
    return g_grid_major_steps;
}

const float *glr_state_grid_extents(void) {
    return g_grid_extents;
}
