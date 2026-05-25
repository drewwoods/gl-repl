/*
 * src/repl/compile.c -- Pure source-text validators that produce
 *                   ReplCompiledChange descriptors.
 *
 * Contract: every entry point in this file is pure. It
 * reads the editor buffer view + REPL state read-only handles
 * passed in via ReplCompileContext, parses + validates the user's
 * input, and writes a ReplCompiledChange describing the source
 * change plus any predef-var side effects. It never:
 *   - calls set_status() / repl_state_status_*()
 *   - writes the editor buffer
 *   - writes the command store
 *   - mutates predef-var registrations
 *   - pushes an undo entry
 *
 * The success / failure paths flow upward through return values
 * and the `err` buffer. The caller decides how to surface the
 * diagnostic.
 *
 * Apply-side mutations (predef declare / undeclare, command-store
 * shift, editor-buffer write) are performed by src/repl/apply.c and
 * orchestrated by editor_commit (implemented in Phase C commits 20-21).
 *
 * Reading guide — entry points and what they emit:
 *   repl_compile_float_decl         INSERT_ONE / REPLACE_ONE  +
 *                                   DECLARE / UNDECLARE / SET_VALUE
 *   repl_compile_var_assign         INSERT_ONE / REPLACE_ONE  +
 *                                   SET_VALUE (+ UNDECLARE on
 *                                   decl-row overwrite)
 *   repl_compile_set_predef_value   REPLACE_ONE on the literal
 *                                   assign or decl initializer
 *                                   that backs the named variable;
 *                                   pure SET_VALUE if neither exists
 *   repl_compile_empty_line         INSERT_ONE
 *   repl_compile_delete_range       DELETE_RANGE + UNDECLARE for any
 *                                   decl rows in the range
 *   repl_compile_toggle_comment     INSERT_MANY for block-batches
 *                                   (head..end), REPLACE_ONE for
 *                                   single rows; UNDECLAREs decl
 *                                   rows being commented out
 *
 * Dispatcher: repl_compile_dispatch() walks the float-decl +
 * var-assign chain and is callable from outside the editor. The
 * uncomment path calls it as a fallback.
 */

#define _POSIX_C_SOURCE 200809L /* for strnlen on linux */
#include "repl/compile.h"

#include "repl/core_internal.h"  /* repl_format_fits, repl_extract_assignment_parts, collect_visible_vars */
#include "repl/parser.h"         /* repl_parser_parse_command_ctx (uncomment fallback) */
#include "repl/source_scope.h"   /* repl_source_scope_cmd_indent, _find_block_end */
#include "repl/state_owners.h"

#include "config.h"              /* REPL_INDENT_TEXT_MAX */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

ReplCompileResult repl_compile_dispatch(const char *text,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!out || !ctx) return REPL_COMPILE_ERROR;

    /* Float decl first: `float x;` would otherwise parse as an
     * assignment to identifier `float`. */
    ReplCompileResult r = repl_compile_float_decl(text, ctx, out,
                                                  err, err_size);
    if (r == REPL_COMPILE_ERROR) return r;
    if (out->kind != REPL_COMPILED_NO_CHANGE) return REPL_COMPILE_OK;

    /* Var assignment: `x = expr;`. */
    r = repl_compile_var_assign(text, ctx, out, err, err_size);
    if (r == REPL_COMPILE_ERROR) return r;
    if (out->kind != REPL_COMPILED_NO_CHANGE) return REPL_COMPILE_OK;

    /* Structured-block syntax (close_brace / if_block / func_def /
     * for_loop) is currently routed through the editor-side
     * `editor_compile_*` chain. Pure variants are extracted into this
     * module; until then this dispatcher
     * returns NO_CHANGE for those inputs and the caller falls
     * through (implemented as step 5a of the decouple plan). */

    out->kind = REPL_COMPILED_NO_CHANGE;
    return REPL_COMPILE_OK;
}

void repl_compiled_change_init(ReplCompiledChange *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->kind = REPL_COMPILED_NO_CHANGE;
    out->pos = 0;
    out->count = 0;
    out->delete_pos = -1;
    out->delete_count = 0;
    out->newly_aliased_slot = -1;
    out->had_previous_alias = 0;
    out->previous_alias[0] = '\0';
}

void repl_compiled_change_rollback_alias(const ReplCompiledChange *change) {
    if (!change || change->newly_aliased_slot < 0)
        return;
    if (change->had_previous_alias) {
        repl_func_alias_set(change->newly_aliased_slot,
                            change->previous_alias);
    } else {
        repl_func_alias_clear(change->newly_aliased_slot);
    }
}

void repl_compiled_change_to_text_change(const ReplCompiledChange *in,
                                         SourceTextChange *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->delete_pos = -1;
    if (!in) return;

    switch (in->kind) {
    case REPL_COMPILED_NO_CHANGE:   out->kind = SOURCE_TEXT_NO_CHANGE;   break;
    case REPL_COMPILED_INSERT_ONE:  out->kind = SOURCE_TEXT_INSERT_ONE;  break;
    case REPL_COMPILED_REPLACE_ONE: out->kind = SOURCE_TEXT_REPLACE_ONE; break;
    case REPL_COMPILED_INSERT_MANY: out->kind = SOURCE_TEXT_INSERT_MANY; break;
    case REPL_COMPILED_DELETE_RANGE:out->kind = SOURCE_TEXT_DELETE_RANGE;break;
    }
    out->pos          = in->pos;
    out->count        = in->count;   /* DELETE_RANGE: rows to delete, may exceed MAX_COMMIT_CMDS */
    out->delete_pos   = in->delete_pos;
    out->delete_count = in->delete_count;

    /* Only the insert/replace/load kinds carry text rows. DELETE_RANGE
     * leaves text[] untouched (and its `count` is the delete range,
     * which is *not* bounded by MAX_COMMIT_CMDS); clamping out->count
     * for that kind would silently shrink the delete. */
    int copy_n;
    switch (in->kind) {
    case REPL_COMPILED_INSERT_ONE:
    case REPL_COMPILED_REPLACE_ONE:
        copy_n = 1;
        break;
    case REPL_COMPILED_INSERT_MANY:
        copy_n = in->count;
        if (copy_n < 0) copy_n = 0;
        if (copy_n > MAX_COMMIT_CMDS) copy_n = MAX_COMMIT_CMDS;
        break;
    case REPL_COMPILED_NO_CHANGE:
    case REPL_COMPILED_DELETE_RANGE:
    default:
        copy_n = 0;
        break;
    }
    for (int i = 0; i < copy_n; i++) {
        size_t len = strnlen(in->text[i], MAX_LINE_LEN - 1);
        memcpy(out->text[i], in->text[i], len);
        out->text[i][len] = '\0';
    }
}

ReplCompileContext repl_compile_context_from_live(int edit_line_idx) {
    /* insert_mode defaults to 0 (overwrite mode). The non-editor load
     * path (src/repl/load.c, demo, parse tests) always appends at the
     * end, so 0 is correct. The editor-side callers (glr_ctrl.c
     * variable-panel commit, editor commit pipeline) overwrite
     * ctx.insert_mode = editor_insert_mode() after this returns —
     * insert mode is editor state, not REPL state, so the REPL
     * pipeline doesn't reach for it.
     *
     * edit_line_idx is supplied by the caller (β: REPL pipeline does
     * not reach into editor_state_* for the cursor), implemented in
     * phase 3.6.1 of plans/in-review/edit-line-ownership.md. */
    ReplCompileContext ctx = {
        .edit_line       = edit_line_idx,
        .document_count  = repl_state_document_count(),
        .insert_mode     = 0,
        .text            = source_document_view(),
        .document_cmds   = repl_state_document_cmds(),
    };
    return ctx;
}

/* Compile-time analog of editor_commit.c's static fill_scope_indent. */
static void compile_scope_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_cmd_indent(pos, buf, buf_sz);
}

/* Insert/replace position for ctx: the edit line in insert mode;
 * otherwise the edit line clamped to document_count so a past-the-end
 * edit line appends rather than opening a gap. */
static int compile_insert_pos(const ReplCompileContext *ctx) {
    return ctx->insert_mode ? ctx->edit_line :
           (ctx->edit_line < ctx->document_count
                ? ctx->edit_line : ctx->document_count);
}

static int compile_set_err(char *err, int err_size, const char *fmt, ...) {
    va_list ap;
    if (!err || err_size <= 0)
        return 0;
    va_start(ap, fmt);
    vsnprintf(err, (size_t)err_size, fmt, ap);
    va_end(ap);
    return REPL_COMPILE_ERROR;
}

static void compile_copy_leading_ws(const char *text, char *out, int out_sz) {
    int off = 0;
    const char *p = text ? text : "";

    if (!out || out_sz <= 0)
        return;

    while (*p && isspace((unsigned char)*p) && *p != '\n' && *p != '\r' &&
           off < out_sz - 1) {
        out[off++] = *p++;
    }
    out[off] = '\0';
}

