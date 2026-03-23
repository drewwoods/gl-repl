/*
 * ui_panels.c — Code panel, autocomplete, help overlay, variable sliders,
 *               and configuration menu rendering.
 *
 * Extracted from sample.c for maintainability.
 */
#include "sample.h"
#include "ui_panels.h"

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
    case CMD_LIGHT_MODEL_I: glColor3f(0.80f, 0.70f, 0.95f); break; /* lavender */
    case CMD_COLOR3F:
    case CMD_COLOR4F:  glColor3f(0.95f, 0.85f, 0.30f); break;
    case CMD_FOR_BEGIN:
    case CMD_FOR_END:  glColor3f(0.95f, 0.60f, 0.30f); break;
    case CMD_FUNC_DEF:
    case CMD_FUNC_END: glColor3f(0.60f, 0.85f, 0.95f); break;
    case CMD_CALL:     glColor3f(0.60f, 0.85f, 0.95f); break;
    case CMD_IF_BEGIN:
    case CMD_IF_END:   glColor3f(0.95f, 0.75f, 0.50f); break;
    case CMD_COMMENT:    glColor3f(0.45f, 0.50f, 0.45f); break;
    case CMD_VAR_ASSIGN: glColor3f(0.55f, 0.80f, 0.95f); break;
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

/* ========================================================================= */
/* Code panel                                                                 */
/* ========================================================================= */

static void render_edit_line(int x, int y, int indent_chars) {
    int indent_px = indent_chars * FONT_W;

    /* Highlight background for active line */
    glEnable(GL_BLEND);
    glColor4f(0.15f, 0.18f, 0.28f, 0.70f);
    int panel_w = (int)(g_win_w * g_panel_frac);
    draw_quad(0, (float)(y - 3), (float)panel_w, (float)(LINE_H));
    glDisable(GL_BLEND);

    /* Indent (dimmed) */
    glColor3f(0.30f, 0.30f, 0.38f);
    { char spaces[16];
      memset(spaces, ' ', indent_chars);
      spaces[indent_chars] = '\0';
      draw_string((float)x, (float)y, spaces, FONT_MONO); }

    /* User-typed text */
    glColor3f(0.95f, 0.95f, 0.90f);
    draw_string((float)(x + indent_px), (float)y, g_input, FONT_MONO);

    /* Ghost autocomplete text (only when cursor is at end) */
    if (g_ac_ghost[0] && g_cursor_pos == g_input_len) {
        glEnable(GL_BLEND);
        glColor4f(0.50f, 0.55f, 0.65f, 0.55f);
        draw_string((float)(x + indent_px + g_input_len * FONT_W),
                    (float)y, g_ac_ghost, FONT_MONO);
        glDisable(GL_BLEND);
    }

    /* Blinking cursor at g_cursor_pos */
    if (g_cursor_on) {
        int cx = x + indent_px + g_cursor_pos * FONT_W;
        glEnable(GL_BLEND);
        glColor4f(0.90f, 0.80f, 0.25f, 0.85f);
        draw_quad((float)cx, (float)(y - 2), 2.0f, (float)(FONT_H + 2));
        glDisable(GL_BLEND);
    }

    /* Save cursor screen position for autocomplete popup */
    g_cursor_px = x + indent_px;
    g_cursor_py = y;
}

