/*
 * scene_render.h - 3D scene rendering orchestration.
 *
 * Top-level entry points for scene rendering. Orchestrates the full rendering
 * pipeline: projection setup, camera transforms, user geometry execution,
 * replay fade passes, grid/axes, and visual overlays (polygon outlines, vertex
 * numbers, normal vectors, orbit target).
 *
 * Pipeline flow (per frame from display_func in repl_core.c):
 *   1. scene_render_init_gl() — one-time setup of GL state, display lists,
 *      shaders, tessellator
 *   2. Per-accum-sample (if AA enabled):
 *      - Set up projection with optional sub-pixel jitter
 *      - Apply camera transforms
 *      - Execute user commands (repl_executor.c)
 *      - Render grid, axes, orbit target
 *      - Render overlays (polygon outline, vertex guides, normals, etc.)
 *      - Accumulate into accumulation buffer
 *   3. scene_render_replay_fade_pass() — if replay active, render fading
 *      geometry snapshots (old geometry fades out as new appears)
 *
 * Rendering modes: The scene can display multiple visual elements toggled via
 * config (F3/F4 for grid/axes themes, F1-F11 for overlays). Grid and axes are
 * themeable; overlays include polygon outlines, vertex numbers, normal vectors,
 * vertex guides (cross-hairs), light indicators, and orbit target.
 *
 * Edit guides: The scene also renders edit-guide visualization when editing
 * (geometry guides show vertex/primitive context, transform guides show pending
 * matrix operations during step-through). These are controlled by scene_*_guides
 * modules.
 */
#ifndef SCENE_RENDER_H
#define SCENE_RENDER_H

#include "scene_render_types.h"

/* Accumulation-buffer AA supports 1, 2, 4, 8, and 16-sample passes. The
 * controller cycles across that fixed ladder, and the scene renderer keeps a
 * 16-entry jitter table whose first N offsets form a good N-sample set. */
#define MAX_ACCUM_SAMPLES 16
#define ACCUM_STEP_COUNT  5

#define CFG_DEFAULT_MULTISAMPLE      1
#define CFG_DEFAULT_LINE_SMOOTH      0
#define CFG_DEFAULT_ATTENUATE_POINTS 1

/* One-time GL initialization: create display lists, compile shaders, allocate
 * tessellator, set up default light state. Called once on startup. */
void scene_render_init_gl(void);

/* Render the full 3D scene for one frame using an explicit config snapshot.
 * Orchestrates projection setup, camera transforms, user geometry execution,
 * grid/axes, overlays, and edit guides.
 * Called once per frame (or multiple times per frame for accumulation-buffer
 * AA). The controller builds the config once and passes it in. */
void scene_render_3d_scene(const SceneRenderConfig *config);

/* Render the replay fade-batch pass: walk config->replay_fade_plan and replay
 * each active batch through config->execute_fn with its per-batch alpha.
 * Normally invoked internally by scene_render_3d_scene; exposed publicly so
 * benchmarks can isolate the fade-pass workload. */
void scene_render_replay_fade_pass(const FrameRenderContext *frame_ctx);

#endif /* SCENE_RENDER_H */
