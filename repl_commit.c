/*
 * repl_commit.c - REPL commit handlers and commit-order helpers.
 *
 * This module owns the syntactic forms that mutate source commands before
 * the general GL parser gets a chance to run: float declarations, variable
 * assignments, structured blocks, and explicit close braces. repl_editor.c
 * remains responsible for deciding when those handlers are invoked.
 */
#include "sample.h"
#include "repl_core_internal.h"
#include "repl_command_store.h"
#include "repl_parser.h"
#include "repl_source_scope.h"
#include "repl_state.h"

static int g_func_decl_resume_delta = 0;

static int function_decl_insert_pos(void) {
    int pos = 0;

    while (pos < repl_state_document_count() && repl_state_document_cmds_mut()[pos].type == CMD_VAR_DECLARE)
        pos++;

    while (pos < repl_state_document_count()) {
        if (repl_state_document_cmds_mut()[pos].type == CMD_COMMENT) {
            pos++;
            continue;
        }
        if (repl_state_document_cmds_mut()[pos].type != CMD_FUNC_DEF)
            break;

        int end = repl_source_scope_find_block_end(pos);
        if (end >= repl_state_document_count())
            return repl_state_document_count();
        pos = end + 1;
    }

    return pos;
}

static int function_leading_comment_start(int pos) {
    int start = pos;

    while (start > 0 &&
           repl_state_document_cmds_mut()[start - 1].valid &&
           repl_state_document_cmds_mut()[start - 1].type == CMD_COMMENT &&
           repl_source_scope_block_depth_at(start - 1) == 0)
        start--;

    return start;
}

static void apply_func_decl_resume(CmdType end_type) {
    if (end_type != CMD_FUNC_END || g_func_decl_resume_delta <= 0)
        return;

    repl_state_edit_line_set(repl_state_edit_line() + (g_func_decl_resume_delta));
    if (repl_state_edit_line() > repl_state_document_count())
        repl_state_edit_line_set(repl_state_document_count());
    g_func_decl_resume_delta = 0;
}

static void fill_scope_indent(int pos, char *buf, int buf_sz) {
    repl_source_scope_cmd_indent(pos, buf, buf_sz);
}

