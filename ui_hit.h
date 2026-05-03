/*
 * ui_hit.h - Neutral hit-test result returned by UI input handlers.
 *
 * Phase E contract:
 *
 *   UI hit-tests and renders.
 *   imrepl_ctrl routes hits to the owning subsystem.
 *   The owner (editor / variable_panel / replay / scene) implements
 *   the behaviour.
 *
 * `UiHit` is the passive payload UI files return from their input
 * handlers. It carries enough context for the controller to dispatch
 * to the right owner — the kind of region that was hit, plus optional
 * coordinate fields (line / row / char / cmd index) the owner uses to
 * interpret the hit.
 *
 * The struct is intentionally narrow. Click-driven UI elements that
 * still need richer state (e.g. color picker drag in progress) keep
 * their own per-module state; UiHit just classifies *where* the
 * pointer landed.
 *
 * UI files do not interpret hits. Routing happens in imrepl_ctrl,
 * which dispatches via `UiHit.kind` to the owning subsystem's
 * coarse handler.
 */
#ifndef UI_HIT_H
#define UI_HIT_H

typedef enum {
    UI_HIT_NONE = 0,         /* outside any UI region */
    UI_HIT_CODE_TEXT,        /* code-panel text area (editable rows) */
    UI_HIT_CODE_GUTTER,      /* code-panel left margin / line numbers */
    UI_HIT_COLOR_SWATCH,     /* color literal swatch in the editor */
    UI_HIT_MENU_ITEM,        /* menu bar dropdown item */
    UI_HIT_PIN_BUTTON,       /* pinned right-side button (search / replay) */
    UI_HIT_VARIABLE_SLIDER,  /* variable panel slider row */
    UI_HIT_REPLAY_BUTTON,    /* replay HUD play/pause/step button */
    UI_HIT_HELP_PANEL,       /* help overlay (read-only editor session) */
    UI_HIT_PANEL_DIVIDER,    /* draggable code-panel ↔ scene divider */
    UI_HIT_SCENE,            /* 3D viewport (scene/camera region) */
} UiHitKind;

typedef struct {
    UiHitKind kind;

    /* Source-command line targeted by the hit. -1 when not
     * applicable. */
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
