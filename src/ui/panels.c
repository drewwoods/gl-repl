/*
 * ui_panels.c - Code-panel row rendering, autocomplete/help/variable panels,
 *               panel input routing, and inline scene rename UI.
 *
 * Extracted from sample.c for maintainability.
 */
#include "repl_command_spec.h"
#include "repl_state_views.h"
#include "state.h"
#include "repl_export.h"
#include "layout.h"
#include "color_picker.h"
#include "metrics.h"
#include "editor/code_panel_document.h"
#include "repl_core.h"
#include "editor/search.h"
#include "menu_bar.h"
#include "variable_panel.h"
#include "repl_replay_annotations.h"
#include "prof.h"
#include "panels.h"
#include "./include/gl_2d.h"

#include <string.h>

/* Header / footer / camera scaffolding text lives in the import_export
 * view. Render code reads it through snap->import_export instead of
 * repl_state_*() so the per-row branch stays snapshot-only. */

static const EditorTransformer *find_color_transformer(const EditorTransformerList *list,
                                                       int line_idx) {
    if (!list)
        return NULL;
    for (int i = 0; i < list->count; i++) {
        const EditorTransformer *t = &list->items[i];
        if (t->kind == TRANSFORMER_COLOR_PICKER && t->line_idx == line_idx)
            return t;
    }
    return NULL;
}


/* Status footer height - design: 22px strip flush against code panel bottom */
/* STATUSBAR_H lives in ui_metrics.h now so scene_render.c can lift the
 * replay HUD above the amber status strip.  */

/* ========================================================================= */
/* Syntax color helpers                                                       */
/* ========================================================================= */

/* Category → RGB palette. The category for each CmdType is declared in
 * repl_command_spec.c's g_command_type_specs[] table; the renderer just
 * looks the color up here. Adding a new CmdType picks up its highlight
 * automatically — set its category in the spec table and the renderer
 * follows. Tweak the palette here if you want to change how a category
 * looks; the spec table doesn't change. */
static const struct { float r, g, b; } k_category_colors[CMD_CAT_COUNT] = {
    [CMD_CAT_DEFAULT]     = { 0.70f, 0.70f, 0.70f },  /* gray (fallback) */
    [CMD_CAT_PRIMITIVE]   = { 0.85f, 0.45f, 0.85f },  /* magenta — glBegin/glEnd */
    [CMD_CAT_VERTEX]      = { 0.40f, 0.90f, 0.40f },  /* green */
    [CMD_CAT_NORMAL]      = { 0.40f, 0.80f, 0.95f },  /* cyan */
    [CMD_CAT_COLOR]       = { 0.95f, 0.85f, 0.30f },  /* yellow */
    [CMD_CAT_TRANSFORM]   = { 0.95f, 0.65f, 0.40f },  /* orange */
    [CMD_CAT_STATE]       = { 0.80f, 0.70f, 0.95f },  /* lavender */
    [CMD_CAT_LOOP]        = { 0.95f, 0.60f, 0.30f },  /* orange-red */
    [CMD_CAT_FUNCTION]    = { 0.60f, 0.85f, 0.95f },  /* sky blue */
    [CMD_CAT_VARIABLE]    = { 0.55f, 0.80f, 0.95f },  /* slightly cooler blue */
    [CMD_CAT_CONDITIONAL] = { 0.95f, 0.75f, 0.50f },  /* peach */
    [CMD_CAT_LABEL]       = { 0.85f, 0.55f, 0.85f },  /* purple */
    [CMD_CAT_COMMENT]     = { 0.45f, 0.50f, 0.45f },  /* gray-green */
    [CMD_CAT_GLUT_SHAPE]  = { 0.50f, 0.90f, 0.70f },  /* mint */
    [CMD_CAT_TESS_BLOCK]  = { 0.70f, 0.55f, 0.90f },  /* violet */
};

static void color_for_type(CmdType t) {
    CmdSyntaxCategory cat = repl_cmd_type_category(t);
    if (cat < 0 || cat >= CMD_CAT_COUNT) cat = CMD_CAT_DEFAULT;
    glColor3f(k_category_colors[cat].r,
              k_category_colors[cat].g,
              k_category_colors[cat].b);
}

static void category_rgb(CmdSyntaxCategory cat,
                         float *r, float *g, float *b) {
    if (cat < 0 || cat >= CMD_CAT_COUNT)
        cat = CMD_CAT_DEFAULT;
    if (r) *r = k_category_colors[cat].r;
    if (g) *g = k_category_colors[cat].g;
    if (b) *b = k_category_colors[cat].b;
}

static void code_panel_static_line_rgb(const char *text,
                                       float fallback_r,
                                       float fallback_g,
                                       float fallback_b,
                                       float *r,
                                       float *g,
                                       float *b) {
    if (text && strcmp(text, REPL_CODE_PANEL_SCRATCH_DECL_LINE) == 0) {
        category_rgb(CMD_CAT_VARIABLE, r, g, b);
        return;
    }
    if (r) *r = fallback_r;
    if (g) *g = fallback_g;
    if (b) *b = fallback_b;
}

/* Replay annotations live in repl_replay_annotations.c. */

/* ========================================================================= */
/* Code panel                                                                 */
/* ========================================================================= */

static void code_panel_draw_segment(int x, int y, const char *text,
                                    int start, int len, void *font) {
    char buf[MAX_INPUT_LEN];

    if (len <= 0)
        return;
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;

    memcpy(buf, text + start, (size_t)len);
    buf[len] = '\0';
    gl2d_draw_string((float)x, (float)y, buf, font);
}

/* Menu bar lives in repl_menu_bar.c. */

static void code_panel_draw_search_highlights(const UiRenderSnapshot *snap,
                                              const char *text, int search_row_idx,
                                              int seg_start, int seg_len,
                                              int seg_x, int y) {
    ReplSearchState srch = snap->search;
    int drew = 0;

    if (!srch.active || srch.query_len <= 0 || search_row_idx < 0 ||
        !text || seg_len <= 0)
        return;

    for (int pos = repl_search_find_next_in_text(text, srch.query, 0);
         pos >= 0;
         pos = repl_search_find_next_in_text(text, srch.query, pos + 1)) {
        int match_end = pos + srch.query_len;
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

        if (search_row_idx == srch.hit_line_idx && pos == srch.hit_char_idx)
            glColor4f(0.95f, 0.65f, 0.18f, 0.55f);
        else
            glColor4f(0.25f, 0.45f, 0.85f, 0.30f);

        glRectf((float)((float)(seg_x + (draw_start - seg_start) * FONT_W)), (float)((float)(y - 2)), (float)((float)(seg_x + (draw_start - seg_start) * FONT_W))+(float)((float)((draw_end - draw_start) * FONT_W)), (float)((float)(y - 2))+(float)(FONT_H + 4));
    }

    if (drew)
        glDisable(GL_BLEND);
}

