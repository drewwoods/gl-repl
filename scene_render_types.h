/*
 * scene_render_types.h — shared render context types
 */
#ifndef SCENE_RENDER_TYPES_H
#define SCENE_RENDER_TYPES_H

typedef struct SceneRgba {
    float r, g, b, a;
} SceneRgba;

typedef struct SceneFocusVertex {
    float pos[3];
    int valid;
} SceneFocusVertex;

/* Snapshot of all per-frame inputs that helper renderers need to read
 * without sampling globals again.  scene_render.c fills this once at frame
 * start, then passes it to grid/axes/overlay helpers. */
typedef struct SceneRenderConfig {
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
    float accum_jitter_x;
    float accum_jitter_y;
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
    int replay_fill_base_limit;
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