static void fill_scope_close_indent(int pos, char *buf, int buf_sz) {
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


int repl_commit_resolve_insert_exit_target(int target) {
    if (!editor_insert_mode() ||
        g_func_decl_resume_delta <= 0 ||
        repl_state_edit_line() < 0 ||
        repl_state_edit_line() >= repl_state_document_count() ||
        repl_state_document_cmds_mut()[repl_state_edit_line()].type != CMD_FUNC_END)
        return target;

    if (target == repl_state_edit_line()) {
        target += g_func_decl_resume_delta;
        if (target > repl_state_document_count())
            target = repl_state_document_count();
    }

    g_func_decl_resume_delta = 0;
    return target;
}


void repl_commit_reset_transients(void) {
    g_func_decl_resume_delta = 0;
}

int try_commit_float_decl(void) {
    const char *p = editor_state_input().input;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "float", 5) != 0) return 0;
    if (isalnum((unsigned char)p[5]) || p[5] == '_') return 0;
    p += 5;

    char names[MAX_NAMES_PER_DECL][16];
    float init_vals[MAX_NAMES_PER_DECL];
    int has_init[MAX_NAMES_PER_DECL];
    int var_count = 0;
    memset(has_init, 0, sizeof(has_init));
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        /* Accept ';' or end-of-string as terminator (';' key doesn't
         * append to g_input, so interactive commits lack the ';'). */
        if (*p == ';' || *p == '\0') break;
        if (var_count > 0) {
            if (*p != ',') {
                /* Not a comma - might be '=' handled below, or junk.
                 * If we already consumed at least one name and the
                 * previous iteration didn't end with '=', this is
                 * not a valid float declaration. */
                return 0;
            }
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
        }
        if (!isalpha((unsigned char)*p) && *p != '_') {
            set_status("syntax error in float declaration: expected identifier");
            return 1;
        }
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        int len = (int)(p - start);
        if (len <= 0 || len >= 16) {
            set_status("invalid identifier (max 15 chars)");
            return 1;
        }
        if (var_count >= MAX_NAMES_PER_DECL) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "too many names per declaration (max %d); split across lines",
                     MAX_NAMES_PER_DECL);
            set_status(buf);
            return 1;
        }
        memcpy(names[var_count], start, len);
        names[var_count][len] = '\0';

        /* Check for optional initializer: float name = expr */
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '=' && p[1] != '=') {
            p++;  /* skip '=' */
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0' || *p == ';' || *p == ',') {
                set_status("expected expression after '='");
                return 1;
            }
            /* Declarations are placed at the top of non-decl code; visible
             * scope vars at that position are always empty (decls live at
             * block depth 0), so the init expression may only reference
             * already-declared predef vars - no loop/function locals.
             * Extract the initializer expression up to ',' or ';' or end. */
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
            memcpy(init_expr, expr_start, elen);
            init_expr[elen] = '\0';
            /* Trim trailing whitespace */
            while (elen > 0 && isspace((unsigned char)init_expr[elen - 1]))
                init_expr[--elen] = '\0';
            if (elen == 0) {
                set_status("expected expression after '='");
                return 1;
            }
            char verr[128];
            if (!repl_eval_validate_expression_idents(init_expr, NULL, 0,
                                            verr, sizeof(verr))) {
                set_status(verr);
                return 1;
            }
            ExprCtx ctx = { init_expr, g_predef_vars, g_num_predef_vars };
            init_vals[var_count] = repl_eval_expr(&ctx);
            has_init[var_count] = 1;
        }

        var_count++;
    }
    if (var_count == 0) {
        set_status("float declaration requires at least one identifier");
        return 1;
    }
    /* Accept ';' or end-of-string as valid terminator */
    if (*p == ';') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0' && !(p[0] == '/' && p[1] == '/')) {
        set_status("syntax error: unexpected trailing text after declaration");
        return 1;
    }

    /* Detect overwrite-in-place (editing an existing CMD_VAR_DECLARE) early
     * so validation can exempt the line's own names from the "already
     * declared" check. Without this, re-committing `float tmp;` after
     * editing - even unchanged - reports "'tmp' already declared". */
    int insert_idx = editor_insert_mode() ? repl_state_edit_line() :
               (repl_state_edit_line() < repl_state_document_count() ? repl_state_edit_line() : repl_state_document_count());
    int overwriting_decl = (!editor_insert_mode() && insert_idx < repl_state_document_count() &&
                            repl_state_document_cmds_mut()[insert_idx].type == CMD_VAR_DECLARE);
    const GLCmd *old_decl = overwriting_decl ? &repl_state_document_cmds_mut()[insert_idx] : NULL;

    /* Validate all names atomically before registering any */
    for (int var_idx = 0; var_idx < var_count; var_idx++) {
        /* Reject duplicates within the same declaration (e.g. float a, a;) */
        for (int prev_var_idx = 0; prev_var_idx < var_idx; prev_var_idx++) {
            if (strcmp(names[var_idx], names[prev_var_idx]) == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "duplicate name '%s' in declaration", names[var_idx]);
                set_status(buf);
                return 1;
            }
        }
        /* Reject re-declaring an already-declared variable - but exempt
         * names carried over from the decl we're overwriting, since those
         * will be undeclared before the new registration runs. */
        if (repl_eval_find_predef_var_idx(names[var_idx]) >= 0) {
            int in_old_decl = 0;
            if (old_decl) {
                /* Check if this name appears in the old declaration. */
                for (int decl_idx = 0; decl_idx < old_decl->var_decl_count; decl_idx++) {
                    if (strcmp(old_decl->var_names[decl_idx], names[var_idx]) == 0) {
                        in_old_decl = 1;
                        break;
                    }
                }
            }
            if (!in_old_decl) {
                char buf[128];
                if (!repl_format_fits(buf, sizeof(buf), "'%s' is already declared", names[var_idx]))
                    repl_format_fits(buf, sizeof(buf), "identifier is already declared");
                set_status(buf);
                return 1;
            }
        }
        if (repl_eval_is_reserved_ident(names[var_idx])) {
            char buf[128];
            if (!repl_format_fits(buf, sizeof(buf), "'%s' is reserved", names[var_idx]))
                repl_format_fits(buf, sizeof(buf), "identifier is reserved");
            set_status(buf);
            return 1;
        }
        if (!(isalpha((unsigned char)names[var_idx][0]) || names[var_idx][0] == '_')) {
            char buf[128];
            if (!repl_format_fits(buf, sizeof(buf), "invalid identifier '%s'", names[var_idx]))
                repl_format_fits(buf, sizeof(buf), "invalid identifier");
            set_status(buf);
            return 1;
        }
    }

    /* Capacity check: in overwrite mode the old decl's slots will be freed
     * before the new ones are registered, so the net delta is
     * new_count - old_count. */
    int old_count = old_decl ? old_decl->var_decl_count : 0;
    if (g_num_predef_vars + var_count - old_count > MAX_PREDEF_VARS) {
        char buf[128];
        snprintf(buf, sizeof(buf), "variable table full (max %d)", MAX_PREDEF_VARS);
        set_status(buf);
        return 1;
    }

    /* Build the GLCmd */
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_VAR_DECLARE;
    cmd.valid = 1;
    cmd.var_decl_count = var_count;
    /* Copy all variable names into the command structure. */
    for (int var_idx = 0; var_idx < var_count; var_idx++) {
        if (!repl_copy_string_fits(cmd.var_names[var_idx], sizeof(cmd.var_names[var_idx]),
                                   names[var_idx])) {
            set_status("invalid identifier (max 15 chars)");
            return 1;
        }
    }

    {
        int decl_pos = 0;
        while (decl_pos < repl_state_document_count() &&
               repl_state_document_cmds_mut()[decl_pos].type == CMD_VAR_DECLARE)
            decl_pos++;

        int ind = 2;  /* decls always at block depth 0 (top-level) */
        char indent[32];
        memset(indent, ' ', (size_t)ind);
        indent[ind] = '\0';

        char decl_text[MAX_LINE_LEN];
        int off = snprintf(decl_text, sizeof(decl_text), "%sfloat ", indent);
        /* Format each variable name and optional initializer. */
        for (int var_idx = 0; var_idx < var_count && off < (int)sizeof(decl_text) - 4; var_idx++) {
            if (var_idx > 0) off += snprintf(decl_text + off, sizeof(decl_text) - off, ", ");
            off += snprintf(decl_text + off, sizeof(decl_text) - off, "%s", names[var_idx]);
            if (has_init[var_idx])
                off += snprintf(decl_text + off, sizeof(decl_text) - off,
                                " = %g", init_vals[var_idx]);
        }
        snprintf(decl_text + off, sizeof(decl_text) - off, ";");

        /* Check overwrite feasibility BEFORE registering new names. Only
         * names being REMOVED (present in old decl, absent from new) need
         * the "in use" check - names being kept stay valid throughout. */
        if (overwriting_decl) {
            EditorBufferView text = editor_buffer_view();
            /* Check each old declaration name. */
            for (int decl_idx = 0; decl_idx < repl_state_document_cmds_mut()[insert_idx].var_decl_count; decl_idx++) {
                const char *nm = repl_state_document_cmds_mut()[insert_idx].var_names[decl_idx];
                int kept = 0;
                for (int var_idx = 0; var_idx < var_count; var_idx++) {
                    if (strcmp(names[var_idx], nm) == 0) { kept = 1; break; }
                }
                if (kept) continue;
                /* Check if this removed name is still used anywhere. */
                for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
                    if (cmd_idx == insert_idx) continue;
                    const char *line = editor_buffer_view_line(text, cmd_idx);
                    if (repl_eval_source_uses_ident(line ? line : "", nm)) {
                        char buf[128];
                        snprintf(buf, sizeof(buf),
                                 "variable '%s' is in use, cannot overwrite", nm);
                        set_status(buf);
                        return 1;
                    }
                }
            }
        }

        /* Undeclare only names being removed (absent from new decl) so kept
         * names retain their slot indices and live values. */
        if (overwriting_decl) {
            /* Undeclare each old name that is not being kept. */
            for (int decl_idx = 0; decl_idx < repl_state_document_cmds_mut()[insert_idx].var_decl_count; decl_idx++) {
                const char *nm = repl_state_document_cmds_mut()[insert_idx].var_names[decl_idx];
                int kept = 0;
                for (int var_idx = 0; var_idx < var_count; var_idx++) {
                    if (strcmp(names[var_idx], nm) == 0) { kept = 1; break; }
                }
                if (kept) continue;
                int slot = repl_eval_find_predef_var_idx(nm);
                if (slot < 0) continue;
                repl_eval_undeclare_predef_var(nm);
                /* Adjust num_args in variable assignments that reference this slot. */
                for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
                    if (repl_state_document_cmds_mut()[cmd_idx].type == CMD_VAR_ASSIGN && repl_state_document_cmds_mut()[cmd_idx].num_args > slot)
                        repl_state_document_cmds_mut()[cmd_idx].num_args--;
                }
            }
        }

        /* Register new names (safe - overwrite check passed, capacity verified).
         * Skip names already registered (kept from old decl) to preserve values. */
        for (int var_idx = 0; var_idx < var_count; var_idx++) {
            if (overwriting_decl && repl_eval_find_predef_var_idx(names[var_idx]) >= 0) {
                if (has_init[var_idx]) {
                    int idx = repl_eval_find_predef_var_idx(names[var_idx]);
                    g_predef_vars[idx].value = init_vals[var_idx];
                }
                continue;
            }
            repl_eval_declare_predef_var(names[var_idx], NULL, 0);
            if (has_init[var_idx]) {
                int idx = repl_eval_find_predef_var_idx(names[var_idx]);
                if (idx >= 0)
                    g_predef_vars[idx].value = init_vals[var_idx];
            }
        }

        ReplCommandStore store = repl_command_store_live();
        if (overwriting_decl) {
            repl_command_store_replace_one(&store, insert_idx, &cmd,
                                           decl_text);
            repl_state_edit_line_set(repl_state_edit_line() + 1);
            load_line_to_input(repl_state_edit_line());
        } else if (repl_command_store_insert_one(
                       &store, decl_pos, &cmd,
                       REPL_COMMAND_STORE_ADJUST_EDIT_LINE,
                       decl_text)) {
            if (!editor_insert_mode() && repl_state_edit_line() < repl_state_document_count())
                load_line_to_input(repl_state_edit_line());
        } else {
            set_status("Command buffer full!");
            return 1;
        }
    }

    {
        char msg[128];
        int off = snprintf(msg, sizeof(msg), "declared ");
        /* Append comma-separated variable names to status message. */
        for (int var_idx = 0; var_idx < var_count && off < (int)sizeof(msg) - 4; var_idx++) {
            if (var_idx > 0) off += snprintf(msg + off, sizeof(msg) - off, ", ");
            off += snprintf(msg + off, sizeof(msg) - off, "%s", names[var_idx]);
        }
        set_status(msg);
    }
    {
        ReplEditorInputState *inp = editor_state_input_mut();
        inp->input[0] = '\0';
        inp->input_len = 0;
    }
    editor_cursor_pos_set(0);
    mark_normals_dirty();
    return 1;
}

