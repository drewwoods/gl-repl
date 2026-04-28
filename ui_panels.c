/*
 * ui_panels.c - Code-panel row rendering, autocomplete/help/variable panels,
 *               panel input routing, and inline scene rename UI.
 *
 * Extracted from sample.c for maintainability.
 */
#include "sample.h"
#include "repl_actions.h"
#include "repl_state.h"
#include "repl_export.h"
#include "repl_layout.h"
#include "repl_source_scope.h"
#include "ui_color_picker.h"
#include "repl_code_panel_document.h"
#include "repl_core.h"
#include "repl_clipboard.h"
#include "repl_keys.h"
#include "repl_replay.h"
#include "ui_menu_bar.h"
#include "repl_replay_annotations.h"
#include "prof.h"
#include "ui_panels.h"
#include "./include/gl_2d.h"

#define IMPORT_EXPORT_STATE (repl_state_import_export())
#define g_workspace_header_lines (IMPORT_EXPORT_STATE->workspace_header_lines)
#define g_workspace_header_line_count (*IMPORT_EXPORT_STATE->workspace_header_line_count)
#define g_render_state_lines (IMPORT_EXPORT_STATE->render_state_lines)
#define g_cam_lines (IMPORT_EXPORT_STATE->cam_lines)

static int code_panel_layout_mode(void) {
    if (*repl_state_presentation()->code_panel_layout < 0 || *repl_state_presentation()->code_panel_layout >= CODE_PANEL_LAYOUT_COUNT)
        return CODE_PANEL_LAYOUT_LEFT;
    return *repl_state_presentation()->code_panel_layout;
}

/* Status footer height - design: 22px strip flush against code panel bottom */
/* STATUSBAR_H lives in sample.h now so scene_render.c can lift the
 * replay HUD above the amber status strip.  */

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
    gl2d_draw_string((float)x, (float)y, buf, font);
}

/* Menu bar lives in repl_menu_bar.c. */

static void code_panel_draw_search_highlights(const char *text, int search_row_idx,
                                              int seg_start, int seg_len,
                                              int seg_x, int y) {
    const ReplSearchState *srch = repl_state_search();
    int drew = 0;

    if (!*srch->active || *srch->query_len <= 0 || search_row_idx < 0 ||
        !text || seg_len <= 0)
        return;

    for (int pos = repl_search_find_next_in_text(text, srch->query, 0);
         pos >= 0;
         pos = repl_search_find_next_in_text(text, srch->query, pos + 1)) {
        int match_end = pos + *srch->query_len;
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

        if (search_row_idx == *srch->hit_line_idx && pos == *srch->hit_char_idx)
            glColor4f(0.95f, 0.65f, 0.18f, 0.55f);
        else
            glColor4f(0.25f, 0.45f, 0.85f, 0.30f);

        glRectf((float)((float)(seg_x + (draw_start - seg_start) * FONT_W)), (float)((float)(y - 2)), (float)((float)(seg_x + (draw_start - seg_start) * FONT_W))+(float)((float)((draw_end - draw_start) * FONT_W)), (float)((float)(y - 2))+(float)(FONT_H + 4));
    }

    if (drew)
        glDisable(GL_BLEND);
}

