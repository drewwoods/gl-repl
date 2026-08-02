/*
 * tour_hud.c - controlled-tour transport HUD (see tour_hud.h).
 *
 * Feature-owned UI under the `tour_ui_*` prefix. Reads ONLY snap->tour (a
 * GlrTourPlaybackView) plus the scene rect; never live pointer-script state.
 * Shares the replay HUD's line structure (metadata / progress / hints) but is
 * styled apart from it - warm amber identity color, a solid top ribbon instead
 * of a thin outline, and a segmented per-event step bar instead of the
 * continuous groove - and sits at the TOP of the scene viewport so both HUDs
 * can show at once without reading as the same control.
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

/* Displayed step number: current_event+1 while an event is
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

/* Copy `src` into `dst`, truncating to `max_chars` glyphs with a trailing ".."
 * elision marker so an over-long line never draws past the panel edge. */
static void tour_hud_truncate(char *dst, size_t dstsz, const char *src,
                              int max_chars) {
    int copy = max_chars;
    if (copy < 0) copy = 0;
    if ((size_t)copy >= dstsz) copy = (int)dstsz - 1;
    memcpy(dst, src, (size_t)copy);
    dst[copy] = '\0';
    if (copy >= 2 && (int)strlen(src) > copy) {
        dst[copy - 1] = '.';
        dst[copy - 2] = '.';
    }
}

/* Pick the first candidate (widest first) that fits within `max_chars`; if none
 * fit, truncate the most compact one. Candidates are progressively elided so a
 * narrow HUD drops optional metadata (filename/source line, then the tour name)
 * before the essential state/speed/step readout. */
static void tour_hud_fit(char *dst, size_t dstsz, int max_chars,
                         char cand[][256], int n) {
    for (int i = 0; i < n; i++) {
        if ((int)strlen(cand[i]) <= max_chars) {
            /* Each candidate is one 256-byte row, so it always fits `dst`,
             * but GCC only sees the flattened cand[][] object and assumes the
             * string may run to its end (-Wformat-truncation). The precision
             * restates the row width and silences it. */
            snprintf(dst, dstsz, "%.*s", (int)sizeof(cand[0]) - 1, cand[i]);
            return;
        }
    }
    tour_hud_truncate(dst, dstsz, cand[n > 0 ? n - 1 : 0], max_chars);
}

/* Build the metadata line, eliding to fit `max_chars`. Candidates, widest
 * first: full (with file:line) -> drop file:line -> drop the "Tour <name>"
 * label -> compact "State | speed | step / total". */
static void tour_hud_build_line1(char *dst, size_t dstsz, int max_chars,
                                 const GlrTourPlaybackView *v) {
    char speed_txt[16];
    const char *name  = v->name ? v->name : "";
    const char *file  = v->file ? v->file : "";
    const char *state = tour_hud_state_text(v->state);
    int step  = tour_hud_step_number(v);
    int total = v->total_events;
    char cand[4][256];
    int n = 0;

    tour_hud_speed_text(speed_txt, sizeof(speed_txt), v->speed);
    if (v->source_line > 0 && file[0])
        snprintf(cand[n++], 256, "Tour  %s | %s | %s | Step %d / %d | %s:%d",
                 name, state, speed_txt, step, total, file, v->source_line);
    snprintf(cand[n++], 256, "Tour  %s | %s | %s | Step %d / %d",
             name, state, speed_txt, step, total);
    snprintf(cand[n++], 256, "%s | %s | %s | Step %d / %d",
             name, state, speed_txt, step, total);
    snprintf(cand[n++], 256, "%s | %s | %d / %d",
             state, speed_txt, step, total);
    tour_hud_fit(dst, dstsz, max_chars, cand, n);
}

/* Build the keyboard-hint line, eliding to fit `max_chars`. */
static void tour_hud_build_line2(char *dst, size_t dstsz, int max_chars) {
    char cand[3][256];
    int n = 0;
    /* The back/step markers are Latin-1 0xAB / 0xBB, matching the replay HUD. */
    snprintf(cand[n++], 256,
             "Space play | %c back | %c step | +/- speed | Esc exit",
             (char)0xAB, (char)0xBB);
    snprintf(cand[n++], 256, "Space | %c back | %c step | +/- | Esc",
             (char)0xAB, (char)0xBB);
    snprintf(cand[n++], 256, "Space  %c %c  +/-  Esc", (char)0xAB, (char)0xBB);
    tour_hud_fit(dst, dstsz, max_chars, cand, n);
}

/* Available panel width for a scene `scene_w` px wide: the scene minus
 * horizontal margins, and NEVER more (no forced minimum width that could push
 * the panel past a narrow scene into a side code panel or off-window - feedback
 * can't see the off-window part, so this is guarded arithmetically instead).
 * Returns 0 when the scene is too narrow to show a readable HUD (render is
 * skipped). tour_ui_hud_rect's expanded width and cap; the single source of
 * truth for the width guard test. */
