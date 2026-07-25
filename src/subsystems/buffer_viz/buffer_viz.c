/*
 * buffer_viz.c - see buffer_viz.h.
 *
 * The hook multiplexer plus the range smoothing both visualizations
 * share. Deliberately tiny: everything that knows what a *buffer* means
 * lives in depth_viz.c / stencil_viz.c, and everything that decides
 * whether a mode is allowed lives in the host.
 */
#include "subsystems/buffer_viz/buffer_viz.h"

#include "subsystems/buffer_viz/depth_viz.h"
#include "subsystems/buffer_viz/stencil_viz.h"

#include <stddef.h>

/* EMA smoothing: s += alpha * (raw - s). */
#define BUFFER_VIZ_EMA_ALPHA  0.25f
/* Snap (no smoothing) when the raw span jumps past this ratio of the
 * smoothed span in either direction. */
#define BUFFER_VIZ_SNAP_RATIO 2.0f

void buffer_viz_range_update(BufferVizRange *range, float raw_lo, float raw_hi) {
    float raw_span, ema_span;
    int snap;

    if (!range)
        return;
    raw_span = raw_hi - raw_lo;
    ema_span = range->hi - range->lo;
    snap = !range->valid ||
           raw_span > ema_span * BUFFER_VIZ_SNAP_RATIO ||
           ema_span > raw_span * BUFFER_VIZ_SNAP_RATIO;
    if (snap) {
        range->lo = raw_lo;
        range->hi = raw_hi;
        range->valid = 1;
    } else {
        range->lo += BUFFER_VIZ_EMA_ALPHA * (raw_lo - range->lo);
        range->hi += BUFFER_VIZ_EMA_ALPHA * (raw_hi - range->hi);
    }
}

/* --- Hook fan-out -------------------------------------------------- */

static void buffer_viz_read_hook(void *user_data, int is_final_pass,
                                 int sx, int sy, int sw, int sh) {
    const BufferVizFrameConfig *frame = (const BufferVizFrameConfig *)user_data;
    if (!frame)
        return;
    /* Depth: one read per frame. Under accumulation every pass clears and
     * rewrites depth, so only the final pass survives into the resolved
     * image the quad is drawn over. */
    if (is_final_pass && frame->depth_mode != BUFFER_VIZ_DEPTH_OFF)
        buffer_viz_depth_capture(sx, sy, sw, sh);
    /* Stencil: every pass, because it composites into every pass. */
    if (frame->stencil_mode != BUFFER_VIZ_STENCIL_OFF)
        buffer_viz_stencil_capture(sx, sy, sw, sh);
}

static void buffer_viz_pass_overlay_hook(void *user_data, int is_final_pass,
                                         int sx, int sy, int sw, int sh) {
    const BufferVizFrameConfig *frame = (const BufferVizFrameConfig *)user_data;
    if (!frame)
        return;
    buffer_viz_stencil_render((BufferVizStencilMode)frame->stencil_mode,
                              is_final_pass, sx, sy, sw, sh);
}

static void buffer_viz_resolve_overlay_hook(void *user_data,
                                            const Render3dProjectionDesc *proj,
                                            int sx, int sy, int sw, int sh) {
    const BufferVizFrameConfig *frame = (const BufferVizFrameConfig *)user_data;
    if (!frame)
        return;
    buffer_viz_depth_render((BufferVizDepthMode)frame->depth_mode, proj,
                            sx, sy, sw, sh);
}

void buffer_viz_install(Render3dRenderConfig *config,
                        BufferVizFrameConfig *frame) {
    if (!config)
        return;
    config->buffer_read_fn                   = frame ? buffer_viz_read_hook : NULL;
    config->buffer_read_user_data            = frame;
    config->buffer_pass_overlay_fn           = frame ? buffer_viz_pass_overlay_hook
                                                     : NULL;
    config->buffer_pass_overlay_user_data    = frame;
    config->buffer_resolve_overlay_fn        = frame ? buffer_viz_resolve_overlay_hook
                                                     : NULL;
    config->buffer_resolve_overlay_user_data = frame;
}

void buffer_viz_reset(void) {
    buffer_viz_depth_reset();
    buffer_viz_stencil_reset();
}
