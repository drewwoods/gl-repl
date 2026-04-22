/*
 * ui_panels.c — Code-panel row rendering, autocomplete/help/variable panels,
 *               panel input routing, and inline scene rename UI.
 *
 * Extracted from sample.c for maintainability.
 */
#include "sample.h"
#include "repl_actions.h"
#include "repl_color_picker.h"
#include "repl_code_panel_document.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_clipboard.h"
#include "repl_keys.h"
#include "repl_menu_bar.h"
#include "repl_replay_annotations.h"
#include "profile_panel.h"
#include "ui_panels.h"

/* Compile-time stringify for embedding macro values in string literals */
#define _HELP_STR2(x) #x
#define _HELP_STR(x)  _HELP_STR2(x)

/* Status footer height — design: 22px strip flush against code panel bottom */
/* STATUSBAR_H lives in sample.h now so scene_render.c can lift the
 * replay HUD above the amber status strip.  */

/* ========================================================================= */
/* Layout geometry helpers                                                    */
/* ========================================================================= */

static int code_panel_layout_mode(void) {
    if (g_code_panel_layout < 0 || g_code_panel_layout >= CODE_PANEL_LAYOUT_COUNT)
        return CODE_PANEL_LAYOUT_LEFT;
    return g_code_panel_layout;
}

static int panel_span_px(int total_px) {
    int span = (int)((float)total_px * g_panel_frac);
    if (span < 1) span = 1;
    if (span > total_px) span = total_px;
    return span;
}

void code_panel_rect(int *x, int *y, int *w, int *h) {
    int layout = code_panel_layout_mode();

    if (layout == CODE_PANEL_LAYOUT_HIDDEN) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
    } else if (layout == CODE_PANEL_LAYOUT_TOP) {
        int panel_h = panel_span_px(g_win_h);
        if (x) *x = 0;
        if (y) *y = g_win_h - panel_h;
        if (w) *w = g_win_w;
        if (h) *h = panel_h;
    } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
        int panel_h = panel_span_px(g_win_h);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = g_win_w;
        if (h) *h = panel_h;
    } else {
        int panel_w = panel_span_px(g_win_w);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = panel_w;
        if (h) *h = g_win_h;
    }
}

void scene_rect(int *x, int *y, int *w, int *h) {
    int layout = code_panel_layout_mode();

    if (layout == CODE_PANEL_LAYOUT_HIDDEN) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = g_win_w;
        if (h) *h = g_win_h;
    } else if (layout == CODE_PANEL_LAYOUT_TOP) {
        int panel_h = panel_span_px(g_win_h);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = g_win_w;
        if (h) *h = g_win_h - panel_h;
    } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
        int panel_h = panel_span_px(g_win_h);
        if (x) *x = 0;
        if (y) *y = panel_h;
        if (w) *w = g_win_w;
        if (h) *h = g_win_h - panel_h;
    } else {
        int panel_w = panel_span_px(g_win_w);
        if (x) *x = panel_w;
        if (y) *y = 0;
        if (w) *w = g_win_w - panel_w;
        if (h) *h = g_win_h;
    }
}

/* ========================================================================= */
/* Syntax color helpers                                                       */
/* ========================================================================= */

static void color_for_type(CmdType t) {
    switch (t) {
    case CMD_BEGIN:
    case CMD_END:      glColor3f(0.85f, 0.45f, 0.85f); break;
    case CMD_VERTEX3F:
    case CMD_VERTEX2F: glColor3f(0.40f, 0.90f, 0.40f); break;
    case CMD_NORMAL3F:      glColor3f(0.40f, 0.80f, 0.95f); break;
    case CMD_TRANSLATE3F:
    case CMD_SCALEF:
    case CMD_ROTATEF:
    case CMD_PUSH_MATRIX:
    case CMD_POP_MATRIX:    glColor3f(0.95f, 0.65f, 0.40f); break; /* orange */
    case CMD_COLOR_MATERIAL:
    case CMD_MATERIALF:     glColor3f(0.95f, 0.85f, 0.30f); break; /* yellow */
    case CMD_COLOR3F:
    case CMD_COLOR4F:
    case CMD_CLEAR_COLOR: glColor3f(0.95f, 0.85f, 0.30f); break; /* yellow */
    case CMD_ENABLE:
    case CMD_DISABLE:
    case CMD_SHADE_MODEL:
    case CMD_LIGHT_MODEL_I:
    case CMD_FRONT_FACE:
    case CMD_POINT_SIZE:
    case CMD_POINT_PARAMETER_FV:
    case CMD_BLEND_FUNC:
    case CMD_DEPTH_MASK: glColor3f(0.80f, 0.70f, 0.95f); break; /* lavender */
    case CMD_FOR_BEGIN:
    case CMD_FOR_END:  glColor3f(0.95f, 0.60f, 0.30f); break;
    case CMD_FUNC_DEF:
    case CMD_FUNC_END: glColor3f(0.60f, 0.85f, 0.95f); break;
    case CMD_CALL:     glColor3f(0.60f, 0.85f, 0.95f); break;
    case CMD_IF_BEGIN:
    case CMD_IF_END:   glColor3f(0.95f, 0.75f, 0.50f); break;
    case CMD_COMMENT:    glColor3f(0.45f, 0.50f, 0.45f); break;
    case CMD_VAR_ASSIGN:
    case CMD_VAR_DECLARE: glColor3f(0.55f, 0.80f, 0.95f); break;
    case CMD_LABEL:
    case CMD_GOTO:       glColor3f(0.85f, 0.55f, 0.85f); break;
    case CMD_GLU_SPHERE:
    case CMD_GLU_CYLINDER:
    case CMD_GLU_DISK:
    case CMD_GLU_PARTIAL_DISK:
    case CMD_GLUT_TORUS:  glColor3f(0.50f, 0.90f, 0.70f); break;
    case CMD_TESS_BEGIN_POLYGON:
    case CMD_TESS_BEGIN_CONTOUR:
    case CMD_TESS_END:    glColor3f(0.70f, 0.55f, 0.90f); break; /* violet */
    case CMD_TESS_NORMAL: glColor3f(0.40f, 0.80f, 0.95f); break; /* cyan */
    case CMD_TESS_COLOR:  glColor3f(0.95f, 0.85f, 0.30f); break; /* yellow */
    case CMD_TESS_VERTEX: glColor3f(0.40f, 0.90f, 0.40f); break; /* green */
    default:             glColor3f(0.70f, 0.70f, 0.70f); break;
    }
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
    draw_string((float)x, (float)y, buf, font);
}

/* Menu bar lives in repl_menu_bar.c. */

/* Inline scene rename state. Menu actions enter rename through
 * ui_panels_begin_rename(); the menu/dropdown state itself lives in
 * repl_menu_bar.c. */
static int  g_rename_slot = -1;
static char g_rename_buf[USER_SCENE_NAME_MAX];
static int  g_rename_len  = 0;

static void rename_refresh_status(void) {
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Rename scene (Enter to save, Esc to cancel): %s", g_rename_buf);
    set_status(msg);
}
static void code_panel_draw_search_highlights(const char *text, int search_row_idx,
                                              int seg_start, int seg_len,
                                              int seg_x, int y) {
    int drew = 0;

    if (!g_search_active || g_search_query_len <= 0 || search_row_idx < 0 ||
        !text || seg_len <= 0)
        return;

    for (int pos = repl_search_find_next_in_text(text, g_search_query, 0);
         pos >= 0;
         pos = repl_search_find_next_in_text(text, g_search_query, pos + 1)) {
        int match_end = pos + g_search_query_len;
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

        if (search_row_idx == g_search_hit_line && pos == g_search_hit_char)
            glColor4f(0.95f, 0.65f, 0.18f, 0.55f);
        else
            glColor4f(0.25f, 0.45f, 0.85f, 0.30f);

        draw_quad((float)(seg_x + (draw_start - seg_start) * FONT_W),
                  (float)(y - 2),
                  (float)((draw_end - draw_start) * FONT_W),
                  (float)(FONT_H + 4));
    }

    if (drew)
        glDisable(GL_BLEND);
}

