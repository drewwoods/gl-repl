/*
 * tour_hud.c - controlled-tour transport HUD (see tour_hud.h).
 *
 * Feature-owned UI under the `tour_ui_*` prefix. Reads ONLY snap->tour (a
 * GlrTourPlaybackView) plus the scene rect; never live pointer-script state.
 * Mirrors the replay HUD's panel/progress/hint layout but sits at the TOP of
 * the scene viewport so both HUDs can show at once.
 */
#include "ui/subsystems/tour_hud.h"
#include "ui/app/snapshot.h"
#include "ui/core/gl_2d.h"
#include "ui/core/layout_utils.h"   /* ui_clamp_panel_y */
#include "ui/core/theme.h"
#include "ui/app/layout.h"          /* ui_layout_scene_rect, STATUSBAR_H */

#include <stdio.h>
#include <string.h>

static const char *tour_hud_state_text(GlrTourPlaybackState state) {
    switch (state) {
    case GLR_TOUR_BASELINE_PENDING: return "Loading";
    case GLR_TOUR_PLAYING:          return "Playing";
    case GLR_TOUR_PAUSED:           return "Paused";
    case GLR_TOUR_STEPPING:         return "Stepping";
    case GLR_TOUR_SEEKING:          return "Seeking";
    case GLR_TOUR_DONE:             return "Done";
    default:                        return "";
    }
}

/* Displayed step number, per the plan: current_event+1 while an event is
 * active, completed_events+1 at a paused between-event boundary, total in Done.
 * Clamped to [1, total]. */
static int tour_hud_step_number(const GlrTourPlaybackView *v) {
    int step;
    if (v->state == GLR_TOUR_DONE)
        step = v->total_events;
    else if (v->current_event >= 0)
        step = v->current_event + 1;
    else
        step = v->completed_events + 1;
    if (step < 1)
        step = 1;
    if (v->total_events > 0 && step > v->total_events)
        step = v->total_events;
    return step;
}

/* "<value>x" where x is the Latin-1 multiplication sign 0xD7 (present in the
 * 8x13 fixed font, like the replay HUD's guillemets). %g renders the discrete
 * ladder cleanly: 0.25 / 0.5 / 1 / 2 / 4 / 8 / 16. */
static void tour_hud_speed_text(char *buf, size_t sz, float speed) {
    int n = snprintf(buf, sz, "%g", (double)speed);
    if (n >= 0 && (size_t)n + 1 < sz) {
        buf[n] = (char)0xD7;
        buf[n + 1] = '\0';
    }
}

static void draw_hud_panel(int x, int y, int w) {
    ui_clr_a(UI_TOK_SURFACE, 0.94f);
    glRectf((float)x, (float)y, (float)(x + w), (float)(y + TOUR_HUD_HEIGHT));
    ui_clr_a(UI_TOK_ACCENT_GLOW_BG, 0.95f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)x + 0.5f,       (float)y + 0.5f);
    glVertex2f((float)(x + w) - 0.5f, (float)y + 0.5f);
    glVertex2f((float)(x + w) - 0.5f, (float)(y + TOUR_HUD_HEIGHT) - 0.5f);
    glVertex2f((float)x + 0.5f,       (float)(y + TOUR_HUD_HEIGHT) - 0.5f);
    glEnd();
}

static void draw_progress_groove(int x, int y, int w, float progress) {
    ui_clr(UI_TOK_SUNKEN);
    glRectf((float)x, (float)y, (float)(x + w), (float)(y + TOUR_HUD_PROGRESS_H));
    ui_clr(UI_TOK_ACCENT);
    glRectf((float)x, (float)y, (float)x + (float)w * progress,
            (float)(y + TOUR_HUD_PROGRESS_H));
    ui_clr(UI_TOK_ACCENT_GLOW_BG);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)x + 0.5f,       (float)y + 0.5f);
    glVertex2f((float)(x + w) - 0.5f, (float)y + 0.5f);
    glVertex2f((float)(x + w) - 0.5f, (float)(y + TOUR_HUD_PROGRESS_H) - 0.5f);
    glVertex2f((float)x + 0.5f,       (float)(y + TOUR_HUD_PROGRESS_H) - 0.5f);
    glEnd();
}

