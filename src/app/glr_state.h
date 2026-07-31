/*
 * glr_state.h -- App-frame presentation and render-policy state owner.
 *
 * Holds the UI/render policy that belongs to the app shell rather than the REPL
 * language runtime: overlay toggles, grid/axes/backdrop presentation, code-panel
 * layout options, anti-aliasing configuration, and other scene-display policy.
 * Controller, editor, UI, and scene code read or mutate this state when they
 * need app-frame behavior; the REPL pipeline itself should not depend on it.
 *
 * That ownership boundary is enforced by `check-repl-state-no-glr-state`: REPL
 * pipeline TUs do not include this header, while app-shell, editor, UI, and
 * scene files may.
 *
 * (Relocated from the old mixed ReplRuntimeState layout into this app-side
 * owner as step 7a of feature/decouple-repl-from-gl-repl-alt.md.)
 */
#ifndef GLR_STATE_H
#define GLR_STATE_H

#include "render3d/guides/xform_guide_mode.h"
#include "render3d/view_mode.h"
#include "render3d/projection_mode.h"
#include "render3d/render_types.h"   /* Render3dLight, MAX_LIGHTS for the theme-seeded lights[] */

/* Scene-presentation policy: what chrome and overlays the app should show, how
 * the code panel should format text, and which view mode/backdrop/filter choices
 * are active for the current session. */
typedef struct {
    Render3dWireframeMode wireframe;
    int grid_theme;
    int grid_major_idx;
    int grid_extent_idx;
    int grid_brightness_idx;
    int axes_theme;
    int show_vertex_labels;
    int vertex_label_placement;
    int overlay_scope;    /* 0 = first loop instance, 1 = all, 2 = all instances at vertex (no declutter) */
    int show_normal_vectors;
    int show_vertex_outlines;
    int vertex_outline_style;
    int show_vertex_points;
    Render3dXformGuideMode xform_guide_mode;
    int autonormal;   /* ReplAutoNormalMode (repl/pipeline.h): off/face/smooth */
    int show_light_indicators;
    int light_theme;
    int backdrop_mode;
    /* Post FX config: scope is Off / 3D View / Frame; effect is the
     * operation selected when scope is not Off. Backed by the Post FX Scope
     * and Post FX Effect config rows, persisted in @cfg. */
    int post_fx_scope;
    int post_fx_effect;
    /* Scene post-processing filter (Render3dPostFilterMode), derived from
     * the Post FX config rows. */
    int post_filter_mode;
    /* Whole-frame (compositor) post-processing (Render3dPostFilterMode),
     * applied over the entire composited frame by the glr_compositor hook
     * at the end of glr_ctrl_display_frame. Also derived from Post FX. */
    int compositor_filter_mode;
    int highlight_current_poly;
    /* Winding-visualization view: paint front faces green, back (inside-out)
     * faces red so winding mistakes are visible. Config-backed: saved in full
     * workspace @cfg headers, but intentionally outside the per-example scene
     * snapshot/default-reset subset so F12 example switches do not change it. */
    int winding_view;
    /* Depth-buffer visualization (BufferVizDepthMode: Off / Linear /
     * Scene / Split). Config-backed (GLR_CONFIG_DEPTH_VIZ): like
     * winding_view it is saved in full workspace @cfg headers but
     * intentionally outside the per-example scene subset/default-reset,
     * so F12 example switches keep a debugging view active. The
     * controller forces the render-config copy to Off when the GL
     * context cannot read depth (web). */
    int depth_viz;
    /* Stencil-buffer visualization (BufferVizStencilMode: Off / Palette /
     * Ramp / Split). Like depth_viz it is a session inspection setting:
     * persisted in full workspace @cfg headers but intentionally left out of
     * per-example defaults and scene-local config. */
    int stencil_viz;
    Render3dViewMode ortho_mode;
    /* Projection mode, INDEPENDENT of ortho_mode's 2D-flatten view:
     * PROJ_PERSPECTIVE = perspective, PROJ_ORTHO = orthographic.
     * Unlike ortho_mode (which flattens and locks the camera to a top-down 2D view),
     * this only swaps the projection matrix — the camera stays free, so it renders
     * ortho from any orbit angle. The controller eases a separate projection-blend
     * scalar for it and combines the two blends with min() (ortho wins:
     * the scene is orthographic if EITHER control asks for it). Config-
     * backed (GLR_CONFIG_PROJECTION), saved in @cfg / per-scene subset. */
    Render3dProjectionMode projection_mode;
    int wrap_at_comma;
    int code_panel_layout;
    int syntax_highlight;   /* 0 = off, 1 = on, 2 = on + drop-shadow */
    int code_focus;         /* 1 = hide derived C boilerplate chrome rows */
    int paren_match;        /* 1 = tint the caret's matching parenthesis */
    int paren_scope;        /* 1 = highlight text inside the caret's parens */
} GlrPresentationState;

