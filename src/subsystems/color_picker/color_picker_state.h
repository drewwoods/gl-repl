/*
 * color_picker_state.h - Floating color-picker peer state and writeback API.
 *
 * Owns the current picker session: which source line is being edited, the HSV/
 * alpha slider state, cached geometry, and the writeback path that rewrites the
 * underlying color command through the editor commit pipeline. The renderer and
 * hit-test in `src/ui/app/color_picker.c` consume `ColorPickerView` and never
 * mutate peer state directly.
 *
 * Public API:
 *   - color_picker_start / _stop / _active_line / _can_edit_cmd
 *     for controller open/close orchestration.
 *   - color_picker_handle_press / _motion / _release for input
 *     dispatch. Each returns a ColorPickerInputResult so the
 *     controller can act on consumed-vs-closed-vs-changed without
 *     reading peer state.
 *   - color_picker_view returns a frame snapshot for renderers and
 *     hit-test consumers.
 *   - color_picker_hsv_to_rgb is the shared color-math helper used
 *     by the SV square, hue strip, alpha preview, and inline swatch
 *     drawers; RGB->HSV stays peer-private since only open/sync needs it.
 */
#ifndef COLOR_PICKER_STATE_H
#define COLOR_PICKER_STATE_H

/* Hit-rect bundle in y-up OpenGL coords. Materialized into the view so
 * UI hit-test and render don't recompute peer geometry. */
typedef struct {
    int sv_x, sv_y, sv_sz;
    int hue_x, hue_y, hue_w, hue_h;
    int alp_x, alp_y, alp_w, alp_h;
} ColorPickerRects;

typedef struct {
    int   open;             /* 0 when no picker is active; other fields stale */
    int   active_line;      /* source-cmd index, -1 when !open */
    int   has_alpha;        /* RGBA-shaped command (incl. CMD_CLEAR_COLOR) */
    float hue, sat, val;    /* HSV slider values, 0..1 */
    float alpha;            /* alpha slider, 0..1 (1.0 when !has_alpha) */
    int   anchor_px;        /* SV-square top-left in y-up OpenGL coords */
    int   anchor_py;
    ColorPickerRects rects;
    /* Maximum permissible V for the active command. 1.0 normally;
     * clamped lower for CMD_CLEAR_COLOR so renderers can shade the
     * out-of-range region without consulting the document. */
    float value_max;
    /* Popup-frame sizing the renderer needs but ColorPickerRects
     * doesn't carry: inter-element gap and preview-strip height.
     * Single source = the peer's CP_GAP/CP_PREV_H, surfaced here so
     * the renderer doesn't re-declare magic-twin literals. */
    int   gap;
    int   prev_h;
} ColorPickerView;

typedef struct {
    int consumed;   /* picker handled the event; controller should not fall through */
    int closed;     /* picker just transitioned from open to closed */
    int changed;    /* writeback fired (slider edit / new-value commit) */
} ColorPickerInputResult;

/* Pure HSV->RGB. h, s, v in [0, 1]; outputs in [0, 1]. */
void color_picker_hsv_to_rgb(float h, float s, float v,
                             float *r, float *g, float *b);

/* Open the picker on a specific source command. cmd_idx must satisfy
 * color_picker_can_edit_cmd (CMD_COLOR3F / CMD_COLOR4F / CMD_TESS_COLOR /
 * CMD_CLEAR_COLOR with constant args). my is the GLUT screen y where the
 * picker was triggered (used for vertical anchoring). No-op if the
 * command is not editable. */
void color_picker_start(int cmd_idx, int my);

/* Dismiss any active picker. Returns 1 if a picker was open and closed,
 * 0 if no picker was active. */
int  color_picker_stop(void);
void color_picker_state_reset(void);

/* Source-cmd index of the open picker, or -1 when closed. */
int  color_picker_active_line(void);

/* Returns 1 if cmd_idx is a glColor3f/glColor4f/gluColor/glClearColor
 * with constant arguments; 0 otherwise. */
int  color_picker_can_edit_cmd(int cmd_idx);

/* Snapshot of picker state for the current frame. Cheap to call (returns
 * a value); UI code should call this once per frame and pass the result
 * to its render/hit-test entry points rather than re-querying. */
ColorPickerView color_picker_view(void);

/* Mouse press / motion / release handlers. mx, my are GLUT screen coords.
 *
 * Press semantics:
 *   - inside a slider rect: { consumed=1, closed=0, changed=1 } (drag begins; changed=1 iff the writeback succeeded)
 *   - inside the picker bounds but outside slider rects: { consumed=1, closed=0, changed=0 } (consumed-no-op to prevent dismiss on padding clicks)
 *   - outside the picker: { consumed=0, closed=1, changed=0 } — picker
 *     dismisses itself; controller should redraw and let the click flow to
 *     menu/scene handlers.
 *   - picker not open: { consumed=0, closed=0, changed=0 }.
 *
 * Motion fires only while a slider drag is active (set by press).
 * Release ends any active drag; no result struct because nothing branches
 * on it today. */
ColorPickerInputResult color_picker_handle_press(int mx, int my);
ColorPickerInputResult color_picker_handle_motion(int mx, int my);
void                   color_picker_handle_release(void);

#endif /* COLOR_PICKER_STATE_H */
