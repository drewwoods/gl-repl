/*
 * ui_panels.c - Code-panel delegation, scene status banner, and top-level
 * overlay-priority hit routing.
 */
#include "ui/app/panels.h"

#include "config.h"
#include "ui/subsystems/color_picker.h"
#include "ui/app/numeric_swatch.h"
#include "ui/core/gl_2d.h"
#include "ui/app/layout.h"
#include "ui/app/menu_bar.h"
#include "ui/core/metrics.h"
#include "ui/app/repl_code_panel.h"
#include "ui/app/scene_tabs.h"
#include "ui/subsystems/variable_panel.h"
#include "ui/app/variable_panel_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Two deliberately non-accent semantic surfaces, theme-stable in every
 * scheme (see theme.h's "named constant" bucket - fixed, non-theme
 * one-offs that must NOT follow the UI accent):
 *   - the inline-rename modal: a distinct blue so it reads as modal;
 *   - the bottom status banner: amber, the conventional status hue. */
static const float k_rename_bar_bg[4]   = { 0.078f, 0.122f, 0.298f, 0.95f };
static const float k_rename_bar_rule[4] = { 0.310f, 0.510f, 0.860f, 1.0f };
static const float k_rename_bar_text[4] = { 0.780f, 0.870f, 1.000f, 1.0f };
static const float k_status_bar_bg[3]   = { 0.227f, 0.165f, 0.063f };
static const float k_status_bar_edge[3] = { 0.102f, 0.071f, 0.031f };
static const float k_status_bar_fg[3]   = { 0.941f, 0.753f, 0.439f };
/* Parallel red palette for UI_STATUS_ERROR. Same luminance band as the
 * amber set above so the bar still reads against the scene background;
 * only the hue differs. */
static const float k_status_bar_bg_err[3]   = { 0.290f, 0.078f, 0.078f };
static const float k_status_bar_edge_err[3] = { 0.137f, 0.027f, 0.027f };
static const float k_status_bar_fg_err[3]   = { 1.000f, 0.557f, 0.494f };

/* Messages-button + recent-message-list palette. Neutral surfaces (not
 * theme tokens) so the affordance reads the same across schemes, matching
 * the deliberately non-accent status banner above. */
static const float k_msgbtn_bg[4]    = { 0.137f, 0.137f, 0.157f, 0.95f };
static const float k_msgbtn_bg_lit[4]= { 0.204f, 0.204f, 0.243f, 0.97f };
static const float k_msgbtn_edge[4]  = { 0.392f, 0.392f, 0.451f, 1.0f };
static const float k_msgbtn_fg[3]    = { 0.780f, 0.780f, 0.820f };
static const float k_msglist_bg[4]   = { 0.090f, 0.090f, 0.110f, 0.96f };
static const float k_msglist_edge[4] = { 0.353f, 0.353f, 0.404f, 1.0f };

void ui_panels_render_code_panel(const UiRenderSnapshot *snap,
                                 UiCodePanelOutput *out) {
    ui_repl_code_panel_render_with_chrome(snap, out);
}

/* Geometry the bottom-bar strip helpers compute once at open. Callers
 * keep it on the stack across the begin / paint / extra-draws / close
 * sequence. Defined here so the three strip clients (modal-prompt
 * bar, status banner) all reach for the same shape. */
typedef struct {
    int sc_x, sc_y, sc_w, sc_h;
    int bar_y, bar_h;
    int text_y;
} StatusStripFrame;

/* Compute the strip geometry, set up gl2d + blend. Returns 0 (no
 * draw) on a degenerate scene rect — callers should bail without
 * a matching close, matching `gl2d_begin` lifetime. */
static int status_strip_begin(const UiRenderSnapshot *snap,
                              StatusStripFrame *f) {
    ui_layout_scene_rect(&f->sc_x, &f->sc_y, &f->sc_w, &f->sc_h);
    if (f->sc_w <= 0 || f->sc_h <= 0)
        return 0;
    f->bar_h  = STATUSBAR_H;
    f->bar_y  = f->sc_y;
    f->text_y = f->bar_y + (f->bar_h - FONT_SMALL_H) / 2 + 1;
    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return 1;
}

/* Paint the strip background rectangle plus its top rule. Both
 * colors carry their own alpha. Caller can layer text / badges /
 * etc. afterward before `status_strip_end`. */