void render_code_panel(void) {
    int panel_w = (int)(g_win_w * g_panel_frac);
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;
    int visible_lines = (g_win_h - 2 * CODE_MARGIN_Y - LINE_H) / LINE_H;

    /* When cursor is on a vertex, find which normal/color lines feed it so
     * we can draw a gutter accent bar on them below. */
    int highlight_normal_idx = -1;
    int highlight_color_idx  = -1;
    if (!g_inserting && g_edit_line < g_num_cmds && g_cmds[g_edit_line].valid) {
        CmdType et = g_cmds[g_edit_line].type;
        int is_gl_vtx  = (et == CMD_VERTEX3F || et == CMD_VERTEX2F);
        int is_glu_vtx = (et == CMD_TESS_VERTEX);
        if (is_gl_vtx || is_glu_vtx) {
            for (int i = g_edit_line - 1; i >= 0; i--) {
                if (!g_cmds[i].valid) continue;
                CmdType t = g_cmds[i].type;
                if (t == CMD_BEGIN || t == CMD_END ||
                    t == CMD_TESS_BEGIN_POLYGON || t == CMD_TESS_BEGIN_CONTOUR ||
                    t == CMD_TESS_END) break;
                if (highlight_normal_idx < 0) {
                    if (is_gl_vtx  && t == CMD_NORMAL3F)     highlight_normal_idx = i;
                    if (is_glu_vtx && t == CMD_TESS_NORMAL)  highlight_normal_idx = i;
                }
                if (highlight_color_idx < 0) {
                    if (is_gl_vtx  && (t == CMD_COLOR3F || t == CMD_COLOR4F))
                        highlight_color_idx = i;
                    if (is_glu_vtx && t == CMD_TESS_COLOR)   highlight_color_idx = i;
                }
                if (highlight_normal_idx >= 0 && highlight_color_idx >= 0) break;
            }
        }
    }

    int n_hpre = 0;
    for (int i = 0; g_header_pre[i]; i++) n_hpre++;
    int n_hpost = 0;
    for (int i = 0; g_header_post[i]; i++) n_hpost++;
    int n_header = n_hpre + 3 + n_hpost;  /* pre + gluLookAt + post */
    int n_footer = 0;
    for (int i = 0; g_footer[i]; i++) n_footer++;
    /* +1 for the new-line slot, +1 if inserting */
    int total_lines = n_header + g_num_cmds + (g_inserting ? 1 : 0)
                    + 1 + n_footer;

    /* Which document line is the cursor on? (offset by header) */
    int cursor_doc_line = n_header + g_edit_line;

    /* Clamp scroll */
    int max_scroll = total_lines - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    /* Only snap to cursor after an edit; manual scroll can stay off-cursor. */
    if (g_scroll_follow_cursor) {
        if (cursor_doc_line < g_scroll)
            g_scroll = cursor_doc_line;
        if (cursor_doc_line >= g_scroll + visible_lines)
            g_scroll = cursor_doc_line - visible_lines + 1;
        if (g_scroll > max_scroll) g_scroll = max_scroll;
        if (g_scroll < 0) g_scroll = 0;
        g_scroll_follow_cursor = 0;
    }

    begin_2d();

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.06f, 0.06f, 0.10f, 0.92f);
    draw_quad(0, 0, (float)panel_w, (float)g_win_h);

    /* Border */
    glColor4f(0.30f, 0.30f, 0.50f, 0.80f);
    glBegin(GL_LINES);
    glVertex2f((float)panel_w, 0);
    glVertex2f((float)panel_w, (float)g_win_h);
    glEnd();
    glDisable(GL_BLEND);

    /* Top info bar */
    {
        char info[256];
        int nv = count_vertices();
        /* Accumulation AA indicator suffix */
        char aa_tag[24] = "";
        if (g_use_accum) {
            if (g_accum_aa_enabled && g_accum_samples > 1)
                snprintf(aa_tag, sizeof(aa_tag), " | AA:%dx", g_accum_samples);
            else
                snprintf(aa_tag, sizeof(aa_tag), " | AA:off");
        }
        /* Time variable indicator */
        char t_tag[32] = "";
        if (g_t_var_idx >= 0) {
            if (g_t_playing)
                snprintf(t_tag, sizeof(t_tag), " | t=%.2f",
                         g_predef_vars[g_t_var_idx].value);
            else
                snprintf(t_tag, sizeof(t_tag), " | t=%.2f[P]",
                         g_predef_vars[g_t_var_idx].value);
        }
        if (g_inserting) {
            snprintf(info, sizeof(info),
                     "F1:Help | %d cmds | %d verts | Ln %d [INSERT]%s%s",
                     g_num_cmds, nv, g_edit_line + 1, t_tag, aa_tag);
        } else if (in_begin_block()) {
            snprintf(info, sizeof(info),
                     "F1:Help | %d cmds | %d verts | %s | Ln %d%s%s",
                     g_num_cmds, nv, mode_name(current_begin_mode()),
                     g_edit_line + 1, t_tag, aa_tag);
        } else {
            snprintf(info, sizeof(info),
                     "F1:Help | %d cmds | %d verts | Ln %d%s%s",
                     g_num_cmds, nv, g_edit_line + 1, t_tag, aa_tag);
        }
        glColor3f(0.50f, 0.55f, 0.65f);
        draw_string(CODE_MARGIN_X, g_win_h - CODE_MARGIN_Y - 2, info,
                    FONT_SMALL);
    }

    /* Code lines */
    int line_y = g_win_h - CODE_MARGIN_Y - LINE_H - LINE_H;
    int cur = 0;
    int file_line = 1;

    /* Macro for rendering a static line (header/footer) */
    #define RENDER_STATIC_LINE(text, set_color) do {                           \
        if (cur >= g_scroll && cur < g_scroll + visible_lines) {               \
            glColor3f(0.30f, 0.30f, 0.38f);                                   \
            { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);         \
              draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }             \
            set_color;                                                          \
            draw_string(text_x, line_y, text, FONT_MONO);                      \
            line_y -= LINE_H;                                                   \
        }                                                                       \
        file_line++;                                                            \
        cur++;                                                                  \
    } while (0)

    /* Header pre-lookAt (dimmed) */
    for (int i = 0; g_header_pre[i]; i++) {
        RENDER_STATIC_LINE(g_header_pre[i], glColor3f(0.38f, 0.38f, 0.42f));
    }
    /* gluLookAt lines (slightly brighter - dynamic) */
    for (int i = 0; i < 3; i++) {
        RENDER_STATIC_LINE(g_lookat[i], glColor3f(0.50f, 0.45f, 0.55f));
    }
    /* Header post-lookAt */
    for (int i = 0; g_header_post[i]; i++) {
        RENDER_STATIC_LINE(g_header_post[i], glColor3f(0.38f, 0.38f, 0.42f));
    }

    /* Commands + insert line + new-line slot */
    int vnum = 0; /* vertex counter within current glBegin/glEnd block */
    for (int i = 0; i <= g_num_cmds; i++) {
        /* If inserting, render the virtual insert line before command[g_edit_line] */
        if (g_inserting && i == g_edit_line) {
            if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                glColor3f(0.55f, 0.55f, 0.30f);
                { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                  draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                int ind = cmd_indent_chars(i);
                render_edit_line(text_x, line_y, ind);
                line_y -= LINE_H;
            }
            file_line++;
            cur++;
        }

        if (i < g_num_cmds) {
            /* Track vertex number for all commands regardless of visibility */
            if (g_cmds[i].valid && (g_cmds[i].type == CMD_BEGIN ||
                                    g_cmds[i].type == CMD_TESS_BEGIN_POLYGON)) vnum = 0;

            int is_edit = (!g_inserting && i == g_edit_line);
            int is_vertex = g_cmds[i].valid && (g_cmds[i].type == CMD_VERTEX3F ||
                                                g_cmds[i].type == CMD_TESS_VERTEX);
            if (is_edit) {
                /* Active editing line */
                if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                    glColor3f(0.55f, 0.55f, 0.30f);
                    { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                      draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                    if (g_show_indices && is_vertex) {
                        char idx_s[16]; snprintf(idx_s, sizeof(idx_s), "v%d", vnum);
                        glColor3f(0.45f, 0.50f, 0.65f);
                        draw_string((float)idx_x, (float)line_y, idx_s, FONT_MONO);
                    }
                    int ind = cmd_indent_chars(i);
                    render_edit_line(text_x, line_y, ind);
                    line_y -= LINE_H;
                }
                file_line++;
                cur++;
            } else {
                /* Existing command, not being edited */
                if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                    /* Selection highlight */
                    if (sel_active() && i >= sel_lo() && i <= sel_hi()) {
                        glEnable(GL_BLEND);
                        glColor4f(0.20f, 0.30f, 0.50f, 0.55f);
                        draw_quad(0, (float)(line_y - 3),
                                  (float)panel_w, (float)LINE_H);
                        glDisable(GL_BLEND);
                    }
                    /* Gutter accent: show which normal/color feeds the cursor vertex */
                    if (i == highlight_normal_idx || i == highlight_color_idx) {
                        glEnable(GL_BLEND);
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                        if (i == highlight_normal_idx)
                            glColor4f(0.40f, 0.80f, 0.95f, 0.85f); /* cyan — normal */
                        else
                            glColor4f(0.95f, 0.85f, 0.30f, 0.85f); /* yellow — color */
                        draw_quad(1.0f, (float)(line_y - 3), 3.0f, (float)LINE_H);
                        glDisable(GL_BLEND);
                    }
                    glColor3f(0.30f, 0.30f, 0.38f);
                    { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                      draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                    if (g_show_indices && is_vertex) {
                        char idx_s[16]; snprintf(idx_s, sizeof(idx_s), "v%d", vnum);
                        glColor3f(0.45f, 0.50f, 0.65f);
                        draw_string((float)idx_x, (float)line_y, idx_s, FONT_MONO);
                    }
                    color_for_type(g_cmds[i].type);
                    draw_string((float)text_x, (float)line_y,
                                g_cmds[i].source, FONT_MONO);
                    line_y -= LINE_H;
                }
                file_line++;
                cur++;
            }

            /* Advance vertex counter after rendering this command */
            if (is_vertex) vnum++;
        } else {
            /* i == g_num_cmds: new-line slot */
            int is_edit_nl = (!g_inserting && g_edit_line == g_num_cmds);
            if (is_edit_nl) {
                if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                    glColor3f(0.55f, 0.55f, 0.30f);
                    { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                      draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                    int ind = cmd_indent_chars(g_num_cmds);
                    render_edit_line(text_x, line_y, ind);
                    line_y -= LINE_H;
                }
            } else {
                if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                    glColor3f(0.30f, 0.30f, 0.38f);
                    { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                      draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                    glColor3f(0.28f, 0.28f, 0.35f);
                    { char ind_s[32]; int nc = cmd_indent_chars(g_num_cmds);
                      if (nc > 31) nc = 31; memset(ind_s, ' ', nc); ind_s[nc] = '\0';
                      draw_string((float)text_x, (float)line_y, ind_s, FONT_MONO); }
                    line_y -= LINE_H;
                }
            }
            file_line++;
            cur++;
        }
    }

    /* Footer (dimmed) */
    for (int i = 0; g_footer[i]; i++) {
        RENDER_STATIC_LINE(g_footer[i], glColor3f(0.38f, 0.38f, 0.42f));
    }

    #undef RENDER_STATIC_LINE

    /* Scroll indicator */
    if (total_lines > visible_lines) {
        int bar_h = g_win_h - 2 * CODE_MARGIN_Y - LINE_H;
        float frac = (float)visible_lines / (float)total_lines;
        float pos  = (float)g_scroll / (float)total_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = g_win_h - CODE_MARGIN_Y - LINE_H
                      - (int)(bar_h * pos) - thumb_h;

        glEnable(GL_BLEND);
        glColor4f(0.50f, 0.50f, 0.65f, 0.35f);
        draw_quad((float)(panel_w - 6), (float)thumb_y,
                  5.0f, (float)thumb_h);
        glDisable(GL_BLEND);
    }

    /* Status bar */
    if (g_status_ttl > 0) {
        float alpha = g_status_ttl > 60 ? 1.0f : (float)g_status_ttl / 60.0f;
        glEnable(GL_BLEND);
        glColor4f(0.12f, 0.12f, 0.05f, 0.92f * alpha);
        draw_quad(0, 0, (float)panel_w, (float)(LINE_H + 6));
        glColor4f(1.0f, 0.85f, 0.20f, alpha);
        draw_string(CODE_MARGIN_X, 5, g_status, FONT_MONO);
        glDisable(GL_BLEND);
    }

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

    /* Clamp to panel width */
    int panel_w = (int)(g_win_w * g_panel_frac);
    if (popup_x + popup_w > panel_w - 4)
        popup_x = panel_w - popup_w - 4;
    if (popup_x < 4) popup_x = 4;

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

    static const char *text[] = {
        "=== OpenGL REPL - Help ===",
        "Press F1 or Escape to close.  Up/Down or PgUp/PgDn to scroll.",
        "",
        "Configuration Menu:",
        "  `                    Open configuration menu",
        "",
        "Supported Commands (type + ;):",
        "  glBegin(MODE)        GL_TRIANGLES, GL_TRIANGLE_STRIP, ...",
        "  glEnd()              End current primitive block",
        "  glVertex3f(x,y,z)    Specify a vertex position",
        "  glNormal3f(x,y,z)    Specify a vertex normal",
        "  glColor3f(r,g,b)     Specify vertex color",
        "  glColor4f(r,g,b,a)   Specify color with alpha",
        "  glTranslatef(x,y,z)  Translate the modelview matrix",
        "  glEnable(CAP)        GL_DEPTH_TEST, GL_LIGHTING, ...",
        "  glDisable(CAP)       GL_COLOR_MATERIAL, GL_NORMALIZE",
        "  glShadeModel(MODE)   GL_SMOOTH, GL_FLAT",
        "",
        "GLU Tessellator Commands (for concave / complex polygons):",
        "  Sequence:   gluBegin(GLU_POLYGON) → gluBegin(GLU_CONTOUR) → vertices → gluEnd() x2",
        "  gluBegin(GLU_POLYGON)  Start a tessellated polygon",
        "  gluBegin(GLU_CONTOUR)  Start a contour within the polygon",
        "  gluEnd()               End contour (1st call) or polygon (2nd call)",
        "  gluNormal(x,y,z)       Set per-vertex normal (persists until changed)",
        "  gluColor(r,g,b[,a])    Set per-vertex color   (persists until changed)",
        "  gluVertex(x,y,z)       Add vertex to current contour",
        "  Note: multiple contours in one polygon create holes (opposite winding)",
        "  Example:",
        "    gluBegin(GLU_POLYGON);",
        "    gluBegin(GLU_CONTOUR);",
        "    gluNormal(0, 0, 1);",
        "    gluColor(1, 0.5, 0, 1);",
        "    gluVertex(-1, -1, 0);  gluVertex(1, -1, 0);",
        "    gluVertex(0.5, 0.5, 0);  gluVertex(-0.5, 0.5, 0);",
        "    gluEnd();",
        "    gluEnd();",
        "",
        "GLU / GLUT Primitives:",
        "  gluSphere(r, slices, stacks)",
        "  gluCylinder(baseR, topR, h, slices, stacks)",
        "  gluDisk(innerR, outerR, slices, loops)",
        "  gluPartialDisk(innerR, outerR, slices, loops, startAngle, sweepAngle)",
        "  glutSolidTorus(innerR, outerR, nsides, rings)",
        "",
        "Comments:",
        "  // text              Type directly to add a comment line",
        "  Ctrl+/               Toggle comment on current line",
        "",
        "Math Expressions (use anywhere floats are expected):",
        "  Constants:  PI, TAU          Functions: sin cos tan sqrt abs pow",
        "  Operators:  + - * / % ( )    Also: min max floor ceil fmod",
        "  Comparison: > < >= <= == !=  Logical: && || !",
        "  Example:    glVertex3f(cos(PI/4), sin(PI/4), 0)",
        "",
        "Variables (predefined: x, y, z, i, j, k, n, t):",
        "  x = 1.5;                    Assign a value to a variable",
        "  glVertex3f(x, y, z);        Use variables in expressions",
        "  Variables persist across commands and are saved/loaded",
        "",
        "For-Loops (stored as editable blocks, saved as C for-loops):",
        "  for(i, 0, 24) glVertex3f(cos(i*TAU/24), sin(i*TAU/24), 0);",
        "  for(i, 0, N) {              Multi-line block:",
        "    glNormal3f(...)              type body lines, end with }",
        "    glVertex3f(...)              or press Esc to exit",
        "  }",
        "  Nesting supported up to 4 levels (for spheres, tori, etc.)",
        "  Ctrl+S preserves loop structure in output.c (not expanded)",
        "",
        "Functions (define reusable blocks, call with funcN(...)):",
        "  func0(radius, sides) {      Define a parameterized function:",
        "    for(i, 0, sides) {          args work in local for(...) / if(...)",
        "      glVertex3f(radius*cos(i*TAU/sides), radius*sin(i*TAU/sides), 0)",
        "    }                           type body lines, end with }",
        "  }",
        "  func0(1.5, 6)               Call with expressions or constants",
        "  func0()                     still works for zero-arg helpers",
        "  Up to func0..func9 supported",
        "",
        "Conditionals:",
        "  if(t > 1) {                 Body included when condition is true",
        "    glColor3f(1, 0, 0)",
        "  }",
        "  Supports: > < >= <= == != && || !",
        "",
        "Editing:",
        "  Up / Down            Navigate lines (scroll help when open)",
        "  Left / Right         Move cursor within line",
        "  Home / End           Jump to start / end of line",
        "  Type + ;             Commit line (edit existing or append new)",
        "  Enter                Insert new line (even in middle of list)",
        "  Tab / Enter          Accept autocomplete suggestion",
        "  Backspace            Delete character before cursor",
        "  Ctrl+/               Toggle comment on current line",
        "  Shift+Up/Down        Select multiple lines",
        "  Ctrl+C               Copy line/selection (for-loop on BEGIN)",
        "  Ctrl+X               Cut line/selection (for-loop on BEGIN)",
        "  Ctrl+V               Paste before current line",
        "  Ctrl+Z               Undo last command",
        "  Ctrl+Y/Ctrl+Shift+Z  Redo last command",
        "  Ctrl+D               Delete line at cursor",
        "  Ctrl+L               Clear all commands",
        "  Ctrl+R               Reformat command buffer",
        "  Ctrl+P               Dump editor code to stdout",
        "  Ctrl+S               Save to output.c",
        "  Ctrl+Q               Exit and save to temporary file",
        "  Escape               Clear input / exit insert / close help",
        "",
        "Camera:",
        "  Left-drag            Orbit",
        "  Right-drag           Pan",
        "  Scroll wheel         Zoom camera",
        "  Scroll wheel (over code panel)  Scroll code panel",
        "",
        "Time variable 't':",
        "  't' is a predefined var that auto-increments with elapsed time.",
        "  Use it in any expression: glVertex3f(sin(t), cos(t), 0)",
        "  Ctrl+T               Play / pause time (shown as t=X.XX or t=X.XX[P])",
        "",
        "Accumulation Buffer AA (requires accum buffer, on by default):",
        "  Ctrl+A               Toggle jitter AA on / off",
        "  Ctrl+=               Increase jitter samples (1->2->4->8->16)",
        "  Ctrl+-               Decrease jitter samples (16->8->4->2->1)",
        "  Status shown in info bar (AA:8x / AA:off)",
        "  Launch with --noaccum to disable the accum buffer entirely",
        "",
        "Toggles:",
        "  F1  Help overlay     F2  Wireframe mode",
        "  F3  Grid theme       F4  Axes theme",
        "  F5  Vertex numbers   F6  Normal vectors",
        "  F7  Vertex outlines  F8  Vertex guides",
        "  F9  Auto-normals     F10 Light indicators",
        "  F11 Camera rotate    F12 Cycle examples",
        "",
        "  PgUp / PgDn          Scroll code panel (or help when open)",
        "",
        "Save / Load:",
        "  Ctrl+S saves the session to output.c",
        "  Reload a saved file:  ./sample output.c",
        "  (Commands between // Snippet start/end are imported)",
        "",
        NULL
    };

    /* Count total lines */
    int n_lines = 0;
    while (text[n_lines]) n_lines++;

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int hx = g_win_w / 6, hy = g_win_h / 12;
    int hw = g_win_w * 2 / 3, hh = g_win_h * 5 / 6;
    int pad_top = 32, pad_bot = 24;
    int content_h = hh - pad_top - pad_bot;
    int visible_lines = content_h / LINE_H;
    if (visible_lines < 1) visible_lines = 1;

    /* Clamp scroll */
    int max_scroll = n_lines - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (g_help_scroll > max_scroll) g_help_scroll = max_scroll;
    if (g_help_scroll < 0) g_help_scroll = 0;

    /* Background */
    glColor4f(0.03f, 0.03f, 0.06f, 0.94f);
    draw_quad((float)hx, (float)hy, (float)hw, (float)hh);

    /* Border */
    glColor4f(0.45f, 0.45f, 0.75f, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)hx, (float)hy);
    glVertex2f((float)(hx + hw), (float)hy);
    glVertex2f((float)(hx + hw), (float)(hy + hh));
    glVertex2f((float)hx, (float)(hy + hh));
    glEnd();

    /* Scissor clip to content area */
    glEnable(GL_SCISSOR_TEST);
    glScissor(hx + 1, hy + pad_bot, hw - 2, content_h);

    int tx = hx + 24;
    int ty_start = hy + hh - pad_top;

    for (int i = g_help_scroll; i < n_lines && i < g_help_scroll + visible_lines + 1; i++) {
        int ty = ty_start - (i - g_help_scroll) * LINE_H;
        if (ty < hy + pad_bot - LINE_H) break;

        if (text[i][0] == '=')
            glColor3f(0.80f, 0.80f, 1.00f);
        else if (text[i][0] == ' ' && text[i][1] == ' ')
            glColor3f(0.65f, 0.90f, 0.65f);
        else if (text[i][0] == '\0')
            continue;   /* skip blank lines (save space) — still scrolls past them */
        else
            glColor3f(0.75f, 0.75f, 0.80f);

        draw_string((float)tx, (float)ty, text[i], FONT_MONO);
    }

    glDisable(GL_SCISSOR_TEST);

    /* Scroll indicator (only if content overflows) */
    if (n_lines > visible_lines) {
        int bar_x = hx + hw - 10;
        int bar_top = hy + hh - pad_top;
        int bar_h = content_h;
        float frac = (float)visible_lines / (float)n_lines;
        float pos  = (float)g_help_scroll / (float)n_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = bar_top - (int)(bar_h * pos) - thumb_h;

        /* Track */
        glColor4f(0.20f, 0.20f, 0.35f, 0.40f);
        draw_quad((float)bar_x, (float)(bar_top - bar_h), 5.0f, (float)bar_h);

        /* Thumb */
        glColor4f(0.55f, 0.55f, 0.80f, 0.65f);
        draw_quad((float)bar_x, (float)thumb_y, 5.0f, (float)thumb_h);

        /* Scroll hint at bottom */
        if (g_help_scroll < max_scroll) {
            glColor4f(0.50f, 0.50f, 0.65f, 0.50f);
            char hint[32];
            snprintf(hint, sizeof(hint), "v %d more v",
                     n_lines - g_help_scroll - visible_lines);
            int hint_x = hx + (hw - (int)strlen(hint) * FONT_W) / 2;
            draw_string((float)hint_x, (float)(hy + 6), hint, FONT_SMALL);
        }
    }

    glDisable(GL_BLEND);
    end_2d();
}

