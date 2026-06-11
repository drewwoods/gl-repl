/*
 * src/repl/core.c - Residual REPL helpers awaiting redistribution.
 *
 * This translation unit is being dissolved into its natural owners.
 * What still lives here (per the R10 plan in ARCHITECTURE.md):
 *
 *   - repl_parse_and_normalize() / parse_and_normalize_impl() — until the
 *     parser absorbs them.
 *   - repl_reformat_program() — pending extraction to repl_reformat.c.
 *   - load_initial_commands() and a handful of startup helpers — pending move
 *     to src/repl/scenes.c.
 *   - current_begin_mode() / count_vertices() — pending move to
 *     src/repl/executor.c.
 *
 * For the live module map see MODULES.md. Editor input dispatch lives in
 * src/editor/input.c; cross-subsystem routing lives in glr_ctrl.c; commit
 * handlers live in src/editor/commit.c. The deleted repl_editor.{c,h} and
 * repl_commit.{c,h} are hard-guarded against return.
 */

#include "repl/core.h"
#include "repl/format.h"
#include "config.h" /* REPL_STATUS_TEXT_MAX */
#include "source_document.h"
#include "support/cpuprof.h"
#include "repl/command_spec.h"
#include "repl/command_store.h"
#include "repl/core_internal.h"
#include "repl/eval.h"
#include "repl/export.h"
#include "repl/flatten.h"
#include "repl/parser.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"

#include <sys/stat.h>
#include <sys/types.h>

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

static const char *outfile = "output.c";

/* ========================================================================= */
/* Global state                                                               */
/* ========================================================================= */

void repl_mark_source_dirty(void) {
    repl_state_mark_source_dirty();
}

/* Predefined variables - defined in src/repl/eval.c */

/* (no display list - commands are executed directly each frame) */

/* Forward declarations (eval_expr, parse_for_header, etc. are in src/repl/eval.h) */
static void get_for_var_name_from_text(const char *text, char *var, int var_sz);

/* ========================================================================= */
/* Utility                                                                    */
/* ========================================================================= */

/* Host-effect bridge: a single file-static pointer the controller
 * installs at startup, mirroring export.c's g_export_cfg_bridge /
 * g_export_camera_bridge. NULL bridge = every dispatch no-ops, which
 * is what tests want when they leave it unset. The standalone demo installs
 * only edit-line hooks, clearing the ui_state_status_set / editor / tutorial
 * stubs from tools/repl_demo/stubs.c.
 *
 * These started as individual callbacks; the six installers were consolidated into
 * one struct per plans/partial/src-repl-simplicity-review.md item 2.
 *
 * The status callback's long-term preferred shape is still per-function
 * out-params (err_buf / ReplDiagnostic) on each pipeline entry point.
 * The callback is the "or a callback" branch the decouple plan allows,
 * chosen because the alternative is 48+ test call-site updates for
 * high-traffic public APIs (export_save_output, save_workspace, etc.).
 * Bundling it in the bridge doesn't block that migration (introduced in
 * step 3 of feature/decouple-repl-from-gl-repl-alt.md). */
static const ReplHostEffects *g_host_effects = NULL;

void repl_install_host_effects(const ReplHostEffects *effects) {
    g_host_effects = effects;
}

void repl_set_status(const char *msg) {
    if (g_host_effects && g_host_effects->status && msg && msg[0])
        g_host_effects->status(msg);
}

void repl_set_status_error(const char *msg) {
    if (!g_host_effects || !msg || !msg[0])
        return;
    if (g_host_effects->status_error)
        g_host_effects->status_error(msg);
    else if (g_host_effects->status)
        g_host_effects->status(msg);
}

void repl_dispatch_example_presentation_reset(unsigned int tag_mask) {
    if (g_host_effects && g_host_effects->example_presentation_reset)
        g_host_effects->example_presentation_reset(tag_mask);
}

void repl_dispatch_input_reset(void) {
    if (g_host_effects && g_host_effects->input_reset)
        g_host_effects->input_reset();
}

void repl_dispatch_insert_mode_off(void) {
    if (g_host_effects && g_host_effects->insert_mode_off)
        g_host_effects->insert_mode_off();
}

void repl_dispatch_scroll_to_line(int target) {
    if (g_host_effects && g_host_effects->scroll_to_line)
        g_host_effects->scroll_to_line(target);
}

