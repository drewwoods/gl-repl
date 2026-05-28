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

#include "scene/guides/xform_guide_mode.h"
#include "scene/view_mode.h"

/* Scene-presentation policy: what chrome and overlays the app should show, how
 * the code panel should format text, and which view mode/backdrop/filter choices
 * are active for the current session. */
typedef struct {
    int wireframe;
    int grid_theme;
    int grid_major_idx;
    int grid_extent_idx;
    int axes_theme;
    int show_vertex_labels;
    int show_normal_vectors;
    int show_vertex_indices;
    int show_vertex_outlines;
    int show_vertex_points;
    SceneXformGuideMode xform_guide_mode;
    int autonormal;
    int show_light_indicators;
    int light_theme;
    int backdrop_mode;
    /* Experimental scene post-processing (ScenePostFilterMode).
     * Session-level only: Ctrl+N toggles it; never in @cfg / examples. */
    int post_filter_mode;
    int highlight_current_poly;
    SceneViewMode ortho_mode;
    int wrap_at_comma;
    int code_panel_layout;
    int syntax_highlight;   /* 0 = off, 1 = on, 2 = on + fake-bold */
    int code_focus;         /* 1 = hide derived C boilerplate chrome rows */
} GlrPresentationState;

/* App-owned render policy toggles. Runtime values emitted directly by user GL
 * commands, such as `lights[]` and `clear_color[]`, stay on ReplRenderState so
 * src/repl/executor.c can mutate them without depending on glr_state. */
typedef struct {
    /* User-config toggles. The runtime-mutated halves of the render
     * slice — `lights[]` and `clear_color[]`, written by the executor
     * in response to glEnable(GL_LIGHTn) / glClearColor commands —
     * stay on `ReplRenderState` since `src/repl/executor.c` is a REPL
     * pipeline TU and cannot include `glr_state.h`. */
    int   use_accum;
    int   accum_aa_enabled;
    int   accum_samples;
    float accum_jitter_x;
    float accum_jitter_y;
    int   multisample_enabled;
    int   msaa_samples;
    int   line_smooth_enabled;
    int   point_attenuation_enabled;
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
