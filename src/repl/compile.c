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
 * orchestrated by the editor commit path.
 *
 * Reading guide — entry points and what they emit:
 *   repl_compile_float_decl         INSERT_ONE / REPLACE_ONE  +
 *                                   DECLARE / UNDECLARE / SET_VALUE
 *   repl_compile_var_assign         INSERT_ONE / REPLACE_ONE  +
 *                                   SET_VALUE (+ UNDECLARE on
 *                                   decl-row overwrite)
 *   repl_compile_set_predef_value   REPLACE_ONE on the decl
 *                                   initializer that backs the named
 *                                   variable; pure SET_VALUE if no
 *                                   declaration exists
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

#include "repl/core_internal.h"  /* repl_extract_assignment_parts, collect_visible_vars */
#include "repl/source_scope.h"   /* repl_source_scope_cmd_indent, _find_block_end */
#include "repl/state_owners.h"
#include "repl/util.h"            /* repl_format_fits / repl_copy_string_fits */

#include "config.h"              /* REPL_INDENT_TEXT_MAX */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "source_document.h"
#include <string.h>

ReplCompileResult repl_compile_dispatch(const char *text,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!out || !ctx) return REPL_COMPILE_ERROR;

    /* Canonical order — mirrors editor_try_commit_any. Each entry
     * is a per-kind compile validator that returns NO_CHANGE when
     * the input doesn't match its grammar, OK + a populated
     * ReplCompiledChange when it does, or ERROR on syntax failure.
     * The first match wins.
     *
     * Ordering is load-bearing: float_decl must come before
     * var_assign (otherwise `float x` would misread as an
     * assignment to identifier `float`); close_brace must come
     * before the three block openers (so a `}` line lands on the
     * close-brace branch). */
    static const struct {
        ReplCompileResult (*fn)(const char *, const ReplCompileContext *,
                                ReplCompiledChange *, char *, int);
    } chain[] = {
        { repl_compile_float_decl  },
        { repl_compile_var_assign  },
        { repl_compile_close_brace },
        { repl_compile_for_loop    },
        { repl_compile_func_def    },
        { repl_compile_if_block    },
    };

    for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); i++) {
        ReplCompileResult r = chain[i].fn(text, ctx, out, err, err_size);
        if (r == REPL_COMPILE_ERROR) return r;
        if (out->kind != REPL_COMPILED_NO_CHANGE) return REPL_COMPILE_OK;
    }

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
    out->alias_op.slot = -1;
    out->alias_op.name[0] = '\0';
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
    /* insert_mode defaults to 0 (overwrite mode). Callers with insert
     * semantics (the editor commit pipeline, repl_load_apply_line)
     * overwrite ctx.insert_mode after this returns. Insert mode is
     * caller/editor state, not REPL state, so the REPL pipeline doesn't
     * reach for it.
     *
     * edit_line_idx is supplied by the caller because the REPL
     * pipeline does not reach into editor_state_* for the cursor. */
    ReplCompileContext ctx = {
        .edit_line       = edit_line_idx,
        .document_count  = repl_state_document_count(),
        .insert_mode     = 0,
        .text            = source_document_view(),
        .document_cmds   = repl_state_document_cmds(),
    };
    return ctx;
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