void repl_dispatch_tutorial_teardown(void) {
    if (g_host_effects && g_host_effects->tutorial_teardown)
        g_host_effects->tutorial_teardown();
}

int repl_dispatch_edit_line_get(void) {
    if (g_host_effects && g_host_effects->edit_line_get)
        return g_host_effects->edit_line_get();
    return 0;
}

void repl_dispatch_edit_line_set(int line) {
    if (g_host_effects && g_host_effects->edit_line_set)
        g_host_effects->edit_line_set(line);
}

void repl_dispatch_host_cursor_park(int line, int insert_mode) {
    if (g_host_effects && g_host_effects->host_cursor_park)
        g_host_effects->host_cursor_park(line, insert_mode);
}

void repl_dispatch_completion_clear(void) {
    if (g_host_effects && g_host_effects->completion_clear)
        g_host_effects->completion_clear();
}

void repl_dispatch_completion_update(void) {
    if (g_host_effects && g_host_effects->completion_update)
        g_host_effects->completion_update();
}

const char *repl_dispatch_host_input_get(void) {
    if (g_host_effects && g_host_effects->host_input_get)
        return g_host_effects->host_input_get();
    return "";
}

void repl_dispatch_set_time_playing(int playing) {
    if (g_host_effects && g_host_effects->set_time_playing)
        g_host_effects->set_time_playing(playing);
}

const char *repl_mode_name(GLenum mode) {
    return repl_begin_mode_name(mode);
}

GLenum repl_current_begin_mode(void) {
    GLenum mode = GL_TRIANGLES;
    const GLCmd *document_cmds = repl_state_document_cmds();
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++)
        if (document_cmds[cmd_idx].valid && document_cmds[cmd_idx].type == CMD_BEGIN)
            mode = (GLenum)document_cmds[cmd_idx].args[0];
    return mode;
}

int repl_count_vertices(void) {
    int n = 0;
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *g_flat_cmds = flat_program.cmds;
    int g_num_flat_cmds = flat_program.cmd_count;

    for (int flat_idx = 0; flat_idx < g_num_flat_cmds; flat_idx++)
        if (g_flat_cmds[flat_idx].valid &&
            repl_cmd_emits_vertex(g_flat_cmds[flat_idx].type)) n++;
    return n;
}

void repl_normalize_from_parsed(const char *parsed_source,
                                const char *raw_expr,
                                int ensure_semicolon,
                                char *out, int out_sz) {
    if (out_sz <= 0) return;
    char tmp[MAX_LINE_LEN];
    repl_format_reindent_from_parsed(parsed_source, raw_expr, tmp, sizeof(tmp));

    int len = (int)strlen(tmp);
    while (len > 0 && isspace((unsigned char)tmp[len - 1]))
        tmp[--len] = '\0';

    if (ensure_semicolon && len > 0) {
        char last = tmp[len - 1];
        if (last != ';' && last != ':' && last != '{' && last != '}') {
            if (len < (int)sizeof(tmp) - 1) {
                tmp[len++] = ';';
                tmp[len] = '\0';
            }
        }
    }

    strncpy(out, tmp, (size_t)out_sz - 1);
    out[out_sz - 1] = '\0';
}

const char *cmd_type_name(CmdType t) {
    return repl_cmd_type_name(t);
}

/* Strip leading/trailing whitespace from `raw_expr`, normalize comma
 * spacing (remove space before comma, ensure one space after), optionally
 * append a semicolon, and prepend `indent_spaces` spaces.  Used by
 * repl_parse_and_normalize() and repl_reformat_program() to produce
 * canonical source text for a command. */
