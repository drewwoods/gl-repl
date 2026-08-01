/*
 * src/ui/app/command_description_panel.c -- Floating GL command help card.
 *
 * Pure renderer over UiCommandDescriptionPanelView. The controller resolves
 * the authored/embedded catalog entry and owns popup lifetime; this module
 * only wraps text, clamps the card into the window, and draws it.
 */
#include "ui/app/command_description_panel.h"

#include <stddef.h>
#include <string.h>

#include "ui/app/layout.h"
#include "ui/core/gl_2d.h"
#include "ui/core/metrics.h"
#include "ui/core/theme.h"

#define CDP_EDGE_MARGIN  8
#define CDP_PAD_X       10
#define CDP_PAD_Y        7
#define CDP_MAX_CHARS   64
#define CDP_MAX_LINES   18
#define CDP_LINE_MAX    (CDP_MAX_CHARS + 1)

typedef struct {
    int px, py;
    int popup_w, popup_h;
    int body_count;
    int title_chars;
    char body[CDP_MAX_LINES][CDP_LINE_MAX];
} CdpLayout;

static int cdp_min(int a, int b) { return a < b ? a : b; }

static void cdp_push_line(CdpLayout *out, const char *text, int len) {
    if (out->body_count >= CDP_MAX_LINES)
        return;
    if (len > CDP_LINE_MAX - 1)
        len = CDP_LINE_MAX - 1;
    if (len > 0)
        memcpy(out->body[out->body_count], text, (size_t)len);
    out->body[out->body_count][len] = '\0';
    out->body_count++;
}

/* Word-wrap into fixed renderer-owned rows. Explicit newlines force a row;
 * overlong individual words are split rather than overflowing the card. */
static void cdp_wrap_body(CdpLayout *out, const char *body, int max_chars) {
    const char *p = body;
    char line[CDP_LINE_MAX];
    int line_len = 0;

    while (*p && out->body_count < CDP_MAX_LINES) {
        const char *word;
        int word_len;

        if (*p == '\n') {
            cdp_push_line(out, line, line_len);
            line_len = 0;
            p++;
            continue;
        }
        if (*p == ' ' || *p == '\t') {
            p++;
            continue;
        }

        word = p;
        while (*p && *p != '\n' && *p != ' ' && *p != '\t')
            p++;
        word_len = (int)(p - word);

        while (word_len > 0 && out->body_count < CDP_MAX_LINES) {
            int gap = line_len > 0 ? 1 : 0;
            int room = max_chars - line_len - gap;
            int take;
            if (room <= 0) {
                cdp_push_line(out, line, line_len);
                line_len = 0;
                continue;
            }
            if (word_len > room && line_len > 0) {
                cdp_push_line(out, line, line_len);
                line_len = 0;
                continue;
            }
            take = cdp_min(word_len, room);
            if (gap)
                line[line_len++] = ' ';
            memcpy(line + line_len, word, (size_t)take);
            line_len += take;
            line[line_len] = '\0';
            word += take;
            word_len -= take;
            if (word_len > 0) {
                cdp_push_line(out, line, line_len);
                line_len = 0;
            }
        }
    }
    if (line_len > 0 && out->body_count < CDP_MAX_LINES)
        cdp_push_line(out, line, line_len);
}

