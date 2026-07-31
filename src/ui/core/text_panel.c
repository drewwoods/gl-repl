/*
 * text_panel.c -- Generic text panel renderer and hit-tester.
 *
 * This file has no dependency on repl, editor, or app headers.
 */
#include "config.h"
#include "support/cpuprof.h"   /* phase probes; sections are opaque ints, the
                                * catalog comes from the force-included
                                * prof_sections.h — no app dependency */
#include "ui/core/gl_2d.h"
#include "ui/core/text_layout.h"
#include "ui/core/text_search.h"
#include "ui/core/text_panel.h"
#include "ui/core/metrics.h"
#include "ui/core/theme.h"

#include <stdio.h>
#include <string.h>

static const GLfloat k_clr_active_row_bg[4]  = { 0.15f, 0.18f, 0.28f, 0.70f };
static const GLfloat k_clr_selection_band[4] = { 0.35f, 0.55f, 0.95f, 0.75f };
static const GLfloat k_clr_search_match[4]   = { 0.25f, 0.45f, 0.85f, 0.30f };
static const GLfloat k_clr_search_hit[4]     = { 0.95f, 0.65f, 0.18f, 0.55f };
static const GLfloat k_clr_ghost_text[4]     = { 0.50f, 0.55f, 0.65f, 0.55f };
static const GLfloat k_clr_hint_text[4]      = { 0.56f, 0.62f, 0.72f, 0.38f };
static const GLfloat k_clr_cursor_caret[4]   = { 0.90f, 0.80f, 0.25f, 0.85f };
static const GLfloat k_clr_paren_match[4]    = { 0.30f, 0.80f, 0.50f, 0.45f };
static const GLfloat k_clr_paren_scope[4]    = { 0.45f, 0.48f, 0.60f, 0.28f };

/* text_panel chrome that has no clean theme-token twin: a very dim
 * gutter/indent-guide tier and a specific dark action-chip ring. Kept
 * as named consts (theme.h "named constant" bucket) - theme-stable,
 * not shoehorned into a token. The k_clr_* block above is the editor
 * sub-palette (intentionally NOT themed - see theme.h bucket 3). */
static const GLfloat k_panel_dim[3]           = { 0.30f, 0.30f, 0.38f };
static const GLfloat k_action_chip_outline[4] = { 0.10f, 0.10f, 0.12f, 0.85f };

static int text_panel_statusbar_h(const UiTextPanelSnapshot *snap) {
    return (snap && (snap->chrome_flags & UI_TEXT_PANEL_CHROME_STATUSBAR))
         ? snap->statusbar_h : 0;
}

typedef struct {
    int statusbar_h;
    int visible_rows;
    int first_line_y;
} TextPanelViewportMetrics;

static TextPanelViewportMetrics text_panel_viewport_metrics(
    const UiTextPanelSnapshot *snap) {
    TextPanelViewportMetrics metrics;

    metrics.statusbar_h = text_panel_statusbar_h(snap);
    metrics.visible_rows = ui_text_panel_visible_lines_for_height(
        snap->cp_h, metrics.statusbar_h, snap->top_chrome_h);

    /* Render, hit-test, and input-row lookup all reason from the
     * same first text baseline directly under the top chrome. */
    metrics.first_line_y = snap->cp_y + snap->cp_h - CODE_MARGIN_Y
                         - 2 * LINE_H - snap->top_chrome_h;
    return metrics;
}

static CodeLayout text_panel_row_layout(const UiTextPanelSnapshot *snap,
                                        const UiTextPanelRow *row) {
    int first_x = snap->text_x + row->indent_chars * FONT_W;
    return code_layout_make(snap->cp_w, first_x, FONT_W, snap->wrap_at_comma);
}

static const char *text_panel_row_text(const UiTextPanelSnapshot *snap,
                                       const UiTextPanelRow *row) {
    if (row->kind == UI_TEXT_PANEL_ROW_INPUT)
        return snap->input.input ? snap->input.input : "";
    return row->text ? row->text : "";
}

static int text_panel_row_text_len(const UiTextPanelSnapshot *snap,
                                   const UiTextPanelRow *row) {
    const char *text = text_panel_row_text(snap, row);

    if (row->kind == UI_TEXT_PANEL_ROW_INPUT) {
        return snap->input.input_len >= 0
             ? snap->input.input_len
             : (int)strlen(text);
    }

    return (int)strlen(text);
}

/* Per-frame wrap-count cache. Populated by ui_text_panel_render as it
 * walks rows; read back by ui_text_panel_hit_test and
 * ui_text_panel_input_row_y so they stop re-walking the same row text
 * the renderer already walked. Keyed on the snapshot pointer plus the
 * geometry fields wrap depends on (row_count, cp_w, wrap_at_comma) —
 * if any of those change the cache is invalidated and consumers fall
 * back to a fresh compute.
 *
 * Cap matches the practical UI_REPL_CODE_PANEL_MAX_ROWS upper bound
 * (~4-5K rows for a full document + virtual lines + chrome) without
 * pulling MAX_EDITOR_COMMANDS into the lower layer. Snapshots that exceed
 * the cap bypass the cache entirely. */
#define UI_TEXT_PANEL_WRAP_CACHE_MAX 8192
static struct {
    const UiTextPanelSnapshot *snap;
    int  row_count;
    int  cp_w;
    int  wrap_at_comma;
    int  text_x;
    int  valid;
    int  wrap[UI_TEXT_PANEL_WRAP_CACHE_MAX];
} g_wrap_cache;

static int wrap_cache_matches(const UiTextPanelSnapshot *snap) {
    return g_wrap_cache.valid &&
           g_wrap_cache.snap == snap &&
           g_wrap_cache.row_count == snap->row_count &&
           g_wrap_cache.cp_w == snap->cp_w &&
           g_wrap_cache.wrap_at_comma == snap->wrap_at_comma &&
           g_wrap_cache.text_x == snap->text_x &&
           snap->row_count <= UI_TEXT_PANEL_WRAP_CACHE_MAX;
}

static void wrap_cache_invalidate(void) {
    g_wrap_cache.valid = 0;
    g_wrap_cache.snap = NULL;
}

static int text_panel_row_wrap_count_cached(const UiTextPanelSnapshot *snap,
                                            int row_idx);

static int text_panel_color_uses_blend(const UiTextPanelColor *color) {
    return color && color->has_alpha && color->a < 1.0f;
}

static void text_panel_set_color(const UiTextPanelColor *color) {
    if (!color)
        return;
    if (color->has_alpha)
        glColor4f(color->r, color->g, color->b, color->a);
    else
        glColor3f(color->r, color->g, color->b);
}

/* Full-width horizontal-bar glyph in GLUT_BITMAP_9_BY_15 (a single row of
 * all-set pixels). Used as the ligature form of a comment-line dash run so
 * consecutive dashes join into one unbroken rule. */
#define TEXT_PANEL_HRULE_GLYPH 0x12

static int text_panel_is_comment_line(const char *text) {
    if (!text)
        return 0;

    while (*text == ' ' || *text == '\t')
        text++;
    return text[0] == '/' && text[1] == '/';
}

static unsigned char text_panel_display_glyph(const char *text, int index,
                                               int join_dash_runs) {
    unsigned char ch = (unsigned char)text[index];

    /* Run membership is checked against the full NUL-terminated text (not
     * the drawn slice) so wrapping or a color-segment boundary never splits
     * a rule.  Only runs of 3+ dashes become the horizontal-rule glyph;
     * a bare "--" stays as literal dashes (e.g. "--detail" in a flag). */
    if (join_dash_runs && ch == '-') {
        int run = 1, j;
        for (j = index - 1; j >= 0 && text[j] == '-'; j--)
            run++;
        for (j = index + 1; text[j] == '-'; j++)
            run++;
        if (run >= 3)
            return TEXT_PANEL_HRULE_GLYPH;
    }
    return ch;
}