static void render_active_input_rows(int panel_w, int text_x, int idx_x,
                                     int visible_lines, int file_line,
                                     int indent_chars, const char *idx_text,
                                     int search_row_idx,
                                     int *io_cur, int *io_line_y) {
    CodePanelWrapIter wrap_it;
    int wrap_row = 0;
    int wrap_start, wrap_len, wrap_x;
    int input_x = text_x + indent_chars * FONT_W;
    int cursor_seg_start = 0;
    int cursor_seg_len = 0;
    int cursor_seg_x = input_x;
    int cursor_row = repl_code_panel_document_cursor_row_for_text(g_input, input_x, panel_w,
                                                    g_cursor_pos,
                                                    &cursor_seg_start,
                                                    &cursor_seg_len,
                                                    &cursor_seg_x);
    int cursor_col = g_cursor_pos - cursor_seg_start;

    repl_code_panel_document_wrap_iter_init(&wrap_it, g_input, input_x, panel_w);
    while (repl_code_panel_document_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
        if (*io_cur >= g_scroll && *io_cur < g_scroll + visible_lines) {
            glColor3f(0.55f, 0.55f, 0.30f);
            if (wrap_row == 0) {
                char ln[16];
                snprintf(ln, sizeof(ln), "%3d", file_line);
                draw_string((float)CODE_MARGIN_X, (float)(*io_line_y), ln, FONT_MONO);
                if (idx_text) {
                    glColor3f(0.45f, 0.50f, 0.65f);
                    draw_string((float)idx_x, (float)(*io_line_y), idx_text, FONT_MONO);
                }
            }

            glEnable(GL_BLEND);
            glColor4f(0.15f, 0.18f, 0.28f, 0.70f);
            draw_quad(0, (float)(*io_line_y - 3), (float)panel_w, (float)LINE_H);
            glDisable(GL_BLEND);

            if (wrap_row == 0 && indent_chars > 0) {
                char spaces[32];
                int draw_indent = indent_chars;
                if (draw_indent > (int)sizeof(spaces) - 1)
                    draw_indent = (int)sizeof(spaces) - 1;
                memset(spaces, ' ', (size_t)draw_indent);
                spaces[draw_indent] = '\0';
                glColor3f(0.30f, 0.30f, 0.38f);
                draw_string((float)text_x, (float)(*io_line_y), spaces, FONT_MONO);
            }

            glColor3f(0.95f, 0.95f, 0.90f);
            code_panel_draw_search_highlights(g_input, search_row_idx,
                                              wrap_start, wrap_len,
                                              wrap_x, *io_line_y);
            code_panel_draw_segment(wrap_x, *io_line_y, g_input,
                                    wrap_start, wrap_len, FONT_MONO);

            if (wrap_row == cursor_row) {
                int cursor_x = wrap_x + cursor_col * FONT_W;
                int hint_x = cursor_x;

                if (g_ac_ghost[0] && g_cursor_pos == g_input_len) {
                    glEnable(GL_BLEND);
                    glColor4f(0.50f, 0.55f, 0.65f, 0.55f);
                    draw_string((float)cursor_x, (float)(*io_line_y),
                                g_ac_ghost, FONT_MONO);
                    glDisable(GL_BLEND);
                    hint_x += (int)strlen(g_ac_ghost) * FONT_W;
                }

                if (g_ac_hint[0] && g_cursor_pos == g_input_len) {
                    glEnable(GL_BLEND);
                    glColor4f(0.56f, 0.62f, 0.72f, 0.38f);
                    draw_string((float)hint_x, (float)(*io_line_y),
                                g_ac_hint, FONT_MONO);
                    glDisable(GL_BLEND);
                }

                if (g_cursor_on && !g_search_active) {
                    glEnable(GL_BLEND);
                    glColor4f(0.90f, 0.80f, 0.25f, 0.85f);
                    draw_quad((float)cursor_x, (float)(*io_line_y - 2),
                              2.0f, (float)(FONT_H + 2));
                    glDisable(GL_BLEND);
                }

                g_cursor_px = cursor_x;
                g_cursor_py = *io_line_y;
            }

            *io_line_y -= LINE_H;
        }

        (*io_cur)++;
        wrap_row++;
    }
}

int code_panel_apply_scroll_follow_for_test(int *out_follow_doc_line,
                                            int *out_visible_lines) {
    CodePanelDocumentLayout layout;
    int cp_x, cp_y, cp_w, cp_h;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;

    refresh_workspace_header_lines();
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    (void)cp_x;
    (void)cp_y;

    repl_code_panel_document_build(&layout, cp_w, text_x, cp_h);
    repl_code_panel_document_apply_follow_scroll(&layout);

    if (out_follow_doc_line)
        *out_follow_doc_line = layout.follow_doc_line;
    if (out_visible_lines)
        *out_visible_lines = layout.visible_lines;
    return layout.follow_doc_line >= g_scroll &&
           layout.follow_doc_line < g_scroll + layout.visible_lines;
}

/* Color picker lives in repl_color_picker.c. */