static void render_active_input_rows(const UiRenderSnapshot *snap,
                                     int panel_w, int text_x, int idx_x,
                                     int visible_lines, int file_line,
                                     int indent_chars, const char *idx_text,
                                     int search_row_idx,
                                     UiCodePanelOutput *out,
                                     int *io_cur, int *io_line_y) {
    ReplEditorInputView            inp    = snap->editor_input;
    ReplCodePanelRuntimeState cp     = snap->code_panel;
    ReplAutocompleteState           ac     = snap->autocomplete;
    ReplSearchState                 srch   = snap->search;
    const char *input = inp.input;
    int cursor_pos = snap->editor_input.cursor_pos;
    int input_len = inp.input_len;
    CodePanelWrapIter wrap_it;
    int wrap_row = 0;
    int wrap_start, wrap_len, wrap_x;
    int input_x = text_x + indent_chars * FONT_W;
    int cursor_seg_start = 0;
    int cursor_seg_len = 0;
    int cursor_seg_x = input_x;
    int cursor_row = repl_code_panel_document_cursor_row_for_text(input, input_x, panel_w,
                                                    cursor_pos,
                                                    &cursor_seg_start,
                                                    &cursor_seg_len,
                                                    &cursor_seg_x);
    int cursor_col = cursor_pos - cursor_seg_start;

    repl_code_panel_document_wrap_iter_init(&wrap_it, input, input_x, panel_w);
    while (repl_code_panel_document_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
        if (*io_cur >= snap->scroll.scroll && *io_cur < snap->scroll.scroll + visible_lines) {
            glColor3f(0.55f, 0.55f, 0.30f);
            if (wrap_row == 0) {
                char ln[16];
                snprintf(ln, sizeof(ln), "%3d", file_line);
                gl2d_draw_string((float)CODE_MARGIN_X, (float)(*io_line_y), ln, FONT_MONO);
                if (idx_text) {
                    glColor3f(0.45f, 0.50f, 0.65f);
                    gl2d_draw_string((float)idx_x, (float)(*io_line_y), idx_text, FONT_MONO);
                }
            }

            glEnable(GL_BLEND);
            glColor4f(0.15f, 0.18f, 0.28f, 0.70f);
            glRectf((float)(0), (float)((float)(*io_line_y - 3)), (float)(0)+(float)((float)panel_w), (float)((float)(*io_line_y - 3))+(float)(LINE_H));
            glDisable(GL_BLEND);

            if (wrap_row == 0 && indent_chars > 0) {
                char spaces[32];
                int draw_indent = indent_chars;
                if (draw_indent > (int)sizeof(spaces) - 1)
                    draw_indent = (int)sizeof(spaces) - 1;
                memset(spaces, ' ', (size_t)draw_indent);
                spaces[draw_indent] = '\0';
                glColor3f(0.30f, 0.30f, 0.38f);
                gl2d_draw_string((float)text_x, (float)(*io_line_y), spaces, FONT_MONO);
            }

            glColor3f(0.95f, 0.95f, 0.90f);
            code_panel_draw_search_highlights(snap, input, search_row_idx,
                                              wrap_start, wrap_len,
                                              wrap_x, *io_line_y);
            code_panel_draw_segment(wrap_x, *io_line_y, input,
                                    wrap_start, wrap_len, FONT_MONO);

            if (wrap_row == cursor_row) {
                int cursor_x = wrap_x + cursor_col * FONT_W;
                int hint_x = cursor_x;

                if (ac.ghost[0] && cursor_pos == input_len) {
                    glEnable(GL_BLEND);
                    glColor4f(0.50f, 0.55f, 0.65f, 0.55f);
                    gl2d_draw_string((float)cursor_x, (float)(*io_line_y),
                                ac.ghost, FONT_MONO);
                    glDisable(GL_BLEND);
                    hint_x += (int)strlen(ac.ghost) * FONT_W;
                }

                if (ac.hint[0] && cursor_pos == input_len) {
                    glEnable(GL_BLEND);
                    glColor4f(0.56f, 0.62f, 0.72f, 0.38f);
                    gl2d_draw_string((float)hint_x, (float)(*io_line_y),
                                ac.hint, FONT_MONO);
                    glDisable(GL_BLEND);
                }

                if (cp.cursor_visible && !srch.active) {
                    glEnable(GL_BLEND);
                    glColor4f(0.90f, 0.80f, 0.25f, 0.85f);
                    glRectf((float)((float)cursor_x), (float)((float)(*io_line_y - 2)), (float)((float)cursor_x)+(float)(2.0f), (float)((float)(*io_line_y - 2))+(float)(FONT_H + 2));
                    glDisable(GL_BLEND);
                }

                if (out) {
                    out->cursor_px    = cursor_x;
                    out->cursor_py    = *io_line_y;
                    out->cursor_valid = 1;
                }
            }

            *io_line_y -= LINE_H;
        }

        (*io_cur)++;
        wrap_row++;
    }
}

int ui_panels_code_panel_apply_scroll_follow_for_test(int show_vertex_indices,
                                            int *out_follow_doc_line,
                                            int *out_visible_lines) {
    CodePanelDocumentLayout layout;
    int cp_x, cp_y, cp_w, cp_h;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = show_vertex_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    (void)cp_x;
    (void)cp_y;

    /* Tests drive this helper directly without going through the
     * controller's per-frame snapshot build, so refresh the
     * virtual-line list here so layout sees the same shape it does
     * in production. */
    repl_replay_annotations_prepare(editor_buffer_view());
    repl_code_panel_document_build(&layout, cp_w, text_x, cp_h);
    repl_code_panel_document_apply_follow_scroll(&layout);

    if (out_follow_doc_line)
        *out_follow_doc_line = layout.follow_doc_line;
    if (out_visible_lines)
        *out_visible_lines = layout.visible_lines;
    int scroll = editor_scroll();
    return layout.follow_doc_line >= scroll &&
           layout.follow_doc_line < scroll + layout.visible_lines;
}

/* Color picker lives in repl_color_picker.c. */

/* ========================================================================= */
/* Code-panel render helpers                                                  */
/*                                                                            */
/* ui_panels_render_code_panel() is split across these helpers to keep each  */
/* phase focused on one concern. They all advance shared row-cursor state    */
/* (visible-row counter, current y in OpenGL coords, file-line gutter        */
/* number) through the CodePanelRowCtx struct so the orchestrator does not   */
/* need to thread half a dozen mutable scalars by hand.                      */
/* ========================================================================= */

typedef struct {
    const UiRenderSnapshot *snap;
    int  panel_w;
    int  text_x;
    int  idx_x;
    int  cp_x;
    int  cp_w;
    int  scroll;
    int  visible_lines;
    int  highlight_normal_idx;
    int  highlight_color_idx;
    /* Per-frame render output discovered while drawing the active
     * input row. NULL when the caller (e.g. test fixtures) doesn't
     * need cursor-pixel publishing. */
    UiCodePanelOutput *out;
    /* Mutable row cursor state advanced as rows are emitted. */
    int  cur;
    int  line_y;
    int  file_line;
} CodePanelRowCtx;

/* Return 1 if the row at ctx->cur is inside the visible scroll window. */
static int code_panel_row_visible(const CodePanelRowCtx *ctx) {
    return ctx->cur >= ctx->scroll &&
           ctx->cur < ctx->scroll + ctx->visible_lines;
}

