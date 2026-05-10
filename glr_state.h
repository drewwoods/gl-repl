/*
 * glr_state.h -- App-frame presentation/render state owner.
 *
 * Step 7a of feature/decouple-repl-from-gl-repl-alt.md relocates the
 * `presentation` and `render` slices out of `ReplRuntimeState` (REPL
 * pipeline) and into this new app-side owner. After the move,
 * `repl_state.{c,h}` contains only REPL-language state (document, flat
 * program, variables, scenes-program-state, REPL-side import_export);
 * `GlrState` covers everything that's app-frame chrome / render policy.
 *
 * REPL pipeline TUs (the REPL_DEMO_DEP_SRCS list) MUST NOT include this
 * header — that's the contract `check-repl-state-no-glr-state` enforces.
 * App-shell (`glr_*.c`), editor (`src/editor/*.c`), and UI / scene
 * renderers may include it freely.
 *
 * Storage layout mirrors the prior `ReplPresentationState` /
 * `ReplRenderState` typedefs minus the dead `focus_vertex[3]` /
 * `_valid` fields. `glr_ctrl.c::glr_ctrl_build_focus_vertex` recomputes
 * the focus vertex from the document each frame into a
 * `SceneFocusVertex` snapshot; the persistent storage was unused.
 */
#ifndef GLR_STATE_H
#define GLR_STATE_H

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
    int show_vertex_guides;
    int xform_guide_mode;
    int autonormal;
    int show_light_indicators;
    int backdrop_mode;
    int highlight_current_poly;
    int ortho_mode;
    int wrap_at_comma;
    int code_panel_layout;
} GlrPresentationState;

typedef struct {
    /* User-config toggles. The runtime-mutated halves of the render
     * slice — `lights[]` and `clear_color[]`, written by the executor
     * in response to glEnable(GL_LIGHTn) / glClearColor commands —
     * stay on `ReplRenderState` since `repl_executor.c` is a REPL
     * pipeline TU and cannot include `glr_state.h`. */
    int   use_accum;
    int   accum_aa_enabled;
    int   accum_samples;
    float accum_jitter_x;
    float accum_jitter_y;
    int   multisample_enabled;
    int   line_smooth_enabled;
    int   point_attenuation_enabled;
} GlrRenderState;

typedef struct {
    GlrPresentationState presentation;
    GlrRenderState       render;
} GlrState;

GlrPresentationState  glr_state_presentation(void);
GlrPresentationState *glr_state_presentation_mut(void);

GlrRenderState        glr_state_render(void);
GlrRenderState       *glr_state_render_mut(void);

void glr_state_presentation_reset_defaults(void);
/* Reset the scene-bound presentation subset to the example baseline.
 * Mirrors the cfg-bridge `fill_scene_subset` whitelist in glr_actions.c
 * (wireframe, grid theme, axes theme, vertex overlays, etc.). The
 * example loader invokes this through the controller-installed sink
 * `repl_install_example_presentation_reset_sink` so the REPL pipeline
 * never reaches into glr_state directly. */
void glr_state_presentation_reset_example_defaults(void);
void glr_state_render_reset_defaults(void);

void glr_state_capture(GlrState *snapshot);
void glr_state_restore(const GlrState *snapshot);

const float *glr_state_grid_major_steps(void);
const float *glr_state_grid_extents(void);

#endif /* GLR_STATE_H */
