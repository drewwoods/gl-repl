/*
 * scene_render_types.h - shared render context types
 */
#ifndef SCENE_RENDER_TYPES_H
#define SCENE_RENDER_TYPES_H

#include "repl_flatten.h"
#include "sample.h"
#include "repl_replay.h"
#include "scene_guides_shared.h"

typedef struct SceneRgba {
    float r, g, b, a;
} SceneRgba;

/* Called by scene_render.c to emit user geometry. May be NULL (geometry
 * is silently skipped; scene background/grid/axes/lights still render).
 *
 * alpha_scale:          1.0 for normal pass; 0.0-1.0 for replay fade batches
 * skip_geom_before_pc:  first flat cmd index to emit geometry for (0 = all;
 *                       used by fade passes to suppress already-faded prefix)
 * flat_cmd_count:       number of commands to execute from program.cmds[]
 * program:              the flat command buffer + local vars
 * user_data:            opaque pointer supplied at config build time
 */
typedef void (*SceneExecuteProgramFn)(float alpha_scale,
                                     int skip_geom_before_pc,
                                     int flat_cmd_count,
                                     FlatProgramView program,
                                     void *user_data);

typedef struct SceneFocusVertex {
    float pos[3];
    int valid;
} SceneFocusVertex;

/* Snapshot of replay fade orchestration for the current frame. */
typedef struct ReplayFadePlan {
    int batch_count;
    ReplayFadeBatch batches[REPLAY_FADE_BATCH_MAX];
    int skip_limits[REPLAY_FADE_BATCH_MAX];
    float batch_alpha[REPLAY_FADE_BATCH_MAX];
    float baseline_predef_vals[MAX_PREDEF_VARS];
} ReplayFadePlan;

/* Snapshot of all per-frame inputs that helper renderers need to read
 * without sampling globals again.  scene_render.c fills this once at frame
 * start, then passes it to grid/axes/overlay helpers. */
typedef struct SceneRenderConfig {
    /* --- Execute hook --- */
    SceneExecuteProgramFn execute_fn;          /* NULL = no geometry */
    void                 *execute_user_data;
    void (*execute_reset_fn)(void *user_data);

    /* --- Flat program --- */
    FlatProgramView flat_program;

    /* --- Animation --- */
    float anim_time;

    /* --- Viewport and scene rectangle --- */
    int viewport_w;
    int viewport_h;
    int scene_x;
    int scene_y;
    int scene_w;
    int scene_h;

    /* --- Camera --- */
    float cam_dist;
    float cam_rx;
    float cam_ry;
    float cam_tx;
    float cam_ty;
    float cam_tz;
    float cam_motion_glow;

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

    /* --- 3D overlay flags --- */
    int show_guides;
    int show_vpoints;
    int show_vnums;
    int show_normals;
    int show_vertex_outlines;
    int show_current_poly;

    /* --- Cursor / editor block overlay --- */
    int          cursor_block_begin_idx;  /* -1 = no active block */
    int          cursor_block_end_idx;
    int          cursor_block_source_line;
    int          edit_line_idx;
    unsigned int cursor_func_scope_mask;
    int          cursor_call_src_cmd_idx; /* -1 = cursor not on a CMD_CALL */

    /* --- Focus and guide snapshots --- */
    SceneFocusVertex focus;
    SceneGuideSnapshot guide_snapshot;

    /* --- Replay --- */
    int            replaying;
    int            replay_mode;
    int            replay_tess_preview;
    int            replay_vertex_points;
    int            replay_has_fades;
    int            replay_base_limit;
    float          alpha_scale; /* alpha boost to counter dark-bg crush; 1.0 = no change */
    ReplayFadePlan replay_fade_plan;
} SceneRenderConfig;

/* Derived state that helper renderers should consume instead of recomputing
 * from globals.  The focus vertex is prepared once per frame and passed into
 * the grid renderer when the focus theme is active. */
typedef struct FrameRenderContext {
    SceneRenderConfig config;
    SceneFocusVertex focus;
} FrameRenderContext;

#endif /* SCENE_RENDER_TYPES_H */