/* ========================================================================= */
/* Variable slider panel (4.8)                                               */
/* ========================================================================= */

#define VAR_PANEL_W   200
#define VAR_PANEL_PAD   6
#define VAR_TITLE_H    20
#define VAR_ROW_H      20

/* Geometry in render coords (y=0 at bottom). */
static void var_panel_geom(int *px, int *py, int *pw, int *ph) {
    *pw = VAR_PANEL_W;
    *ph = VAR_TITLE_H + g_num_predef_vars * VAR_ROW_H + 2 * VAR_PANEL_PAD;
    int vp_left = (int)(g_win_w * g_panel_frac);
    *px = g_win_w - *pw - 8;
    if (*px < vp_left + 4) *px = vp_left + 4;
    *py = 8;
}

/* Return 1 if GLUT screen coord (gx, gy) is in the panel; sets *out_row. */
int var_panel_hit(int gx, int gy, int *out_row) {
    int px, py, pw, ph;
    var_panel_geom(&px, &py, &pw, &ph);
    int ry = g_win_h - gy;
    if (gx < px || gx >= px + pw || ry < py || ry >= py + ph) return 0;
    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;
    int row = (inner_top - ry) / VAR_ROW_H;
    if (row < 0 || row >= g_num_predef_vars) return 0;
    if (out_row) *out_row = row;
    return 1;
}

