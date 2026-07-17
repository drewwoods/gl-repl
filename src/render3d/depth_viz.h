/*
 * depth_viz.h - Depth-buffer visualization overlay.
 *
 * Renders the scene's depth buffer as a grayscale image: the controller-
 * selected mode flows through Render3dRenderConfig.depth_viz, render.c
 * captures the scene-rect depth at the end of the fill pass (after user
 * geometry + the replay-fade post_fill hook, BEFORE the backdrop/grid
 * helpers write their own depths — so grid exclusion falls out of read
 * placement), and the resolved image is drawn as a GL_LUMINANCE textured
 * quad just before the scene post-filter.
 *
 * Conventions (see render3d_depth_viz_map for the math):
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
 * latter is absent from both the web build and the GL stubs). No REPL /
 * app dependencies; links into render3d_demo like the rest of
 * src/render3d/.
 */
#ifndef RENDER3D_DEPTH_VIZ_H
#define RENDER3D_DEPTH_VIZ_H

#include "render.h"   /* Render3dProjectionDesc */

typedef enum Render3dDepthVizMode {
    RENDER3D_DEPTH_VIZ_OFF = 0,
    RENDER3D_DEPTH_VIZ_LINEAR,   /* full near..far range */
    RENDER3D_DEPTH_VIZ_SCENE,    /* normalized to captured geometry range */
    RENDER3D_DEPTH_VIZ_SPLIT,    /* right half depth (scene-normalized) */
    RENDER3D_DEPTH_VIZ_COUNT
} Render3dDepthVizMode;

/* EMA-smoothed linear-depth range for SCENE normalization. Owned by the
 * module for the live path; exposed so the pure mapping function below
 * can be driven with caller-owned state in tests. */
typedef struct Render3dDepthVizRange {
    float lo, hi;
    int   valid;   /* 0 until the first in-range capture seeds lo/hi */
} Render3dDepthVizRange;

/* Free the CPU buffers, delete the texture, clear the EMA range and the
 * cached GL_MAX_TEXTURE_SIZE. Called from render3d_init_gl() so a fresh
 * GL context never reuses a stale texture name. */
void render3d_depth_viz_reset(void);

/* Read the scene-rect depth buffer (GL window coords, the rect the
 * scene rendered into). Call at fill-end, before the helper passes;
 * under accumulation, on the last pass only. Reads the FULL rect even
 * for SPLIT so the scene-normalized range is independent of the split
 * position. Buffer-allocation failure degrades to a no-op capture. */
void render3d_depth_viz_capture(int sx, int sy, int sw, int sh);

/* Convert the captured depth to luminance and draw the quad over the
 * scene rect (right half only for SPLIT). No-op without a valid
 * same-sized capture from this frame; each capture is consumed by at
 * most one render. */
void render3d_depth_viz_render(Render3dDepthVizMode mode,
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
void render3d_depth_viz_map(const float *depth, int count,
                            Render3dDepthVizMode mode,
                            const Render3dProjectionDesc *proj,
                            Render3dDepthVizRange *range,
                            unsigned char *lum_out);

#endif /* RENDER3D_DEPTH_VIZ_H */
