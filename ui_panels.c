/*
 * ui_panels.c — Code panel, autocomplete, help overlay, variable sliders,
 *               and configuration menu rendering.
 *
 * Extracted from sample.c for maintainability.
 */
#include "sample.h"
#include "repl_core.h"
#include "ui_panels.h"

/* ========================================================================= */
/* Layout geometry helpers                                                    */
/* ========================================================================= */

void code_panel_rect(int *x, int *y, int *w, int *h) {
    if (g_layout_vertical) {
        int panel_h = (int)(g_win_h * g_panel_frac);
        if (panel_h < 1) panel_h = 1;
        if (panel_h > g_win_h) panel_h = g_win_h;
        if (x) *x = 0;
        if (y) *y = g_win_h - panel_h;
        if (w) *w = g_win_w;
        if (h) *h = panel_h;
    } else {
        int panel_w = (int)(g_win_w * g_panel_frac);
        if (panel_w < 1) panel_w = 1;
        if (panel_w > g_win_w) panel_w = g_win_w;
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = panel_w;
        if (h) *h = g_win_h;
    }
}

void scene_rect(int *x, int *y, int *w, int *h) {
    if (g_layout_vertical) {
        int panel_h = (int)(g_win_h * g_panel_frac);
        if (panel_h < 1) panel_h = 1;
        if (panel_h > g_win_h) panel_h = g_win_h;
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = g_win_w;
        if (h) *h = g_win_h - panel_h;
    } else {
        int panel_w = (int)(g_win_w * g_panel_frac);
        if (panel_w < 1) panel_w = 1;
        if (panel_w > g_win_w) panel_w = g_win_w;
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
/* Replay variable display helpers                                            */
/* ========================================================================= */

/* Find the most recent flat command for a source line, at or before replay_pc */
static int find_replay_flat_cmd(int src_line) {
    if (g_replay_pc <= 0) return -1;
    for (int j = g_replay_pc - 1; j >= 0; j--) {
        if (g_flat_cmds[j].src_cmd_idx == src_line && g_flat_cmds[j].valid)
            return j;
    }
    return -1;
}

/* Replace predefined variable identifiers with their formatted values.
 * local_vars (loop variables) take priority over g_predef_vars.
 * Returns the count of distinct variables substituted.
 * Builds var_comment like " // t = 3.3, n = 12" */
static int subst_predef_vars(const char *source, char *out, int out_size,
                              char *var_comment, int comment_size,
                              const ExprVar *local_vars, int n_local_vars) {
    char used_names[MAX_PREDEF_VARS + MAX_EXPR_VARS][16];
    float used_vals[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    int num_used = 0;
    int max_used = MAX_PREDEF_VARS + MAX_EXPR_VARS;

    int oi = 0;
    const char *p = source;
    while (*p && oi < out_size - 20) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int len = (int)(p - start);

            int found = 0;
            float found_val = 0.0f;
            char found_name[16] = "";

            /* Check local (loop) vars first — they shadow predefined vars */
            for (int v = 0; !found && v < n_local_vars; v++) {
                int nlen = (int)strlen(local_vars[v].name);
                if (nlen == len &&
                    strncmp(start, local_vars[v].name, len) == 0) {
                    found = 1;
                    found_val = local_vars[v].value;
                    strncpy(found_name, local_vars[v].name, 15);
                    found_name[15] = '\0';
                }
            }
            /* Fall back to predefined (slider) vars */
            for (int v = 0; !found && v < g_num_predef_vars; v++) {
                int nlen = (int)strlen(g_predef_vars[v].name);
                if (nlen == len &&
                    strncmp(start, g_predef_vars[v].name, len) == 0) {
                    found = 1;
                    found_val = g_predef_vars[v].value;
                    strncpy(found_name, g_predef_vars[v].name, 15);
                    found_name[15] = '\0';
                }
            }

            if (found) {
                oi += snprintf(out + oi, out_size - oi, "%g", found_val);
                int dup = 0;
                for (int u = 0; u < num_used; u++) {
                    if (strcmp(used_names[u], found_name) == 0) {
                        dup = 1; break;
                    }
                }
                if (!dup && num_used < max_used) {
                    strncpy(used_names[num_used], found_name, 15);
                    used_names[num_used][15] = '\0';
                    used_vals[num_used] = found_val;
                    num_used++;
                }
            } else {
                if (oi + len < out_size) {
                    memcpy(out + oi, start, len);
                    oi += len;
                }
            }
        } else {
            out[oi++] = *p++;
        }
    }
    out[oi] = '\0';

    var_comment[0] = '\0';
    if (num_used > 0) {
        int ci = snprintf(var_comment, comment_size, " // ");
        for (int u = 0; u < num_used && ci < comment_size - 20; u++) {
            if (u > 0)
                ci += snprintf(var_comment + ci, comment_size - ci, ", ");
            ci += snprintf(var_comment + ci, comment_size - ci, "%s = %g",
                           used_names[u], used_vals[u]);
        }
    }

    return num_used;
}

/* Get format string for a command type's evaluated display */
static const char *eval_fmt_for_type(CmdType type, int *nargs_out) {
    switch (type) {
    case CMD_VERTEX3F:         *nargs_out = 3; return "glVertex3f(%g, %g, %g);";
    case CMD_VERTEX2F:         *nargs_out = 2; return "glVertex2f(%g, %g);";
    case CMD_NORMAL3F:         *nargs_out = 3; return "glNormal3f(%g, %g, %g);";
    case CMD_COLOR3F:          *nargs_out = 3; return "glColor3f(%g, %g, %g);";
    case CMD_COLOR4F:          *nargs_out = 4; return "glColor4f(%g, %g, %g, %g);";
    case CMD_TRANSLATE3F:      *nargs_out = 3; return "glTranslatef(%g, %g, %g);";
    case CMD_SCALEF:           *nargs_out = 3; return "glScalef(%g, %g, %g);";
    case CMD_ROTATEF:          *nargs_out = 4; return "glRotatef(%g, %g, %g, %g);";
    case CMD_GLU_SPHERE:       *nargs_out = 3; return "gluSphere(q, %g, %g, %g);";
    case CMD_GLU_CYLINDER:     *nargs_out = 5; return "gluCylinder(q, %g, %g, %g, %g, %g);";
    case CMD_GLU_DISK:         *nargs_out = 4; return "gluDisk(q, %g, %g, %g, %g);";
    case CMD_GLU_PARTIAL_DISK: *nargs_out = 6; return "gluPartialDisk(q, %g, %g, %g, %g, %g, %g);";
    case CMD_GLUT_TORUS:       *nargs_out = 4; return "glutSolidTorus(%g, %g, %g, %g);";
    case CMD_TESS_NORMAL:      *nargs_out = 3; return "gluNormal(%g, %g, %g);";
    case CMD_TESS_VERTEX:      *nargs_out = 3; return "gluVertex(%g, %g, %g);";
    default:                   *nargs_out = 0; return NULL;
    }
}

/* Format a command with evaluated numeric args.
 * Preserves leading whitespace from the original source. */
static int format_evaluated_cmd(const GLCmd *cmd, const char *orig_source,
                                 char *out, int out_size) {
    int indent = 0;
    while (orig_source[indent] &&
           isspace((unsigned char)orig_source[indent]))
        indent++;

    int oi = 0;
    if (indent > 0) {
        if (indent > out_size - 1) indent = out_size - 1;
        memcpy(out, orig_source, indent);
        oi = indent;
    }

    /* VAR_ASSIGN: show "var = value;" */
    if (cmd->type == CMD_VAR_ASSIGN) {
        const char *p = orig_source;
        while (*p && isspace((unsigned char)*p)) p++;
        char vname[16];
        int ni = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') && ni < 15)
            vname[ni++] = *p++;
        vname[ni] = '\0';
        snprintf(out + oi, out_size - oi, "%s = %g;", vname, cmd->args[0]);
        return 1;
    }

    int nargs;
    const char *fmt = eval_fmt_for_type(cmd->type, &nargs);
    if (!fmt || nargs < 1) return 0;

    switch (nargs) {
    case 2:
        snprintf(out + oi, out_size - oi, fmt,
                 cmd->args[0], cmd->args[1]);
        break;
    case 3:
        snprintf(out + oi, out_size - oi, fmt,
                 cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case 4:
        snprintf(out + oi, out_size - oi, fmt,
                 cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
        break;
    case 5:
        snprintf(out + oi, out_size - oi, fmt,
                 cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3],
                 cmd->args[4]);
        break;
    case 6:
        snprintf(out + oi, out_size - oi, fmt,
                 cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3],
                 cmd->args[4], cmd->args[5]);
        break;
    default:
        return 0;
    }
    return 1;
}

/* ========================================================================= */
/* Code panel                                                                 */
/* ========================================================================= */

typedef struct {
    const char *text;
    int         len;
    int         panel_w;
    int         pos;
    int         x;
    int         cont_x;
    int         done;
} CodeWrapIter;

#define CODE_PANEL_MAX_HANG_INDENT_CHARS 12

static int code_panel_available_chars(int panel_w, int x) {
    int avail_px = panel_w - x - 4;
    if (avail_px < FONT_W)
        return 0;
    return avail_px / FONT_W;
}

static int code_panel_cont_indent_chars(const char *text) {
    const char *src = text ? text : "";
    int leading = 0;

    while (src[leading] && isspace((unsigned char)src[leading]))
        leading++;

    const char *paren = strchr(src, '(');
    if (paren && paren[1] != '\0') {
        int align = (int)(paren - src) + 1;
        int max_align = leading + CODE_PANEL_MAX_HANG_INDENT_CHARS;
        if (align > max_align)
            align = max_align;
        return align;
    }

    return leading + 4;
}

/* Secondary break characters for long lines without commas. Preference order:
 * comma > closing paren > space > operator. Commas are tried first in a
 * separate pass so they always win when present. */
static int is_secondary_break(char c) {
    return c == ')' || c == ' ' || c == '+' || c == '*' || c == '-' || c == '/';
}

static int code_panel_find_wrap_break(const char *text, int start,
                                      int max_chars, int len) {
    int end = start + max_chars - 1;
    int search_start = start;
    if (end >= len)
        end = len - 1;

    while (search_start < len &&
           isspace((unsigned char)text[search_start]))
        search_start++;

    /* Prefer a comma within the window. */
    for (int i = end; i > search_start; i--) {
        if (text[i] == ',')
            return i;
    }

    /* Fall back to secondary break chars within the window. */
    for (int i = end; i > search_start; i--) {
        if (is_secondary_break(text[i]))
            return i;
    }

    /* Last resort: extend past the window to the next comma or break. */
    for (int i = end + 1; i < len; i++) {
        if (text[i] == ',' ||
            (i > search_start && is_secondary_break(text[i])))
            return i;
    }

    return -1;
}

static void code_wrap_iter_init(CodeWrapIter *it, const char *text,
                                int first_x, int panel_w) {
    it->text = text ? text : "";
    it->len = (int)strlen(it->text);
    it->panel_w = panel_w;
    it->pos = 0;
    it->x = first_x;
    it->cont_x = first_x + code_panel_cont_indent_chars(it->text) * FONT_W;
    it->done = 0;
}

static int code_wrap_iter_next(CodeWrapIter *it, int *out_start,
                               int *out_len, int *out_x) {
    if (it->done)
        return 0;

    *out_start = it->pos;
    *out_x = it->x;

    if (it->len == 0) {
        *out_len = 0;
        it->done = 1;
        return 1;
    }

    {
        int width_chars = code_panel_available_chars(it->panel_w, it->x);
        int remaining = it->len - it->pos;

        if (!g_wrap_at_comma || width_chars < 1 || remaining <= width_chars) {
            *out_len = remaining;
            it->done = 1;
            return 1;
        }

        {
            int break_idx = code_panel_find_wrap_break(it->text, it->pos,
                                                       width_chars, it->len);
            if (break_idx < 0) {
                *out_len = remaining;
                it->done = 1;
                return 1;
            }

            *out_len = break_idx - it->pos + 1;
            it->pos = break_idx + 1;
            it->x = it->cont_x;
            return 1;
        }
    }
}

static int code_panel_row_count_for_text(const char *text, int first_x,
                                         int panel_w) {
    CodeWrapIter it;
    int rows = 0;
    int start, len, x;

    code_wrap_iter_init(&it, text, first_x, panel_w);
    while (code_wrap_iter_next(&it, &start, &len, &x))
        rows++;

    return rows > 0 ? rows : 1;
}

static int code_panel_segment_for_row(const char *text, int first_x, int panel_w,
                                      int want_row, int *out_start,
                                      int *out_len, int *out_x) {
    CodeWrapIter it;
    int row = 0;
    int start, len, x;

    code_wrap_iter_init(&it, text, first_x, panel_w);
    while (code_wrap_iter_next(&it, &start, &len, &x)) {
        if (row == want_row) {
            if (out_start) *out_start = start;
            if (out_len) *out_len = len;
            if (out_x) *out_x = x;
            return 1;
        }
        row++;
    }

    return 0;
}

static int code_panel_cursor_row_for_text(const char *text, int first_x,
                                          int panel_w, int cursor_pos,
                                          int *out_seg_start,
                                          int *out_seg_len,
                                          int *out_seg_x) {
    CodeWrapIter it;
    int row = 0;
    int start, len, x;
    int text_len = (int)strlen(text ? text : "");

    if (cursor_pos < 0)
        cursor_pos = 0;
    if (cursor_pos > text_len)
        cursor_pos = text_len;

    code_wrap_iter_init(&it, text, first_x, panel_w);
    while (code_wrap_iter_next(&it, &start, &len, &x)) {
        int next_start = start + len;
        if (next_start >= text_len || cursor_pos < next_start) {
            if (out_seg_start) *out_seg_start = start;
            if (out_seg_len) *out_seg_len = len;
            if (out_seg_x) *out_seg_x = x;
            return row;
        }
        row++;
    }

    if (out_seg_start) *out_seg_start = text_len;
    if (out_seg_len) *out_seg_len = 0;
    if (out_seg_x) *out_seg_x = first_x;
    return 0;
}

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

static int code_panel_header_row_count(int panel_w, int text_x) {
    int rows = 0;

    for (int i = 0; i < g_workspace_header_line_count; i++)
        rows += code_panel_row_count_for_text(g_workspace_header_lines[i], text_x, panel_w);
    for (int i = 0; g_header_pre[i]; i++)
        rows += code_panel_row_count_for_text(g_header_pre[i], text_x, panel_w);
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        rows += code_panel_row_count_for_text(g_render_state_lines[i], text_x, panel_w);
    for (int i = 0; i < LOOKAT_LINE_COUNT; i++)
        rows += code_panel_row_count_for_text(g_lookat[i], text_x, panel_w);
    for (int i = 0; g_header_post[i]; i++)
        rows += code_panel_row_count_for_text(g_header_post[i], text_x, panel_w);

    return rows;
}

/* Header button bar: Save C | Example | Config | Replay */
#define NUM_HEADER_BTNS 4
static const char *g_header_btn_labels[NUM_HEADER_BTNS] = {
    "Save C", "Example", "Config", "Replay"
};

/* Example dropdown state */
static int g_example_dropdown_open  = 0;
static int g_example_dropdown_hover = -1;

static void header_btn_rects(int bx[NUM_HEADER_BTNS], int *by, int *bw, int *bh) {
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    int panel_top = cp_y + cp_h;
    /* Sit between info bar (row 0) and first code line (row 2) */
    if (by) *by = panel_top - CODE_MARGIN_Y - 2 * LINE_H;
    if (bh) *bh = LINE_H;
    /* Left-align buttons */
    int x = cp_x + CODE_MARGIN_X;
    for (int i = 0; i < NUM_HEADER_BTNS; i++) {
        bx[i] = x;
        x += (int)strlen(g_header_btn_labels[i]) * 8 + 12 + 4;
    }
    (void)bw; /* widths vary per button; callers compute per-label */
}

static int header_btn_hit(int gx, int gy) {
    int bx[NUM_HEADER_BTNS], by, bh;
    int ry = g_win_h - gy;
    header_btn_rects(bx, &by, NULL, &bh);
    for (int i = 0; i < NUM_HEADER_BTNS; i++) {
        int w = (int)strlen(g_header_btn_labels[i]) * 8 + 12;
        if (gx >= bx[i] && gx < bx[i] + w && ry >= by && ry < by + bh)
            return i;
    }
    return -1;
}

/* Returns dropdown item index under (gx, gy), or -1 if none */
static int example_dropdown_item_hit(int gx, int gy) {
    if (!g_example_dropdown_open) return -1;
    int n = repl_example_count();
    if (n == 0) return -1;
    int bx[NUM_HEADER_BTNS], by, bh;
    int ry = g_win_h - gy;
    header_btn_rects(bx, &by, NULL, &bh);
    int dx = bx[1];
    /* Width: widest label + padding */
    int max_w = 0;
    for (int i = 0; i < n; i++) {
        int w = (int)strlen(repl_example_name(i)) * FONT_W;
        if (w > max_w) max_w = w;
    }
    int dw = max_w + 20;
    int dh = n * LINE_H + 6;
    int dy = by - dh;  /* drops below the button row */
    if (gx < dx || gx >= dx + dw || ry < dy || ry >= dy + dh) return -1;
    int row = (dy + dh - 3 - ry) / LINE_H;
    if (row < 0 || row >= n) return -1;
    return row;
}

static int code_panel_footer_row_count(int panel_w, int text_x) {
    int rows = 0;
    char line[MAX_LINE_LEN];

    for (int i = 0; g_footer_pre_init[i]; i++)
        rows += code_panel_row_count_for_text(g_footer_pre_init[i], text_x, panel_w);
    for (int i = 0; i < init_section_line_count(); i++) {
        init_section_line(i, line, sizeof(line));
        rows += code_panel_row_count_for_text(line, text_x, panel_w);
    }
    for (int i = 0; g_footer_post_init[i]; i++)
        rows += code_panel_row_count_for_text(g_footer_post_init[i], text_x, panel_w);

    return rows;
}

static int code_panel_replay_extra_rows_for_line(int cmd_idx) {
    if (!g_replay_active || g_replay_state == REPLAY_OFF)
        return 0;
    if (cmd_idx < 0 || cmd_idx >= g_num_cmds)
        return 0;
    if (cmd_idx != g_replay_src_line)
        return 0;
    if (!g_cmds[cmd_idx].has_vars)
        return 0;
    return 2;
}

static int code_panel_command_main_rows(int cmd_idx, int panel_w, int text_x) {
    if (!g_inserting && cmd_idx == g_edit_line) {
        int indent_chars = cmd_indent_chars(cmd_idx);
        return code_panel_row_count_for_text(g_input,
                                             text_x + indent_chars * FONT_W,
                                             panel_w);
    }

    return code_panel_row_count_for_text(g_cmds[cmd_idx].source, text_x, panel_w);
}

static int code_panel_insert_rows(int panel_w, int text_x) {
    int indent_chars = cmd_indent_chars(g_edit_line);
    return code_panel_row_count_for_text(g_input,
                                         text_x + indent_chars * FONT_W,
                                         panel_w);
}

static int code_panel_newline_rows(int panel_w, int text_x) {
    if (!g_inserting && g_edit_line == g_num_cmds) {
        int indent_chars = cmd_indent_chars(g_num_cmds);
        return code_panel_row_count_for_text(g_input,
                                             text_x + indent_chars * FONT_W,
                                             panel_w);
    }
    return 1;
}

static int code_panel_target_for_doc_line(int doc_line, int panel_w, int text_x,
                                          int *out_target,
                                          int *out_on_insert_line,
                                          int *out_row_offset) {
    int row = doc_line - code_panel_header_row_count(panel_w, text_x);

    if (row < 0)
        return 0;

    for (int i = 0; i <= g_num_cmds; i++) {
        if (g_inserting && i == g_edit_line) {
            int insert_rows = code_panel_insert_rows(panel_w, text_x);
            if (row < insert_rows) {
                if (out_target) *out_target = -1;
                if (out_on_insert_line) *out_on_insert_line = 1;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            row -= insert_rows;
        }

        if (i < g_num_cmds) {
            int main_rows = code_panel_command_main_rows(i, panel_w, text_x);
            if (row < main_rows) {
                if (out_target) *out_target = i;
                if (out_on_insert_line) *out_on_insert_line = 0;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            row -= main_rows;

            {
                int replay_rows = code_panel_replay_extra_rows_for_line(i);
                if (row < replay_rows) {
                    if (out_target) *out_target = i;
                    if (out_on_insert_line) *out_on_insert_line = 0;
                    if (out_row_offset) *out_row_offset = 0;
                    return 1;
                }
                row -= replay_rows;
            }
        } else {
            int newline_rows = code_panel_newline_rows(panel_w, text_x);
            if (row < newline_rows) {
                if (out_target) *out_target = g_num_cmds;
                if (out_on_insert_line) *out_on_insert_line = 0;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            return 0;
        }
    }

    return 0;
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

static void code_panel_format_search_query(char *out, int out_sz,
                                           int max_chars,
                                           int *out_cursor_col) {
    int start = 0;
    int take = 0;

    if (out_sz <= 0)
        return;

    out[0] = '\0';
    if (out_cursor_col)
        *out_cursor_col = 0;

    if (max_chars <= 0 || g_search_query_len <= 0)
        return;

    if (g_search_query_len > max_chars) {
        start = g_search_cursor_pos - max_chars + 1;
        if (start < 0)
            start = 0;
        if (start > g_search_query_len - max_chars)
            start = g_search_query_len - max_chars;
    }

    take = g_search_query_len - start;
    if (take > max_chars)
        take = max_chars;
    if (take >= out_sz)
        take = out_sz - 1;
    if (take < 0)
        take = 0;

    if (take > 0)
        memcpy(out, g_search_query + start, (size_t)take);
    out[take] = '\0';

    if (out_cursor_col) {
        int col = g_search_cursor_pos - start;
        if (col < 0)
            col = 0;
        if (col > take)
            col = take;
        *out_cursor_col = col;
    }
}

static void render_code_panel_search_overlay(int cp_x, int panel_w, int panel_top) {
    char count_buf[32];
    char query_buf[128];
    int pad_x = 8;
    int box_h = LINE_H + 8;
    int box_w;
    int box_x;
    int box_y;
    int count_w;
    int label_w = 8 * FONT_W;
    int max_query_chars;
    int cursor_col = 0;
    int text_y;
    int count_x;
    int query_x;

    if (!g_search_active)
        return;

    if (g_search_query_len <= 0)
        snprintf(count_buf, sizeof(count_buf), "type");
    else if (g_search_match_count <= 0)
        snprintf(count_buf, sizeof(count_buf), "0 matches");
    else
        snprintf(count_buf, sizeof(count_buf), "%d/%d",
                 g_search_hit_ordinal, g_search_match_count);

    box_w = panel_w / 2;
    if (box_w < 220) box_w = 220;
    if (box_w > panel_w - 20) box_w = panel_w - 20;
    if (box_w < 120) box_w = panel_w - 8;

    box_x = cp_x + panel_w - box_w - 8;
    box_y = panel_top - CODE_MARGIN_Y - box_h + 2;
    text_y = box_y + 6;
    count_w = (int)strlen(count_buf) * FONT_W;
    count_x = box_x + box_w - pad_x - count_w;
    query_x = box_x + pad_x + label_w;
    max_query_chars = (count_x - query_x - pad_x) / FONT_W;
    code_panel_format_search_query(query_buf, sizeof(query_buf),
                                   max_query_chars, &cursor_col);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.08f, 0.09f, 0.16f, 0.94f);
    draw_quad((float)box_x, (float)box_y, (float)box_w, (float)box_h);
    glColor4f(0.48f, 0.56f, 0.82f, 0.85f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)box_x, (float)box_y);
    glVertex2f((float)(box_x + box_w), (float)box_y);
    glVertex2f((float)(box_x + box_w), (float)(box_y + box_h));
    glVertex2f((float)box_x, (float)(box_y + box_h));
    glEnd();

    glColor3f(0.62f, 0.70f, 0.88f);
    draw_string((float)(box_x + pad_x), (float)text_y, "Search:", FONT_MONO);
    glColor3f(0.96f, 0.96f, 0.92f);
    draw_string((float)query_x, (float)text_y, query_buf, FONT_MONO);
    glColor3f(0.62f, 0.70f, 0.88f);
    draw_string((float)count_x, (float)text_y, count_buf, FONT_MONO);

    if (g_cursor_on) {
        int cursor_x = query_x + cursor_col * FONT_W;
        glColor4f(0.95f, 0.80f, 0.24f, 0.85f);
        draw_quad((float)cursor_x, (float)(text_y - 2), 2.0f,
                  (float)(FONT_H + 2));
    }

    glDisable(GL_BLEND);
}

static void render_active_input_rows(int panel_w, int text_x, int idx_x,
                                     int visible_lines, int file_line,
                                     int indent_chars, const char *idx_text,
                                     int search_row_idx,
                                     int *io_cur, int *io_line_y) {
    CodeWrapIter wrap_it;
    int wrap_row = 0;
    int wrap_start, wrap_len, wrap_x;
    int input_x = text_x + indent_chars * FONT_W;
    int cursor_seg_start = 0;
    int cursor_seg_len = 0;
    int cursor_seg_x = input_x;
    int cursor_row = code_panel_cursor_row_for_text(g_input, input_x, panel_w,
                                                    g_cursor_pos,
                                                    &cursor_seg_start,
                                                    &cursor_seg_len,
                                                    &cursor_seg_x);
    int cursor_col = g_cursor_pos - cursor_seg_start;

    code_wrap_iter_init(&wrap_it, g_input, input_x, panel_w);
    while (code_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
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

void render_code_panel(void) {
    refresh_workspace_header_lines();
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;  /* y of the panel's top edge (OpenGL coords) */
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;
    int visible_lines = (cp_h - 2 * CODE_MARGIN_Y - 2 * LINE_H) / LINE_H;
    if (visible_lines < 1) visible_lines = 1;

    /* When cursor is on a vertex, find which normal/color lines feed it so
     * we can draw a gutter accent bar on them below. */
    int highlight_normal_idx = -1;
    int highlight_color_idx  = -1;
    if (!g_inserting && g_edit_line < g_num_cmds && g_cmds[g_edit_line].valid) {
        highlight_normal_idx = repl_find_feeding_normal_cmd(g_edit_line);
        highlight_color_idx  = repl_find_feeding_color_cmd(g_edit_line);
    }

    int header_rows = code_panel_header_row_count(panel_w, text_x);
    int footer_rows = code_panel_footer_row_count(panel_w, text_x);
    int total_lines = header_rows + footer_rows + code_panel_newline_rows(panel_w, text_x);
    for (int i = 0; i < g_num_cmds; i++) {
        if (g_inserting && i == g_edit_line)
            total_lines += code_panel_insert_rows(panel_w, text_x);
        total_lines += code_panel_command_main_rows(i, panel_w, text_x);
        total_lines += code_panel_replay_extra_rows_for_line(i);
    }

    int cursor_doc_line = header_rows;
    if (g_inserting) {
        for (int i = 0; i < g_edit_line && i < g_num_cmds; i++) {
            cursor_doc_line += code_panel_command_main_rows(i, panel_w, text_x);
            cursor_doc_line += code_panel_replay_extra_rows_for_line(i);
        }
        cursor_doc_line += code_panel_cursor_row_for_text(
            g_input, text_x + cmd_indent_chars(g_edit_line) * FONT_W,
            panel_w, g_cursor_pos, NULL, NULL, NULL);
    } else if (g_edit_line < g_num_cmds) {
        for (int i = 0; i < g_edit_line; i++) {
            cursor_doc_line += code_panel_command_main_rows(i, panel_w, text_x);
            cursor_doc_line += code_panel_replay_extra_rows_for_line(i);
        }
        cursor_doc_line += code_panel_cursor_row_for_text(
            g_input, text_x + cmd_indent_chars(g_edit_line) * FONT_W,
            panel_w, g_cursor_pos, NULL, NULL, NULL);
    } else {
        for (int i = 0; i < g_num_cmds; i++) {
            cursor_doc_line += code_panel_command_main_rows(i, panel_w, text_x);
            cursor_doc_line += code_panel_replay_extra_rows_for_line(i);
        }
        cursor_doc_line += code_panel_cursor_row_for_text(
            g_input, text_x + cmd_indent_chars(g_num_cmds) * FONT_W,
            panel_w, g_cursor_pos, NULL, NULL, NULL);
    }

    int follow_doc_line = cursor_doc_line;
    if (g_replay_active && g_replay_state != REPLAY_OFF &&
        g_replay_src_line >= 0 && g_replay_src_line < g_num_cmds &&
        g_cmds[g_replay_src_line].has_vars) {
        follow_doc_line = header_rows;
        for (int i = 0; i < g_replay_src_line; i++) {
            follow_doc_line += code_panel_command_main_rows(i, panel_w, text_x);
            follow_doc_line += code_panel_replay_extra_rows_for_line(i);
        }
        follow_doc_line += code_panel_command_main_rows(g_replay_src_line, panel_w, text_x);
        follow_doc_line += code_panel_replay_extra_rows_for_line(g_replay_src_line) - 1;
    }

    /* Clamp scroll */
    int max_scroll = total_lines - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    /* Only snap to cursor after an edit; manual scroll can stay off-cursor. */
    if (g_scroll_follow_cursor) {
        if (follow_doc_line < g_scroll)
            g_scroll = follow_doc_line;
        if (follow_doc_line >= g_scroll + visible_lines)
            g_scroll = follow_doc_line - visible_lines + 1;
        if (g_scroll > max_scroll) g_scroll = max_scroll;
        if (g_scroll < 0) g_scroll = 0;
        g_scroll_follow_cursor = 0;
    }

    begin_2d();

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.06f, 0.06f, 0.10f, 0.92f);
    draw_quad((float)cp_x, (float)cp_y, (float)cp_w, (float)cp_h);

    /* Border: divider between code panel and scene */
    glColor4f(0.30f, 0.30f, 0.50f, 0.80f);
    glBegin(GL_LINES);
    if (g_layout_vertical) {
        /* Horizontal divider along bottom of code panel */
        glVertex2f(0.0f, (float)cp_y);
        glVertex2f((float)g_win_w, (float)cp_y);
    } else {
        /* Vertical divider along right edge of code panel */
        glVertex2f((float)panel_w, 0.0f);
        glVertex2f((float)panel_w, (float)g_win_h);
    }
    glEnd();
    glDisable(GL_BLEND);

    /* Top info bar */
    {
        char info[256];
        int hover_btn = header_btn_hit(g_mouse_x, g_mouse_y);
        /* Accumulation AA indicator suffix */
        char aa_tag[24] = "";
        if (g_use_accum) {
            if (g_accum_aa_enabled && g_accum_samples > 1)
                snprintf(aa_tag, sizeof(aa_tag), " | AA:%dx", g_accum_samples);
            else
                snprintf(aa_tag, sizeof(aa_tag), " | AA:off");
        }
        /* Time variable indicator */
        if (g_inserting) {
            snprintf(info, sizeof(info),
                     "F1:Help | %d cmds | Ln %d [INSERT]%s",
                     g_num_cmds, g_edit_line + 1, aa_tag);
        } else if (in_begin_block()) {
            snprintf(info, sizeof(info),
                     "F1:Help | %d cmds | %s | Ln %d%s",
                     g_num_cmds, mode_name(current_begin_mode()),
                     g_edit_line + 1, aa_tag);
        } else {
            snprintf(info, sizeof(info),
                     "F1:Help | %d cmds | Ln %d%s",
                     g_num_cmds, g_edit_line + 1, aa_tag);
        }
        glColor3f(0.50f, 0.55f, 0.65f);
        draw_string(CODE_MARGIN_X, panel_top - CODE_MARGIN_Y - 2, info,
                    FONT_SMALL);

        /* Header button bar */
        {
            int bx[NUM_HEADER_BTNS], by, bh;
            header_btn_rects(bx, &by, NULL, &bh);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            for (int i = 0; i < NUM_HEADER_BTNS; i++) {
                int bw = (int)strlen(g_header_btn_labels[i]) * 8 + 12;
                /* Active state highlight for toggle buttons */
                int active = (i == 2 && g_show_config) ||
                             (i == 3 && g_replay_active);
                if (i == hover_btn)
                    glColor4f(0.20f, 0.38f, 0.60f, 0.95f);
                else if (active)
                    glColor4f(0.18f, 0.35f, 0.18f, 0.90f);
                else
                    glColor4f(0.14f, 0.20f, 0.30f, 0.88f);
                draw_quad((float)bx[i], (float)by, (float)bw, (float)bh);
                glColor4f(0.55f, 0.72f, 0.95f, 0.95f);
                glBegin(GL_LINE_LOOP);
                glVertex2f((float)bx[i],        (float)by);
                glVertex2f((float)(bx[i] + bw), (float)by);
                glVertex2f((float)(bx[i] + bw), (float)(by + bh));
                glVertex2f((float)bx[i],        (float)(by + bh));
                glEnd();
                glColor3f(0.88f, 0.93f, 1.0f);
                draw_string((float)(bx[i] + 6), (float)(by + 3),
                            g_header_btn_labels[i], FONT_SMALL);
            }
            glDisable(GL_BLEND);
        }
    }

    render_code_panel_search_overlay(cp_x, panel_w, panel_top);

    /* Code lines: row 0 = info bar, row 1 = button bar, row 2+ = code */
    int line_y = panel_top - CODE_MARGIN_Y - 3 * LINE_H;
    int cur = 0;
    int file_line = 1;

    /* Macro for rendering a static line (header/footer) */
    #define RENDER_STATIC_LINE(text, set_color) do {                           \
        CodeWrapIter wrap_it;                                                   \
        int wrap_row = 0;                                                       \
        int wrap_start, wrap_len, wrap_x;                                       \
        code_wrap_iter_init(&wrap_it, text, text_x, panel_w);                   \
        while (code_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {\
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
    /* gluLookAt lines (slightly brighter - dynamic) */
    for (int i = 0; i < LOOKAT_LINE_COUNT; i++) {
        RENDER_STATIC_LINE(g_lookat[i], glColor3f(0.50f, 0.45f, 0.55f));
    }
    /* Header post-lookAt */
    for (int i = 0; g_header_post[i]; i++) {
        RENDER_STATIC_LINE(g_header_post[i], glColor3f(0.38f, 0.38f, 0.42f));
    }

    /* Commands + insert line + new-line slot */
    int vnum = 0; /* vertex counter within current glBegin/glEnd block */
    int loop_depth = 0;
    int in_tess_poly = 0;
    int tess_depth = 0;
    int primitive_vnums_exact = 1;
    for (int i = 0; i <= g_num_cmds; i++) {
        /* If inserting, render the virtual insert line before command[g_edit_line] */
        if (g_inserting && i == g_edit_line) {
                        render_active_input_rows(panel_w, text_x, idx_x,
                                                                         visible_lines, file_line,
                                                                         cmd_indent_chars(i), NULL,
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
                                         cmd_indent_chars(i), idx_text,
                                         g_edit_line,
                                         &cur, &line_y);
                file_line++;
            } else {
                /* Existing command, not being edited */
                CodeWrapIter wrap_it;
                int wrap_row = 0;
                int wrap_start, wrap_len, wrap_x;
                int search_row_idx = repl_search_row_for_cmd_index(i);
                code_wrap_iter_init(&wrap_it, g_cmds[i].source, text_x, panel_w);
                while (code_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
                    if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                        if (g_replay_active && g_replay_state != REPLAY_OFF &&
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
                        }
                        color_for_type(g_cmds[i].type);
                        code_panel_draw_search_highlights(g_cmds[i].source,
                                                          search_row_idx,
                                                          wrap_start, wrap_len,
                                                          wrap_x, line_y);
                        code_panel_draw_segment(wrap_x, line_y, g_cmds[i].source,
                                                wrap_start, wrap_len, FONT_MONO);
                        line_y -= LINE_H;
                    }
                    cur++;
                    wrap_row++;
                }
                file_line++;

                /* Replay annotation: variable-substituted and evaluated */
                if (g_replay_active && g_replay_state != REPLAY_OFF &&
                    g_replay_src_line >= 0 && i == g_replay_src_line &&
                    g_cmds[i].has_vars) {
                    int flat_idx = find_replay_flat_cmd(i);
                    if (flat_idx >= 0) {
                        /* Annotation 1: variable-substituted */
                        char subst[MAX_LINE_LEN], var_comment[128];
                        const FlatCmdLocalVars *lcvars =
                            &g_flat_cmd_local_vars[flat_idx];
                        int nsubst = subst_predef_vars(
                            g_cmds[i].source, subst, sizeof(subst),
                            var_comment, sizeof(var_comment),
                            lcvars->vars, lcvars->num_vars);
                        if (nsubst > 0) {
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
                                    draw_string(
                                        (float)(text_x + sw),
                                        (float)line_y,
                                        var_comment, FONT_MONO);
                                }
                                line_y -= LINE_H;
                            }
                            cur++;
                        }

                        /* Annotation 2: fully evaluated */
                        char eval_buf[MAX_LINE_LEN];
                        if (format_evaluated_cmd(
                                &g_flat_cmds[flat_idx],
                                g_cmds[i].source,
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
        } else {
            /* i == g_num_cmds: new-line slot */
            int is_edit_nl = (!g_inserting && g_edit_line == g_num_cmds);
            if (is_edit_nl) {
                render_active_input_rows(panel_w, text_x, idx_x,
                                         visible_lines, file_line,
                                         cmd_indent_chars(g_num_cmds), NULL,
                                         g_edit_line,
                                         &cur, &line_y);
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

    #undef RENDER_STATIC_LINE

    /* Scroll indicator */
    if (total_lines > visible_lines) {
        int bar_h = cp_h - 2 * CODE_MARGIN_Y - 2 * LINE_H;
        float frac = (float)visible_lines / (float)total_lines;
        float pos  = (float)g_scroll / (float)total_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = panel_top - CODE_MARGIN_Y - 2 * LINE_H
                      - (int)(bar_h * pos) - thumb_h;

        glEnable(GL_BLEND);
        glColor4f(0.50f, 0.50f, 0.65f, 0.35f);
        draw_quad((float)(cp_x + cp_w - 6), (float)thumb_y,
                  5.0f, (float)thumb_h);
        glDisable(GL_BLEND);
    }

    /* Status bar: along the bottom edge of the code panel */
    if (g_status_ttl > 0) {
        float alpha = g_status_ttl > 60 ? 1.0f : (float)g_status_ttl / 60.0f;
        glEnable(GL_BLEND);
        glColor4f(0.12f, 0.12f, 0.05f, 0.92f * alpha);
        draw_quad((float)cp_x, (float)cp_y, (float)cp_w, (float)(LINE_H + 6));
        glColor4f(1.0f, 0.85f, 0.20f, alpha);
        draw_string((float)(cp_x + CODE_MARGIN_X), (float)(cp_y + 5),
                    g_status, FONT_MONO);
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

void render_example_dropdown(void) {
    if (!g_example_dropdown_open) return;
    int n = repl_example_count();
    if (n == 0) { g_example_dropdown_open = 0; return; }

    /* Position: drops down from the Example button */
    int bx[NUM_HEADER_BTNS], by, bh;
    header_btn_rects(bx, &by, NULL, &bh);
    int dx = bx[1];

    /* Width: widest example name + padding */
    int max_w = 0;
    for (int i = 0; i < n; i++) {
        int w = (int)strlen(repl_example_name(i)) * FONT_W;
        if (w > max_w) max_w = w;
    }
    int dw = max_w + 20;
    int dh = n * LINE_H + 6;
    int dy = by - dh;   /* OpenGL Y: drops below the button row */

    /* Update hover from current mouse position */
    g_example_dropdown_hover = example_dropdown_item_hit(g_mouse_x, g_mouse_y);

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Background */
    glColor4f(0.08f, 0.10f, 0.18f, 0.96f);
    draw_quad((float)dx, (float)dy, (float)dw, (float)dh);

    /* Border */
    glColor4f(0.55f, 0.72f, 0.95f, 0.85f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)dx,        (float)dy);
    glVertex2f((float)(dx + dw), (float)dy);
    glVertex2f((float)(dx + dw), (float)(dy + dh));
    glVertex2f((float)dx,        (float)(dy + dh));
    glEnd();

    /* Entries (index 0 at top) */
    int ey = dy + dh - LINE_H + 1;
    for (int i = 0; i < n; i++) {
        if (i == g_example_dropdown_hover) {
            glColor4f(0.20f, 0.25f, 0.45f, 0.90f);
            draw_quad((float)(dx + 1), (float)(ey - 2),
                      (float)(dw - 2), (float)LINE_H);
            glColor3f(1.0f, 1.0f, 0.90f);
        } else if (i == g_example_idx) {
            glColor3f(0.60f, 1.0f, 0.60f);
        } else {
            glColor3f(0.65f, 0.65f, 0.72f);
        }
        draw_string((float)(dx + 8), (float)ey,
                    repl_example_name(i), FONT_MONO);
        ey -= LINE_H;
    }

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
        "  glNormal3f(x,y,z)    \tSpecify a vertex normal",
        "  glColor3f(r,g,b)     \tSpecify vertex color",
        "  glColor4f(r,g,b,a)   \tSpecify color with alpha",
        "  glTranslatef(x,y,z)  \tTranslate the modelview matrix",
        "  glScalef(sx,sy,sz)   \tScale the modelview matrix",
        "  glRotatef(d,x,y,z)   \tRotate the modelview matrix",
        "  glPushMatrix()       \tPush current matrix onto stack",
        "  glPopMatrix()        \tPop matrix from stack",
        "  glEnable(CAP)        \tGL_DEPTH_TEST, GL_LIGHTING, ...",
        "  glDisable(CAP)       \tGL_COLOR_MATERIAL, GL_NORMALIZE",
        "  glShadeModel(MODE)   \tGL_SMOOTH, GL_FLAT",
        "  glFrontFace(MODE)    \tGL_CW, GL_CCW",
        "  glPointSize(size)    \tRasterized point diameter",
        "  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)",
        "       \tDistance attenuation: size *= 1/sqrt(const + linear*d + quadratic*d*d)",
        "  glBlendFunc(sfactor, dfactor)",
        "       \tGL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA / GL_ONE",
        "",
        "Lighting / Material:",
        "  glColorMaterial(face, mode)",
        "       \tface: GL_FRONT..., mode: GL_DIFFUSE...",
        "  glMaterialf(face, pname, value | {r,g,b,a})",
        "  glLightModeli(pname, param)",
        "       \tGL_LIGHT_MODEL_TWO_SIDE, GL_LIGHT_MODEL_LOCAL_VIEWER",
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
        "Variables (predefined: x, y, z, i, j, k, n, t):",
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
        "Functions (func0..func9):",
        "  func0(radius, sides) {   \tDefine with parameters",
        "    for(i, 0, sides) {",
        "      glVertex3f(radius*cos(i*TAU/sides), ...)",
        "    }",
        "  }",
        "  func0(1.5, 6)            \tCall with args",
        "  Recursion works with if(...) guard",
        "",
        "Conditionals:",
        "  if(t > 1) {              \tBody runs when condition is true",
        "    glColor3f(1, 0, 0)",
        "  }",
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
        "  Auto-increments with elapsed time when playing.",
        "  Use in any expression: glVertex3f(sin(t), cos(t), 0)",
        "",
        "Accumulation Buffer AA:",
        "  On by default; launch with --noaccum to disable.",
        "  Status shown in info bar (AA:8x / AA:off).",
        "",
        NULL
    };

    /* --- Tab 1: Keys ---
     * Same '\t' convention: left column = key, right = action. */
    static const char *tab_keys[] = {
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
        "  Ctrl+R               \tReformat buffer",
        "  Ctrl+/               \tToggle comment on line",
        "  Ctrl+P               \tDump code to stdout",
        "  Ctrl+S               \tSave to output.c",
        "  Ctrl+Q               \tExit and save to temp file",
        "  Escape               \tClear input / close help",
        "",
        "Camera:",
        "  Left-drag            \tOrbit",
        "  Right-drag           \tPan",
        "  Scroll wheel         \tZoom (viewport) / Scroll (code panel)",
        "",
        "Time & Replay:",
        "  Ctrl+T               \tPlay / pause time variable",
        "  Ctrl+G               \tStart / stop replay",
        "  Space                \tPause / resume replay",
        "  + / -                \tChange replay speed",
        "  m                    \tToggle polygon / vertex replay mode",
        "  Left / Right         \tStep backward / forward (when paused)",
        "  Esc                  \tStop replay",
        "",
        "Render State:",
        "  Ctrl+B               \tToggle accumulation-buffer AA",
        "  Ctrl+=               \tIncrease jitter samples",
        "  Ctrl+-               \tDecrease jitter samples",
        "  Ctrl+U               \tToggle GL_MULTISAMPLE",
        "  Ctrl+N               \tToggle GL_LINE_SMOOTH",
        "",
        "Configuration:",
        "  `                    \tOpen configuration menu",
        "",
        "F-Key Toggles:",
        "  F1  \tHelp overlay       F2  Wireframe mode",
        "  F3  \tGrid theme         F4  Axes theme",
        "  F5  \tVertex numbers     F6  Normal vectors",
        "  F7  \tVertex outlines    F8  Vertex guides",
        "  `   \tVertex points (config menu toggle)",
        "  F9  \tAuto-normals       F10 Light indicators",
        "  F11 \tCamera rotate      F12 Cycle examples",
        "",
        "Navigation:",
        "  PgUp / PgDn          \tScroll code panel (or help)",
        "",
        NULL
    };

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
    int tab_bar_h = 28;
    int pad_top = 32 + tab_bar_h, pad_bot = 24;
    int content_h = hh - pad_top - pad_bot;
    int visible_lines = (content_h - FONT_H - 4) / LINE_H;
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

    /* --- Title --- */
    glColor3f(0.80f, 0.80f, 1.00f);
    {
        const char *title = "OpenGL REPL - Help";
        int title_x = hx + (hw - (int)strlen(title) * FONT_W) / 2;
        draw_string((float)title_x, (float)(hy + hh - 22), title, FONT_MONO);
    }

    /* --- Tab bar --- */
    {
        int tab_y = hy + hh - 32 - tab_bar_h;
        int tab_w = hw / HELP_NUM_TABS;

        for (int t = 0; t < HELP_NUM_TABS; t++) {
            int tx_tab = hx + t * tab_w;

            if (t == g_help_tab) {
                /* Active tab background */
                glColor4f(0.15f, 0.15f, 0.30f, 0.90f);
                draw_quad((float)tx_tab, (float)tab_y, (float)tab_w, (float)tab_bar_h);
                /* Active tab underline */
                glColor4f(0.55f, 0.65f, 1.00f, 0.90f);
                draw_quad((float)tx_tab, (float)tab_y, (float)tab_w, 2.0f);
                /* Active tab text */
                glColor3f(0.85f, 0.85f, 1.00f);
            } else {
                /* Inactive tab background */
                glColor4f(0.08f, 0.08f, 0.14f, 0.70f);
                draw_quad((float)tx_tab, (float)tab_y, (float)tab_w, (float)tab_bar_h);
                /* Inactive tab text */
                glColor3f(0.45f, 0.45f, 0.55f);
            }

            int lbl_len = (int)strlen(tab_labels[t]);
            int lbl_x = tx_tab + (tab_w - lbl_len * FONT_W) / 2;
            draw_string((float)lbl_x, (float)(tab_y + 8), tab_labels[t], FONT_MONO);
        }

        /* Separator line under tab bar */
        glColor4f(0.35f, 0.35f, 0.55f, 0.60f);
        glBegin(GL_LINES);
        glVertex2f((float)hx, (float)tab_y);
        glVertex2f((float)(hx + hw), (float)tab_y);
        glEnd();

        /* Tab switch hint */
        glColor4f(0.40f, 0.40f, 0.55f, 0.60f);
        const char *nav_hint = "Left/Right: switch tabs";
        int nh_x = hx + hw - (int)strlen(nav_hint) * 8 - 12;
        draw_string((float)nh_x, (float)(hy + hh - 22), nav_hint, FONT_SMALL);
    }

    /* --- Content --- */
    /* Scissor clip to content area */
    glEnable(GL_SCISSOR_TEST);
    glScissor(hx + 1, hy + pad_bot, hw - 2, content_h);

    int tx = hx + 24;
    /* Lower baseline so first line's glyphs don't clip at scissor top */
    int ty_start = hy + hh - pad_top - FONT_H - 4;

    for (int i = g_help_scroll; i < n_lines && i < g_help_scroll + visible_lines + 1; i++) {
        int ty = ty_start - (i - g_help_scroll) * LINE_H;
        if (ty < hy + pad_bot - LINE_H) break;
        if (text[i][0] == '\0') continue;

        /* '\t' marks the left/right column boundary */
        const char *tab = strchr(text[i], '\t');
        if (tab) {
            /* Left column (command / key) */
            char left[256];
            int ln = (int)(tab - text[i]);
            if (ln > 255) ln = 255;
            memcpy(left, text[i], ln);
            left[ln] = '\0';
            glColor3f(0.45f, 0.90f, 0.50f);
            draw_string((float)tx, (float)ty, left, FONT_MONO);

            /* Right column (description) */
            glColor3f(0.62f, 0.62f, 0.72f);
            draw_string((float)(tx + ln * FONT_W), (float)ty,
                        tab + 1, FONT_MONO);
        } else if (text[i][0] != ' ') {
            /* Section header */
            glColor3f(0.95f, 0.80f, 0.40f);
            draw_string((float)tx, (float)ty, text[i], FONT_MONO);
        } else if (text[i][2] == ' ' && text[i][3] == ' ') {
            /* 4+ space indent — code example */
            glColor3f(0.45f, 0.68f, 0.78f);
            draw_string((float)tx, (float)ty, text[i], FONT_MONO);
        } else {
            /* 2-space indent, no split — uniform green */
            glColor3f(0.45f, 0.90f, 0.50f);
            draw_string((float)tx, (float)ty, text[i], FONT_MONO);
        }
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

    #undef HELP_NUM_TABS
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
    int sc_x, sc_y, sc_w, sc_h;
    scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    *pw = VAR_PANEL_W;
    *ph = VAR_TITLE_H + g_num_predef_vars * VAR_ROW_H + 2 * VAR_PANEL_PAD;
    *px = sc_x + sc_w - *pw - 8;
    if (*px < sc_x + 4) *px = sc_x + 4;
    *py = sc_y + 8;
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
    int sc_x, sc_y, sc_w, sc_h;
    scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    *pw = CFG_PANEL_W;
    *ph = CFG_TITLE_H + CFG_ITEM_COUNT * CFG_ROW_H + 2 * CFG_PAD;
    *px = sc_x + (sc_w - *pw) / 2;
    if (*px < sc_x + 4) *px = sc_x + 4;
    *py = sc_y + (sc_h - *ph) / 2;
    if (*py < sc_y + 4) *py = sc_y + 4;
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

static int g_code_panel_drag_active = 0;
static int g_code_panel_drag_anchor = -1;
static int g_code_panel_drag_moved = 0;

static int code_panel_hit_test(int mx, int my,
                               int *out_target,
                               int *out_on_insert_line,
                               int *out_row_offset) {
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;
    /* Convert GLUT Y (top=0) to OpenGL Y (bottom=0) */
    int gl_y = g_win_h - my;
    if (mx < cp_x || mx >= cp_x + cp_w) return 0;
    if (gl_y < cp_y || gl_y >= cp_y + cp_h) return 0;

    /* Same layout constants as render_code_panel */
    int line_y_start = panel_top - CODE_MARGIN_Y - 3 * LINE_H;
    int vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;
    if (vis < 0) return 0;   /* clicked in header */

    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = g_scroll + vis;
    int target;
    int on_insert_line;
    int row_offset;

    if (!code_panel_target_for_doc_line(doc_line, panel_w, text_x,
                                        &target, &on_insert_line,
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
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;
    int gl_y = g_win_h - my;
    int line_y_start = panel_top - CODE_MARGIN_Y - 3 * LINE_H;
    int vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;

    int visible_lines = (cp_h - CODE_MARGIN_Y - 2 * LINE_H - CODE_MARGIN_Y) / LINE_H;
    if (visible_lines < 1) visible_lines = 1;
    if (vis < 0) vis = 0;
    if (vis >= visible_lines) vis = visible_lines - 1;

    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = g_scroll + vis;
    int target;
    int on_insert_line;

    if (!code_panel_target_for_doc_line(doc_line, panel_w, text_x,
                                        &target, &on_insert_line, NULL))
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

    int panel_w = (int)(g_win_w * g_panel_frac);
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int edit_idx = on_insert_line ? g_edit_line : target;
    int indent_chars = cmd_indent_chars(
        edit_idx < g_num_cmds ? edit_idx : g_num_cmds);
    int seg_start = g_input_len;
    int seg_len = 0;
    int seg_x = text_x + indent_chars * FONT_W;
    int col;

    code_panel_segment_for_row(g_input, seg_x, panel_w, row_offset,
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
    /* Check example dropdown first (it floats over code) */
    if (g_example_dropdown_open) {
        int item = example_dropdown_item_hit(mx, my);
        if (item >= 0) {
            repl_load_example(item);
            g_example_dropdown_open = 0;
            return 1;
        }
        g_example_dropdown_open = 0;
        /* fall through so clicks still navigate code */
    }

    int btn = header_btn_hit(mx, my);
    if (btn >= 0) {
        switch (btn) {
        case 0: repl_save_default_output(); break;
        case 1: g_example_dropdown_open ^= 1; break;
        case 2: g_show_config ^= 1; break;
        case 3:
            if (g_replay_active) replay_stop();
            else                 replay_start();
            break;
        }
        return 1;
    }

    int target, on_insert_line;
    if (!code_panel_hit_test(mx, my, &target, &on_insert_line, NULL)) return 0;

    handle_code_panel_click(mx, my);

    g_code_panel_drag_active = 0;
    g_code_panel_drag_anchor = -1;
    g_code_panel_drag_moved = 0;
    if (!on_insert_line && target >= 0 && target < g_num_cmds) {
        g_code_panel_drag_active = 1;
        g_code_panel_drag_anchor = target;
    }
    return 1;
}

int handle_code_panel_drag(int mx, int my) {
    int target;
    if (!g_code_panel_drag_active || g_code_panel_drag_anchor < 0) return 0;
    if (!code_panel_drag_target(mx, my, &target)) return 0;

    if (target != g_code_panel_drag_anchor || g_code_panel_drag_moved) {
        g_code_panel_drag_moved = 1;
        g_sel_anchor = g_code_panel_drag_anchor;
        g_sel_end = target;
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