/* Draw "  3" gutter line number at the current row. */
static void code_panel_draw_gutter_lineno(int line_y, int file_line) {
    char ln[16];
    snprintf(ln, sizeof(ln), "%3d", file_line);
    glColor3f(0.30f, 0.30f, 0.38f);
    gl2d_draw_string((float)CODE_MARGIN_X, (float)line_y, ln, FONT_MONO);
}

/* Background, border divider, menu bar, and search overlay. Mirrors the
 * Header Wireframes v2 chrome and runs once per frame. */
static void code_panel_draw_chrome(const UiRenderSnapshot *snap,
                                   int cp_x, int cp_y, int cp_w, int cp_h,
                                   int panel_top, int panel_w) {
    /* Background fill */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.06f, 0.06f, 0.10f, 0.92f);
    glRectf((float)cp_x, (float)cp_y,
            (float)cp_x + (float)cp_w, (float)cp_y + (float)cp_h);

    /* Divider between code panel and scene; orientation depends on layout. */
    glColor4f(0.30f, 0.30f, 0.50f, 0.80f);
    glBegin(GL_LINES);
    {
        int layout = snap->code_panel.layout_mode;
        if (layout < 0 || layout >= CODE_PANEL_LAYOUT_COUNT)
            layout = CODE_PANEL_LAYOUT_LEFT;
        if (layout == CODE_PANEL_LAYOUT_TOP) {
            glVertex2f(0.0f, (float)cp_y);
            glVertex2f((float)snap->viewport.window_w, (float)cp_y);
        } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
            glVertex2f(0.0f, (float)(cp_y + cp_h));
            glVertex2f((float)snap->viewport.window_w, (float)(cp_y + cp_h));
        } else {
            glVertex2f((float)(cp_x + cp_w), 0.0f);
            glVertex2f((float)(cp_x + cp_w), (float)snap->viewport.window_h);
        }
    }
    glEnd();
    glDisable(GL_BLEND);

    ui_menu_bar_render(snap);
    ui_menu_bar_render_search_overlay(snap, cp_x, panel_w, panel_top);
}

/* One static (header / footer / workspace) line, possibly wrapped. The
 * gutter line number is drawn on the first wrap row only. */
static void code_panel_draw_static_line(CodePanelRowCtx *ctx, const char *text,
                                        float r, float g, float b) {
    CodePanelWrapIter wrap_it;
    int wrap_row = 0;
    int wrap_start, wrap_len, wrap_x;
    float draw_r, draw_g, draw_b;

    code_panel_static_line_rgb(text, r, g, b, &draw_r, &draw_g, &draw_b);

    repl_code_panel_document_wrap_iter_init(&wrap_it, text, ctx->text_x, ctx->panel_w);
    while (repl_code_panel_document_wrap_iter_next(&wrap_it,
                                                   &wrap_start, &wrap_len, &wrap_x)) {
        if (code_panel_row_visible(ctx)) {
            if (wrap_row == 0)
                code_panel_draw_gutter_lineno(ctx->line_y, ctx->file_line);
            glColor3f(draw_r, draw_g, draw_b);
            code_panel_draw_segment(wrap_x, ctx->line_y, text,
                                    wrap_start, wrap_len, FONT_MONO);
            ctx->line_y -= LINE_H;
        }
        ctx->cur++;
        wrap_row++;
    }
    ctx->file_line++;
}

/* Per-row gutter + background overlays for an existing (non-edit) command
 * row: replay PC bar, clipboard selection band, feeding-cmd accent. */
static void code_panel_draw_row_overlays(const CodePanelRowCtx *ctx, int i) {
    ReplReplayRuntimeState replay = ctx->snap->replay;

    if (replay.active && replay.src_line_idx >= 0 && i == replay.src_line_idx) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0.10f, 0.35f, 0.15f, 0.55f);
        glRectf(0.0f, (float)(ctx->line_y - 3),
                (float)ctx->panel_w, (float)(ctx->line_y - 3) + (float)LINE_H);
        glColor4f(0.20f, 0.90f, 0.30f, 0.85f);
        glRectf(1.0f, (float)(ctx->line_y - 3),
                1.0f + 3.0f, (float)(ctx->line_y - 3) + (float)LINE_H);
        glDisable(GL_BLEND);
    }
    if (ctx->snap->selection_active &&
        i >= ctx->snap->selection_lo && i <= ctx->snap->selection_hi) {
        glEnable(GL_BLEND);
        glColor4f(0.20f, 0.30f, 0.50f, 0.55f);
        glRectf(0.0f, (float)(ctx->line_y - 3),
                (float)ctx->panel_w, (float)(ctx->line_y - 3) + (float)LINE_H);
        glDisable(GL_BLEND);
    }
    if (i == ctx->highlight_normal_idx || i == ctx->highlight_color_idx) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (i == ctx->highlight_normal_idx)
            glColor4f(0.40f, 0.80f, 0.95f, 0.85f);
        else
            glColor4f(0.95f, 0.85f, 0.30f, 0.85f);
        glRectf(1.0f, (float)(ctx->line_y - 3),
                1.0f + 3.0f, (float)(ctx->line_y - 3) + (float)LINE_H);
        glDisable(GL_BLEND);
    }
}

/* First-wrap-row gutter contents: line number, vertex index, color swatch. */
static void code_panel_draw_row_gutter(const CodePanelRowCtx *ctx, int i,
                                       int is_vertex, int vnum,
                                       int primitive_vnums_exact) {
    code_panel_draw_gutter_lineno(ctx->line_y, ctx->file_line);

    if (ctx->snap->code_panel.show_vertex_indices && is_vertex) {
        char idx_s[16];
        snprintf(idx_s, sizeof(idx_s),
                 primitive_vnums_exact ? "v%d" : "vn", vnum);
        glColor3f(0.45f, 0.50f, 0.65f);
        gl2d_draw_string((float)ctx->idx_x, (float)ctx->line_y,
                         idx_s, FONT_MONO);
    }

    /* Color swatch read from the controller-pushed transformer snapshot
     * so the row stays decoupled from live document scans. */
    const EditorTransformer *ct =
        find_color_transformer(ctx->snap->editor_transformers, i);
    if (ct) {
        int sw = UI_COLOR_SWATCH_W;
        int sx = ctx->cp_x + ctx->cp_w - CODE_MARGIN_X - sw - 2;
        int sy = ctx->line_y + (LINE_H - sw) / 2 - 1;
        ui_color_picker_render_swatch(ct, sx, sy,
                                      ctx->snap->color_picker.active_line);
    }
}

/* Replay-only virtual rows (substitution + evaluation) emitted below the
 * source line at after_line_idx. Layout already reserved the rows; this
 * just paints them. */