static int compile_name_is_active_func_param(const ReplCompileContext *ctx,
                                             int pos,
                                             const char *name) {
    typedef struct {
        CmdType type;
        char params[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
        int param_count;
    } ScopeFrame;

    ScopeFrame frames[64] = {0};
    int depth = 0;

    if (!ctx || !ctx->document_cmds || !name || !name[0])
        return 0;

    for (int cmd_idx = 0; cmd_idx < pos && cmd_idx < ctx->document_count; cmd_idx++) {
        CmdType type = ctx->document_cmds[cmd_idx].type;

        if (repl_cmd_is_block_head(type)) {
            if (depth >= (int)(sizeof(frames) / sizeof(frames[0])))
                break;

            frames[depth].type = type;
            frames[depth].param_count = 0;

            if (type == CMD_FUNC_DEF) {
                int parsed_fn = -1;
                int param_count = 0;
                const char *func_text = source_text_line(ctx->text, cmd_idx);

                if (parse_repl_func_signature(func_text ? func_text : "",
                                              &parsed_fn,
                                              frames[depth].params,
                                              MAX_EXPR_VARS,
                                              &param_count)) {
                    frames[depth].param_count = param_count;
                }
            }

            depth++;
        } else if (repl_cmd_is_block_end(type)) {
            if (depth > 0)
                depth--;
        }
    }

    for (int depth_idx = depth - 1; depth_idx >= 0; depth_idx--) {
        if (frames[depth_idx].type != CMD_FUNC_DEF)
            continue;
        for (int param_idx = 0; param_idx < frames[depth_idx].param_count; param_idx++) {
            if (strcmp(frames[depth_idx].params[param_idx], name) == 0)
                return 1;
        }
    }

    return 0;
}

/* A same-name local (function param / for-loop var) shadows a global on
 * that entire source line, including the binder line itself. Use
 * collect_visible_vars(line_idx + 1) so `func0(x) {` and `for(x, ...) {`
 * treat `x` as already bound for the current-line reference scan. */
static int compile_line_uses_global_ident(const ReplCompileContext *ctx,
                                          int line_idx,
                                          const char *name) {
    const char *line;
    ExprVar visible_vars[MAX_EXPR_VARS];
    int visible_nv;

    if (!ctx || !name || !name[0] ||
        line_idx < 0 || line_idx >= ctx->document_count)
        return 0;

    line = source_text_line(ctx->text, line_idx);
    if (!line || !repl_eval_source_uses_ident(line, name))
        return 0;

    visible_nv = collect_visible_vars(line_idx + 1, visible_vars,
                                      MAX_EXPR_VARS, NULL);
    for (int var_idx = 0; var_idx < visible_nv; var_idx++) {
        if (strcmp(visible_vars[var_idx].name, name) == 0)
            return 0;
    }
    return 1;
}

static int compile_name_is_still_referenced(const ReplCompileContext *ctx,
                                            const char *name,
                                            int skip_start,
                                            int skip_end) {
    if (!ctx || !name || !name[0])
        return 0;

    for (int cmd_idx = 0; cmd_idx < ctx->document_count; cmd_idx++) {
        if (cmd_idx >= skip_start && cmd_idx < skip_end)
            continue;
        if (compile_line_uses_global_ident(ctx, cmd_idx, name))
            return 1;
    }
    return 0;
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

static int compile_find_var_decl(const ReplCompileContext *ctx,
                                 const char *name) {
    if (!ctx || !ctx->document_cmds || !name || !name[0])
        return -1;

    for (int cmd_idx = 0; cmd_idx < ctx->document_count; cmd_idx++) {
        const GLCmd *cmd = &ctx->document_cmds[cmd_idx];
        if (!cmd->valid || cmd->type != CMD_VAR_DECLARE)
            continue;
        for (int decl_idx = 0; decl_idx < cmd->payload.decl.count; decl_idx++) {
            if (strcmp(cmd->payload.decl.names[decl_idx], name) == 0)
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
        /* A trailing `// comment` ends the name list just like `;` or
         * end-of-string — the comment-capture below reads it. This lets
         * an interactive decl carry a comment without a typed semicolon
         * (`float n // @tune`), since the `;` key commits before the user
         * can type one; mirrors repl_compile_var_assign's `//` handling. */
        if (*p == ';' || *p == '\0' || (p[0] == '/' && p[1] == '/')) break;
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
         * outer name loop picks up the next decl, and at a top-level
         * `//` so `float n = 1 // @tune` keeps the comment out of the
         * initializer. `==` is left for the eval validator to reject —
         * a literal `=` followed by `=` is not a decl initializer. */
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
                else if (p[0] == '/' && p[1] == '/' && depth == 0) break;
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

            char verr[REPL_DIAG_TEXT_MAX];
            if (!repl_eval_validate_expression_idents(init_expr, NULL, 0,
                                                      verr, sizeof(verr)))
                return compile_set_err(err, err_size, "%s", verr);
            ExprCtx eval_ctx = { init_expr, g_predef_vars, g_num_predef_vars, NULL, 0 };
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
                for (int d = 0; d < old_decl->payload.decl.count; d++) {
                    if (strcmp(old_decl->payload.decl.names[d], nm) == 0) {
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

    int old_count = old_decl ? old_decl->payload.decl.count : 0;
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
        for (int d = 0; d < old_decl->payload.decl.count; d++) {
            const char *nm = old_decl->payload.decl.names[d];
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
        for (int d = 0; d < old_decl->payload.decl.count; d++) {
            const char *nm = old_decl->payload.decl.names[d];
            int kept = 0;
            for (int v = 0; v < parsed.count; v++) {
                if (strcmp(parsed.names[v], nm) == 0) { kept = 1; break; }
            }
            if (kept) continue;
            if (compile_name_is_still_referenced(ctx, nm, insert_idx,
                                                 insert_idx + 1))
                return compile_set_err(err, err_size,
                    "variable '%s' is in use, cannot overwrite", nm);
        }
    }

    /* Build the GLCmd that backs the source row. */
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_VAR_DECLARE;
    cmd.valid = 1;
    cmd.payload.decl.count = parsed.count;
    for (int v = 0; v < parsed.count; v++) {
        if (!repl_copy_string_fits(cmd.payload.decl.names[v],
                                   sizeof(cmd.payload.decl.names[v]),
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

ReplCompileResult repl_compile_split_decl(const ReplCompileContext *ctx,
                                          int line_idx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);

    if (line_idx < 0 || line_idx >= ctx->document_count ||
        ctx->document_cmds[line_idx].type != CMD_VAR_DECLARE) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    /* Recover names + initializers + trailing comment from the line's
     * canonical source text. A committed decl stores evaluated literals
     * (commit already evaluated any init expression), so re-parsing and
     * re-emitting is lossless. */
    const char *line = source_text_line(ctx->text, line_idx);
    FloatDeclParse parsed;
    int recognized = 0;
    ReplCompileResult r =
        parse_float_name_list(line ? line : "", &parsed, &recognized,
                              err, err_size);
    if (r != REPL_COMPILE_OK)
        return r;
    if (!recognized || parsed.count < 2) {
        /* Single-name (or unparseable) decl: nothing to split. */
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }
    if (parsed.count > MAX_COMMIT_CMDS)
        return compile_set_err(err, err_size,
            "too many names to split (%d > %d)", parsed.count, MAX_COMMIT_CMDS);

    for (int i = 0; i < parsed.count; i++) {
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type  = CMD_VAR_DECLARE;
        cmd.valid = 1;
        cmd.payload.decl.count = 1;
        repl_copy_string_fits(cmd.payload.decl.names[0],
                              sizeof(cmd.payload.decl.names[0]),
                              parsed.names[i]);
        out->cmds[i] = cmd;

        /* A single-name view drives format_decl_text so each line gets
         * its own `= value`; the original line's trailing comment rides
         * the first line only, leaving the rest clean for per-variable
         * // @tune tags. */
        FloatDeclParse one;
        memset(&one, 0, sizeof(one));
        one.count = 1;
        repl_copy_string_fits(one.names[0], sizeof(one.names[0]),
                              parsed.names[i]);
        one.has_init[0]  = parsed.has_init[i];
        one.init_vals[0] = parsed.init_vals[i];
        if (i == 0)
            repl_copy_string_fits(one.decl_comment, sizeof(one.decl_comment),
                                  parsed.decl_comment);
        format_decl_text(&one, out->text[i], (int)sizeof(out->text[i]));
    }

    /* Replace the one decl line in place: delete it, then insert the N
     * single-name decls at the same index. `pos` is in post-delete
     * coordinates (compile.h convention); deleting one line at line_idx
     * leaves line_idx as the insert point. No predef ops — the
     * variables stay declared with their current values. */
    out->kind             = REPL_COMPILED_INSERT_MANY;
    out->pos              = line_idx;
    out->count            = parsed.count;
    out->delete_pos       = line_idx;
    out->delete_count     = 1;
    out->adjust_edit_line = 0;

    snprintf(out->commit_message, sizeof(out->commit_message),
             "Split declaration into %d lines", parsed.count);
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

    /* Bail out before extraction when the input itself can't fit in a
     * source line. Otherwise repl_extract_assignment_target_parts would
     * silently truncate rhs into the MAX_LINE_LEN-sized buffer, which
     * can land mid-token (e.g. inside `+(0)`) and trip the validator
     * with a spurious "missing ')'" before the format-fits step gets
     * to emit the real diagnostic. */
    if (input && (int)strlen(input) >= MAX_LINE_LEN)
        return compile_set_err(err, err_size, "Command too long");

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
    char verr[REPL_DIAG_TEXT_MAX];
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
        if (compile_name_is_active_func_param(ctx, insert_idx, name))
            return compile_set_err(err, err_size,
                                   "cannot assign to function parameter '%s' - function parameters are constant",
                                   name);

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
    repl_source_scope_cmd_indent(insert_idx, indent, sizeof(indent));
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
        for (int decl_idx = 0; decl_idx < old_decl->payload.decl.count; decl_idx++) {
            const char *nm = old_decl->payload.decl.names[decl_idx];
            if (compile_name_is_still_referenced(ctx, nm, insert_idx,
                                                 insert_idx + 1))
                return compile_set_err(err, err_size,
                    "variable '%s' is in use, cannot overwrite", nm);
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
 *   1. Verify no line outside the range still references it. Comments are
 *      ignored, and same-name locals (function params / for-loop vars)
 *      shadow the global on their header and body lines.
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
    /* Reference scan. */
    for (int i = range_start; i < range_end; i++) {
        const GLCmd *cmd = &ctx->document_cmds[i];
        if (cmd->type != CMD_VAR_DECLARE) continue;
        for (int d = 0; d < cmd->payload.decl.count; d++) {
            const char *nm = cmd->payload.decl.names[d];
            if (compile_name_is_still_referenced(ctx, nm, range_start, range_end))
                return compile_set_err(err, err_size,
                                       "Cannot %s '%s': still referenced",
                                       action_verb, nm);
        }
    }

    /* Append UNDECLARE ops for every declared name in the range. */
    for (int i = range_start; i < range_end; i++) {
        const GLCmd *cmd = &ctx->document_cmds[i];
        if (cmd->type != CMD_VAR_DECLARE) continue;
        for (int d = 0; d < cmd->payload.decl.count; d++) {
            if (out->predef_op_count >= MAX_PREDEF_OPS_PER_COMMIT)
                return compile_set_err(err, err_size,
                                       "Too many declarations in range");
            ReplPredefOp *op = &out->predef_ops[out->predef_op_count++];
            op->kind = REPL_PREDEF_OP_UNDECLARE;
            repl_copy_string_fits(op->name, sizeof(op->name),
                                  cmd->payload.decl.names[d]);
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
            /* Dispatch didn't recognize. Go through the normal commit
             * pipeline so visible-variable references in the stripped
             * text are preserved (otherwise `// glVertex3f(t, 0, 0)`
             * round-trips back as `glVertex3f(0.0000, 0, 0)` — the
             * parser's canonical text emits args from cmd->args[]
             * regardless of has_vars). */
            ExprVar vis[MAX_EXPR_VARS];
            int vis_n = collect_visible_vars(line_idx, vis, MAX_EXPR_VARS, NULL);
            int preserve_expr = input_has_any_visible_vars(stripped,
                                                           vis_n > 0 ? vis : NULL,
                                                           vis_n);
            GLCmd parsed_cmd;
            char parsed_text[MAX_LINE_LEN];
            memset(&parsed_cmd, 0, sizeof(parsed_cmd));
            parsed_text[0] = '\0';
            if (!repl_parse_and_normalize_strict(stripped, line_idx,
                                                 vis_n > 0 ? vis : NULL, vis_n,
                                                 preserve_expr, &parsed_cmd,
                                                 parsed_text, sizeof(parsed_text)))
                return compile_set_err(err, err_size,
                                       "Cannot uncomment: not a valid command");
            repl_compiled_change_init(out);
            out->cmds[0] = parsed_cmd;
            repl_copy_string_fits(out->text[0], sizeof(out->text[0]), parsed_text);
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

ReplCompileResult repl_compile_close_brace_kernel(const char *input,
                                                  const ReplCompileContext *ctx,
                                                  ReplCloseBraceKernel *out,
                                                  char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    memset(out, 0, sizeof(*out));

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '}') {
        out->valid = 0;
        return REPL_COMPILE_OK;
    }

    out->pos = compile_insert_pos(ctx);

    out->open_type = repl_source_scope_nearest_open_block_at(out->pos);
    if (out->open_type == CMD_FOR_BEGIN)      out->end_type = CMD_FOR_END;
    else if (out->open_type == CMD_FUNC_DEF)  out->end_type = CMD_FUNC_END;
    else if (out->open_type == CMD_IF_BEGIN)  out->end_type = CMD_IF_END;
    else {
        if (err && err_size > 0)
            snprintf(err, (size_t)err_size, "unmatched '}'");
        return REPL_COMPILE_ERROR;
    }

    /* Matched-existing-end branch: close-brace lands on a row that's
     * already the right end marker. */
    if (out->pos < ctx->document_count &&
        ctx->document_cmds[out->pos].type == out->end_type) {
        out->matched_existing = 1;
        out->valid = 1;
        return REPL_COMPILE_OK;
    }

    /* Insert-new-end-marker branch. */
    char indent[REPL_INDENT_TEXT_MAX];
    compile_close_brace_indent(out->pos, indent, sizeof(indent));

    out->fe.type  = out->end_type;
    out->fe.valid = 1;
    snprintf(out->fe_text, sizeof(out->fe_text), "%s}", indent);
    out->matched_existing = 0;
    out->valid = 1;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_close_brace(const char *input,
                                           const ReplCompileContext *ctx,
                                           ReplCompiledChange *out,
                                           char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    ReplCloseBraceKernel kernel;
    ReplCompileResult r = repl_compile_close_brace_kernel(input, ctx, &kernel,
                                                          err, err_size);
    if (r != REPL_COMPILE_OK) return r;
    if (!kernel.valid || kernel.matched_existing) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = kernel.pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = kernel.fe;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", kernel.fe_text);
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_if_block_kernel(const char *input,
                                               const ReplCompileContext *ctx,
                                               ReplIfBlockKernel *out,
                                               char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    memset(out, 0, sizeof(*out));

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "if(", 3) != 0 && strncmp(p, "if (", 4) != 0) {
        out->valid = 0;
        return REPL_COMPILE_OK;
    }

    out->pos = compile_insert_pos(ctx);

    ExprVar visible_vars[MAX_EXPR_VARS];
    int visible_nv = collect_visible_vars(out->pos, visible_vars,
                                          MAX_EXPR_VARS, NULL);

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

    char verr[REPL_DIAG_TEXT_MAX];
    if (!repl_eval_validate_expression_idents(cond_text,
                                              visible_nv > 0 ? visible_vars : NULL,
                                              visible_nv,
                                              verr, sizeof(verr))) {
        snprintf(err, (size_t)err_size, "%s", verr);
        return REPL_COMPILE_ERROR;
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

    repl_source_scope_cmd_indent(out->pos, out->indent, sizeof(out->indent));

    out->ib.type    = CMD_IF_BEGIN;
    out->ib.args[0] = cond_val;
    out->ib.valid   = 1;
    out->ib.has_vars = input_has_any_visible_vars(cond_text,
                                                  visible_vars, visible_nv);

    /* Trim cond_text in place for canonical formatting. */
    char *ct = cond_text;
    int ctlen;
    while (*ct && isspace((unsigned char)*ct)) ct++;
    ctlen = (int)strlen(ct);
    while (ctlen > 0 && isspace((unsigned char)ct[ctlen - 1]))
        ct[--ctlen] = '\0';

    if (!repl_format_fits(out->ib_text, sizeof(out->ib_text),
                          "%sif(%s) {", out->indent, ct)) {
        snprintf(err, (size_t)err_size, "Command too long");
        return REPL_COMPILE_ERROR;
    }

    out->valid = 1;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_if_block(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    ReplIfBlockKernel kernel;
    ReplCompileResult r = repl_compile_if_block_kernel(input, ctx, &kernel,
                                                       err, err_size);
    if (r != REPL_COMPILE_OK) return r;
    if (!kernel.valid) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = kernel.pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = kernel.ib;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", kernel.ib_text);
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_func_def_resolve_alias(const ReplCompileContext *ctx,
                                                      const char *trimmed,
                                                      ReplCompiledChange *out,
                                                      int *rejected_keyword,
                                                      char *err, int err_size) {
    if (rejected_keyword)
        *rejected_keyword = 0;

    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    const char *p = trimmed;
    if (!(strncmp(p, "func", 4) == 0 && p[4] >= '0' && p[4] <= '9' &&
          !repl_eval_is_ident_continue((unsigned char)p[5])) &&
        repl_eval_is_ident_start((unsigned char)*p)) {
        char ident[REPL_FUNC_NAME_MAX];
        int len = 0;
        while (*p && repl_eval_is_ident_continue((unsigned char)*p)) {
            if (len >= REPL_FUNC_NAME_MAX - 1) { ident[0] = '\0'; break; }
            ident[len++] = *p++;
        }
        if (len > 0) {
            ident[len] = '\0';
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '{' || *p == '(') {
                int existing = repl_func_alias_lookup_slot(ident);
                if (existing < 0) {
                    /* Reject reserved / control-flow names by returning REPL_COMPILE_OK
                     * and setting out->kind = REPL_COMPILED_NO_CHANGE so the next
                     * commit handler in the chain (if-block, close-brace, etc.) can claim the input. */
                    if (!repl_func_alias_name_is_valid(ident)) {
                        out->kind = REPL_COMPILED_NO_CHANGE;
                        if (rejected_keyword)
                            *rejected_keyword = 1;
                        return REPL_COMPILE_OK;
                    }
                    int target_slot = -1;
                    int ep = ctx->insert_mode ? ctx->edit_line :
                             (ctx->edit_line < ctx->document_count
                                  ? ctx->edit_line : ctx->document_count);
                    if (!ctx->insert_mode &&
                        ep < ctx->document_count &&
                        ctx->document_cmds[ep].type == CMD_FUNC_DEF) {
                        target_slot = (int)ctx->document_cmds[ep].args[0];
                    }
                    if (target_slot < 0)
                        target_slot = repl_func_alias_first_free_slot();
                    if (target_slot < 0) {
                        if (err && err_size > 0) {
                            snprintf(err, (size_t)err_size,
                                     "no free function slots (max %d)",
                                     REPL_FUNC_SLOT_COUNT);
                        }
                        return REPL_COMPILE_ERROR;
                    }
                    out->alias_op.slot = target_slot;
                    snprintf(out->alias_op.name, sizeof(out->alias_op.name),
                             "%s", ident);
                }
            }
        }
    }
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_func_def_kernel(const char *input,
                                               const ReplCompileContext *ctx,
                                               int allow_overwrite_at_pos,
                                               ReplFuncDefKernel *out,
                                               char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    memset(out, 0, sizeof(*out));

    /* Quick-reject inputs that look like function calls (have `(` and
     * no `{`). They go through the normal command parser. */
    const char *trimmed = input ? input : "";
    while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
    if (strchr(trimmed, '(') != NULL && strchr(trimmed, '{') == NULL) {
        out->valid = 0;
        return REPL_COMPILE_OK;
    }

    /* Resolve new user names (e.g. `drawCube { ... }`) into a pending
     * alias op. The parser accepts that pending name for this call only;
     * the alias table is updated later by apply, after the command-store
     * mutation succeeds. */
    ReplCompiledChange alias_state;
    repl_compiled_change_init(&alias_state);
    int rejected_keyword = 0;
    ReplCompileResult alias_res = repl_compile_func_def_resolve_alias(
        ctx, trimmed, &alias_state, &rejected_keyword, err, err_size);
    if (alias_res != REPL_COMPILE_OK)
        return alias_res;
    if (rejected_keyword) {
        out->rejected_keyword = 1;
        return REPL_COMPILE_OK;
    }
    out->alias_op = alias_state.alias_op;

    if (!parse_repl_func_signature_with_pending_alias(
            input ? input : "",
            out->alias_op.name, out->alias_op.slot,
            &out->fn, out->param_names, MAX_EXPR_VARS,
            &out->param_count)) {
        out->alias_op.slot = -1;
        out->alias_op.name[0] = '\0';
        out->valid = 0;
        return REPL_COMPILE_OK;
    }

    /* Reject duplicate funcN definitions. The loader passes
     * allow_overwrite_at_pos = -1 to reject any duplicate; the editor
     * passes its edit_pos when it has detected an in-place rewrite so
     * the dup-at-cursor case is exempt. */
    for (int ei = 0; ei < ctx->document_count; ei++) {
        const GLCmd *c = &ctx->document_cmds[ei];
        if (!c->valid) continue;
        if (c->type != CMD_FUNC_DEF) continue;
        if ((int)c->args[0] != out->fn) continue;
        if (ei == allow_overwrite_at_pos) continue;
        snprintf(err, (size_t)err_size,
                 "func%d already defined (line %d)", out->fn, ei + 1);
        out->alias_op.slot = -1;
        out->alias_op.name[0] = '\0';
        return REPL_COMPILE_ERROR;
    }

    out->pos = compile_insert_pos(ctx);

    repl_source_scope_cmd_indent(out->pos, out->indent, sizeof(out->indent));

    out->fd.type     = CMD_FUNC_DEF;
    out->fd.args[0]  = (float)out->fn;
    out->fd.num_args = out->param_count;
    out->fd.valid    = 1;

    format_func_header_with_alias(out->fd_text, (int)sizeof(out->fd_text),
                                  out->indent, out->fn, out->param_names,
                                  out->param_count,
                                  out->alias_op.slot >= 0 ? out->alias_op.name : NULL);

    out->valid = 1;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_func_def(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    ReplFuncDefKernel kernel;
    /* Loader path: reject every duplicate. */
    ReplCompileResult r = repl_compile_func_def_kernel(input, ctx, -1, &kernel,
                                                       err, err_size);
    if (r != REPL_COMPILE_OK) return r;
    if (kernel.rejected_keyword || !kernel.valid) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = kernel.pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = kernel.fd;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", kernel.fd_text);
    out->alias_op = kernel.alias_op;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_for_loop_kernel(const char *input,
                                               const ReplCompileContext *ctx,
                                               ReplForLoopKernel *out,
                                               char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    memset(out, 0, sizeof(*out));

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "for(", 4) != 0 && strncmp(p, "for (", 5) != 0) {
        out->valid = 0;
        return REPL_COMPILE_OK;
    }

    out->pos = compile_insert_pos(ctx);

    out->visible_nv = collect_visible_vars(out->pos, out->visible_vars,
                                           MAX_EXPR_VARS, NULL);

    if (!repl_eval_parse_for_header_with_vars(p, out->var_name,
                                              sizeof(out->var_name),
                                              &out->start, &out->end, &out->step,
                                              out->visible_vars, out->visible_nv,
                                              &out->body_start)) {
        snprintf(err, (size_t)err_size,
                 "for syntax: for(var, start, end[, step]) body;");
        return REPL_COMPILE_ERROR;
    }

    repl_source_scope_cmd_indent(out->pos, out->indent, sizeof(out->indent));

    out->fb.type    = CMD_FOR_BEGIN;
    out->fb.args[0] = out->start;
    out->fb.args[1] = out->end;
    out->fb.args[2] = out->step;
    out->fb.valid   = 1;

    /* Re-walk the input to extract the args expression text so the
     * formatted line preserves the user's symbolic form when args
     * reference visible vars. */
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

    char verr[REPL_DIAG_TEXT_MAX];
    if (!repl_eval_validate_expression_idents(ra, out->visible_vars,
                                              out->visible_nv,
                                              verr, sizeof(verr))) {
        snprintf(err, (size_t)err_size, "%s", verr);
        return REPL_COMPILE_ERROR;
    }

    if (input_has_any_visible_vars(ra, out->visible_vars, out->visible_nv)) {
        out->fb.has_vars = 1;
        if (!repl_format_fits(out->fb_text, sizeof(out->fb_text),
                              "%sfor(%s, %s) {",
                              out->indent, out->var_name, ra)) {
            snprintf(err, (size_t)err_size, "Command too long");
            return REPL_COMPILE_ERROR;
        }
    } else if (out->step != 1.0f) {
        char start_buf[32];
        char end_buf[32];
        char step_buf[32];
        repl_format_source_float(start_buf, sizeof(start_buf), out->start);
        repl_format_source_float(end_buf,   sizeof(end_buf),   out->end);
        repl_format_source_float(step_buf,  sizeof(step_buf),  out->step);
        if (!repl_format_fits(out->fb_text, sizeof(out->fb_text),
                              "%sfor(%s, %s, %s, %s) {",
                              out->indent, out->var_name,
                              start_buf, end_buf, step_buf)) {
            snprintf(err, (size_t)err_size, "Command too long");
            return REPL_COMPILE_ERROR;
        }
    } else {
        char start_buf[32];
        char end_buf[32];
        repl_format_source_float(start_buf, sizeof(start_buf), out->start);
        repl_format_source_float(end_buf,   sizeof(end_buf),   out->end);
        if (!repl_format_fits(out->fb_text, sizeof(out->fb_text),
                              "%sfor(%s, %s, %s) {",
                              out->indent, out->var_name, start_buf, end_buf)) {
            snprintf(err, (size_t)err_size, "Command too long");
            return REPL_COMPILE_ERROR;
        }
    }

    out->valid = 1;
    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_for_loop(const char *input,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    if (!ctx || !out) return REPL_COMPILE_ERROR;
    repl_compiled_change_init(out);

    ReplForLoopKernel kernel;
    ReplCompileResult r = repl_compile_for_loop_kernel(input, ctx, &kernel,
                                                       err, err_size);
    if (r != REPL_COMPILE_OK) return r;
    if (!kernel.valid) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }

    out->kind  = REPL_COMPILED_INSERT_ONE;
    out->pos   = kernel.pos;
    out->count = 1;
    out->adjust_edit_line = 1;
    out->cmds[0] = kernel.fb;
    snprintf(out->text[0], sizeof(out->text[0]), "%s", kernel.fb_text);
    return REPL_COMPILE_OK;
}

/* repl_load_apply_line moved to src/repl/load.c after a [P2] review
 * finding: this file is the pure-validator module per the contract
 * at the top, and apply orchestration (writing the command store /
 * editor buffer / predef-var registrations) belongs elsewhere. */