static void text_panel_draw_segment(int x, int y, const char *text,
                                    int start, int len, void *font,
                                    int comment_rule) {
    int join_dash_runs;

    if (!text || len <= 0)
        return;

    join_dash_runs = comment_rule && text_panel_is_comment_line(text);
    glRasterPos2f((float)x, (float)y);

#ifdef USE_GLUT
    /* Apple GLUT does not expose the freeglut glutBitmapString extension. */
    for (int i = 0; i < len; i++) {
        int a = start + i;
        glutBitmapCharacter(font,
                            text_panel_display_glyph(text, a, join_dash_runs));
    }
#else
    /* freeglut preserves pixel-store state once per string rather than once
     * per glyph. A span reaching the source terminator can be submitted
     * directly unless it needs the display-only comment-rule substitution. */
    if (!join_dash_runs && text[start + len] == '\0') {
        glutBitmapString(font, (const unsigned char *)(text + start));
    } else {
        unsigned char display[len + 1];

        for (int i = 0; i < len; i++)
            display[i] = text_panel_display_glyph(text, start + i,
                                                  join_dash_runs);
        display[len] = '\0';
        glutBitmapString(font, display);
    }
#endif
}

static int text_panel_row_uses_blend(const UiTextPanelRow *row) {
    int seg_count;

    if (!row)
        return 0;
    if (text_panel_color_uses_blend(&row->color))
        return 1;

    seg_count = row->color_segment_count;
    if (seg_count > UI_TEXT_PANEL_MAX_COLOR_SEGMENTS)
        seg_count = UI_TEXT_PANEL_MAX_COLOR_SEGMENTS;

    for (int i = 0; i < seg_count; i++) {
        if (text_panel_color_uses_blend(&row->color_segments[i].color))
            return 1;
    }

    return 0;
}

static void text_panel_draw_colored_span(const UiTextPanelSnapshot *snap,
                                         const char *text,
                                         int wrap_start,
                                         int wrap_x,
                                         int span_start,
                                         int span_end,
                                         int line_y,
                                         const UiTextPanelColor *color,
                                         int shadow,
                                         int blend_on) {
    int span_len;
    int x;

    if (!snap || !text || !color || span_end <= span_start)
        return;
    if (color->has_alpha && color->a <= 0.0f)
        return;

    span_len = span_end - span_start;
    x = snap->cp_x + wrap_x + (span_start - wrap_start) * FONT_W;

    if (shadow) {
        /* Drop shadow: a dark copy offset by (+1, -1) drawn behind the
         * glyphs, then the colored text on top. Replaces the old additive
         * "fake bold" pass, which barely registered. The shadow alpha
         * tracks the span's own alpha so faded spans (tutorial reveal)
         * keep their shadow in step. */
        float a = color->has_alpha ? color->a : 1.0f;

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(1.0f, 1.0f, 1.0f, UI_TEXT_PANEL_SHADOW_ALPHA * a);
        text_panel_draw_segment(x + 1, line_y - 1, text, span_start, span_len,
                                FONT_MONO, snap->comment_rule_ligature);
        if (!blend_on)
            glDisable(GL_BLEND);

        text_panel_set_color(color);
        text_panel_draw_segment(x, line_y, text, span_start, span_len,
                                FONT_MONO, snap->comment_rule_ligature);
        return;
    }

    text_panel_set_color(color);
    text_panel_draw_segment(x, line_y, text, span_start, span_len, FONT_MONO,
                            snap->comment_rule_ligature);
}

static void text_panel_draw_colored_text(const UiTextPanelSnapshot *snap,
                                         const UiTextPanelRow *row,
                                         const char *text,
                                         int wrap_start,
                                         int wrap_len,
                                         int wrap_x,
                                         int line_y) {
    int wrap_end;
    int seg_count;
    int cursor;
    int use_segments;

    if (!snap || !row || !text || wrap_len <= 0)
        return;

    wrap_end = wrap_start + wrap_len;
    seg_count = row->color_segment_count;
    if (seg_count > UI_TEXT_PANEL_MAX_COLOR_SEGMENTS)
        seg_count = UI_TEXT_PANEL_MAX_COLOR_SEGMENTS;
    use_segments = seg_count > 0;

    int blend_on = text_panel_row_uses_blend(row);
    if (blend_on) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    if (!use_segments) {
        text_panel_set_color(&row->color);
        text_panel_draw_segment(snap->cp_x + wrap_x, line_y, text,
                                wrap_start, wrap_len, FONT_MONO,
                                snap->comment_rule_ligature);
        if (blend_on)
            glDisable(GL_BLEND);
        return;
    }

    cursor = wrap_start;
    for (int i = 0; i < seg_count; i++) {
        const UiTextPanelColorSegment *segment = &row->color_segments[i];
        int seg_start = segment->char_start;
        int seg_end = segment->char_start + segment->char_count;

        if (seg_end <= wrap_start || seg_start >= wrap_end)
            continue;

        if (seg_start < wrap_start)
            seg_start = wrap_start;
        if (seg_end > wrap_end)
            seg_end = wrap_end;
        if (cursor < seg_start) {
            text_panel_draw_colored_span(snap, text, wrap_start, wrap_x,
                                         cursor, seg_start, line_y,
                                         &row->color, 0, blend_on);
        }
        text_panel_draw_colored_span(snap, text, wrap_start, wrap_x,
                                     seg_start, seg_end, line_y,
                                     &segment->color, segment->shadow,
                                     blend_on);
        if (seg_end > cursor)
            cursor = seg_end;
    }

    if (cursor < wrap_end) {
        text_panel_draw_colored_span(snap, text, wrap_start, wrap_x,
                                     cursor, wrap_end, line_y,
                                     &row->color, 0, blend_on);
    }

    if (blend_on)
        glDisable(GL_BLEND);
}

static void text_panel_draw_line_number(const UiTextPanelSnapshot *snap,
                                        int line_y, int file_line) {
    char ln[16];

    if (!(snap->chrome_flags & UI_TEXT_PANEL_CHROME_LINE_NUMS) ||
        file_line <= 0)
        return;

    snprintf(ln, sizeof(ln), "%3d", file_line);
    glColor3fv(k_panel_dim);
    gl2d_draw_string((float)(snap->cp_x + CODE_MARGIN_X),
                     (float)line_y, ln, FONT_MONO);
}

static void text_panel_draw_left_aux(const UiTextPanelSnapshot *snap,
                                     const UiTextPanelRow *row,
                                     int line_y) {
    int aux_x;
    int blend_on;

    if (!(snap->chrome_flags & UI_TEXT_PANEL_CHROME_AUX_COL) ||
        !row->left_aux_label[0])
        return;

    aux_x = snap->cp_x + snap->text_x - 6 * FONT_W;

    /* Blend is off by default in the panel pass — the row-text path turns it
     * on per span and back off again — so a translucent label has to bracket
     * its own draw or the alpha is simply discarded. */
    blend_on = row->left_aux_alpha > 0.0f && row->left_aux_alpha < 1.0f;
    if (blend_on) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    ui_clr_a(UI_TOK_TEXT_SECTION,
             row->left_aux_alpha > 0.0f ? row->left_aux_alpha : 1.0f);
    gl2d_draw_string((float)aux_x, (float)line_y,
                     row->left_aux_label, FONT_MONO);
    if (blend_on)
        glDisable(GL_BLEND);
}