void render_var_panel(void) {
    if (!g_show_var_panel) return;

    int px, py, pw, ph;
    var_panel_geom(&px, &py, &pw, &ph);

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
                "Variables", FONT_SMALL);

    /* Column offsets within the panel */
    int label_x  = px + 6;
    int val_x    = px + 22;       /* 1 char label + space */
    int track_x  = px + 88;       /* label + 8-char value */
    int track_w  = pw - 88 - 8;
    int handle_w = 10;
    float vmin = -5.0f, vmax = 5.0f;

    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;

    for (int i = 0; i < g_num_predef_vars; i++) {
        int row_y  = inner_top - (i + 1) * VAR_ROW_H;
        int text_y = row_y + 4;
        float val  = g_predef_vars[i].value;

        /* Drag highlight */
        if (g_drag_var == i) {
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

        /* Centre tick */
        float cx = (float)track_x + (float)track_w * 0.5f;
        glColor4f(0.35f, 0.35f, 0.50f, 0.70f);
        glBegin(GL_LINES);
        glVertex2f(cx, (float)(row_y + 5));
        glVertex2f(cx, (float)(row_y + VAR_ROW_H - 5));
        glEnd();

        /* Handle */
        float t = (val - vmin) / (vmax - vmin);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float hx = (float)track_x + t * (float)(track_w - handle_w);
        if (g_drag_var == i)
            glColor4f(1.00f, 0.80f, 0.20f, 0.95f);
        else
            glColor4f(0.55f, 0.70f, 1.00f, 0.90f);
        draw_quad(hx, (float)(row_y + 4),
                  (float)handle_w, (float)(VAR_ROW_H - 8));
    }

    glDisable(GL_BLEND);
    end_2d();
}