int try_assign_variable(void) {
    char name[16];
    char rhs[MAX_LINE_LEN];
    char comment[MAX_LINE_LEN];
    int has_rhs_vars;
    float val;
    char indent[32];

    if (!repl_extract_assignment_parts(editor_state_input().input, name, sizeof(name), rhs, sizeof(rhs)))
        return 0;

    comment[0] = '\0';
    {
        const char *comment_p = strstr(editor_state_input().input, "//");
        if (comment_p) {
            while (*comment_p && isspace((unsigned char)*comment_p))
                comment_p++;
            if (comment_p[0] == '/' && comment_p[1] == '/') {
                snprintf(comment, sizeof(comment), " %s", comment_p);
            }
        }
    }

    int var_idx = repl_eval_find_predef_var_idx(name);
    if (var_idx < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "undeclared variable '%s' - use 'float %s;' first", name, name);
        set_status(buf);
        return 1;
    }

    {
        int insert_idx = editor_insert_mode() ? repl_state_edit_line() :
                   (repl_state_edit_line() < repl_state_document_count() ? repl_state_edit_line() : repl_state_document_count());
        ExprVar vis[MAX_EXPR_VARS];
        int vis_n = collect_visible_vars(insert_idx, vis, MAX_EXPR_VARS);
        char verr[128];
        if (!repl_eval_validate_expression_idents(rhs, vis_n > 0 ? vis : NULL, vis_n, verr, sizeof(verr))) {
            set_status(verr);
            return 1;
        }
    }
    {
        ExprCtx ctx = { rhs, g_predef_vars, g_num_predef_vars };
        val = repl_eval_expr(&ctx);
    }
    g_predef_vars[var_idx].value = val;
    has_rhs_vars = repl_eval_input_has_predef_vars(rhs);

    {
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CMD_VAR_ASSIGN;
        cmd.valid = 1;
        cmd.args[0] = val;
        cmd.num_args = var_idx;
        cmd.has_vars = has_rhs_vars;

        {
            int insert_idx = editor_insert_mode() ? repl_state_edit_line() :
                       (repl_state_edit_line() < repl_state_document_count() ? repl_state_edit_line() : repl_state_document_count());
            fill_scope_indent(insert_idx, indent, sizeof(indent));
            char assign_text[MAX_LINE_LEN];
            if (!repl_format_fits(assign_text, sizeof(assign_text),
                                  "%s%s = %s;%s",
                                  indent, name, rhs, comment)) {
                set_status("Command too long");
                return 1;
            }

            ReplCommandStore store = repl_command_store_live();
            if (editor_insert_mode()) {
                if (repl_command_store_insert_one(&store, insert_idx, &cmd, 0,
                                                  assign_text))
                    repl_state_edit_line_set(repl_state_edit_line() + 1);
                else {
                    set_status("Command buffer full!");
                    return 1;
                }
            } else if (insert_idx < repl_state_document_count()) {
                if (repl_state_document_cmds_mut()[insert_idx].type == CMD_VAR_DECLARE) {
                    EditorBufferView text = editor_buffer_view();
                    /* Check if any variables are still in use. */
                    for (int decl_idx = 0; decl_idx < repl_state_document_cmds_mut()[insert_idx].var_decl_count; decl_idx++) {
                        const char *nm = repl_state_document_cmds_mut()[insert_idx].var_names[decl_idx];
                        for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
                            if (cmd_idx == insert_idx) continue;
                            const char *line = editor_buffer_view_line(text, cmd_idx);
                            if (repl_eval_source_uses_ident(line ? line : "", nm)) {
                                char buf[128];
                                snprintf(buf, sizeof(buf),
                                         "variable '%s' is in use, cannot overwrite", nm);
                                set_status(buf);
                                return 1;
                            }
                        }
                    }
                    /* Undeclare removed variables and adjust assignments. */
                    for (int decl_idx = 0; decl_idx < repl_state_document_cmds_mut()[insert_idx].var_decl_count; decl_idx++) {
                        const char *nm = repl_state_document_cmds_mut()[insert_idx].var_names[decl_idx];
                        int slot = repl_eval_find_predef_var_idx(nm);
                        if (slot < 0) continue;
                        repl_eval_undeclare_predef_var(nm);
                        /* Adjust num_args in variable assignments that reference this slot. */
                        for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
                            if (repl_state_document_cmds_mut()[cmd_idx].type == CMD_VAR_ASSIGN && repl_state_document_cmds_mut()[cmd_idx].num_args > slot)
                                repl_state_document_cmds_mut()[cmd_idx].num_args--;
                        }
                    }
                }
                repl_command_store_replace_one(&store, insert_idx, &cmd,
                                               assign_text);
                repl_state_edit_line_set(repl_state_edit_line() + 1);
                load_line_to_input(repl_state_edit_line());
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%s = %g", name, val);
                    set_status(msg);
                }
                mark_normals_dirty();
                return 1;
            } else {
                if (!repl_command_store_insert_one(&store,
                                                   repl_state_document_count(),
                                                   &cmd, 0, assign_text)) {
                    set_status("Command buffer full!");
                    return 1;
                }
                repl_state_edit_line_set(repl_state_document_count());
            }
        }
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s = %g", name, val);
        set_status(msg);
    }
    {
        ReplEditorInputState *inp = editor_state_input_mut();
        inp->input[0] = '\0';
        inp->input_len = 0;
    }
    editor_cursor_pos_set(0);
    mark_normals_dirty();
    return 1;
}

