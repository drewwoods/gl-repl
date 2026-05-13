/*
 * text_panel.c -- Generic text panel renderer and hit-tester (stub).
 *
 * Phase 1: stub implementations only. The rendering and hit-test logic
 * moves here from src/ui/panels.c in Phase 2 of the editor-demo SRP split
 * (feature/editor-demo.md).
 *
 * This file has no dependency on repl/*, editor/*, or app/* headers.
 */
#include "ui/text_panel.h"
#include "ui/metrics.h"

int ui_text_panel_visible_lines_for_height(int panel_h) {
    int available = panel_h - CODE_MARGIN_Y - 2 * LINE_H - 3 - STATUSBAR_H;
    if (available < 0)
        return 1;
    return available / LINE_H + 1;
}

void ui_text_panel_render(const UiTextPanelSnapshot *snap,
                          UiTextPanelOutput         *out) {
    (void)snap;
    if (out) {
        *out = (UiTextPanelOutput){0};
        out->visible_rows = snap
            ? ui_text_panel_visible_lines_for_height(snap->cp_h)
            : 1;
    }
}

UiHit ui_text_panel_hit_test(const UiTextPanelSnapshot *snap,
                              int mx, int my) {
    (void)snap;
    (void)mx;
    (void)my;
    return ui_hit_none();
}
