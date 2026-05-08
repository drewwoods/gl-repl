/*
 * scene_render_types.h - shared render context types
 */
#ifndef SCENE_RENDER_TYPES_H
#define SCENE_RENDER_TYPES_H

#include "themes.h"
#include <gl_includes.h>     /* GLenum for SceneLight.id */

#define MAX_LIGHTS 4

/* Polygon-outline depth-bias constants. Shared by live overlays
 * (scene_overlays.c) and exported-output writeback (repl_export.c)
 * so both paths use exactly the same offset values. Living here
 * lets repl_export.c access them transitively (via repl_state_views.h)
 * without including a `scene_*` header directly, which would
 * violate check-controller-boundaries. */
#define REPL_OUTLINE_POLYGON_OFFSET_FACTOR (-0.01f)
#define REPL_OUTLINE_POLYGON_OFFSET_UNITS  (-100.0f)

typedef struct {
    GLenum   id;         /* GL_LIGHT0 .. GL_LIGHT3 */
    int      enabled;
    float    pos[4];     /* xyz + w (0=directional, 1=positional) */
    float    diffuse[4];
    float    ambient[4];
    float    specular[4];
} SceneLight;

typedef struct SceneRgba {
    float r, g, b, a;
} SceneRgba;

/* Per-call context the scene passes back to the user's geometry callback.
 * Currently a placeholder — kept as a struct so future scene-to-callback
 * metadata (e.g. an enum describing why the callback is being invoked:
 * main fill, fade-replay overlay, picking pass, etc.) can be added
 * without changing the function-pointer signature again. */
typedef struct SceneExecuteContext {
    int unused_;
} SceneExecuteContext;

/* Called by scene_render.c to emit user geometry. May be NULL (geometry
 * is silently skipped; scene background/grid/axes/lights still render).
 * The callback gets only a scene-supplied opaque context plus the user_data
 * the caller stashed when building the config — any REPL state (flat
 * program, current PC, alpha overrides for fade passes, etc.) is the
 * caller's responsibility, carried through user_data. */
typedef void (*SceneExecuteProgramFn)(const SceneExecuteContext *ctx,
                                      void *user_data);

typedef struct SceneFocusVertex {
    float pos[3];
    int valid;
} SceneFocusVertex;

/* Snapshot of all per-frame inputs that helper renderers need to read
 * without sampling globals again.  scene_render.c fills this once at frame
 * start, then passes it to grid/axes/overlay helpers. */
typedef struct SceneRenderConfig {
    /* --- Execute hook --- */
    SceneExecuteProgramFn execute_fn;          /* NULL = no geometry */
    void                 *execute_user_data;

    /* --- Optional post-fill hook ---
     * Invoked once per pass between the main user-geometry fill and the
     * scene helpers (grid / axes / backdrop / overlays). The REPL
     * controller installs this to overlay fading-replay geometry on top
     * of the main fill; non-REPL callers leave it NULL. */
    void (*post_fill_fn)(void *user_data);
    void  *post_fill_user_data;

    /* --- Optional post-overlays hook ---
     * Invoked at the end of the pass (after lights_render, before the
     * outermost glPopAttrib). The REPL controller uses this for
     * vertex-number / normal-vector overlays that walk the user's
     * program; non-REPL callers leave it NULL. */
    void (*post_overlays_fn)(void *user_data);
    void  *post_overlays_user_data;

    /* --- Background clear color (RGBA) ---
     * Populated by the controller from the user's CMD_CLEAR_COLOR (or a
     * default if none was set). The scene module no longer scans the
     * flat program for this; it's a pre-resolved float[4]. */
    float clear_color[4];

    /* --- Animation --- */
    float anim_time;

    /* --- Viewport and scene rectangle --- */
    int viewport_w;
    int viewport_h;
    int scene_x;
    int scene_y;
    int scene_w;
    int scene_h;

    /* --- Camera state (read-only inputs) ---
     * The actual modelview transform is no longer applied by the scene module —
     * callers must invoke scene_apply_camera() before scene_render_3d_scene().
     * These fields are still passed in because grid/axes themes orient
     * themselves to camera angle, the orbit-target gizmo is sized by cam_dist,
     * and the gizmo position comes from cam_tx/ty/tz. */
    float cam_dist;
    float cam_rx;
    float cam_ry;
    float cam_tx;
    float cam_ty;
    float cam_tz;
    float cam_motion_glow; /* 0 = orbit-target gizmo hidden */

    /* --- Rendering quality --- */
    int multisample_enabled;
    int line_smooth_enabled;
    int use_accum;
    int accum_aa_enabled;
    int accum_samples;

    /* --- Lighting --- */
    int        user_lighting_enabled;
    SceneLight lights[MAX_LIGHTS];
    int        show_light_indicators;

    /* --- Environment --- */
    int backdrop_mode;
    int wireframe;

    /* --- Grid and axes --- */
    int   grid_theme;
    int   grid_extent_idx;
    int   grid_major_idx;
    int   axes_theme;
    float grid_major_steps[GRID_MAJOR_COUNT];
    float grid_extents[GRID_EXTENT_COUNT];

    /* --- Focus marker (consumed by the focus grid theme) --- */
    SceneFocusVertex focus;

    /* --- Visual scaling --- */
    float alpha_scale; /* alpha boost to counter dark-bg crush; 1.0 = no change */
} SceneRenderConfig;

/* Derived state that helper renderers should consume instead of recomputing
 * from globals.  The focus vertex is prepared once per frame and passed into
 * the grid renderer when the focus theme is active. */
typedef struct FrameRenderContext {
    SceneRenderConfig config;
    SceneFocusVertex focus;
} FrameRenderContext;

#endif /* SCENE_RENDER_TYPES_H */