static void text_panel_draw_right_action(const UiTextPanelSnapshot *snap,
                                         const UiTextPanelRow *row,
                                         int line_y) {
    int sx;
    int sy;
    int sw;

    if (!row->right_action.active)
        return;

    if (!ui_text_panel_right_action_rect(snap, line_y, &sx, &sy, &sw))
        return;

    /* Opaque checkerboard base under the fill (same 4px grays as the floating
     * picker's preview strip). The panel is dark, so without a base a near-
     * black or low-alpha fill composites into the background and the chip
     * disappears; against the checker it always reads, and a translucent
     * color still reads as translucent. */
    {
        int ck = 4;
        int ix, iy;
        for (iy = 0; iy < sw; iy += ck) {
            for (ix = 0; ix < sw; ix += ck) {
                float gv = ((ix / ck + iy / ck) % 2) ? 0.35f : 0.55f;
                int tw = (ix + ck < sw) ? ck : sw - ix;
                int th = (iy + ck < sw) ? ck : sw - iy;
                glColor3f(gv, gv, gv);
                glRectf((float)(sx + ix), (float)(sy + iy),
                        (float)(sx + ix + tw), (float)(sy + iy + th));
            }
        }
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    text_panel_set_color(&row->right_action.color);
    glRectf((float)sx, (float)sy,
            (float)(sx + sw), (float)(sy + sw));

    /* Outline contrast follows the composited fill, so a dark chip gets a
     * bright edge rather than the mid-gray default that vanishes next to it. */
    {
        const UiTextPanelColor *c = &row->right_action.color;
        float a = c->has_alpha ? c->a : 1.0f;
        float lum = (0.299f * c->r + 0.587f * c->g + 0.114f * c->b) * a
                    + 0.45f * (1.0f - a);   /* checkerboard mean */
        if (lum > 0.55f) glColor4fv(k_action_chip_outline);
        else             ui_clr_a(UI_TOK_TEXT_ON_HILITE, 0.85f);
    }
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)sx,        (float)sy);
    glVertex2f((float)(sx + sw), (float)sy);
    glVertex2f((float)(sx + sw), (float)(sy + sw));
    glVertex2f((float)sx,        (float)(sy + sw));
    glEnd();
    if (row->right_action.emphasized) {
        ui_clr_a(UI_TOK_TEXT_ON_HILITE, 0.9f);
        glBegin(GL_LINE_LOOP);
        glVertex2f((float)(sx - 1),        (float)(sy - 1));
        glVertex2f((float)(sx + sw + 1),   (float)(sy - 1));
        glVertex2f((float)(sx + sw + 1),   (float)(sy + sw + 1));
        glVertex2f((float)(sx - 1),        (float)(sy + sw + 1));
        glEnd();
    }
    glDisable(GL_BLEND);
}

static void text_panel_draw_row_background(const UiTextPanelSnapshot *snap,
                                           const UiTextPanelRow *row,
                                           int line_y) {
    if (!snap || !row)
        return;

    if (row->background_active) {
        if (text_panel_color_uses_blend(&row->background_color)) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        text_panel_set_color(&row->background_color);
        glRectf((float)snap->cp_x, (float)(line_y - 3),
                (float)(snap->cp_x + snap->cp_w),
                (float)(line_y - 3 + LINE_H));
        if (text_panel_color_uses_blend(&row->background_color))
            glDisable(GL_BLEND);
    }

    if (row->left_marker_band_count > 0) {
        /* Segmented marker: split the strip into equal stacked bands, one per
         * covering colour in canonical order (first UI_TEXT_PANEL_MAX_MARKER_BANDS). */
        int nb = row->left_marker_band_count;
        float bx0 = (float)(snap->cp_x + 1);
        float bx1 = (float)(snap->cp_x + 4);
        float y0 = (float)(line_y - 3);
        float y1 = (float)(line_y - 3 + LINE_H);
        if (nb > UI_TEXT_PANEL_MAX_MARKER_BANDS)
            nb = UI_TEXT_PANEL_MAX_MARKER_BANDS;
        for (int b = 0; b < nb; b++) {
            const UiTextPanelColor *bc = &row->left_marker_band_colors[b];
            float by0 = y0 + (y1 - y0) * (float)b / (float)nb;
            float by1 = y0 + (y1 - y0) * (float)(b + 1) / (float)nb;
            if (text_panel_color_uses_blend(bc)) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
            text_panel_set_color(bc);
            glRectf(bx0, by0, bx1, by1);
            if (text_panel_color_uses_blend(bc))
                glDisable(GL_BLEND);
        }
    } else if (row->left_marker_active) {
        if (text_panel_color_uses_blend(&row->left_marker_color)) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        text_panel_set_color(&row->left_marker_color);
        glRectf((float)(snap->cp_x + 1), (float)(line_y - 3),
                (float)(snap->cp_x + 4),
                (float)(line_y - 3 + LINE_H));
        if (text_panel_color_uses_blend(&row->left_marker_color))
            glDisable(GL_BLEND);
    }
}

static void text_panel_draw_search_highlights(const UiTextPanelSnapshot *snap,
                                              const UiTextPanelRow *row,
                                              const char *text,
                                              int seg_start, int seg_len,
                                              int seg_x, int line_y) {
    int abs_seg_x = snap->cp_x + seg_x;
    int drew = 0;

    if (!snap->search.active || snap->search.query_len <= 0 ||
        row->search_row_idx < 0 || !text || seg_len <= 0)
        return;

    for (int pos = ui_text_find_next_in_text_opts(text, snap->search.query, 0,
                                                  snap->search.whole_word);
         pos >= 0;
         pos = ui_text_find_next_in_text_opts(text, snap->search.query, pos + 1,
                                              snap->search.whole_word)) {
        int match_end = pos + snap->search.query_len;
        int seg_end = seg_start + seg_len;
        int draw_start = pos > seg_start ? pos : seg_start;
        int draw_end = match_end < seg_end ? match_end : seg_end;

        if (draw_start >= draw_end)
            continue;

        if (!drew) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            drew = 1;
        }

        if (row->search_row_idx == snap->search.hit_row &&
            pos == snap->search.hit_char)
            glColor4fv(k_clr_search_hit);
        else
            glColor4fv(k_clr_search_match);

        glRectf((float)(abs_seg_x + (draw_start - seg_start) * FONT_W),
                (float)(line_y - 2),
            (float)(abs_seg_x + (draw_end - seg_start) * FONT_W),
                (float)(line_y - 2 + FONT_H + 4));
    }

    if (drew)
        glDisable(GL_BLEND);
}


int ui_text_panel_match_paren(const char *s, int len, int cursor,
                              int *self, int *match) {
    char open, close;
    int i, depth;

    if (!s || cursor < 0 || cursor >= len)
        return 0;

    /* Resolve the bracket pair from the char in front of the caret.
     * '(' / ')' and '{' / '}' are matched independently — depth counts
     * only the active pair's own brackets, so the other kind nested
     * between them is ignored. */
    if (s[cursor] == '(' || s[cursor] == ')') {
        open = '('; close = ')';
    } else if (s[cursor] == '{' || s[cursor] == '}') {
        open = '{'; close = '}';
    } else {
        return 0;
    }

    if (s[cursor] == open) {
        depth = 0;
        for (i = cursor; i < len; i++) {
            if (s[i] == open) depth++;
            else if (s[i] == close) depth--;
            if (depth == 0) {
                if (self)  *self = cursor;
                if (match) *match = i;
                return 1;
            }
        }
    } else {  /* s[cursor] == close */
        depth = 0;
        for (i = cursor; i >= 0; i--) {
            if (s[i] == close) depth++;
            else if (s[i] == open) depth--;
            if (depth == 0) {
                if (self)  *self = cursor;
                if (match) *match = i;
                return 1;
            }
        }
    }
    return 0;
}