static void code_panel_draw_virtual_lines(CodePanelRowCtx *ctx, int after_line_idx) {
    const EditorVirtualLineList *vlist = ctx->snap->editor_virtual_lines;
    if (!vlist)
        return;

    for (int vi = 0; vi < vlist->count; vi++) {
        const EditorVirtualLine *vl = &vlist->items[vi];
        if (vl->after_line_idx != after_line_idx)
            continue;
        if (code_panel_row_visible(ctx)) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (vl->style == VIRTUAL_STYLE_REPLAY_SUBST)
                glColor4f(0.10f, 0.25f, 0.15f, 0.35f);
            else
                glColor4f(0.15f, 0.15f, 0.25f, 0.35f);
            glRectf(0.0f, (float)(ctx->line_y - 3),
                    (float)ctx->panel_w, (float)(ctx->line_y - 3) + (float)LINE_H);
            glDisable(GL_BLEND);

            if (vl->style == VIRTUAL_STYLE_REPLAY_SUBST)
                glColor3f(0.50f, 0.75f, 0.50f);
            else
                glColor3f(0.50f, 0.60f, 0.80f);
            gl2d_draw_string((float)ctx->text_x, (float)ctx->line_y,
                             vl->text, FONT_MONO);
            if (vl->aux[0]) {
                int sw = (int)strlen(vl->text) * FONT_W;
                glColor3f(0.40f, 0.55f, 0.40f);
                gl2d_draw_string((float)(ctx->text_x + sw),
                                 (float)ctx->line_y, vl->aux, FONT_MONO);
            }
            ctx->line_y -= LINE_H;
        }
        ctx->cur++;
    }
}

/* Render an existing (non-edit) command row plus any virtual annotation
 * rows that follow it. Edit / insert rows are handled separately by
 * render_active_input_rows() in the orchestrator. */
static void code_panel_draw_command_row(CodePanelRowCtx *ctx, int i,
                                        int is_vertex, int vnum,
                                        int primitive_vnums_exact) {
    const GLCmd *document_cmds = ctx->snap->document_cmds;
    char display_text[MAX_INPUT_LEN];
    /* Search row index: shifted by one when an insert preview row sits
     * before this command. Pure function of snap fields. */
    int search_row_idx;
    if (i < 0 || i >= ctx->snap->document_count)
        search_row_idx = -1;
    else if (ctx->snap->editor_input.insert_mode && i >= ctx->snap->edit_line)
        search_row_idx = i + 1;
    else
        search_row_idx = i;
    /* Read the per-line display text from the same source layout
     * uses (controller-pushed override list) so wrap-row counts
     * never disagree with what render draws — even if the override
     * list capped out (every entry past the cap falls back to
     * buffer text consistently for both layout and render). */
    {
        const char *override_text = editor_state_line_override_for(i);
        const char *src;
        if (override_text)
            src = override_text;
        else {
            const char *buf_line = editor_buffer_line(i);
            src = buf_line ? buf_line : "";
        }
        snprintf(display_text, sizeof(display_text), "%s", src);
    }

    CodePanelWrapIter wrap_it;
    int wrap_row = 0;
    int wrap_start, wrap_len, wrap_x;
    repl_code_panel_document_wrap_iter_init(&wrap_it, display_text,
                                            ctx->text_x, ctx->panel_w);
    while (repl_code_panel_document_wrap_iter_next(&wrap_it,
                                                   &wrap_start, &wrap_len, &wrap_x)) {
        if (code_panel_row_visible(ctx)) {
            code_panel_draw_row_overlays(ctx, i);
            if (wrap_row == 0)
                code_panel_draw_row_gutter(ctx, i, is_vertex, vnum,
                                           primitive_vnums_exact);
            color_for_type(document_cmds[i].type);
            code_panel_draw_search_highlights(ctx->snap, display_text,
                                              search_row_idx,
                                              wrap_start, wrap_len,
                                              wrap_x, ctx->line_y);
            code_panel_draw_segment(wrap_x, ctx->line_y, display_text,
                                    wrap_start, wrap_len, FONT_MONO);
            ctx->line_y -= LINE_H;
        }
        ctx->cur++;
        wrap_row++;
    }
    ctx->file_line++;

    code_panel_draw_virtual_lines(ctx, i);
}

/* Trailing newline slot rendered after the last command. If the cursor is
 * past the document end this slot is the active edit row; otherwise it is
 * just a placeholder so the user can see "next line will go here". */
static void code_panel_draw_trailing_newline(CodePanelRowCtx *ctx,
                                             int document_count) {
    int edit_line = ctx->snap->edit_line;
    int is_edit_nl = (edit_line == document_count);
    if (is_edit_nl) {
        render_active_input_rows(ctx->snap, ctx->panel_w, ctx->text_x, ctx->idx_x,
                                 ctx->visible_lines, ctx->file_line,
                                 ctx->snap->active_indent_chars,
                                 NULL,
                                 edit_line,
                                 ctx->out,
                                 &ctx->cur, &ctx->line_y);
    } else if (code_panel_row_visible(ctx)) {
        code_panel_draw_gutter_lineno(ctx->line_y, ctx->file_line);
        glColor3f(0.28f, 0.28f, 0.35f);
        char ind_s[32];
        int  nc = ctx->snap->trailing_indent_chars;
        if (nc > 31) nc = 31;
        if (nc < 0)  nc = 0;
        memset(ind_s, ' ', (size_t)nc);
        ind_s[nc] = '\0';
        gl2d_draw_string((float)ctx->text_x, (float)ctx->line_y, ind_s, FONT_MONO);
        ctx->line_y -= LINE_H;
    }
    ctx->file_line++;
    ctx->cur++;
}

/* Vertical scrollbar thumb on the right edge. Drawn only when content
 * exceeds the visible window. */
static void code_panel_draw_scrollbar(int cp_x, int cp_w, int cp_h,
                                      int panel_top, int scroll,
                                      int total_lines, int visible_lines) {
    if (total_lines <= visible_lines)
        return;

    int   bar_h    = cp_h - CODE_MARGIN_Y - LINE_H - STATUSBAR_H;
    float frac     = (float)visible_lines / (float)total_lines;
    float pos      = (float)scroll       / (float)total_lines;
    int   thumb_h  = (int)(bar_h * frac);
    if (thumb_h < 12) thumb_h = 12;
    int   thumb_y  = panel_top - CODE_MARGIN_Y - LINE_H
                   - (int)(bar_h * pos) - thumb_h;

    glEnable(GL_BLEND);
    glColor4f(0.50f, 0.50f, 0.65f, 0.35f);
    glRectf((float)(cp_x + cp_w - 6), (float)thumb_y,
            (float)(cp_x + cp_w - 6) + 5.0f, (float)thumb_y + (float)thumb_h);
    glDisable(GL_BLEND);
}

/* Statusbar separator: a one-pixel vertical rule with 8px padding on each
 * side. Mutates *tx so callers can chain segments left-to-right. */
static void code_panel_statusbar_sep(int *tx, int sy, int sh) {
    *tx += 8;
    glColor4f(0.20f, 0.20f, 0.20f, 1.0f);
    glBegin(GL_LINES);
    glVertex2f((float)*tx, (float)(sy + 4));
    glVertex2f((float)*tx, (float)(sy + sh - 4));
    glEnd();
    *tx += 8;
}

/* Bottom status strip: cmd count, line/insert mode, AA indicator, and the
 * right-aligned F1 help affordance. The amber `g_status` message renders
 * separately below the scene panel because the statusbar is narrower. */
