/*
 * src/ui/subsystems/buffer_viz_legend.h - Colour-key legend panel.
 *
 * UI mirror of the `subsystems/buffer_viz/` peer, in the same tier as
 * replay_hud.c / variable_panel.c. Pure renderer over a controller-built
 * view: a subsystem owns the data and publishes raw counts, the controller
 * decides which rows are worth showing, and this module only draws them.
 *
 *   buffer_viz_stencil_histogram()            (subsystem: raw counts)
 *   call_depth_viz_scan()                     (subsystem: raw counts)
 *     -> glr_ctrl_build_buffer_viz_legend_view()   (controller: selection)
 *       -> ui_buffer_viz_legend_render(view)       (here: pixels)
 *
 * The panel exists because a colour overlay alone cannot answer "which
 * value is that colour, and how much of the frame does it cover" - the
 * stencil palette repeats every 16 values, so a swatch is ambiguous and
 * the printed number is not; a call-depth ramp is continuous, so the
 * swatch says "deeper" but never says how deep. Rows carry the colour the
 * overlay actually painted, so the panel and the viewport always agree.
 *
 * Row selection - top-N by count, the zero row (when the producer has one)
 * and the total always retained - is the controller's job: a valid stencil
 * capture can hold all 256 values, recursion can reach
 * MAX_FLATTEN_CALL_DEPTH, and this panel has no scrolling, so an unbounded
 * legend would run off the viewport.
 */
#ifndef UI_BUFFER_VIZ_LEGEND_H
#define UI_BUFFER_VIZ_LEGEND_H

/* Listed value rows, excluding the always-present zero and total rows.
 * Tuned to the panel: enough for the handful of values a real masking
 * scene uses, short enough to never need scrolling. */
enum { UI_BUFFER_VIZ_LEGEND_MAX_ROWS = 8 };

/* One listed value. `rgb` is the colour the overlay drew for it under the
 * frame's mode, so the swatch is a sample of the viewport rather than a
 * second opinion about it. */
typedef struct {
    int value;              /* stencil: 0..255; call depth: 0..max */
    unsigned int count;     /* the producer's unit - see `total_px` */
    unsigned char rgb[3];
} UiBufferVizLegendRow;

/* Flat by-value view. `title` names the active mode and points at static
 * storage owned by the controller.
 *
 * **Two producers, one panel.** The stencil legend was the first; the
 * call-depth tint's key is the same table - swatch, value, count - and the
 * scene rect's top-left corner is single-tenant, so it fills the same view
 * rather than growing a near-identical second panel. The two fields below
 * are the whole difference between them; everything else is shared, which
 * is the point. The controller picks which producer speaks each frame.
 *
 * Counts are in the producer's unit: scene-rect pixels for stencil (from
 * the final accumulation pass's capture), flat commands for call depth. */
typedef struct {
    int visible;
    int window_w, window_h;
    int scene_x, scene_y, scene_w, scene_h;
    const char *title;
    /* Printed ahead of each row's value, e.g. "d" for "d0". NULL or "" is
     * the bare number stencil wants. */
    const char *row_prefix;
    /* Drop the trailing zero row. Stencil needs it - zero is the clear
     * value, it is transparent in every mode, and "how much of the frame is
     * still background" is the question the panel most often answers. Call
     * depth has no such row: depth 0 is the top level, an ordinary value
     * with an ordinary swatch, and a second "0" line under it would be a
     * lie. */
    int omit_zero_row;
    int row_count;                  /* listed rows, <= MAX_ROWS */
    UiBufferVizLegendRow rows[UI_BUFFER_VIZ_LEGEND_MAX_ROWS];
    int hidden_rows;                /* values not listed */
    unsigned int hidden_px;         /* what those values cover */
    unsigned int zero_px;           /* background (stencil == 0) */
    unsigned int total_px;          /* pixels / commands scanned */
} UiBufferVizLegendView;

/* Draw the legend in the scene rect's top-left corner. No-op when
 * view->visible is 0. */
void ui_buffer_viz_legend_render(const UiBufferVizLegendView *view);

/* Pure geometry: the panel's solved pixel size for `view` (0 x 0 when it
 * would not draw). Either output may be NULL. */
void ui_buffer_viz_legend_size(const UiBufferVizLegendView *view,
                               int *w, int *h);

#endif /* UI_BUFFER_VIZ_LEGEND_H */