static void normalize_with_indent(const char *raw_expr, int indent_spaces,
                                  int ensure_semicolon, char *out, int out_sz) {
    if (out_sz <= 0) return;

    const char *p = raw_expr;
    while (*p == ' ' || *p == '\t') p++;

    char body[MAX_LINE_LEN];
    size_t body_len = strlen(p);
    if (body_len >= sizeof(body))
        body_len = sizeof(body) - 1;
    memcpy(body, p, body_len);
    body[body_len] = '\0';

    /* Split off a trailing `// ...` so the ';'-normalization and comma
     * spacing below act on the code only; the comment is re-appended from
     * the original line at the end (otherwise the re-added ';' would land
     * after the comment as `... // c;`). */
    {
        const char *bc = repl_line_trailing_comment(body);
        if (bc) body[bc - body] = '\0';
    }

    int len = (int)strlen(body);
    while (len > 0 && isspace((unsigned char)body[len - 1]))
        body[--len] = '\0';
    while (ensure_semicolon && len > 0 && body[len - 1] == ';')
        body[--len] = '\0';
    while (len > 0 && isspace((unsigned char)body[len - 1]))
        body[--len] = '\0';
    if (ensure_semicolon && len > 0 && len < (int)sizeof(body) - 1) {
        body[len++] = ';';
        body[len] = '\0';
    }

    /* Keep expression tokens but normalize comma delimiters for readability. */
    {
        char spaced[MAX_LINE_LEN];
        int si = 0;
        for (int char_idx = 0; body[char_idx] && si < (int)sizeof(spaced) - 1; char_idx++) {
            char c = body[char_idx];
            if (c == ',') {
                while (si > 0 && isspace((unsigned char)spaced[si - 1]))
                    si--;
                spaced[si++] = ',';
                if (si < (int)sizeof(spaced) - 1)
                    spaced[si++] = ' ';
                while (body[char_idx + 1] && isspace((unsigned char)body[char_idx + 1]))
                    char_idx++;
                continue;
            }
            spaced[si++] = c;
        }
        spaced[si] = '\0';
        memcpy(body, spaced, (size_t)si + 1);
    }

    /* Re-attach the original line's trailing comment after the normalized
     * code (a single space separator). No-op if there was none. */
    repl_append_trailing_comment(body, sizeof(body), raw_expr);

    if (indent_spaces < 0) indent_spaces = 0;
    if (indent_spaces > out_sz - 1) indent_spaces = out_sz - 1;
    memset(out, ' ', (size_t)indent_spaces);
    size_t body_copy_len = strlen(body);
    size_t body_cap = (size_t)(out_sz - 1 - indent_spaces);
    if (body_copy_len > body_cap)
        body_copy_len = body_cap;
    memcpy(out + indent_spaces, body, body_copy_len);
    out[indent_spaces + (int)body_copy_len] = '\0';
}

static int repl_core_append_text(char *dst, int dst_sz, int *off,
                                 const char *text) {
    size_t len;

    if (!dst || dst_sz <= 0 || !off || !text)
        return 0;
    if (*off < 0 || *off >= dst_sz)
        return 0;

    len = strlen(text);
    if ((size_t)*off + len >= (size_t)dst_sz)
        return 0;

    memcpy(dst + *off, text, len);
    *off += (int)len;
    dst[*off] = '\0';
    return 1;
}

static int repl_core_append_span(char *dst, int dst_sz, int *off,
                                 const char *start, const char *end) {
    size_t len;

    if (!dst || dst_sz <= 0 || !off || !start || !end || end < start)
        return 0;
    if (*off < 0 || *off >= dst_sz)
        return 0;

    len = (size_t)(end - start);
    if ((size_t)*off + len >= (size_t)dst_sz)
        return 0;

    memcpy(dst + *off, start, len);
    *off += (int)len;
    dst[*off] = '\0';
    return 1;
}

/* Re-emit a `float a = expr, b;` declaration line as canonical text:
 * `<indent>static float a = expr, b;` with single-space comma joins,
 * initializer expressions preserved verbatim, and any trailing `// ...`
 * comment re-attached. One left-to-right scan over the name list; every
 * branch is either a token step or a reject — returns 0 on anything that
 * isn't a well-formed decl (caller falls back to rebuilding the text from
 * the parsed payload.decl names, dropping initializer text). */