static void code_panel_draw_statusbar(const UiRenderSnapshot *snap,
                                      int cp_x, int cp_y, int cp_w,
                                      int edit_line, int insert_mode) {
    GlrRenderState rs = snap->render;
    int sy = cp_y;
    int sh = STATUSBAR_H;

    glPushAttrib(GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Background strip + top divider */
    glColor4f(0.094f, 0.094f, 0.094f, 0.98f);
    glRectf((float)cp_x, (float)sy, (float)cp_x + (float)cp_w, (float)sy + (float)sh);
    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_LINES);
    glVertex2f((float)cp_x,          (float)(sy + sh));
    glVertex2f((float)(cp_x + cp_w), (float)(sy + sh));
    glEnd();

    int text_y = sy + (sh - FONT_SMALL_H) / 2 + 1;
    int tx = cp_x + CODE_MARGIN_X;

    /* Cmd count (left-aligned, brighter color) */
    char cmds_buf[48];
    snprintf(cmds_buf, sizeof(cmds_buf), "%d/%d cmds",
             snap->flat_program_count, MAX_COMMANDS);
    glColor3f(0.878f, 0.878f, 0.878f);
    gl2d_draw_string((float)tx, (float)text_y, cmds_buf, FONT_SMALL);
    tx += (int)strlen(cmds_buf) * FONT_SMALL_W;

    code_panel_statusbar_sep(&tx, sy, sh);

    /* Cursor line / insert-mode badge / glBegin mode */
    char ln_buf[64];
    if (insert_mode)
        snprintf(ln_buf, sizeof(ln_buf), "Ln %d [INSERT]", edit_line + 1);
    else if (snap->in_begin_block)
        snprintf(ln_buf, sizeof(ln_buf), "Ln %d  %s",
                 edit_line + 1, mode_name(snap->current_begin_mode));
    else
        snprintf(ln_buf, sizeof(ln_buf), "Ln %d", edit_line + 1);
    glColor3f(0.627f, 0.627f, 0.627f);
    gl2d_draw_string((float)tx, (float)text_y, ln_buf, FONT_SMALL);
    tx += (int)strlen(ln_buf) * FONT_SMALL_W;

    /* AA indicator (only when the accumulation buffer is in play) */
    if (rs.use_accum) {
        code_panel_statusbar_sep(&tx, sy, sh);
        char aa_buf[24];
        if (rs.accum_aa_enabled && rs.accum_samples > 1)
            snprintf(aa_buf, sizeof(aa_buf), "AA %dx", rs.accum_samples);
        else
            snprintf(aa_buf, sizeof(aa_buf), "AA off");
        gl2d_draw_string((float)tx, (float)text_y, aa_buf, FONT_SMALL);
        tx += (int)strlen(aa_buf) * FONT_SMALL_W;
    }

    /* Right-aligned F1 help affordance: keycap chip + "help" label */
    {
        const char *help_kbd = "F1";
        const char *help_lbl = "help";
        int kbd_w = (int)strlen(help_kbd) * FONT_SMALL_W + 10;
        int lbl_w = (int)strlen(help_lbl) * FONT_SMALL_W;
        int rx = cp_x + cp_w - CODE_MARGIN_X - lbl_w;
        glColor3f(0.627f, 0.627f, 0.627f);
        gl2d_draw_string((float)rx, (float)text_y, help_lbl, FONT_SMALL);

        int kx = rx - kbd_w - 6;
        int ky = sy + 3;
        int kh = sh - 6;
        glColor4f(0.078f, 0.078f, 0.078f, 1.0f);
        glRectf((float)kx, (float)ky, (float)kx + (float)kbd_w, (float)ky + (float)kh);
        glColor4f(0.20f, 0.20f, 0.20f, 1.0f);
        glBegin(GL_LINE_LOOP);
        glVertex2f((float)kx,           (float)ky);
        glVertex2f((float)(kx + kbd_w), (float)ky);
        glVertex2f((float)(kx + kbd_w), (float)(ky + kh));
        glVertex2f((float)kx,           (float)(ky + kh));
        glEnd();
        glColor3f(0.733f, 0.733f, 0.733f);
        gl2d_draw_string((float)(kx + 5), (float)(ky + 2), help_kbd, FONT_SMALL);
    }
    glDisable(GL_BLEND);
    glPopAttrib();
}