static void status_strip_paint_bar(const StatusStripFrame *f,
                                   const float bg[4],
                                   const float rule[4]) {
    glColor4fv(bg);
    glRectf((float)f->sc_x, (float)f->bar_y,
            (float)(f->sc_x + f->sc_w), (float)(f->bar_y + f->bar_h));
    glColor4fv(rule);
    glBegin(GL_LINES);
    glVertex2f((float)f->sc_x, (float)(f->bar_y + f->bar_h));
    glVertex2f((float)(f->sc_x + f->sc_w), (float)(f->bar_y + f->bar_h));
    glEnd();
}

static void status_strip_end(void) {
    glDisable(GL_BLEND);
    gl2d_end();
}

static void draw_modal_strip(const UiRenderSnapshot *snap,
                             const char *msg, int msg_len, int max_buf) {
    StatusStripFrame f;
    int tx, max_px, max_chars;
    char trunc[320];

    if (!status_strip_begin(snap, &f))
        return;
    status_strip_paint_bar(&f, k_rename_bar_bg, k_rename_bar_rule);

    tx = f.sc_x + CODE_MARGIN_X;
    max_px = f.sc_w - 2 * CODE_MARGIN_X;
    max_chars = max_px / FONT_SMALL_W;
    if (max_chars < 8)
        max_chars = 8;
    if (max_chars > max_buf - 1)
        max_chars = max_buf - 1;
    if (msg_len > max_chars) {
        snprintf(trunc, sizeof(trunc), "%.*s", max_chars, msg);
        msg = trunc;
    }
    glColor4fv(k_rename_bar_text);
    gl2d_draw_string((float)tx, (float)f.text_y, msg, FONT_SMALL);

    status_strip_end();
}

/* --- Recent-message ("messages") button + history list --- */

enum {
    MSGBTN_PAD_X       = 8,   /* label inset each side */
    MSGLIST_ROW_H      = LINE_H,
    MSGLIST_PAD        = 4,    /* inner padding around the list text block */
    MSGLIST_MAX_ROWS   = 10,   /* visible rows cap, also clamped to fit */
    MSGLIST_MIN_W      = 200,
    MSGLIST_MAX_W      = 460,
};

/* Build the button label ("Messages" or "Messages (N)") into buf. */
static void status_history_button_label(const UiRenderSnapshot *snap,
                                         char *buf, int cap) {
    int n = snap->status_history.count;
    if (n > 0)
        snprintf(buf, (size_t)cap, "Messages (%d)", n);
    else
        snprintf(buf, (size_t)cap, "Messages");
}

int ui_panels_status_history_button_rect(const UiRenderSnapshot *snap,
                                          int *x, int *y, int *w, int *h) {
    int sc_x, sc_y, sc_w, sc_h;
    int bw, bh;
    char label[32];

    if (!snap)
        return 0;
    /* Nothing to show until at least one message has been recorded; the
     * ring is session-retained, so once it appears the button stays. */
    if (snap->status_history.count <= 0)
        return 0;
    /* The modal prompt strips own the bottom band; no button while active. */
    if (snap->rename_active || snap->file_prompt_active)
        return 0;

    ui_layout_scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    if (sc_w <= 0 || sc_h <= 0)
        return 0;

    status_history_button_label(snap, label, (int)sizeof(label));
    bw = (int)strlen(label) * FONT_SMALL_W + 2 * MSGBTN_PAD_X;
    bh = STATUSBAR_H;
    if (bw > sc_w)
        bw = sc_w;

    if (x) *x = sc_x + sc_w - bw;     /* flush to the scene's right edge */
    if (y) *y = sc_y;                 /* sits in the status-bar band */
    if (w) *w = bw;
    if (h) *h = bh;
    return 1;
}

/* List rect (OpenGL bottom-left coords), grown upward from the button.
 * Returns the number of visible rows (0 when empty / no room). */
static int status_history_list_rect(const UiRenderSnapshot *snap,
                                     int *x, int *y, int *w, int *h,
                                     int *visible_rows) {
    int bx, by, bw, bh;
    int sc_x, sc_y, sc_w, sc_h;
    int count, vis, room_rows, lw, lh, lx, ly;

    if (!ui_panels_status_history_button_rect(snap, &bx, &by, &bw, &bh))
        return 0;
    ui_layout_scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);

    count = snap->status_history.count;
    if (count <= 0)
        return 0;

    vis = count;
    if (vis > MSGLIST_MAX_ROWS)
        vis = MSGLIST_MAX_ROWS;
    /* Clamp to the space above the bar so the list never grows off-screen. */
    room_rows = (sc_y + sc_h - (by + bh) - 2 * MSGLIST_PAD) / MSGLIST_ROW_H;
    if (room_rows < 1)
        room_rows = 1;
    if (vis > room_rows)
        vis = room_rows;

    lw = MSGLIST_MAX_W;
    if (lw > sc_w - 2 * CODE_MARGIN_X)
        lw = sc_w - 2 * CODE_MARGIN_X;
    if (lw < MSGLIST_MIN_W)
        lw = MSGLIST_MIN_W;
    lh = vis * MSGLIST_ROW_H + 2 * MSGLIST_PAD;

    lx = bx + bw - lw;                /* right-align list under the button */
    if (lx < sc_x)
        lx = sc_x;
    ly = by + bh;                     /* directly above the button */

    if (x) *x = lx;
    if (y) *y = ly;
    if (w) *w = lw;
    if (h) *h = lh;
    if (visible_rows) *visible_rows = vis;
    return vis;
}

