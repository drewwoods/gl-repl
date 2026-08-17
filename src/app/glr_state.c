/*
 * glr_state.c -- Storage and accessors for app-frame presentation/render
 * state.
 *
 * Workspace-header read/write routes through the cfg bridge (`ReplConfigBridge`)
 * and per-scene snapshots use the same opaque bag. The bridge uses
 * `glr_config_get/set`, which points at this struct via
 * `glr_config.c::config_value_ptr`.
 *
 * Defaults come from `glr_defaults.h` (CFG_DEFAULT_*) which already
 * documents itself as controller-side render3d/presentation defaults.
 *
 * The camera bridge's `apply` callback resets `auto_rotate` during
 * render3d-config restore.
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
        .vertex_label_placement = CFG_DEFAULT_VERTEX_LABEL_PLACEMENT, \
        .overlay_scope           = CFG_DEFAULT_OVERLAY_SCOPE, \
        .show_normal_vectors    = CFG_DEFAULT_NORMAL_VECTORS, \
        .show_vertex_outlines   = CFG_DEFAULT_VERTEX_OUTLINES, \
        .vertex_outline_style   = CFG_DEFAULT_VERTEX_OUTLINE_STYLE, \
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
        .depth_viz              = CFG_DEFAULT_DEPTH_VIZ, \
        .stencil_viz            = CFG_DEFAULT_STENCIL_VIZ, \
        .call_depth_tint        = CFG_DEFAULT_CALL_DEPTH_TINT, \
        .ortho_mode             = CFG_DEFAULT_ORTHO_MODE, \
        .projection_mode        = CFG_DEFAULT_PROJECTION, \
        .wrap_at_comma          = CFG_DEFAULT_WRAP_AT_COMMA, \
        .code_panel_layout      = CFG_DEFAULT_CODE_PANEL_LAYOUT, \
        .syntax_highlight       = CFG_DEFAULT_SYNTAX_HIGHLIGHT, \
        .code_focus             = CFG_DEFAULT_CODE_FOCUS, \
        .paren_match            = CFG_DEFAULT_PAREN_MATCH, \
        .paren_scope            = CFG_DEFAULT_PAREN_SCOPE, \
    }, \
    .render = { \
        .use_accum                 = CFG_DEFAULT_USE_ACCUM, \
        .accum_bits                = -1, \
        .stencil_bits              = -1, \
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

/* The one presentation default that is not a compile-time constant: the
 * controller probes GL_RENDERER at init and installs a renderer-appropriate
 * syntax-highlight mode here (see glr_state_set_default_syntax_highlight).
 * Held separately from g_glr_state_defaults - which stays const, so nothing
 * else can drift a "default" out from under a reset - and re-applied by
 * glr_state_presentation_reset_defaults so the verdict survives every later
 * whole-world reset, not just startup. */
static int g_default_syntax_highlight = CFG_DEFAULT_SYNTAX_HIGHLIGHT;

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
    g_glr_state.presentation.syntax_highlight = g_default_syntax_highlight;
}

void glr_state_set_default_syntax_highlight(int mode) {
    g_default_syntax_highlight = mode;
    /* Move the live value too. The installer runs once at GL init, before any
     * file/example `@cfg` is applied, so the current value is by definition
     * still the compile-time default - assigning it here is what makes the
     * renderer verdict take effect on the very first frame rather than only
     * after the next reset. */
    g_glr_state.presentation.syntax_highlight = mode;
}

int glr_state_default_syntax_highlight(void) {
    return g_default_syntax_highlight;
}

void glr_state_presentation_reset_example_defaults(void) {
    /* Reset the presentation fields of the **scene-local** roster - the
     * settings a scene owns and therefore must not inherit from whatever
     * was loaded before it. The roster is `k_cfg_scene_defaults[]` in
     * glr_actions.c (which also drives the cfg-bridge's
     * `fill_scene_subset` / `fill_scene_defaults`); this function is its
     * direct-write form, deliberately bypassing glr_config_set() so the
     * reset carries no side effects. ortho_mode and projection_mode are
     * in that roster too, and both are reset here so the 2D/3D view and
     * the projection don't leak across example loads in the F12 cycle -
     * an example whose @cfg omits `view_mode` gets the default 3D, not
     * whatever the prior example set. The roster's two non-presentation
     * members (camera auto-rotate, variable-panel visibility) live in peer
     * modules and are reset by glr_ctrl_reset_example_chrome() instead.
     *
     * Everything else is deliberately NOT scene-local and survives an
     * example switch: session-inspection settings (the profilers, depth /
     * stencil viz, the call-depth tint, post FX, replay options),
     * interface settings
     * (code_panel_layout, wrap_at_comma, syntax_highlight, paren match /
     * scope), render features (MSAA, line smooth, accum), and autonormal.
     * test_glr_ctrl.c pins this reset against the roster. */
    GlrPresentationState *p = &g_glr_state.presentation;
    p->wireframe             = CFG_DEFAULT_WIREFRAME;
    p->grid_theme            = CFG_DEFAULT_GRID_THEME;
    p->grid_major_idx        = CFG_DEFAULT_GRID_MAJOR_IDX;
    p->grid_extent_idx       = CFG_DEFAULT_GRID_EXTENT_IDX;
    p->grid_brightness_idx   = CFG_DEFAULT_GRID_BRIGHTNESS_IDX;
    p->axes_theme            = CFG_DEFAULT_AXES_THEME;
    p->show_vertex_labels    = CFG_DEFAULT_VERTEX_LABELS;
    p->vertex_label_placement = CFG_DEFAULT_VERTEX_LABEL_PLACEMENT;
    p->overlay_scope         = CFG_DEFAULT_OVERLAY_SCOPE;
    p->show_normal_vectors   = CFG_DEFAULT_NORMAL_VECTORS;
    p->show_vertex_outlines  = CFG_DEFAULT_VERTEX_OUTLINES;
    p->vertex_outline_style  = CFG_DEFAULT_VERTEX_OUTLINE_STYLE;
    p->show_vertex_points    = CFG_DEFAULT_VERTEX_POINTS;
    p->highlight_current_poly = CFG_DEFAULT_HIGHLIGHT_POLY;
    p->xform_guide_mode      = CFG_DEFAULT_XFORM_GUIDE_MODE;
    p->show_light_indicators = CFG_DEFAULT_LIGHT_INDICATORS;
    p->light_theme           = CFG_DEFAULT_LIGHT_THEME;
    p->backdrop_mode         = CFG_DEFAULT_BACKDROP_MODE;
    p->ortho_mode            = CFG_DEFAULT_ORTHO_MODE;
    p->projection_mode       = CFG_DEFAULT_PROJECTION;
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