static int compile_find_last_literal_assign(const ReplCompileContext *ctx,
                                            int var_idx) {
    int found = -1;

    if (!ctx || !ctx->document_cmds)
        return -1;

    for (int cmd_idx = 0; cmd_idx < ctx->document_count; cmd_idx++) {
        const GLCmd *cmd = &ctx->document_cmds[cmd_idx];
        if (!cmd->valid || cmd->type != CMD_VAR_ASSIGN)
            continue;
        if (cmd->var_idx != var_idx || cmd->has_vars)
            continue;
        found = cmd_idx;
    }
    return found;
}

static int compile_has_any_assign(const ReplCompileContext *ctx,
                                  int var_idx) {
    if (!ctx || !ctx->document_cmds)
        return 0;

    for (int cmd_idx = 0; cmd_idx < ctx->document_count; cmd_idx++) {
        const GLCmd *cmd = &ctx->document_cmds[cmd_idx];
        if (!cmd->valid || cmd->type != CMD_VAR_ASSIGN)
            continue;
        if (cmd->var_idx == var_idx)
            return 1;
    }
    return 0;
}

static int compile_find_var_decl(const ReplCompileContext *ctx,
                                 const char *name) {
    if (!ctx || !ctx->document_cmds || !name || !name[0])
        return -1;

    for (int cmd_idx = 0; cmd_idx < ctx->document_count; cmd_idx++) {
        const GLCmd *cmd = &ctx->document_cmds[cmd_idx];
        if (!cmd->valid || cmd->type != CMD_VAR_DECLARE)
            continue;
        for (int decl_idx = 0; decl_idx < cmd->var_decl_count; decl_idx++) {
            if (strcmp(cmd->var_names[decl_idx], name) == 0)
                return cmd_idx;
        }
    }
    return -1;
}

static int compile_append_text(char *out, int out_sz, int *off,
                               const char *fmt, ...) {
    va_list ap;
    int wrote;

    if (!out || !off || *off < 0 || *off >= out_sz)
        return 0;

    va_start(ap, fmt);
    wrote = vsnprintf(out + *off, (size_t)(out_sz - *off), fmt, ap);
    va_end(ap);
    if (wrote < 0 || *off + wrote >= out_sz)
        return 0;
    *off += wrote;
    return 1;
}

static int compile_append_span(char *out, int out_sz, int *off,
                               const char *start, const char *end) {
    int len;

    if (!out || !off || !start || !end || end < start)
        return 0;
    len = (int)(end - start);
    if (*off < 0 || *off + len >= out_sz)
        return 0;
    memcpy(out + *off, start, (size_t)len);
    *off += len;
    out[*off] = '\0';
    return 1;
}

static int compile_build_literal_assign_change(const ReplCompileContext *ctx,
                                               int cmd_idx,
                                               const char *name,
                                               float value,
                                               ReplCompiledChange *out) {
    char indent[REPL_INDENT_TEXT_MAX];

    if (!ctx || !out || !name || cmd_idx < 0 || cmd_idx >= ctx->document_count)
        return 0;

    compile_copy_leading_ws(source_text_line(ctx->text, cmd_idx),
                            indent, sizeof(indent));

    out->kind = REPL_COMPILED_REPLACE_ONE;
    out->pos = cmd_idx;
    out->count = 1;
    out->adjust_edit_line = 0;
    out->cmds[0] = ctx->document_cmds[cmd_idx];
    out->cmds[0].args[0] = value;
    out->cmds[0].has_vars = 0;

    return snprintf(out->text[0], sizeof(out->text[0]), "%s%s = %g;",
                    indent, name, (double)value) < (int)sizeof(out->text[0]);
}

/* Rewrite the initializer for one name in a multi-name decl line.
 *
 * Given `  float a = sin(t), b, c = 2; // ok` and (name="b", value=4),
 * produce `  float a = sin(t), b = 4, c = 2; // ok`.
 *
 * Approach: tokenize the body between `float ` and the first `;`
 * into name segments separated by commas at paren-depth 0 (so
 * `sin(t, u)` initializers don't split). For each segment, parse
 * the leading identifier; if it matches `name`, emit `name = value`,
 * otherwise emit the segment verbatim. Returns 1 on a clean rewrite,
 * 0 if `name` was not found or anything failed (caller falls back to
 * a pure SET_VALUE op without rewriting source).
 *
 * Indentation, the trailing `;`, and any trailing `// comment` are
 * preserved. */
static int compile_rewrite_decl_initializer_text(const char *orig_text,
                                                 const char *name,
                                                 float value,
                                                 char *out,
                                                 int out_sz) {
    char indent[REPL_INDENT_TEXT_MAX];
    const char *line;
    const char *scan;
    const char *comment;
    const char *body_end;
    const char *semi;
    const char *chunk_start;
    int off = 0;
    int found = 0;

    if (!name || !name[0] || !out || out_sz <= 0)
        return 0;

    line = orig_text ? orig_text : "";
    compile_copy_leading_ws(line, indent, sizeof(indent));
    scan = line + strlen(indent);
    /* Optional `static ` prefix (canonical form per format_decl_text). */
    if (strncmp(scan, "static", 6) == 0 && isspace((unsigned char)scan[6])) {
        scan += 6;
        while (*scan && isspace((unsigned char)*scan))
            scan++;
    }
    if (strncmp(scan, "float", 5) != 0 ||
        isalnum((unsigned char)scan[5]) || scan[5] == '_')
        return 0;

    scan += 5;
    while (*scan && isspace((unsigned char)*scan))
        scan++;

    comment = strstr(scan, "//");
    body_end = comment ? comment : scan + strlen(scan);
    semi = body_end;
    for (const char *p = scan; p < body_end; p++) {
        if (*p == ';') {
            semi = p;
            break;
        }
    }

    out[0] = '\0';
    if (!compile_append_text(out, out_sz, &off, "%sstatic float ", indent))
        return 0;

    chunk_start = scan;
    int depth = 0;
    for (const char *p = scan; ; p++) {
        int at_end = (p >= semi);
        char ch = at_end ? '\0' : *p;
        if (!at_end) {
            if (ch == '(') depth++;
            else if (ch == ')' && depth > 0) depth--;
        }
        if (at_end || (ch == ',' && depth == 0)) {
            const char *seg_start = chunk_start;
            const char *seg_end = p;
            const char *name_start;
            const char *name_end;
            char decl_name[REPL_PREDEF_NAME_MAX];
            int decl_len;

            while (seg_start < seg_end && isspace((unsigned char)*seg_start))
                seg_start++;
            while (seg_end > seg_start && isspace((unsigned char)seg_end[-1]))
                seg_end--;
            if (seg_start >= seg_end)
                return 0;

            name_start = seg_start;
            if (!isalpha((unsigned char)*name_start) && *name_start != '_')
                return 0;
            name_end = name_start;
            while (name_end < seg_end &&
                   (isalnum((unsigned char)*name_end) || *name_end == '_'))
                name_end++;
            decl_len = (int)(name_end - name_start);
            if (decl_len <= 0 || decl_len >= (int)sizeof(decl_name))
                return 0;
            memcpy(decl_name, name_start, (size_t)decl_len);
            decl_name[decl_len] = '\0';

            if (seg_start != scan && !compile_append_text(out, out_sz, &off, ", "))
                return 0;

            if (strcmp(decl_name, name) == 0) {
                if (!compile_append_text(out, out_sz, &off, "%s = %g",
                                         name, (double)value))
                    return 0;
                found = 1;
            } else if (!compile_append_span(out, out_sz, &off, seg_start, seg_end)) {
                return 0;
            }

            if (at_end)
                break;
            chunk_start = p + 1;
        }
    }

    if (!found || !compile_append_text(out, out_sz, &off, ";"))
        return 0;
    if (comment && *comment) {
        while (*comment && isspace((unsigned char)*comment))
            comment++;
        if (*comment && !compile_append_text(out, out_sz, &off, " %s", comment))
            return 0;
    }

    return 1;
}

/* Parsed shape of `float a, b = expr, c;` — names + optional init
 * values (already evaluated at compile time so apply doesn't need
 * the source string). decl_comment carries any trailing `// ...`
 * verbatim so format_decl_text can re-emit it. */
typedef struct {
    char  names[MAX_NAMES_PER_DECL][16];
    float init_vals[MAX_NAMES_PER_DECL];
    int   has_init[MAX_NAMES_PER_DECL];
    int   count;
    char  decl_comment[MAX_LINE_LEN];
} FloatDeclParse;

/* Parse `float NAME [= EXPR] (, NAME [= EXPR])* ;  // comment`.
 * Returns:
 *   REPL_COMPILE_OK + parsed != NULL, *recognized = 1   on success.
 *   REPL_COMPILE_OK + *recognized = 0                   when input
 *      doesn't start with the `float` keyword (caller falls through
 *      to the next handler).
 *   REPL_COMPILE_ERROR with err filled                  on a real
 *      syntax error inside an otherwise float-shaped line. */
