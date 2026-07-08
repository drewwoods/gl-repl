/*
 * glr_state.c -- Storage and accessors for app-frame presentation/render
 * state.
 *
 * The fields here used to live on `ReplRuntimeState.{presentation,render}`.
 * Workspace-header read/write already routed through the cfg bridge
 * (`ReplConfigBridge`) and per-scene snapshots through the same
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
 * (Relocated from ReplRuntimeState to glr_state.)
 */
#include "app/glr_state.h"

#include <string.h>

#include "app/glr_defaults.h"    /* CFG_DEFAULT_* */
#include "render3d/themes.h"    /* GRID_MAJOR_*, GRID_EXTENT_*, Render3dGridTheme defaults */
#include "render3d/postprocess_filter.h" /* RENDER3D_POST_FILTER_OFF */
#include "c_compat.h"        /* STATIC_ASSERT */

/* The render defaults below seed exactly four GL_LIGHTn ids by hand, so the
 * initializer would silently under-seed if the light count ever changed. */
STATIC_ASSERT(MAX_LIGHTS == 4, "glr_state light-id seed assumes MAX_LIGHTS == 4");

static const float g_grid_major_steps[GRID_MAJOR_COUNT] = {
    [GRID_MAJOR_1]  = 1.0f,
    [GRID_MAJOR_2]  = 2.0f,
    [GRID_MAJOR_5]  = 5.0f,
    [GRID_MAJOR_10] = 10.0f,
};
static const float g_grid_extents[GRID_EXTENT_COUNT] = {
    [GRID_EXTENT_CLOSE] = 5.0f,
    [GRID_EXTENT_MID]   = 30.0f,
    [GRID_EXTENT_FAR]   = 60.0f,
};

#define GLR_STATE_DEFAULTS_INITIALIZER { \
    .presentation = { \
        .wireframe              = CFG_DEFAULT_WIREFRAME, \
        .grid_theme             = CFG_DEFAULT_GRID_THEME, \
        .grid_major_idx         = CFG_DEFAULT_GRID_MAJOR_IDX, \
        .grid_extent_idx        = CFG_DEFAULT_GRID_EXTENT_IDX, \
        .grid_brightness_idx    = CFG_DEFAULT_GRID_BRIGHTNESS_IDX, \
        .axes_theme             = CFG_DEFAULT_AXES_THEME, \
        .show_vertex_labels     = CFG_DEFAULT_VERTEX_LABELS, \
        .overlay_scope           = CFG_DEFAULT_OVERLAY_SCOPE, \
        .show_normal_vectors    = CFG_DEFAULT_NORMAL_VECTORS, \
        .show_vertex_indices    = CFG_DEFAULT_VERTEX_INDICES, \
        .show_vertex_outlines   = CFG_DEFAULT_VERTEX_OUTLINES, \
        .show_vertex_points     = CFG_DEFAULT_VERTEX_POINTS, \
        .xform_guide_mode       = CFG_DEFAULT_XFORM_GUIDE_MODE, \
        .autonormal             = CFG_DEFAULT_AUTONORMAL, \
        .show_light_indicators  = CFG_DEFAULT_LIGHT_INDICATORS, \
        .light_theme            = CFG_DEFAULT_LIGHT_THEME, \
        .backdrop_mode          = CFG_DEFAULT_BACKDROP_MODE, \
        .post_fx_scope          = GLR_POST_FX_SCOPE_OFF, \
        .post_fx_effect         = GLR_POST_FX_EFFECT_CHROMATIC_ABERRATION, \
        .post_filter_mode       = RENDER3D_POST_FILTER_OFF, \
        .compositor_filter_mode = RENDER3D_POST_FILTER_OFF, \
        .highlight_current_poly = CFG_DEFAULT_HIGHLIGHT_POLY, \
        .ortho_mode             = CFG_DEFAULT_ORTHO_MODE, \
        .projection_ortho       = CFG_DEFAULT_PROJECTION, \
        .wrap_at_comma          = CFG_DEFAULT_WRAP_AT_COMMA, \
        .code_panel_layout      = CFG_DEFAULT_CODE_PANEL_LAYOUT, \
        .syntax_highlight       = CFG_DEFAULT_SYNTAX_HIGHLIGHT, \
        .code_focus             = CFG_DEFAULT_CODE_FOCUS, \
        .paren_match            = CFG_DEFAULT_PAREN_MATCH, \
        .paren_scope            = CFG_DEFAULT_PAREN_SCOPE, \
    }, \
    .render = { \
        .use_accum                 = CFG_DEFAULT_USE_ACCUM, \
        .accum_effect              = CFG_DEFAULT_ACCUM_EFFECT, \
        .accum_passes              = CFG_DEFAULT_ACCUM_PASSES, \
        .multisample_enabled       = CFG_DEFAULT_MULTISAMPLE, \
        .line_smooth_enabled       = CFG_DEFAULT_LINE_SMOOTH, \
        .point_attenuation_enabled = CFG_DEFAULT_ATTENUATE_POINTS, \
        /* Seed the stable GL_LIGHTn ids; positions/colors/eye-space are \
         * filled by render3d_lights_apply_theme when the controller applies \
         * the active light theme (at init and on every example reset). */ \
        .lights = { { .id = GL_LIGHT0 }, { .id = GL_LIGHT1 }, \
                    { .id = GL_LIGHT2 }, { .id = GL_LIGHT3 } }, \
    }, \
}

static const GlrState g_glr_state_defaults = GLR_STATE_DEFAULTS_INITIALIZER;

static GlrState g_glr_state = GLR_STATE_DEFAULTS_INITIALIZER;

#undef GLR_STATE_DEFAULTS_INITIALIZER


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
     * wrap_at_comma) keep their current values
     * across example switches. */
    GlrPresentationState *p = &g_glr_state.presentation;
    p->wireframe             = CFG_DEFAULT_WIREFRAME;
    p->grid_theme            = CFG_DEFAULT_GRID_THEME;
    p->grid_major_idx        = CFG_DEFAULT_GRID_MAJOR_IDX;
    p->grid_extent_idx       = CFG_DEFAULT_GRID_EXTENT_IDX;
    p->grid_brightness_idx   = CFG_DEFAULT_GRID_BRIGHTNESS_IDX;
    p->axes_theme            = CFG_DEFAULT_AXES_THEME;
    p->show_vertex_labels    = CFG_DEFAULT_VERTEX_LABELS;
    p->overlay_scope         = CFG_DEFAULT_OVERLAY_SCOPE;
    p->show_vertex_indices   = CFG_DEFAULT_VERTEX_INDICES;
    p->show_normal_vectors   = CFG_DEFAULT_NORMAL_VECTORS;
    p->show_vertex_outlines  = CFG_DEFAULT_VERTEX_OUTLINES;
    p->show_vertex_points    = CFG_DEFAULT_VERTEX_POINTS;
    p->highlight_current_poly = CFG_DEFAULT_HIGHLIGHT_POLY;
    p->xform_guide_mode      = CFG_DEFAULT_XFORM_GUIDE_MODE;
    p->show_light_indicators = CFG_DEFAULT_LIGHT_INDICATORS;
    p->light_theme           = CFG_DEFAULT_LIGHT_THEME;
    p->backdrop_mode         = CFG_DEFAULT_BACKDROP_MODE;
    p->ortho_mode            = CFG_DEFAULT_ORTHO_MODE;
    /* Reset alongside ortho_mode so the projection choice doesn't leak
     * across example loads in the F12 cycle (an example whose @cfg omits
     * `projection` gets the default perspective). */
    p->projection_ortho      = CFG_DEFAULT_PROJECTION;
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
