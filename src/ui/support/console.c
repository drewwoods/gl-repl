/*
 * console.c - Implementation of Console UI overlay panel.
 */
#include "ui/support/console.h"
#include "ui/core/gl_2d.h"
#include "ui/core/theme.h"

#include <stdio.h>
#include <string.h>

#define CONSOLE_CLOSE_LABEL "[x]"
#define CONSOLE_MAX_DISPLAY_LINES 16

static inline int console_control_w(const char *label) {
    return (label ? (int)strlen(label) : 0) * FONT_SMALL_W;
}

void ui_console_panel_size(int line_count, int *out_w, int *out_h) {
    if (out_w) *out_w = UI_CONSOLE_PANEL_W;
    if (out_h) {
        int lines = line_count > 0 ? line_count : 1;
        if (lines > CONSOLE_MAX_DISPLAY_LINES) lines = CONSOLE_MAX_DISPLAY_LINES;
        *out_h = UI_CONSOLE_HEADER_H + UI_CONSOLE_PAD * 2 + lines * UI_CONSOLE_LINE_H;
    }
}

int ui_console_panel_hit_test(const UiConsolePanelView *view, int mx, int my) {
    if (!view || !view->visible)
        return UI_CONSOLE_HIT_NONE;

    int panel_w, panel_h;
    ui_console_panel_size(view->console.count, &panel_w, &panel_h);
    int panel_x = view->panel_x;
    int panel_y = view->panel_y;

    /* OpenGL coordinates (y up) -> GLUT coordinates (y down) conversion:
     * In GLUT window coords:
     * gx in [panel_x, panel_x + panel_w]
     * gy in [window_h - (panel_y + panel_h), window_h - panel_y] */
    int gy_top = view->window_h - (panel_y + panel_h);
    int gy_bottom = view->window_h - panel_y;

    if (mx < panel_x || mx >= panel_x + panel_w ||
        my < gy_top || my >= gy_bottom) {
        return UI_CONSOLE_HIT_NONE;
    }

    /* Close chip [x] is in the header, right-aligned */
    int close_w = console_control_w(CONSOLE_CLOSE_LABEL);
    int close_x = panel_x + panel_w - close_w - UI_CONSOLE_PAD;
    int close_y_top = gy_top;
    int close_y_bottom = gy_top + UI_CONSOLE_HEADER_H;

    if (mx >= close_x - 2 && mx < close_x + close_w + 4 &&
        my >= close_y_top && my <= close_y_bottom) {
        return UI_CONSOLE_HIT_CLOSE;
    }

    return UI_CONSOLE_HIT_PANEL;
}

void ui_console_panel_render(const UiConsolePanelView *view) {
    if (!view || !view->visible)
        return;

    int panel_w, panel_h;
    ui_console_panel_size(view->console.count, &panel_w, &panel_h);
    int panel_x = view->panel_x;
    int panel_y = view->panel_y;

    gl2d_begin(view->window_w, view->window_h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)panel_x, (float)panel_y,
                     (float)panel_w, (float)panel_h,
                     UI_TOK_SUNKEN, 0.91f, UI_TOK_BORDER, 0.85f);
    glDisable(GL_BLEND);

    int tx = panel_x + UI_CONSOLE_PAD;
    int ty = panel_y + panel_h - UI_CONSOLE_HEADER_H + 2;

    /* Header: "Console" (+ line count / overflow count) + [x] */
    int close_w = console_control_w(CONSOLE_CLOSE_LABEL);
    ui_clr(UI_TOK_TEXT_PRIMARY);
    {
        char header[64];
        int vis_lines = view->console.count > CONSOLE_MAX_DISPLAY_LINES
                      ? CONSOLE_MAX_DISPLAY_LINES : view->console.count;
        int unshown = view->console.total_count - vis_lines;
        if (unshown > 0) {
            snprintf(header, sizeof(header), "Console (%d, +%d)",
                     view->console.total_count, unshown);
        } else if (view->console.total_count > 0) {
            snprintf(header, sizeof(header), "Console (%d)", view->console.total_count);
        } else {
            snprintf(header, sizeof(header), "Console");
        }
        gl2d_draw_string((float)tx, (float)ty, header, FONT_SMALL);
    }
    gl2d_chip_action((float)(panel_x + panel_w - close_w - UI_CONSOLE_PAD), (float)ty,
                     CONSOLE_CLOSE_LABEL);

    /* Body text lines */
    int line_y = panel_y + panel_h - UI_CONSOLE_HEADER_H - UI_CONSOLE_LINE_H + 2;
    if (view->console.count == 0) {
        ui_clr(UI_TOK_TEXT_PLACEHOLDER);
        gl2d_draw_string((float)tx, (float)line_y, "(no output)", FONT_SMALL);
    } else {
        int max_chars = (panel_w - 2 * UI_CONSOLE_PAD) / FONT_SMALL_W;
        if (max_chars < 1) max_chars = 1;
        int display_count = view->console.count > CONSOLE_MAX_DISPLAY_LINES
                          ? CONSOLE_MAX_DISPLAY_LINES : view->console.count;
        for (int i = 0; i < display_count; i++) {
            char line_buf[64];
            const char *src = view->console.lines[i].text;
            int src_len = (int)strlen(src);
            if (src_len > max_chars && max_chars > 3) {
                snprintf(line_buf, sizeof(line_buf), "%.*s...", max_chars - 3, src);
            } else {
                snprintf(line_buf, sizeof(line_buf), "%.*s", max_chars, src);
            }
            ui_clr(UI_TOK_TEXT_PRIMARY);
            gl2d_draw_string((float)tx, (float)line_y, line_buf, FONT_SMALL);
            line_y -= UI_CONSOLE_LINE_H;
        }
    }

    gl2d_end();
}