void ui_panels_render_code_panel(const UiRenderSnapshot *snap,
                                 UiCodePanelOutput *out) {
    if (out) {
        out->cursor_px    = 0;
        out->cursor_py    = 0;
        out->cursor_valid = 0;
    }

    /* ------------------------------------------------------------------ */
    /* Phase 1: layout. Compute panel rectangle and bail early if empty;  */
    /* otherwise build the document layout (per-cmd row counts including  */
    /* virtual annotation rows) and apply scroll-follow if the cursor or  */
    /* replay PC just moved.                                              */
    /* ------------------------------------------------------------------ */
    prof_begin(PROF_CODE_PANEL_LAYOUT);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM_SETUP);

    int cp_x, cp_y, cp_w, cp_h;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0) {
        prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_SETUP);
        prof_end(PROF_CODE_PANEL_LAYOUT_GEOM);
        prof_end(PROF_CODE_PANEL_LAYOUT);
        return;
    }

    int panel_w   = cp_w;
    int panel_top = cp_y + cp_h;            /* OpenGL-y of panel top edge */
    int linenum_w = 4 * FONT_W;
    int idx_col_w = snap->code_panel.show_vertex_indices ? (6 * FONT_W) : 0;
    int idx_x     = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x    = idx_x + idx_col_w;

    const GLCmd *document_cmds = snap->document_cmds;
    int          document_count = snap->document_count;
    int          edit_line      = snap->edit_line;
    int          insert_mode    = snap->editor_input.insert_mode;

    /* Pull feeding-cmd kinds out of the highlight snapshot once so the
     * per-row branch in code_panel_draw_row_overlays() stays cheap. */
    int highlight_normal_idx = -1;
    int highlight_color_idx  = -1;
    if (snap->editor_highlights) {
        for (int hi = 0; hi < snap->editor_highlights->count; hi++) {
            const EditorHighlight *h = &snap->editor_highlights->items[hi];
            if (h->kind == HIGHLIGHT_FEEDING_NORMAL)
                highlight_normal_idx = h->line_idx;
            else if (h->kind == HIGHLIGHT_FEEDING_COLOR)
                highlight_color_idx = h->line_idx;
        }
    }

    /* Own the full-window 2D viewport for the chrome + UI stack. */
    glViewport(0, 0, snap->viewport.window_w, snap->viewport.window_h);

    prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_SETUP);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM_PRECOMPUTE);

    CodePanelDocumentLayout doc_layout;
    repl_code_panel_document_build(&doc_layout, panel_w, text_x, cp_h);
    int visible_lines = doc_layout.visible_lines;
    int total_lines   = doc_layout.total_lines;

    prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_PRECOMPUTE);
    prof_end(PROF_CODE_PANEL_LAYOUT_GEOM);
    prof_begin(PROF_CODE_PANEL_LAYOUT_SCROLL);

    /* Snap the scroll position to follow the cursor / replay PC, but only
     * if either just moved; manual scroll stays where the user left it. */
    repl_code_panel_document_apply_follow_scroll(&doc_layout);

    prof_end(PROF_CODE_PANEL_LAYOUT_SCROLL);
    prof_end(PROF_CODE_PANEL_LAYOUT);

    /* ------------------------------------------------------------------ */
    /* Phase 2: chrome (background, divider, menu bar, search overlay).    */
    /* ------------------------------------------------------------------ */
    prof_begin(PROF_CODE_PANEL_CHROME);

    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    code_panel_draw_chrome(snap, cp_x, cp_y, cp_w, cp_h, panel_top, panel_w);

    prof_end(PROF_CODE_PANEL_CHROME);

    /* ------------------------------------------------------------------ */
    /* Phase 3: row cursor. Code lines start one row below the menu bar.  */
    /* The CodePanelRowCtx threads cur / line_y / file_line through every */
    /* phase that emits rows.                                             */
    /* ------------------------------------------------------------------ */
    CodePanelRowCtx ctx = {
        .snap                 = snap,
        .panel_w              = panel_w,
        .text_x               = text_x,
        .idx_x                = idx_x,
        .cp_x                 = cp_x,
        .cp_w                 = cp_w,
        .scroll               = snap->scroll.scroll,
        .visible_lines        = visible_lines,
        .highlight_normal_idx = highlight_normal_idx,
        .highlight_color_idx  = highlight_color_idx,
        .out                  = out,
        .cur                  = 0,
        .line_y               = panel_top - CODE_MARGIN_Y - 2 * LINE_H,
        .file_line            = 1,
    };

    /* ------------------------------------------------------------------ */
    /* Phase 4: static header lines (workspace state + framing scaffolding) */
    /* ------------------------------------------------------------------ */
    prof_begin(PROF_CODE_PANEL_LINES);
    prof_begin(PROF_CODE_PANEL_LINES_STATIC);

    const ReplImportExportView *imex = &snap->import_export;

    /* Workspace state (saved variable values + config toggles, dimmed) */
    for (int i = 0; i < imex->workspace_header_line_count; i++)
        code_panel_draw_static_line(&ctx, imex->workspace_header_lines[i],
                                    0.45f, 0.55f, 0.42f);
    /* Header pre-lookAt scaffolding */
    for (int i = 0; g_header_pre[i]; i++)
        code_panel_draw_static_line(&ctx, g_header_pre[i],
                                    0.38f, 0.38f, 0.42f);
    /* Dynamic render-state lines (rebuilt every frame in the controller) */
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        code_panel_draw_static_line(&ctx, imex->render_state_lines[i],
                                    0.50f, 0.45f, 0.55f);
    /* Camera transform lines (also rebuilt every frame) */
    for (int i = 0; i < CAM_LINE_COUNT; i++)
        code_panel_draw_static_line(&ctx, imex->cam_lines[i],
                                    0.50f, 0.45f, 0.55f);
    /* Header post-camera scaffolding */
    for (int i = 0; g_header_post[i]; i++)
        code_panel_draw_static_line(&ctx, g_header_post[i],
                                    0.38f, 0.38f, 0.42f);

    prof_end(PROF_CODE_PANEL_LINES_STATIC);

    /* ------------------------------------------------------------------ */
    /* Phase 5: body. Walk the document, emitting one row per command (or */
    /* its wrap rows + virtual annotation rows). The active edit row and  */
    /* the insert-mode virtual row are drawn via render_active_input_rows.*/
    /* The vertex / loop / tess counters track primitive structure so the */
    /* gutter "v3 / vn" indicator is correct even inside loops.           */
    /* ------------------------------------------------------------------ */
    prof_begin(PROF_CODE_PANEL_LINES_BODY);
    prof_begin(PROF_CODE_PANEL_LINES_BODY_CMDS);

    int vnum = 0;
    int loop_depth = 0;
    int tess_depth = 0;
    int in_tess_poly = 0;
    int primitive_vnums_exact = 1;

    for (int i = 0; i < document_count; i++) {
        /* Insert-mode preview row appears *before* document_cmds[edit_line]. */
        if (insert_mode && i == edit_line) {
            render_active_input_rows(snap, panel_w, text_x, idx_x,
                                     visible_lines, ctx.file_line,
                                     repl_code_panel_document_active_indent_chars(),
                                     NULL,
                                     edit_line,
                                     out,
                                     &ctx.cur, &ctx.line_y);
            ctx.file_line++;
        }

        /* Reset vertex counter at the start of a primitive block. The
         * "exact" flag drops to 0 inside loops because we cannot know how
         * many iterations have already run at draw time. */
        if (document_cmds[i].valid) {
            if (document_cmds[i].type == CMD_BEGIN) {
                vnum = 0;
                primitive_vnums_exact = (loop_depth == 0);
            } else if (document_cmds[i].type == CMD_TESS_BEGIN_POLYGON) {
                vnum = 0;
                in_tess_poly = 1;
                tess_depth = 1;
                primitive_vnums_exact = (loop_depth == 0);
            }
        }

        int is_edit = (!insert_mode && i == edit_line);
        int is_vertex = document_cmds[i].valid &&
                        (document_cmds[i].type == CMD_VERTEX3F ||
                         document_cmds[i].type == CMD_VERTEX2F ||
                         document_cmds[i].type == CMD_TESS_VERTEX);

        if (is_edit) {
            /* Active edit row uses its own input-rendering path so the
             * cursor, autocomplete ghost, and parameter hint render
             * correctly with current keystroke state. */
            char idx_s[16];
            const char *idx_text = NULL;
            if (snap->code_panel.show_vertex_indices && is_vertex) {
                snprintf(idx_s, sizeof(idx_s),
                         primitive_vnums_exact ? "v%d" : "vn", vnum);
                idx_text = idx_s;
            }
            render_active_input_rows(snap, panel_w, text_x, idx_x,
                                     visible_lines, ctx.file_line,
                                     repl_code_panel_document_active_indent_chars(),
                                     idx_text,
                                     edit_line,
                                     out,
                                     &ctx.cur, &ctx.line_y);
            ctx.file_line++;
        } else {
            code_panel_draw_command_row(&ctx, i, is_vertex, vnum,
                                        primitive_vnums_exact);
        }

        /* Advance vertex / block counters AFTER rendering this row. */
        if (is_vertex) vnum++;
        if (document_cmds[i].valid) {
            switch (document_cmds[i].type) {
            case CMD_FOR_BEGIN:
                loop_depth++;
                primitive_vnums_exact = 0;
                break;
            case CMD_FOR_END:
                if (loop_depth > 0) loop_depth--;
                break;
            case CMD_END:
                primitive_vnums_exact = 1;
                break;
            case CMD_TESS_BEGIN_CONTOUR:
                if (in_tess_poly) tess_depth++;
                break;
            case CMD_TESS_END:
                if (in_tess_poly) {
                    if (tess_depth > 0) tess_depth--;
                    if (tess_depth == 0) {
                        in_tess_poly = 0;
                        primitive_vnums_exact = 1;
                    }
                }
                break;
            default:
                break;
            }
        }
    }

    prof_end(PROF_CODE_PANEL_LINES_BODY_CMDS);
    prof_begin(PROF_CODE_PANEL_LINES_BODY_NEWLINE);

    code_panel_draw_trailing_newline(&ctx, document_count);

    prof_end(PROF_CODE_PANEL_LINES_BODY_NEWLINE);
    prof_end(PROF_CODE_PANEL_LINES_BODY);

    /* ------------------------------------------------------------------ */
    /* Phase 6: footer (dimmed scaffolding wrapping the user's body)       */
    /* ------------------------------------------------------------------ */
    prof_begin(PROF_CODE_PANEL_LINES_FOOTER);

    for (int i = 0; g_footer_pre_init[i]; i++)
        code_panel_draw_static_line(&ctx, g_footer_pre_init[i],
                                    0.38f, 0.38f, 0.42f);
    for (int i = 0; i < init_section_line_count(); i++) {
        char line[MAX_LINE_LEN];
        init_section_line(i, line, sizeof(line));
        code_panel_draw_static_line(&ctx, line, 0.38f, 0.38f, 0.42f);
    }
    for (int i = 0; g_footer_post_init[i]; i++)
        code_panel_draw_static_line(&ctx, g_footer_post_init[i],
                                    0.38f, 0.38f, 0.42f);

    prof_end(PROF_CODE_PANEL_LINES_FOOTER);
    prof_end(PROF_CODE_PANEL_LINES);

    /* ------------------------------------------------------------------ */
    /* Phase 7: floating overlays - scrollbar, statusbar, color picker.   */
    /* ------------------------------------------------------------------ */
    prof_begin(PROF_CODE_PANEL_OVERLAYS);

    code_panel_draw_scrollbar(cp_x, cp_w, cp_h, panel_top,
                              snap->scroll.scroll, total_lines, visible_lines);
    code_panel_draw_statusbar(snap, cp_x, cp_y, cp_w, edit_line, insert_mode);
    ui_color_picker_render(&snap->color_picker,
                           snap->viewport.window_w,
                           snap->viewport.window_h);

    prof_end(PROF_CODE_PANEL_OVERLAYS);

    gl2d_end();
}