void render_code_panel(void) {
    prof_begin(PROF_CODE_PANEL_LAYOUT);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM_SETUP);

    CodePanelDocumentLayout doc_layout;
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0) {
        prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_SETUP);
        prof_end(PROF_CODE_PANEL_LAYOUT_GEOM);
        prof_end(PROF_CODE_PANEL_LAYOUT);
        return;
    }
    refresh_workspace_header_lines();
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;  /* y of the panel's top edge (OpenGL coords) */
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;
    int visible_lines;
    int total_lines;

    /* When cursor is on a vertex, find which normal/color lines feed it so
     * we can draw a gutter accent bar on them below. */
    int highlight_normal_idx = -1;
    int highlight_color_idx  = -1;
    if (!g_inserting && g_edit_line < g_num_cmds && g_cmds[g_edit_line].valid) {
        highlight_normal_idx = repl_find_feeding_normal_cmd(g_edit_line);
        highlight_color_idx  = repl_find_feeding_color_cmd(g_edit_line);
    }

    prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_SETUP);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM_PRECOMPUTE);

    repl_code_panel_document_build(&doc_layout, panel_w, text_x, cp_h);
    visible_lines = doc_layout.visible_lines;
    prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_PRECOMPUTE);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM_TOTALS);

    total_lines = doc_layout.total_lines;

    prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_TOTALS);

    prof_end(PROF_CODE_PANEL_LAYOUT_GEOM);
    prof_begin(PROF_CODE_PANEL_LAYOUT_CURSOR);

    (void)doc_layout.cursor_doc_line;

    prof_end(PROF_CODE_PANEL_LAYOUT_CURSOR);
    prof_begin(PROF_CODE_PANEL_LAYOUT_SCROLL);

    /* Only snap to cursor/replay after an edit or replay step; manual scroll
     * can stay off-target. */
    repl_code_panel_document_apply_follow_scroll(&doc_layout);

    prof_end(PROF_CODE_PANEL_LAYOUT_SCROLL);

    prof_end(PROF_CODE_PANEL_LAYOUT);
    prof_begin(PROF_CODE_PANEL_CHROME);

    begin_2d();

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.06f, 0.06f, 0.10f, 0.92f);
    draw_quad((float)cp_x, (float)cp_y, (float)cp_w, (float)cp_h);

    /* Border: divider between code panel and scene */
    glColor4f(0.30f, 0.30f, 0.50f, 0.80f);
    glBegin(GL_LINES);
    if (code_panel_layout_mode() == CODE_PANEL_LAYOUT_TOP) {
        /* Horizontal divider along bottom of code panel. */
        glVertex2f(0.0f, (float)cp_y);
        glVertex2f((float)g_win_w, (float)cp_y);
    } else if (code_panel_layout_mode() == CODE_PANEL_LAYOUT_BOTTOM) {
        /* Horizontal divider along top of code panel. */
        glVertex2f(0.0f, (float)(cp_y + cp_h));
        glVertex2f((float)g_win_w, (float)(cp_y + cp_h));
    } else {
        /* Vertical divider along right edge of code panel */
        glVertex2f((float)(cp_x + cp_w), 0.0f);
        glVertex2f((float)(cp_x + cp_w), (float)g_win_h);
    }
    glEnd();
    glDisable(GL_BLEND);

    repl_menu_bar_render();
    repl_menu_bar_render_search_overlay(cp_x, panel_w, panel_top);

    prof_end(PROF_CODE_PANEL_CHROME);
    prof_begin(PROF_CODE_PANEL_LINES);
    prof_begin(PROF_CODE_PANEL_LINES_STATIC);

    /* Code lines begin immediately below the menu bar (row 0). */
    int line_y = panel_top - CODE_MARGIN_Y - 2 * LINE_H;
    int cur = 0;
    int file_line = 1;

    /* Macro for rendering a static line (header/footer) */
    #define RENDER_STATIC_LINE(text, set_color) do {                           \
        CodePanelWrapIter wrap_it;                                              \
        int wrap_row = 0;                                                       \
        int wrap_start, wrap_len, wrap_x;                                       \
        repl_code_panel_document_wrap_iter_init(&wrap_it, text, text_x, panel_w);                   \
        while (repl_code_panel_document_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {\
            if (cur >= g_scroll && cur < g_scroll + visible_lines) {            \
                if (wrap_row == 0) {                                             \
                    glColor3f(0.30f, 0.30f, 0.38f);                            \
                    { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);  \
                      draw_string((float)CODE_MARGIN_X, (float)line_y,          \
                                  ln, FONT_MONO); }                             \
                }                                                               \
                set_color;                                                       \
                code_panel_draw_segment(wrap_x, line_y, text,                   \
                                        wrap_start, wrap_len, FONT_MONO);       \
                line_y -= LINE_H;                                                \
            }                                                                    \
            cur++;                                                               \
            wrap_row++;                                                          \
        }                                                                        \
        file_line++;                                                            \
    } while (0)

    /* Workspace state (saved var values + config toggles) */
    for (int i = 0; i < g_workspace_header_line_count; i++) {
        RENDER_STATIC_LINE(g_workspace_header_lines[i], glColor3f(0.45f, 0.55f, 0.42f));
    }
    /* Header pre-lookAt (dimmed) */
    for (int i = 0; g_header_pre[i]; i++) {
        RENDER_STATIC_LINE(g_header_pre[i], glColor3f(0.38f, 0.38f, 0.42f));
    }
    /* Dynamic render-state lines */
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++) {
        RENDER_STATIC_LINE(g_render_state_lines[i], glColor3f(0.50f, 0.45f, 0.55f));
    }
    /* Camera transform lines (dynamic — updated every frame) */
    for (int i = 0; i < CAM_LINE_COUNT; i++) {
        RENDER_STATIC_LINE(g_cam_lines[i], glColor3f(0.50f, 0.45f, 0.55f));
    }
    /* Header post-camera */
    for (int i = 0; g_header_post[i]; i++) {
        RENDER_STATIC_LINE(g_header_post[i], glColor3f(0.38f, 0.38f, 0.42f));
    }

    prof_end(PROF_CODE_PANEL_LINES_STATIC);
    prof_begin(PROF_CODE_PANEL_LINES_BODY);
    prof_begin(PROF_CODE_PANEL_LINES_BODY_CMDS);

    /* Commands + insert line + new-line slot */
    int vnum = 0; /* vertex counter within current glBegin/glEnd block */
    int loop_depth = 0;
    int in_tess_poly = 0;
    int tess_depth = 0;
    int primitive_vnums_exact = 1;
    for (int i = 0; i < g_num_cmds; i++) {
        /* If inserting, render the virtual insert line before command[g_edit_line] */
        if (g_inserting && i == g_edit_line) {
                        render_active_input_rows(panel_w, text_x, idx_x,
                                                                         visible_lines, file_line,
                                                                         repl_code_panel_document_active_indent_chars(), NULL,
                                                                         g_edit_line,
                                                                         &cur, &line_y);
            file_line++;
        }

        if (i < g_num_cmds) {
            /* Track vertex number for all commands regardless of visibility */
            if (g_cmds[i].valid) {
                if (g_cmds[i].type == CMD_BEGIN) {
                    vnum = 0;
                    primitive_vnums_exact = (loop_depth == 0);
                } else if (g_cmds[i].type == CMD_TESS_BEGIN_POLYGON) {
                    vnum = 0;
                    in_tess_poly = 1;
                    tess_depth = 1;
                    primitive_vnums_exact = (loop_depth == 0);
                }
            }

            int is_edit = (!g_inserting && i == g_edit_line);
            int is_vertex = g_cmds[i].valid && (g_cmds[i].type == CMD_VERTEX3F ||
                                                g_cmds[i].type == CMD_TESS_VERTEX);
            if (is_edit) {
                /* Active editing line */
                char idx_s[16];
                const char *idx_text = NULL;
                if (g_show_indices && is_vertex) {
                    snprintf(idx_s, sizeof(idx_s),
                             primitive_vnums_exact ? "v%d" : "vn", vnum);
                    idx_text = idx_s;
                }
                render_active_input_rows(panel_w, text_x, idx_x,
                                         visible_lines, file_line,
                                         repl_code_panel_document_active_indent_chars(), idx_text,
                                         g_edit_line,
                                         &cur, &line_y);
                file_line++;
            } else {
                /* Existing command, not being edited */
                CodePanelWrapIter wrap_it;
                char display_text[MAX_INPUT_LEN];
                int wrap_row = 0;
                int wrap_start, wrap_len, wrap_x;
                int search_row_idx = repl_search_row_for_cmd_index(i);
                code_panel_get_command_display_text(i, display_text,
                                                    sizeof(display_text));
                repl_code_panel_document_wrap_iter_init(&wrap_it, display_text, text_x, panel_w);
                while (repl_code_panel_document_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
                    if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                        if (g_replay_active &&
                            g_replay_src_line >= 0 && i == g_replay_src_line) {
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            glColor4f(0.10f, 0.35f, 0.15f, 0.55f);
                            draw_quad(0, (float)(line_y - 3),
                                      (float)panel_w, (float)LINE_H);
                            glColor4f(0.20f, 0.90f, 0.30f, 0.85f);
                            draw_quad(1.0f, (float)(line_y - 3), 3.0f, (float)LINE_H);
                            glDisable(GL_BLEND);
                        }
                        if (sel_active() && i >= sel_lo() && i <= sel_hi()) {
                            glEnable(GL_BLEND);
                            glColor4f(0.20f, 0.30f, 0.50f, 0.55f);
                            draw_quad(0, (float)(line_y - 3),
                                      (float)panel_w, (float)LINE_H);
                            glDisable(GL_BLEND);
                        }
                        if (i == highlight_normal_idx || i == highlight_color_idx) {
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            if (i == highlight_normal_idx)
                                glColor4f(0.40f, 0.80f, 0.95f, 0.85f);
                            else
                                glColor4f(0.95f, 0.85f, 0.30f, 0.85f);
                            draw_quad(1.0f, (float)(line_y - 3), 3.0f, (float)LINE_H);
                            glDisable(GL_BLEND);
                        }
                        if (wrap_row == 0) {
                            glColor3f(0.30f, 0.30f, 0.38f);
                            { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                              draw_string((float)CODE_MARGIN_X, (float)line_y,
                                          ln, FONT_MONO); }
                            if (g_show_indices && is_vertex) {
                                char idx_s[16];
                                snprintf(idx_s, sizeof(idx_s),
                                         primitive_vnums_exact ? "v%d" : "vn", vnum);
                                glColor3f(0.45f, 0.50f, 0.65f);
                                draw_string((float)idx_x, (float)line_y,
                                            idx_s, FONT_MONO);
                            }
                            /* Color swatch for glColor / glClearColor */
                            if (repl_color_picker_can_edit_cmd(i)) {
                                int sw = REPL_COLOR_SWATCH_W;
                                int sx = cp_x + cp_w - CODE_MARGIN_X - sw - 2;
                                int sy = line_y + (LINE_H - sw) / 2 - 1;
                                repl_color_picker_render_swatch(i, sx, sy);
                            }
                        }
                        color_for_type(g_cmds[i].type);
                        code_panel_draw_search_highlights(g_cmds[i].source,
                                                          search_row_idx,
                                                          wrap_start, wrap_len,
                                                          wrap_x, line_y);
                        code_panel_draw_segment(wrap_x, line_y, display_text,
                                                wrap_start, wrap_len, FONT_MONO);
                        line_y -= LINE_H;
                    }
                    cur++;
                    wrap_row++;
                }
                file_line++;

                if (g_replay_active &&
                    g_replay_expand_args &&
                    g_replay_src_line >= 0 && i == g_replay_src_line &&
                    g_cmds[i].has_vars &&
                    g_cmds[i].type != CMD_VAR_ASSIGN) {
                    int flat_idx = repl_replay_annotation_flat_cmd_for_source(i);
                    if (flat_idx >= 0) {
                        char subst[MAX_LINE_LEN], var_comment[128];
                        if (repl_replay_build_subst_annotation(i, flat_idx,
                                                          subst, sizeof(subst),
                                                          var_comment, sizeof(var_comment)) > 0) {
                            if (cur >= g_scroll &&
                                cur < g_scroll + visible_lines) {
                                glEnable(GL_BLEND);
                                glBlendFunc(GL_SRC_ALPHA,
                                            GL_ONE_MINUS_SRC_ALPHA);
                                glColor4f(0.10f, 0.25f, 0.15f, 0.35f);
                                draw_quad(0, (float)(line_y - 3),
                                          (float)panel_w, (float)LINE_H);
                                glDisable(GL_BLEND);
                                glColor3f(0.50f, 0.75f, 0.50f);
                                draw_string((float)text_x, (float)line_y,
                                            subst, FONT_MONO);
                                if (var_comment[0]) {
                                    int sw = (int)strlen(subst) * FONT_W;
                                    glColor3f(0.40f, 0.55f, 0.40f);
                                    draw_string((float)(text_x + sw),
                                                (float)line_y,
                                                var_comment, FONT_MONO);
                                }
                                line_y -= LINE_H;
                            }
                            cur++;
                        }

                        {
                            char eval_buf[MAX_LINE_LEN];
                            if (repl_replay_build_eval_annotation(i, flat_idx,
                                                             eval_buf, sizeof(eval_buf))) {
                                if (cur >= g_scroll &&
                                    cur < g_scroll + visible_lines) {
                                    glEnable(GL_BLEND);
                                    glBlendFunc(GL_SRC_ALPHA,
                                                GL_ONE_MINUS_SRC_ALPHA);
                                    glColor4f(0.15f, 0.15f, 0.25f, 0.35f);
                                    draw_quad(0, (float)(line_y - 3),
                                              (float)panel_w, (float)LINE_H);
                                    glDisable(GL_BLEND);
                                    glColor3f(0.50f, 0.60f, 0.80f);
                                    draw_string((float)text_x, (float)line_y,
                                                eval_buf, FONT_MONO);
                                    line_y -= LINE_H;
                                }
                                cur++;
                            }
                        }
                    }
                }
            }

            /* Advance vertex counter after rendering this command */
            if (is_vertex) vnum++;

            if (g_cmds[i].valid) {
                if (g_cmds[i].type == CMD_FOR_BEGIN) {
                    loop_depth++;
                    primitive_vnums_exact = 0;
                } else if (g_cmds[i].type == CMD_FOR_END) {
                    if (loop_depth > 0) loop_depth--;
                } else if (g_cmds[i].type == CMD_END) {
                    primitive_vnums_exact = 1;
                } else if (g_cmds[i].type == CMD_TESS_BEGIN_CONTOUR && in_tess_poly) {
                    tess_depth++;
                } else if (g_cmds[i].type == CMD_TESS_END && in_tess_poly) {
                    if (tess_depth > 0) tess_depth--;
                    if (tess_depth == 0) {
                        in_tess_poly = 0;
                        primitive_vnums_exact = 1;
                    }
                }
            }
        }
    }

    prof_end(PROF_CODE_PANEL_LINES_BODY_CMDS);
    prof_begin(PROF_CODE_PANEL_LINES_BODY_NEWLINE);

    /* New-line slot after the last command */
    {
        int is_edit_nl = (g_edit_line == g_num_cmds);
        if (is_edit_nl) {
            render_active_input_rows(panel_w, text_x, idx_x,
                                     visible_lines, file_line,
                                     repl_code_panel_document_active_indent_chars(), NULL,
                                     g_edit_line,
                                     &cur, &line_y);
        } else {
            if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                glColor3f(0.30f, 0.30f, 0.38f);
                { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                  draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                glColor3f(0.28f, 0.28f, 0.35f);
                { char ind_s[32]; int nc = cmd_indent_chars(g_num_cmds);
                  if (nc > 31) nc = 31;
                  memset(ind_s, ' ', nc); ind_s[nc] = '\0';
                  draw_string((float)text_x, (float)line_y, ind_s, FONT_MONO); }
                line_y -= LINE_H;
            }
        }
        file_line++;
        cur++;
    }

    prof_end(PROF_CODE_PANEL_LINES_BODY_NEWLINE);

    prof_end(PROF_CODE_PANEL_LINES_BODY);
    prof_begin(PROF_CODE_PANEL_LINES_FOOTER);

    /* Footer (dimmed) */
    for (int i = 0; g_footer_pre_init[i]; i++) {
        RENDER_STATIC_LINE(g_footer_pre_init[i], glColor3f(0.38f, 0.38f, 0.42f));
    }
    for (int i = 0; i < init_section_line_count(); i++) {
        char line[MAX_LINE_LEN];
        init_section_line(i, line, sizeof(line));
        RENDER_STATIC_LINE(line, glColor3f(0.38f, 0.38f, 0.42f));
    }
    for (int i = 0; g_footer_post_init[i]; i++) {
        RENDER_STATIC_LINE(g_footer_post_init[i], glColor3f(0.38f, 0.38f, 0.42f));
    }

    prof_end(PROF_CODE_PANEL_LINES_FOOTER);

    prof_end(PROF_CODE_PANEL_LINES);
    prof_begin(PROF_CODE_PANEL_OVERLAYS);

    #undef RENDER_STATIC_LINE

    /* Scroll indicator */
    if (total_lines > visible_lines) {
        int bar_h = cp_h - CODE_MARGIN_Y - LINE_H - STATUSBAR_H;
        float frac = (float)visible_lines / (float)total_lines;
        float pos  = (float)g_scroll / (float)total_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = panel_top - CODE_MARGIN_Y - LINE_H
                      - (int)(bar_h * pos) - thumb_h;

        glEnable(GL_BLEND);
        glColor4f(0.50f, 0.50f, 0.65f, 0.35f);
        draw_quad((float)(cp_x + cp_w - 6), (float)thumb_y,
                  5.0f, (float)thumb_h);
        glDisable(GL_BLEND);
    }

    /* Bottom status strip — design ref: Header Wireframes v2 statusbar.
     * Always drawn; shows cmd counts, cursor location, AA indicator, and the
     * transient `g_status` message (when set) in amber "err" style. */
    {
        int sy = cp_y;
        int sh = STATUSBAR_H;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        /* Strip bg (#181818) + top divider (#000) */
        glColor4f(0.094f, 0.094f, 0.094f, 0.98f);
        draw_quad((float)cp_x, (float)sy, (float)cp_w, (float)sh);
        glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)cp_x,          (float)(sy + sh));
        glVertex2f((float)(cp_x + cp_w), (float)(sy + sh));
        glEnd();

        int text_y = sy + (sh - FONT_SMALL_H) / 2 + 1;
        int tx = cp_x + CODE_MARGIN_X;

        /* cmd count */
        char cmds_buf[48];
        snprintf(cmds_buf, sizeof(cmds_buf), "%d/%d cmds",
                 g_num_flat_cmds, MAX_COMMANDS);
        glColor3f(0.878f, 0.878f, 0.878f); /* #e0e0e0 — stronger for counts */
        draw_string((float)tx, (float)text_y, cmds_buf, FONT_SMALL);
        tx += (int)strlen(cmds_buf) * FONT_SMALL_W;

        #define STATUSBAR_SEP() do {                                    \
            tx += 8;                                                    \
            glColor4f(0.20f, 0.20f, 0.20f, 1.0f); /* #333 */            \
            glBegin(GL_LINES);                                          \
            glVertex2f((float)tx, (float)(sy + 4));                     \
            glVertex2f((float)tx, (float)(sy + sh - 4));                \
            glEnd();                                                    \
            tx += 8;                                                    \
        } while (0)

        STATUSBAR_SEP();

        /* Line / insert-mode / begin-mode */
        char ln_buf[64];
        if (g_inserting)
            snprintf(ln_buf, sizeof(ln_buf), "Ln %d [INSERT]", g_edit_line + 1);
        else if (in_begin_block())
            snprintf(ln_buf, sizeof(ln_buf), "Ln %d  %s",
                     g_edit_line + 1, mode_name(current_begin_mode()));
        else
            snprintf(ln_buf, sizeof(ln_buf), "Ln %d", g_edit_line + 1);
        glColor3f(0.627f, 0.627f, 0.627f); /* #a0a0a0 */
        draw_string((float)tx, (float)text_y, ln_buf, FONT_SMALL);
        tx += (int)strlen(ln_buf) * FONT_SMALL_W;

        /* AA indicator */
        if (g_use_accum) {
            STATUSBAR_SEP();
            char aa_buf[24];
            if (g_accum_aa_enabled && g_accum_samples > 1)
                snprintf(aa_buf, sizeof(aa_buf), "AA %dx", g_accum_samples);
            else
                snprintf(aa_buf, sizeof(aa_buf), "AA off");
            draw_string((float)tx, (float)text_y, aa_buf, FONT_SMALL);
            tx += (int)strlen(aa_buf) * FONT_SMALL_W;
        }

        /* The amber g_status message renders at the bottom of the scene panel
         * (see render_scene_status()) — the statusbar has limited width. */

        /* Right-aligned F1 help affordance */
        {
            const char *help_kbd = "F1";
            const char *help_lbl = "help";
            int kbd_w = (int)strlen(help_kbd) * FONT_SMALL_W + 10;
            int lbl_w = (int)strlen(help_lbl) * FONT_SMALL_W;
            int rx = cp_x + cp_w - CODE_MARGIN_X - lbl_w;
            glColor3f(0.627f, 0.627f, 0.627f);
            draw_string((float)rx, (float)text_y, help_lbl, FONT_SMALL);
            int kx = rx - kbd_w - 6;
            int ky = sy + 3;
            int kh = sh - 6;
            glColor4f(0.078f, 0.078f, 0.078f, 1.0f); /* #141414 */
            draw_quad((float)kx, (float)ky, (float)kbd_w, (float)kh);
            glColor4f(0.20f, 0.20f, 0.20f, 1.0f); /* #333 */
            glBegin(GL_LINE_LOOP);
            glVertex2f((float)kx,           (float)ky);
            glVertex2f((float)(kx + kbd_w), (float)ky);
            glVertex2f((float)(kx + kbd_w), (float)(ky + kh));
            glVertex2f((float)kx,           (float)(ky + kh));
            glEnd();
            glColor3f(0.733f, 0.733f, 0.733f); /* #bbb */
            draw_string((float)(kx + 5), (float)(ky + 2), help_kbd, FONT_SMALL);
        }

        #undef STATUSBAR_SEP
        glDisable(GL_BLEND);
    }

    repl_color_picker_render();

    prof_end(PROF_CODE_PANEL_OVERLAYS);

    end_2d();
}