int ui_text_panel_match_bracket_multiline(const UiTextPanelSnapshot *snap,
                                          int self_row, int self_char,
                                          int *match_row, int *match_char) {
    const char *self_text;
    int self_len;
    char inc_char, dec_char;   /* depth ++ / -- in the scan direction */
    int dir;                   /* +1 forward (open), -1 backward (close) */
    int r, depth, first;

    if (!snap || self_row < 0 || self_row >= snap->row_count)
        return 0;

    self_text = text_panel_row_text(snap, &snap->rows[self_row]);
    self_len  = text_panel_row_text_len(snap, &snap->rows[self_row]);
    if (self_char < 0 || self_char >= self_len)
        return 0;

    /* Only curly braces match across rows; parens are single-row and
     * resolved by ui_text_panel_match_paren. */
    if (self_text[self_char] == '{') {
        dir = +1; inc_char = '{'; dec_char = '}';
    } else if (self_text[self_char] == '}') {
        dir = -1; inc_char = '}'; dec_char = '{';
    } else {
        return 0;
    }

    /* Walk char-by-char through the editable rows (TEXT / INPUT) in
     * document order, skipping chrome / virtual / placeholder rows so
     * only the user's source braces count. */
    depth = 0;
    first = 1;
    for (r = self_row; r >= 0 && r < snap->row_count; r += dir) {
        const UiTextPanelRow *row = &snap->rows[r];
        const char *t;
        int len, c;

        if (r != self_row &&
            row->kind != UI_TEXT_PANEL_ROW_TEXT &&
            row->kind != UI_TEXT_PANEL_ROW_INPUT)
            continue;

        t   = text_panel_row_text(snap, row);
        len = text_panel_row_text_len(snap, row);

        if (first) {
            c = self_char;
            first = 0;
        } else {
            c = (dir > 0) ? 0 : len - 1;
        }

        for (; c >= 0 && c < len; c += dir) {
            if (t[c] == inc_char) depth++;
            else if (t[c] == dec_char) depth--;
            if (depth == 0) {
                if (match_row)  *match_row = r;
                if (match_char) *match_char = c;
                return 1;
            }
        }
    }
    return 0;
}

int ui_text_panel_enclosing_parens(const char *s, int len, int cursor,
                                   int *open, int *close) {
    int i, depth, o = -1;

    if (!s)
        return 0;
    if (cursor < 0) cursor = 0;
    if (cursor > len) cursor = len;

    /* Nearest unbalanced '(' to the left of the caret. */
    depth = 0;
    for (i = cursor - 1; i >= 0; i--) {
        if (s[i] == ')') depth++;
        else if (s[i] == '(') {
            if (depth == 0) { o = i; break; }
            depth--;
        }
    }
    if (o < 0)
        return 0;

    /* Its matching ')'. */
    depth = 0;
    for (i = o; i < len; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') depth--;
        if (depth == 0) {
            if (open)  *open = o;
            if (close) *close = i;
            return 1;
        }
    }
    return 0;
}

/* Resolved bracket-pair highlight for the frame. self_* is the caret's
 * bracket (always on the input row); match_* is its balanced partner,
 * which for '{' / '}' may sit on a different row. self_row == match_row
 * for a same-row pair (every '(' / ')' pair, and inline '{ }'). Both
 * cells are tinted wherever their owning row is drawn, so the partner
 * stays visible across an if / for block. */
typedef struct {
    int active;
    int self_row;
    int self_char;
    int match_row;
    int match_char;
} TextPanelBracketHL;

/* Tint one matched bracket char with a single-cell band, clamped to
 * the wrap row [wrap_start, wrap_start + wrap_len). Drawn behind the text
 * glyphs (called before the input segment) so the character stays legible
 * on top. No-op when the char lies on a different wrap row. */
static void text_panel_draw_paren_cell(const UiTextPanelSnapshot *snap,
                                       int wrap_x, int wrap_start,
                                       int wrap_len, int line_y, int pos) {
    float bx;

    if (pos < wrap_start || pos >= wrap_start + wrap_len)
        return;

    bx = (float)(snap->cp_x + wrap_x + (pos - wrap_start) * FONT_W);
    glEnable(GL_BLEND);
    glColor4fv(k_clr_paren_match);
    glRectf(bx, (float)(line_y - 3),
            bx + (float)FONT_W, (float)(line_y - 3 + LINE_H));
    glDisable(GL_BLEND);
}

/* Tint whichever of the self / match bracket cells fall on row_idx for the
 * current wrap segment. text_panel_draw_paren_cell no-ops the cells that
 * land off this wrap row, so both can be offered unconditionally. */
static void text_panel_draw_bracket_cells(const UiTextPanelSnapshot *snap,
                                          const TextPanelBracketHL *hl,
                                          int row_idx, int wrap_x,
                                          int wrap_start, int wrap_len,
                                          int line_y) {
    if (!hl || !hl->active)
        return;
    if (hl->self_row == row_idx)
        text_panel_draw_paren_cell(snap, wrap_x, wrap_start, wrap_len,
                                   line_y, hl->self_char);
    if (hl->match_row == row_idx)
        text_panel_draw_paren_cell(snap, wrap_x, wrap_start, wrap_len,
                                   line_y, hl->match_char);
}

/* Faint background band behind the in-scope characters [lo, hi] (inclusive)
 * of the caret's enclosing-paren span, clamped to the wrap row. Marks the
 * active scope without recoloring any glyph, so syntax colors — and the
 * near-grey comment shade — stay intact (an earlier version dimmed the
 * out-of-scope text and read as a comment). Drawn before the text segment. */
static void text_panel_draw_paren_scope_band(const UiTextPanelSnapshot *snap,
                                             int wrap_x, int wrap_start,
                                             int wrap_len, int line_y,
                                             int lo, int hi) {
    int wrap_end = wrap_start + wrap_len;
    int a = lo > wrap_start ? lo : wrap_start;
    int b = (hi + 1) < wrap_end ? (hi + 1) : wrap_end;   /* exclusive */
    float bx, bw;

    if (b <= a)
        return;
    bx = (float)(snap->cp_x + wrap_x + (a - wrap_start) * FONT_W);
    bw = (float)((b - a) * FONT_W);
    glEnable(GL_BLEND);
    glColor4fv(k_clr_paren_scope);
    glRectf(bx, (float)(line_y - 3), bx + bw, (float)(line_y - 3 + LINE_H));
    glDisable(GL_BLEND);
}

