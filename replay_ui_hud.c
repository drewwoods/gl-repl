/*
 * replay_ui_hud.c - replay subsystem's UI surface (status HUD overlay).
 *
 * Feature-owned UI under the `replay_ui_*` prefix. May know replay
 * concepts (mode / PC / play-paused-done / speed / expand) and read
 * the replay snapshot. Must not own unrelated editor / REPL state and
 * must not call parser / compile / apply. Enforced by
 * `scripts/check-replay-ui-isolation.sh`.
 */
#include "replay_ui_hud.h"
#include "./include/gl_2d.h"
#include "replay.h"
#include "ui/layout.h" /* CODE_PANEL_LAYOUT_TOP enum value */
#include "ui/metrics.h"

#include <stdio.h>
#include <string.h>

void replay_ui_hud_render(const UiReplayHudState *state) {
    char progress_txt[64];
    char kbd_txt[128];
    float progress = 0.0f;
    int scene_x = state->scene_x;
    int scene_y = state->scene_y;
    int scene_w = state->scene_w;
    int scene_h = state->scene_h;
    int hud_x = scene_x + REPLAY_HUD_MARGIN_X;
    /* Lifted by STATUSBAR_H so the HUD clears the amber status strip along
     * the bottom of the scene. */
    int hud_y = scene_y + REPLAY_HUD_MARGIN_Y + STATUSBAR_H;
    int hud_w = scene_w - 2 * REPLAY_HUD_MARGIN_X;
    int min_y = scene_y + STATUSBAR_H + 4;
    int max_y = scene_y + scene_h - REPLAY_HUD_HEIGHT - 4;

    if (!state->replaying)
        return;

    if (hud_w < REPLAY_HUD_MIN_WIDTH)
        hud_w = REPLAY_HUD_MIN_WIDTH;
    if (max_y >= min_y) {
        if (hud_y < min_y) hud_y = min_y;
        if (hud_y > max_y) hud_y = max_y;
    } else {
        hud_y = state->code_panel_layout == CODE_PANEL_LAYOUT_TOP
              ? scene_y + scene_h - REPLAY_HUD_HEIGHT - 4
              : min_y;
    }
    if (state->replay_total_cmds > 0)
        progress = (float)state->replay_pc / (float)state->replay_total_cmds;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    glViewport(0, 0, state->viewport_w, state->viewport_h);
    gl2d_begin(state->viewport_w, state->viewport_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Panel bg matches the menubar palette: #1d1d1d with subtle green tint
     * on the border so the HUD reads as paired with the green Replay button. */
    glColor4f(0.114f, 0.118f, 0.114f, 0.94f); /* #1d1e1d */
    glRectf((float)(hud_x), (float)(hud_y), (float)(hud_x)+(float)(hud_w), (float)(hud_y)+(float)(REPLAY_HUD_HEIGHT));
    glColor4f(0.188f, 0.298f, 0.220f, 0.95f); /* #304c38 */
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

    glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 1.0f);
    if (state->replay_state_val == REPLAY_PLAYING) {
        float bw = 3.0f, gap = 3.0f;
        float by0 = (float)icon_cy - (float)icon_sz * 0.5f;
        glRectf((float)(icon_cx - bw - gap * 0.5f), (float)(by0), (float)(icon_cx - bw - gap * 0.5f) + (float)(bw), (float)(by0) + (float)(icon_sz));
        glRectf((float)(icon_cx + gap * 0.5f),      (float)(by0), (float)(icon_cx + gap * 0.5f) + (float)(bw), (float)(by0) + (float)(icon_sz));
    } else if (state->replay_state_val == REPLAY_DONE) {
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
     * is right-aligned so 4-digit totals don't push other fields around. */
    snprintf(progress_txt, sizeof(progress_txt),
             "Replay  %11.1f cmd/s  | %7s  | %s",
             state->replay_speed,
             state->replay_mode == REPLAY_MODE_VERTEX ? "Vertex" : "Polygon",
             state->replay_expand_args ? "Code Expanded" : ""
             );
    glColor3f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B);
    gl2d_draw_string((float)text_col_x,
                (float)(hud_y + REPLAY_HUD_TEXT_LINE1_Y),
                progress_txt, FONT_SMALL);

    char count_txt[32];
    snprintf(count_txt, sizeof(count_txt), "%d / %d",
             state->replay_pc, state->replay_total_cmds);
    int count_w = (int)strlen(count_txt) * FONT_SMALL_W;
    gl2d_draw_string((float)(hud_x + hud_w - REPLAY_HUD_TEXT_PAD_X - count_w),
                (float)(hud_y + REPLAY_HUD_TEXT_LINE1_Y),
                count_txt, FONT_SMALL);

    /* Progress groove + green fill */
    int groove_x = hud_x + REPLAY_HUD_TEXT_PAD_X;
    int groove_w = hud_w - 2 * REPLAY_HUD_TEXT_PAD_X;
    int groove_y = hud_y + REPLAY_HUD_PROGRESS_Y;
    glColor4f(0.094f, 0.118f, 0.102f, 1.0f);  /* #181e1a */
    glRectf((float)(groove_x), (float)(groove_y),
              (float)(groove_x) + (float)(groove_w), (float)(groove_y) + (float)(REPLAY_HUD_PROGRESS_H));
    glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 1.0f);
    glRectf((float)(groove_x), (float)(groove_y),
              (float)(groove_x) + (float)(groove_w * progress), (float)(groove_y) + (float)(REPLAY_HUD_PROGRESS_H));
    glColor4f(0.227f, 0.298f, 0.243f, 1.0f);  /* #3a4c3e border */
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)groove_x + 0.5f,                     (float)groove_y + 0.5f);
    glVertex2f((float)(groove_x + groove_w) - 0.5f,        (float)groove_y + 0.5f);
    glVertex2f((float)(groove_x + groove_w) - 0.5f,        (float)(groove_y + REPLAY_HUD_PROGRESS_H) - 0.5f);
    glVertex2f((float)groove_x + 0.5f,                     (float)(groove_y + REPLAY_HUD_PROGRESS_H) - 0.5f);
    glEnd();

    /* Line 2 - compact kbd hints along the bottom in muted gray */
    snprintf(kbd_txt, sizeof(kbd_txt),
             "Space pause  |  +/- speed  |  m mode  |  e expand |  %c %c step |  Esc stop", 0xAB, 0xBB);
    glColor3f(0.533f, 0.533f, 0.533f);  /* #888 */
    gl2d_draw_string((float)text_col_x,
                (float)(hud_y + REPLAY_HUD_TEXT_LINE2_Y),
                kbd_txt, FONT_SMALL);

    glDisable(GL_BLEND);
    gl2d_end();
}