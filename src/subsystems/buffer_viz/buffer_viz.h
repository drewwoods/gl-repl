/*
 * buffer_viz.h - Framebuffer-inspection subsystem: shared vocabulary and
 * the single subscriber for render3d's neutral buffer hooks.
 *
 * render3d fires three hooks (Render3dRenderConfig.buffer_read_fn /
 * buffer_pass_overlay_fn / buffer_resolve_overlay_fn) and knows nothing
 * about what for. Each is a SINGLE slot, so this file is the one
 * subscriber and fans out to the per-buffer modules - depth_viz.c,
 * stencil_viz.c - according to the modes in the frame config the host
 * hands over as `user_data`.
 *
 * The two visualizations deliberately composite at different points:
 *
 *   depth   read (final pass only) -> resolve_overlay
 *           a full-rect REPLACEMENT that belongs on the resolved image
 *   stencil read (every pass)      -> pass_overlay
 *           a SPARSE overlay that must stay under the edit overlays, and
 *           must accumulate like everything else in the pass
 *
 * Policy stays with the host: it owns the readback-capability probes and
 * the config rows, and passes the already-masked modes per frame. This
 * module never asks whether a mode is allowed - only what it is.
 */
#ifndef BUFFER_VIZ_H
#define BUFFER_VIZ_H

#include "render3d/render_types.h"   /* Render3dRenderConfig */

/* EMA-smoothed value range for range-normalized viz modes. Shared rather
 * than per-buffer: "normalize against the captured data's own extent,
 * smoothed so animation flicker doesn't strobe the mapping" is the same
 * problem for depth's linear distances and stencil's integer values. */
typedef struct BufferVizRange {
    float lo, hi;
    int   valid;   /* 0 until the first capture seeds lo/hi */
} BufferVizRange;

/* Fold one frame's raw [lo, hi] into the smoothed range. Snaps outright
 * when the raw span jumps past BUFFER_VIZ_SNAP_RATIO of the smoothed one
 * in either direction - an example switch must not lag through the EMA -
 * and eases otherwise. Pure; the caller owns `range`. */
void buffer_viz_range_update(BufferVizRange *range, float raw_lo, float raw_hi);

/* Per-frame modes, owned by the host and handed to the hooks as
 * `user_data`. Plain ints so this header stays independent of the
 * per-buffer mode enums; the host has already masked each one to Off
 * where the GL context cannot support it. */
typedef struct BufferVizFrameConfig {
    int depth_mode;     /* BufferVizDepthMode */
    int stencil_mode;   /* BufferVizStencilMode */
} BufferVizFrameConfig;

/* Subscribe all three buffer hooks on `config`, with `frame` as the
 * user_data every one of them reads. `frame` must outlive the
 * render3d_draw_scene call. Passing frame = NULL unsubscribes. */
void buffer_viz_install(Render3dRenderConfig *config,
                        BufferVizFrameConfig *frame);

/* Drop every GPU-resident cache and smoothing state in the subsystem.
 * The host calls this from its init-GL path so a fresh context never
 * reuses a stale texture name or a stale range. */
void buffer_viz_reset(void);

#endif /* BUFFER_VIZ_H */