int try_commit_for_loop(void) {
    const char *p = editor_state_input().input;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "for(", 4) != 0 && strncmp(p, "for (", 5) != 0)
        return 0;

    {
        int pos = editor_insert_mode() ? repl_state_edit_line() :
                  (repl_state_edit_line() < repl_state_document_count() ? repl_state_edit_line() : repl_state_document_count());
        ExprVar visible_vars[MAX_EXPR_VARS];
        int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS);
        char var_name[16];
        float start, end, step;
        const char *body_start;

        if (!repl_eval_parse_for_header_with_vars(p, var_name, sizeof(var_name),
                                        &start, &end, &step,
                                        visible_vars, visible_nv, &body_start)) {
            set_status("for syntax: for(var, start, end[, step]) body;");
            return 1;
        }

        while (*body_start && isspace((unsigned char)*body_start))
            body_start++;

        {
            int ind;
            char indent[32];
            char fb_text[MAX_LINE_LEN] = "";
            char fe_text[MAX_LINE_LEN] = "";
            GLCmd fb;
            GLCmd fe;

            fill_scope_indent(pos, indent, sizeof(indent));
            ind = (int)strlen(indent);

            memset(&fb, 0, sizeof(fb));
            fb.type = CMD_FOR_BEGIN;
            fb.args[0] = start;
            fb.args[1] = end;
            fb.args[2] = step;
            fb.valid = 1;

            {
                const char *raw = p;
                while (*raw && *raw != '(')
                    raw++;
                if (*raw)
                    raw++;
                while (*raw && isspace((unsigned char)*raw))
                    raw++;
                while (*raw && (isalnum((unsigned char)*raw) || *raw == '_'))
                    raw++;
                while (*raw && isspace((unsigned char)*raw))
                    raw++;
                if (*raw == ',')
                    raw++;

                {
                    const char *args_start = raw;
                    int paren = 1;
                    const char *ap = args_start;
                    char raw_args[MAX_LINE_LEN];
                    int rlen;

                    while (*ap && paren > 0) {
                        if (*ap == '(')
                            paren++;
                        else if (*ap == ')')
                            paren--;
                        if (paren > 0)
                            ap++;
                    }

                    rlen = (int)(ap - args_start);
                    if (rlen > (int)sizeof(raw_args) - 1)
                        rlen = (int)sizeof(raw_args) - 1;
                    memcpy(raw_args, args_start, (size_t)rlen);
                    raw_args[rlen] = '\0';
                    while (rlen > 0 && isspace((unsigned char)raw_args[rlen - 1]))
                        raw_args[--rlen] = '\0';

                    {
                        char *ra = raw_args;
                        while (*ra && isspace((unsigned char)*ra))
                            ra++;

                        {
                            char verr[128];
                            if (!repl_eval_validate_expression_idents(ra, visible_vars, visible_nv, verr, sizeof(verr))) {
                                set_status(verr);
                                return 1;
                            }
                        }

                        if (input_has_any_visible_vars(ra, visible_vars, visible_nv)) {
                            fb.has_vars = 1;
                            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                                  "%sfor(%s, %s) {",
                                                  indent, var_name, ra)) {
                                set_status("Command too long");
                                return 1;
                            }
                        } else if (step != 1.0f) {
                            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                                  "%sfor(%s, %g, %g, %g) {",
                                                  indent, var_name, start, end, step)) {
                                set_status("Command too long");
                                return 1;
                            }
                        } else {
                            if (!repl_format_fits(fb_text, sizeof(fb_text),
                                                  "%sfor(%s, %g, %g) {",
                                                  indent, var_name, start, end)) {
                                set_status("Command too long");
                                return 1;
                            }
                        }
                    }
                }
            }

            memset(&fe, 0, sizeof(fe));
            fe.type = CMD_FOR_END;
            fe.valid = 1;
            snprintf(fe_text, sizeof(fe_text), "%s}", indent);

            if (*body_start == '{' || *body_start == '\0') {
                const char *loop_lines[2] = { fb_text, fe_text };

                if (!editor_insert_mode() && repl_state_edit_line() < repl_state_document_count() &&
                    repl_state_document_cmds_mut()[repl_state_edit_line()].type == CMD_FOR_BEGIN) {
                    ReplCommandStore store = repl_command_store_live();
                    repl_command_store_replace_one(&store, repl_state_edit_line(),
                                                   &fb, fb_text);
                    repl_state_edit_line_set(repl_state_edit_line() + 1);
                    editor_insert_mode_set(1);
                    {
                        ReplEditorInputState *inp = editor_state_input_mut();
                        inp->input[0] = '\0';
                        inp->input_len = 0;
                    }
                    editor_cursor_pos_set(0);
                    clear_autocomplete_state();
                    set_status("for-loop header updated");
                    mark_normals_dirty();
                    return 1;
                }

                ReplCommandStore store = repl_command_store_live();
                GLCmd loop_cmds[2] = { fb, fe };
                if (!repl_command_store_insert_many(&store, pos,
                                                    loop_cmds, 2, 0,
                                                    loop_lines)) {
                    set_status("Command buffer full!");
                    return 1;
                }

                repl_state_edit_line_set(pos + 1);
                editor_insert_mode_set(1);
                {
                    ReplEditorInputState *inp = editor_state_input_mut();
                    inp->input[0] = '\0';
                    inp->input_len = 0;
                }
                editor_cursor_pos_set(0);
                set_status("for-loop: type body lines, press Esc when done");
                mark_normals_dirty();
                return 1;
            }

            {
                char body[MAX_LINE_LEN];
                int blen;
                ExprVar dv[MAX_EXPR_VARS];
                int dvn = 0;
                GLCmd body_cmd;

                strncpy(body, body_start, MAX_LINE_LEN - 1);
                body[MAX_LINE_LEN - 1] = '\0';
                blen = (int)strlen(body);
                while (blen > 0 &&
                       (body[blen - 1] == ';' || isspace((unsigned char)body[blen - 1])))
                    body[--blen] = '\0';
                if (blen == 0) {
                    set_status("for-loop needs a body");
                    return 1;
                }

                repl_copy_string_fits(dv[dvn].name, sizeof(dv[dvn].name),
                                      var_name);
                dv[dvn].value = start;
                dvn++;
                /* Append visible variables to the loop context. */
                for (int var_idx = 0; var_idx < visible_nv && dvn < MAX_EXPR_VARS; var_idx++)
                    dv[dvn++] = visible_vars[var_idx];

                ReplParseContext parse_ctx = { pos, dv, dvn, 1 };
                {
                    ReplParsedLine body_pl;
                    if (!repl_parser_parse_command_ctx(body, &body_pl, &parse_ctx)) {
                        set_status("Invalid for-loop body command");
                        return 1;
                    }
                    body_cmd = body_pl.cmd;
                }

                char body_text[MAX_LINE_LEN];
                {
                    char bind[32];
                    int bi = ind + 2;
                    if (bi > (int)sizeof(bind) - 1)
                        bi = (int)sizeof(bind) - 1;
                    memset(bind, ' ', (size_t)bi);
                    bind[bi] = '\0';
                    snprintf(body_text, sizeof(body_text), "%s%s;", bind, body);
                }

                ReplCommandStore store = repl_command_store_live();
                GLCmd loop_cmds[3] = { fb, body_cmd, fe };
                const char *loop_lines[3] = { fb_text, body_text, fe_text };
                if (!repl_command_store_insert_many(&store, pos,
                                                    loop_cmds, 3, 0,
                                                    loop_lines)) {
                    set_status("Command buffer full!");
                    return 1;
                }

                repl_state_edit_line_set(pos + 3);
                editor_insert_mode_set(0);
                {
                    ReplEditorInputState *inp = editor_state_input_mut();
                    inp->input[0] = '\0';
                    inp->input_len = 0;
                }
                editor_cursor_pos_set(0);
                {
                    ReplEditorInputState *inp = editor_state_input_mut();
                    inp->pending_newline[0] = '\0';
                    inp->pending_newline_len = 0;
                }

                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "for-loop: %s from %g to %g",
                             var_name, start, end);
                    set_status(msg);
                }
                mark_normals_dirty();
                return 1;
            }
        }
    }
}

