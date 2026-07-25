/*
 * depth_viz.h - Depth-buffer visualization overlay.
 *
 * Renders the scene's depth buffer as a grayscale image. The controller
 * owns the mode and drives both halves through render3d's neutral
 * buffer hooks (Render3dRenderConfig.buffer_read_fn /
 * buffer_resolve_overlay_fn): the scene-rect depth is captured at the
 * end of the fill pass (after user geometry + the replay-fade post_fill
 * hook, BEFORE the backdrop/grid helpers write their own depths — so
 * grid exclusion falls out of read placement), and the resolved image is
 * drawn as a GL_LUMINANCE textured quad just before the scene
 * post-filter.
 *
 * Conventions (see buffer_viz_depth_map for the math):
 *   - Depth is linearized through the active projection: perspective
 *     L = n*f / (f - z*(f-n)) (eye distance), ortho L = z (already
 *     linear in eye z). Mid 2D<->3D transition frames use the snapped
 *     discrete mode from Render3dProjectionDesc — momentary, invisible.
 *   - Near = bright, far = dark; background pixels (z at the clear
 *     value 1.0) render black.
 *   - LINEAR maps L across the full near..far range. SCENE normalizes
 *     to the captured geometry's own [Lmin, Lmax] (EMA-smoothed against
 *     animation flicker) for full contrast; the farthest scene pixel
 *     keeps a floor luminance so it stays distinguishable from
 *     background. SPLIT overlays the right half of the scene rect with
 *     the scene-normalized image, pixel-aligned with the normal render
 *     underneath (one integer split coordinate drives both the quad
 *     geometry and the texture crop).
 *
 * Pure fixed-function GL (no shaders, no FBOs, no glDrawPixels — the
 * latter is absent from both the web build and the GL stubs). Depends
 * downward on render3d (the projection desc, the screen-space 2D
 * bracket) and on nothing above it — the peer-subsystem direction
 * src/subsystems/edit_overlays/ already takes.
 */
#ifndef BUFFER_VIZ_DEPTH_H
#define BUFFER_VIZ_DEPTH_H

#include "render3d/render.h"   /* Render3dProjectionDesc */
#include "subsystems/buffer_viz/buffer_viz.h"   /* BufferVizRange */

typedef enum BufferVizDepthMode {
    BUFFER_VIZ_DEPTH_OFF = 0,
    BUFFER_VIZ_DEPTH_LINEAR,   /* full near..far range */
    BUFFER_VIZ_DEPTH_SCENE,    /* normalized to captured geometry range */
    BUFFER_VIZ_DEPTH_SPLIT,    /* right half depth (scene-normalized) */
    BUFFER_VIZ_DEPTH_COUNT
} BufferVizDepthMode;

/* The SCENE normalization range is the shared BufferVizRange from
 * buffer_viz.h — owned by this module for the live path, but taken as a
 * parameter by the mapping function below so tests can drive it with
 * caller-owned state. */

/* Free the CPU buffers, delete the texture, clear the EMA range and the
 * cached GL_MAX_TEXTURE_SIZE. Called by the controller from
 * glr_ctrl_init_gl() so a fresh GL context never reuses a stale texture
 * name. */
void buffer_viz_depth_reset(void);

/* Read the scene-rect depth buffer (GL window coords, the rect the
 * scene rendered into). Call from buffer_read_fn (fill-end, before the
 * helper passes); under accumulation, on the final pass only — the
 * subscriber gates on the hook's is_final_pass. Reads the FULL rect even
 * for SPLIT so the scene-normalized range is independent of the split
 * position. Buffer-allocation failure degrades to a no-op capture. */
void buffer_viz_depth_capture(int sx, int sy, int sw, int sh);

/* Convert the captured depth to luminance and draw the quad over the
 * scene rect (right half only for SPLIT). No-op without a valid
 * same-sized capture from this frame; each capture is consumed by at
 * most one render. */
void buffer_viz_depth_render(BufferVizDepthMode mode,
                             const Render3dProjectionDesc *proj,
                             int sx, int sy, int sw, int sh);

/* Pure (no GL calls) depth -> luminance conversion; the render path is
 * a thin GL wrapper around this, and synthetic depth-map tests drive it
 * directly. Linearizes each of the `count` depth values through `proj`,
 * normalizes per `mode` (SPLIT maps like SCENE; `range` is the caller-
 * owned EMA state, updated for SCENE/SPLIT), clamps to [0,1] before the
 * byte conversion — EMA lag may put samples outside the smoothed range
 * — and writes one byte per input. Degenerate spans (constant-depth
 * scene) map in-range pixels to mid-gray instead of dividing by ~0; a
 * capture with no in-range pixel at all falls back to the LINEAR map. */
void buffer_viz_depth_map(const float *depth, int count,
                          BufferVizDepthMode mode,
                          const Render3dProjectionDesc *proj,
                          BufferVizRange *range,
                          unsigned char *lum_out);

#endif /* BUFFER_VIZ_DEPTH_H */