static void render_active_input_rows(int panel_w, int text_x, int idx_x,
                                     int visible_lines, int file_line,
                                     int indent_chars, const char *idx_text,
                                     int search_row_idx,
                                     int *io_cur, int *io_line_y) {
    const ReplEditorInputState      *inp    = repl_state_editor_input();
    const ReplCodePanelRuntimeState *cp     = repl_state_code_panel();
    const ReplAutocompleteState     *ac     = repl_state_autocomplete();
    const ReplSearchState           *srch   = repl_state_search();
    const char *input = inp->input;
    int cursor_pos = repl_state_cursor_pos();
    int input_len = *inp->input_len;
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
        if (*io_cur >= *cp->scroll && *io_cur < *cp->scroll + visible_lines) {
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
            code_panel_draw_search_highlights(input, search_row_idx,
                                              wrap_start, wrap_len,
                                              wrap_x, *io_line_y);
            code_panel_draw_segment(wrap_x, *io_line_y, input,
                                    wrap_start, wrap_len, FONT_MONO);

            if (wrap_row == cursor_row) {
                int cursor_x = wrap_x + cursor_col * FONT_W;
                int hint_x = cursor_x;

                if (ac->ghost[0] && cursor_pos == input_len) {
                    glEnable(GL_BLEND);
                    glColor4f(0.50f, 0.55f, 0.65f, 0.55f);
                    gl2d_draw_string((float)cursor_x, (float)(*io_line_y),
                                ac->ghost, FONT_MONO);
                    glDisable(GL_BLEND);
                    hint_x += (int)strlen(ac->ghost) * FONT_W;
                }

                if (ac->hint[0] && cursor_pos == input_len) {
                    glEnable(GL_BLEND);
                    glColor4f(0.56f, 0.62f, 0.72f, 0.38f);
                    gl2d_draw_string((float)hint_x, (float)(*io_line_y),
                                ac->hint, FONT_MONO);
                    glDisable(GL_BLEND);
                }

                if (*cp->cursor_visible && !*srch->active) {
                    glEnable(GL_BLEND);
                    glColor4f(0.90f, 0.80f, 0.25f, 0.85f);
                    glRectf((float)((float)cursor_x), (float)((float)(*io_line_y - 2)), (float)((float)cursor_x)+(float)(2.0f), (float)((float)(*io_line_y - 2))+(float)(FONT_H + 2));
                    glDisable(GL_BLEND);
                }

                *cp->cursor_px = cursor_x;
                *cp->cursor_py = *io_line_y;
            }

            *io_line_y -= LINE_H;
        }

        (*io_cur)++;
        wrap_row++;
    }
}

int ui_panels_code_panel_apply_scroll_follow_for_test(int *out_follow_doc_line,
                                            int *out_visible_lines) {
    CodePanelDocumentLayout layout;
    int cp_x, cp_y, cp_w, cp_h;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = *repl_state_presentation()->show_vertex_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;

    repl_state_refresh_workspace_header_lines();
    repl_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    (void)cp_x;
    (void)cp_y;

    repl_code_panel_document_build(&layout, cp_w, text_x, cp_h);
    repl_code_panel_document_apply_follow_scroll(&layout);

    if (out_follow_doc_line)
        *out_follow_doc_line = layout.follow_doc_line;
    if (out_visible_lines)
        *out_visible_lines = layout.visible_lines;
    int scroll = *repl_state_code_panel()->scroll;
    return layout.follow_doc_line >= scroll &&
           layout.follow_doc_line < scroll + layout.visible_lines;
}

/* Color picker lives in repl_color_picker.c. */