/* ========================================================================= */
/* Autocomplete popup                                                         */
/* ========================================================================= */

void render_autocomplete(void) {
    if (g_ac_count < 1) return;

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int popup_x = g_cursor_px;
    int popup_y = g_cursor_py - LINE_H - 4;

    /* Calculate popup width from longest match */
    int max_w = 0;
    for (int i = 0; i < g_ac_count; i++) {
        int w = (int)strlen(g_ac_matches[i]) * FONT_W;
        if (w > max_w) max_w = w;
    }
    int popup_w = max_w + 16;
    int popup_h = g_ac_count * LINE_H + 6;

    /* Clamp to code panel width */
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (popup_x + popup_w > cp_x + cp_w - 4)
        popup_x = cp_x + cp_w - popup_w - 4;
    if (popup_x < cp_x + 4) popup_x = cp_x + 4;

    /* Background */
    glColor4f(0.08f, 0.08f, 0.15f, 0.95f);
    draw_quad((float)popup_x, (float)(popup_y - popup_h),
              (float)popup_w, (float)popup_h);

    /* Border */
    glColor4f(0.40f, 0.40f, 0.65f, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)popup_x, (float)(popup_y - popup_h));
    glVertex2f((float)(popup_x + popup_w), (float)(popup_y - popup_h));
    glVertex2f((float)(popup_x + popup_w), (float)popup_y);
    glVertex2f((float)popup_x, (float)popup_y);
    glEnd();

    /* Entries */
    int ey = popup_y - LINE_H + 1;
    for (int i = 0; i < g_ac_count; i++) {
        if (i == g_ac_sel) {
            /* Highlight selected */
            glColor4f(0.20f, 0.25f, 0.42f, 0.90f);
            draw_quad((float)(popup_x + 1), (float)(ey - 2),
                      (float)(popup_w - 2), (float)LINE_H);
            glColor3f(1.0f, 1.0f, 0.90f);
        } else {
            glColor3f(0.65f, 0.65f, 0.72f);
        }
        draw_string((float)(popup_x + 8), (float)ey,
                    g_ac_matches[i], FONT_MONO);
        ey -= LINE_H;
    }

    /* Hint text */
    glColor4f(0.45f, 0.45f, 0.55f, 0.70f);
    draw_string((float)(popup_x + 4),
                (float)(popup_y - popup_h - FONT_H - 2),
                "Tab to accept", FONT_SMALL);

    glDisable(GL_BLEND);
    end_2d();
}

/* ========================================================================= */
/* Help overlay                                                               */
/* ========================================================================= */

