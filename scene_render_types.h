/*
 * scene_render_types.h - shared render context types
 */
#ifndef SCENE_RENDER_TYPES_H
#define SCENE_RENDER_TYPES_H

#include "repl_flatten.h"
#include "sample.h"
#include "scene_guides_shared.h"

typedef struct SceneRgba {
    float r, g, b, a;
} SceneRgba;

/* Called by scene_render.c to emit user geometry. May be NULL (geometry
 * is silently skipped; scene background/grid/axes/lights still render).
 *
 * alpha_scale:          1.0 for normal pass; 0.0–1.0 for replay fade batches
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

/* Snapshot of all per-frame inputs that helper renderers need to read
 * without sampling globals again.  scene_render.c fills this once at frame
 * start, then passes it to grid/axes/overlay helpers. */
typedef struct SceneRenderConfig {
    /* ── Execute callback ─────────────────────────────────────────────── */
    SceneExecuteProgramFn execute_fn;          /* NULL = no geometry */
    void                 *execute_user_data;
    void (*execute_reset_fn)(void *user_data);

    /* ── Flat program (snapshot for overlays / outline pass) ──────────── */
    FlatProgramView flat_program;

    /* ── Animation ───────────────────────────────────────────────────── */
    float anim_time;

    /* ── Viewport ───────────────────────────────────────────────────── */
    int viewport_w;
    int viewport_h;

    /* ── Lighting ────────────────────────────────────────────────────── */
    int        user_lighting_enabled;
    SceneLight lights[MAX_LIGHTS];
    int        show_light_indicators;

    /* ── Backdrop ────────────────────────────────────────────────────── */
    int backdrop_mode;

    /* ── Outline overlay ─────────────────────────────────────────────── */
    int show_vertex_outlines;

    /* ── Replay HUD / layout ─────────────────────────────────────────── */
    int   code_panel_layout;
    int   replay_pc;
    int   replay_total_cmds;
    int   replay_state_val;     /* REPLAY_PLAYING / REPLAY_PAUSED / REPLAY_DONE */
    float replay_speed;
    int   replay_expand_args;

    /* ── Grid tables ─────────────────────────────────────────────────── */
    float grid_major_steps[GRID_MAJOR_COUNT];
    float grid_extents[GRID_EXTENT_COUNT];

    /* ── Cursor block bounds ─────────────────────────────────────────── */
    int          cursor_block_begin_idx;  /* -1 = no active block */
    int          cursor_block_end_idx;
    int          cursor_block_source_line;
    int          edit_line_idx;
    unsigned int cursor_func_scope_mask;
    int          cursor_call_src_cmd_idx; /* -1 = cursor not on a CMD_CALL */

    /* ── Focus / guide snapshots ────────────────────────────────────── */
    SceneFocusVertex focus;
    SceneGuideSnapshot guide_snapshot;

    /* ── Existing fields (legacy, preserved) ────────────────────────── */
    int scene_x;
    int scene_y;
    int scene_w;
    int scene_h;
    float cam_dist;
    float cam_rx;
    float cam_ry;
    float cam_tx;
    float cam_ty;
    float cam_tz;
    float cam_motion_glow;
    int multisample_enabled;
    int line_smooth_enabled;
    int wireframe;
    int grid_theme;
    int grid_extent_idx;
    int grid_major_idx;
    int axes_theme;
    int show_guides;
    int show_vpoints;
    int show_vnums;
    int show_normals;
    int replaying;
    int replay_mode;
    int replay_tess_preview;
    int replay_vertex_points;
    int replay_has_fades;
    int replay_base_limit;
    int show_current_poly;
    float alpha_scale; /* alpha boost to counter dark-bg crush; 1.0 = no change */
} SceneRenderConfig;

/* Derived state that helper renderers should consume instead of recomputing
 * from globals.  The focus vertex is prepared once per frame and passed into
 * the grid renderer when the focus theme is active. */
typedef struct FrameRenderContext {
    SceneRenderConfig config;
    SceneFocusVertex focus;
    float camera_world_y;
    int camera_below_water_surface;
} FrameRenderContext;

#endif /* SCENE_RENDER_TYPES_H */