/* ========================================================================= */
/* Scene status banner                                                        */
/* ========================================================================= */

/* Amber status/error strip along the bottom of the scene panel.  The scene
 * is much wider than the code-panel statusbar slot, so long diagnostics
 * (~80 chars) fit here without truncation. */
void ui_panels_render_scene_status(const UiRenderSnapshot *snap) {
    ReplStatusState status = snap->status;
    if (status.ttl <= 0 || !status.text[0]) return;

    int sc_x, sc_y, sc_w, sc_h;
    ui_layout_scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    if (sc_w <= 0 || sc_h <= 0) return;

    int bar_h = STATUSBAR_H;
    int bar_y = sc_y;

    float alpha = status.ttl > 60 ? 1.0f : (float)status.ttl / 60.0f;

    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Design ref: amber error banner - bg #3a2a10, top rule #1a1208, text
     * #f0c070, "!" in bordered circle. */
    glColor4f(0.227f, 0.165f, 0.063f, 0.92f * alpha);
    glRectf((float)((float)sc_x), (float)((float)bar_y), (float)((float)sc_x)+(float)((float)sc_w), (float)((float)bar_y)+(float)(bar_h));
    glColor4f(0.102f, 0.071f, 0.031f, alpha);
    glBegin(GL_LINES);
    glVertex2f((float)sc_x,          (float)(bar_y + bar_h));
    glVertex2f((float)(sc_x + sc_w), (float)(bar_y + bar_h));
    glEnd();

    int text_y = bar_y + (bar_h - FONT_SMALL_H) / 2 + 1;

    /* Bordered "!" badge on the left */
    int badge_d = 14;
    int badge_x = sc_x + CODE_MARGIN_X;
    int badge_y = bar_y + (bar_h - badge_d) / 2;
    glColor4f(0.941f, 0.753f, 0.439f, alpha);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 16; i++) {
        float a = (float)i * (6.2831853f / 16.0f);
        glVertex2f(badge_x + badge_d * 0.5f + cosf(a) * (badge_d * 0.5f),
                   badge_y + badge_d * 0.5f + sinf(a) * (badge_d * 0.5f));
    }
    glEnd();
    gl2d_draw_string((float)(badge_x + badge_d * 0.5f - FONT_SMALL_W * 0.5f + 1.0f),
                (float)text_y, "!", FONT_SMALL);

    int tx = badge_x + badge_d + 8;
    int max_px = sc_x + sc_w - CODE_MARGIN_X - tx;
    int max_chars = max_px / FONT_SMALL_W;
    if (max_chars < 8) max_chars = 8;
    if (max_chars > 255) max_chars = 255;

    char msg[256];
    int n = (int)strlen(status.text);
    if (n > max_chars) {
        snprintf(msg, sizeof(msg), "%.*s...", max_chars - 3, status.text);
    } else {
        snprintf(msg, sizeof(msg), "%s", status.text);
    }
    glColor4f(0.941f, 0.753f, 0.439f, alpha); /* #f0c070 */
    gl2d_draw_string((float)tx, (float)text_y, msg, FONT_SMALL);

    glDisable(GL_BLEND);
    gl2d_end();
}


/* ========================================================================= */
/* Configuration menu - now hosted inside the MENU_CONFIG dropdown            */
/* ========================================================================= */

int ui_panels_handle_right_press(int mx, int my) {
    return ui_menu_bar_handle_config_right_press(mx, my);
}

/* Divider geometry for the code-panel ↔ scene splitter. Mirrors the
 * legacy editor_input_point_on_code_panel_divider so the hit-test
 * stays self-contained — no UI input file needs to know about
 * editor_input.c's predicate. */
static int ui_panels_point_on_panel_divider(int mx, int gl_y,
                                            int cp_x, int cp_y,
                                            int cp_w, int cp_h) {
    int layout = ui_state_code_panel().layout_mode;
    if (layout < 0 || layout >= CODE_PANEL_LAYOUT_COUNT)
        layout = CODE_PANEL_LAYOUT_LEFT;
    if (layout == CODE_PANEL_LAYOUT_HIDDEN)
        return 0;
    if (cp_w <= 0 || cp_h <= 0)
        return 0;
    if (layout == CODE_PANEL_LAYOUT_TOP)
        return abs(gl_y - cp_y) < 10;
    if (layout == CODE_PANEL_LAYOUT_BOTTOM)
        return abs(gl_y - (cp_y + cp_h)) < 10;
    return abs(mx - (cp_x + cp_w)) < 10;
}

/* Translate a code-panel click on a wrapped row into a position in
 * the input buffer. Mirrors ui_panels_handle_code_panel_click's column
 * derivation so the controller doesn't have to reimplement wrap /
 * indent / segment math. */
static int ui_panels_input_cursor_for_click(int mx, int row_offset,
                                            int cp_w) {
    int linenum_w = 4 * FONT_W;
    int idx_col_w = ui_state_code_panel().show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int indent_chars = repl_code_panel_document_active_indent_chars();
    int seg_start = editor_state_input().input_len;
    int seg_len = 0;
    int seg_x = text_x + indent_chars * FONT_W;

    repl_code_panel_document_segment_for_row(editor_state_input().input,
                                             seg_x, cp_w, row_offset,
                                             &seg_start, &seg_len, &seg_x);

    int col = (mx - seg_x + FONT_W / 2) / FONT_W;
    if (col < 0) col = 0;
    if (col > seg_len) col = seg_len;
    int new_cursor = seg_start + col;
    int input_len = editor_state_input().input_len;
    if (new_cursor > input_len) new_cursor = input_len;
    return new_cursor;
}