/*
 * Layout:
 * +---------------------------------------------------------------------------+
 * | Tour  <name> | <State> | <speed>x | Step n / total | <file>:<line>        |
 * | [=== progress groove ===================================================] |
 * | Space play | « back | » step | +/- speed | Esc exit                       |
 * +---------------------------------------------------------------------------+
 */
void tour_ui_hud_render(const struct UiRenderSnapshot *snap) {
    int render3d_x, render3d_y, render3d_w, render3d_h;
    char line1[256];
    char speed_txt[16];
    float progress = 0.0f;

    if (!snap || !snap->tour.active)
        return;
    const GlrTourPlaybackView *v = &snap->tour;

    ui_layout_scene_rect(&render3d_x, &render3d_y, &render3d_w, &render3d_h);

    int hud_x = render3d_x + TOUR_HUD_MARGIN_X;
    int hud_w = render3d_w - 2 * TOUR_HUD_MARGIN_X;
    if (hud_w < TOUR_HUD_MIN_WIDTH)
        hud_w = TOUR_HUD_MIN_WIDTH;
    /* Sit TOUR_HUD_MARGIN_Y below the scene's top edge; ui_clamp_panel_y keeps
     * it inside the scene and above the bottom status/replay-HUD band. */
    int hud_y = render3d_y + render3d_h - TOUR_HUD_MARGIN_Y - TOUR_HUD_HEIGHT;
    hud_y = ui_clamp_panel_y(render3d_y, render3d_h, TOUR_HUD_HEIGHT, hud_y,
                             /*prefer_top_on_overflow=*/1, STATUSBAR_H, 4);

    if (v->total_events > 0) {
        progress = (float)v->completed_events / (float)v->total_events;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
    }

    tour_hud_speed_text(speed_txt, sizeof(speed_txt), v->speed);
    if (v->source_line > 0)
        snprintf(line1, sizeof(line1), "Tour  %s | %s | %s | Step %d / %d | %s:%d",
                 v->name ? v->name : "", tour_hud_state_text(v->state), speed_txt,
                 tour_hud_step_number(v), v->total_events,
                 v->file ? v->file : "", v->source_line);
    else
        snprintf(line1, sizeof(line1), "Tour  %s | %s | %s | Step %d / %d",
                 v->name ? v->name : "", tour_hud_state_text(v->state), speed_txt,
                 tour_hud_step_number(v), v->total_events);

    glViewport(0, 0, snap->viewport.window_w, snap->viewport.window_h);
    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    draw_hud_panel(hud_x, hud_y, hud_w);

    ui_clr(UI_TOK_ACCENT);
    gl2d_draw_string((float)(hud_x + TOUR_HUD_TEXT_PAD_X),
                     (float)(hud_y + TOUR_HUD_TEXT_LINE1_Y), line1, FONT_SMALL);

    int groove_x = hud_x + TOUR_HUD_TEXT_PAD_X;
    int groove_w = hud_w - 2 * TOUR_HUD_TEXT_PAD_X;
    draw_progress_groove(groove_x, hud_y + TOUR_HUD_PROGRESS_Y, groove_w, progress);

    /* « and » are Latin-1 0xAB / 0xBB, matching the replay HUD's step glyphs. */
    char kbd[96];
    snprintf(kbd, sizeof(kbd), "Space play | %c back | %c step | +/- speed | Esc exit",
             (char)0xAB, (char)0xBB);
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)(hud_x + TOUR_HUD_TEXT_PAD_X),
                     (float)(hud_y + TOUR_HUD_TEXT_LINE2_Y), kbd, FONT_SMALL);

    glDisable(GL_BLEND);
    gl2d_end();
}
