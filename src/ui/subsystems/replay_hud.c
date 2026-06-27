/*
 * replay_ui_hud.c - replay subsystem's UI surface (status HUD overlay).
 *
 * Feature-owned UI under the `replay_ui_*` prefix. May know replay
 * concepts (mode / PC / play-paused-done / speed / expand) and read
 * the replay snapshot. Must not own unrelated editor / REPL state and
 * must not call parser / compile / apply. Enforced by
 * `scripts/check/check-replay-ui-isolation.sh`.
 */
#include "ui/subsystems/replay_hud.h"
#include "ui/app/snapshot.h"
#include "ui/core/gl_2d.h"
#include "subsystems/replay/replay.h"
#include "ui/app/layout.h" /* CODE_PANEL_LAYOUT_TOP enum value, ui_layout_scene_rect */
#include "ui/core/metrics.h"
#include "ui/core/theme.h"

#include <stdio.h>
#include <string.h>

void replay_ui_hud_render(const struct UiRenderSnapshot *snap) {
    char progress_txt[96];
    char kbd_txt[128];
    float progress = 0.0f;
    int render3d_x, render3d_y, render3d_w, render3d_h;

    if (!snap || !snap->replay.active)
        return;

    /* Scene rect comes from the live layout module like the other panel
     * renderers (variable_panel does the same); other inputs come from
     * the per-frame snapshot. */
    ui_layout_scene_rect(&render3d_x, &render3d_y, &render3d_w, &render3d_h);

    int hud_x = render3d_x + REPLAY_HUD_MARGIN_X;
    /* Lifted by STATUSBAR_H so the HUD clears the amber status strip along
     * the bottom of the scene. */
    int hud_y = render3d_y + REPLAY_HUD_MARGIN_Y + STATUSBAR_H;
    int hud_w = render3d_w - 2 * REPLAY_HUD_MARGIN_X;

    if (hud_w < REPLAY_HUD_MIN_WIDTH)
        hud_w = REPLAY_HUD_MIN_WIDTH;
    hud_y = ui_clamp_panel_y(render3d_y, render3d_h, REPLAY_HUD_HEIGHT, hud_y,
                             snap->code_panel.layout_mode == CODE_PANEL_LAYOUT_TOP,
                             STATUSBAR_H, 4);
    if (snap->replay.total_flat_cmds > 0)
        progress = (float)snap->replay.pc / (float)snap->replay.total_flat_cmds;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    glViewport(0, 0, snap->viewport.window_w, snap->viewport.window_h);
    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Neutral menubar surface; accent-glow border so the HUD reads as
     * paired with the accent Replay button (both follow the theme). */
    ui_clr_a(UI_TOK_SURFACE, 0.94f);
    glRectf((float)(hud_x), (float)(hud_y), (float)(hud_x)+(float)(hud_w), (float)(hud_y)+(float)(REPLAY_HUD_HEIGHT));
    ui_clr_a(UI_TOK_ACCENT_GLOW_BG, 0.95f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)hud_x + 0.5f,                      (float)hud_y + 0.5f);
    glVertex2f((float)(hud_x + hud_w) - 0.5f,            (float)hud_y + 0.5f);
    glVertex2f((float)(hud_x + hud_w) - 0.5f,            (float)(hud_y + REPLAY_HUD_HEIGHT) - 0.5f);
    glVertex2f((float)hud_x + 0.5f,                      (float)(hud_y + REPLAY_HUD_HEIGHT) - 0.5f);
    glEnd();

    /* Column layout: icon in a fixed gutter, both text rows share one
     * left edge so line1 (green status) and line2 (kbd hints) align. */
    int text_col_x = hud_x + REPLAY_HUD_TEXT_PAD_X + 18;   /* after 18px icon gutter */
    int icon_cx    = hud_x + REPLAY_HUD_TEXT_PAD_X + 7;    /* centered in gutter */
    int icon_cy    = hud_y + REPLAY_HUD_TEXT_LINE1_Y + FONT_SMALL_H / 2;
    int icon_sz    = 10;

    ui_clr(UI_TOK_ACCENT);
    if (snap->replay.state == REPLAY_PLAYING) {
        float bw = 3.0f, gap = 3.0f;
        float by0 = (float)icon_cy - (float)icon_sz * 0.5f;
        glRectf((float)(icon_cx - bw - gap * 0.5f), (float)(by0), (float)(icon_cx - bw - gap * 0.5f) + (float)(bw), (float)(by0) + (float)(icon_sz));
        glRectf((float)(icon_cx + gap * 0.5f),      (float)(by0), (float)(icon_cx + gap * 0.5f) + (float)(bw), (float)(by0) + (float)(icon_sz));
    } else if (snap->replay.state == REPLAY_DONE) {
        /* Square - run complete */
        float sx = (float)icon_cx - (float)icon_sz * 0.5f;
        float sy = (float)icon_cy - (float)icon_sz * 0.5f;
        glRectf((float)(sx), (float)(sy), (float)(sx) + (float)(icon_sz), (float)(sy) + (float)(icon_sz));
    } else {
        /* Play triangle - paused / stopped, click to (re)start */
        float x0 = (float)icon_cx - (float)icon_sz * 0.5f;
        float cy = (float)icon_cy;
        glBegin(GL_TRIANGLES);
        glVertex2f(x0,              cy - (float)icon_sz * 0.5f);
        glVertex2f(x0,              cy + (float)icon_sz * 0.5f);
        glVertex2f(x0 + icon_sz,    cy);
        glEnd();
    }

    /* Line 1 - "Replay  4.0 cmd/s | Polygon" in green; command count
     * is right-aligned so 4-digit totals don't push other fields around.
     * A "depth N" segment appears only when the focused command came from a
     * funcN(...) call, so recursion/nesting depth is visible while stepping. */
    char depth_seg[24] = "";
    if (snap->replay.focus_call_depth > 0)
        snprintf(depth_seg, sizeof(depth_seg), "  | depth %d",
                 snap->replay.focus_call_depth);
    snprintf(progress_txt, sizeof(progress_txt),
             "Replay  %11.1f cmd/s  | %7s  | %s%s",
             snap->replay.speed,
             snap->replay.mode == REPLAY_MODE_VERTEX ? "Vertex" : "Polygon",
             snap->replay.expand_args ? "Code Expanded" : "",
             depth_seg
             );
    ui_clr(UI_TOK_ACCENT);
    gl2d_draw_string((float)text_col_x,
                (float)(hud_y + REPLAY_HUD_TEXT_LINE1_Y),
                progress_txt, FONT_SMALL);

    char count_txt[32];
    snprintf(count_txt, sizeof(count_txt), "%d / %d",
             snap->replay.pc, snap->replay.total_flat_cmds);
    int count_w = (int)strlen(count_txt) * FONT_SMALL_W;
    gl2d_draw_string((float)(hud_x + hud_w - REPLAY_HUD_TEXT_PAD_X - count_w),
                (float)(hud_y + REPLAY_HUD_TEXT_LINE1_Y),
                count_txt, FONT_SMALL);

    /* Progress groove + green fill */
    int groove_x = hud_x + REPLAY_HUD_TEXT_PAD_X;
    int groove_w = hud_w - 2 * REPLAY_HUD_TEXT_PAD_X;
    int groove_y = hud_y + REPLAY_HUD_PROGRESS_Y;
    ui_clr(UI_TOK_SUNKEN);
    glRectf((float)(groove_x), (float)(groove_y),
              (float)(groove_x) + (float)(groove_w), (float)(groove_y) + (float)(REPLAY_HUD_PROGRESS_H));
    ui_clr(UI_TOK_ACCENT);
    glRectf((float)(groove_x), (float)(groove_y),
              (float)(groove_x) + (float)(groove_w * progress), (float)(groove_y) + (float)(REPLAY_HUD_PROGRESS_H));
    ui_clr(UI_TOK_ACCENT_GLOW_BG);  /* accent-glow groove border */
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)groove_x + 0.5f,                     (float)groove_y + 0.5f);
    glVertex2f((float)(groove_x + groove_w) - 0.5f,        (float)groove_y + 0.5f);
    glVertex2f((float)(groove_x + groove_w) - 0.5f,        (float)(groove_y + REPLAY_HUD_PROGRESS_H) - 0.5f);
    glVertex2f((float)groove_x + 0.5f,                     (float)(groove_y + REPLAY_HUD_PROGRESS_H) - 0.5f);
    glEnd();

    /* Line 2 - compact kbd hints along the bottom in muted gray */
    snprintf(kbd_txt, sizeof(kbd_txt),
             /* 0xAB / 0xBB = Latin-1 « / » step-direction arrows */
             "Space pause  |  +/- speed  |  m mode  |  e expand |  %c %c step |  Esc stop", 0xAB, 0xBB);
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)text_col_x,
                (float)(hud_y + REPLAY_HUD_TEXT_LINE2_Y),
                kbd_txt, FONT_SMALL);

    glDisable(GL_BLEND);
    gl2d_end();
}