/* ========================================================================= */
/* Configuration menu (4.10)                                                  */
/* ========================================================================= */

#define CFG_ROW_H   22
#define CFG_PANEL_W 380
#define CFG_PAD      10
#define CFG_TITLE_H  24

static void cfg_panel_geom(int *px, int *py, int *pw, int *ph) {
    *pw = CFG_PANEL_W;
    *ph = CFG_TITLE_H + CFG_ITEM_COUNT * CFG_ROW_H + 2 * CFG_PAD;
    int vp_left = (int)(g_win_w * g_panel_frac);
    int vp_w    = g_win_w - vp_left;
    *px = vp_left + (vp_w - *pw) / 2;
    if (*px < vp_left + 4) *px = vp_left + 4;
    *py = (g_win_h - *ph) / 2;
    if (*py < 4) *py = 4;
}

/* Return config row index for GLUT coord, or -1 if outside panel. */
int cfg_hit_row(int gx, int gy) {
    int px, py, pw, ph;
    cfg_panel_geom(&px, &py, &pw, &ph);
    int ry = g_win_h - gy;
    if (gx < px || gx >= px + pw || ry < py || ry >= py + ph) return -1;
    int inner_top = py + ph - CFG_PAD - CFG_TITLE_H;
    int row = (inner_top - ry) / CFG_ROW_H;
    if (row < 0 || row >= CFG_ITEM_COUNT) return -1;
    return row;
}