static int repl_core_format_var_decl_text(const char *orig_text,
                                          const char *indent,
                                          char *out, int out_sz) {
    char buf[MAX_LINE_LEN] = "";
    const char *p = orig_text ? orig_text : "";
    int decl_count = 0;
    int off = 0;

    while (*p && isspace((unsigned char)*p)) p++;
    /* Optional canonical `static ` prefix (see format_decl_text). */
    if (strncmp(p, "static", 6) == 0 && isspace((unsigned char)p[6])) {
        p += 6;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    if (strncmp(p, "float", 5) != 0)
        return 0;
    if (isalnum((unsigned char)p[5]) || p[5] == '_')
        return 0;
    p += 5;

    if (!repl_core_append_text(buf, sizeof(buf), &off, indent ? indent : ""))
        return 0;
    if (!repl_core_append_text(buf, sizeof(buf), &off, "static float "))
        return 0;

    while (*p) {
        const char *name_start;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ';' || (*p == '/' && p[1] == '/'))
            break;

        if (decl_count > 0) {
            if (*p != ',')
                return 0;
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            if (!repl_core_append_text(buf, sizeof(buf), &off, ", "))
                return 0;
        }

        if (!isalpha((unsigned char)*p) && *p != '_')
            return 0;
        name_start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        if (!repl_core_append_span(buf, sizeof(buf), &off, name_start, p))
            return 0;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '=' && p[1] != '=') {
            const char *expr_start;
            const char *expr_end;
            int depth = 0;

            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            expr_start = p;
            while (*p) {
                if (*p == '(') {
                    depth++;
                } else if (*p == ')') {
                    if (depth > 0)
                        depth--;
                } else if (depth == 0 && (*p == ',' || *p == ';')) {
                    break;
                } else if (depth == 0 && *p == '/' && p[1] == '/') {
                    break;
                }
                p++;
            }
            expr_end = p;
            while (expr_end > expr_start &&
                   isspace((unsigned char)expr_end[-1]))
                expr_end--;
            if (expr_end == expr_start)
                return 0;
            if (!repl_core_append_text(buf, sizeof(buf), &off, " = ") ||
                !repl_core_append_span(buf, sizeof(buf), &off,
                                       expr_start, expr_end))
                return 0;
        }

        decl_count++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ';' || (*p == '/' && p[1] == '/'))
            break;
    }

    if (decl_count == 0)
        return 0;

    if (*p == ';')
        p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0' && !(p[0] == '/' && p[1] == '/'))
        return 0;

    if (!repl_core_append_text(buf, sizeof(buf), &off, ";"))
        return 0;
    if (p[0] == '/' && p[1] == '/') {
        const char *comment_end = p + strlen(p);
        while (comment_end > p && isspace((unsigned char)comment_end[-1]))
            comment_end--;
        if (!repl_core_append_text(buf, sizeof(buf), &off, " ") ||
            !repl_core_append_span(buf, sizeof(buf), &off, p, comment_end))
            return 0;
    }

    if (!out || out_sz <= 0)
        return 0;
    snprintf(out, (size_t)out_sz, "%s", buf);
    return 1;
}

static int parse_and_normalize_impl(const char *line, int pos,
                                    ExprVar *vars, int num_vars,
                                    int preserve_expr, GLCmd *out_cmd,
                                    char *text_out, int text_sz,
                                    int strict_refs) {
    /* repl_parse_and_normalize is called from many sites — commit
     * paths, reformatter, tests. The parser never calls set_status
     * itself (it writes diagnostics into ctx->err_buf; enforced by
     * the check-no-set-status-in-repl-parser build guard). This
     * routine owns the scratch err_buf and surfaces the message to
     * the status bar on failure for callers that want it. */
    char normalize_parse_err[REPL_STATUS_TEXT_MAX];
    normalize_parse_err[0] = '\0';
    ReplParseContext parse_ctx = {
        .source_line_idx = pos,
        .vars = vars, .num_vars = num_vars,
        .strict_refs = strict_refs,
        .err_buf = normalize_parse_err,
        .err_sz  = (int)sizeof(normalize_parse_err),
    };
    ReplParsedLine pl;
    int parsed = repl_parser_parse_command_ctx(line, &pl, &parse_ctx);
    if (!parsed && normalize_parse_err[0])
        repl_set_status_error(normalize_parse_err);

    if (!parsed) return 0;
    *out_cmd = pl.cmd;
    if (preserve_expr) {
        /* pl.text holds the canonical (indented) form; measure its indent */
        int parsed_indent = 0;
        while (pl.text[parsed_indent] == ' ' || pl.text[parsed_indent] == '\t')
            parsed_indent++;

        if (text_out && text_sz > 0)
            normalize_with_indent(line, parsed_indent,
                                  repl_cmd_type_needs_semicolon(out_cmd->type),
                                  text_out, text_sz);
        out_cmd->has_vars = 1;
    } else {
        if (text_out && text_sz > 0) {
            /* pl.text already carries any trailing `// ...` comment —
             * repl_parser_parse_command_ctx re-attaches it to the
             * canonical text, so copying pl.text preserves it. */
            int n = (int)strlen(pl.text);
            if (n >= text_sz) n = text_sz - 1;
            memcpy(text_out, pl.text, (size_t)n);
            text_out[n] = '\0';
        }
    }
    return 1;
}