static ReplCompileResult parse_float_name_list(const char *input,
                                               FloatDeclParse *parsed,
                                               int *recognized,
                                               char *err, int err_size) {
    *recognized = 0;
    memset(parsed, 0, sizeof(*parsed));

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    /* Optional `static ` prefix: format_decl_text emits it, so we
     * must accept it on the round-trip. */
    if (strncmp(p, "static", 6) == 0 && isspace((unsigned char)p[6])) {
        p += 6;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    if (strncmp(p, "float", 5) != 0)
        return REPL_COMPILE_OK;
    if (isalnum((unsigned char)p[5]) || p[5] == '_')
        return REPL_COMPILE_OK;
    p += 5;
    *recognized = 1;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ';' || *p == '\0') break;
        if (parsed->count > 0) {
            /* Subsequent name must be preceded by ','. A non-comma
             * here means the line was float-shaped through `float `
             * but breaks the comma-separated list — fall through to
             * the next handler instead of erroring. */
            if (*p != ',') {
                *recognized = 0;
                return REPL_COMPILE_OK;
            }
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
        }
        if (!isalpha((unsigned char)*p) && *p != '_')
            return compile_set_err(err, err_size,
                "syntax error in float declaration: expected identifier");

        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int len = (int)(p - start);
        if (len <= 0 || len >= 16)
            return compile_set_err(err, err_size, "invalid identifier (max 15 chars)");
        if (parsed->count >= MAX_NAMES_PER_DECL)
            return compile_set_err(err, err_size,
                "too many names per declaration (max %d); split across lines",
                MAX_NAMES_PER_DECL);
        memcpy(parsed->names[parsed->count], start, (size_t)len);
        parsed->names[parsed->count][len] = '\0';

        /* Optional `= expr`. Stop at unparenthesized comma so the
         * outer name loop picks up the next decl. `==` is left for
         * the eval validator to reject — a literal `=` followed by
         * `=` is not a decl initializer. */
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '=' && p[1] != '=') {
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0' || *p == ';' || *p == ',')
                return compile_set_err(err, err_size, "expected expression after '='");

            const char *expr_start = p;
            int depth = 0;
            while (*p && *p != ';') {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                else if (*p == ',' && depth == 0) break;
                p++;
            }

            char init_expr[MAX_LINE_LEN];
            int elen = (int)(p - expr_start);
            if (elen >= (int)sizeof(init_expr)) elen = (int)sizeof(init_expr) - 1;
            memcpy(init_expr, expr_start, (size_t)elen);
            init_expr[elen] = '\0';
            while (elen > 0 && isspace((unsigned char)init_expr[elen - 1]))
                init_expr[--elen] = '\0';
            if (elen == 0)
                return compile_set_err(err, err_size, "expected expression after '='");

            char verr[128];
            if (!repl_eval_validate_expression_idents(init_expr, NULL, 0,
                                                      verr, sizeof(verr)))
                return compile_set_err(err, err_size, "%s", verr);
            ExprCtx eval_ctx = { init_expr, g_predef_vars, g_num_predef_vars };
            parsed->init_vals[parsed->count] = repl_eval_expr(&eval_ctx);
            parsed->has_init[parsed->count] = 1;
        }

        parsed->count++;
    }

    if (parsed->count == 0)
        return compile_set_err(err, err_size,
            "float declaration requires at least one identifier");
    if (*p == ';') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0' && !(p[0] == '/' && p[1] == '/'))
        return compile_set_err(err, err_size,
            "syntax error: unexpected trailing text after declaration");
    if (p[0] == '/' && p[1] == '/')
        snprintf(parsed->decl_comment, sizeof(parsed->decl_comment), " %s", p);

    return REPL_COMPILE_OK;
}

/* Validate the parsed name list against project invariants:
 *   - no duplicates within the declaration
 *   - no clash with reserved keywords (`t`, `PI`, etc.)
 *   - identifiers start with a letter or underscore
 *   - names already in the predef table are only re-declarable when
 *     they're being *kept* across a decl-row overwrite (old_decl has
 *     them); otherwise it's a duplicate decl
 *   - net new slot count fits MAX_PREDEF_VARS */
static ReplCompileResult validate_decl_names(const FloatDeclParse *parsed,
                                             const GLCmd *old_decl,
                                             char *err, int err_size) {
    for (int var_idx = 0; var_idx < parsed->count; var_idx++) {
        const char *nm = parsed->names[var_idx];

        for (int prev = 0; prev < var_idx; prev++) {
            if (strcmp(nm, parsed->names[prev]) == 0)
                return compile_set_err(err, err_size,
                    "duplicate name '%s' in declaration", nm);
        }
        if (repl_eval_find_predef_var_idx(nm) >= 0) {
            int in_old_decl = 0;
            if (old_decl) {
                for (int d = 0; d < old_decl->var_decl_count; d++) {
                    if (strcmp(old_decl->var_names[d], nm) == 0) {
                        in_old_decl = 1;
                        break;
                    }
                }
            }
            if (!in_old_decl)
                return compile_set_err(err, err_size,
                    "'%s' is already declared", nm);
        }
        if (repl_eval_is_reserved_ident(nm))
            return compile_set_err(err, err_size, "'%s' is reserved", nm);
        if (!(isalpha((unsigned char)nm[0]) || nm[0] == '_'))
            return compile_set_err(err, err_size, "invalid identifier '%s'", nm);
    }

    int old_count = old_decl ? old_decl->var_decl_count : 0;
    if (g_num_predef_vars + parsed->count - old_count > MAX_PREDEF_VARS)
        return compile_set_err(err, err_size,
            "variable table full (max %d)", MAX_PREDEF_VARS);

    return REPL_COMPILE_OK;
}

/* Format the decl into source text. Decls always live at depth 0,
 * so the indent is the project's standard 2-space gutter. The
 * `static` keyword is canonical: predef vars are file-scope statics
 * that retain their values across frames (the exporter emits them
 * as `static float ...` in the generated C file), and surfacing the
 * keyword in the code panel makes that lifetime obvious. */
static void format_decl_text(const FloatDeclParse *parsed,
                             char *out, int out_sz) {
    const char indent[] = "  ";
    int off = snprintf(out, (size_t)out_sz, "%sstatic float ", indent);
    for (int var_idx = 0;
         var_idx < parsed->count && off < out_sz - 4;
         var_idx++) {
        if (var_idx > 0)
            off += snprintf(out + off, (size_t)(out_sz - off), ", ");
        off += snprintf(out + off, (size_t)(out_sz - off), "%s",
                        parsed->names[var_idx]);
        if (parsed->has_init[var_idx])
            off += snprintf(out + off, (size_t)(out_sz - off),
                            " = %g", parsed->init_vals[var_idx]);
    }
    snprintf(out + off, (size_t)(out_sz - off), ";%s", parsed->decl_comment);
}

/* Build the predef-op plan for the decl change. Three cases per
 * name:
 *   - dropped (in old_decl, not in new) -> UNDECLARE
 *   - new (not currently in predef table) -> DECLARE [+ value]
 *   - kept (already declared) -> SET_VALUE if has_init else NOOP
 *
 * UNDECLAREs go first so apply's slot-shift cascade observes the
 * pre-removal indices (see repl_apply_predef_ops). */
static void build_decl_predef_ops(const FloatDeclParse *parsed,
                                  const GLCmd *old_decl,
                                  ReplCompiledChange *out) {
    int op_count = 0;

    if (old_decl) {
        for (int d = 0; d < old_decl->var_decl_count; d++) {
            const char *nm = old_decl->var_names[d];
            int kept = 0;
            for (int v = 0; v < parsed->count; v++) {
                if (strcmp(parsed->names[v], nm) == 0) { kept = 1; break; }
            }
            if (kept) continue;
            if (op_count >= MAX_PREDEF_OPS_PER_COMMIT) break;
            out->predef_ops[op_count].kind = REPL_PREDEF_OP_UNDECLARE;
            repl_copy_string_fits(out->predef_ops[op_count].name,
                                  sizeof(out->predef_ops[op_count].name), nm);
            op_count++;
        }
    }
    for (int v = 0; v < parsed->count; v++) {
        if (op_count >= MAX_PREDEF_OPS_PER_COMMIT) break;
        const char *nm = parsed->names[v];
        int already_registered =
            (old_decl && repl_eval_find_predef_var_idx(nm) >= 0);
        ReplPredefOp *op = &out->predef_ops[op_count++];

        if (already_registered) {
            if (parsed->has_init[v]) {
                op->kind = REPL_PREDEF_OP_SET_VALUE;
                op->value = parsed->init_vals[v];
                op->has_value = 1;
                repl_copy_string_fits(op->name, sizeof(op->name), nm);
            } else {
                op->kind = REPL_PREDEF_OP_NOOP;
            }
        } else {
            op->kind = REPL_PREDEF_OP_DECLARE;
            repl_copy_string_fits(op->name, sizeof(op->name), nm);
            if (parsed->has_init[v]) {
                op->value = parsed->init_vals[v];
                op->has_value = 1;
            }
        }
    }
    out->predef_op_count = op_count;
}

