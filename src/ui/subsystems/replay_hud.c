/*
 * replay_ui_hud.c - replay subsystem's UI surface (status HUD overlay).
 *
 * Feature-owned UI under the `replay_ui_*` prefix. May know replay
 * concepts (mode / PC / play-paused-done / speed / expand / overlays) and read
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

static const char *replay_hud_normal_text(int mode) {
    switch (mode) {
    case REPLAY_NORMAL_DISPLAY_VECTOR:
        return "Vector";
    case REPLAY_NORMAL_DISPLAY_DIRECTION:
        return "Direction";
    default:
        return "";
    }
}

static const char *replay_hud_vertex_label_text(int enabled) {
    return enabled ? "Labelled" : "";
}

static void center_format(char *buf, size_t size, const char *str, int width) {
    int len = (int)strlen(str);
    if (len >= width) {
        snprintf(buf, size, "%s", str);
        return;
    }
    int total_spaces = width - len;
    int left = total_spaces / 2;
    int right = total_spaces - left;
    int pos = 0;
    for (int i = 0; i < left && pos < (int)size - 1; i++) {
        buf[pos++] = ' ';
    }
    for (int i = 0; i < len && pos < (int)size - 1; i++) {
        buf[pos++] = str[i];
    }
    for (int i = 0; i < right && pos < (int)size - 1; i++) {
        buf[pos++] = ' ';
    }
    buf[pos] = '\0';
}

struct ReplayStatusField {
    const char *top;
    const char *bottom;
    int width;
};

static void build_replay_status(char *progress_buf, size_t progress_sz,
                                char *kbd_buf, size_t kbd_sz,
                                float speed, const char *depth_seg,
                                const struct ReplayStatusField *fields, int field_count) {
    snprintf(progress_buf, progress_sz, "Replay  %8.1f cmd/s", speed);
    snprintf(kbd_buf, kbd_sz, "Space pause | +/- speed");

    for (int i = 0; i < field_count; i++) {
        char centered_top[32];
        char centered_bottom[32];
        center_format(centered_top, sizeof(centered_top), fields[i].top, fields[i].width);
        center_format(centered_bottom, sizeof(centered_bottom), fields[i].bottom, fields[i].width);

        const char *progress_sep = (i == 0) ? "  | " : " | ";
        size_t len = strlen(progress_buf);
        if (len < progress_sz) {
            snprintf(progress_buf + len, progress_sz - len, "%s%s", progress_sep, centered_top);
        }

        size_t kbd_len = strlen(kbd_buf);
        if (kbd_len < kbd_sz) {
            snprintf(kbd_buf + kbd_len, kbd_sz - kbd_len, " | %s", centered_bottom);
        }
    }

    size_t len = strlen(progress_buf);
    if (len < progress_sz) {
        snprintf(progress_buf + len, progress_sz - len, "%s", depth_seg);
    }

    size_t kbd_len = strlen(kbd_buf);
    if (kbd_len < kbd_sz) {
        snprintf(kbd_buf + kbd_len, kbd_sz - kbd_len, " | %c %c step | Esc stop", 0xAB, 0xBB);
    }
}

/* Draws the main HUD panel background: a semi-transparent dark gray surface
 * with a thin colored border outline matching the accent theme (accent glow). */
static void draw_hud_panel(int x, int y, int w) {
    ui_clr_a(UI_TOK_SURFACE, 0.94f);
    glRectf((float)x, (float)y, (float)(x + w), (float)(y + REPLAY_HUD_HEIGHT));
    ui_clr_a(UI_TOK_ACCENT_GLOW_BG, 0.95f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)x + 0.5f,       (float)y + 0.5f);
    glVertex2f((float)(x + w) - 0.5f, (float)y + 0.5f);
    glVertex2f((float)(x + w) - 0.5f, (float)(y + REPLAY_HUD_HEIGHT) - 0.5f);
    glVertex2f((float)x + 0.5f,       (float)(y + REPLAY_HUD_HEIGHT) - 0.5f);
    glEnd();
}

/* Draws a 10px status glyph in the left gutter of Line 1:
 * - Two vertical bars for REPLAY_PLAYING (Pause icon).
 * - A solid square for REPLAY_DONE (Stopped/Complete).
 * - A right-pointing triangle for paused/stopped states (Play icon). */
static void draw_playback_icon(int cx, int cy, int sz, int state) {
    ui_clr(UI_TOK_ACCENT);
    if (state == REPLAY_PLAYING) {
        float bw = 3.0f, gap = 3.0f;
        float by0 = (float)cy - (float)sz * 0.5f;
        glRectf((float)(cx - bw - gap * 0.5f), (float)by0, (float)(cx - bw - gap * 0.5f) + bw, by0 + (float)sz);
        glRectf((float)(cx + gap * 0.5f),      (float)by0, (float)(cx + gap * 0.5f) + bw, by0 + (float)sz);
    } else if (state == REPLAY_DONE) {
        /* Square - run complete */
        float sx = (float)cx - (float)sz * 0.5f;
        float sy = (float)cy - (float)sz * 0.5f;
        glRectf(sx, sy, sx + (float)sz, sy + (float)sz);
    } else {
        /* Play triangle - paused / stopped, click to (re)start */
        float x0 = (float)cx - (float)sz * 0.5f;
        glBegin(GL_TRIANGLES);
        glVertex2f(x0,             (float)cy - (float)sz * 0.5f);
        glVertex2f(x0,             (float)cy + (float)sz * 0.5f);
        glVertex2f(x0 + (float)sz, (float)cy);
        glEnd();
    }
}