void render_help(void) {
    if (!g_show_help) return;

    /* --- Tab 0: Commands ---
     * '\t' marks the boundary between left column (command) and
     * right column (description).  Lines without '\t' render in
     * a single colour based on indent level. */
    static const char *tab_commands[] = {
        "Supported Commands (type + ;):",
        "  glBegin(MODE)        \tGL_TRIANGLES, GL_TRIANGLE_STRIP, ...",
        "  glEnd()              \tEnd current primitive block",
        "  glVertex3f(x,y,z)    \tSpecify a vertex position",
        "  glVertex2f(x,y)      \tSpecify a 2D vertex (z=0)",
        "  glNormal3f(x,y,z)    \tSpecify a vertex normal",
        "  glColor3f(r,g,b)     \tSpecify vertex color",
        "  glColor4f(r,g,b,a)   \tSpecify color with alpha",
        "  glClearColor(r,g,b,a)\tSet the background clear color",
        "  glTranslatef(x,y,z)  \tTranslate the modelview matrix",
        "  glScalef(sx,sy,sz)   \tScale the modelview matrix",
        "  glRotatef(d,x,y,z)   \tRotate the modelview matrix",
        "  glPushMatrix()       \tPush current matrix onto stack",
        "  glPopMatrix()        \tPop matrix from stack",
        "  glEnable(CAP) / glDisable(CAP)",
        "       \tGL_BLEND, GL_COLOR_MATERIAL, GL_CULL_FACE, GL_DEPTH_TEST",
        "       \tGL_LIGHTING, GL_LINE_SMOOTH, GL_NORMALIZE, GL_POINT_SMOOTH",
        "       \tGL_LIGHT0..GL_LIGHT3",
        "  glShadeModel(MODE)   \tGL_SMOOTH, GL_FLAT",
        "  glFrontFace(MODE)    \tGL_CW, GL_CCW",
        "  glDepthMask(FLAG)    \tGL_TRUE, GL_FALSE (depth-buffer writes)",
        "  glPointSize(size)    \tRasterized point diameter",
        "  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)",
        "       \tDistance attenuation: size *= 1/sqrt(const + linear*d + quadratic*d*d)",
        "  glBlendFunc(sfactor, dfactor)",
        "       \tGL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA / GL_ONE",
        "",
        "Lighting / Material:",
        "  glColorMaterial(face, mode)",
        "       \tface: GL_FRONT, GL_BACK, or GL_FRONT_AND_BACK",
        "       \tmode: GL_AMBIENT / GL_AMBIENT_AND_DIFFUSE / GL_DIFFUSE / GL_SPECULAR / GL_EMISSION",
        "  glMaterialf(face, pname, value | {r,g,b,a})",
        "  glLightModeli(pname, param)",
        "       \tpname: GL_LIGHT_MODEL_TWO_SIDE, GL_LIGHT_MODEL_LOCAL_VIEWER",
        "",
        "GLU / GLUT Primitives:",
        "  gluSphere(r, slices, stacks)",
        "  gluCylinder(baseR, topR, h, slices, stacks)",
        "  gluDisk(innerR, outerR, slices, loops)",
        "  gluPartialDisk(innerR, outerR, slices, loops, start, sweep)",
        "  glutSolidTorus(innerR, outerR, nsides, rings)",
        "",
        "GLU Tessellator (concave / complex polygons):",
        "  gluBegin(GLU_POLYGON)  \tStart a tessellated polygon",
        "  gluBegin(GLU_CONTOUR)  \tStart a contour within the polygon",
        "  gluEnd()               \tEnd contour or polygon",
        "  gluNormal(x,y,z)       \tSet per-vertex normal",
        "  gluColor(r,g,b[,a])    \tSet per-vertex color",
        "  gluVertex(x,y,z)       \tAdd vertex to current contour",
        "  Multiple contours in one polygon create holes (opposite winding)",
        "",
        "Math Expressions (use anywhere floats are expected):",
        "  Constants:  PI, TAU       \tFunctions: sin cos tan sqrt abs pow rand",
        "  Operators:  + - * / % ( ) \tAlso: min max floor ceil fmod  rand(seed[,iter])",
        "  Comparison: > < >= <= == !=  Logical: && || !",
        "  Example:    glVertex3f(cos(PI/4), sin(PI/4), 0)",
        "",
        "Variables (declare before use):",
        "  float x, y, z;           \tDeclare variables",
        "  x = 1.5;                 \tAssign a value",
        "  glVertex3f(x, y, z);     \tUse in expressions",
        "  Variables persist across commands and are saved/loaded",
        "",
        "For-Loops:",
        "  for(i, 0, 24) glVertex3f(cos(i*TAU/24), sin(i*TAU/24), 0);",
        "  for(i, 0, N) {           \tMulti-line block:",
        "    glVertex3f(...)         \tend with }",
        "  }",
        "  Nesting supported up to 4 levels",
        "",
        "Functions (func0..func9, up to " _HELP_STR(MAX_FUNC_HINT_PARAMS) " params):",
        "  func0(radius, sides) {   \tDefine with parameters",
        "    for(i, 0, sides) {",
        "      glVertex3f(radius*cos(i*TAU/sides), ...)",
        "    }",
        "  }",
        "  func0(1.5, 6)            \tCall with args",
        "  Recursion works with if(...) guard",
        "  Up to " _HELP_STR(MAX_FUNC_HINT_PARAMS) " parameters per function",
        "",
        "Conditionals:",
        "  if(t > 1) {              \tBody runs when condition is true",
        "    glColor3f(1, 0, 0)",
        "  }",
        "",
        "Labels / Goto (experimental, top-level only):",
        "  :loop                    \tDeclare a jump target",
        "  goto loop                \tJump back; pair with if(...) to exit",
        "",
        "Comments:",
        "  // text                   \tType directly to add a comment line",
        "",
        "Save / Load:",
        "  Click Save C or press Ctrl+S to export output.c",
        "  Reload a saved file:  ./sample output.c",
        "  (Commands between // Snippet start/end are imported)",
        "",
        "Time variable 't':",
        "  Auto-advances from its current value when playing.",
        "  You can pull it back manually, then resume from there.",
        "  Use in any expression: glVertex3f(sin(t), cos(t), 0)",
        "",
        "Accumulation Buffer AA:",
        "  On by default; launch with --noaccum to disable.",
        "  Status shown in info bar (AA:8x / AA:off).",
        "",
        NULL
    };

    /* --- Tab 1: Keys ---
     * Same '\t' convention: left column = key, right = action.
     * The F-Key Toggles section is generated dynamically from g_cfg_items
     * so it always reflects the actual bindings without manual sync. */
    static const char *tab_keys_base[] = {
        "Editing:",
        "  ;                    \tCommit current line",
        "  Enter                \tInsert new line",
        "  Backspace            \tDelete character or selected lines",
        "  Tab / Enter          \tAccept autocomplete suggestion",
        "  Up / Down            \tNavigate lines",
        "  Left / Right         \tMove cursor within line",
        "  Home / Ctrl+A        \tJump to start of line",
        "  End / Ctrl+E         \tJump to end of line",
        "  Shift+Up/Down        \tSelect multiple lines",
        "  Click + drag         \tSelect lines with mouse",
        "  PgUp / PgDn         \tScroll active panel/overlay",
        "",
        "Clipboard & Undo:",
        "  Ctrl+C               \tCopy line/selection",
        "  Ctrl+X               \tCut line/selection",
        "  Ctrl+V               \tPaste before current line",
        "  Ctrl+Z               \tUndo",
        "  Ctrl+Y               \tRedo",
        "",
        "Buffer Operations:",
        "  Ctrl+F               \tSearch source buffer",
        "  Ctrl+D               \tDelete line or selection",
        "  Ctrl+L               \tClear all commands",
        "  Ctrl+\\              \tReformat buffer",
        "  Ctrl+/               \tToggle comment on line",
        "  Ctrl+P               \tDump debug state to stdout",
        "  Ctrl+S               \tSave to output.c",
        "  Ctrl+Q               \tExit and save to temp file",
        "  Escape               \tClear input / close overlay",
        "",
        "Camera:",
        "  Left-drag            \tOrbit",
        "  Right-drag           \tPan (XZ)",
        "  Shift+Right-drag     \tPan (Y)",
        "  Scroll wheel         \tZoom (viewport) / Scroll (code panel)",
        "",
        "Time & Replay:",
        "  Ctrl+T               \tPlay / pause time variable",
        "  Ctrl+Shift+T         \tReset t to 0",
        "  Ctrl+R               \tStart / stop replay",
        "  Ctrl+K               \tJump replay to cursor line (first geometry at/after)",
        "  Space                \tPause / resume replay",
        "  + / -                \tChange replay speed",
        "  m / M                \tToggle polygon / vertex replay mode",
        "  Left / Right         \tStep backward / forward (when paused)",
        "  Esc                  \tStop replay",
        "",
        "Render State:",
        "  Ctrl+B               \tCycle code panel layout",
        "  Ctrl+=               \tIncrease jitter samples",
        "  Ctrl+-               \tDecrease jitter samples",
        "  Ctrl+U               \tToggle GL_MULTISAMPLE",
        "  Ctrl+O               \tCycle grid major tick spacing (1 / 2 / 5 / 10)",
        "  Ctrl+W               \tCycle CPU profile panel",
        "  Ctrl+B               \tToggle Accum AA",
        "",
        "Interface:",
        "  `                    \tOpen Config menu",
        "  Left-click item      \tCycle config entry forward",
        "  Right-click item     \tCycle config entry backward",
        "",
        "Audio:",
        "  Ctrl+Left            \tPrevious track",
        "  Ctrl+Right           \tNext track",
        "",
        "F-Key Toggles:",
        NULL  /* dynamic F-key lines follow */
    };

    /* Build complete tab_keys array: static base + dynamic F-key entries */
    #define HELP_FKEY_MAX 16
    #define HELP_KEYS_MAX 128
    static char      fkey_strbuf[HELP_FKEY_MAX][48];
    static const char *tab_keys[HELP_KEYS_MAX];
    {
        int nk = 0;
        for (int i = 0; tab_keys_base[i] != NULL && nk < HELP_KEYS_MAX - HELP_FKEY_MAX - 4; i++)
            tab_keys[nk++] = tab_keys_base[i];

        /* F1 — not in g_cfg_items */
        snprintf(fkey_strbuf[0], sizeof(fkey_strbuf[0]), "  F1   \tHelp overlay");
        tab_keys[nk++] = fkey_strbuf[0];

        /* F2–F11 — pulled from g_cfg_items by matching key_code (GLUT_KEY_Fn == n) */
        int di = 1;
        for (int fn = 2; fn <= 11 && di < HELP_FKEY_MAX - 1; fn++) {
            for (int ci = 0; ci < CFG_ITEM_COUNT; ci++) {
                if (g_cfg_items[ci].is_special && g_cfg_items[ci].key_code == fn) {
                    snprintf(fkey_strbuf[di], sizeof(fkey_strbuf[di]),
                             "  F%-2d  \t%s", fn, g_cfg_items[ci].label);
                    tab_keys[nk++] = fkey_strbuf[di++];
                    break;
                }
            }
        }

        /* F12 — not in g_cfg_items */
        snprintf(fkey_strbuf[di], sizeof(fkey_strbuf[di]), "  F12  \tCycle examples");
        tab_keys[nk++] = fkey_strbuf[di];

        tab_keys[nk++] = "";
        tab_keys[nk]   = NULL;
    }
    #undef HELP_FKEY_MAX
    #undef HELP_KEYS_MAX

    static const char *tab_labels[] = { "Commands", "Keys" };
    static const char **tabs[]      = { tab_commands, tab_keys };
    #define HELP_NUM_TABS 2

    if (g_help_tab < 0) g_help_tab = 0;
    if (g_help_tab >= HELP_NUM_TABS) g_help_tab = HELP_NUM_TABS - 1;

    const char **text = tabs[g_help_tab];

    /* Count total lines */
    int n_lines = 0;
    while (text[n_lines]) n_lines++;

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int hx = g_win_w / 6, hy = g_win_h / 12;
    int hw = g_win_w * 2 / 3, hh = g_win_h * 5 / 6;
    int tab_bar_h = LINE_H + 2;
    int title_h   = LINE_H + 4;
    int pad_top   = title_h + tab_bar_h + 6;
    int pad_bot   = 20;
    int content_h = hh - pad_top - pad_bot;
    int visible_lines = content_h / LINE_H;
    if (visible_lines < 1) visible_lines = 1;

    /* Clamp scroll */
    int max_scroll = n_lines - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (g_help_scroll > max_scroll) g_help_scroll = max_scroll;
    if (g_help_scroll < 0) g_help_scroll = 0;

    /* Background — matches config menu #222 */
    glColor4f(0.133f, 0.133f, 0.133f, 0.98f);
    draw_quad((float)hx, (float)hy, (float)hw, (float)hh);

    /* Border — matches config menu #3a3a3a */
    glColor4f(0.227f, 0.227f, 0.227f, 1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)hx,        (float)hy);
    glVertex2f((float)(hx + hw), (float)hy);
    glVertex2f((float)(hx + hw), (float)(hy + hh));
    glVertex2f((float)hx,        (float)(hy + hh));
    glEnd();

    /* --- Title bar --- */
    {
        int title_y = hy + hh - title_h;
        /* Title bar separator */
        glColor4f(0.20f, 0.20f, 0.20f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)hx,        (float)title_y);
        glVertex2f((float)(hx + hw), (float)title_y);
        glEnd();

        /* Title text — dim, left-aligned like config menu section headers */
        glColor4f(0.478f, 0.518f, 0.580f, 1.0f);
        draw_string((float)(hx + 14), (float)(title_y + 4), "HELP", FONT_SMALL);

        /* Tab switch hint right-aligned */
        const char *nav_hint = "Left/Right: switch tabs";
        int nh_x = hx + hw - (int)strlen(nav_hint) * FONT_SMALL_W - 14;
        glColor4f(0.533f, 0.533f, 0.533f, 0.70f);
        draw_string((float)nh_x, (float)(title_y + 4), nav_hint, FONT_SMALL);
    }

    /* --- Tab bar --- */
    {
        int tab_y  = hy + hh - title_h - tab_bar_h;
        int tab_w  = hw / HELP_NUM_TABS;

        /* Tab bar background */
        glColor4f(0.10f, 0.10f, 0.10f, 1.0f);
        draw_quad((float)hx, (float)tab_y, (float)hw, (float)tab_bar_h);

        for (int t = 0; t < HELP_NUM_TABS; t++) {
            int tx_tab = hx + t * tab_w;
            if (t == g_help_tab) {
                /* Active tab: bottom accent bar + bright label */
                glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 0.85f);
                draw_quad((float)tx_tab, (float)tab_y, (float)tab_w, 2.0f);
                glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            } else {
                glColor4f(0.533f, 0.533f, 0.533f, 1.0f);
            }
            int lbl_len = (int)strlen(tab_labels[t]);
            int lbl_x   = tx_tab + (tab_w - lbl_len * FONT_SMALL_W) / 2;
            draw_string((float)lbl_x, (float)(tab_y + 3), tab_labels[t], FONT_SMALL);
        }

        /* Separator line below tab bar */
        glColor4f(0.20f, 0.20f, 0.20f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)hx,        (float)tab_y);
        glVertex2f((float)(hx + hw), (float)tab_y);
        glEnd();
    }

    /* --- Content --- */
    {
        int scissor_x = hx + 1;
        int scissor_y = hy + pad_bot;
        int scissor_w = hw - 2;
        int scissor_h = content_h;
        int have_scissor = (scissor_w > 0 && scissor_h > 0);

        if (scissor_w < 0) scissor_w = 0;
        if (scissor_h < 0) scissor_h = 0;

        if (have_scissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(scissor_x, scissor_y, scissor_w, scissor_h);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    }
    int tx      = hx + 14;
    int ty_start = hy + hh - pad_top - LINE_H + 3;

    /* Compute tab stop from widest left column so all right columns align */
    int tab_stop = 0;
    for (int i = 0; i < n_lines; i++) {
        const char *t = strchr(text[i], '\t');
        if (t) {
            int ln = (int)(t - text[i]);
            if (ln > tab_stop) tab_stop = ln;
        }
    }

    for (int i = g_help_scroll; i < n_lines && i < g_help_scroll + visible_lines + 1; i++) {
        int ty = ty_start - (i - g_help_scroll) * LINE_H;
        if (ty < hy + pad_bot - LINE_H) break;
        if (text[i][0] == '\0') continue;

        /* '\t' marks the left/right column boundary */
        const char *tab = strchr(text[i], '\t');
        if (tab) {
            /* Left column (command / key) — #d8d8d8 */
            char left[256];
            int ln = (int)(tab - text[i]);
            if (ln > 255) ln = 255;
            memcpy(left, text[i], ln);
            left[ln] = '\0';
            glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            draw_string((float)tx, (float)ty, left, FONT_SMALL);

            /* Right column (description) — aligned to shared tab stop */
            glColor4f(0.533f, 0.533f, 0.533f, 1.0f);
            draw_string((float)(tx + tab_stop * FONT_SMALL_W), (float)ty,
                        tab + 1, FONT_SMALL);
        } else if (text[i][0] != ' ') {
            /* Section header — dim gray-blue like config menu */
            glColor4f(0.478f, 0.518f, 0.580f, 1.0f);
            draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        } else if (text[i][2] == ' ' && text[i][3] == ' ') {
            /* 4+ space indent — code example, green accent */
            glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 0.90f);
            draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        } else {
            /* 2-space indent, no split — light label colour */
            glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        }
    }

    glDisable(GL_SCISSOR_TEST);

    /* Scroll indicator (only if content overflows) */
    if (n_lines > visible_lines) {
        int bar_x   = hx + hw - 8;
        int bar_top = hy + hh - pad_top;
        int bar_h   = content_h;
        float frac  = (float)visible_lines / (float)n_lines;
        float pos   = (float)g_help_scroll / (float)n_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = bar_top - (int)(bar_h * pos) - thumb_h;

        /* Track — #333 */
        glColor4f(0.20f, 0.20f, 0.20f, 0.60f);
        draw_quad((float)bar_x, (float)(bar_top - bar_h), 4.0f, (float)bar_h);

        /* Thumb — #888 */
        glColor4f(0.533f, 0.533f, 0.533f, 0.80f);
        draw_quad((float)bar_x, (float)thumb_y, 4.0f, (float)thumb_h);

        /* Scroll hint at bottom */
        if (g_help_scroll < max_scroll) {
            char hint[32];
            snprintf(hint, sizeof(hint), "v %d more v",
                     n_lines - g_help_scroll - visible_lines);
            int hint_x = hx + (hw - (int)strlen(hint) * FONT_SMALL_W) / 2;
            glColor4f(0.533f, 0.533f, 0.533f, 0.50f);
            draw_string((float)hint_x, (float)(hy + 4), hint, FONT_SMALL);
        }
    }

    glDisable(GL_BLEND);
    end_2d();

    #undef HELP_NUM_TABS
}