/* Compose "declared a, b, c" for the status banner. */
static void build_decl_commit_message(const FloatDeclParse *parsed,
                                      char *msg, int msg_sz) {
    int off = snprintf(msg, (size_t)msg_sz, "declared ");
    for (int v = 0; v < parsed->count && off < msg_sz - 4; v++) {
        if (v > 0)
            off += snprintf(msg + off, (size_t)(msg_sz - off), ", ");
        off += snprintf(msg + off, (size_t)(msg_sz - off), "%s",
                        parsed->names[v]);
    }
}

ReplCompileResult repl_compile_float_decl(const char *input,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);

    FloatDeclParse parsed;
    int recognized = 0;
    ReplCompileResult r =
        parse_float_name_list(input, &parsed, &recognized, err, err_size);
    if (r != REPL_COMPILE_OK)
        return r;
    if (!recognized) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    /* Decl placement and overwrite-in-place detection. New decls
     * always land at the top of the non-decl region, so existing
     * references appear strictly after their declaration. Editing
     * an existing CMD_VAR_DECLARE row is the only case that
     * replaces in-place. */
    int insert_idx = compile_insert_pos(ctx);
    int overwriting_decl = (!ctx->insert_mode &&
                            insert_idx < ctx->document_count &&
                            ctx->document_cmds[insert_idx].type == CMD_VAR_DECLARE);
    const GLCmd *old_decl = overwriting_decl ? &ctx->document_cmds[insert_idx] : NULL;

    r = validate_decl_names(&parsed, old_decl, err, err_size);
    if (r != REPL_COMPILE_OK)
        return r;

    /* Overwrite-feasibility: removed names must not be referenced
     * elsewhere in the document. Replacement is rejected outright
     * rather than auto-deleting the references. */
    if (overwriting_decl) {
        for (int d = 0; d < old_decl->var_decl_count; d++) {
            const char *nm = old_decl->var_names[d];
            int kept = 0;
            for (int v = 0; v < parsed.count; v++) {
                if (strcmp(parsed.names[v], nm) == 0) { kept = 1; break; }
            }
            if (kept) continue;
            for (int cmd_idx = 0; cmd_idx < ctx->document_count; cmd_idx++) {
                if (cmd_idx == insert_idx) continue;
                const char *line = source_text_line(ctx->text, cmd_idx);
                if (repl_eval_source_uses_ident(line ? line : "", nm))
                    return compile_set_err(err, err_size,
                        "variable '%s' is in use, cannot overwrite", nm);
            }
        }
    }

    /* Build the GLCmd that backs the source row. */
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_VAR_DECLARE;
    cmd.valid = 1;
    cmd.var_decl_count = parsed.count;
    for (int v = 0; v < parsed.count; v++) {
        if (!repl_copy_string_fits(cmd.var_names[v],
                                   sizeof(cmd.var_names[v]),
                                   parsed.names[v]))
            return compile_set_err(err, err_size, "invalid identifier (max 15 chars)");
    }

    int decl_pos = 0;
    while (decl_pos < ctx->document_count &&
           ctx->document_cmds[decl_pos].type == CMD_VAR_DECLARE)
        decl_pos++;

    if (overwriting_decl) {
        out->kind  = REPL_COMPILED_REPLACE_ONE;
        out->pos   = insert_idx;
        out->count = 1;
        out->adjust_edit_line = 0;
    } else {
        out->kind  = REPL_COMPILED_INSERT_ONE;
        out->pos   = decl_pos;
        out->count = 1;
        out->adjust_edit_line = 1;  /* REPL_COMMAND_STORE_ADJUST_EDIT_LINE */
    }
    out->cmds[0] = cmd;

    char decl_text[MAX_LINE_LEN];
    format_decl_text(&parsed, decl_text, (int)sizeof(decl_text));
    repl_copy_string_fits(out->text[0], sizeof(out->text[0]), decl_text);

    build_decl_predef_ops(&parsed, old_decl, out);
    build_decl_commit_message(&parsed, out->commit_message,
                              (int)sizeof(out->commit_message));
    return REPL_COMPILE_OK;
}

static int compile_rebase_var_assign_slot_after_undeclares(
        const char *name,
        int original_slot,
        const ReplCompiledChange *change) {
    int shifted_slot = original_slot;

    for (int op_idx = 0; op_idx < change->predef_op_count; op_idx++) {
        const ReplPredefOp *op = &change->predef_ops[op_idx];
        int dropped_slot;

        if (op->kind != REPL_PREDEF_OP_UNDECLARE)
            continue;

        dropped_slot = repl_eval_find_predef_var_idx(op->name);
        if (dropped_slot < 0)
            continue;
        if (strcmp(op->name, name) == 0)
            return -1;
        if (dropped_slot < original_slot)
            shifted_slot--;
    }

    return shifted_slot;
}

/* CONTRACT (audit #11): context-pure for document data, live-state-
 * coupled for visible-var collection. The ReplCompileContext snapshot
 * is authoritative for document_cmds / _count / edit_line, but the
 * `collect_visible_vars` call below reads from the live g_repl_state
 * document. Callers must apply each change to the live document
 * before the next visible-vars compile call. Reached from both the
 * editor commit path and the file-load path via load_try_block. */
