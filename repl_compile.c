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

#include "repl_command_store.h"  /* MAX_COMMANDS, REPL_COMMAND_STORE_ADJUST_EDIT_LINE */
#include "repl_core_internal.h"  /* repl_format_fits, repl_extract_assignment_parts, collect_visible_vars */
#include "repl_source_scope.h"   /* repl_source_scope_cmd_indent */
#include "repl_state.h"          /* repl_state_document_*, repl_state_edit_line, etc. */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void repl_compiled_change_init(ReplCompiledChange *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->kind = REPL_COMPILED_NO_CHANGE;
    out->pos = 0;
    out->count = 0;
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
    snprintf(decl_text + off, sizeof(decl_text) - off, ";");

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
    char rhs[MAX_LINE_LEN];
    char comment[MAX_LINE_LEN];

    if (!repl_extract_assignment_parts(input ? input : "",
                                       name, sizeof(name),
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

    int var_idx = repl_eval_find_predef_var_idx(name);
    if (var_idx < 0)
        return compile_set_err(err, err_size,
            "undeclared variable '%s' - use 'float %s;' first", name, name);

    int insert_idx = ctx->insert_mode ? ctx->edit_line :
                     (ctx->edit_line < ctx->document_count
                          ? ctx->edit_line : ctx->document_count);

    ExprVar vis[MAX_EXPR_VARS];
    int vis_n = collect_visible_vars(insert_idx, vis, MAX_EXPR_VARS);
    char verr[128];
    if (!repl_eval_validate_expression_idents(rhs,
                                              vis_n > 0 ? vis : NULL, vis_n,
                                              verr, sizeof(verr)))
        return compile_set_err(err, err_size, "%s", verr);

    ExprCtx eval_ctx = { rhs, g_predef_vars, g_num_predef_vars };
    float val = repl_eval_expr(&eval_ctx);
    int has_rhs_vars = repl_eval_input_has_predef_vars(rhs);

    /* Build the GLCmd. */
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type     = CMD_VAR_ASSIGN;
    cmd.valid    = 1;
    cmd.args[0]  = val;
    cmd.num_args = var_idx;
    cmd.has_vars = has_rhs_vars;

    /* Format text using the scope indent at the insert position. */
    char indent[32];
    compile_scope_indent(insert_idx, indent, sizeof(indent));
    char assign_text[MAX_LINE_LEN];
    if (!repl_format_fits(assign_text, sizeof(assign_text),
                          "%s%s = %s;%s", indent, name, rhs, comment))
        return compile_set_err(err, err_size, "Command too long");

    /* Decide insert / replace.
     *
     * The legacy try_assign_variable() has a special branch for
     * "overwrite a CMD_VAR_DECLARE in non-insert mode": it walks the
     * doc looking for ident usage of the dropped names and bails if
     * any are found. That validation is identical to the float-decl
     * overwrite check; we let the legacy apply path handle it for now
     * (Phase C commit 20 absorbs it into the apply layer). For
     * compile, we just classify the kind and let apply take care of
     * the predef-op cascade.
     */
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
    } else {
        out->kind  = REPL_COMPILED_INSERT_ONE;
        out->pos   = ctx->document_count;
        out->count = 1;
        out->adjust_edit_line = 0;
    }
    out->cmds[0] = cmd;
    repl_copy_string_fits(out->text[0], sizeof(out->text[0]), assign_text);

    /* Predef-op plan: write the live value. */
    out->predef_ops[0].kind = REPL_PREDEF_OP_SET_VALUE;
    repl_copy_string_fits(out->predef_ops[0].name,
                          sizeof(out->predef_ops[0].name), name);
    out->predef_ops[0].value = val;
    out->predef_ops[0].has_value = 1;
    out->predef_op_count = 1;

    snprintf(out->commit_message, sizeof(out->commit_message),
             "%s = %g", name, val);

    return REPL_COMPILE_OK;
}
