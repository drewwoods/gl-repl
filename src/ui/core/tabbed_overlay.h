/*
 * ui_tabbed_overlay.h - Modal tabbed text overlay (renderer + types).
 *
 * A generic full-screen modal that draws a titled, tabbed reference card.
 * Lines are split into left (key/command) and right (description) columns
 * on the first '\t'; lines without '\t' render in a single colour based
 * on indent level.
 *
 * The renderer is feature-agnostic: callers supply title, tabs, and
 * cursor (visible / tab_idx / scroll) through `UiOverlayState`. The
 * F1 help overlay is the current sole consumer (content adapted by
 * glr_ctrl from src/repl/help_text.c) but nothing here knows about "help"
 * specifically.
 *
 * Visibility, tab index, and scroll are inputs — callers gate display
 * with `visible = 0` rather than letting the renderer reach into a
 * named state slice.
 */
#ifndef UI_TABBED_OVERLAY_H
#define UI_TABBED_OVERLAY_H

typedef struct {
    const char        *label;     /* tab label drawn in the tab bar */
    const char *const *lines;     /* NULL-terminated lines */
} UiOverlayTab;

typedef struct UiOverlayContent {
    const char         *title;    /* title-bar text (e.g. "HELP") */
    const UiOverlayTab *tabs;     /* array of UiOverlayTab */
    int                 tab_count;
} UiOverlayContent;

typedef struct {
    int                     visible;
    int                     tab_idx;
    int                     scroll;
    int                     viewport_w;
    int                     viewport_h;
    const UiOverlayContent *content;
} UiOverlayState;

void ui_tabbed_overlay_render(const UiOverlayState *in);

/* Pure geometry queries sharing the renderer's layout math, so input
 * routing clamps/hit-tests against exactly what was drawn. */

typedef enum {
    UI_OVERLAY_HIT_OUTSIDE = 0, /* point is outside the panel rect */
    UI_OVERLAY_HIT_TAB,         /* point is on a tab; .tab is its index */
    UI_OVERLAY_HIT_BODY         /* inside the panel, not on a tab */
} UiOverlayHitKind;

typedef struct {
    UiOverlayHitKind kind;
    int              tab;        /* valid when kind == UI_OVERLAY_HIT_TAB */
    int              max_scroll; /* scroll clamp bound for the active tab */
} UiOverlayHit;

/* Largest valid scroll offset for the active tab (0 when content fits). */
int ui_tabbed_overlay_max_scroll(const UiOverlayState *in);

/* Center of tab `tab_idx` in GLUT mouse space (origin top-left) — the
 * inverse of the tab branch of the hit-test, so a scripted click aimed here
 * lands on exactly that tab. Returns 0 when hidden or out of range. */
int ui_tabbed_overlay_tab_point(const UiOverlayState *in, int tab_idx,
                                int *mx, int *my);

/* Classify a GLUT-space mouse point (top-left origin) against the
 * overlay. Returns OUTSIDE when not visible or off the panel. */
UiOverlayHit ui_tabbed_overlay_hit_test(const UiOverlayState *in,
                                        int mx, int my);

#endif /* UI_TABBED_OVERLAY_H */