static int text_panel_draw_input_row(const UiTextPanelSnapshot *snap,
                                     const UiTextPanelRow *row,
                                     int row_idx,
                                     const TextPanelBracketHL *hl,
                                     int visible_rows,
                                     int *io_cur,
                                     int *io_line_y,
                                     UiTextPanelOutput *out) {
    const char *input = snap->input.input ? snap->input.input : "";
    int input_len = snap->input.input_len >= 0
                  ? snap->input.input_len
                  : (int)strlen(input);
    int cursor_pos = snap->input.cursor;
    int anchor_pos = snap->input.anchor;
    int cursor_seg_start = 0;
    int cursor_seg_len = 0;
    int cursor_seg_x = snap->text_x + row->indent_chars * FONT_W;
    int wrap_row = 0;
    int wrap_start;
    int wrap_len;
    int wrap_x;
    CodeWrapIter wrap_it;
    CodeLayout layout = text_panel_row_layout(snap, row);
    int cursor_row;
    int cursor_col;

    if (cursor_pos < 0)
        cursor_pos = 0;
    if (cursor_pos > input_len)
        cursor_pos = input_len;
    if (anchor_pos < 0)
        anchor_pos = cursor_pos;

    cursor_row = code_layout_cursor_row_for_text(input, &layout, cursor_pos,
                                                 &cursor_seg_start,
                                                 &cursor_seg_len,
                                                 &cursor_seg_x);
    cursor_col = cursor_pos - cursor_seg_start;

    /* Bracket-pair highlight (self + partner cells) is resolved once per
     * frame by the caller and threaded in via `hl`; this row tints any of
     * those cells that land on it. Scope highlight stays input-row-local. */

    /* Scope highlight: when the caret sits inside a paren pair, draw a
     * faint background band behind that innermost [open, close] span so
     * the active scope stands out. Off while a range selection is active. */
    int enc_open = -1, enc_close = -1;
    int has_enclosing = snap->paren_scope && (anchor_pos == cursor_pos) &&
        ui_text_panel_enclosing_parens(input, input_len, cursor_pos,
                                       &enc_open, &enc_close);

    code_layout_wrap_iter_init(&wrap_it, input, &layout);
    while (code_layout_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
        if (*io_cur >= snap->scroll && *io_cur < snap->scroll + visible_rows) {
            text_panel_draw_row_background(snap, row, *io_line_y);
            if (wrap_row == 0) {
                text_panel_draw_line_number(snap, *io_line_y,
                                            row->left_gutter_label);
                text_panel_draw_left_aux(snap, row, *io_line_y);
            }

            glEnable(GL_BLEND);
            glColor4fv(k_clr_active_row_bg);
            glRectf((float)snap->cp_x, (float)(*io_line_y - 3),
                    (float)(snap->cp_x + snap->cp_w),
                    (float)(*io_line_y - 3 + LINE_H));
            glDisable(GL_BLEND);

            /* Paint the right-action color swatch AFTER the active-row
             * tint. The 70%-opacity highlight spans the full row, so
             * drawing the swatch before it darkens the swatch into
             * invisibility whenever the cursor sits on a color line. */
            if (wrap_row == 0)
                text_panel_draw_right_action(snap, row, *io_line_y);

            text_panel_draw_search_highlights(snap, row, input,
                                              wrap_start, wrap_len,
                                              wrap_x, *io_line_y);

            if (anchor_pos != cursor_pos) {
                int sel_lo = anchor_pos < cursor_pos ? anchor_pos : cursor_pos;
                int sel_hi = anchor_pos > cursor_pos ? anchor_pos : cursor_pos;
                int row_lo = sel_lo > wrap_start ? sel_lo : wrap_start;
                int row_hi = sel_hi < wrap_start + wrap_len
                           ? sel_hi : wrap_start + wrap_len;

                if (row_hi > row_lo) {
                    float bx = (float)(snap->cp_x + wrap_x +
                                       (row_lo - wrap_start) * FONT_W);
                    float bw = (float)((row_hi - row_lo) * FONT_W);
                    glEnable(GL_BLEND);
                    glColor4fv(k_clr_selection_band);
                    glRectf(bx, (float)(*io_line_y - 3),
                            bx + bw, (float)(*io_line_y - 3 + LINE_H));
                    glDisable(GL_BLEND);
                }
            }

            /* Background bands first (scope band under the matched-bracket
             * cells), then the glyphs on top at their full color. */
            if (has_enclosing) {
                text_panel_draw_paren_scope_band(snap, wrap_x, wrap_start,
                                                 wrap_len, *io_line_y,
                                                 enc_open, enc_close);
            }
            text_panel_draw_bracket_cells(snap, hl, row_idx, wrap_x,
                                          wrap_start, wrap_len, *io_line_y);

            /* A sparse adapter-supplied segment list (currently the live
             * glPushAttrib bit tokens) keeps those tokens colored. Its gaps
             * use the row's legacy near-white input color; rows with no
             * segments take the original flat-wash path below. */
            if (row->color_segment_count > 0) {
                text_panel_draw_colored_text(snap, row, input, wrap_start,
                                             wrap_len, wrap_x, *io_line_y);
            } else {
                UiTextPanelColor input_color = ui_text_panel_input_text_color();
                text_panel_set_color(&input_color);
                text_panel_draw_segment(snap->cp_x + wrap_x, *io_line_y, input,
                                        wrap_start, wrap_len, FONT_MONO,
                                        snap->comment_rule_ligature);
            }

            if (wrap_row == cursor_row) {
                int cursor_x = snap->cp_x + wrap_x + cursor_col * FONT_W;
                int hint_x = cursor_x;

                if (snap->input.ghost && snap->input.ghost[0] &&
                    cursor_pos == input_len) {
                    glEnable(GL_BLEND);
                    glColor4fv(k_clr_ghost_text);
                    gl2d_draw_string((float)cursor_x, (float)(*io_line_y),
                                     snap->input.ghost, FONT_MONO);
                    glDisable(GL_BLEND);
                    hint_x += (int)strlen(snap->input.ghost) * FONT_W;
                }

                if (snap->input.hint && snap->input.hint[0] &&
                    cursor_pos == input_len) {
                    glEnable(GL_BLEND);
                    glColor4fv(k_clr_hint_text);
                    gl2d_draw_string((float)hint_x, (float)(*io_line_y),
                                     snap->input.hint, FONT_MONO);
                    glDisable(GL_BLEND);
                }

                if (snap->input.cursor_visible && !snap->search.active) {
                    glEnable(GL_BLEND);
                    glColor4fv(k_clr_cursor_caret);
                    glRectf((float)cursor_x, (float)(*io_line_y - 2),
                            (float)(cursor_x + 2),
                            (float)(*io_line_y - 2 + FONT_H + 2));
                    glDisable(GL_BLEND);
                }

                if (out) {
                    out->cursor_px = cursor_x;
                    out->cursor_py = *io_line_y;
                    out->cursor_valid = 1;
                }
            }

            *io_line_y -= LINE_H;
        }

        (*io_cur)++;
        wrap_row++;
    }

    return wrap_row > 0 ? wrap_row : 1;
}

static int text_panel_draw_regular_row(const UiTextPanelSnapshot *snap,
                                       const UiTextPanelRow *row,
                                       int row_idx,
                                       const TextPanelBracketHL *hl,
                                       int visible_rows,
                                       int *io_cur,
                                       int *io_line_y) {
    const char *text = row->text ? row->text : "";
    int wrap_row = 0;
    int wrap_start;
    int wrap_len;
    int wrap_x;
    CodeWrapIter wrap_it;
    CodeLayout layout = text_panel_row_layout(snap, row);

    if (row->kind == UI_TEXT_PANEL_ROW_PLACEHOLDER && text[0] == '\0') {
        if (*io_cur >= snap->scroll && *io_cur < snap->scroll + visible_rows) {
            text_panel_draw_row_background(snap, row, *io_line_y);
            text_panel_draw_line_number(snap, *io_line_y, row->left_gutter_label);
            text_panel_draw_left_aux(snap, row, *io_line_y);
            text_panel_draw_right_action(snap, row, *io_line_y);
            *io_line_y -= LINE_H;
        }
        (*io_cur)++;
        return 1;
    }

    code_layout_wrap_iter_init(&wrap_it, text, &layout);
    while (code_layout_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
        if (*io_cur >= snap->scroll && *io_cur < snap->scroll + visible_rows) {
            text_panel_draw_row_background(snap, row, *io_line_y);
            if (wrap_row == 0) {
                text_panel_draw_line_number(snap, *io_line_y,
                                            row->left_gutter_label);
                text_panel_draw_left_aux(snap, row, *io_line_y);
                text_panel_draw_right_action(snap, row, *io_line_y);
            }

            text_panel_draw_search_highlights(snap, row, text,
                                              wrap_start, wrap_len,
                                              wrap_x, *io_line_y);
            /* Matching-bracket cell (behind the glyphs) when this row owns
             * the caret's partner brace on another line. */
            text_panel_draw_bracket_cells(snap, hl, row_idx, wrap_x,
                                          wrap_start, wrap_len, *io_line_y);
            text_panel_draw_colored_text(snap, row, text,
                                         wrap_start, wrap_len,
                                         wrap_x, *io_line_y);
            *io_line_y -= LINE_H;
        }
        (*io_cur)++;
        wrap_row++;
    }

    return wrap_row > 0 ? wrap_row : 1;
}

/* --- Scrollbar -----------------------------------------------------------
 *
 * The strip is inset from the panel's right edge rather than flush against
 * it: at the edge it would sit inside the resize divider's grab band (the
 * divider is classified first, so those pixels can never start a thumb drag
 * in the LEFT layout). TEXT_PANEL_SCROLLBAR_INSET clears that band, and the
 * click band's left edge stops short of the inline right-action swatch
 * column (ui_text_panel_right_action_rect) so swatch clicks stay reachable.
 */
#define TEXT_PANEL_SCROLLBAR_W         7
#define TEXT_PANEL_SCROLLBAR_INSET     4
#define TEXT_PANEL_SCROLLBAR_GRAB_PAD  1
#define TEXT_PANEL_SCROLLBAR_MIN_THUMB 12