int tour_hud_panel_width(int scene_w) {
    int hud_w = scene_w - 2 * TOUR_HUD_MARGIN_X;
    if (hud_w < TOUR_HUD_MIN_VISIBLE)
        return 0;
    return hud_w;
}

/* Unlike the replay HUD's thin full outline, the tour panel wears a solid
 * amber ribbon along its top edge (it hangs from the top of the scene) with
 * only a dim bottom rule closing the silhouette. */
static void draw_hud_panel(int x, int y, int w, int h) {
    ui_clr_a(UI_TOK_SURFACE, 0.94f);
    glRectf((float)x, (float)y, (float)(x + w), (float)(y + h));
    ui_clr_a(UI_TOK_ACCENT_ALT, 0.95f);
    glRectf((float)x, (float)(y + h - TOUR_HUD_RIBBON_H),
            (float)(x + w), (float)(y + h));
    ui_clr_a(UI_TOK_ACCENT_ALT_DIM, 0.9f);
    glBegin(GL_LINES);
    glVertex2f((float)x,       (float)y + 0.5f);
    glVertex2f((float)(x + w), (float)y + 0.5f);
    glEnd();
}

/* Segmented per-event step bar: a filled amber block per completed event with
 * panel-colored separator ticks - tours advance in discrete steps, unlike the
 * replay HUD's continuous command groove. Falls back to a plain fill when
 * segments would be too narrow to read. */
static void draw_step_bar(int x, int y, int w, float progress, int total) {
    ui_clr(UI_TOK_SUNKEN);
    glRectf((float)x, (float)y, (float)(x + w), (float)(y + TOUR_HUD_PROGRESS_H));
    ui_clr(UI_TOK_ACCENT_ALT);
    glRectf((float)x, (float)y, (float)x + (float)w * progress,
            (float)(y + TOUR_HUD_PROGRESS_H));
    if (total > 1 && w / total >= 4) {
        ui_clr_a(UI_TOK_SURFACE, 0.94f);
        glBegin(GL_LINES);
        for (int i = 1; i < total; i++) {
            float tx = (float)x + (float)w * (float)i / (float)total;
            glVertex2f(tx, (float)y);
            glVertex2f(tx, (float)(y + TOUR_HUD_PROGRESS_H));
        }
        glEnd();
    }
    ui_clr(UI_TOK_ACCENT_ALT_DIM);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)x + 0.5f,       (float)y + 0.5f);
    glVertex2f((float)(x + w) - 0.5f, (float)y + 0.5f);
    glVertex2f((float)(x + w) - 0.5f, (float)(y + TOUR_HUD_PROGRESS_H) - 0.5f);
    glVertex2f((float)x + 0.5f,       (float)(y + TOUR_HUD_PROGRESS_H) - 0.5f);
    glEnd();
}

static void tour_hud_build_compact(char *dst, size_t dstsz, int max_chars,
                                   const GlrTourPlaybackView *v) {
    const char *name = (v->name && v->name[0]) ? v->name : "Untitled";
    char cand[3][256];

    /* "Esc exit" is in the COMPACT strip, not just the expanded transport
     * line: the compact strip is what a tour actually runs under, and a mode
     * has to keep saying how to leave it. First thing dropped when the scene
     * is too narrow to carry it, since the presence border still marks the
     * mode even when this strip has room for nothing but a label. */
    snprintf(cand[0], sizeof(cand[0]), TOUR_HUD_COMPACT_FMT, name);
    snprintf(cand[1], sizeof(cand[1]), "Tour | %s | [+]", name);
    snprintf(cand[2], sizeof(cand[2]), "Tour | [+]");
    tour_hud_fit(dst, dstsz, max_chars, cand, 3);
}