int try_commit_func_def(void) {
    int fn = -1;
    int param_count = 0;
    char param_names[MAX_EXPR_VARS][16];
    const char *trimmed = editor_state_input().input;

    while (*trimmed && isspace((unsigned char)*trimmed))
        trimmed++;
    if (strchr(trimmed, '(') && strchr(trimmed, '{') == NULL)
        return 0;
    if (!parse_repl_func_signature(editor_state_input().input, &fn,
                                   param_names, MAX_EXPR_VARS,
                                   &param_count))
        return 0;

    {
        int edit_pos = editor_insert_mode() ? repl_state_edit_line() :
                       (repl_state_edit_line() < repl_state_document_count() ? repl_state_edit_line() : repl_state_document_count());
        int overwriting_func = (!editor_insert_mode() && edit_pos < repl_state_document_count() &&
                                repl_state_document_cmds_mut()[edit_pos].type == CMD_FUNC_DEF);

        /* Reject duplicate func definitions: each func<N> may only be defined
         * once.  Overwriting the existing definition (cursor already on that
         * line) is still allowed. */
        for (int ei = 0; ei < repl_state_document_count(); ei++) {
            if (!repl_state_document_cmds_mut()[ei].valid) continue;
            if (repl_state_document_cmds_mut()[ei].type != CMD_FUNC_DEF) continue;
            if ((int)repl_state_document_cmds_mut()[ei].args[0] != fn) continue;
            if (overwriting_func && ei == edit_pos) continue;
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "func%d already defined (line %d)", fn, ei + 1);
            set_status(buf);
            return 1;
        }

        int pos = overwriting_func ? edit_pos : function_decl_insert_pos();
        char indent[32];
        GLCmd fd;
        GLCmd fe;
        ReplCommandStore store = repl_command_store_live();
        char fd_text[MAX_LINE_LEN] = "";
        char fe_text[MAX_LINE_LEN] = "";

        fill_scope_indent(pos, indent, sizeof(indent));

        if (overwriting_func) {
            GLCmd updated = repl_state_document_cmds_mut()[edit_pos];
            updated.args[0] = (float)fn;
            updated.num_args = param_count;
            format_func_header(fd_text, (int)sizeof(fd_text),
                               indent, fn, param_names, param_count);
            repl_command_store_replace_one(&store, edit_pos, &updated,
                                           fd_text);
            repl_state_edit_line_set(edit_pos + 1);
            editor_insert_mode_set(1);
            {
                ReplEditorInputState *inp = editor_state_input_mut();
                inp->input[0] = '\0';
                inp->input_len = 0;
            }
            editor_cursor_pos_set(0);
            clear_autocomplete_state();
            set_status("func def header updated");
            mark_normals_dirty();
            return 1;
        }

        memset(&fd, 0, sizeof(fd));
        fd.type = CMD_FUNC_DEF;
        fd.args[0] = (float)fn;
        fd.num_args = param_count;
        fd.valid = 1;
        format_func_header(fd_text, (int)sizeof(fd_text),
                           indent, fn, param_names, param_count);

        memset(&fe, 0, sizeof(fe));
        fe.type = CMD_FUNC_END;
        fe.valid = 1;
        snprintf(fe_text, sizeof(fe_text), "%s}", indent);

        int comment_start = edit_pos;
        int comment_count = 0;
        int resume_pos = edit_pos;
        char (*comment_lines)[MAX_LINE_LEN] = NULL;
        const char *insert_lines[MAX_COMMANDS];

        if (!overwriting_func) {
            comment_start = function_leading_comment_start(edit_pos);
            comment_count = edit_pos - comment_start;
            if (comment_count > 0) {
                EditorBufferView text = editor_buffer_view();
                comment_lines = malloc((size_t)comment_count * sizeof(*comment_lines));
                if (!comment_lines) {
                    set_status("Out of memory");
                    return 1;
                }
                for (int comment_idx = 0; comment_idx < comment_count; comment_idx++)
                    repl_copy_string_fits(comment_lines[comment_idx], MAX_LINE_LEN,
                                          editor_buffer_view_line(text,
                                                                  comment_start + comment_idx));
            }
        }

        int insert_count = comment_count + 2;
        GLCmd *insert_cmds = (GLCmd *)malloc((size_t)insert_count * sizeof(*insert_cmds));
        if (!insert_cmds) {
            free(comment_lines);
            set_status("Out of memory");
            return 1;
        }
        if (comment_count > 0) {
            memcpy(insert_cmds, &repl_state_document_cmds_mut()[comment_start],
                   (size_t)comment_count * sizeof(*insert_cmds));
            repl_command_store_delete_range(&store, comment_start, comment_count);
            resume_pos = edit_pos - comment_count;
        }

        pos = function_decl_insert_pos();
        insert_cmds[comment_count] = fd;
        insert_cmds[comment_count + 1] = fe;
        for (int comment_idx = 0; comment_idx < comment_count; comment_idx++)
            insert_lines[comment_idx] = comment_lines[comment_idx];
        insert_lines[comment_count] = fd_text;
        insert_lines[comment_count + 1] = fe_text;
        if (!repl_command_store_insert_many(&store, pos, insert_cmds,
                                            insert_count, 0,
                                            insert_lines)) {
            free(comment_lines);
            free(insert_cmds);
            set_status("Command buffer full!");
            return 1;
        }
        g_func_decl_resume_delta = resume_pos > pos ? resume_pos - pos : 0;
        free(comment_lines);
        free(insert_cmds);

        repl_state_edit_line_set(pos + comment_count + 1);
        editor_insert_mode_set(1);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        set_status("func def: type body lines, press Esc when done");
        mark_normals_dirty();
        return 1;
    }
}