void render_config_menu(void) {
    if (!g_show_config) return;

    int px, py, pw, ph;
    cfg_panel_geom(&px, &py, &pw, &ph);

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Background */
    glColor4f(0.04f, 0.04f, 0.08f, 0.93f);
    draw_quad((float)px, (float)py, (float)pw, (float)ph);

    /* Border */
    glColor4f(0.50f, 0.50f, 0.80f, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)px,        (float)py);
    glVertex2f((float)(px + pw), (float)py);
    glVertex2f((float)(px + pw), (float)(py + ph));
    glVertex2f((float)px,        (float)(py + ph));
    glEnd();

    /* Title */
    glColor3f(0.80f, 0.80f, 1.00f);
    draw_string((float)(px + 10),
                (float)(py + ph - CFG_PAD - 4),
                "Configuration  ( ` to close )", FONT_MONO);

    /* Column x positions */
    int col_label = px + 10;
    int col_state = px + pw - 190;
    int col_key   = px + pw - 56;

    int inner_top = py + ph - CFG_PAD - CFG_TITLE_H;

    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        int row_y  = inner_top - (i + 1) * CFG_ROW_H;
        int text_y = row_y + 5;

        /* Hover highlight */
        if (g_config_hover == i) {
            glColor4f(0.20f, 0.20f, 0.45f, 0.65f);
            draw_quad((float)(px + 1), (float)row_y,
                      (float)(pw - 2), (float)CFG_ROW_H);
        }

        /* Label */
        glColor3f(0.80f, 0.80f, 0.85f);
        draw_string((float)col_label, (float)text_y,
                    g_cfg_items[i].label, FONT_SMALL);

        /* Current state */
        int val = *g_cfg_items[i].value;
        const char *state_str;
        if (g_cfg_items[i].state_names) {
            state_str = g_cfg_items[i].state_names[val];
        } else {
            state_str = val ? "ON" : "OFF";
        }
        if (val)
            glColor3f(0.50f, 1.00f, 0.50f);
        else
            glColor3f(0.60f, 0.60f, 0.65f);
        draw_string((float)col_state, (float)text_y, state_str, FONT_SMALL);

        /* Key hint */
        glColor3f(0.50f, 0.60f, 0.75f);
        draw_string((float)col_key, (float)text_y,
                    g_cfg_items[i].key_hint, FONT_SMALL);
    }

    glDisable(GL_BLEND);
    end_2d();
}

