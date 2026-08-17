/*
 * console.h - The Console UI overlay panel.
 *
 * A floating panel (overlay layout slot) rendering captured trace lines from
 * `console("fmt", ...)`.
 */
#ifndef UI_SUPPORT_CONSOLE_H
#define UI_SUPPORT_CONSOLE_H

#include "subsystems/console/console.h"

#define UI_CONSOLE_PANEL_W  320
#define UI_CONSOLE_HEADER_H 20
#define UI_CONSOLE_LINE_H   15
#define UI_CONSOLE_PAD      8

enum {
    UI_CONSOLE_HIT_NONE  = 0,
    UI_CONSOLE_HIT_PANEL = 1,
    UI_CONSOLE_HIT_CLOSE = 2
};

typedef struct {
    int         window_w, window_h;
    int         visible;
    int         panel_x, panel_y; /* solved position from overlay layout */
    ConsoleView console;
} UiConsolePanelView;

void ui_console_panel_size(int line_count, int *out_w, int *out_h);
void ui_console_panel_render(const UiConsolePanelView *view);
int  ui_console_panel_hit_test(const UiConsolePanelView *view, int mx, int my);

#endif /* UI_SUPPORT_CONSOLE_H */