int try_commit_if_block(void) {
    const char *p = editor_state_input().input;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "if(", 3) != 0 && strncmp(p, "if (", 4) != 0)
        return 0;

    {
        int pos = editor_insert_mode() ? repl_state_edit_line() :
                  (repl_state_edit_line() < repl_state_document_count() ? repl_state_edit_line() : repl_state_document_count());
        ExprVar visible_vars[MAX_EXPR_VARS];
        int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS);
        float cond_args[1];
        float cond_val;
        char cond_text[MAX_LINE_LEN];
        int clen;
        char indent[32];
        char ib_text[MAX_LINE_LEN] = "";
        char ie_text[MAX_LINE_LEN] = "";
        GLCmd ib;
        GLCmd ie;

        while (*p && *p != '(')
            p++;
        if (!*p)
            return 0;
        p++;

        {
            int paren = 1;
            const char *expr_start = p;

            while (*p && paren > 0) {
                if (*p == '(')
                    paren++;
                else if (*p == ')')
                    paren--;
                if (paren > 0)
                    p++;
            }
            if (paren != 0) {
                set_status("if syntax: if(expr) {");
                return 1;
            }

            clen = (int)(p - expr_start);
            if (clen > (int)sizeof(cond_text) - 1)
                clen = (int)sizeof(cond_text) - 1;
            memcpy(cond_text, expr_start, (size_t)clen);
            cond_text[clen] = '\0';
        }

        {
            char verr[128];
            if (!repl_eval_validate_expression_idents(cond_text,
                                            visible_nv > 0 ? visible_vars : NULL, visible_nv,
                                            verr, sizeof(verr))) {
                set_status(verr);
                return 1;
            }
        }
        {
            int neval = repl_eval_parse_exprs(cond_text, cond_args, 1,
                                    visible_nv > 0 ? visible_vars : NULL, visible_nv);
            cond_val = (neval >= 1) ? cond_args[0] : 0.0f;
        }

        p++;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p != '{' && *p != '\0') {
            set_status("if syntax: if(expr) {");
            return 1;
        }

        fill_scope_indent(pos, indent, sizeof(indent));

        memset(&ib, 0, sizeof(ib));
        ib.type = CMD_IF_BEGIN;
        ib.args[0] = cond_val;
        ib.valid = 1;
        ib.has_vars = input_has_any_visible_vars(cond_text, visible_vars, visible_nv);

        {
            char *ct = cond_text;
            int ctlen;

            while (*ct && isspace((unsigned char)*ct))
                ct++;
            ctlen = (int)strlen(ct);
            while (ctlen > 0 && isspace((unsigned char)ct[ctlen - 1]))
                ct[--ctlen] = '\0';
            snprintf(ib_text, sizeof(ib_text), "%sif(%s) {", indent, ct);
        }

        if (!editor_insert_mode() && repl_state_edit_line() < repl_state_document_count() &&
            repl_state_document_cmds_mut()[repl_state_edit_line()].type == CMD_IF_BEGIN) {
            ReplCommandStore store = repl_command_store_live();
            repl_command_store_replace_one(&store, repl_state_edit_line(), &ib,
                                           ib_text);
            repl_state_edit_line_set(repl_state_edit_line() + 1);
            editor_insert_mode_set(1);
            {
                ReplEditorInputState *inp = editor_state_input_mut();
                inp->input[0] = '\0';
                inp->input_len = 0;
            }
            editor_cursor_pos_set(0);
            clear_autocomplete_state();
            set_status("if condition updated");
            mark_normals_dirty();
            return 1;
        }

        memset(&ie, 0, sizeof(ie));
        ie.type = CMD_IF_END;
        ie.valid = 1;
        snprintf(ie_text, sizeof(ie_text), "%s}", indent);

        ReplCommandStore store = repl_command_store_live();
        GLCmd if_cmds[2] = { ib, ie };
        const char *if_lines[2] = { ib_text, ie_text };
        if (!repl_command_store_insert_many(&store, pos, if_cmds, 2, 0,
                                            if_lines)) {
            set_status("Command buffer full!");
            return 1;
        }

        repl_state_edit_line_set(pos + 1);
        editor_insert_mode_set(1);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        set_status("if-block: type body lines, press Esc when done");
        mark_normals_dirty();
        return 1;
    }
}