/* Draw the persistent messages button. Lit when history is open or
 * non-empty so it reads as a live affordance. */
static void status_history_render_button(const UiRenderSnapshot *snap) {
    int bx, by, bw, bh;
    char label[32];
    const float *bg;

    if (!ui_panels_status_history_button_rect(snap, &bx, &by, &bw, &bh))
        return;

    status_history_button_label(snap, label, (int)sizeof(label));
    bg = (snap->status_history.open || snap->status_history.count > 0)
             ? k_msgbtn_bg_lit : k_msgbtn_bg;

    glColor4fv(bg);
    glRectf((float)bx, (float)by, (float)(bx + bw), (float)(by + bh));
    glColor4fv(k_msgbtn_edge);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)bx + 0.5f, (float)by + 0.5f);
    glVertex2f((float)(bx + bw) - 0.5f, (float)by + 0.5f);
    glVertex2f((float)(bx + bw) - 0.5f, (float)(by + bh) - 0.5f);
    glVertex2f((float)bx + 0.5f, (float)(by + bh) - 0.5f);
    glEnd();

    int text_y = by + (bh - FONT_SMALL_H) / 2 + 1;
    glColor3fv(k_msgbtn_fg);
    gl2d_draw_string((float)(bx + MSGBTN_PAD_X), (float)text_y,
                     label, FONT_SMALL);
}

/* Draw the inline history list when the toggle is open. Newest entry sits
 * at the bottom (just above the button); older rows stack upward, dimmed
 * by age. ERROR entries take the red status hue, INFO the amber. */
static void status_history_render_list(const UiRenderSnapshot *snap) {
    const UiStatusHistory *hist = &snap->status_history;
    int lx, ly, lw, lh, vis;
    int max_chars, r;

    if (!hist->open)
        return;
    vis = status_history_list_rect(snap, &lx, &ly, &lw, &lh, NULL);
    if (vis <= 0)
        return;

    glColor4fv(k_msglist_bg);
    glRectf((float)lx, (float)ly, (float)(lx + lw), (float)(ly + lh));
    glColor4fv(k_msglist_edge);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)lx + 0.5f, (float)ly + 0.5f);
    glVertex2f((float)(lx + lw) - 0.5f, (float)ly + 0.5f);
    glVertex2f((float)(lx + lw) - 0.5f, (float)(ly + lh) - 0.5f);
    glVertex2f((float)lx + 0.5f, (float)(ly + lh) - 0.5f);
    glEnd();

    max_chars = (lw - 2 * MSGLIST_PAD) / FONT_SMALL_W;
    if (max_chars < 8)
        max_chars = 8;
    if (max_chars > REPL_STATUS_TEXT_MAX - 16)
        max_chars = REPL_STATUS_TEXT_MAX - 16;

    /* r = 0 is the newest, drawn on the bottom row just above the button. */
    for (r = 0; r < vis; r++) {
        int ordinal = hist->count - 1 - r;
        int idx = ui_status_history_index(hist, ordinal);
        const UiStatusEntry *e;
        const float *fg;
        float dim;
        int ty;
        char line[REPL_STATUS_TEXT_MAX];

        if (idx < 0)
            continue;
        e = &hist->entries[idx];
        fg = (e->kind == UI_STATUS_ERROR) ? k_status_bar_fg_err
                                          : k_status_bar_fg;
        /* Newest at full strength; older rows fade toward 0.45 alpha. */
        dim = 1.0f - 0.55f * ((float)r / (float)(vis > 1 ? vis - 1 : 1));

        if (e->dup_count > 1)
            snprintf(line, sizeof(line), "%.*s (x%u)",
                     max_chars - 8, e->text, e->dup_count);
        else
            snprintf(line, sizeof(line), "%.*s", max_chars, e->text);

        ty = ly + MSGLIST_PAD + (vis - 1 - r) * MSGLIST_ROW_H
             + (MSGLIST_ROW_H - FONT_SMALL_H) / 2;
        glColor4f(fg[0], fg[1], fg[2], dim);
        gl2d_draw_string((float)(lx + MSGLIST_PAD), (float)ty, line, FONT_SMALL);
    }
}

