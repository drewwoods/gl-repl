/*
 * repl_compile.c -- Pure source-text validators that produce
 *                   ReplCompiledChange descriptors.
 *
 * Phase C contract: every entry point in this file is pure. It
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
 * shift, editor-buffer write) are performed by repl_apply.c (Phase
 * C commit 20) and orchestrated by editor_commit (Phase C commit
 * 21).
 */

#include "repl_compile.h"

#include "repl_core_internal.h"  /* repl_format_fits, repl_extract_assignment_parts, collect_visible_vars */
#include "repl_parser.h"         /* repl_parser_parse_command_ctx (uncomment fallback) */
#include "repl_source_scope.h"   /* repl_source_scope_cmd_indent, _find_block_end */
#include "repl_state_owners.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Forward decl. Body lives in editor_services.c. The dispatch
 * function is grammar-side; its current location is a historical
 * artifact and should migrate to repl_compile.c in a follow-up. */
ReplCompileResult repl_compile_dispatch(const char *text,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size);

void repl_compiled_change_init(ReplCompiledChange *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->kind = REPL_COMPILED_NO_CHANGE;
    out->pos = 0;
    out->count = 0;
    out->delete_pos = -1;
    out->delete_count = 0;
}

ReplCompileContext repl_compile_context_from_live(void) {
    ReplCompileContext ctx = {
        .edit_line       = repl_state_edit_line(),
        .document_count  = repl_state_document_count(),
        .insert_mode     = editor_insert_mode(),
        .text            = editor_buffer_view(),
        .document_cmds   = repl_state_document_cmds(),
    };
    return ctx;
}

/* Compile-time analog of repl_commit.c's static fill_scope_indent. */
static void compile_scope_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_cmd_indent(pos, buf, buf_sz);
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

