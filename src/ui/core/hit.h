/*
 * ui_hit.h - Neutral hit-test result returned by UI modules.
 *
 * `UiHit` is the passive payload UI hit-testers return to the controller. It
 * carries just enough context for routing: the kind of region that was hit plus
 * optional line/row/char/item indices the owner needs to interpret that hit.
 * UI code classifies; the controller routes; the owning subsystem implements the
 * behavior.
 *
 * The struct is intentionally narrow. Widgets that need richer drag or modal
 * state keep that state in their own peer subsystem; `UiHit` only describes
 * where the pointer landed.
 */
#ifndef UI_HIT_H
#define UI_HIT_H

typedef enum {
    UI_HIT_NONE = 0,             /* outside any UI region */
    UI_HIT_CODE_TEXT,            /* committed code-panel text row */
    UI_HIT_CODE_INSERT_LINE,     /* code-panel "next line" past last commit */
    UI_HIT_CODE_GUTTER,          /* code-panel left margin / line numbers */
    UI_HIT_CODE_SCROLLBAR,       /* code-panel scrollbar track / thumb */
    UI_HIT_PANEL_DIVIDER,        /* draggable code-panel ↔ scene divider */
    UI_HIT_CORE_COUNT
} UiHitKind;

/* Per-kind field semantics. The struct shape is a flat union of optional
 * fields; each composer fills only what its kind requires.
 *
 *   UI_HIT_CODE_TEXT
 *     line_idx = committed source-cmd row the click landed on after any
 *                adapter-side rewrite; generic text-panel virtual rows may
 *                still arrive with line_idx = -1 until that rewrite
 *                happens
 *     char_idx = input-cursor target (column within input buffer)
 *     visual_row = wrap-row offset within line_idx
 *     cmd_idx = logical text-panel row index (adapter-private lookup key)
 *
 *   UI_HIT_CODE_INSERT_LINE
 *     line_idx = current edit_line (the row appended commands land on)
 *     char_idx = input-cursor target
 *     visual_row = wrap-row offset
 *     cmd_idx = logical text-panel row index
 *
 *   UI_HIT_CODE_GUTTER
 *     line_idx = committed source-cmd row
 *     visual_row = wrap-row offset
 *     cmd_idx = logical text-panel row index
 *
 *   UI_HIT_CODE_SCROLLBAR
 *     item_idx = grab offset in pixels from the pointer up to the thumb's
 *                top edge, so a drag maps a later pointer y back to a thumb
 *                top (thumb_top = gl_y + item_idx) without the thumb
 *                jumping on the press. A press on the track rather than the
 *                thumb reports half the thumb height, which centers the
 *                thumb on the pointer.
 *
 *   UI_HIT_PANEL_DIVIDER  — coordinates only, no line / row payload
 */
typedef struct {
    int kind; /* int kind to allow enum extension by ui/app/hit.h */

    /* Source-command line targeted by the hit. -1 when not
     * applicable, including generic text-panel virtual rows before the
     * adapter rewrites them. */
    int  line_idx;

    /* Visual row inside that line (for wrap-aware code panel
     * hits). -1 when not applicable. */
    int  visual_row;

    /* Char position within the wrapped row. -1 when not applicable. */
    int  char_idx;

    /* Source-command index for hits tied to a cmd (color swatch,
     * variable slider). -1 when not applicable. */
    int  cmd_idx;

    /* Item index for menu / pin / replay-button hits. -1 when not
     * applicable. */
    int  item_idx;

    /* Local coordinates relative to the hit region, in OpenGL
     * (bottom-up) y. Useful when the owner needs sub-row pixel
     * offsets (slider thumb position, picker swatch coordinate). */
    float local_x;
    float local_y;
} UiHit;

/* Initialize a UiHit to UI_HIT_NONE with all fields cleared/-1. */
static inline UiHit ui_hit_none(void) {
    UiHit h = {
        .kind       = UI_HIT_NONE,
        .line_idx   = -1,
        .visual_row = -1,
        .char_idx   = -1,
        .cmd_idx    = -1,
        .item_idx   = -1,
        .local_x    = 0.0f,
        .local_y    = 0.0f,
    };
    return h;
}

#endif /* UI_HIT_H */