/* Classify a pointer over the messages button / open list. item_idx == 1
 * is the button (toggle); item_idx == 0 is the open list body (consume so
 * the click doesn't fall through to the scene behind it). */
static UiHit status_history_hit_test(const UiRenderSnapshot *snap,
                                     int mx, int my) {
    UiHit h = ui_hit_none();
    int win_h = snap->viewport.window_h;
    int ry = win_h - my;
    int bx, by, bw, bh;

    if (ui_panels_status_history_button_rect(snap, &bx, &by, &bw, &bh) &&
        mx >= bx && mx < bx + bw && ry >= by && ry < by + bh) {
        h.kind = UI_HIT_STATUS_HISTORY;
        h.item_idx = 1;
        return h;
    }

    if (snap->status_history.open) {
        int lx, ly, lw, lh;
        if (status_history_list_rect(snap, &lx, &ly, &lw, &lh, NULL) > 0 &&
            mx >= lx && mx < lx + lw && ry >= ly && ry < ly + lh) {
            h.kind = UI_HIT_STATUS_HISTORY;
            h.item_idx = 0;
            return h;
        }
    }
    return h;
}

void ui_panels_render_scene_status(const UiRenderSnapshot *snap) {
    if (snap->rename_active) {
        char msg[REPL_STATUS_TEXT_MAX];
        int n = snprintf(msg, sizeof(msg),
                         "Rename: %s_   [Enter] save   [Esc] cancel",
                         snap->rename_text);
        draw_modal_strip(snap, msg, n, (int)sizeof(msg));
        return;
    }

    if (snap->file_prompt_active) {
        char msg[320];
        int n;
        if (snap->file_prompt_error[0])
            n = snprintf(msg, sizeof(msg),
                         "Load scene: %s_   %s   [Esc] cancel",
                         snap->file_prompt_text, snap->file_prompt_error);
        else
            n = snprintf(msg, sizeof(msg),
                         "Load scene: %s_   [Enter] load   [Esc] cancel",
                         snap->file_prompt_text);
        draw_modal_strip(snap, msg, n, (int)sizeof(msg));
        return;
    }

    /* Persistent messages button + (when open) the recent-message list.
     * Rendered before the transient banner's ttl gate so the button stays
     * visible even with no active message. The list draws under the bar
     * band but is layered first; the button paints on top of both. */
    {
        gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        status_history_render_list(snap);
        status_history_render_button(snap);
        glDisable(GL_BLEND);
        gl2d_end();
    }

    UiStatusState status = snap->status;
    if (status.ttl <= 0 || !status.text[0])
        return;

    {
        StatusStripFrame f;
        float alpha;
        int badge_d, badge_x, badge_y;
        int tx, max_px, max_chars;
        char msg[REPL_STATUS_TEXT_MAX];
        int n;
        const float *bg_rgb;
        const float *edge_rgb;
        const float *fg_rgb;
        float bg[4], rule[4], fg[4];

        if (!status_strip_begin(snap, &f))
            return;

        alpha = status.ttl > UI_STATUS_FADE_FRAMES
                    ? 1.0f
                    : (float)status.ttl / (float)UI_STATUS_FADE_FRAMES;
        if (status.kind == UI_STATUS_ERROR) {
            bg_rgb   = k_status_bar_bg_err;
            edge_rgb = k_status_bar_edge_err;
            fg_rgb   = k_status_bar_fg_err;
        } else {
            bg_rgb   = k_status_bar_bg;
            edge_rgb = k_status_bar_edge;
            fg_rgb   = k_status_bar_fg;
        }
        bg[0] = bg_rgb[0]; bg[1] = bg_rgb[1]; bg[2] = bg_rgb[2]; bg[3] = 0.92f * alpha;
        rule[0] = edge_rgb[0]; rule[1] = edge_rgb[1]; rule[2] = edge_rgb[2]; rule[3] = alpha;
        fg[0] = fg_rgb[0]; fg[1] = fg_rgb[1]; fg[2] = fg_rgb[2]; fg[3] = alpha;

        status_strip_paint_bar(&f, bg, rule);

        badge_d = 14;
        badge_x = f.sc_x + CODE_MARGIN_X;
        badge_y = f.bar_y + (f.bar_h - badge_d) / 2;
        glColor4f(fg[0], fg[1], fg[2], fg[3]);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < 16; i++) {
            float angle = (float)i * (6.2831853f / 16.0f);
            glVertex2f(badge_x + badge_d * 0.5f + cosf(angle) * (badge_d * 0.5f),
                       badge_y + badge_d * 0.5f + sinf(angle) * (badge_d * 0.5f));
        }
        glEnd();
        gl2d_draw_string((float)(badge_x + badge_d * 0.5f - FONT_SMALL_W * 0.5f + 1.0f),
                         (float)f.text_y, "!", FONT_SMALL);

        tx = badge_x + badge_d + 8;
        max_px = f.sc_x + f.sc_w - CODE_MARGIN_X - tx;
        /* Stop short of the right-anchored messages button so the banner
         * text never slides under it. */
        {
            int bx, by, bw, bh;
            if (ui_panels_status_history_button_rect(snap, &bx, &by, &bw, &bh))
                max_px = bx - 8 - tx;
        }
        max_chars = max_px / FONT_SMALL_W;
        if (max_chars < 8)
            max_chars = 8;
        if (max_chars > 255)
            max_chars = 255;

        n = (int)strlen(status.text);
        if (n > max_chars)
            snprintf(msg, sizeof(msg), "%.*s...", max_chars - 3, status.text);
        else
            snprintf(msg, sizeof(msg), "%s", status.text);
        glColor4f(fg[0], fg[1], fg[2], fg[3]);
        gl2d_draw_string((float)tx, (float)f.text_y, msg, FONT_SMALL);

        status_strip_end();
    }
}