/* Draws the horizontal progress bar between Line 1 and Line 2:
 * - A sunken dark background track/groove.
 * - A bright accent-colored fill indicating replay progress fraction (pc / total_cmds).
 * - A subtle accent-glow border outline around the groove. */
static void draw_progress_groove(int x, int y, int w, float progress) {
    ui_clr(UI_TOK_SUNKEN);
    glRectf((float)x, (float)y, (float)(x + w), (float)(y + REPLAY_HUD_PROGRESS_H));
    ui_clr(UI_TOK_ACCENT);
    glRectf((float)x, (float)y, (float)(x + (float)w * progress), (float)(y + REPLAY_HUD_PROGRESS_H));
    ui_clr(UI_TOK_ACCENT_GLOW_BG);  /* accent-glow groove border */
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)x + 0.5f,       (float)y + 0.5f);
    glVertex2f((float)(x + w) - 0.5f, (float)y + 0.5f);
    glVertex2f((float)(x + w) - 0.5f, (float)(y + REPLAY_HUD_PROGRESS_H) - 0.5f);
    glVertex2f((float)x + 0.5f,       (float)(y + REPLAY_HUD_PROGRESS_H) - 0.5f);
    glEnd();
}

/* Renders the interactive 2D HUD overlay for the replay controls.
 *
 * Layout Structure:
 * +--------------------------------------------------------------------------------------+
 * | [Icon] Replay <Speed> | <Mode> | <Code Exp> | <Normals> | <Labels>        [PC/Total] |
 * | [=== Progress Bar Groove ==========================================================] |
 * | Space pause | +/- speed | m mode | e expand | n normals | v vertex | « » step ...    |
 * +--------------------------------------------------------------------------------------+
 *
 * - Line 1: The current playback speed, replay mode (Vertex/Polygon), code expansion mode,
 *   normal vector overlay status, vertex index label status, nesting depth, and the command
 *   program counter fraction (right-aligned).
 * - Horizontal center bar: Visual progress groove showing commands executed.
 * - Line 2: Compact key-binding shortcut hints in muted gray.
 */
void replay_ui_hud_render(const struct UiRenderSnapshot *snap) {
    char progress_txt[128];
    char kbd_txt[160];
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

    /* Background panel */
    draw_hud_panel(hud_x, hud_y, hud_w);

    /* Playback icon */
    int text_col_x = hud_x + REPLAY_HUD_TEXT_PAD_X + 18;   /* after 18px icon gutter */
    int icon_cx    = hud_x + REPLAY_HUD_TEXT_PAD_X + 7;    /* centered in gutter */
    int icon_cy    = hud_y + REPLAY_HUD_TEXT_LINE1_Y + FONT_SMALL_H / 2;
    int icon_sz    = 10;
    draw_playback_icon(icon_cx, icon_cy, icon_sz, snap->replay.state);

    /* Status builder and Line 1 Text */
    char depth_seg[24] = "";
    if (snap->replay.focus_call_depth > 0)
        snprintf(depth_seg, sizeof(depth_seg), "  | depth %d",
                 snap->replay.focus_call_depth);

    struct ReplayStatusField fields[4] = {
        {
            .top = snap->replay.mode == REPLAY_MODE_VERTEX ? "Vertex" : "Polygon",
            .bottom = "m mode",
            .width = 7
        },
        {
            .top = snap->replay.expand_args == REPLAY_EXPAND_VERBOSE
                 ? "Code Verbose"
                 : (snap->replay.expand_args == REPLAY_EXPAND_EXPANDED
                    ? "Code Expanded" : ""),
            .bottom = "e expand",
            .width = 13
        },
        {
            .top = replay_hud_normal_text(snap->replay.normal_display),
            .bottom = "n normals",
            .width = 9
        },
        {
            .top = replay_hud_vertex_label_text(snap->replay.vertex_label),
            .bottom = "v vertex",
            .width = 8
        }
    };

    build_replay_status(progress_txt, sizeof(progress_txt),
                        kbd_txt, sizeof(kbd_txt),
                        snap->replay.speed,
                        depth_seg,
                        fields, 4);

    ui_clr(UI_TOK_ACCENT);
    gl2d_draw_string((float)text_col_x,
                (float)(hud_y + REPLAY_HUD_TEXT_LINE1_Y),
                progress_txt, FONT_SMALL);

    /* Command count */
    char count_txt[32];
    snprintf(count_txt, sizeof(count_txt), "%d / %d",
             snap->replay.pc, snap->replay.total_flat_cmds);
    int count_w = (int)strlen(count_txt) * FONT_SMALL_W;
    gl2d_draw_string((float)(hud_x + hud_w - REPLAY_HUD_TEXT_PAD_X - count_w),
                (float)(hud_y + REPLAY_HUD_TEXT_LINE1_Y),
                count_txt, FONT_SMALL);

    /* Progress groove */
    int groove_x = hud_x + REPLAY_HUD_TEXT_PAD_X;
    int groove_w = hud_w - 2 * REPLAY_HUD_TEXT_PAD_X;
    int groove_y = hud_y + REPLAY_HUD_PROGRESS_Y;
    draw_progress_groove(groove_x, groove_y, groove_w, progress);

    /* Line 2 text */
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)text_col_x,
                (float)(hud_y + REPLAY_HUD_TEXT_LINE2_Y),
                kbd_txt, FONT_SMALL);

    glDisable(GL_BLEND);
    gl2d_end();
}