int repl_parse_and_normalize(const char *line, int pos,
                             ExprVar *vars, int num_vars,
                             int preserve_expr, GLCmd *out_cmd,
                             char *text_out, int text_sz) {
    return parse_and_normalize_impl(line, pos, vars, num_vars,
                                    preserve_expr, out_cmd,
                                    text_out, text_sz, 0);
}

int repl_parse_and_normalize_strict(const char *line, int pos,
                                    ExprVar *vars, int num_vars,
                                    int preserve_expr, GLCmd *out_cmd,
                                    char *text_out, int text_sz) {
    return parse_and_normalize_impl(line, pos, vars, num_vars,
                                    preserve_expr, out_cmd,
                                    text_out, text_sz, 1);
}

static void repl_core_replace_formatted_cmd(ReplCommandStore *store,
                                            int cmd_idx,
                                            const GLCmd *cmd,
                                            const char *text) {
    if (repl_command_store_replace_one(store, cmd_idx, cmd))
        source_document_replace_line(cmd_idx, text);
}

/* The Ctrl+\ whole-document reformatter: re-derive canonical text for
 * every valid source command and write it back through the command
 * store. One independent case per command family — block heads re-emit
 * their header from parsed args (or the original expression text when
 * has_vars), block ends re-align to their opening line's indent, and
 * the default arm round-trips plain GL commands through
 * repl_parse_and_normalize with the line's visible loop/param vars. A
 * case that can't reconstruct the line leaves it untouched rather than
 * guessing. */