/* ========================================================================= */
/* Code panel click handler                                                   */
/* ========================================================================= */

/* Handle left-click in the code panel: navigate to line + column */
void handle_code_panel_click(int mx, int my) {
    /* Convert GLUT Y (top=0) to OpenGL Y (bottom=0) */
    int gl_y = g_win_h - my;

    /* Same layout constants as render_code_panel */
    int line_y_start = g_win_h - CODE_MARGIN_Y - LINE_H - LINE_H;
    int vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;
    if (vis < 0) return;   /* clicked in info bar */

    int n_hpre = 0;
    for (int i = 0; g_header_pre[i]; i++) n_hpre++;
    int n_hpost = 0;
    for (int i = 0; g_header_post[i]; i++) n_hpost++;
    int n_header = n_hpre + 3 + n_hpost;

    int doc_line = g_scroll + vis;
    int cmd_area = doc_line - n_header;

    /* Ignore clicks on header or footer */
    int n_cmd_area = g_num_cmds + (g_inserting ? 1 : 0) + 1;
    if (cmd_area < 0 || cmd_area >= n_cmd_area) return;

    /* Map cmd_area index to actual command index, accounting for insert line */
    int target;
    int on_insert_line = 0;
    if (g_inserting) {
        if (cmd_area < g_edit_line) {
            target = cmd_area;
        } else if (cmd_area == g_edit_line) {
            target = -1;
            on_insert_line = 1;
        } else {
            target = cmd_area - 1;
        }
    } else {
        target = cmd_area;
    }

    if (!on_insert_line) {
        if (target < 0) target = 0;
        if (target > g_num_cmds) target = g_num_cmds;
        navigate_to_line(target);
    }

    /* Compute cursor column from click X */
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int edit_idx = on_insert_line ? g_edit_line : target;
    int indent_chars = cmd_indent_chars(
        edit_idx < g_num_cmds ? edit_idx : g_num_cmds);
    int col = (mx - text_x - indent_chars * FONT_W + FONT_W / 2) / FONT_W;
    if (col < 0) col = 0;
    if (col > g_input_len) col = g_input_len;
    g_cursor_pos = col;

    g_cursor_on = 1;
    g_blink_tick = 0;
    g_ac_count = 0;
    g_ac_ghost[0] = '\0';
    clear_selection();
}