#include <stdarg.h>

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
        if (cmd->num_args != var_idx || cmd->has_vars)
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
        if (cmd->num_args == var_idx)
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
    char indent[32];

    if (!ctx || !out || !name || cmd_idx < 0 || cmd_idx >= ctx->document_count)
        return 0;

    compile_copy_leading_ws(editor_buffer_view_line(ctx->text, cmd_idx),
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

static int compile_rewrite_decl_initializer_text(const char *orig_text,
                                                 const char *name,
                                                 float value,
                                                 char *out,
                                                 int out_sz) {
    char indent[32];
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
    if (!compile_append_text(out, out_sz, &off, "%sfloat ", indent))
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
            char decl_name[16];
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

ReplCompileResult repl_compile_float_decl(const char *input,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);

    const char *p = input ? input : "";
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "float", 5) != 0) {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }
    if (isalnum((unsigned char)p[5]) || p[5] == '_') {
        out->kind = REPL_COMPILED_NO_CHANGE;
        return REPL_COMPILE_OK;
    }
    p += 5;

    char names[MAX_NAMES_PER_DECL][16];
    float init_vals[MAX_NAMES_PER_DECL];
    int has_init[MAX_NAMES_PER_DECL];
    char decl_comment[MAX_LINE_LEN] = "";
    int var_count = 0;
    memset(has_init, 0, sizeof(has_init));

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ';' || *p == '\0') break;
        if (var_count > 0) {
            if (*p != ',') {
                out->kind = REPL_COMPILED_NO_CHANGE;
                return REPL_COMPILE_OK;
            }
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
        }
        if (!isalpha((unsigned char)*p) && *p != '_') {
            return compile_set_err(err, err_size,
                "syntax error in float declaration: expected identifier");
        }
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int len = (int)(p - start);
        if (len <= 0 || len >= 16)
            return compile_set_err(err, err_size, "invalid identifier (max 15 chars)");
        if (var_count >= MAX_NAMES_PER_DECL)
            return compile_set_err(err, err_size,
                "too many names per declaration (max %d); split across lines",
                MAX_NAMES_PER_DECL);
        memcpy(names[var_count], start, (size_t)len);
        names[var_count][len] = '\0';

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '=' && p[1] != '=') {
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0' || *p == ';' || *p == ',')
                return compile_set_err(err, err_size, "expected expression after '='");
            char init_expr[MAX_LINE_LEN];
            const char *expr_start = p;
            int depth = 0;
            while (*p && *p != ';') {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                else if (*p == ',' && depth == 0) break;
                p++;
            }
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
            init_vals[var_count] = repl_eval_expr(&eval_ctx);
            has_init[var_count] = 1;
        }

        var_count++;
    }
    if (var_count == 0)
        return compile_set_err(err, err_size,
            "float declaration requires at least one identifier");
    if (*p == ';') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0' && !(p[0] == '/' && p[1] == '/'))
        return compile_set_err(err, err_size,
            "syntax error: unexpected trailing text after declaration");
    if (p[0] == '/' && p[1] == '/')
        snprintf(decl_comment, sizeof(decl_comment), " %s", p);

    /* Detect overwrite-in-place. */
    int insert_idx = ctx->insert_mode ? ctx->edit_line :
                     (ctx->edit_line < ctx->document_count
                          ? ctx->edit_line : ctx->document_count);
    int overwriting_decl = (!ctx->insert_mode &&
                            insert_idx < ctx->document_count &&
                            ctx->document_cmds[insert_idx].type == CMD_VAR_DECLARE);
    const GLCmd *old_decl = overwriting_decl ? &ctx->document_cmds[insert_idx] : NULL;

    /* Validate names. */
    for (int var_idx = 0; var_idx < var_count; var_idx++) {
        for (int prev_var_idx = 0; prev_var_idx < var_idx; prev_var_idx++) {
            if (strcmp(names[var_idx], names[prev_var_idx]) == 0)
                return compile_set_err(err, err_size,
                    "duplicate name '%s' in declaration", names[var_idx]);
        }
        if (repl_eval_find_predef_var_idx(names[var_idx]) >= 0) {
            int in_old_decl = 0;
            if (old_decl) {
                for (int decl_idx = 0; decl_idx < old_decl->var_decl_count; decl_idx++) {
                    if (strcmp(old_decl->var_names[decl_idx], names[var_idx]) == 0) {
                        in_old_decl = 1;
                        break;
                    }
                }
            }
            if (!in_old_decl)
                return compile_set_err(err, err_size,
                    "'%s' is already declared", names[var_idx]);
        }
        if (repl_eval_is_reserved_ident(names[var_idx]))
            return compile_set_err(err, err_size, "'%s' is reserved", names[var_idx]);
        if (!(isalpha((unsigned char)names[var_idx][0]) || names[var_idx][0] == '_'))
            return compile_set_err(err, err_size,
                "invalid identifier '%s'", names[var_idx]);
    }

    int old_count = old_decl ? old_decl->var_decl_count : 0;
    if (g_num_predef_vars + var_count - old_count > MAX_PREDEF_VARS)
        return compile_set_err(err, err_size,
            "variable table full (max %d)", MAX_PREDEF_VARS);

    /* Overwrite-feasibility check: removed names must not be in use. */
    if (overwriting_decl) {
        for (int decl_idx = 0; decl_idx < old_decl->var_decl_count; decl_idx++) {
            const char *nm = old_decl->var_names[decl_idx];
            int kept = 0;
            for (int var_idx = 0; var_idx < var_count; var_idx++) {
                if (strcmp(names[var_idx], nm) == 0) { kept = 1; break; }
            }
            if (kept) continue;
            for (int cmd_idx = 0; cmd_idx < ctx->document_count; cmd_idx++) {
                if (cmd_idx == insert_idx) continue;
                const char *line = editor_buffer_view_line(ctx->text, cmd_idx);
                if (repl_eval_source_uses_ident(line ? line : "", nm))
                    return compile_set_err(err, err_size,
                        "variable '%s' is in use, cannot overwrite", nm);
            }
        }
    }

    /* Build the GLCmd. */
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_VAR_DECLARE;
    cmd.valid = 1;
    cmd.var_decl_count = var_count;
    for (int var_idx = 0; var_idx < var_count; var_idx++) {
        if (!repl_copy_string_fits(cmd.var_names[var_idx],
                                   sizeof(cmd.var_names[var_idx]),
                                   names[var_idx]))
            return compile_set_err(err, err_size, "invalid identifier (max 15 chars)");
    }

    /* Decl placement: at the top of the non-decl region. */
    int decl_pos = 0;
    while (decl_pos < ctx->document_count &&
           ctx->document_cmds[decl_pos].type == CMD_VAR_DECLARE)
        decl_pos++;

    /* Format text. Decls always live at depth 0 → 2-space indent. */
    char indent[3] = "  ";
    char decl_text[MAX_LINE_LEN];
    int off = snprintf(decl_text, sizeof(decl_text), "%sfloat ", indent);
    for (int var_idx = 0; var_idx < var_count && off < (int)sizeof(decl_text) - 4; var_idx++) {
        if (var_idx > 0) off += snprintf(decl_text + off, sizeof(decl_text) - off, ", ");
        off += snprintf(decl_text + off, sizeof(decl_text) - off, "%s", names[var_idx]);
        if (has_init[var_idx])
            off += snprintf(decl_text + off, sizeof(decl_text) - off,
                            " = %g", init_vals[var_idx]);
    }
    snprintf(decl_text + off, sizeof(decl_text) - off, ";%s", decl_comment);

    /* Populate the change. */
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
    repl_copy_string_fits(out->text[0], sizeof(out->text[0]), decl_text);

    /* Build predef-op plan:
     *   - For each old name not present in the new decl: UNDECLARE.
     *   - For each new name not already registered: DECLARE (with init
     *     value if has_init).
     *   - For each kept name with an init: SET_VALUE (preserve slot,
     *     update value).
     */
    int op_count = 0;
    if (overwriting_decl) {
        for (int decl_idx = 0; decl_idx < old_decl->var_decl_count; decl_idx++) {
            const char *nm = old_decl->var_names[decl_idx];
            int kept = 0;
            for (int var_idx = 0; var_idx < var_count; var_idx++) {
                if (strcmp(names[var_idx], nm) == 0) { kept = 1; break; }
            }
            if (kept) continue;
            if (op_count >= MAX_PREDEF_OPS_PER_COMMIT) break;
            out->predef_ops[op_count].kind = REPL_PREDEF_OP_UNDECLARE;
            repl_copy_string_fits(out->predef_ops[op_count].name,
                                  sizeof(out->predef_ops[op_count].name), nm);
            op_count++;
        }
    }
    for (int var_idx = 0; var_idx < var_count; var_idx++) {
        if (op_count >= MAX_PREDEF_OPS_PER_COMMIT) break;
        int already_registered = (overwriting_decl &&
                                  repl_eval_find_predef_var_idx(names[var_idx]) >= 0);
        ReplPredefOp *op = &out->predef_ops[op_count++];
        if (already_registered) {
            if (has_init[var_idx]) {
                op->kind = REPL_PREDEF_OP_SET_VALUE;
                op->value = init_vals[var_idx];
                op->has_value = 1;
                repl_copy_string_fits(op->name, sizeof(op->name), names[var_idx]);
            } else {
                /* Kept without init: nothing to do. */
                op->kind = REPL_PREDEF_OP_NOOP;
            }
        } else {
            op->kind = REPL_PREDEF_OP_DECLARE;
            repl_copy_string_fits(op->name, sizeof(op->name), names[var_idx]);
            if (has_init[var_idx]) {
                op->value = init_vals[var_idx];
                op->has_value = 1;
            }
        }
    }
    out->predef_op_count = op_count;

    /* Success message. */
    int msg_off = snprintf(out->commit_message, sizeof(out->commit_message),
                           "declared ");
    for (int var_idx = 0; var_idx < var_count && msg_off < (int)sizeof(out->commit_message) - 4; var_idx++) {
        if (var_idx > 0)
            msg_off += snprintf(out->commit_message + msg_off,
                                sizeof(out->commit_message) - msg_off, ", ");
        msg_off += snprintf(out->commit_message + msg_off,
                            sizeof(out->commit_message) - msg_off, "%s", names[var_idx]);
    }

    return REPL_COMPILE_OK;
}