void repl_reformat_program(void) {
    prof_begin(PROF_REFORMAT);
    ReplCommandStore store = repl_command_store_live();
    const GLCmd *document_cmds = repl_state_document_cmds();
    /* Source text reads route through a SourceTextView so the
     * dependency is declared at function scope rather than via a
     * scattered global accessor. Phase D will replace this entry-time
     * fetch with a view threaded from the controller. */
    SourceTextView text = source_document_view();

    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!document_cmds[cmd_idx].valid) continue;

        GLCmd orig = document_cmds[cmd_idx];
        GLCmd fmt = orig;
        /* *3 allows room for indentation, the canonicalized command text,
         * and trailing comments without snprintf truncation warnings. */
        char fmt_text[MAX_LINE_LEN * 3] = "";

        /* Canonical text for this command lives in the editor buffer. */
        const char *orig_text = source_text_line(text, cmd_idx);
        if (!orig_text) orig_text = "";

        char ind_s[REPL_INDENT_TEXT_MAX];
        repl_source_scope_cmd_indent(cmd_idx, ind_s, sizeof(ind_s));

        switch (orig.type) {
        case CMD_FOR_BEGIN: {
            char var[16] = "";
            char args[128] = "";
            if (!extract_for_args_text(orig_text, var, sizeof(var), args, sizeof(args)))
                get_for_var_name_from_text(orig_text, var, sizeof(var));
            if (!var[0]) strncpy(var, "i", sizeof(var) - 1);

            if (orig.has_vars && args[0]) {
                snprintf(fmt_text, sizeof(fmt_text), "%sfor(%s, %s) {", ind_s, var, args);
                fmt.has_vars = 1;
            } else if (orig.args[2] != 1.0f) {
                char start_buf[32];
                char end_buf[32];
                char step_buf[32];
                repl_format_source_float(start_buf, sizeof(start_buf), orig.args[0]);
                repl_format_source_float(end_buf, sizeof(end_buf), orig.args[1]);
                repl_format_source_float(step_buf, sizeof(step_buf), orig.args[2]);
                snprintf(fmt_text, sizeof(fmt_text), "%sfor(%s, %s, %s, %s) {",
                         ind_s, var, start_buf, end_buf, step_buf);
            } else {
                char start_buf[32];
                char end_buf[32];
                repl_format_source_float(start_buf, sizeof(start_buf), orig.args[0]);
                repl_format_source_float(end_buf, sizeof(end_buf), orig.args[1]);
                snprintf(fmt_text, sizeof(fmt_text), "%sfor(%s, %s, %s) {",
                         ind_s, var, start_buf, end_buf);
            }
            repl_core_replace_formatted_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_FOR_END:
        case CMD_FUNC_END:
        case CMD_IF_END: {
            int close_ind;
            char close_s[32];
            repl_source_scope_cmd_indent(cmd_idx, close_s, sizeof(close_s));
            close_ind = (int)strlen(close_s);
            if (close_ind >= 2)
                close_ind -= 2;
            else
                close_ind = 0;
            if (close_ind > (int)sizeof(close_s) - 1) close_ind = (int)sizeof(close_s) - 1;
            memset(close_s, ' ', (size_t)close_ind);
            close_s[close_ind] = '\0';
            snprintf(fmt_text, sizeof(fmt_text), "%s}", close_s);
            repl_core_replace_formatted_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_FUNC_DEF: {
            int fn = (int)orig.args[0];
            int parsed_fn = fn;
            int param_count = 0;
            char param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
            if (parse_repl_func_signature(orig_text, &parsed_fn,
                                          param_names, MAX_EXPR_VARS,
                                          &param_count))
                format_func_header(fmt_text, sizeof(fmt_text), ind_s,
                                   parsed_fn, param_names, param_count);
            else
                snprintf(fmt_text, sizeof(fmt_text), "%sfunc%d {", ind_s, fn);
            repl_core_replace_formatted_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_IF_BEGIN: {
            char cond[MAX_LINE_LEN] = "";
            if (!repl_extract_paren_payload(orig_text, cond, sizeof(cond)))
                snprintf(cond, sizeof(cond), "%g", orig.args[0]);
            snprintf(fmt_text, sizeof(fmt_text), "%sif(%s) {", ind_s, cond);
            repl_core_replace_formatted_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_VAR_ASSIGN: {
            const char *name = NULL;
            char rhs[MAX_LINE_LEN] = "";
            if (orig.var_idx >= 0 && orig.var_idx < g_num_predef_vars)
                name = g_predef_vars[orig.var_idx].name;
            char fallback[16] = "";
            if (!name) {
                const char *p = orig_text;
                while (*p && isspace((unsigned char)*p)) p++;
                int n = 0;
                while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
                       n < (int)sizeof(fallback) - 1)
                    fallback[n++] = *p++;
                fallback[n] = '\0';
                if (fallback[0]) name = fallback;
            }
            repl_extract_assignment_parts(orig_text, NULL, 0, rhs, sizeof(rhs));
            {
                char comment[MAX_LINE_LEN] = "";
                const char *cp = strstr(orig_text, "//");
                if (cp) snprintf(comment, sizeof(comment), " %s", cp);
                if (name && rhs[0])
                    snprintf(fmt_text, sizeof(fmt_text), "%s%s = %s;%s", ind_s, name, rhs, comment);
                else if (name)
                    snprintf(fmt_text, sizeof(fmt_text), "%s%s = %g;%s", ind_s, name, orig.args[0], comment);
            }
            repl_core_replace_formatted_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_COMMENT: {
            const char *p = orig_text;
            while (*p && isspace((unsigned char)*p)) p++;
            if (p[0] == '/' && p[1] == '/') {
                char suffix[MAX_LINE_LEN];
                p += 2;
                strncpy(suffix, p, sizeof(suffix) - 1);
                suffix[sizeof(suffix) - 1] = '\0';
                int suffix_len = (int)strlen(suffix);
                while (suffix_len > 0 &&
                       isspace((unsigned char)suffix[suffix_len - 1]))
                    suffix[--suffix_len] = '\0';
                snprintf(fmt_text, sizeof(fmt_text), "%s//%s", ind_s, suffix);
            } else {
                snprintf(fmt_text, sizeof(fmt_text), "%s//", ind_s);
            }
            repl_core_replace_formatted_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_VAR_DECLARE: {
            if (!repl_core_format_var_decl_text(orig_text, ind_s,
                                                fmt_text, sizeof(fmt_text))) {
                int off = snprintf(fmt_text, sizeof(fmt_text), "%sfloat ", ind_s);
                for (int decl_idx = 0;
                     decl_idx < orig.payload.decl.count && off < (int)sizeof(fmt_text) - 4;
                     decl_idx++) {
                    if (decl_idx > 0)
                        off += snprintf(fmt_text + off, sizeof(fmt_text) - off, ", ");
                    off += snprintf(fmt_text + off, sizeof(fmt_text) - off,
                                    "%s", orig.payload.decl.names[decl_idx]);
                }
                snprintf(fmt_text + off, sizeof(fmt_text) - off, ";");
            }
            repl_core_replace_formatted_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_GOTO_LABEL: {
            char label[REPL_GOTO_LABEL_MAX] = "";
            if (repl_extract_label_name(orig_text, label, sizeof(label)))
                snprintf(fmt_text, sizeof(fmt_text), "%s:", label);
            repl_core_replace_formatted_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_GOTO: {
            char label[REPL_GOTO_LABEL_MAX] = "";
            if (repl_extract_goto_label(orig_text, label, sizeof(label)))
                snprintf(fmt_text, sizeof(fmt_text), "%sgoto %s;", ind_s, label);
            repl_core_replace_formatted_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        default: {
            ExprVar vis_vars[MAX_EXPR_VARS];
            int num_vis_vars = collect_visible_vars(cmd_idx, vis_vars, MAX_EXPR_VARS, NULL);
            int preserve_expr = (num_vis_vars > 0) || orig.has_vars;
            GLCmd parsed;
            char parsed_text[MAX_LINE_LEN] = "";
            memset(&parsed, 0, sizeof(parsed));
            if (repl_parse_and_normalize(orig_text, cmd_idx,
                                         num_vis_vars > 0 ? vis_vars : NULL,
                                         num_vis_vars > 0 ? num_vis_vars : 0,
                                         preserve_expr, &parsed,
                                         parsed_text, sizeof(parsed_text)) &&
                parsed.type == orig.type) {
                parsed.is_auto = orig.is_auto;
                parsed.src_cmd_idx = orig.src_cmd_idx;
                if (!preserve_expr) parsed.has_vars = orig.has_vars;
                repl_core_replace_formatted_cmd(&store, cmd_idx, &parsed, parsed_text);
            }
            break;
        }
        }
    }

    repl_source_scope_depth_cache_invalidate();
    repl_mark_source_dirty();

    prof_end(PROF_REFORMAT);
}

/* ========================================================================= */
/* GLUT callbacks                                                             */
/* ========================================================================= */

/* ========================================================================= */
/* For-loop parsing and expansion                                             */
/* ========================================================================= */

/* parse_for_header, parse_c_for_header: see src/repl/eval.c */


/* Parse variable name from a FOR_BEGIN text string */
static void get_for_var_name_from_text(const char *text, char *var, int var_sz) {
    const char *p = text ? text : "";
    while (*p && *p != '(') p++;
    if (*p) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    int i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && i < var_sz - 1)
        var[i++] = *p++;
    var[i] = '\0';
}

int collect_visible_vars(int pos, ExprVar *vars, int max_vars, int *total_out) {
    typedef struct {
        CmdType type;
        ExprVar vars[MAX_EXPR_VARS];
        int count;
    } ScopeFrame;

    ScopeFrame frames[64];
    int depth = 0;
    /* Source text reads for for-loop / func-def reparse route through
     * a SourceTextView fetched at entry. Phase D will accept the
     * view as a parameter once collect_visible_vars is folded into
     * the editor commit path. */
    SourceTextView text = source_document_view();
    const GLCmd *document_cmds = repl_state_document_cmds();

    for (int cmd_idx = 0; cmd_idx < pos && cmd_idx < repl_state_document_count(); cmd_idx++) {
        CmdType t = document_cmds[cmd_idx].type;
        if (repl_cmd_is_block_head(t)) {
            if (depth >= (int)(sizeof(frames) / sizeof(frames[0])))
                break;

            frames[depth].type = t;
            frames[depth].count = 0;

            if (t == CMD_FOR_BEGIN) {
                char vn[16];
                const char *for_text = source_text_line(text, cmd_idx);
                get_for_var_name_from_text(for_text ? for_text : "", vn, sizeof(vn));
                repl_copy_string_fits(frames[depth].vars[0].name,
                                      sizeof(frames[depth].vars[0].name),
                                      vn);
                frames[depth].vars[0].value = document_cmds[cmd_idx].args[0];
                frames[depth].count = 1;
            } else if (t == CMD_FUNC_DEF) {
                int fn = -1;
                int param_count = 0;
                char param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
                const char *func_text = source_text_line(text, cmd_idx);
                if (parse_repl_func_signature(func_text ? func_text : "", &fn,
                                              param_names, MAX_EXPR_VARS,
                                              &param_count)) {
                    for (int param_idx = 0; param_idx < param_count; param_idx++) {
                        repl_copy_string_fits(frames[depth].vars[param_idx].name,
                                              sizeof(frames[depth].vars[param_idx].name),
                                              param_names[param_idx]);
                        frames[depth].vars[param_idx].value = 0.0f;
                    }
                    frames[depth].count = param_count;
                }
            }
            depth++;
        } else if (repl_cmd_is_block_end(t)) {
            if (depth > 0) depth--;
        }
    }

    int count = 0, total = 0;
    for (int depth_idx = depth - 1; depth_idx >= 0; depth_idx--) {
        for (int var_idx = 0; var_idx < frames[depth_idx].count; var_idx++) {
            if (count < max_vars)
                vars[count++] = frames[depth_idx].vars[var_idx];
            total++;
        }
    }
    if (total_out) *total_out = total;
    return count;
}

/* ========================================================================= */
/* Initialization                                                             */
/* ========================================================================= */

static void scroll_to_display_function(void) {
    repl_state_refresh_workspace_header_lines();
    ReplImportExportView meta = repl_state_import_export();
    int target = meta.workspace_header_line_count;
    for (int line_idx = 0; g_header_pre[line_idx]; line_idx++) {
        if (strcmp(g_header_pre[line_idx], REPL_EXPORT_DISPLAY_OPEN_LINE) == 0)
            break;
        target++;
    }
    repl_dispatch_scroll_to_line(target);
}

static int load_initial_commands(const char *import_file) {
    /* Returns the post-load cursor target. Caller (controller above
     * the β boundary) applies via editor_state_edit_line_set
     * (implemented in phase 3.6.4; see the edit-line-ownership
     * plan doc). */
    if (import_file) {
        struct stat st;
        if (stat(import_file, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (repl_load_workspace(import_file) > 0) {
                /* repl_load_workspace leaves the active slot at -1 so
                 * the live document is the pre-load stash (empty at
                 * startup). On a CLI bootstrap that strands the user on
                 * an empty buffer with all the workspace tabs visible
                 * but none of them active. Land on the first occupied
                 * slot — symmetric with the single-file branch below
                 * (which activates the home slot). */
                repl_scenes_activate_first_loaded_slot();
                scroll_to_display_function();
                return repl_state_document_count();
            }
        } else {
            ReplImportResult import_result;
            if (repl_export_load_from_file(import_file, &import_result)) {
                repl_scenes_activate_home_slot(import_result.scene_name);
                scroll_to_display_function();
                return repl_state_document_count();
            }
        }
    }

    /* Show example 0 as a starting demo, then anchor slot 0 ("My Scene")
     * to the current live state so user edits accumulate there and persist
     * across example switches. The startup banner is the controller's
     * to emit (see glr_ctrl_bootstrap_repl); pipeline TUs do not own
     * display-string side effects. */
    int example_edit_line = repl_load_example(0);
    repl_scenes_activate_home_slot(NULL);
    scroll_to_display_function();
    return example_edit_line;
}

void repl_save_default_output(const ReplExportLayout *layout) {
    (void)repl_export_save_output(outfile, source_document_view(), layout);
}

int repl_load_initial_commands(const char *import_file) {
    return load_initial_commands(import_file);
}

void repl_advance_time(float dt) {
    repl_state_time_advance(dt);
}

void repl_reset_time_to_zero(void) {
    repl_state_time_reset_to_zero();
}

void repl_set_time(float value) {
    repl_state_time_set(value);
}

/* repl_reset_state was removed in step 2 of
 * feature/decouple-repl-from-gl-repl-alt.md. Tests and callers that
 * want full-world reset call glr_ctrl_reset_all() (declared in
 * glr_ctrl.h). REPL-only callers can use repl_state_reset_program(). */

/* ========================================================================= */
/* Tunable-variable (@tune) collection                                        */
/* ========================================================================= */

int repl_collect_tuned_vars(const GLCmd *cmds, int count, SourceTextView text,
                            const char **out, int max, int *total_out) {
    int written = 0;
    int total = 0;
    if (!cmds || count < 0)
        count = 0;
    for (int i = 0; i < count; i++) {
        if (cmds[i].type != CMD_VAR_DECLARE)
            continue;
        if (!repl_eval_line_has_tune_tag(source_text_line(text, i)))
            continue;
        for (int n = 0; n < cmds[i].payload.decl.count; n++) {
            total++;
            if (out && written < max)
                out[written++] = cmds[i].payload.decl.names[n];
        }
    }
    if (total_out)
        *total_out = total;
    return written;
}