/* ========================================================================= */
/* Variable slider panel (4.8)                                               */
/* ========================================================================= */

/* Compute a shared logarithmic display scale from all variable absolute values.
 * All sliders use the same scale so their handles are normalized relative to
 * each other (a var at 100 shows near the extreme, one at 0.01 still visible). */
static float var_panel_log_scale(void) {
    float max_abs = 0.1f;   /* minimum display range */
    for (int i = 0; i < g_num_predef_vars; i++) {
        float av = fabsf(g_predef_vars[i].value);
        if (av > max_abs) max_abs = av;
    }
    return max_abs * 1.25f; /* 25% headroom so handle doesn't hug the edge */
}

/* Map a value to slider t in [0,1] using a symmetric asinh (log-like) scale.
 * The "knee" epsilon = 5% of scale is the linear region; beyond that the
 * response is logarithmic.  Zero always maps to 0.5 (centre). */
static float val_to_slider_t(float val, float scale) {
    float eps  = scale * 0.05f;               /* linear knee */
    float norm = asinhf(scale / eps);          /* ≈ asinh(20) ≈ 4.0 – fixed for scale */
    float t    = 0.5f + 0.5f * asinhf(val / eps) / norm;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

#define VAR_PANEL_W   200
#define VAR_PANEL_PAD   6
#define VAR_TITLE_H    20
#define VAR_ROW_H      20
#define VAR_PANEL_BASE_Y 8
/* Extra gap above REPLAY_HUD_BOTTOM_Y while replay HUD is active. */
#define VAR_REPLAY_CLEARANCE 10

static float g_var_panel_replay_lift_px = 0.0f;
static float g_var_panel_lift_update_time = -1.0f;
static float g_var_panel_lift_update_target = -1.0f;

static float var_panel_replay_target_lift_px(void) {
    float target = (float)((REPLAY_HUD_BOTTOM_Y + VAR_REPLAY_CLEARANCE)
                         - VAR_PANEL_BASE_Y);
    if (target < 0.0f) target = 0.0f;
    return target;
}

static float var_panel_replay_lift(void) {
    float target = 0.0f;
    if (g_replay_active)
        target = var_panel_replay_target_lift_px();

    if (g_var_panel_lift_update_time == g_anim_time &&
        g_var_panel_lift_update_target == target)
        return g_var_panel_replay_lift_px;
    g_var_panel_lift_update_time = g_anim_time;
    g_var_panel_lift_update_target = target;

    /* Exponential-decay style easing toward target (and back to 0 when replay ends). */
    g_var_panel_replay_lift_px += (target - g_var_panel_replay_lift_px) * 0.22f;
    if (fabsf(target - g_var_panel_replay_lift_px) < 0.25f)
        g_var_panel_replay_lift_px = target;

    return g_var_panel_replay_lift_px;
}

/* Geometry in render coords (y=0 at bottom). */
void var_panel_rect(int *px, int *py, int *pw, int *ph) {
    int sc_x, sc_y, sc_w, sc_h;
    int panel_w, panel_h, panel_x, panel_y;
    int min_y, max_y;

    scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    panel_w = VAR_PANEL_W;
    panel_h = VAR_TITLE_H + g_num_predef_vars * VAR_ROW_H + 2 * VAR_PANEL_PAD;
    panel_x = sc_x + sc_w - panel_w - 8;
    if (panel_x < sc_x + 4) panel_x = sc_x + 4;

    panel_y = sc_y + VAR_PANEL_BASE_Y + STATUSBAR_H
            + (int)lroundf(var_panel_replay_lift());
    min_y = sc_y + STATUSBAR_H + 4;
    max_y = sc_y + sc_h - panel_h - 4;
    if (max_y >= min_y) {
        if (panel_y < min_y) panel_y = min_y;
        if (panel_y > max_y) panel_y = max_y;
    } else {
        panel_y = code_panel_layout_mode() == CODE_PANEL_LAYOUT_TOP
                ? sc_y + sc_h - panel_h - 4
                : min_y;
    }

    if (px) *px = panel_x;
    if (py) *py = panel_y;
    if (pw) *pw = panel_w;
    if (ph) *ph = panel_h;
}

/* Return 1 if GLUT screen coord (gx, gy) is in the panel; sets *out_row. */
int var_panel_hit(int gx, int gy, int *out_row) {
    int px, py, pw, ph;
    var_panel_rect(&px, &py, &pw, &ph);
    int ry = g_win_h - gy;
    if (gx < px || gx >= px + pw || ry < py || ry >= py + ph) return 0;
    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;
    int row = (inner_top - ry) / VAR_ROW_H;
    if (row < 0 || row >= g_num_predef_vars) return 0;
    if (out_row) *out_row = row;
    return 1;
}

/* Amber status/error strip along the bottom of the scene panel.  The scene
 * is much wider than the code-panel statusbar slot, so long diagnostics
 * (~80 chars) fit here without truncation. */
void render_scene_status(void) {
    if (g_status_ttl <= 0 || !g_status[0]) return;

    int sc_x, sc_y, sc_w, sc_h;
    scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    if (sc_w <= 0 || sc_h <= 0) return;

    int bar_h = STATUSBAR_H;
    int bar_y = sc_y;

    float alpha = g_status_ttl > 60 ? 1.0f : (float)g_status_ttl / 60.0f;

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Design ref: amber error banner — bg #3a2a10, top rule #1a1208, text
     * #f0c070, "!" in bordered circle. */
    glColor4f(0.227f, 0.165f, 0.063f, 0.92f * alpha);
    draw_quad((float)sc_x, (float)bar_y, (float)sc_w, (float)bar_h);
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
    draw_string((float)(badge_x + badge_d / 2 - FONT_SMALL_W / 2 + 1),
                (float)text_y, "!", FONT_SMALL);

    int tx = badge_x + badge_d + 8;
    int max_px = sc_x + sc_w - CODE_MARGIN_X - tx;
    int max_chars = max_px / FONT_SMALL_W;
    if (max_chars < 8) max_chars = 8;
    if (max_chars > 255) max_chars = 255;

    char msg[256];
    int n = (int)strlen(g_status);
    if (n > max_chars) {
        snprintf(msg, sizeof(msg), "%.*s...", max_chars - 3, g_status);
    } else {
        snprintf(msg, sizeof(msg), "%s", g_status);
    }
    glColor4f(0.941f, 0.753f, 0.439f, alpha); /* #f0c070 */
    draw_string((float)tx, (float)text_y, msg, FONT_SMALL);

    glDisable(GL_BLEND);
    end_2d();
}

void render_var_panel(void) {
    if (!g_show_var_panel) return;

    int px, py, pw, ph;
    var_panel_rect(&px, &py, &pw, &ph);

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Background */
    glColor4f(0.05f, 0.05f, 0.10f, 0.88f);
    draw_quad((float)px, (float)py, (float)pw, (float)ph);

    /* Border */
    glColor4f(0.40f, 0.40f, 0.70f, 0.75f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)px,        (float)py);
    glVertex2f((float)(px + pw), (float)py);
    glVertex2f((float)(px + pw), (float)(py + ph));
    glVertex2f((float)px,        (float)(py + ph));
    glEnd();

    /* Title */
    glColor3f(0.75f, 0.75f, 1.00f);
    draw_string((float)(px + 6),
                (float)(py + ph - VAR_PANEL_PAD - 4),
                "Variables (declared)", FONT_SMALL);

    /* Column offsets within the panel — sized for multi-char var names */
    int max_name_len = 1;
    for (int i = 0; i < g_num_predef_vars; i++) {
        int len = (int)strlen(g_predef_vars[i].name);
        if (len > max_name_len) max_name_len = len;
    }
    int label_w  = max_name_len * FONT_SMALL_W + FONT_SMALL_W;
    if (label_w > pw / 3) label_w = pw / 3;
    int label_x  = px + 6;
    int val_x    = px + 6 + label_w;
    int track_x  = val_x + 66;
    int track_w  = pw - (track_x - px) - 8;
    int handle_w = 10;
    if (track_w < handle_w + 4) track_w = handle_w + 4;

    /* Shared logarithmic scale: all handles normalized relative to each other. */
    float log_scale = var_panel_log_scale();

    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;

    for (int i = 0; i < g_num_predef_vars; i++) {
        int row_y  = inner_top - (i + 1) * VAR_ROW_H;
        int text_y = row_y + 4;
        float val  = g_predef_vars[i].value;

        /* Drag highlight — amber tint for log mode, blue for linear */
        if (g_drag_var == i) {
            if (g_drag_log_mode)
                glColor4f(0.30f, 0.20f, 0.05f, 0.60f);
            else
                glColor4f(0.20f, 0.20f, 0.40f, 0.60f);
            draw_quad((float)(px + 1), (float)row_y,
                      (float)(pw - 2), (float)VAR_ROW_H);
        }

        /* Label */
        glColor3f(0.70f, 0.85f, 0.70f);
        draw_string((float)label_x, (float)text_y,
                    g_predef_vars[i].name, FONT_SMALL);

        /* Value */
        char valstr[16]; snprintf(valstr, sizeof(valstr), "%7.3f", (double)val);
        glColor3f(0.90f, 0.90f, 0.60f);
        draw_string((float)val_x, (float)text_y, valstr, FONT_SMALL);

        /* Slider track */
        glColor4f(0.18f, 0.18f, 0.28f, 0.90f);
        draw_quad((float)track_x, (float)(row_y + 6),
                  (float)track_w, (float)(VAR_ROW_H - 12));

        /* Centre tick (marks zero on the log scale) */
        float cx = (float)track_x + (float)track_w * 0.5f;
        glColor4f(0.35f, 0.35f, 0.50f, 0.70f);
        glBegin(GL_LINES);
        glVertex2f(cx, (float)(row_y + 5));
        glVertex2f(cx, (float)(row_y + VAR_ROW_H - 5));
        glEnd();

        /* Handle — position computed via shared log-normalized scale.
         * Yellow = linear drag, orange = log drag, blue = idle. */
        float t  = val_to_slider_t(val, log_scale);
        float hx = (float)track_x + t * (float)(track_w - handle_w);
        if (g_drag_var == i) {
            if (g_drag_log_mode)
                glColor4f(1.00f, 0.55f, 0.10f, 0.95f);  /* orange: log mode */
            else
                glColor4f(1.00f, 0.80f, 0.20f, 0.95f);  /* yellow: linear mode */
        } else {
            glColor4f(0.55f, 0.70f, 1.00f, 0.90f);      /* blue: idle */
        }
        draw_quad(hx, (float)(row_y + 4),
                  (float)handle_w, (float)(VAR_ROW_H - 8));
    }

    glDisable(GL_BLEND);
    end_2d();
}