ReplCompileResult repl_compile_var_assign(const char *input,
                                          const ReplCompileContext *ctx,
                                          ReplCompiledChange *out,
                                          char *err, int err_size) {
    if (!ctx || !out)
        return REPL_COMPILE_ERROR;

    repl_compiled_change_init(out);

    char name[16];
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

    int insert_idx = ctx->insert_mode ? ctx->edit_line :
                     (ctx->edit_line < ctx->document_count
                          ? ctx->edit_line : ctx->document_count);

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
        cmd.num_args = var_idx;
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
    char indent[32];
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
    out->cmds[0] = cmd;
    repl_copy_string_fits(out->text[0], sizeof(out->text[0]), assign_text);

    /* Overwrite-feasibility check + UNDECLARE op plan. Mirrors the
     * float-decl overwrite check at line 215+; the in-use predicate
     * skips the line being replaced. */
    int op_count = 0;
    if (overwriting_decl) {
        for (int decl_idx = 0; decl_idx < old_decl->var_decl_count; decl_idx++) {
            const char *nm = old_decl->var_names[decl_idx];
            for (int cmd_idx = 0; cmd_idx < ctx->document_count; cmd_idx++) {
                if (cmd_idx == insert_idx) continue;
                const char *line = editor_buffer_view_line(ctx->text, cmd_idx);
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

    if (!index_expr[0] && out->predef_op_count > 0) {
        ReplPredefOp set_value = out->predef_ops[out->predef_op_count - 1];
        out->predef_op_count--;
        if (op_count < MAX_PREDEF_OPS_PER_COMMIT) {
            out->predef_ops[op_count++] = set_value;
        }
    }
    out->predef_op_count = op_count;

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
                editor_buffer_view_line(ctx->text, decl_idx),
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

ReplCompileResult repl_compile_delete_range(int start, int count,
                                            const ReplCompileContext *ctx,
                                            ReplCompiledChange *out,
                                            char *err, int err_size) {
    int n;
    int end;
    int op_count;

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

    /* Reference check: any CMD_VAR_DECLARE in the range whose name is
     * still referenced outside the range blocks the delete. */
    for (int i = start; i < end; i++) {
        const GLCmd *cmd = &ctx->document_cmds[i];
        if (cmd->type != CMD_VAR_DECLARE) continue;
        for (int d = 0; d < cmd->var_decl_count; d++) {
            const char *nm = cmd->var_names[d];
            for (int j = 0; j < n; j++) {
                if (j >= start && j < end) continue;
                const char *line = editor_buffer_view_line(ctx->text, j);
                if (line && repl_eval_source_uses_ident(line, nm)) {
                    return compile_set_err(err, err_size,
                                           "Cannot remove '%s': still referenced",
                                           nm);
                }
            }
        }
    }

    /* Populate UNDECLARE predef ops for every variable declared in
     * the range. Apply cascades the CMD_VAR_ASSIGN.num_args
     * compaction. */
    op_count = 0;
    for (int i = start; i < end; i++) {
        const GLCmd *cmd = &ctx->document_cmds[i];
        if (cmd->type != CMD_VAR_DECLARE) continue;
        for (int d = 0; d < cmd->var_decl_count; d++) {
            if (op_count >= MAX_PREDEF_OPS_PER_COMMIT)
                return compile_set_err(err, err_size,
                                       "Too many declarations in range");
            out->predef_ops[op_count].kind = REPL_PREDEF_OP_UNDECLARE;
            repl_copy_string_fits(out->predef_ops[op_count].name,
                                  sizeof(out->predef_ops[op_count].name),
                                  cmd->var_names[d]);
            op_count++;
        }
    }
    out->predef_op_count = op_count;

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
        if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) {
            depth++;
        } else if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN) {
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
    is_block_head = (type == CMD_FOR_BEGIN || type == CMD_FUNC_DEF ||
                     type == CMD_IF_BEGIN);
    is_block_end  = (type == CMD_FOR_END || type == CMD_FUNC_END ||
                     type == CMD_IF_END);

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

        for (int i = 0; i < n; i++) {
            const char *orig = editor_buffer_view_line(ctx->text, head + i);
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
        const char *orig = editor_buffer_view_line(ctx->text, line_idx);
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
            out->kind == REPL_COMPILED_DELETE_RANGE ||
            out->kind == REPL_COMPILED_LOAD_ALL)
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
        const char *orig = editor_buffer_view_line(ctx->text, line_idx);
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