ReplCompileResult repl_compile_var_assign(const char *input,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);

    char name[REPL_PREDEF_NAME_MAX];
    char index_expr[MAX_LINE_LEN];
    char rhs[MAX_LINE_LEN];
    char comment[MAX_LINE_LEN];

    if (!repl_extract_assignment_target_parts(input ? input : "",
                                              name, sizeof(name),
                                              index_expr, sizeof(index_expr),
                                              rhs, sizeof(rhs))) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    comment[0] = '\0';
    {
        const char *cp = strstr(input ? input : "", "//");
        if (cp) {
            while (*cp && isspace((unsigned char)*cp)) cp++;
            if (cp[0] == '/' && cp[1] == '/')
                snprintf(comment, sizeof(comment), " %s", cp);
        }
    }

    int insert_idx = compile_insert_pos(ctx);

    ExprVar vis[MAX_EXPR_VARS];
    int vis_n = collect_visible_vars(insert_idx, vis, MAX_EXPR_VARS, NULL);
    char verr[128];
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));

    if (index_expr[0]) {
        int scratch_array_idx = repl_eval_scratch_array_index(name);
        if (scratch_array_idx < 0)
            return compile_set_err(err, err_size, "unknown array '%s'", name);

        if (!repl_eval_validate_expression_idents(index_expr,
                                                  vis_n > 0 ? vis : NULL, vis_n,
                                                  verr, sizeof(verr)))
            return compile_set_err(err, err_size, "%s", verr);
        if (!repl_eval_validate_expression_idents(rhs,
                                                  vis_n > 0 ? vis : NULL, vis_n,
                                                  verr, sizeof(verr)))
            return compile_set_err(err, err_size, "%s", verr);

        ExprCtx idx_ctx = { index_expr, vis_n > 0 ? vis : NULL, vis_n, NULL, 0 };
        ExprCtx rhs_ctx = { rhs, vis_n > 0 ? vis : NULL, vis_n, NULL, 0 };
        int elem_idx = (int)repl_eval_expr(&idx_ctx);
        if (elem_idx < 0 || elem_idx >= REPL_SCRATCH_ARRAY_LEN)
            return compile_set_err(err, err_size,
                                   "scratch array index out of range: %d", elem_idx);

        float val = repl_eval_expr(&rhs_ctx);
        int has_index_vars = input_has_any_visible_vars(index_expr,
                                                        vis_n > 0 ? vis : NULL, vis_n);
        int has_rhs_vars = input_has_any_visible_vars(rhs,
                                                      vis_n > 0 ? vis : NULL, vis_n);

        cmd.type = CMD_SCRATCH_ASSIGN;
        cmd.valid = 1;
        cmd.args[0] = (float)scratch_array_idx;
        cmd.args[1] = (float)elem_idx;
        cmd.args[2] = val;
        cmd.num_args = 3;
        cmd.has_vars = has_index_vars || has_rhs_vars;

        out->scratch_ops[0].array_idx = scratch_array_idx;
        out->scratch_ops[0].elem_idx = elem_idx;
        out->scratch_ops[0].value = val;
        out->scratch_op_count = 1;

        snprintf(out->commit_message, sizeof(out->commit_message),
                 "%s[%d] = %g", name, elem_idx, (double)val);
    } else {
        int var_idx = repl_eval_find_predef_var_idx(name);
        if (var_idx < 0)
            return compile_set_err(err, err_size,
                "undeclared variable '%s' - use 'float %s;' first", name, name);

        if (!repl_eval_validate_expression_idents(rhs,
                                                  vis_n > 0 ? vis : NULL, vis_n,
                                                  verr, sizeof(verr)))
            return compile_set_err(err, err_size, "%s", verr);

        ExprCtx eval_ctx = { rhs, vis_n > 0 ? vis : NULL, vis_n, NULL, 0 };
        float val = repl_eval_expr(&eval_ctx);
        int has_rhs_vars = input_has_any_visible_vars(rhs,
                                                      vis_n > 0 ? vis : NULL, vis_n);

        cmd.type     = CMD_VAR_ASSIGN;
        cmd.valid    = 1;
        cmd.args[0]  = val;
        cmd.num_args = 1;       /* args[0] holds the assigned value */
        cmd.var_idx  = var_idx; /* predef slot the executor will write */
        cmd.has_vars = has_rhs_vars;

        if (out->predef_op_count < MAX_PREDEF_OPS_PER_COMMIT) {
            out->predef_ops[out->predef_op_count].kind = REPL_PREDEF_OP_SET_VALUE;
            repl_copy_string_fits(out->predef_ops[out->predef_op_count].name,
                                  sizeof(out->predef_ops[out->predef_op_count].name), name);
            out->predef_ops[out->predef_op_count].value = val;
            out->predef_ops[out->predef_op_count].has_value = 1;
            out->predef_op_count++;
        }

        snprintf(out->commit_message, sizeof(out->commit_message),
                 "%s = %g", name, (double)val);
    }

    /* Format text using the scope indent at the insert position. */
    char indent[REPL_INDENT_TEXT_MAX];
    compile_scope_indent(insert_idx, indent, sizeof(indent));
    char assign_text[MAX_LINE_LEN];
    if (index_expr[0]) {
        if (!repl_format_fits(assign_text, sizeof(assign_text),
                              "%s%s[%s] = %s;%s",
                              indent, name, index_expr, rhs, comment))
            return compile_set_err(err, err_size, "Command too long");
    } else if (!repl_format_fits(assign_text, sizeof(assign_text),
                                 "%s%s = %s;%s", indent, name, rhs, comment)) {
        return compile_set_err(err, err_size, "Command too long");
    }

    /* Decide insert / replace. The REPLACE_ONE branch absorbs the
     * legacy var-decl overwrite cascade: when the assignment lands on
     * a CMD_VAR_DECLARE in non-insert mode, validate that the dropped
     * names are not in use elsewhere and emit UNDECLARE predef ops so
     * apply replays the slot shift through repl_apply_predef_ops. */
    int overwriting_decl = 0;
    const GLCmd *old_decl = NULL;
    if (ctx->insert_mode) {
        out->kind  = REPL_COMPILED_INSERT_ONE;
        out->pos   = insert_idx;
        out->count = 1;
        out->adjust_edit_line = 0;
    } else if (insert_idx < ctx->document_count) {
        out->kind  = REPL_COMPILED_REPLACE_ONE;
        out->pos   = insert_idx;
        out->count = 1;
        out->adjust_edit_line = 0;
        if (ctx->document_cmds[insert_idx].type == CMD_VAR_DECLARE) {
            overwriting_decl = 1;
            old_decl = &ctx->document_cmds[insert_idx];
        }
    } else {
        out->kind  = REPL_COMPILED_INSERT_ONE;
        out->pos   = ctx->document_count;
        out->count = 1;
        out->adjust_edit_line = 0;
    }
    repl_copy_string_fits(out->text[0], sizeof(out->text[0]), assign_text);

    /* Overwrite-feasibility check + UNDECLARE op plan. Mirrors the
     * float-decl overwrite check; the in-use predicate skips the line
     * being replaced. SET_VALUE and UNDECLARE ordering is irrelevant:
     * repl_apply_predef_ops runs independent passes per op kind. When
     * queued undeclares remove lower predef slots, rebase the staged
     * CMD_VAR_ASSIGN slot to the post-undeclare index before publish. */
    int op_count = out->predef_op_count;
    if (overwriting_decl) {
        for (int decl_idx = 0; decl_idx < old_decl->var_decl_count; decl_idx++) {
            const char *nm = old_decl->var_names[decl_idx];
            for (int cmd_idx = 0; cmd_idx < ctx->document_count; cmd_idx++) {
                if (cmd_idx == insert_idx) continue;
                const char *line = source_text_line(ctx->text, cmd_idx);
                if (repl_eval_source_uses_ident(line ? line : "", nm))
                    return compile_set_err(err, err_size,
                        "variable '%s' is in use, cannot overwrite", nm);
            }
            if (op_count >= MAX_PREDEF_OPS_PER_COMMIT) break;
            out->predef_ops[op_count].kind = REPL_PREDEF_OP_UNDECLARE;
            repl_copy_string_fits(out->predef_ops[op_count].name,
                                  sizeof(out->predef_ops[op_count].name), nm);
            op_count++;
        }
    }

    out->predef_op_count = op_count;
    if (cmd.type == CMD_VAR_ASSIGN && overwriting_decl) {
        int rebased_slot = compile_rebase_var_assign_slot_after_undeclares(
            name, cmd.var_idx, out);
        if (rebased_slot < 0)
            return compile_set_err(err, err_size,
                                   "cannot overwrite declaration of '%s' with assignment",
                                   name);
        cmd.var_idx = rebased_slot;
    }
    out->cmds[0] = cmd;

    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_set_predef_value(const char *name,
                                                float value,
                                                const ReplCompileContext *ctx,
                                                ReplCompiledChange *out,
                                                char *err, int err_size) {
    int var_idx;
    int literal_assign_idx;
    int has_any_assign;
    int decl_idx;

    if (!ctx || !out || !name || !name[0])
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);
    if (err && err_size > 0)
        err[0] = '\0';

    var_idx = repl_eval_find_predef_var_idx(name);
    if (var_idx < 0)
        return compile_set_err(err, err_size,
                               "undeclared variable '%s'", name);

    out->predef_ops[0].kind = REPL_PREDEF_OP_SET_VALUE;
    repl_copy_string_fits(out->predef_ops[0].name,
                          sizeof(out->predef_ops[0].name), name);
    out->predef_ops[0].value = value;
    out->predef_ops[0].has_value = 1;
    out->predef_op_count = 1;
    snprintf(out->commit_message, sizeof(out->commit_message),
             "%s = %g", name, (double)value);

    literal_assign_idx = compile_find_last_literal_assign(ctx, var_idx);
    if (literal_assign_idx >= 0) {
        if (!compile_build_literal_assign_change(ctx, literal_assign_idx,
                                                 name, value, out))
            return compile_set_err(err, err_size, "Command too long");
        return REPL_COMPILE_OK;
    }

    has_any_assign = compile_has_any_assign(ctx, var_idx);
    if (has_any_assign)
        return REPL_COMPILE_OK;

    decl_idx = compile_find_var_decl(ctx, name);
    if (decl_idx >= 0) {
        char rewritten[MAX_LINE_LEN];
        if (compile_rewrite_decl_initializer_text(
                source_text_line(ctx->text, decl_idx),
                name, value, rewritten, sizeof(rewritten))) {
            out->kind = REPL_COMPILED_REPLACE_ONE;
            out->pos = decl_idx;
            out->count = 1;
            out->adjust_edit_line = 0;
            out->cmds[0] = ctx->document_cmds[decl_idx];
            repl_copy_string_fits(out->text[0], sizeof(out->text[0]), rewritten);
        }
    }

    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_empty_line(int line_idx,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);
    if (err && err_size > 0)
        err[0] = '\0';

    if (line_idx < 0)
        line_idx = 0;
    if (line_idx > ctx->document_count)
        line_idx = ctx->document_count;

    memset(&out->cmds[0], 0, sizeof(out->cmds[0]));
    out->cmds[0].type = CMD_EMPTY;
    out->cmds[0].valid = 1;
    out->text[0][0] = '\0';

    out->kind = REPL_COMPILED_INSERT_ONE;
    out->pos = line_idx;
    out->count = 1;
    out->adjust_edit_line = 0;
    snprintf(out->commit_message, sizeof(out->commit_message),
             "Inserted blank line");
    return REPL_COMPILE_OK;
}

/* Walk [range_start, range_end) for CMD_VAR_DECLARE rows. For each
 * declared name:
 *   1. Verify no line outside the range still references it. CMD_COMMENT
 *      lines are skipped — a line like `// x axis` is not a real use of
 *      `x`, and counting it would block legitimate decl removals.
 *   2. Append a REPL_PREDEF_OP_UNDECLARE op to `out->predef_ops`.
 *
 * Returns REPL_COMPILE_OK on success (predef_op_count incremented).
 * Returns REPL_COMPILE_ERROR with `err` filled on still-referenced
 * name (`"Cannot %s '%s': still referenced"`, %s = action_verb) or
 * predef-op cap overflow. */