int try_commit_close_brace(void) {
    const char *p = editor_state_input().input;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '}')
        return 0;

    {
        int pos = editor_insert_mode() ? repl_state_edit_line() :
                  (repl_state_edit_line() < repl_state_document_count() ? repl_state_edit_line() : repl_state_document_count());
        CmdType open_type = repl_source_scope_nearest_open_block_at(pos);
        CmdType end_type;
        const char *label;
        char indent[32];
        char fe_text[MAX_LINE_LEN] = "";
        GLCmd fe;

        if (open_type == CMD_TYPE_COUNT)
            return 0;

        if (open_type == CMD_FOR_BEGIN) {
            end_type = CMD_FOR_END;
            label = "for-loop";
        } else if (open_type == CMD_FUNC_DEF) {
            end_type = CMD_FUNC_END;
            label = "func def";
        } else if (open_type == CMD_IF_BEGIN) {
            end_type = CMD_IF_END;
            label = "if-block";
        } else {
            return 0;
        }

        /* Reuse an existing synthesized end marker even if we're no longer
         * in insert mode (e.g. after closing an inner block). Otherwise a
         * function/if/for close brace can duplicate the trailing end command. */
        if (pos < repl_state_document_count() && repl_state_document_cmds_mut()[pos].type == end_type) {
            int keep_inserting = (g_func_decl_resume_delta > 0 &&
                                  end_type != CMD_FUNC_END);
            repl_state_edit_line_set(pos + 1);
            apply_func_decl_resume(end_type);
            editor_insert_mode_set(keep_inserting ? 1 : 0);
            {
                ReplEditorInputState *inp = editor_state_input_mut();
                inp->input[0] = '\0';
                inp->input_len = 0;
            }
            editor_cursor_pos_set(0);
            load_line_to_input(repl_state_edit_line());
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "%s block closed", label);
                set_status(msg);
            }
            mark_normals_dirty();
            return 1;
        }

        fill_scope_close_indent(pos, indent, sizeof(indent));

        memset(&fe, 0, sizeof(fe));
        fe.type = end_type;
        fe.valid = 1;
        snprintf(fe_text, sizeof(fe_text), "%s}", indent);

        ReplCommandStore store = repl_command_store_live();
        if (!repl_command_store_insert_one(&store, pos, &fe, 0, fe_text)) {
            set_status("Command buffer full!");
            return 1;
        }
        int keep_inserting = (g_func_decl_resume_delta > 0 &&
                              end_type != CMD_FUNC_END);
        repl_state_edit_line_set(pos + 1);
        apply_func_decl_resume(end_type);
        editor_insert_mode_set(keep_inserting ? 1 : 0);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->pending_newline[0] = '\0';
            inp->pending_newline_len = 0;
        }
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s block closed", label);
            set_status(msg);
        }
        mark_normals_dirty();
        return 1;
    }
}