void ui_panels_render_code_panel(void) {
    const ReplReplayRuntimeState    *replay = repl_state_replay();
    const ReplCodePanelRuntimeState *cp     = repl_state_code_panel();
    const ReplRenderState           *rs     = repl_state_render();
    prof_begin(PROF_CODE_PANEL_LAYOUT);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM_SETUP);

    CodePanelDocumentLayout doc_layout;
    int cp_x, cp_y, cp_w, cp_h;
    repl_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0) {
        prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_SETUP);
        prof_end(PROF_CODE_PANEL_LAYOUT_GEOM);
        prof_end(PROF_CODE_PANEL_LAYOUT);
        return;
    }
    repl_state_refresh_workspace_header_lines();
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;  /* y of the panel's top edge (OpenGL coords) */
    int linenum_w = 4 * FONT_W;
    int idx_col_w = *repl_state_presentation()->show_vertex_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;
    int visible_lines;
    int total_lines;
    const GLCmd *document_cmds = repl_state_document_cmds();

    /* Own the full-window overlay viewport so the code panel and the UI
     * stack beneath it render in the same 2D projection. */
    glViewport(0, 0, *repl_state_viewport()->window_w, *repl_state_viewport()->window_h);

    /* When cursor is on a vertex, find which normal/color lines feed it so
     * we can draw a gutter accent bar on them below. */
    int highlight_normal_idx = -1;
    int highlight_color_idx  = -1;
    if (!repl_state_insert_mode() && repl_state_edit_line() < repl_state_document_count() && document_cmds[repl_state_edit_line()].valid) {
        highlight_normal_idx = repl_find_feeding_normal_cmd(repl_state_edit_line());
        highlight_color_idx  = repl_find_feeding_color_cmd(repl_state_edit_line());
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

    gl2d_begin(*repl_state_viewport()->window_w, *repl_state_viewport()->window_h);

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.06f, 0.06f, 0.10f, 0.92f);
    glRectf((float)((float)cp_x), (float)((float)cp_y), (float)((float)cp_x)+(float)((float)cp_w), (float)((float)cp_y)+(float)(cp_h));

    /* Border: divider between code panel and scene */
    glColor4f(0.30f, 0.30f, 0.50f, 0.80f);
    glBegin(GL_LINES);
    if (code_panel_layout_mode() == CODE_PANEL_LAYOUT_TOP) {
        /* Horizontal divider along bottom of code panel. */
        glVertex2f(0.0f, (float)cp_y);
        glVertex2f((float)*repl_state_viewport()->window_w, (float)cp_y);
    } else if (code_panel_layout_mode() == CODE_PANEL_LAYOUT_BOTTOM) {
        /* Horizontal divider along top of code panel. */
        glVertex2f(0.0f, (float)(cp_y + cp_h));
        glVertex2f((float)*repl_state_viewport()->window_w, (float)(cp_y + cp_h));
    } else {
        /* Vertical divider along right edge of code panel */
        glVertex2f((float)(cp_x + cp_w), 0.0f);
        glVertex2f((float)(cp_x + cp_w), (float)*repl_state_viewport()->window_h);
    }
    glEnd();
    glDisable(GL_BLEND);

    ui_menu_bar_render();
    ui_menu_bar_render_search_overlay(cp_x, panel_w, panel_top);

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
            if (cur >= *cp->scroll && cur < *cp->scroll + visible_lines) {            \
                if (wrap_row == 0) {                                             \
                    glColor3f(0.30f, 0.30f, 0.38f);                            \
                    { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);  \
                      gl2d_draw_string((float)CODE_MARGIN_X, (float)line_y,          \
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
    /* Camera transform lines (dynamic - updated every frame) */
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
    for (int i = 0; i < repl_state_document_count(); i++) {
        /* If inserting, render the virtual insert line before command[repl_state_edit_line()] */
        if (repl_state_insert_mode() && i == repl_state_edit_line()) {
                        render_active_input_rows(panel_w, text_x, idx_x,
                                                                         visible_lines, file_line,
                                                                         repl_code_panel_document_active_indent_chars(), NULL,
                                                                         repl_state_edit_line(),
                                                                         &cur, &line_y);
            file_line++;
        }

        if (i < repl_state_document_count()) {
            /* Track vertex number for all commands regardless of visibility */
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

            int is_edit = (!repl_state_insert_mode() && i == repl_state_edit_line());
            int is_vertex = document_cmds[i].valid && (document_cmds[i].type == CMD_VERTEX3F ||
                                                document_cmds[i].type == CMD_TESS_VERTEX);
            if (is_edit) {
                /* Active editing line */
                char idx_s[16];
                const char *idx_text = NULL;
                if (*repl_state_presentation()->show_vertex_indices && is_vertex) {
                    snprintf(idx_s, sizeof(idx_s),
                             primitive_vnums_exact ? "v%d" : "vn", vnum);
                    idx_text = idx_s;
                }
                render_active_input_rows(panel_w, text_x, idx_x,
                                         visible_lines, file_line,
                                         repl_code_panel_document_active_indent_chars(), idx_text,
                                         repl_state_edit_line(),
                                         &cur, &line_y);
                file_line++;
            } else {
                /* Existing command, not being edited */
                CodePanelWrapIter wrap_it;
                char display_text[MAX_INPUT_LEN];
                int wrap_row = 0;
                int wrap_start, wrap_len, wrap_x;
                int search_row_idx = repl_search_row_for_cmd_index(i);
                repl_replay_code_panel_get_command_display_text(i, display_text,
                                                    sizeof(display_text));
                repl_code_panel_document_wrap_iter_init(&wrap_it, display_text, text_x, panel_w);
                while (repl_code_panel_document_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
                    if (cur >= *cp->scroll && cur < *cp->scroll + visible_lines) {
                        if (*replay->active &&
                            *replay->src_line_idx >= 0 &&
                            i == *replay->src_line_idx) {
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            glColor4f(0.10f, 0.35f, 0.15f, 0.55f);
                            glRectf((float)(0), (float)((float)(line_y - 3)), (float)(0)+(float)((float)panel_w), (float)((float)(line_y - 3))+(float)(LINE_H));
                            glColor4f(0.20f, 0.90f, 0.30f, 0.85f);
                            glRectf((float)(1.0f), (float)((float)(line_y - 3)), (float)(1.0f)+(float)(3.0f), (float)((float)(line_y - 3))+(float)(LINE_H));
                            glDisable(GL_BLEND);
                        }
                        if (repl_clipboard_sel_active() && i >= repl_clipboard_sel_lo() && i <= repl_clipboard_sel_hi()) {
                            glEnable(GL_BLEND);
                            glColor4f(0.20f, 0.30f, 0.50f, 0.55f);
                            glRectf((float)(0), (float)((float)(line_y - 3)), (float)(0)+(float)((float)panel_w), (float)((float)(line_y - 3))+(float)(LINE_H));
                            glDisable(GL_BLEND);
                        }
                        if (i == highlight_normal_idx || i == highlight_color_idx) {
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            if (i == highlight_normal_idx)
                                glColor4f(0.40f, 0.80f, 0.95f, 0.85f);
                            else
                                glColor4f(0.95f, 0.85f, 0.30f, 0.85f);
                            glRectf((float)(1.0f), (float)((float)(line_y - 3)), (float)(1.0f)+(float)(3.0f), (float)((float)(line_y - 3))+(float)(LINE_H));
                            glDisable(GL_BLEND);
                        }
                        if (wrap_row == 0) {
                            glColor3f(0.30f, 0.30f, 0.38f);
                            { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                              gl2d_draw_string((float)CODE_MARGIN_X, (float)line_y,
                                          ln, FONT_MONO); }
                            if (*repl_state_presentation()->show_vertex_indices && is_vertex) {
                                char idx_s[16];
                                snprintf(idx_s, sizeof(idx_s),
                                         primitive_vnums_exact ? "v%d" : "vn", vnum);
                                glColor3f(0.45f, 0.50f, 0.65f);
                                gl2d_draw_string((float)idx_x, (float)line_y,
                                            idx_s, FONT_MONO);
                            }
                            /* Color swatch for glColor / glClearColor */
                            if (ui_color_picker_can_edit_cmd(i)) {
                                int sw = UI_COLOR_SWATCH_W;
                                int sx = cp_x + cp_w - CODE_MARGIN_X - sw - 2;
                                int sy = line_y + (LINE_H - sw) / 2 - 1;
                                ui_color_picker_render_swatch(i, sx, sy);
                            }
                        }
                        color_for_type(document_cmds[i].type);
                        code_panel_draw_search_highlights(document_cmds[i].source,
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

                if (*replay->active &&
                    *replay->expand_args &&
                    *replay->src_line_idx >= 0 &&
                    i == *replay->src_line_idx &&
                    document_cmds[i].has_vars &&
                    document_cmds[i].type != CMD_VAR_ASSIGN) {
                    int flat_idx = repl_replay_annotation_flat_cmd_for_source(i);
                    if (flat_idx >= 0) {
                        char subst[MAX_LINE_LEN], var_comment[128];
                        if (repl_replay_build_subst_annotation(i, flat_idx,
                                                          subst, sizeof(subst),
                                                          var_comment, sizeof(var_comment)) > 0) {
                            if (cur >= *cp->scroll &&
                                cur < *cp->scroll + visible_lines) {
                                glEnable(GL_BLEND);
                                glBlendFunc(GL_SRC_ALPHA,
                                            GL_ONE_MINUS_SRC_ALPHA);
                                glColor4f(0.10f, 0.25f, 0.15f, 0.35f);
                                glRectf((float)(0), (float)((float)(line_y - 3)), (float)(0)+(float)((float)panel_w), (float)((float)(line_y - 3))+(float)(LINE_H));
                                glDisable(GL_BLEND);
                                glColor3f(0.50f, 0.75f, 0.50f);
                                gl2d_draw_string((float)text_x, (float)line_y,
                                            subst, FONT_MONO);
                                if (var_comment[0]) {
                                    int sw = (int)strlen(subst) * FONT_W;
                                    glColor3f(0.40f, 0.55f, 0.40f);
                                    gl2d_draw_string((float)(text_x + sw),
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
                                if (cur >= *cp->scroll &&
                                    cur < *cp->scroll + visible_lines) {
                                    glEnable(GL_BLEND);
                                    glBlendFunc(GL_SRC_ALPHA,
                                                GL_ONE_MINUS_SRC_ALPHA);
                                    glColor4f(0.15f, 0.15f, 0.25f, 0.35f);
                                    glRectf((float)(0), (float)((float)(line_y - 3)), (float)(0)+(float)((float)panel_w), (float)((float)(line_y - 3))+(float)(LINE_H));
                                    glDisable(GL_BLEND);
                                    glColor3f(0.50f, 0.60f, 0.80f);
                                    gl2d_draw_string((float)text_x, (float)line_y,
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

            if (document_cmds[i].valid) {
                if (document_cmds[i].type == CMD_FOR_BEGIN) {
                    loop_depth++;
                    primitive_vnums_exact = 0;
                } else if (document_cmds[i].type == CMD_FOR_END) {
                    if (loop_depth > 0) loop_depth--;
                } else if (document_cmds[i].type == CMD_END) {
                    primitive_vnums_exact = 1;
                } else if (document_cmds[i].type == CMD_TESS_BEGIN_CONTOUR && in_tess_poly) {
                    tess_depth++;
                } else if (document_cmds[i].type == CMD_TESS_END && in_tess_poly) {
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
        int is_edit_nl = (repl_state_edit_line() == repl_state_document_count());
        if (is_edit_nl) {
            render_active_input_rows(panel_w, text_x, idx_x,
                                     visible_lines, file_line,
                                     repl_code_panel_document_active_indent_chars(), NULL,
                                     repl_state_edit_line(),
                                     &cur, &line_y);
        } else {
            if (cur >= *cp->scroll && cur < *cp->scroll + visible_lines) {
                glColor3f(0.30f, 0.30f, 0.38f);
                { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                  gl2d_draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                glColor3f(0.28f, 0.28f, 0.35f);
                { char ind_s[32]; int nc = repl_source_scope_cmd_indent_chars(repl_state_document_count());
                  if (nc > 31) nc = 31;
                  memset(ind_s, ' ', nc); ind_s[nc] = '\0';
                  gl2d_draw_string((float)text_x, (float)line_y, ind_s, FONT_MONO); }
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
        float pos  = (float)*cp->scroll / (float)total_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = panel_top - CODE_MARGIN_Y - LINE_H
                      - (int)(bar_h * pos) - thumb_h;

        glEnable(GL_BLEND);
        glColor4f(0.50f, 0.50f, 0.65f, 0.35f);
        glRectf((float)((float)(cp_x + cp_w - 6)), (float)((float)thumb_y), (float)((float)(cp_x + cp_w - 6))+(float)(5.0f), (float)((float)thumb_y)+(float)(thumb_h));
        glDisable(GL_BLEND);
    }

    /* Bottom status strip - design ref: Header Wireframes v2 statusbar.
     * Always drawn; shows cmd counts, cursor location, AA indicator, and the
     * transient `g_status` message (when set) in amber "err" style. */
    {
        int sy = cp_y;
        int sh = STATUSBAR_H;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        /* Strip bg (#181818) + top divider (#000) */
        glColor4f(0.094f, 0.094f, 0.094f, 0.98f);
        glRectf((float)((float)cp_x), (float)((float)sy), (float)((float)cp_x)+(float)((float)cp_w), (float)((float)sy)+(float)(sh));
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
                 repl_state_flat_program_count(), MAX_COMMANDS);
        glColor3f(0.878f, 0.878f, 0.878f); /* #e0e0e0 - stronger for counts */
        gl2d_draw_string((float)tx, (float)text_y, cmds_buf, FONT_SMALL);
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
        if (repl_state_insert_mode())
            snprintf(ln_buf, sizeof(ln_buf), "Ln %d [INSERT]", repl_state_edit_line() + 1);
        else if (repl_source_scope_in_begin_block())
            snprintf(ln_buf, sizeof(ln_buf), "Ln %d  %s",
                     repl_state_edit_line() + 1, mode_name(current_begin_mode()));
        else
            snprintf(ln_buf, sizeof(ln_buf), "Ln %d", repl_state_edit_line() + 1);
        glColor3f(0.627f, 0.627f, 0.627f); /* #a0a0a0 */
        gl2d_draw_string((float)tx, (float)text_y, ln_buf, FONT_SMALL);
        tx += (int)strlen(ln_buf) * FONT_SMALL_W;

        /* AA indicator */
        if (*rs->use_accum) {
            STATUSBAR_SEP();
            char aa_buf[24];
            if (*rs->accum_aa_enabled && *rs->accum_samples > 1)
                snprintf(aa_buf, sizeof(aa_buf), "AA %dx", *rs->accum_samples);
            else
                snprintf(aa_buf, sizeof(aa_buf), "AA off");
            gl2d_draw_string((float)tx, (float)text_y, aa_buf, FONT_SMALL);
            tx += (int)strlen(aa_buf) * FONT_SMALL_W;
        }

        /* The amber g_status message renders at the bottom of the scene panel
         * (see render_scene_status()) - the statusbar has limited width. */

        /* Right-aligned F1 help affordance */
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
            glColor4f(0.078f, 0.078f, 0.078f, 1.0f); /* #141414 */
            glRectf((float)((float)kx), (float)((float)ky), (float)((float)kx)+(float)((float)kbd_w), (float)((float)ky)+(float)(kh));
            glColor4f(0.20f, 0.20f, 0.20f, 1.0f); /* #333 */
            glBegin(GL_LINE_LOOP);
            glVertex2f((float)kx,           (float)ky);
            glVertex2f((float)(kx + kbd_w), (float)ky);
            glVertex2f((float)(kx + kbd_w), (float)(ky + kh));
            glVertex2f((float)kx,           (float)(ky + kh));
            glEnd();
            glColor3f(0.733f, 0.733f, 0.733f); /* #bbb */
            gl2d_draw_string((float)(kx + 5), (float)(ky + 2), help_kbd, FONT_SMALL);
        }

        #undef STATUSBAR_SEP
        glDisable(GL_BLEND);
    }

    ui_color_picker_render();

    prof_end(PROF_CODE_PANEL_OVERLAYS);

    gl2d_end();
}

/* ========================================================================= */
/* Scene status banner                                                        */
/* ========================================================================= */

/* Amber status/error strip along the bottom of the scene panel.  The scene
 * is much wider than the code-panel statusbar slot, so long diagnostics
 * (~80 chars) fit here without truncation. */
void ui_panels_render_scene_status(void) {
    const ReplStatusState *status = repl_state_status();
    if (*status->ttl <= 0 || !status->text[0]) return;

    int sc_x, sc_y, sc_w, sc_h;
    repl_layout_scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    if (sc_w <= 0 || sc_h <= 0) return;

    int bar_h = STATUSBAR_H;
    int bar_y = sc_y;

    float alpha = *status->ttl > 60 ? 1.0f : (float)*status->ttl / 60.0f;

    gl2d_begin(*repl_state_viewport()->window_w, *repl_state_viewport()->window_h);
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
    int n = (int)strlen(status->text);
    if (n > max_chars) {
        snprintf(msg, sizeof(msg), "%.*s...", max_chars - 3, status->text);
    } else {
        snprintf(msg, sizeof(msg), "%s", status->text);
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

void ui_panels_close_menus(void) {
    ui_menu_bar_close();
    ui_color_picker_close();
}
void ui_panels_open_config(void) {
    ui_menu_bar_open_config();
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
    repl_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0) return 0;
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;
    /* Convert GLUT Y (top=0) to OpenGL Y (bottom=0) */
    int gl_y = *repl_state_viewport()->window_h - my;
    if (mx < cp_x || mx >= cp_x + cp_w) return 0;
    if (gl_y < cp_y || gl_y >= cp_y + cp_h) return 0;

    /* Same layout constants as render_code_panel */
    int line_y_start = panel_top - CODE_MARGIN_Y - 2 * LINE_H;
    int vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;
    if (vis < 0) return 0;   /* clicked in header */
    if (vis >= repl_code_panel_document_visible_lines_for_height(cp_h)) return 0;

    int linenum_w = 4 * FONT_W;
    int idx_col_w = *repl_state_presentation()->show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = *repl_state_code_panel()->scroll + vis;
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
    repl_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0) return 0;
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;
    int gl_y = *repl_state_viewport()->window_h - my;
    int line_y_start = panel_top - CODE_MARGIN_Y - 2 * LINE_H;
    int vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;

    int visible_lines = repl_code_panel_document_visible_lines_for_height(cp_h);
    if (vis < 0) vis = 0;
    if (vis >= visible_lines) vis = visible_lines - 1;

    int linenum_w = 4 * FONT_W;
    int idx_col_w = *repl_state_presentation()->show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = *repl_state_code_panel()->scroll + vis;
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
        if (repl_state_edit_line() < repl_state_document_count())
            target = repl_state_edit_line();
        else if (repl_state_document_count() > 0)
            target = repl_state_document_count() - 1;
        else
            return 0;
    } else if (target >= repl_state_document_count()) {
        target = repl_state_document_count() - 1;
    }

    if (target < 0) target = 0;
    if (target >= repl_state_document_count()) target = repl_state_document_count() - 1;
    if (out_target) *out_target = target;
    return repl_state_document_count() > 0;
}

/* Handle left-click in the code panel: navigate to line + column */
void ui_panels_handle_code_panel_click(int mx, int my) {
    int target, on_insert_line, row_offset;
    if (!code_panel_hit_test(mx, my, &target, &on_insert_line, &row_offset)) return;

    if (!on_insert_line) {
        if (target < 0) target = 0;
        if (target > repl_state_document_count()) target = repl_state_document_count();
        navigate_to_line(target);
    }

    int cp_w;
    repl_layout_code_panel_rect(NULL, NULL, &cp_w, NULL);
    int panel_w = cp_w;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = *repl_state_presentation()->show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int indent_chars = repl_code_panel_document_active_indent_chars();
    int seg_start = *repl_state_editor_input()->input_len;
    int seg_len = 0;
    int seg_x = text_x + indent_chars * FONT_W;
    int col;

    repl_code_panel_document_segment_for_row(repl_state_editor_input()->input,
                               seg_x, panel_w, row_offset,
                               &seg_start, &seg_len, &seg_x);

    col = (mx - seg_x + FONT_W / 2) / FONT_W;
    if (col < 0) col = 0;
    if (col > seg_len) col = seg_len;
    {
        int new_cursor = seg_start + col;
        int cur_input_len = *repl_state_editor_input()->input_len;
        if (new_cursor > cur_input_len) new_cursor = cur_input_len;
        repl_state_cursor_pos_set(new_cursor);
    }

    repl_action_cursor_blink_reset();
    clear_autocomplete_state();
    repl_clipboard_clear_selection();
}

int ui_panels_handle_code_panel_press(int mx, int my) {
    int actions = UI_PANEL_PRESS_NONE;

    /* Color picker floats and may overlap the code panel (e.g. top/bottom
     * layouts).  Give it first crack so its hit rects take priority. */
    if (ui_color_picker_press(mx, my))
        return UI_PANEL_PRESS_CONSUMED;

    /* Pins (Search, Replay) take priority over menu labels and dropdown items
     * so they remain clickable even when a menu label visually overlaps them
     * in a narrow window - matches the render order (pins drawn on top). */
    int pin = ui_menu_bar_pin_hit(mx, my);
    if (pin >= 0) {
        ui_menu_bar_close();
        switch (pin) {
        case REPL_MENU_BAR_PIN_REPLAY:
            repl_replay_toggle_play_pause();
            break;
        case REPL_MENU_BAR_PIN_SEARCH:
            handle_search_key(KEY_CTRL_F);
            ui_menu_bar_note_search_opened();
            break;
        }
        return UI_PANEL_PRESS_CONSUMED;
    }

    /* Menu dropdown (floats over code) */
    if (ui_menu_bar_menu_dropdown_is_open()) {
        /* Clicking the same top-level menu toggles closed; clicking another
         * switches to it. */
        int open_menu = ui_menu_bar_open_menu_id();
        int over_menu = ui_menu_bar_menu_hit(mx, my);
        if (over_menu >= 0) {
            if (over_menu == open_menu) {
                ui_menu_bar_close();
            } else {
                ui_menu_bar_set_open_menu(over_menu);
            }
            return UI_PANEL_PRESS_CONSUMED;
        }
        int item = ui_menu_bar_dropdown_item_hit(mx, my);
        if (item >= 0) {
            ui_menu_bar_activate_dropdown_item(item);
            return UI_PANEL_PRESS_CONSUMED;
        }
        /* Click outside dropdown: dismiss, fall through for code nav */
        ui_menu_bar_close();
    }

    int menu = ui_menu_bar_menu_hit(mx, my);
    if (menu >= 0) {
        ui_menu_bar_set_open_menu(menu);
        return UI_PANEL_PRESS_CONSUMED;
    }

    int target, on_insert_line, row_offset;
    if (!code_panel_hit_test(mx, my, &target, &on_insert_line, &row_offset))
        return UI_PANEL_PRESS_NONE;

    /* Check for swatch click on a color line */
    if (!on_insert_line && row_offset == 0 && target >= 0 && target < repl_state_document_count()) {
        if (ui_color_picker_can_edit_cmd(target)) {
            int cp_x2, cp_w2;
            repl_layout_code_panel_rect(&cp_x2, NULL, &cp_w2, NULL);
            int sx = cp_x2 + cp_w2 - CODE_MARGIN_X - UI_COLOR_SWATCH_W - 2;
            if (mx >= sx && mx < sx + UI_COLOR_SWATCH_W) {
                if (ui_color_picker_active_line() == target) {
                    ui_color_picker_close();   /* toggle: close picker */
                } else {
                    actions |= UI_PANEL_PRESS_OPENED_COLOR_PICKER;
                    ui_color_picker_open(target, my);
                }
                glutPostRedisplay();
                return actions | UI_PANEL_PRESS_CONSUMED;
            }
        }
    }
    /* Any non-swatch code-panel click closes the picker */
    ui_color_picker_close();

    ui_panels_handle_code_panel_click(mx, my);

    g_code_panel_drag_active = 0;
    g_code_panel_drag_anchor = -1;
    g_code_panel_drag_moved = 0;
    if (!on_insert_line && target >= 0 && target < repl_state_document_count()) {
        g_code_panel_drag_active = 1;
        g_code_panel_drag_anchor = target;
    }
    return actions | UI_PANEL_PRESS_CONSUMED;
}

int ui_panels_handle_code_panel_drag(int mx, int my) {
    int target;
    if (!g_code_panel_drag_active || g_code_panel_drag_anchor < 0) return 0;
    if (!code_panel_drag_target(mx, my, &target)) return 0;

    if (target != g_code_panel_drag_anchor || g_code_panel_drag_moved) {
        g_code_panel_drag_moved = 1;
        repl_selection_start(g_code_panel_drag_anchor);
        repl_selection_set_end(target);
        navigate_to_line(target);
        repl_action_cursor_blink_reset();
    }
    return 1;
}

void ui_panels_handle_code_panel_release(void) {
    g_code_panel_drag_active = 0;
    g_code_panel_drag_anchor = -1;
    g_code_panel_drag_moved = 0;
}

int ui_panels_handle_escape(void) {
    return ui_color_picker_close();
}

int ui_panels_handle_scene_press(int mx, int my) {
    return ui_color_picker_press(mx, my);
}

int ui_panels_handle_motion(int mx, int my) {
    return ui_color_picker_motion(mx, my);
}

void ui_panels_handle_mouse_release(void) {
    ui_color_picker_release();
    ui_panels_handle_code_panel_release();
}
