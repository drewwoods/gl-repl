/*
 * stencil_viz.h - Stencil-buffer visualization overlay.
 *
 * Stencil is the one buffer you cannot infer from the rendered frame:
 * color and depth show up as pixels, but a mask that did the wrong thing
 * looks exactly like a mask that did the right thing until geometry goes
 * missing. This paints the stencil buffer over the scene so you can see
 * what the mask actually contains.
 *
 * Same shape as depth_viz: a GL-free conversion core (buffer_viz_stencil_scan
 * / _map, unit-tested on synthetic buffers) plus a thin GL shell
 * (glReadPixels GL_STENCIL_INDEX, RGBA texture, one screen-space quad).
 * The host drives both halves through render3d's neutral buffer hooks —
 * see buffer_viz.h for the fan-out.
 *
 * Conventions:
 *   - **Zero is transparent, and that is the whole compositing model.**
 *     stencil == 0 -> alpha 0, leaving the scene untouched; non-zero ->
 *     a colour at a fixed alpha. Without this a full-rect quad would
 *     paint over the entire viewport.
 *   - **The contract is "zero vs non-zero", never write history.** A
 *     readback cannot distinguish "never written", "cleared to 0" and
 *     "the program explicitly wrote 0" — all three are byte 0 — so an
 *     explicit write of 0 is invisible. Recovering provenance would need
 *     a second coverage buffer; it is not in the stencil data.
 *   - PALETTE maps value -> a fixed 16-entry table indexed `value & 15`,
 *     so a given value is always the same colour no matter what else is
 *     on screen (that view-independence is the point of the mode). Values
 *     16 apart therefore share a swatch; the legend prints the numeric
 *     value beside each one, so a collision is disambiguated wherever it
 *     can be seen.
 *   - RAMP normalizes non-zero values across their own EMA-smoothed
 *     [min, max] onto a cool -> warm ramp. The range deliberately
 *     EXCLUDES zero: it is the clear value and dominates every scene, so
 *     including it would pin min at 0 and flatten the contrast the mode
 *     exists to provide. Zeros are transparent, so they show nothing
 *     either way.
 *   - SPLIT palette-maps, but composites over the right half only,
 *     pixel-aligned with the untouched render on the left (one integer
 *     split coordinate drives both the quad and the texture crop).
 *
 * Pure fixed-function GL (no shaders, no FBOs, no glDrawPixels — the
 * latter is absent from both the web build and the GL stubs).
 */
#ifndef BUFFER_VIZ_STENCIL_H
#define BUFFER_VIZ_STENCIL_H

#include "subsystems/buffer_viz/buffer_viz.h"   /* BufferVizRange */

typedef enum BufferVizStencilMode {
    BUFFER_VIZ_STENCIL_OFF = 0,
    BUFFER_VIZ_STENCIL_PALETTE,  /* deterministic value -> colour */
    BUFFER_VIZ_STENCIL_RAMP,     /* non-zero min/max normalized, EMA-smoothed */
    BUFFER_VIZ_STENCIL_SPLIT,    /* right half, palette-mapped */
    BUFFER_VIZ_STENCIL_COUNT
} BufferVizStencilMode;

/* Per-value pixel counts for the legend. Produced by the same scan RAMP
 * already needs for its range, so the counts cost nothing extra. */
typedef struct BufferVizStencilHistogram {
    unsigned int counts[256];
    int distinct;    /* number of non-zero VALUES present (bin 0 excluded) */
    int total_px;    /* pixels scanned, including zeros */
    int nonzero_px;  /* pixels with a non-zero value */
    int valid;       /* 0 when no scan has completed since the last reset */
} BufferVizStencilHistogram;

/* Free the CPU buffers, delete the texture, clear the smoothed range and
 * the published histogram. Called through buffer_viz_reset(). */
void buffer_viz_stencil_reset(void);

/* Read the scene-rect stencil buffer (GL window coords). Call from
 * buffer_read_fn on EVERY accumulation pass — the overlay composites per
 * pass, so it needs that pass's own buffer. Buffer-allocation failure
 * degrades to a no-op capture. */
void buffer_viz_stencil_capture(int sx, int sy, int sw, int sh);

/* Convert the captured buffer and composite it over the scene rect
 * (right half only for SPLIT). No-op without a valid same-sized capture
 * from this pass; each capture is consumed by at most one render.
 *
 * `is_final_pass` is frame bookkeeping, not a gate: the smoothed range is
 * folded ONCE per frame and reused unchanged by every later pass (16
 * folds at max accum would change its time constant relative to depth's
 * and let the colours drift between blur samples of one frame), and the
 * histogram the legend reads is published from the final pass so its
 * counts do not flicker. */
void buffer_viz_stencil_render(BufferVizStencilMode mode, int is_final_pass,
                               int sx, int sy, int sw, int sh);

/* The most recently published histogram. Never NULL; check `.valid`. */
const BufferVizStencilHistogram *buffer_viz_stencil_histogram(void);

/* Pure (no GL). One pass over `stencil` producing both products: the
 * per-value histogram and the non-zero value extent. Returns 1 when at
 * least one non-zero value was found (and writes *lo_out / *hi_out),
 * 0 otherwise (outputs untouched). Either output pointer may be NULL. */
int buffer_viz_stencil_scan(const unsigned char *stencil, int count,
                            BufferVizStencilHistogram *hist_out,
                            float *lo_out, float *hi_out);

/* Pure (no GL) stencil -> RGBA conversion; the render path is a thin GL
 * wrapper around this. Writes FOUR bytes per input: colour per `mode`,
 * and alpha 0 for value 0 or BUFFER_VIZ_STENCIL_ALPHA otherwise. `range`
 * is consulted by RAMP only (a degenerate or invalid span maps every
 * non-zero pixel to the middle of the ramp instead of dividing by ~0);
 * PALETTE and SPLIT ignore it, which is exactly why their colours do not
 * shift as the scene changes. SPLIT maps like PALETTE — the mode differs
 * in where it is composited, not in what it contains. */
void buffer_viz_stencil_map(const unsigned char *stencil, int count,
                            BufferVizStencilMode mode,
                            const BufferVizRange *range,
                            unsigned char *rgba_out);

/* The palette colour for one stencil value, for the legend to draw the
 * same swatch the overlay used. Writes 3 bytes. */
void buffer_viz_stencil_palette_rgb(int value, unsigned char *rgb_out);

/* The colour `mode` actually painted `value` with in the most recently
 * rendered frame, for a legend swatch that samples the viewport rather
 * than offering a second opinion about it. Writes 3 bytes. Identical to
 * buffer_viz_stencil_palette_rgb() for PALETTE and SPLIT; RAMP resolves
 * against that frame's smoothed range, so it is the one mode where the
 * two disagree. Value 0 has no painted colour (it is transparent) and
 * falls back to the palette entry. */
void buffer_viz_stencil_swatch_rgb(int value, BufferVizStencilMode mode,
                                   unsigned char *rgb_out);

/* Overlay alpha applied to every non-zero pixel (0..255). Exposed so a
 * legend can preview swatches at the same weight the overlay draws. */
#define BUFFER_VIZ_STENCIL_ALPHA 184

#endif /* BUFFER_VIZ_STENCIL_H */