/* ========================================================================= */
/* Configuration menu — now hosted inside the MENU_CONFIG dropdown            */
/* ========================================================================= */

int ui_panels_handle_right_press(int mx, int my) {
    return repl_menu_bar_handle_config_right_press(mx, my);
}

void ui_panels_close_menus(void) {
    repl_menu_bar_close();
    repl_color_picker_close();
}

/* ========================================================================= */
/* Inline scene rename                                                        */
/* ========================================================================= */

int ui_panels_rename_active(void) {
    return g_rename_slot >= 0;
}

int ui_panels_begin_rename(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return 0;
    if (!repl_user_scene_slot_used(slot))    return 0;
    g_rename_slot = slot;
    const char *cur = repl_user_scene_name(slot);
    snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", cur ? cur : "");
    g_rename_len = (int)strlen(g_rename_buf);
    rename_refresh_status();
    return 1;
}

void ui_panels_cancel_rename(void) {
    g_rename_slot = -1;
    g_rename_buf[0] = '\0';
    g_rename_len = 0;
}

static int rename_char_ok(unsigned char c) {
    if (c < 32 || c >= 127) return 0;
    if (c == '/' || c == '\\' || c == ':') return 0;
    return 1;
}

int ui_panels_handle_rename_key(unsigned char key) {
    if (g_rename_slot < 0) return 0;

    if (key == KEY_ESC) {
        ui_panels_cancel_rename();
        return 1;
    }
    if (key == '\r' || key == '\n') {
        /* repl_user_scene_rename trims whitespace and rejects empty
         * names at the API boundary.  A 0 return means reject-and-retry
         * (keep the overlay open); non-zero means success and we close. */
        if (!repl_user_scene_rename(g_rename_slot, g_rename_buf)) {
            set_status("Scene name cannot be empty");
            return 1;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "Renamed to: %s",
                 repl_user_scene_name(g_rename_slot));
        set_status(msg);
        ui_panels_cancel_rename();
        return 1;
    }
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (g_rename_len > 0) {
            g_rename_buf[--g_rename_len] = '\0';
            rename_refresh_status();
        }
        return 1;
    }
    if (rename_char_ok(key) && g_rename_len < (int)sizeof(g_rename_buf) - 1) {
        g_rename_buf[g_rename_len++] = (char)key;
        g_rename_buf[g_rename_len] = '\0';
        rename_refresh_status();
        return 1;
    }
    /* Swallow everything else so no stray character hits the editor. */
    return 1;
}