typedef struct {
    int track_x, track_y, track_w, track_h;
    int thumb_y, thumb_h;
    int max_scroll;  /* total_rows - visible_rows, > 0 */
    int travel;      /* track_h - thumb_h, the thumb top's usable range */
} TextPanelScrollbar;

static int text_panel_total_visual_rows(const UiTextPanelSnapshot *snap) {
    int total = 0;

    for (int i = 0; i < snap->row_count; i++) {
        int rows = text_panel_row_wrap_count_cached(snap, i);
        total += rows < 1 ? 1 : rows;
    }
    return total;
}

/* Solve the scrollbar for the snapshot's current scroll. Returns 0 when the
 * panel shows no scrollbar. The thumb is placed along `travel` (not the raw
 * track height) so a thumb clamped up to the minimum height still bottoms
 * out exactly at the track's bottom edge — that is what makes the placement
 * exactly invertible for drag mapping. */
static int text_panel_scrollbar_solve(const UiTextPanelSnapshot *snap,
                                      TextPanelScrollbar *out) {
    TextPanelViewportMetrics metrics;
    TextPanelScrollbar sb;
    int total_rows;
    int track_top;
    int scroll;

    if (!snap || snap->cp_w <= 0 || snap->cp_h <= 0 ||
        !(snap->chrome_flags & UI_TEXT_PANEL_CHROME_SCROLLBAR))
        return 0;

    metrics = text_panel_viewport_metrics(snap);
    total_rows = text_panel_total_visual_rows(snap);
    if (total_rows <= metrics.visible_rows)
        return 0;

    track_top = snap->cp_y + snap->cp_h - CODE_MARGIN_Y - LINE_H
                - snap->top_chrome_h;
    sb.track_h = snap->cp_h - CODE_MARGIN_Y - LINE_H - metrics.statusbar_h
                 - snap->top_chrome_h;
    if (sb.track_h <= 0)
        return 0;

    sb.track_w = TEXT_PANEL_SCROLLBAR_W;
    sb.track_x = snap->cp_x + snap->cp_w - TEXT_PANEL_SCROLLBAR_INSET
                 - sb.track_w;
    sb.track_y = track_top - sb.track_h;

    sb.thumb_h = (int)((float)sb.track_h * (float)metrics.visible_rows
                       / (float)total_rows);
    if (sb.thumb_h < TEXT_PANEL_SCROLLBAR_MIN_THUMB)
        sb.thumb_h = TEXT_PANEL_SCROLLBAR_MIN_THUMB;
    if (sb.thumb_h > sb.track_h)
        sb.thumb_h = sb.track_h;

    sb.max_scroll = total_rows - metrics.visible_rows;
    sb.travel = sb.track_h - sb.thumb_h;

    scroll = snap->scroll;
    if (scroll < 0)
        scroll = 0;
    if (scroll > sb.max_scroll)
        scroll = sb.max_scroll;

    sb.thumb_y = track_top - sb.thumb_h
                 - (int)((float)sb.travel * (float)scroll
                         / (float)sb.max_scroll);

    if (out)
        *out = sb;
    return 1;
}

int ui_text_panel_scrollbar_geometry(const UiTextPanelSnapshot *snap,
                                     UiTextPanelRect *out_track,
                                     UiTextPanelRect *out_thumb) {
    TextPanelScrollbar sb;

    if (!text_panel_scrollbar_solve(snap, &sb))
        return 0;

    if (out_track) {
        out_track->x = sb.track_x;
        out_track->y = sb.track_y;
        out_track->w = sb.track_w;
        out_track->h = sb.track_h;
    }
    if (out_thumb) {
        out_thumb->x = sb.track_x;
        out_thumb->y = sb.thumb_y;
        out_thumb->w = sb.track_w;
        out_thumb->h = sb.thumb_h;
    }
    return 1;
}

int ui_text_panel_scroll_for_thumb_top(const UiTextPanelSnapshot *snap,
                                       int thumb_top_y) {
    TextPanelScrollbar sb;
    int track_top;
    int offset;
    int scroll;

    if (!text_panel_scrollbar_solve(snap, &sb))
        return -1;
    if (sb.travel <= 0)
        return 0;

    track_top = sb.track_y + sb.track_h;
    offset = track_top - thumb_top_y;   /* pixels the thumb top moved down */
    if (offset < 0)
        offset = 0;
    if (offset > sb.travel)
        offset = sb.travel;

    /* Round to the nearest row so a thumb parked mid-row does not bias the
     * drag toward the top of the document. */
    scroll = (offset * sb.max_scroll + sb.travel / 2) / sb.travel;
    if (scroll < 0)
        scroll = 0;
    if (scroll > sb.max_scroll)
        scroll = sb.max_scroll;
    return scroll;
}

static void text_panel_draw_scrollbar(const UiTextPanelSnapshot *snap) {
    TextPanelScrollbar sb;

    if (!text_panel_scrollbar_solve(snap, &sb))
        return;

    glEnable(GL_BLEND);
    /* Faint track behind the thumb — the strip is draggable along its whole
     * length, so it reads as a control rather than a floating tick. */
    ui_clr_a(UI_TOK_TEXT_MUTED, 0.12f);
    glRectf((float)sb.track_x, (float)sb.track_y,
            (float)(sb.track_x + sb.track_w),
            (float)(sb.track_y + sb.track_h));

    ui_clr_a(UI_TOK_TEXT_MUTED, snap->scrollbar_drag ? 0.75f : 0.35f);
    glRectf((float)sb.track_x, (float)sb.thumb_y,
            (float)(sb.track_x + sb.track_w),
            (float)(sb.thumb_y + sb.thumb_h));
    glDisable(GL_BLEND);
}

/* Classify a pointer already known to be inside the panel rect against the
 * scrollbar's click band. Writes the press's grab offset (see UiHit's
 * UI_HIT_CODE_SCROLLBAR note) to *out_grab_dy. */
static int text_panel_point_on_scrollbar(const UiTextPanelSnapshot *snap,
                                         int mx, int gl_y,
                                         int *out_grab_dy) {
    TextPanelScrollbar sb;
    const int pad = TEXT_PANEL_SCROLLBAR_GRAB_PAD;

    if (!text_panel_scrollbar_solve(snap, &sb))
        return 0;
    if (mx < sb.track_x - pad || mx >= sb.track_x + sb.track_w + pad)
        return 0;
    if (gl_y < sb.track_y || gl_y >= sb.track_y + sb.track_h)
        return 0;

    if (out_grab_dy) {
        *out_grab_dy = (gl_y >= sb.thumb_y && gl_y < sb.thumb_y + sb.thumb_h)
                     ? sb.thumb_y + sb.thumb_h - gl_y  /* grabbed the thumb */
                     : sb.thumb_h / 2;                 /* track: center it */
    }
    return 1;
}

int ui_text_panel_point_on_divider(const UiTextPanelSnapshot *snap,
                                   int mx, int gl_y) {
    const int pad = UI_PANEL_DIVIDER_GRAB_PX;

    if (!snap || snap->cp_w <= 0 || snap->cp_h <= 0)
        return 0;

    if (snap->cp_w < snap->vp_w) {
        int div_x = snap->cp_x + snap->cp_w;
        return mx >= div_x - pad && mx <= div_x + pad;
    }

    if (snap->cp_y <= 0) {
        int div_y = snap->cp_y + snap->cp_h;
        return gl_y >= div_y - pad && gl_y <= div_y + pad;
    }

    return gl_y >= snap->cp_y - pad && gl_y <= snap->cp_y + pad;
}