/* Block-structural commit handlers: `}`, `for(`, `funcN`, `if(`.
 * These inspect the start of g_input and are mutually exclusive with
 * var-statement handlers. */
int try_commit_block_structs(void) {
    if (try_commit_close_brace()) return 1;
    if (try_commit_for_loop())    return 1;
    if (try_commit_func_def())    return 1;
    if (try_commit_if_block())    return 1;
    return 0;
}

/* Statement-level commit handlers. float decl MUST precede assign so that
 * `float x` is not misread as an assignment to an identifier named "float". */
int try_commit_var_statements(void) {
    if (try_commit_float_decl())  return 1;
    if (try_assign_variable())    return 1;
    return 0;
}

/* Canonical order: var statements first (float/assign), then block structs.
 * Handlers are mutually exclusive on input prefix, so the relative ordering
 * of the two groups doesn't affect observed behavior. */
int try_commit_any(void) {
    if (try_commit_var_statements()) return 1;
    if (try_commit_block_structs())  return 1;
    return 0;
}

/* Overwrite-mode Enter variant: on successful var-statement commit, enter
 * insert mode and clear the input. Assign additionally publishes the
 * "Insert mode" status and marks normals dirty (float decl does not). */
int try_commit_var_statements_then_insert(void) {
    if (try_commit_float_decl()) {
        editor_insert_mode_set(1);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        clear_autocomplete_state();
        return 1;
    }
    if (try_assign_variable()) {
        editor_insert_mode_set(1);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        clear_autocomplete_state();
        set_status("Insert mode");
        mark_normals_dirty();
        return 1;
    }
    return 0;
}