/* Pure hit-test: classify (mx, my) as a UiHit. Reads UI layout state and
 * caller-supplied counts; never mutates. The controller dispatches on
 * UiHit.kind and uses the per-kind payload (line_idx, char_idx,
 * cmd_idx, item_idx) without consulting any UI module's state. */
UiHit ui_panels_hit_test(int mx, int my, int variable_count) {
    UiHit h = ui_hit_none();

    int win_w = ui_state_viewport().window_w;
    int win_h = ui_state_viewport().window_h;
    if (win_w <= 0 || win_h <= 0)
        return h;

    int gl_y = win_h - my;

    /* Help overlay takes priority when visible — hit goes there
     * regardless of where in the window the pointer landed. The
     * controller then routes to the help session for scroll/search. */
    if (ui_state_help().visible) {
        h.kind = UI_HIT_HELP_PANEL;
        h.local_x = (float)mx;
        h.local_y = (float)gl_y;
        return h;
    }

    /* Floating overlays beat the underlying code panel / scene. The
     * color picker, when open, is the most modal of these — it captures
     * input while the user is dragging a slider. */
    {
        ColorPickerView pv = color_picker_view();
        UiHit picker = ui_color_picker_hit_test(&pv, mx, my, win_h);
        if (picker.kind != UI_HIT_NONE)
            return picker;
    }

    /* Menu-bar regions: open dropdown row, top-level menu button,
     * pin button. */
    UiHit menu = ui_menu_bar_hit_test(mx, my);
    if (menu.kind != UI_HIT_NONE)
        return menu;

    /* Variable panel is a non-modal floating panel; only matters when
     * its slider rows are visible. */
    UiHit varp = ui_variable_panel_hit_test(mx, my, variable_count);
    if (varp.kind != UI_HIT_NONE)
        return varp;

    /* Code-panel region. */
    int cp_x, cp_y, cp_w, cp_h;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);

    /* Panel divider drag handle has priority over the code-panel
     * interior so dragging the divider doesn't accidentally hit a
     * code-text row at the panel edge. */
    if (ui_panels_point_on_panel_divider(mx, gl_y, cp_x, cp_y, cp_w, cp_h)) {
        h.kind = UI_HIT_PANEL_DIVIDER;
        h.local_x = (float)mx;
        h.local_y = (float)gl_y;
        return h;
    }

    if (cp_w > 0 && cp_h > 0 &&
        mx >= cp_x && mx < cp_x + cp_w &&
        gl_y >= cp_y && gl_y < cp_y + cp_h) {
        /* Within the code panel. Distinguish gutter (line numbers)
         * from text area. Layout: CODE_MARGIN_X | linenum (4*FONT_W)
         * | gap (FONT_W) | optional idx col | text. */
        int linenum_w = 4 * FONT_W;
        int gutter_right = cp_x + CODE_MARGIN_X + linenum_w;
        int in_gutter = (mx < gutter_right);

        /* Best-effort line / row mapping. Mirrors code_panel_hit_test
         * but failure-tolerant — we always return a hit kind even
         * when the row mapping doesn't resolve a target. */
        int target = -1;
        int on_insert_line = 0;
        int row_offset = 0;
        int panel_top = cp_y + cp_h;
        int line_y_start = panel_top - CODE_MARGIN_Y - 2 * LINE_H;
        int vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;
        int row_resolved = 0;
        if (vis >= 0 &&
            vis < repl_code_panel_document_visible_lines_for_height(cp_h)) {
            int idx_col_w = ui_state_code_panel().show_vertex_indices ? (6 * FONT_W) : 0;
            int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
            int doc_line = editor_scroll() + vis;
            CodePanelDocumentLayout layout;
            repl_code_panel_document_build(&layout, cp_w, text_x, cp_h);
            if (repl_code_panel_document_target_for_doc_line(doc_line, &layout,
                                                             &target,
                                                             &on_insert_line,
                                                             &row_offset)) {
                row_resolved = 1;
            }
        }
        h.local_x = (float)(mx - cp_x);
        h.local_y = (float)(gl_y - cp_y);

        /* Inline color swatch on the right edge of a committed
         * source row (row_offset == 0, not the insert line). The
         * controller opens / toggles / closes the picker on this hit
         * kind, distinct from UI_HIT_COLOR_SWATCH which represents
         * the floating picker's slider controls. */
        if (!in_gutter && row_resolved && !on_insert_line && row_offset == 0 &&
            target >= 0 &&
            find_color_transformer(editor_state_transformers(), target)) {
            int sx = cp_x + cp_w - CODE_MARGIN_X - UI_COLOR_SWATCH_W - 2;
            if (mx >= sx && mx < sx + UI_COLOR_SWATCH_W) {
                h.kind = UI_HIT_INLINE_COLOR_SWATCH;
                h.line_idx = target;
                h.cmd_idx = target;
                h.visual_row = row_offset;
                return h;
            }
        }

        if (in_gutter) {
            h.kind = UI_HIT_CODE_GUTTER;
            if (row_resolved) {
                h.line_idx = target;
                h.visual_row = row_offset;
            }
            return h;
        }

        /* Code-text row: split insert-line vs committed-line. The
         * insert line is the row past the last commit; clicks there
         * set the cursor without navigating. The line_idx surfaced
         * for UI_HIT_CODE_INSERT_LINE is the current edit_line so
         * the controller's drag-anchor bookkeeping has a stable
         * reference even when the insert row is virtual. */
        if (row_resolved) {
            h.kind = on_insert_line ? UI_HIT_CODE_INSERT_LINE : UI_HIT_CODE_TEXT;
            h.line_idx = on_insert_line ? editor_state_input().edit_line_idx : target;
            h.visual_row = row_offset;
            h.char_idx = ui_panels_input_cursor_for_click(mx, row_offset, cp_w);
        } else {
            h.kind = UI_HIT_CODE_TEXT;
        }
        return h;
    }

    /* Outside the code panel: assume scene/viewport region. The
     * scene controller is the owner; the controller routes
     * UI_HIT_SCENE to the camera/viewport handler. */
    h.kind = UI_HIT_SCENE;
    h.local_x = (float)mx;
    h.local_y = (float)gl_y;
    return h;
}

/* J2.3: ui_panels_close_menus / ui_panels_open_config were thin
 * wrappers around ui_menu_bar / ui_color_picker. They were inlined
 * at their callers (editor_input.c, repl_actions.c, imrepl_ctrl.c)
 * so ui_panels.c can be hit-test-only without the
 * check-ui-panels-no-mutators guard needing an allowlist exception
 * for the wrapper bodies.
 *
 * J2.2: ui_panels_handle_code_panel_press / _click / _drag /
 * _release / _scene_press / _motion / _mouse_release / _escape were
 * deleted. The controller dispatches code-panel clicks via
 * glr_ctrl_router_handle_code_panel_hit on the UiHit returned by
 * ui_panels_hit_test; drag tracking + release / picker press / motion
 * / release / escape live on the controller side. UI input files
 * report hit-test data only. */