int ui_panels_handle_rename_special(int key) {
    if (g_rename_slot < 0) return 0;
    (void)key;
    return 1;  /* swallow all specials while renaming */
}

void ui_panels_open_config(void) {
    repl_menu_bar_open_config();
}

/* ========================================================================= */
/* Code panel click handler                                                   */
/* ========================================================================= */

static int g_code_panel_drag_active = 0;
static int g_code_panel_drag_anchor = -1;
static int g_code_panel_drag_moved = 0;

static int code_panel_hit_test(int mx, int my,
                               int *out_target,
                               int *out_on_insert_line,
                               int *out_row_offset) {
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0) return 0;
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;
    /* Convert GLUT Y (top=0) to OpenGL Y (bottom=0) */
    int gl_y = g_win_h - my;
    if (mx < cp_x || mx >= cp_x + cp_w) return 0;
    if (gl_y < cp_y || gl_y >= cp_y + cp_h) return 0;

    /* Same layout constants as render_code_panel */
    int line_y_start = panel_top - CODE_MARGIN_Y - 2 * LINE_H;
    int vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;
    if (vis < 0) return 0;   /* clicked in header */
    if (vis >= repl_code_panel_document_visible_lines_for_height(cp_h)) return 0;

    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = g_scroll + vis;
    CodePanelDocumentLayout layout;
    int target;
    int on_insert_line;
    int row_offset;

    repl_code_panel_document_build(&layout, panel_w, text_x, cp_h);
    if (!repl_code_panel_document_target_for_doc_line(doc_line, &layout,
                                                      &target,
                                                      &on_insert_line,
                                                      &row_offset))
        return 0;

    if (out_target) *out_target = target;
    if (out_on_insert_line) *out_on_insert_line = on_insert_line;
    if (out_row_offset) *out_row_offset = row_offset;
    return 1;
}

static int code_panel_drag_target(int mx, int my, int *out_target) {
    (void)mx;
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0) return 0;
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;
    int gl_y = g_win_h - my;
    int line_y_start = panel_top - CODE_MARGIN_Y - 2 * LINE_H;
    int vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;

    int visible_lines = repl_code_panel_document_visible_lines_for_height(cp_h);
    if (vis < 0) vis = 0;
    if (vis >= visible_lines) vis = visible_lines - 1;

    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = g_scroll + vis;
    CodePanelDocumentLayout layout;
    int target;
    int on_insert_line;

    repl_code_panel_document_build(&layout, panel_w, text_x, cp_h);
    if (!repl_code_panel_document_target_for_doc_line(doc_line, &layout,
                                                      &target,
                                                      &on_insert_line,
                                                      NULL))
        return 0;

    if (on_insert_line) {
        if (g_edit_line < g_num_cmds)
            target = g_edit_line;
        else if (g_num_cmds > 0)
            target = g_num_cmds - 1;
        else
            return 0;
    } else if (target >= g_num_cmds) {
        target = g_num_cmds - 1;
    }

    if (target < 0) target = 0;
    if (target >= g_num_cmds) target = g_num_cmds - 1;
    if (out_target) *out_target = target;
    return g_num_cmds > 0;
}

/* Handle left-click in the code panel: navigate to line + column */
void handle_code_panel_click(int mx, int my) {
    int target, on_insert_line, row_offset;
    if (!code_panel_hit_test(mx, my, &target, &on_insert_line, &row_offset)) return;

    if (!on_insert_line) {
        if (target < 0) target = 0;
        if (target > g_num_cmds) target = g_num_cmds;
        navigate_to_line(target);
    }

    int cp_w;
    code_panel_rect(NULL, NULL, &cp_w, NULL);
    int panel_w = cp_w;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int indent_chars = repl_code_panel_document_active_indent_chars();
    int seg_start = g_input_len;
    int seg_len = 0;
    int seg_x = text_x + indent_chars * FONT_W;
    int col;

    repl_code_panel_document_segment_for_row(g_input, seg_x, panel_w, row_offset,
                               &seg_start, &seg_len, &seg_x);

    col = (mx - seg_x + FONT_W / 2) / FONT_W;
    if (col < 0) col = 0;
    if (col > seg_len) col = seg_len;
    g_cursor_pos = seg_start + col;
    if (g_cursor_pos > g_input_len) g_cursor_pos = g_input_len;

    g_cursor_on = 1;
    g_blink_tick = 0;
    clear_autocomplete_state();
    clear_selection();
}

int handle_code_panel_press(int mx, int my) {
    int actions = UI_PANEL_PRESS_NONE;

    /* Color picker floats and may overlap the code panel (e.g. top/bottom
     * layouts).  Give it first crack so its hit rects take priority. */
    if (repl_color_picker_press(mx, my))
        return UI_PANEL_PRESS_CONSUMED;

    /* Pins (Search, Replay) take priority over menu labels and dropdown items
     * so they remain clickable even when a menu label visually overlaps them
     * in a narrow window — matches the render order (pins drawn on top). */
    int pin = repl_menu_bar_pin_hit(mx, my);
    if (pin >= 0) {
        repl_menu_bar_close();
        switch (pin) {
        case REPL_MENU_BAR_PIN_REPLAY:
            /* Button mirrors its glyph: pause when playing, resume when
             * paused, (re)start when stopped or done. */
            if (g_replay_state == REPLAY_PLAYING) {
                g_replay_state = REPLAY_PAUSED;
            } else if (g_replay_state == REPLAY_PAUSED) {
                g_replay_state = REPLAY_PLAYING;
            } else {
                replay_start();
            }
            break;
        case REPL_MENU_BAR_PIN_SEARCH:
            handle_search_key(KEY_CTRL_F);
            repl_menu_bar_note_search_opened();
            break;
        }
        return UI_PANEL_PRESS_CONSUMED;
    }

    /* Menu dropdown (floats over code) */
    if (menu_dropdown_is_open()) {
        /* Clicking the same top-level menu toggles closed; clicking another
         * switches to it. */
        int open_menu = repl_menu_bar_open_menu_id();
        int over_menu = repl_menu_bar_menu_hit(mx, my);
        if (over_menu >= 0) {
            if (over_menu == open_menu) {
                repl_menu_bar_close();
            } else {
                repl_menu_bar_set_open_menu(over_menu);
            }
            return UI_PANEL_PRESS_CONSUMED;
        }
        int item = repl_menu_bar_dropdown_item_hit(mx, my);
        if (item >= 0) {
            repl_menu_bar_activate_dropdown_item(item);
            return UI_PANEL_PRESS_CONSUMED;
        }
        /* Click outside dropdown: dismiss, fall through for code nav */
        repl_menu_bar_close();
    }

    int menu = repl_menu_bar_menu_hit(mx, my);
    if (menu >= 0) {
        repl_menu_bar_set_open_menu(menu);
        return UI_PANEL_PRESS_CONSUMED;
    }

    int target, on_insert_line, row_offset;
    if (!code_panel_hit_test(mx, my, &target, &on_insert_line, &row_offset))
        return UI_PANEL_PRESS_NONE;

    /* Check for swatch click on a color line */
    if (!on_insert_line && row_offset == 0 && target >= 0 && target < g_num_cmds) {
        if (repl_color_picker_can_edit_cmd(target)) {
            int cp_x2, cp_w2;
            code_panel_rect(&cp_x2, NULL, &cp_w2, NULL);
            int sx = cp_x2 + cp_w2 - CODE_MARGIN_X - REPL_COLOR_SWATCH_W - 2;
            if (mx >= sx && mx < sx + REPL_COLOR_SWATCH_W) {
                if (repl_color_picker_active_line() == target) {
                    repl_color_picker_close();   /* toggle: close picker */
                } else {
                    actions |= UI_PANEL_PRESS_OPENED_COLOR_PICKER;
                    repl_color_picker_open(target, my);
                }
                glutPostRedisplay();
                return actions | UI_PANEL_PRESS_CONSUMED;
            }
        }
    }
    /* Any non-swatch code-panel click closes the picker */
    repl_color_picker_close();

    handle_code_panel_click(mx, my);

    g_code_panel_drag_active = 0;
    g_code_panel_drag_anchor = -1;
    g_code_panel_drag_moved = 0;
    if (!on_insert_line && target >= 0 && target < g_num_cmds) {
        g_code_panel_drag_active = 1;
        g_code_panel_drag_anchor = target;
    }
    return actions | UI_PANEL_PRESS_CONSUMED;
}

int handle_code_panel_drag(int mx, int my) {
    int target;
    if (!g_code_panel_drag_active || g_code_panel_drag_anchor < 0) return 0;
    if (!code_panel_drag_target(mx, my, &target)) return 0;

    if (target != g_code_panel_drag_anchor || g_code_panel_drag_moved) {
        g_code_panel_drag_moved = 1;
        repl_selection_start(g_code_panel_drag_anchor);
        repl_selection_set_end(target);
        navigate_to_line(target);
        g_cursor_on = 1;
        g_blink_tick = 0;
    }
    return 1;
}

void handle_code_panel_release(void) {
    g_code_panel_drag_active = 0;
    g_code_panel_drag_anchor = -1;
    g_code_panel_drag_moved = 0;
}

int ui_panels_handle_escape(void) {
    return repl_color_picker_close();
}

int ui_panels_handle_scene_press(int mx, int my) {
    return repl_color_picker_press(mx, my);
}

int ui_panels_handle_motion(int mx, int my) {
    return repl_color_picker_motion(mx, my);
}

void ui_panels_handle_mouse_release(void) {
    repl_color_picker_release();
    handle_code_panel_release();
}
