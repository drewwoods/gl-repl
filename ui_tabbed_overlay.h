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
 * F1 help overlay is the current sole consumer (content built by
 * repl_help_text.c) but nothing here knows about "help" specifically.
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

#endif /* UI_TABBED_OVERLAY_H */