UiHit ui_panels_handle_right_press(int mx, int my) {
    return ui_menu_bar_handle_config_right_press(mx, my);
}

UiHit ui_panels_hit_test(const UiRenderSnapshot *snap,
                         int mx, int my, int variable_count) {
    UiHit hit = ui_hit_none();
    int win_w;
    int win_h;
    int gl_y;

    if (!snap)
        return hit;

    win_w = snap->viewport.window_w;
    win_h = snap->viewport.window_h;
    if (win_w <= 0 || win_h <= 0)
        return hit;

    gl_y = win_h - my;

    if (snap->help.visible) {
        /* Help is modal, but keep the statusbar "F1 help" keycap live
         * so a second click dismisses the overlay (mirrors pressing
         * F1 again). Everything else stays captured by the panel. */
        UiHit code_hit = ui_repl_code_panel_hit_test(snap, mx, my);
        if (code_hit.kind == UI_HIT_HELP_TOGGLE)
            return code_hit;
        hit.kind = UI_HIT_HELP_PANEL;
        hit.local_x = (float)mx;
        hit.local_y = (float)gl_y;
        return hit;
    }

    {
        UiHit picker_hit = ui_color_picker_hit_test(&snap->color_picker,
                                                    mx, my, win_h);
        if (picker_hit.kind != UI_HIT_NONE)
            return picker_hit;
    }

    {
        UiHit menu_hit = ui_menu_bar_hit_test(mx, my);
        if (menu_hit.kind != UI_HIT_NONE)
            return menu_hit;
    }

    /* Claim the scene tab strip before the code panel: the band consumes
     * its whole rect (TAB on a tab, CHROME in-band off-tab) so blank-strip
     * clicks never fall through to a code-text/gutter hit. After the
     * menu-bar block so an open dropdown over the band still wins. */
    {
        UiHit tab_hit = ui_scene_tabs_hit_test(snap, mx, my);
        if (tab_hit.kind != UI_HIT_NONE)
            return tab_hit;
    }

    {
        UiVariablePanelView var_view = ui_app_variable_panel_view(snap);
        var_view.var_count = variable_count;   /* honor the caller's count */
        UiHit variable_hit = ui_variable_panel_hit_test(&var_view, mx, my);
        if (variable_hit.kind != UI_HIT_NONE)
            return variable_hit;
    }

    {
        UiHit swatch_hit = ui_numeric_swatch_hit_test(snap, mx, my);
        if (swatch_hit.kind != UI_HIT_NONE)
            return swatch_hit;
    }

    {
        UiHit code_hit = ui_repl_code_panel_hit_test(snap, mx, my);
        if (code_hit.kind != UI_HIT_NONE)
            return code_hit;
    }

    /* Messages button / open history list sit over the bottom-right of
     * the scene, after the code panel so an overlapping panel still wins. */
    {
        UiHit msg_hit = status_history_hit_test(snap, mx, my);
        if (msg_hit.kind != UI_HIT_NONE)
            return msg_hit;
    }

    hit.kind = UI_HIT_SCENE;
    hit.local_x = (float)mx;
    hit.local_y = (float)gl_y;
    return hit;
}