static int cdp_solve(const UiCommandDescriptionPanelView *view,
                     CdpLayout *out) {
    int menu_y = 0;
    int left, right, bottom, top;
    int available_w, max_chars, max_rows, longest, i;

    memset(out, 0, sizeof(*out));
    if (!view || !view->visible || !view->title || !view->body)
        return 0;
    if (view->window_w <= 0 || view->window_h <= 0)
        return 0;

    /* The card is clamped to the window, not to the code panel it describes a
     * row of — same box as the state inspector next door. It used to stop at
     * the panel's bottom edge, which meant a card anchored on one of the last
     * rows was pushed *up* over the code the reader had just right-clicked in;
     * hanging down over the scene hides nothing they were reading. Nothing
     * routes through the card either — it has no hit-test surface and the next
     * key or click dismisses it — so overlapping the viewport cannot swallow a
     * camera drag. The menu bar stays the ceiling. */
    left = CDP_EDGE_MARGIN;
    right = view->window_w - CDP_EDGE_MARGIN;
    bottom = CDP_EDGE_MARGIN;
    ui_layout_menu_bar_rect(NULL, &menu_y, NULL, NULL);
    top = (menu_y > bottom && menu_y < view->window_h)
              ? menu_y - 2 : view->window_h - CDP_EDGE_MARGIN;
    if (right <= left || top <= bottom)
        return 0;

    available_w = right - left;
    max_chars = (available_w - 2 * CDP_PAD_X) / FONT_W;
    if (max_chars > CDP_MAX_CHARS) max_chars = CDP_MAX_CHARS;
    if (max_chars < 12) return 0;

    cdp_wrap_body(out, view->body, max_chars);
    if (out->body_count <= 0)
        return 0;

    max_rows = (top - bottom - 2 * CDP_PAD_Y - 3) / LINE_H;
    if (max_rows < 2)
        return 0;
    if (out->body_count > max_rows - 1) {
        out->body_count = max_rows - 1;
        if (out->body_count > 0) {
            char *last = out->body[out->body_count - 1];
            int len = (int)strlen(last);
            if (len > max_chars - 3) len = max_chars - 3;
            memcpy(last + len, "...", 4);
        }
    }

    out->title_chars = (int)strlen(view->title);
    if (out->title_chars > max_chars) out->title_chars = max_chars;
    longest = out->title_chars;
    for (i = 0; i < out->body_count; i++) {
        int len = (int)strlen(out->body[i]);
        if (len > longest) longest = len;
    }
    out->popup_w = longest * FONT_W + 2 * CDP_PAD_X;
    out->popup_h = 2 * CDP_PAD_Y +
                   (1 + out->body_count) * LINE_H + 3;

    out->px = view->anchor_px + 12;
    if (out->px + out->popup_w > right) out->px = right - out->popup_w;
    if (out->px < left) out->px = left;
    out->py = view->anchor_py;
    if (out->py > top) out->py = top;
    if (out->py - out->popup_h < bottom)
        out->py = bottom + out->popup_h;
    return 1;
}

static void cdp_draw_clipped_title(const char *title, int chars,
                                   float x, float y) {
    char clipped[CDP_LINE_MAX];
    if ((int)strlen(title) <= chars) {
        gl2d_draw_string(x, y, title, FONT_MONO);
        return;
    }
    memcpy(clipped, title, (size_t)chars);
    clipped[chars] = '\0';
    if (chars >= 2) {
        clipped[chars - 2] = '.';
        clipped[chars - 1] = '.';
    }
    gl2d_draw_string(x, y, clipped, FONT_MONO);
}

void ui_command_description_panel_render(
    const UiCommandDescriptionPanelView *view) {
    CdpLayout lo;
    int ty, i;

    if (!cdp_solve(view, &lo))
        return;

    gl2d_begin(view->window_w, view->window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)lo.px, (float)(lo.py - lo.popup_h),
                     (float)lo.popup_w, (float)lo.popup_h,
                     UI_TOK_RAISED, 0.97f, UI_TOK_BORDER, 0.88f);

    ty = lo.py - CDP_PAD_Y - LINE_H + 5;
    ui_clr(UI_TOK_TEXT_SECTION);
    cdp_draw_clipped_title(view->title, lo.title_chars,
                           (float)(lo.px + CDP_PAD_X), (float)ty);
    ui_clr(UI_TOK_DIVIDER);
    glRectf((float)(lo.px + 1), (float)(ty - 4),
            (float)(lo.px + lo.popup_w - 1), (float)(ty - 3));
    ty -= LINE_H;

    ui_clr(UI_TOK_TEXT_PRIMARY);
    for (i = 0; i < lo.body_count; i++) {
        gl2d_draw_string((float)(lo.px + CDP_PAD_X), (float)ty,
                         lo.body[i], FONT_MONO);
        ty -= LINE_H;
    }

    glDisable(GL_BLEND);
    gl2d_end();
}
