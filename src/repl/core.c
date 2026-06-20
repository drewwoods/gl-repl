/*
 * src/repl/core.c - Residual REPL helpers awaiting redistribution.
 *
 * This translation unit is being dissolved into its natural owners.
 * What still lives here (per the R10 plan in ARCHITECTURE.md):
 *
 *   - repl_parse_and_normalize() / parse_and_normalize_impl() — until the
 *     parser absorbs them.
 *   - load_initial_commands() and a handful of startup helpers — pending move
 *     to src/repl/scenes.c.
 * For the live module map see MODULES.md. Editor input dispatch lives in
 * src/editor/input.c; cross-subsystem routing lives in glr_ctrl.c; commit
 * handlers live in src/editor/commit.c. The deleted repl_editor.{c,h} and
 * repl_commit.{c,h} are hard-guarded against return.
 */

#include "repl/core.h"
#include "repl/format.h"
#include "config.h" /* REPL_STATUS_TEXT_MAX */
#include "source_document.h"
#include "repl/command_spec.h"
#include "repl/core_internal.h"
#include "repl/scenes.h"
#include "repl/export.h"
#include "repl/parser.h"
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
 * append a semicolon, and prepend `indent_spaces` spaces. Used by
 * repl_parse_and_normalize() to produce canonical source text for a command. */
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

/* repl_reset_state was removed in step 2 of
 * feature/decouple-repl-from-gl-repl-alt.md. Tests and callers that
 * want full-world reset call glr_ctrl_reset_all() (declared in
 * glr_ctrl.h). REPL-only callers can use repl_state_reset_program(). */
