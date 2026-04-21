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

static int g_func_decl_resume_delta = 0;

static int function_decl_insert_pos(void) {
    int pos = 0;

    while (pos < g_num_cmds && g_cmds[pos].type == CMD_VAR_DECLARE)
        pos++;

    while (pos < g_num_cmds) {
        if (g_cmds[pos].type == CMD_COMMENT) {
            pos++;
            continue;
        }
        if (g_cmds[pos].type != CMD_FUNC_DEF)
            break;

        int end = find_block_end(pos);
        if (end >= g_num_cmds)
            return g_num_cmds;
        pos = end + 1;
    }

    return pos;
}

static int function_leading_comment_start(int pos) {
    int start = pos;

    while (start > 0 &&
           g_cmds[start - 1].valid &&
           g_cmds[start - 1].type == CMD_COMMENT &&
           block_depth_at(start - 1) == 0)
        start--;

    return start;
}

static void apply_func_decl_resume(CmdType end_type) {
    if (end_type != CMD_FUNC_END || g_func_decl_resume_delta <= 0)
        return;

    g_edit_line += g_func_decl_resume_delta;
    if (g_edit_line > g_num_cmds)
        g_edit_line = g_num_cmds;
    g_func_decl_resume_delta = 0;
}

int repl_commit_resolve_insert_exit_target(int target) {
    if (!g_inserting ||
        g_func_decl_resume_delta <= 0 ||
        g_edit_line < 0 ||
        g_edit_line >= g_num_cmds ||
        g_cmds[g_edit_line].type != CMD_FUNC_END)
        return target;

    if (target == g_edit_line) {
        target += g_func_decl_resume_delta;
        if (target > g_num_cmds)
            target = g_num_cmds;
    }

    g_func_decl_resume_delta = 0;
    return target;
}


void repl_commit_reset_transients(void) {
    g_func_decl_resume_delta = 0;
}