static ReplCompileResult compile_collect_undeclare_for_range(
        const ReplCompileContext *ctx,
        int range_start, int range_end,
        const char *action_verb,
        ReplCompiledChange *out, char *err, int err_size) {
    int n = ctx->document_count;

    /* Reference scan. Comments are not real references. */
    for (int i = range_start; i < range_end; i++) {
        const GLCmd *cmd = &ctx->document_cmds[i];
        if (cmd->type != CMD_VAR_DECLARE) continue;
        for (int d = 0; d < cmd->var_decl_count; d++) {
            const char *nm = cmd->var_names[d];
            for (int j = 0; j < n; j++) {
                if (j >= range_start && j < range_end) continue;
                if (ctx->document_cmds[j].type == CMD_COMMENT) continue;
                const char *line = source_text_line(ctx->text, j);
                if (line && repl_eval_source_uses_ident(line, nm))
                    return compile_set_err(err, err_size,
                                           "Cannot %s '%s': still referenced",
                                           action_verb, nm);
            }
        }
    }

    /* Append UNDECLARE ops for every declared name in the range. */
    for (int i = range_start; i < range_end; i++) {
        const GLCmd *cmd = &ctx->document_cmds[i];
        if (cmd->type != CMD_VAR_DECLARE) continue;
        for (int d = 0; d < cmd->var_decl_count; d++) {
            if (out->predef_op_count >= MAX_PREDEF_OPS_PER_COMMIT)
                return compile_set_err(err, err_size,
                                       "Too many declarations in range");
            ReplPredefOp *op = &out->predef_ops[out->predef_op_count++];
            op->kind = REPL_PREDEF_OP_UNDECLARE;
            repl_copy_string_fits(op->name, sizeof(op->name),
                                  cmd->var_names[d]);
        }
    }
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_delete_range(int start, int count,
                                            const ReplCompileContext *ctx,
                                            ReplCompiledChange *out,
                                            char *err, int err_size) {
    int n;
    int end;

    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);
    if (err && err_size > 0)
        err[0] = '\0';

    n = ctx->document_count;
    if (start < 0)
        start = 0;
    if (count <= 0 || start >= n)
        return REPL_COMPILE_OK;
    if (start + count > n)
        count = n - start;
    end = start + count;

    /* Reference check + UNDECLARE op collection. Shared with the
     * comment-toggle paths; comments are not real uses, so the
     * scanner skips CMD_COMMENT lines. */
    if (compile_collect_undeclare_for_range(ctx, start, end, "remove",
                                             out, err, err_size)
            != REPL_COMPILE_OK)
        return REPL_COMPILE_ERROR;

    out->kind = REPL_COMPILED_DELETE_RANGE;
    out->pos = start;
    out->count = count;
    snprintf(out->commit_message, sizeof(out->commit_message),
             "Removed %d line%s", count, count > 1 ? "s" : "");

    return REPL_COMPILE_OK;
}

/* Build "<leading_ws><prefix><rest_of_line>" into `dst`. Returns the
 * number of bytes written (excluding the NUL). Truncates safely if
 * the source overflows `cap`. */
static int compile_prepend_prefix(const char *orig, const char *prefix,
                                  char *dst, int cap) {
    int off = 0;
    int ws = 0;
    int prefix_len;

    if (cap <= 0) return 0;
    if (!orig) orig = "";
    if (!prefix) prefix = "";
    prefix_len = (int)strlen(prefix);

    while (orig[ws] && isspace((unsigned char)orig[ws]))
        ws++;
    for (int k = 0; k < ws && off < cap - 1; k++)
        dst[off++] = orig[k];
    for (int k = 0; k < prefix_len && off < cap - 1; k++)
        dst[off++] = prefix[k];
    for (int k = ws; orig[k] && off < cap - 1; k++)
        dst[off++] = orig[k];
    dst[off] = '\0';
    return off;
}

/* Strip the configured prefix from `orig`. Returns 1 if the line
 * begins with `prefix` after leading whitespace and the stripped
 * result was written to `dst`; 0 otherwise (line doesn't carry the
 * configured prefix). */
static int compile_strip_prefix(const char *orig, const char *prefix,
                                char *dst, int cap) {
    int ws = 0;
    int prefix_len;
    int off = 0;

    if (cap <= 0) return 0;
    if (!orig) orig = "";
    if (!prefix || !prefix[0]) return 0;
    prefix_len = (int)strlen(prefix);

    while (orig[ws] && isspace((unsigned char)orig[ws]))
        ws++;
    if (strncmp(orig + ws, prefix, (size_t)prefix_len) != 0)
        return 0;

    for (int k = 0; k < ws && off < cap - 1; k++)
        dst[off++] = orig[k];
    for (int k = ws + prefix_len; orig[k] && off < cap - 1; k++)
        dst[off++] = orig[k];
    dst[off] = '\0';
    return 1;
}

/* Scan backward from `end_idx` (a CMD_FOR_END / CMD_FUNC_END /
 * CMD_IF_END row) to find the matching block-head row. Returns the
 * head index or -1 if unmatched. */
static int compile_find_block_head(const ReplCompileContext *ctx,
                                   int end_idx) {
    int depth = 1;
    for (int j = end_idx - 1; j >= 0; j--) {
        CmdType t = ctx->document_cmds[j].type;
        if (repl_cmd_is_block_end(t)) {
            depth++;
        } else if (repl_cmd_is_block_head(t)) {
            depth--;
            if (depth == 0) return j;
        }
    }
    return -1;
}

ReplCompileResult repl_compile_toggle_comment(int line_idx,
                                              const char *prefix,
                                              const ReplCompileContext *ctx,
                                              ReplCompiledChange *out,
                                              char *err, int err_size) {
    CmdType type;
    int is_block_head;
    int is_block_end;

    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);
    if (err && err_size > 0)
        err[0] = '\0';

    if (!prefix || !prefix[0])
        return REPL_COMPILE_OK;
    if (line_idx < 0 || line_idx >= ctx->document_count)
        return REPL_COMPILE_OK;

    type = ctx->document_cmds[line_idx].type;
    is_block_head = repl_cmd_is_block_head(type);
    is_block_end  = repl_cmd_is_block_end(type);

    /* Block head/end: batch-comment the whole [head..end] range. */
    if (is_block_head || is_block_end) {
        int head;
        int end;
        int n;

        if (is_block_head) {
            head = line_idx;
            end = repl_source_scope_find_block_end(line_idx);
            if (end >= ctx->document_count)
                return compile_set_err(err, err_size, "Unmatched block start");
        } else {
            end = line_idx;
            head = compile_find_block_head(ctx, line_idx);
            if (head < 0)
                return compile_set_err(err, err_size, "Unmatched block end");
        }

        n = end - head + 1;
        if (n > MAX_COMMIT_CMDS)
            return compile_set_err(err, err_size,
                                   "Block too large to toggle (max %d lines)",
                                   MAX_COMMIT_CMDS);

        /* Decl-reference check + UNDECLARE op collection for any
         * CMD_VAR_DECLARE rows inside the block. Commenting a decl
         * out is symmetric to deleting it: references must be
         * removed first, and apply must undeclare the variable so
         * the runtime variable table matches the source. */
        if (compile_collect_undeclare_for_range(ctx, head, end + 1,
                                                 "comment",
                                                 out, err, err_size)
                != REPL_COMPILE_OK)
            return REPL_COMPILE_ERROR;

        for (int i = 0; i < n; i++) {
            const char *orig = source_text_line(ctx->text, head + i);
            compile_prepend_prefix(orig, prefix,
                                   out->text[i], (int)sizeof(out->text[i]));
            memset(&out->cmds[i], 0, sizeof(out->cmds[i]));
            out->cmds[i].type = CMD_COMMENT;
            out->cmds[i].valid = 1;
        }

        out->kind = REPL_COMPILED_INSERT_MANY;
        out->pos = head;
        out->count = n;
        out->delete_pos = head;
        out->delete_count = n;
        out->adjust_edit_line = 0;
        snprintf(out->commit_message, sizeof(out->commit_message),
                 "Commented out %d line%s", n, n > 1 ? "s" : "");
        return REPL_COMPILE_OK;
    }

    /* CMD_COMMENT: strip prefix and re-parse. */
    if (type == CMD_COMMENT) {
        const char *orig = source_text_line(ctx->text, line_idx);
        char stripped[MAX_LINE_LEN];
        ReplCompileResult r;

        if (!compile_strip_prefix(orig, prefix, stripped, sizeof(stripped)))
            return compile_set_err(err, err_size,
                                   "Line not commented with the configured prefix");

        /* Run the float-decl + var-assign dispatch chain. */
        r = repl_compile_dispatch(stripped, ctx, out, err, err_size);
        if (r != REPL_COMPILE_OK)
            return r;

        if (out->kind == REPL_COMPILED_NO_CHANGE) {
            /* Dispatch didn't recognize. Try the GL-command parser. */
            ReplParsedLine pl;
            char parser_err[REPL_STATUS_TEXT_MAX];
            ReplParseContext parse_ctx = {
                .source_line_idx = line_idx,
                .vars            = NULL,
                .num_vars        = 0,
                .strict_refs     = 0,
                .err_buf         = parser_err,
                .err_sz          = (int)sizeof(parser_err),
            };
            parser_err[0] = '\0';
            if (!repl_parser_parse_command_ctx(stripped, &pl, &parse_ctx))
                return compile_set_err(err, err_size,
                                       "Cannot uncomment: not a valid command");
            repl_compiled_change_init(out);
            out->cmds[0] = pl.cmd;
            repl_copy_string_fits(out->text[0], sizeof(out->text[0]), pl.text);
        }

        /* Coerce dispatch / parser result to REPLACE_ONE at line_idx. */
        if (out->kind == REPL_COMPILED_INSERT_MANY ||
            out->kind == REPL_COMPILED_DELETE_RANGE)
            return compile_set_err(err, err_size,
                                   "Cannot uncomment into multi-line construct");
        out->kind = REPL_COMPILED_REPLACE_ONE;
        out->pos = line_idx;
        out->count = 1;
        out->adjust_edit_line = 0;
        out->delete_pos = -1;
        out->delete_count = 0;
        snprintf(out->commit_message, sizeof(out->commit_message),
                 "Uncommented 1 line");
        return REPL_COMPILE_OK;
    }

    /* Plain non-comment, non-structural: prepend prefix → CMD_COMMENT. */
    {
        const char *orig = source_text_line(ctx->text, line_idx);

        /* If the line is a CMD_VAR_DECLARE, commenting it out must
         * also remove the variable from the predef table — otherwise
         * the live runtime keeps a name the source no longer
         * declares (and a save/reload would disagree). The shared
         * helper validates references and emits UNDECLARE ops with
         * the same shape delete-range uses. */
        if (compile_collect_undeclare_for_range(ctx, line_idx, line_idx + 1,
                                                 "comment",
                                                 out, err, err_size)
                != REPL_COMPILE_OK)
            return REPL_COMPILE_ERROR;

        compile_prepend_prefix(orig, prefix,
                               out->text[0], (int)sizeof(out->text[0]));
        memset(&out->cmds[0], 0, sizeof(out->cmds[0]));
        out->cmds[0].type = CMD_COMMENT;
        out->cmds[0].valid = 1;
        out->kind = REPL_COMPILED_REPLACE_ONE;
        out->pos = line_idx;
        out->count = 1;
        out->adjust_edit_line = 0;
        snprintf(out->commit_message, sizeof(out->commit_message),
                 "Commented out 1 line");
        return REPL_COMPILE_OK;
    }
}

