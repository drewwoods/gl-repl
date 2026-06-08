/*
 * ui/state_types.h - Small UI-chrome value types.
 *
 * Defines the lightweight structs shared by `ui_state.h`, `UiRenderSnapshot`,
 * and UI renderers: code-panel chrome, help/profile/status flags, pointer, and
 * viewport size. These are UI-owned values, all `Ui*`-prefixed; ownership
 * lives here. `src/repl/state_views.h` points readers here in a comment but
 * does not re-export them.
 */
#ifndef UI_STATE_TYPES_H
#define UI_STATE_TYPES_H

#include "config.h"          /* REPL_STATUS_TEXT_MAX */

/* How long a status-bar message stays visible, in frames (~60 fps, so
 * 360 is roughly 6 s). The banner telescopes out of / back into the
 * messages bell over the first/last frames of this window (see
 * STATUS_ANIM_*_FRAMES in src/ui/app/panels.c). */
#define UI_STATUS_MESSAGE_TTL 360

/* Code-panel render chrome: panel divider and per-frame mirrors
 * of the presentation flags renderers need.
 *
 * `layout_mode` and `show_vertex_indices` are per-frame mirrors of
 * ReplPresentationState fields so ui_*.c renderers and hit-tests can
 * read them without crossing the repl_state_*() boundary; the controller
 * refreshes them in glr_ctrl_build_ui_snapshot. The source of truth still lives
 * on app-side presentation state. Scroll state intentionally does not live here;
 * it belongs to the editor session, not UI chrome. */
typedef struct {
    float panel_frac;
    int   resizing_panel;
    int   layout_mode;          /* mirror of presentation.code_panel_layout */
    int   show_vertex_indices;  /* mirror of presentation.show_vertex_indices */
    int   wrap_at_comma;        /* mirror of presentation.wrap_at_comma */
    int   syntax_highlight;     /* mirror of presentation.syntax_highlight */
    int   code_focus;           /* mirror of presentation.code_focus */
    int   paren_match;          /* mirror of presentation.paren_match */
    int   paren_scope;          /* mirror of presentation.paren_scope */
} UiCodePanelRuntimeState;

/* Help-overlay visibility. Tab selection and scroll live in the separate
 * EditorHelpSession snapshot carried alongside this flag. */
typedef struct {
    int visible;
} UiHelpState;



typedef struct {
    int mode;
} UiProfilePanelState;

typedef struct {
    int mode;
} UiMemoryPanelState;

/* Status banner severity. Renderer picks the palette from this. INFO is
 * the historical amber; ERROR is a red palette so failures (flatten
 * limit, command-buffer full, parse errors) are unmissable. */
typedef enum {
    UI_STATUS_INFO = 0,
    UI_STATUS_ERROR
} UiStatusKind;

typedef struct {
    char text[REPL_STATUS_TEXT_MAX];
    int  ttl;
    int  kind;  /* UiStatusKind; default INFO since enum starts at 0 */
    int  age;   /* frames since this message first appeared; reset only
                 * when the text/kind actually changes (not on a same-text
                 * refresh), so a message re-emitted every frame still
                 * telescopes out once and stays put. Controller ages it. */
} UiStatusState;

/* Recent-message history. Every status message (INFO and ERROR alike)
 * funnels through ui_state_status_set_kind, which pushes it here so the
 * user can pull up messages that already flashed and faded. Session-only
 * ring (no persistence); newest entry sits at index (head - 1). */
#define UI_STATUS_HISTORY_CAP 16

typedef struct {
    char         text[REPL_STATUS_TEXT_MAX];
    int          kind;       /* UiStatusKind */
    unsigned int dup_count;  /* collapsed consecutive duplicates, >= 1 */
    unsigned int seq;        /* monotonic id; larger == newer */
} UiStatusEntry;

typedef struct {
    UiStatusEntry entries[UI_STATUS_HISTORY_CAP];
    int           head;      /* next write slot (0..CAP-1) */
    int           count;     /* valid entries, <= CAP */
    unsigned int  next_seq;  /* id handed to the next pushed entry */
    int           open;      /* the inline history list is toggled visible */
} UiStatusHistory;

/* Ring index of the `ordinal`-th entry counting from oldest (0) to
 * newest (count-1). Returns -1 when ordinal is out of range. Pure, so
 * both the renderer and tests can walk the ring in display order. */
static inline int ui_status_history_index(const UiStatusHistory *h, int ordinal) {
    int start;
    if (!h || ordinal < 0 || ordinal >= h->count)
        return -1;
    /* oldest = head - count, normalized into [0, CAP). */
    start = h->head - h->count;
    start %= UI_STATUS_HISTORY_CAP;
    if (start < 0)
        start += UI_STATUS_HISTORY_CAP;
    return (start + ordinal) % UI_STATUS_HISTORY_CAP;
}

/* Camera pose is intentionally not part of the UI chrome types. See
 * glr_camera.h for the app-owned orbit/pan/zoom state. */

typedef struct {
    int mouse_x;
    int mouse_y;
    int mouse_button;
} UiPointerState;

typedef struct {
    int window_w;
    int window_h;
} UiViewportState;

#endif /* UI_STATE_TYPES_H */