static void text_panel_draw_chrome(const UiTextPanelSnapshot *snap) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ui_clr_a(UI_TOK_SUNKEN, 0.92f);
    glRectf((float)snap->cp_x, (float)snap->cp_y,
            (float)(snap->cp_x + snap->cp_w),
            (float)(snap->cp_y + snap->cp_h));

    ui_clr_a(UI_TOK_DIVIDER, 0.80f);
    glBegin(GL_LINES);
    if (snap->cp_w < snap->vp_w) {
        glVertex2f((float)(snap->cp_x + snap->cp_w), 0.0f);
        glVertex2f((float)(snap->cp_x + snap->cp_w), (float)snap->vp_h);
    } else if (snap->cp_y <= 0) {
        glVertex2f(0.0f, (float)(snap->cp_y + snap->cp_h));
        glVertex2f((float)snap->vp_w, (float)(snap->cp_y + snap->cp_h));
    } else {
        glVertex2f(0.0f, (float)snap->cp_y);
        glVertex2f((float)snap->vp_w, (float)snap->cp_y);
    }
    glEnd();
    glDisable(GL_BLEND);
}

static int text_panel_row_wrap_count(const UiTextPanelSnapshot *snap,
                                     const UiTextPanelRow *row) {
    CodeLayout layout = text_panel_row_layout(snap, row);
    if (row->kind == UI_TEXT_PANEL_ROW_INPUT)
        return code_layout_row_count_for_text(text_panel_row_text(snap, row),
                                              &layout);
    if (row->kind == UI_TEXT_PANEL_ROW_PLACEHOLDER &&
        (!row->text || row->text[0] == '\0'))
        return 1;
    return code_layout_row_count_for_text(text_panel_row_text(snap, row),
                                          &layout);
}

/* Indexed lookup that returns a cached wrap count when the snapshot's
 * geometry matches the cache. Falls back to a fresh compute if the
 * cache is cold (no render this frame, snap mismatch, or row_count
 * exceeded the cache cap). Used by hit_test and input_row_y. */
static int text_panel_row_wrap_count_cached(const UiTextPanelSnapshot *snap,
                                            int row_idx) {
    if (wrap_cache_matches(snap) &&
        row_idx >= 0 && row_idx < g_wrap_cache.row_count)
        return g_wrap_cache.wrap[row_idx];
    return text_panel_row_wrap_count(snap, &snap->rows[row_idx]);
}

/* Populate the wrap-count cache from a full row walk. Called once
 * from ui_text_panel_render so the hit-test / input-row-y paths can
 * reuse the counts instead of re-walking row text. */
static void wrap_cache_populate(const UiTextPanelSnapshot *snap) {
    if (!snap || snap->row_count > UI_TEXT_PANEL_WRAP_CACHE_MAX) {
        wrap_cache_invalidate();
        return;
    }
    for (int i = 0; i < snap->row_count; i++)
        g_wrap_cache.wrap[i] =
            text_panel_row_wrap_count(snap, &snap->rows[i]);
    g_wrap_cache.snap          = snap;
    g_wrap_cache.row_count     = snap->row_count;
    g_wrap_cache.cp_w          = snap->cp_w;
    g_wrap_cache.wrap_at_comma = snap->wrap_at_comma;
    g_wrap_cache.text_x        = snap->text_x;
    g_wrap_cache.valid         = 1;
}

static int text_panel_char_for_click(const UiTextPanelSnapshot *snap,
                                     const UiTextPanelRow *row,
                                     int mx,
                                     int row_offset) {
    const char *text = text_panel_row_text(snap, row);
    int text_len = text_panel_row_text_len(snap, row);
    int seg_start = text_len;
    int seg_len = 0;
    int seg_x = snap->text_x + row->indent_chars * FONT_W;
    int col;
    int new_cursor;
    CodeLayout layout = text_panel_row_layout(snap, row);

    code_layout_segment_for_row(text, &layout, row_offset,
                                &seg_start, &seg_len, &seg_x);
    col = (mx - (snap->cp_x + seg_x) + FONT_W / 2) / FONT_W;
    if (col < 0)
        col = 0;
    if (col > seg_len)
        col = seg_len;
    new_cursor = seg_start + col;
    if (new_cursor > text_len)
        new_cursor = text_len;
    return new_cursor;
}

static int text_panel_row_is_insert_slot(const UiTextPanelRow *row) {
    return row->source_line_idx < 0 &&
           (row->kind == UI_TEXT_PANEL_ROW_PLACEHOLDER ||
            row->kind == UI_TEXT_PANEL_ROW_INPUT);
}

static int text_panel_resolved_line_idx(const UiTextPanelRow *row) {
    int resolved_line = row->source_line_idx >= 0
                      ? row->source_line_idx
                      : row->hit_target_line_idx;

    /* Generic text_panel hit-testing leaves virtual rows unresolved;
     * the REPL adapter owns rewriting them to a concrete line. */
    if (row->kind == UI_TEXT_PANEL_ROW_VIRTUAL)
        return row->source_line_idx;
    return resolved_line;
}

static UiHit text_panel_hit_for_row(const UiTextPanelSnapshot *snap,
                                    const UiTextPanelRow *row,
                                    int row_idx,
                                    int mx,
                                    int row_offset) {
    UiHit h = ui_hit_none();

    if (!row->hit_eligible)
        return h;

    h.line_idx = text_panel_resolved_line_idx(row);
    h.visual_row = row_offset;
    h.cmd_idx = row_idx;

    if (mx < snap->cp_x + snap->text_x) {
        h.kind = UI_HIT_CODE_GUTTER;
        return h;
    }

    h.char_idx = text_panel_char_for_click(snap, row, mx, row_offset);
    h.kind = text_panel_row_is_insert_slot(row)
           ? UI_HIT_CODE_INSERT_LINE
           : UI_HIT_CODE_TEXT;
    return h;
}

int ui_text_panel_visible_lines_for_height(int panel_h, int statusbar_h,
                                            int top_chrome_h) {
    /* The first visible text baseline sits below the top gutter and
     * top chrome. Each additional visible row then consumes one LINE_H. */
    int visible_band_h = panel_h - CODE_MARGIN_Y - 2 * LINE_H - 3
                       - statusbar_h - top_chrome_h;

    if (visible_band_h < 0)
        return 1;
    return visible_band_h / LINE_H + 1;
}

int ui_text_panel_row_y(const UiTextPanelSnapshot *snap,
                        int row_idx,
                        int *out_py) {
    TextPanelViewportMetrics metrics;
    int cur;

    if (!snap || !out_py || row_idx < 0 || row_idx >= snap->row_count)
        return 0;

    metrics = text_panel_viewport_metrics(snap);
    cur = 0;

    for (int i = 0; i < snap->row_count; i++) {
        int visual_rows = text_panel_row_wrap_count_cached(snap, i);
        if (visual_rows < 1) visual_rows = 1;

        if (i == row_idx) {
            if (cur < snap->scroll ||
                cur >= snap->scroll + metrics.visible_rows)
                return 0;
            *out_py = metrics.first_line_y - (cur - snap->scroll) * LINE_H;
            return 1;
        }
        cur += visual_rows;
    }
    return 0;
}

int ui_text_panel_input_row_y(const UiTextPanelSnapshot *snap,
                              int input_row_idx,
                              int *out_py) {
    return ui_text_panel_row_y(snap, input_row_idx, out_py);
}

/* Resolve the active-input-row bracket-pair highlight for this frame.
 * The caret's '(' / ')' pairs within the input row; its '{' / '}' may
 * pair across rows (if / for blocks). Suppressed when the paren_match
 * aid is off, a range selection is active, or the caret is not on a
 * bracket. */