/* ===== Pure structured-block validators =====
 * (implemented as step 5a of the decouple plan)
 *
 * These are the REPL-pipeline-side counterparts to the editor's
 * editor_compile_close_brace / _if_block / _func_def / _for_loop in
 * src/editor/commit.c. The editor versions handle edit-time semantics
 * (cursor target, insert mode, header-replace branch, oneliner body,
 * matched-existing-end close-brace). These pure versions cover only
 * the line-by-line load case the lean source loader (step 5b) needs:
 * single line of input → single CMD_* command appended.
 *
 * For the lean loader, ctx->edit_line == ctx->document_count and
 * ctx->insert_mode == 0; the new command lands at the end of the
 * document. The editor's edit-time branches don't arise here.
 *
 * Some parsing logic duplicates the editor versions. Step 5c (deferred)
 * can refactor the editor wrappers to call these pure versions and
 * attach editor effects on top.
 */

/* Compute the dedented (one-block-out) indent for a close-brace.
 * Mirror of close_brace_indent in src/editor/commit.c. */
static void compile_close_brace_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_cmd_indent(pos, buf, buf_sz);
    int len = (int)strlen(buf);
    if (len >= 2)
        len -= 2;
    else
        len = 0;
    if (len > buf_sz - 1)
        len = buf_sz - 1;
    memset(buf, ' ', (size_t)len);
    buf[len] = '\0';
}