int try_commit_float_decl(void) {
    const char *p = g_input;
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
                /* Not a comma — might be '=' handled below, or junk.
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
             * already-declared predef vars — no loop/function locals.
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
            if (!validate_expression_idents(init_expr, NULL, 0,
                                            verr, sizeof(verr))) {
                set_status(verr);
                return 1;
            }
            ExprCtx ctx = { init_expr, g_predef_vars, g_num_predef_vars };
            init_vals[var_count] = eval_expr(&ctx);
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
     * editing — even unchanged — reports "'tmp' already declared". */
    int insert_idx = g_inserting ? g_edit_line :
               (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
    int overwriting_decl = (!g_inserting && insert_idx < g_num_cmds &&
                            g_cmds[insert_idx].type == CMD_VAR_DECLARE);
    const GLCmd *old_decl = overwriting_decl ? &g_cmds[insert_idx] : NULL;

    /* Validate all names atomically before registering any */
    for (int i = 0; i < var_count; i++) {
        /* Reject duplicates within the same declaration (e.g. float a, a;) */
        for (int j = 0; j < i; j++) {
            if (strcmp(names[i], names[j]) == 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "duplicate name '%s' in declaration", names[i]);
                set_status(buf);
                return 1;
            }
        }
        /* Reject re-declaring an already-declared variable — but exempt
         * names carried over from the decl we're overwriting, since those
         * will be undeclared before the new registration runs. */
        if (find_predef_var_idx(names[i]) >= 0) {
            int in_old_decl = 0;
            if (old_decl) {
                for (int d = 0; d < old_decl->var_decl_count; d++) {
                    if (strcmp(old_decl->var_names[d], names[i]) == 0) {
                        in_old_decl = 1;
                        break;
                    }
                }
            }
            if (!in_old_decl) {
                char buf[128];
                if (!repl_format_fits(buf, sizeof(buf), "'%s' is already declared", names[i]))
                    repl_format_fits(buf, sizeof(buf), "identifier is already declared");
                set_status(buf);
                return 1;
            }
        }
        if (is_reserved_ident(names[i])) {
            char buf[128];
            if (!repl_format_fits(buf, sizeof(buf), "'%s' is reserved", names[i]))
                repl_format_fits(buf, sizeof(buf), "identifier is reserved");
            set_status(buf);
            return 1;
        }
        if (!(isalpha((unsigned char)names[i][0]) || names[i][0] == '_')) {
            char buf[128];
            if (!repl_format_fits(buf, sizeof(buf), "invalid identifier '%s'", names[i]))
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
    for (int i = 0; i < var_count; i++) {
        if (!repl_copy_string_fits(cmd.var_names[i], sizeof(cmd.var_names[i]),
                                   names[i])) {
            set_status("invalid identifier (max 15 chars)");
            return 1;
        }
    }

    {
        int decl_pos = 0;
        while (decl_pos < g_num_cmds &&
               g_cmds[decl_pos].type == CMD_VAR_DECLARE)
            decl_pos++;

        int ind = 2;  /* decls always at block depth 0 (top-level) */
        char indent[32];
        memset(indent, ' ', (size_t)ind);
        indent[ind] = '\0';

        int off = snprintf(cmd.source, sizeof(cmd.source), "%sfloat ", indent);
        for (int i = 0; i < var_count && off < (int)sizeof(cmd.source) - 4; i++) {
            if (i > 0) off += snprintf(cmd.source + off, sizeof(cmd.source) - off, ", ");
            off += snprintf(cmd.source + off, sizeof(cmd.source) - off, "%s", names[i]);
            if (has_init[i])
                off += snprintf(cmd.source + off, sizeof(cmd.source) - off,
                                " = %g", init_vals[i]);
        }
        snprintf(cmd.source + off, sizeof(cmd.source) - off, ";");

        /* Check overwrite feasibility BEFORE registering new names. Only
         * names being REMOVED (present in old decl, absent from new) need
         * the "in use" check — names being kept stay valid throughout. */
        if (overwriting_decl) {
            for (int d = 0; d < g_cmds[insert_idx].var_decl_count; d++) {
                const char *nm = g_cmds[insert_idx].var_names[d];
                int kept = 0;
                for (int k = 0; k < var_count; k++) {
                    if (strcmp(names[k], nm) == 0) { kept = 1; break; }
                }
                if (kept) continue;
                for (int j = 0; j < g_num_cmds; j++) {
                    if (j == insert_idx) continue;
                    if (source_uses_ident(g_cmds[j].source, nm)) {
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
            for (int d = 0; d < g_cmds[insert_idx].var_decl_count; d++) {
                const char *nm = g_cmds[insert_idx].var_names[d];
                int kept = 0;
                for (int k = 0; k < var_count; k++) {
                    if (strcmp(names[k], nm) == 0) { kept = 1; break; }
                }
                if (kept) continue;
                int slot = find_predef_var_idx(nm);
                if (slot < 0) continue;
                undeclare_predef_var(nm);
                for (int j = 0; j < g_num_cmds; j++) {
                    if (g_cmds[j].type == CMD_VAR_ASSIGN && g_cmds[j].num_args > slot)
                        g_cmds[j].num_args--;
                }
            }
        }

        /* Register new names (safe — overwrite check passed, capacity verified).
         * Skip names already registered (kept from old decl) to preserve values. */
        for (int i = 0; i < var_count; i++) {
            if (overwriting_decl && find_predef_var_idx(names[i]) >= 0) {
                if (has_init[i]) {
                    int idx = find_predef_var_idx(names[i]);
                    g_predef_vars[idx].value = init_vals[i];
                }
                continue;
            }
            declare_predef_var(names[i], NULL, 0);
            if (has_init[i]) {
                int idx = find_predef_var_idx(names[i]);
                if (idx >= 0)
                    g_predef_vars[idx].value = init_vals[i];
            }
        }

        ReplCommandStore store = repl_command_store_live();
        if (overwriting_decl) {
            repl_command_store_replace_one(&store, insert_idx, &cmd);
            g_edit_line++;
            load_line_to_input(g_edit_line);
        } else if (repl_command_store_insert_one(
                       &store, decl_pos, &cmd,
                       REPL_COMMAND_STORE_ADJUST_EDIT_LINE)) {
            if (!g_inserting && g_edit_line < g_num_cmds)
                load_line_to_input(g_edit_line);
        } else {
            set_status("Command buffer full!");
            return 1;
        }
    }

    {
        char msg[128];
        int off = snprintf(msg, sizeof(msg), "declared ");
        for (int i = 0; i < var_count && off < (int)sizeof(msg) - 4; i++) {
            if (i > 0) off += snprintf(msg + off, sizeof(msg) - off, ", ");
            off += snprintf(msg + off, sizeof(msg) - off, "%s", names[i]);
        }
        set_status(msg);
    }
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
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
    int ind;

    if (!repl_extract_assignment_parts(g_input, name, sizeof(name), rhs, sizeof(rhs)))
        return 0;

    comment[0] = '\0';
    {
        const char *comment_p = strstr(g_input, "//");
        if (comment_p) {
            while (*comment_p && isspace((unsigned char)*comment_p))
                comment_p++;
            if (comment_p[0] == '/' && comment_p[1] == '/') {
                snprintf(comment, sizeof(comment), " %s", comment_p);
            }
        }
    }

    int var_idx = find_predef_var_idx(name);
    if (var_idx < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "undeclared variable '%s' — use 'float %s;' first", name, name);
        set_status(buf);
        return 1;
    }

    {
        int insert_idx = g_inserting ? g_edit_line :
                   (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
        ExprVar vis[MAX_EXPR_VARS];
        int vis_n = collect_visible_vars(insert_idx, vis, MAX_EXPR_VARS);
        char verr[128];
        if (!validate_expression_idents(rhs, vis_n > 0 ? vis : NULL, vis_n, verr, sizeof(verr))) {
            set_status(verr);
            return 1;
        }
    }
    {
        ExprCtx ctx = { rhs, g_predef_vars, g_num_predef_vars };
        val = eval_expr(&ctx);
    }
    g_predef_vars[var_idx].value = val;
    has_rhs_vars = input_has_predef_vars(rhs);

    {
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CMD_VAR_ASSIGN;
        cmd.valid = 1;
        cmd.args[0] = val;
        cmd.num_args = var_idx;
        cmd.has_vars = has_rhs_vars;

        {
            int insert_idx = g_inserting ? g_edit_line :
                       (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
            ind = (in_begin_block_at(insert_idx) ? 4 : 2) + block_depth_at(insert_idx) * 2;
            if (ind > (int)sizeof(indent) - 1)
                ind = (int)sizeof(indent) - 1;
            memset(indent, ' ', (size_t)ind);
            indent[ind] = '\0';
            if (!repl_format_fits(cmd.source, sizeof(cmd.source),
                                  "%s%s = %s;%s",
                                  indent, name, rhs, comment)) {
                set_status("Command too long");
                return 1;
            }

            ReplCommandStore store = repl_command_store_live();
            if (g_inserting) {
                if (repl_command_store_insert_one(&store, insert_idx, &cmd, 0))
                    g_edit_line++;
                else {
                    set_status("Command buffer full!");
                    return 1;
                }
            } else if (insert_idx < g_num_cmds) {
                if (g_cmds[insert_idx].type == CMD_VAR_DECLARE) {
                    for (int d = 0; d < g_cmds[insert_idx].var_decl_count; d++) {
                        const char *nm = g_cmds[insert_idx].var_names[d];
                        for (int j = 0; j < g_num_cmds; j++) {
                            if (j == insert_idx) continue;
                            if (source_uses_ident(g_cmds[j].source, nm)) {
                                char buf[128];
                                snprintf(buf, sizeof(buf),
                                         "variable '%s' is in use, cannot overwrite", nm);
                                set_status(buf);
                                return 1;
                            }
                        }
                    }
                    for (int d = 0; d < g_cmds[insert_idx].var_decl_count; d++) {
                        const char *nm = g_cmds[insert_idx].var_names[d];
                        int slot = find_predef_var_idx(nm);
                        if (slot < 0) continue;
                        undeclare_predef_var(nm);
                        for (int j = 0; j < g_num_cmds; j++) {
                            if (g_cmds[j].type == CMD_VAR_ASSIGN && g_cmds[j].num_args > slot)
                                g_cmds[j].num_args--;
                        }
                    }
                }
                repl_command_store_replace_one(&store, insert_idx, &cmd);
                g_edit_line++;
                load_line_to_input(g_edit_line);
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%s = %g", name, val);
                    set_status(msg);
                }
                mark_normals_dirty();
                return 1;
            } else {
                if (!repl_command_store_insert_one(&store, g_num_cmds, &cmd, 0)) {
                    set_status("Command buffer full!");
                    return 1;
                }
                g_edit_line = g_num_cmds;
            }
        }
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s = %g", name, val);
        set_status(msg);
    }
    g_input[0] = '\0';
    g_input_len = 0;
    g_cursor_pos = 0;
    mark_normals_dirty();
    return 1;
}

int try_commit_for_loop(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "for(", 4) != 0 && strncmp(p, "for (", 5) != 0)
        return 0;

    {
        int pos = g_inserting ? g_edit_line :
                  (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
        ExprVar visible_vars[MAX_EXPR_VARS];
        int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS);
        char var_name[16];
        float start, end, step;
        const char *body_start;

        if (!parse_for_header_with_vars(p, var_name, sizeof(var_name),
                                        &start, &end, &step,
                                        visible_vars, visible_nv, &body_start)) {
            set_status("for syntax: for(var, start, end[, step]) body;");
            return 1;
        }

        while (*body_start && isspace((unsigned char)*body_start))
            body_start++;

        {
            int fdepth = block_depth_at(pos);
            int bb = in_begin_block_at(pos);
            int ind = (bb ? 4 : 2) + fdepth * 2;
            char indent[32];
            GLCmd fb;
            GLCmd fe;

            if (ind > (int)sizeof(indent) - 1)
                ind = (int)sizeof(indent) - 1;
            memset(indent, ' ', (size_t)ind);
            indent[ind] = '\0';

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
                            if (!validate_expression_idents(ra, visible_vars, visible_nv, verr, sizeof(verr))) {
                                set_status(verr);
                                return 1;
                            }
                        }

                        if (input_has_any_visible_vars(ra, visible_vars, visible_nv)) {
                            fb.has_vars = 1;
                            if (!repl_format_fits(fb.source, sizeof(fb.source),
                                                  "%sfor(%s, %s) {",
                                                  indent, var_name, ra)) {
                                set_status("Command too long");
                                return 1;
                            }
                        } else if (step != 1.0f) {
                            if (!repl_format_fits(fb.source, sizeof(fb.source),
                                                  "%sfor(%s, %g, %g, %g) {",
                                                  indent, var_name, start, end, step)) {
                                set_status("Command too long");
                                return 1;
                            }
                        } else {
                            if (!repl_format_fits(fb.source, sizeof(fb.source),
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
            snprintf(fe.source, sizeof(fe.source), "%s}", indent);

            if (*body_start == '{' || *body_start == '\0') {
                if (!g_inserting && g_edit_line < g_num_cmds &&
                    g_cmds[g_edit_line].type == CMD_FOR_BEGIN) {
                    ReplCommandStore store = repl_command_store_live();
                    repl_command_store_replace_one(&store, g_edit_line, &fb);
                    g_edit_line++;
                    g_inserting = 1;
                    g_input[0] = '\0';
                    g_input_len = 0;
                    g_cursor_pos = 0;
                    clear_autocomplete_state();
                    set_status("for-loop header updated");
                    mark_normals_dirty();
                    return 1;
                }

                ReplCommandStore store = repl_command_store_live();
                GLCmd loop_cmds[2] = { fb, fe };
                if (!repl_command_store_insert_many(&store, pos,
                                                    loop_cmds, 2, 0)) {
                    set_status("Command buffer full!");
                    return 1;
                }

                g_edit_line = pos + 1;
                g_inserting = 1;
                g_input[0] = '\0';
                g_input_len = 0;
                g_cursor_pos = 0;
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
                int saved;

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
                for (int i = 0; i < visible_nv && dvn < MAX_EXPR_VARS; i++)
                    dv[dvn++] = visible_vars[i];

                memset(&body_cmd, 0, sizeof(body_cmd));
                saved = g_edit_line;
                g_edit_line = pos;
                if (!repl_parse_command_with_vars(body, &body_cmd, dv, dvn)) {
                    g_edit_line = saved;
                    set_status("Invalid for-loop body command");
                    return 1;
                }
                g_edit_line = saved;

                {
                    char bind[32];
                    int bi = ind + 2;
                    if (bi > (int)sizeof(bind) - 1)
                        bi = (int)sizeof(bind) - 1;
                    memset(bind, ' ', (size_t)bi);
                    bind[bi] = '\0';
                    snprintf(body_cmd.source, sizeof(body_cmd.source), "%s%s;", bind, body);
                }

                ReplCommandStore store = repl_command_store_live();
                GLCmd loop_cmds[3] = { fb, body_cmd, fe };
                if (!repl_command_store_insert_many(&store, pos,
                                                    loop_cmds, 3, 0)) {
                    set_status("Command buffer full!");
                    return 1;
                }

                g_edit_line = pos + 3;
                g_inserting = 0;
                g_input[0] = '\0';
                g_input_len = 0;
                g_cursor_pos = 0;
                g_newline_buf[0] = '\0';
                g_newline_len = 0;

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
    const char *trimmed = g_input;

    while (*trimmed && isspace((unsigned char)*trimmed))
        trimmed++;
    if (strchr(trimmed, '(') && strchr(trimmed, '{') == NULL)
        return 0;
    if (!parse_repl_func_signature(g_input, &fn,
                                   param_names, MAX_EXPR_VARS,
                                   &param_count))
        return 0;

    {
        int edit_pos = g_inserting ? g_edit_line :
                       (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
        int overwriting_func = (!g_inserting && edit_pos < g_num_cmds &&
                                g_cmds[edit_pos].type == CMD_FUNC_DEF);
        int pos = overwriting_func ? edit_pos : function_decl_insert_pos();
        int bdepth = overwriting_func ? block_depth_at(pos) : 0;
        int bb = overwriting_func ? in_begin_block_at(pos) : 0;
        int ind = (bb ? 4 : 2) + bdepth * 2;
        char indent[32];
        GLCmd fd;
        GLCmd fe;
        ReplCommandStore store = repl_command_store_live();

        if (ind > (int)sizeof(indent) - 1)
            ind = (int)sizeof(indent) - 1;
        memset(indent, ' ', (size_t)ind);
        indent[ind] = '\0';

        if (overwriting_func) {
            GLCmd updated = g_cmds[edit_pos];
            updated.args[0] = (float)fn;
            updated.num_args = param_count;
            format_func_header(updated.source,
                               (int)sizeof(updated.source),
                               indent, fn, param_names, param_count);
            repl_command_store_replace_one(&store, edit_pos, &updated);
            g_edit_line = edit_pos + 1;
            g_inserting = 1;
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
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
        format_func_header(fd.source, (int)sizeof(fd.source),
                           indent, fn, param_names, param_count);

        memset(&fe, 0, sizeof(fe));
        fe.type = CMD_FUNC_END;
        fe.valid = 1;
        snprintf(fe.source, sizeof(fe.source), "%s}", indent);

        int comment_start = edit_pos;
        int comment_count = 0;
        int resume_pos = edit_pos;

        if (!overwriting_func) {
            comment_start = function_leading_comment_start(edit_pos);
            comment_count = edit_pos - comment_start;
        }

        int insert_count = comment_count + 2;
        GLCmd *insert_cmds = (GLCmd *)malloc((size_t)insert_count * sizeof(*insert_cmds));
        if (!insert_cmds) {
            set_status("Out of memory");
            return 1;
        }
        if (comment_count > 0) {
            memcpy(insert_cmds, &g_cmds[comment_start],
                   (size_t)comment_count * sizeof(*insert_cmds));
            repl_command_store_delete_range(&store, comment_start, comment_count);
            resume_pos = edit_pos - comment_count;
        }

        pos = function_decl_insert_pos();
        insert_cmds[comment_count] = fd;
        insert_cmds[comment_count + 1] = fe;
        if (!repl_command_store_insert_many(&store, pos, insert_cmds,
                                            insert_count, 0)) {
            free(insert_cmds);
            set_status("Command buffer full!");
            return 1;
        }
        g_func_decl_resume_delta = resume_pos > pos ? resume_pos - pos : 0;
        free(insert_cmds);

        g_edit_line = pos + comment_count + 1;
        g_inserting = 1;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        set_status("func def: type body lines, press Esc when done");
        mark_normals_dirty();
        return 1;
    }
}

int try_commit_if_block(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (strncmp(p, "if(", 3) != 0 && strncmp(p, "if (", 4) != 0)
        return 0;

    {
        int pos = g_inserting ? g_edit_line :
                  (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
        ExprVar visible_vars[MAX_EXPR_VARS];
        int visible_nv = collect_visible_vars(pos, visible_vars, MAX_EXPR_VARS);
        float cond_args[1];
        float cond_val;
        char cond_text[MAX_LINE_LEN];
        int clen;
        int bdepth;
        int bb;
        int ind;
        char indent[32];
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
            if (!validate_expression_idents(cond_text,
                                            visible_nv > 0 ? visible_vars : NULL, visible_nv,
                                            verr, sizeof(verr))) {
                set_status(verr);
                return 1;
            }
        }
        {
            int neval = parse_exprs(cond_text, cond_args, 1,
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

        bdepth = block_depth_at(pos);
        bb = in_begin_block_at(pos);
        ind = (bb ? 4 : 2) + bdepth * 2;
        if (ind > (int)sizeof(indent) - 1)
            ind = (int)sizeof(indent) - 1;
        memset(indent, ' ', (size_t)ind);
        indent[ind] = '\0';

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
            snprintf(ib.source, sizeof(ib.source), "%sif(%s) {", indent, ct);
        }

        if (!g_inserting && g_edit_line < g_num_cmds &&
            g_cmds[g_edit_line].type == CMD_IF_BEGIN) {
            ReplCommandStore store = repl_command_store_live();
            repl_command_store_replace_one(&store, g_edit_line, &ib);
            g_edit_line++;
            g_inserting = 1;
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
            clear_autocomplete_state();
            set_status("if condition updated");
            mark_normals_dirty();
            return 1;
        }

        memset(&ie, 0, sizeof(ie));
        ie.type = CMD_IF_END;
        ie.valid = 1;
        snprintf(ie.source, sizeof(ie.source), "%s}", indent);

        ReplCommandStore store = repl_command_store_live();
        GLCmd if_cmds[2] = { ib, ie };
        if (!repl_command_store_insert_many(&store, pos, if_cmds, 2, 0)) {
            set_status("Command buffer full!");
            return 1;
        }

        g_edit_line = pos + 1;
        g_inserting = 1;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        set_status("if-block: type body lines, press Esc when done");
        mark_normals_dirty();
        return 1;
    }
}

int try_commit_close_brace(void) {
    const char *p = g_input;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '}')
        return 0;

    {
        int pos = g_inserting ? g_edit_line :
                  (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
        CmdType open_type = nearest_open_block_at(pos);
        CmdType end_type;
        const char *label;
        int bdepth;
        int bb_val;
        int ind_len;
        char indent[32];
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
        if (pos < g_num_cmds && g_cmds[pos].type == end_type) {
            int keep_inserting = (g_func_decl_resume_delta > 0 &&
                                  end_type != CMD_FUNC_END);
            g_edit_line = pos + 1;
            apply_func_decl_resume(end_type);
            g_inserting = keep_inserting ? 1 : 0;
            g_input[0] = '\0';
            g_input_len = 0;
            g_cursor_pos = 0;
            load_line_to_input(g_edit_line);
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "%s block closed", label);
                set_status(msg);
            }
            mark_normals_dirty();
            return 1;
        }

        bdepth = block_depth_at(pos) - 1;
        if (bdepth < 0)
            bdepth = 0;
        bb_val = in_begin_block_at(pos);
        ind_len = (bb_val ? 4 : 2) + bdepth * 2;
        if (ind_len > (int)sizeof(indent) - 1)
            ind_len = (int)sizeof(indent) - 1;
        memset(indent, ' ', (size_t)ind_len);
        indent[ind_len] = '\0';

        memset(&fe, 0, sizeof(fe));
        fe.type = end_type;
        fe.valid = 1;
        snprintf(fe.source, sizeof(fe.source), "%s}", indent);

        ReplCommandStore store = repl_command_store_live();
        if (!repl_command_store_insert_one(&store, pos, &fe, 0)) {
            set_status("Command buffer full!");
            return 1;
        }
        int keep_inserting = (g_func_decl_resume_delta > 0 &&
                              end_type != CMD_FUNC_END);
        g_edit_line = pos + 1;
        apply_func_decl_resume(end_type);
        g_inserting = keep_inserting ? 1 : 0;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        g_newline_buf[0] = '\0';
        g_newline_len = 0;
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
        g_inserting = 1;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        clear_autocomplete_state();
        return 1;
    }
    if (try_assign_variable()) {
        g_inserting = 1;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        clear_autocomplete_state();
        set_status("Insert mode");
        mark_normals_dirty();
        return 1;
    }
    return 0;
}