static TextPanelBracketHL text_panel_resolve_bracket_hl(
    const UiTextPanelSnapshot *snap) {
    TextPanelBracketHL hl = {0};
    const char *in;
    int in_len, cursor, anchor, input_row = -1, i;
    char ch;

    if (!snap->paren_match)
        return hl;

    for (i = 0; i < snap->row_count; i++) {
        if (snap->rows[i].kind == UI_TEXT_PANEL_ROW_INPUT) {
            input_row = i;
            break;
        }
    }
    if (input_row < 0)
        return hl;

    in     = snap->input.input ? snap->input.input : "";
    in_len = snap->input.input_len >= 0 ? snap->input.input_len
                                        : (int)strlen(in);
    cursor = snap->input.cursor;
    anchor = snap->input.anchor < 0 ? cursor : snap->input.anchor;

    /* Char "in front of" the caret drives the pair; suppress under a
     * range selection so the bands do not fight. */
    if (anchor != cursor || cursor < 0 || cursor >= in_len)
        return hl;

    ch = in[cursor];
    if (ch == '(' || ch == ')') {
        int s, m;
        if (ui_text_panel_match_paren(in, in_len, cursor, &s, &m)) {
            hl.active = 1;
            hl.self_row = hl.match_row = input_row;
            hl.self_char = s;
            hl.match_char = m;
        }
    } else if (ch == '{' || ch == '}') {
        int mr, mc;
        if (ui_text_panel_match_bracket_multiline(snap, input_row, cursor,
                                                  &mr, &mc)) {
            hl.active = 1;
            hl.self_row = input_row;
            hl.self_char = cursor;
            hl.match_row = mr;
            hl.match_char = mc;
        }
    }
    return hl;
}

void ui_text_panel_render(const UiTextPanelSnapshot *snap,
                          UiTextPanelOutput         *out) {
    TextPanelViewportMetrics metrics;
    TextPanelBracketHL bracket_hl;
    int line_y;
    int cur = 0;
    int total_rows = 0;

    if (out)
        *out = (UiTextPanelOutput){0};
    if (!snap || !out)
        return;

    /* Fill the per-frame wrap-count cache so the hit-test and
     * input-row-y paths can reuse our walk instead of redoing it. */
    prof_begin(PROF_CODE_PANEL_TEXT_LAYOUT);
    wrap_cache_populate(snap);

    metrics = text_panel_viewport_metrics(snap);
    prof_end(PROF_CODE_PANEL_TEXT_LAYOUT);

    out->visible_rows = metrics.visible_rows;
    out->statusbar_slot = (UiTextPanelRect){
        .x = snap->cp_x,
        .y = snap->cp_y,
        .w = snap->cp_w,
        .h = metrics.statusbar_h,
    };
    out->text_area = (UiTextPanelRect){
        .x = snap->cp_x + snap->text_x,
        .y = snap->cp_y + metrics.statusbar_h,
        .w = snap->cp_w - snap->text_x - CODE_MARGIN_X,
        .h = snap->cp_h - metrics.statusbar_h,
    };

    if (snap->vp_w <= 0 || snap->vp_h <= 0 || snap->cp_w <= 0 || snap->cp_h <= 0)
        return;

    gl2d_begin(snap->vp_w, snap->vp_h);
    /* Chrome is two disjoint spans (background/border here, scrollbar after
     * the row loop), so it uses the accumulate-then-commit probes. */
    prof_accum_reset(PROF_CODE_PANEL_TEXT_CHROME);
    prof_begin(PROF_CODE_PANEL_TEXT_CHROME);
    text_panel_draw_chrome(snap);
    prof_accum_end(PROF_CODE_PANEL_TEXT_CHROME);

    /* Clip the text rows to the panel interior. Rows are laid out
     * left-to-right with no per-row width clamp, so a long unbroken
     * line, the right-action swatch, or wide indented content would
     * otherwise bleed past the panel's right edge onto the adjacent
     * scene in split layouts. Chrome (background + the deliberately
     * full-extent divider) is drawn above this, unclipped; the
     * scrollbar is drawn after, unclipped (already within bounds).
     * gl2d's gluOrtho2D(0,vp_w,0,vp_h) over the full-window viewport
     * maps panel coords 1:1 to scissor window coords. cp_w/cp_h are
     * > 0 here (guarded above). No glPushAttrib covers scissor state,
     * so it is disabled explicitly after the loop. */
    glEnable(GL_SCISSOR_TEST);
    glScissor(snap->cp_x, snap->cp_y, snap->cp_w, snap->cp_h);

    prof_begin(PROF_CODE_PANEL_TEXT_LINES);
    line_y = metrics.first_line_y;
    bracket_hl = text_panel_resolve_bracket_hl(snap);

    for (int i = 0; i < snap->row_count; i++) {
        const UiTextPanelRow *row = &snap->rows[i];

        if (row->kind == UI_TEXT_PANEL_ROW_INPUT)
            total_rows += text_panel_draw_input_row(snap, row, i, &bracket_hl,
                                                    metrics.visible_rows,
                                                    &cur, &line_y, out);
        else
            total_rows += text_panel_draw_regular_row(snap, row, i, &bracket_hl,
                                                      metrics.visible_rows,
                                                      &cur, &line_y);
    }

    glDisable(GL_SCISSOR_TEST);
    prof_end(PROF_CODE_PANEL_TEXT_LINES);

    out->total_rows = total_rows;
    prof_begin(PROF_CODE_PANEL_TEXT_CHROME);
    text_panel_draw_scrollbar(snap);
    prof_accum_end(PROF_CODE_PANEL_TEXT_CHROME);
    prof_accum_commit(PROF_CODE_PANEL_TEXT_CHROME);
    gl2d_end();
}

UiHit ui_text_panel_hit_test(const UiTextPanelSnapshot *snap,
                              int mx, int my) {
    UiHit h = ui_hit_none();
    TextPanelViewportMetrics metrics;
    int gl_y;
    int row_from_top;
    int target_visual_row;
    int cur = 0;

    if (!snap || snap->vp_w <= 0 || snap->vp_h <= 0)
        return h;

    metrics = text_panel_viewport_metrics(snap);

    gl_y = snap->vp_h - my;

    if (ui_text_panel_point_on_divider(snap, mx, gl_y)) {
        h.kind = UI_HIT_PANEL_DIVIDER;
        h.local_x = (float)mx;
        h.local_y = (float)gl_y;
        return h;
    }

    if (mx < snap->cp_x || mx >= snap->cp_x + snap->cp_w ||
        gl_y < snap->cp_y || gl_y >= snap->cp_y + snap->cp_h)
        return h;

    /* Scrollbar ahead of the rows underneath it: the strip overlaps the
     * right end of every visible row, and a press there is a scroll gesture,
     * not a cursor move. */
    {
        int grab_dy = 0;
        if (text_panel_point_on_scrollbar(snap, mx, gl_y, &grab_dy)) {
            h.kind = UI_HIT_CODE_SCROLLBAR;
            h.item_idx = grab_dy;
            h.local_x = (float)(mx - snap->cp_x);
            h.local_y = (float)(gl_y - snap->cp_y);
            return h;
        }
    }

    row_from_top = (metrics.first_line_y + LINE_H - 3 - gl_y) / LINE_H;
    if (row_from_top < 0 || row_from_top >= metrics.visible_rows)
        return h;

    target_visual_row = snap->scroll + row_from_top;
    h.local_x = (float)(mx - snap->cp_x);
    h.local_y = (float)(gl_y - snap->cp_y);

    for (int i = 0; i < snap->row_count; i++) {
        const UiTextPanelRow *row = &snap->rows[i];
        int wrap_count = text_panel_row_wrap_count_cached(snap, i);

        if (target_visual_row < cur + wrap_count)
            return text_panel_hit_for_row(snap, row, i, mx,
                                          target_visual_row - cur);

        cur += wrap_count;
    }

    return h;
}

int ui_text_panel_right_action_rect(const UiTextPanelSnapshot *snap,
                                    int line_y,
                                    int *out_sx, int *out_sy, int *out_sw) {
    if (!snap)
        return 0;

    int sw = UI_TEXT_PANEL_RIGHT_ACTION_W;
    int sx = snap->cp_x + snap->cp_w - CODE_MARGIN_X - sw - 2;
    int sy = line_y + (LINE_H - sw) / 2 - 1;

    if (out_sx) *out_sx = sx;
    if (out_sy) *out_sy = sy;
    if (out_sw) *out_sw = sw;
    return 1;
}