ReplCompileResult repl_compile_close_brace(const char *input,
                                           const ReplCompileContext *ctx,
                                           ReplCompiledChange *out,
                                           char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '}') {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    int pos = compile_insert_pos(ctx);

    CmdType open_type = repl_source_scope_nearest_open_block_at(pos);
    CmdType end_type;
    if (open_type == CMD_FOR_BEGIN)      end_type = CMD_FOR_END;
    else if (open_type == CMD_FUNC_DEF)  end_type = CMD_FUNC_END;
    else if (open_type == CMD_IF_BEGIN)  end_type = CMD_IF_END;
    else {
        if (err && err_size > 0)
            snprintf(err, (size_t)err_size, "unmatched '}'");
        return REPL_COMPILE_ERROR;
    }

    /* Matched-existing-end branch: close-brace lands on a row that's
     * already the right end marker. No source mutation. (Editor cursor
     * effects live on the editor wrapper.) */
    if (pos < ctx->document_count &&
        ctx->document_cmds[pos].type == end_type) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    /* Insert-new-end-marker branch. */
    char indent[REPL_INDENT_TEXT_MAX];
    compile_close_brace_indent(pos, indent, sizeof(indent));

    GLCmd fe;
    memset(&fe, 0, sizeof(fe));
    fe.type = end_type;
    fe.valid = 1;

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = fe;
    snprintf(out->text[0], sizeof(out->text[0]), "%s}", indent);
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_if_block(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "if(", 3) != 0 && strncmp(p, "if (", 4) != 0) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    int pos = compile_insert_pos(ctx);

    ExprVar visible_vars[MAX_EXPR_VARS];
    int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS, NULL);

    /* Skip past `if` to the opening `(`. */
    while (*p && *p != '(') p++;
    if (!*p) {
        snprintf(err, (size_t)err_size, "if syntax: if(expr) {");
        return REPL_COMPILE_ERROR;
    }
    p++;

    char cond_text[MAX_LINE_LEN];
    int  paren = 1;
    const char *expr_start = p;
    while (*p && paren > 0) {
        if (*p == '(')      paren++;
        else if (*p == ')') paren--;
        if (paren > 0) p++;
    }
    if (paren != 0) {
        snprintf(err, (size_t)err_size, "if syntax: if(expr) {");
        return REPL_COMPILE_ERROR;
    }
    int clen = (int)(p - expr_start);
    if (clen > (int)sizeof(cond_text) - 1)
        clen = (int)sizeof(cond_text) - 1;
    memcpy(cond_text, expr_start, (size_t)clen);
    cond_text[clen] = '\0';

    {
        char verr[128];
        if (!repl_eval_validate_expression_idents(cond_text,
                                                  visible_nv > 0 ? visible_vars : NULL,
                                                  visible_nv,
                                                  verr, sizeof(verr))) {
            snprintf(err, (size_t)err_size, "%s", verr);
            return REPL_COMPILE_ERROR;
        }
    }

    float cond_val = 0.0f;
    {
        float cond_args[1];
        int neval = repl_eval_parse_exprs(cond_text, cond_args, 1,
                                          visible_nv > 0 ? visible_vars : NULL,
                                          visible_nv);
        cond_val = (neval >= 1) ? cond_args[0] : 0.0f;
    }

    /* Skip `)`; trailing `{` is optional for the lean loader. */
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' && *p != '\0') {
        snprintf(err, (size_t)err_size, "if syntax: if(expr) {");
        return REPL_COMPILE_ERROR;
    }

    char indent[REPL_INDENT_TEXT_MAX];
    repl_source_scope_cmd_indent(pos, indent, sizeof(indent));

    GLCmd ib;
    memset(&ib, 0, sizeof(ib));
    ib.type = CMD_IF_BEGIN;
    ib.args[0] = cond_val;
    ib.valid = 1;
    ib.has_vars = input_has_any_visible_vars(cond_text, visible_vars, visible_nv);

    /* Trim cond_text in place for canonical formatting. */
    char *ct = cond_text;
    int ctlen;
    while (*ct && isspace((unsigned char)*ct)) ct++;
    ctlen = (int)strlen(ct);
    while (ctlen > 0 && isspace((unsigned char)ct[ctlen - 1]))
        ct[--ctlen] = '\0';

    char ib_text[MAX_LINE_LEN];
    if (!repl_format_fits(ib_text, sizeof(ib_text), "%sif(%s) {", indent, ct)) {
        snprintf(err, (size_t)err_size, "Command too long");
        return REPL_COMPILE_ERROR;
    }

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = ib;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", ib_text);
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_func_def(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    /* Quick-reject inputs that look like function calls (have `(` and
     * no `{`). They go through the normal command parser. */
    const char *trimmed = input ? input : "";
    while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
    if (strchr(trimmed, '(') != NULL && strchr(trimmed, '{') == NULL) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    /* Alias pre-registration for new user names (e.g. `drawCube { ... }`).
     *
     * parse_repl_func_signature recognises the bare `funcN` form plus
     * already-registered aliases, so to parse a brand-new identifier
     * we have to register the alias before calling the parser. That
     * mutates global state during what's nominally a pure compile, so:
     *
     *   - record the slot we registered and its previous contents
     *   - if the parse / duplicate-check / format steps below fail,
     *     restore the previous alias state before returning so the
     *     global table doesn't leak a name with no matching CMD_FUNC_DEF
     *     or lose an existing alias during a failed rewrite.
     *
     * [P1] regression: previously the alias_set call could leak on
     * parse failure (or apply failure further upstream); the rollback
     * below covers parse-side failures and the published rollback
     * fields cover downstream preflight/apply failures. */
    int newly_aliased_slot = -1;
    {
        const char *p = trimmed;
        if (!(strncmp(p, "func", 4) == 0 && p[4] >= '0' && p[4] <= '9' &&
              !isalnum((unsigned char)p[5]) && p[5] != '_') &&
            (isalpha((unsigned char)*p) || *p == '_')) {
            char ident[REPL_FUNC_NAME_MAX];
            int len = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
                if (len >= REPL_FUNC_NAME_MAX - 1) { ident[0] = '\0'; break; }
                ident[len++] = *p++;
            }
            if (len > 0) {
                ident[len] = '\0';
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p == '{' || *p == '(') {
                    int existing = repl_func_alias_lookup_slot(ident);
                    if (existing < 0) {
                        if (!repl_func_alias_name_is_valid(ident)) {
                            out->kind = REPL_COMPILED_NO_CHANGE;
                            return REPL_COMPILE_OK;
                        }
                        int target_slot = repl_func_alias_first_free_slot();
                        if (target_slot < 0) {
                            snprintf(err, (size_t)err_size,
                                     "no free function slots (max %d)",
                                     REPL_FUNC_SLOT_COUNT);
                            return REPL_COMPILE_ERROR;
                        }
                        char prev_alias[REPL_FUNC_NAME_MAX] = "";
                        const char *prev_alias_live = repl_func_alias_get(target_slot);
                        if (prev_alias_live)
                            snprintf(prev_alias, sizeof(prev_alias),
                                     "%s", prev_alias_live);
                        if (!repl_func_alias_set(target_slot, ident)) {
                            snprintf(err, (size_t)err_size,
                                     "name '%s' already used", ident);
                            return REPL_COMPILE_ERROR;
                        }
                        newly_aliased_slot = target_slot;
                        out->newly_aliased_slot = target_slot;
                        out->had_previous_alias = prev_alias[0] ? 1 : 0;
                        if (out->had_previous_alias)
                            snprintf(out->previous_alias,
                                     sizeof(out->previous_alias),
                                     "%s", prev_alias);
                    }
                }
            }
        }
    }

    int fn = -1;
    int param_count = 0;
    char param_names[MAX_EXPR_VARS][16];
    if (!parse_repl_func_signature(input ? input : "", &fn,
                                   param_names, MAX_EXPR_VARS, &param_count)) {
        repl_compiled_change_rollback_alias(out);
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    /* Reject duplicate funcN definitions: an existing CMD_FUNC_DEF for
     * the same fn anywhere in the document means the new line is a
     * second definition that would shadow the first. The editor's
     * editor_compile_func_def has the same check (src/editor/commit.c
     * lines 670-681) so editor_feed_line rejects this; the lean loader path
     * needs to match.
     *
     * Note: this validator is the line-by-line load case, so we don't
     * have an "overwriting an existing func line" branch (the editor
     * does, for in-place renames). [P2] regression. */
    for (int ei = 0; ei < ctx->document_count; ei++) {
        const GLCmd *c = &ctx->document_cmds[ei];
        if (!c->valid) continue;
        if (c->type != CMD_FUNC_DEF) continue;
        if ((int)c->args[0] != fn) continue;
        snprintf(err, (size_t)err_size,
                 "func%d already defined (line %d)", fn, ei + 1);
        repl_compiled_change_rollback_alias(out);
        return REPL_COMPILE_ERROR;
    }

    int pos = compile_insert_pos(ctx);

    char indent[REPL_INDENT_TEXT_MAX];
    repl_source_scope_cmd_indent(pos, indent, sizeof(indent));

    GLCmd fd;
    memset(&fd, 0, sizeof(fd));
    fd.type = CMD_FUNC_DEF;
    fd.args[0] = (float)fn;
    fd.num_args = param_count;
    fd.valid = 1;

    char fd_text[MAX_LINE_LEN];
    format_func_header(fd_text, (int)sizeof(fd_text),
                       indent, fn, param_names, param_count);

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = fd;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", fd_text);
    /* Publish the alias slot (if any) so callers can roll back the
     * registration on preflight/apply failure downstream. */
    out->newly_aliased_slot = newly_aliased_slot;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_for_loop(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "for(", 4) != 0 && strncmp(p, "for (", 5) != 0) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    int pos = compile_insert_pos(ctx);

    ExprVar visible_vars[MAX_EXPR_VARS];
    int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS, NULL);

    char var_name[REPL_PREDEF_NAME_MAX];
    float start, end, step;
    const char *body_start = NULL;
    if (!repl_eval_parse_for_header_with_vars(p, var_name, sizeof(var_name),
                                              &start, &end, &step,
                                              visible_vars, visible_nv,
                                              &body_start)) {
        snprintf(err, (size_t)err_size,
                 "for syntax: for(var, start, end[, step]) body;");
        return REPL_COMPILE_ERROR;
    }

    char indent[REPL_INDENT_TEXT_MAX];
    repl_source_scope_cmd_indent(pos, indent, sizeof(indent));

    GLCmd fb;
    memset(&fb, 0, sizeof(fb));
    fb.type = CMD_FOR_BEGIN;
    fb.args[0] = start;
    fb.args[1] = end;
    fb.args[2] = step;
    fb.valid = 1;

    /* Re-walk the input to extract the args expression text so the
     * formatted line preserves the user's symbolic form when args
     * reference visible vars. Same approach as editor_compile_for_loop. */
    char fb_text[MAX_LINE_LEN];
    {
        const char *raw = p;
        while (*raw && *raw != '(') raw++;
        if (*raw) raw++;
        while (*raw && isspace((unsigned char)*raw)) raw++;
        while (*raw && (isalnum((unsigned char)*raw) || *raw == '_')) raw++;
        while (*raw && isspace((unsigned char)*raw)) raw++;
        if (*raw == ',') raw++;

        const char *args_start = raw;
        int paren = 1;
        const char *ap = args_start;
        while (*ap && paren > 0) {
            if (*ap == '(')      paren++;
            else if (*ap == ')') paren--;
            if (paren > 0) ap++;
        }
        int rlen = (int)(ap - args_start);
        char raw_args[MAX_LINE_LEN];
        if (rlen > (int)sizeof(raw_args) - 1)
            rlen = (int)sizeof(raw_args) - 1;
        memcpy(raw_args, args_start, (size_t)rlen);
        raw_args[rlen] = '\0';
        while (rlen > 0 && isspace((unsigned char)raw_args[rlen - 1]))
            raw_args[--rlen] = '\0';

        char *ra = raw_args;
        while (*ra && isspace((unsigned char)*ra)) ra++;

        char verr[128];
        if (!repl_eval_validate_expression_idents(ra, visible_vars, visible_nv,
                                                  verr, sizeof(verr))) {
            snprintf(err, (size_t)err_size, "%s", verr);
            return REPL_COMPILE_ERROR;
        }

        if (input_has_any_visible_vars(ra, visible_vars, visible_nv)) {
            fb.has_vars = 1;
            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                  "%sfor(%s, %s) {", indent, var_name, ra)) {
                snprintf(err, (size_t)err_size, "Command too long");
                return REPL_COMPILE_ERROR;
            }
        } else if (step != 1.0f) {
            char start_buf[32];
            char end_buf[32];
            char step_buf[32];
            repl_format_source_float(start_buf, sizeof(start_buf), start);
            repl_format_source_float(end_buf, sizeof(end_buf), end);
            repl_format_source_float(step_buf, sizeof(step_buf), step);
            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                  "%sfor(%s, %s, %s, %s) {",
                                  indent, var_name, start_buf, end_buf, step_buf)) {
                snprintf(err, (size_t)err_size, "Command too long");
                return REPL_COMPILE_ERROR;
            }
        } else {
            char start_buf[32];
            char end_buf[32];
            repl_format_source_float(start_buf, sizeof(start_buf), start);
            repl_format_source_float(end_buf, sizeof(end_buf), end);
            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                  "%sfor(%s, %s, %s) {",
                                  indent, var_name, start_buf, end_buf)) {
                snprintf(err, (size_t)err_size, "Command too long");
                return REPL_COMPILE_ERROR;
            }
        }
    }

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = fb;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", fb_text);
    return REPL_COMPILE_OK;
}

/* repl_load_apply_line moved to src/repl/load.c after a [P2] review
 * finding: this file is the pure-validator module per the contract
 * at the top, and apply orchestration (writing the command store /
 * editor buffer / predef-var registrations) belongs elsewhere. */