/* App-owned render policy toggles plus the theme-seeded light table. The
 * runtime-mutated halves written by the executor — the light-enable bitmask
 * and `clear_color[]` — stay on `ReplRenderState` since `src/repl/executor.c`
 * is a REPL pipeline TU and cannot include `glr_state.h`. The dimensional
 * per-light data (positions/colors/eye-space) is presentation state seeded
 * here from a scene light theme (render3d_lights_apply_theme), so it lives on the
 * app side; the controller merges it with the REPL enable bitmask each frame
 * when building Render3dRenderConfig.lights[]. */
typedef struct {
    int   use_accum;
    int   accum_bits;     /* accumulation depth probed at GL init; -1 = not probed */
    int   stencil_bits;   /* stencil depth probed at GL init; -1 = not probed */
    int   accum_effect;   /* Render3dAccumEffect: OFF / AA / BLUR */
    int   accum_passes;   /* resolved sample count: a GLR_ACCUM_PASS_LADDER step */
    int   multisample_enabled;
    int   line_smooth_enabled;
    int   point_attenuation_enabled;
    /* Theme-seeded light slots. The `.enabled` field here is unused (the
     * REPL pipeline owns enable state via ReplRenderState.light_enabled_mask);
     * `.id` is seeded to GL_LIGHT0+i, and positions/colors/eye-space come
     * from the active light theme. */
    Render3dLight lights[MAX_LIGHTS];
} GlrRenderState;

/* Whole app-side state bundle captured/restored alongside REPL/editor/UI peers
 * for full-world resets and snapshots. */
typedef struct {
    GlrPresentationState presentation;
    GlrRenderState       render;
} GlrState;

/* Read/write accessors for the live app-side presentation and render-policy
 * slices. Callers that only need to inspect the current values can use the
 * by-value accessors; mutating callers take the pointer forms. */
GlrPresentationState  glr_state_presentation(void);
GlrPresentationState *glr_state_presentation_mut(void);

GlrRenderState        glr_state_render(void);
GlrRenderState       *glr_state_render_mut(void);

void glr_state_presentation_reset_defaults(void);
/* Reset the scene-bound presentation subset to the example baseline.
 * Mirrors the cfg-bridge `fill_scene_subset` whitelist in glr_actions.c
 * (wireframe, grid theme, axes theme, vertex overlays, etc.). The
 * example loader invokes this through the controller-installed
 * `ReplHostEffects.example_presentation_reset` callback so the REPL
 * pipeline never reaches into glr_state directly. */
void glr_state_presentation_reset_example_defaults(void);
void glr_state_render_reset_defaults(void);

/* Copy or restore the full app-side state bundle. Used by whole-app snapshot and
 * reset flows that pair this state with REPL/editor/UI/replay captures. */
void glr_state_capture(GlrState *snapshot);
void glr_state_restore(const GlrState *snapshot);

/* Lookup tables for the currently-supported grid step/extents enum values. These
 * live with app-side presentation policy because grid theme/extent selection is
 * an app concern rather than a REPL-language concern. */
const float *glr_state_grid_major_steps(void);
const float *glr_state_grid_extents(void);

#endif /* GLR_STATE_H */