int tour_ui_hud_rect(const struct UiRenderSnapshot *snap,
                     int *x, int *y, int *w, int *h) {
    int scene_x, scene_y, scene_w, scene_h;
    int avail_w;
    int hud_w;
    int hud_h;

    if (!snap || !snap->tour.active)
        return 0;

    ui_layout_scene_rect(&scene_x, &scene_y, &scene_w, &scene_h);
    avail_w = tour_hud_panel_width(scene_w);   /* scene minus margins, or 0 */
    if (avail_w <= 0)
        return 0;

    hud_h = snap->tour.hud_expanded
          ? TOUR_HUD_EXPANDED_HEIGHT : TOUR_HUD_COMPACT_HEIGHT;
    if (snap->tour.hud_expanded) {
        hud_w = avail_w;
    } else {
        const char *name = (snap->tour.name && snap->tour.name[0])
                         ? snap->tour.name : "Untitled";
        int desired_chars = TOUR_HUD_COMPACT_CHROME_CHARS + (int)strlen(name);
        hud_w = desired_chars * FONT_SMALL_W + 2 * TOUR_HUD_TEXT_PAD_X;
        if (hud_w > avail_w)
            hud_w = avail_w;
        if (hud_w < TOUR_HUD_MIN_VISIBLE)
            hud_w = TOUR_HUD_MIN_VISIBLE;
    }

    if (x) *x = scene_x + TOUR_HUD_MARGIN_X;
    if (w) *w = hud_w;
    if (h) *h = hud_h;
    if (y) {
        int hud_y = scene_y + scene_h - TOUR_HUD_MARGIN_Y - hud_h;
        *y = ui_clamp_panel_y(scene_y, scene_h, hud_h, hud_y,
                             /*prefer_top_on_overflow=*/1, STATUSBAR_H, 4);
    }
    return 1;
}

UiHit tour_ui_hud_hit_test(const struct UiRenderSnapshot *snap,
                           int mx, int my) {
    UiHit hit = ui_hit_none();
    int x, y, w, h;
    int gl_y;

    if (!tour_ui_hud_rect(snap, &x, &y, &w, &h))
        return hit;
    gl_y = snap->viewport.window_h - my;
    if (mx < x || mx >= x + w || gl_y < y || gl_y >= y + h)
        return hit;

    hit.kind = UI_HIT_TOUR_HUD;
    hit.local_x = (float)(mx - x);
    hit.local_y = (float)(gl_y - y);
    return hit;
}

/*
 * Layout:
 * +---------------------------------------------------------------------------+
 * | Tour  <name> | <State> | <speed>x | Step n / total | <file>:<line>        |
 * | [==|==|==|== segmented step bar (one cell per event) ==|==|==|==|==|==|=] |
 * | Space play | back | step | +/- speed | Esc exit                          |
 * +---------------------------------------------------------------------------+
 */
void tour_ui_hud_render(const struct UiRenderSnapshot *snap) {
    int hud_x, hud_y, hud_w, hud_h;
    char line1[256];
    char kbd[256];
    float progress = 0.0f;

    if (!tour_ui_hud_rect(snap, &hud_x, &hud_y, &hud_w, &hud_h))
        return;
    const GlrTourPlaybackView *v = &snap->tour;

    if (v->total_events > 0) {
        progress = (float)v->completed_events / (float)v->total_events;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
    }

    /* Character budget for the text lines: the panel width minus its padding,
     * in fixed-width FONT_SMALL glyphs. */
    int max_chars = (hud_w - 2 * TOUR_HUD_TEXT_PAD_X) / FONT_SMALL_W;
    if (max_chars < 1)
        max_chars = 1;
    if (v->hud_expanded) {
        /* Reserve room for the right-aligned collapse affordance. */
        tour_hud_build_line1(line1, sizeof(line1), max_chars - 5, v);
        tour_hud_build_line2(kbd, sizeof(kbd), max_chars);
    } else {
        tour_hud_build_compact(line1, sizeof(line1), max_chars, v);
    }

    glViewport(0, 0, snap->viewport.window_w, snap->viewport.window_h);
    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    draw_hud_panel(hud_x, hud_y, hud_w, hud_h);

    ui_clr(UI_TOK_ACCENT_ALT);
    gl2d_draw_string((float)(hud_x + TOUR_HUD_TEXT_PAD_X),
                     (float)(hud_y + (v->hud_expanded
                                     ? TOUR_HUD_TEXT_LINE1_Y
                                     : TOUR_HUD_COMPACT_TEXT_Y)),
                     line1, FONT_SMALL);

    if (v->hud_expanded) {
        int groove_x = hud_x + TOUR_HUD_TEXT_PAD_X;
        int groove_w = hud_w - 2 * TOUR_HUD_TEXT_PAD_X;
        int collapse_x = hud_x + hud_w - TOUR_HUD_TEXT_PAD_X -
                         3 * FONT_SMALL_W;

        gl2d_draw_string((float)collapse_x,
                         (float)(hud_y + TOUR_HUD_TEXT_LINE1_Y),
                         "[-]", FONT_SMALL);
        draw_step_bar(groove_x, hud_y + TOUR_HUD_PROGRESS_Y,
                      groove_w, progress, v->total_events);

        ui_clr(UI_TOK_TEXT_MUTED);
        gl2d_draw_string((float)(hud_x + TOUR_HUD_TEXT_PAD_X),
                         (float)(hud_y + TOUR_HUD_TEXT_LINE2_Y),
                         kbd, FONT_SMALL);
    }
    glDisable(GL_BLEND);
    gl2d_end();
}
