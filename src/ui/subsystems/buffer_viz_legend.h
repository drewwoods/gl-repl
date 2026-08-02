/*
 * src/ui/subsystems/buffer_viz_legend.h - Stencil-buffer legend panel.
 *
 * UI mirror of the `subsystems/buffer_viz/` peer, in the same tier as
 * replay_hud.c / variable_panel.c. Pure renderer over a controller-built
 * view: the subsystem owns the captured buffer and publishes a raw
 * histogram, the controller decides which rows are worth showing, and
 * this module only draws them.
 *
 *   buffer_viz_stencil_histogram()            (subsystem: raw counts)
 *     -> glr_ctrl_build_buffer_viz_legend_view()   (controller: selection)
 *       -> ui_buffer_viz_legend_render(view)       (here: pixels)
 *
 * The panel exists because the stencil overlay alone cannot answer "which
 * value is that colour, and how much of the frame does it cover" - the
 * palette repeats every 16 values, so a swatch is ambiguous and the
 * printed number is not. Rows carry the colour the overlay actually
 * painted (RAMP included), so the panel and the viewport always agree.
 *
 * Row selection - top-N by pixel count, the zero row and the total always
 * retained - is the controller's job: a valid capture can hold all 256
 * values and this panel has no scrolling, so an unbounded legend would
 * run off the viewport.
 */
#ifndef UI_BUFFER_VIZ_LEGEND_H
#define UI_BUFFER_VIZ_LEGEND_H

/* Listed value rows, excluding the always-present zero and total rows.
 * Tuned to the panel: enough for the handful of values a real masking
 * scene uses, short enough to never need scrolling. */
enum { UI_BUFFER_VIZ_LEGEND_MAX_ROWS = 8 };

/* One listed stencil value. `rgb` is the colour the overlay drew for
 * this value under the frame's mode, so the swatch is a sample of the
 * viewport rather than a second opinion about it. */
typedef struct {
    int value;              /* 0..255 */
    unsigned int count;     /* pixels carrying it in the scene rect */
    unsigned char rgb[3];
} UiBufferVizLegendRow;

/* Flat by-value view. `title` names the active mode and points at static
 * storage owned by the controller. Counts are scene-rect pixels from the
 * final accumulation pass's capture. */
typedef struct {
    int visible;
    int window_w, window_h;
    int scene_x, scene_y, scene_w, scene_h;
    const char *title;
    int row_count;                  /* listed rows, <= MAX_ROWS */
    UiBufferVizLegendRow rows[UI_BUFFER_VIZ_LEGEND_MAX_ROWS];
    int hidden_rows;                /* non-zero values not listed */
    unsigned int hidden_px;         /* pixels those values cover */
    unsigned int zero_px;           /* background (stencil == 0) */
    unsigned int total_px;          /* pixels scanned */
} UiBufferVizLegendView;

/* Draw the legend in the scene rect's top-left corner. No-op when
 * view->visible is 0. */
void ui_buffer_viz_legend_render(const UiBufferVizLegendView *view);

/* Pure geometry: the panel's solved pixel size for `view` (0 x 0 when it
 * would not draw). Either output may be NULL. */
void ui_buffer_viz_legend_size(const UiBufferVizLegendView *view,
                               int *w, int *h);

#endif /* UI_BUFFER_VIZ_LEGEND_H */
