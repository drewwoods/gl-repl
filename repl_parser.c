/*
 * repl_parser.c - REPL text command parser.
 *
 * Converts one source line into a GLCmd and canonical source text. The parse
 * context carries the source-line index used for scope-sensitive indentation
 * plus the loop/function locals visible at that line.
 */
#include "repl_parser.h"

#include "repl_command_spec.h"
#include "repl_command_store.h"
#include "repl_core_internal.h"
#include "repl_source_scope.h"
#include "repl_state.h"

static void set_incomplete_missing_paren_status(const char *func) {
    char msg[128];

    if (func && func[0])
        snprintf(msg, sizeof(msg), "Incomplete command: missing ')' in %s(...)", func);
    else
        snprintf(msg, sizeof(msg), "Incomplete command: missing ')'");
    set_status(msg);
}

static void set_incomplete_arg_count_status(const char *func, int expected, int got) {
    char msg[128];

    snprintf(msg, sizeof(msg),
             "Incomplete command: %s expects %d argument%s (got %d)",
             func, expected, expected == 1 ? "" : "s", got);
    set_status(msg);
}

static int command_name_matches_or_prefixes(const char *func, const char *known) {
    size_t flen;

    if (!func || !func[0] || !known || !known[0])
        return 0;
    if (strcmp(func, known) == 0)
        return 1;

    flen = strlen(func);
    return flen >= 4 && flen <= strlen(known) && strncmp(known, func, flen) == 0;
}

static int is_known_incomplete_func_name(const char *func) {
    static const char *const special_funcs[] = {
        "glEnd",
        "glPointParameterfv",
        "glPushMatrix",
        "glPopMatrix",
        "gluBegin",
        "gluEnd",
        "gluColor",
        NULL
    };

    if (!func || !func[0])
        return 0;

    for (const ReplEnumCommandSpec *def = repl_enum_command_specs(); def->name; def++) {
        if (command_name_matches_or_prefixes(func, def->name))
            return 1;
    }
    for (const ReplStdCommandSpec *def = repl_std_command_specs(); def->name; def++) {
        if (command_name_matches_or_prefixes(func, def->name))
            return 1;
    }
    for (int i = 0; special_funcs[i]; i++) {
        if (command_name_matches_or_prefixes(func, special_funcs[i]))
            return 1;
    }

    return strncmp(func, "func", 4) == 0 &&
           func[4] >= '0' && func[4] <= '9' &&
           func[5] == '\0';
}

/*
 * parse_command - Convert a single REPL text line into a GLCmd.
 *
 * This is the main entry point for the parser. It tries each command
 * grammar in order:
 *
 *   1. Comments (// ...)
 *   2. Table-driven enum commands (glBegin, glEnable, glShadeModel, ...)
 *   3. glEnd
 *   4. Table-driven standard commands (glVertex3f, glColor3f, glTranslatef, ...)
 *   5. Ad-hoc commands (glMaterialf, glPointParameterfv, glPush/PopMatrix,
 *      funcN calls, glu* tessellator commands, goto/label)
 *
 * Returns 1 on success (cmd populated), 0 on parse failure (status set).
 */
static int parse_command(const char *line, GLCmd *cmd,
                         const ReplParseContext *ctx) {
    int source_line_idx = ctx ? ctx->source_line_idx : repl_state_edit_line();
    ExprVar *vars = ctx ? ctx->vars : NULL;
    int num_vars = ctx ? ctx->num_vars : 0;
    char buf[MAX_LINE_LEN];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;

    int len = (int)strlen(p);
    while (len > 0 && (p[len - 1] == ';' || isspace((unsigned char)p[len - 1])))
        p[--len] = '\0';

    cmd->valid = 0;
    cmd->num_args = 0;
    cmd->source[0] = '\0';

    if (len == 0) return 0;

    /* Comment: line starts with // */
    if (p[0] == '/' && p[1] == '/') {
        cmd->type = CMD_COMMENT;
        cmd->valid = 1;
        cmd->is_auto = 0;
        cmd->num_args = 0;
        char indent[32];
        repl_source_scope_cmd_indent(source_line_idx, indent, sizeof(indent));
        snprintf(cmd->source, sizeof(cmd->source), "%s%s", indent, p);
        return 1;
    }

    char *open_p = strchr(p, '(');
    char *close_p = open_p ? strrchr(p, ')') : NULL;
    char func[64] = "";
    char args[MAX_LINE_LEN] = "";

    if (open_p) {
        int flen = (int)(open_p - p);
        if (flen > 0 && flen < (int)sizeof(func)) {
            memcpy(func, p, (size_t)flen);
            func[flen] = '\0';
        }

        if (!close_p || close_p < open_p) {
            if (!is_known_incomplete_func_name(func))
                goto unknown_command;
            set_incomplete_missing_paren_status(func);
            return 0;
        }

        int alen = (int)(close_p - open_p - 1);
        if (alen > 0 && alen < (int)sizeof(args)) {
            memcpy(args, open_p + 1, (size_t)alen);
            args[alen] = '\0';
        }
    } else {
        if (!repl_copy_string_fits(func, sizeof(func), p))
            goto unknown_command;
    }

    /* Table-driven parsing for enum commands */
    for (const ReplEnumCommandSpec *def = repl_enum_command_specs(); def->name; def++) {
        if (strcmp(func, def->name) == 0) {
            if (def->num_args == 1) {
                char *arg_str = args;
                while (*arg_str && isspace((unsigned char)*arg_str)) arg_str++;
                int arg_len = (int)strlen(arg_str);
                while (arg_len > 0 && isspace((unsigned char)arg_str[arg_len - 1])) arg_str[--arg_len] = '\0';
                for (int i = 0; def->enums1[i].name; i++) {
                    if (strcmp(arg_str, def->enums1[i].name) == 0) {
                        cmd->type = def->type;
                        cmd->mode = def->enums1[i].value;
                        cmd->valid = 1;
                        if (def->indent_type == 1) {
                            /* glBegin-style indent: 2 + 2*tess + 2*block  (begin depth excluded) */
                            char ind[32];
                            int td = repl_source_scope_tess_scope_depth_at(source_line_idx);
                            int kd = repl_source_scope_block_depth_at(source_line_idx);
                            int spaces = 2 + 2 * td + 2 * kd;
                            if (spaces > (int)sizeof(ind) - 1) spaces = (int)sizeof(ind) - 1;
                            memset(ind, ' ', (size_t)spaces);
                            ind[spaces] = '\0';
                            snprintf(cmd->source, sizeof(cmd->source), def->fmt, ind, def->enums1[i].name);
                        } else {
                            char ind[32];
                            repl_source_scope_cmd_indent(source_line_idx, ind, sizeof(ind));
                            snprintf(cmd->source, sizeof(cmd->source), def->fmt, ind, def->enums1[i].name);
                        }
                        return 1;
                    }
                }
                set_status(def->usage1);
                return 0;
            } else if (def->num_args == 2) {
                char raw_arg1[64] = "", raw_arg2[64] = "";
                char *comma = strchr(args, ',');
                if (!comma) { set_status(def->usage1 ? def->usage1 : "Invalid arguments"); return 0; }
                int len1 = (int)(comma - args);
                if (len1 >= (int)sizeof(raw_arg1)) len1 = (int)sizeof(raw_arg1) - 1;
                strncpy(raw_arg1, args, len1); raw_arg1[len1] = '\0';
                strncpy(raw_arg2, comma + 1, sizeof(raw_arg2) - 1);

                /* Trim whitespace from both arguments */
                char *trimmed1 = raw_arg1; while (*trimmed1 == ' ') trimmed1++;
                int tlen1 = (int)strlen(trimmed1); while (tlen1 > 0 && trimmed1[tlen1-1] == ' ') trimmed1[--tlen1] = '\0';
                char *trimmed2 = raw_arg2; while (*trimmed2 == ' ') trimmed2++;
                int tlen2 = (int)strlen(trimmed2); while (tlen2 > 0 && trimmed2[tlen2-1] == ' ') trimmed2[--tlen2] = '\0';

                GLenum val1 = 0;
                int found1 = 0, found2 = 0;
                float val2_f = 0.0f;

                for (int i = 0; def->enums1[i].name; i++) {
                    if (strcmp(trimmed1, def->enums1[i].name) == 0) { val1 = def->enums1[i].value; found1 = 1; break; }
                }
                for (int i = 0; def->enums2[i].name; i++) {
                    if (strcmp(trimmed2, def->enums2[i].name) == 0) { val2_f = (float)def->enums2[i].value; found2 = 1; break; }
                }
                if (!found1) { set_status(def->usage1); return 0; }

                if (!found2 && def->type == CMD_LIGHT_MODEL_I) {
                    char verr[128];
                    if (!repl_eval_validate_expression_idents(trimmed2, vars, num_vars, verr, sizeof(verr))) {
                        set_status(verr); return 0;
                    }
                    float fv; if (repl_eval_parse_exprs(trimmed2, &fv, 1, vars, num_vars) == 1) { val2_f = fv; found2 = 1; }
                }

                if (!found2) { set_status(def->usage2); return 0; }

                cmd->type = def->type;
                cmd->valid = 1;
                cmd->mode = val1;
                cmd->args[0] = val2_f;
                cmd->num_args = 1;
                char ind[32]; repl_source_scope_cmd_indent(source_line_idx, ind, sizeof(ind));
                snprintf(cmd->source, sizeof(cmd->source), def->fmt, ind, trimmed1, trimmed2);
                return 1;
            }
        }
    }

    /* glEnd() - aligns with its matching glBegin: 2 + 2*tess + 2*block (begin depth not added) */
    if (strcmp(func, "glEnd") == 0) {
        cmd->type = CMD_END;
        cmd->valid = 1;
        {
            int tess_depth = repl_source_scope_tess_scope_depth_at(source_line_idx);
            int kd = repl_source_scope_block_depth_at(source_line_idx);
            int spaces = 2 + 2 * tess_depth + 2 * kd;
            char end_ind[32];
            if (spaces > (int)sizeof(end_ind) - 1) spaces = (int)sizeof(end_ind) - 1;
            memset(end_ind, ' ', (size_t)spaces);
            end_ind[spaces] = '\0';
            snprintf(cmd->source, sizeof(cmd->source), "%sglEnd();", end_ind);
        }
        return 1;
    }

    /* Indent for gl commands: 2 + 2*tess + 2*begin */
    char indent_buf[32];
    repl_source_scope_cmd_indent(source_line_idx, indent_buf, sizeof(indent_buf));
    const char *indent = indent_buf;

    /* Indent for glu (tessellator) commands: 2 + 2*tess only.
     * glu commands belong to the tessellator scope, not the GL vertex block,
     * so glBegin depth is intentionally excluded. */
    char tess_indent_buf[32];
    repl_source_scope_cmd_tess_indent(source_line_idx, tess_indent_buf, sizeof(tess_indent_buf));
    const char *tess_indent = tess_indent_buf;

    /* Table-driven parsing for standard commands */
    for (const ReplStdCommandSpec *def = repl_std_command_specs(); def->name; def++) {
        if (strcmp(func, def->name) == 0) {
            {
                char verr[128];
                if (!repl_eval_validate_expression_idents(args, vars, num_vars, verr, sizeof(verr))) {
                    set_status(verr); return 0;
                }
            }
            int exact_count = 0;
            if (parse_expr_list_exact(args, cmd->args, def->num_args,
                                      vars, num_vars, &exact_count) &&
                exact_count == def->num_args) {
                cmd->num_args = exact_count;
                cmd->type = def->type;
                cmd->valid = 1;
                cmd->has_vars = input_has_any_visible_vars(args, vars, num_vars);

                const char *ind = def->is_tess ? tess_indent : indent;
                snprintf(cmd->source, sizeof(cmd->source), "%s", ind);
                size_t current_len = strlen(cmd->source);

                switch (def->num_args) {
                case 1:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0]);
                    break;
                case 2:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0], cmd->args[1]);
                    break;
                case 3:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0], cmd->args[1], cmd->args[2]);
                    break;
                case 4:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
                    break;
                case 5:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3], cmd->args[4]);
                    break;
                case 6:
                    snprintf(cmd->source + current_len, sizeof(cmd->source) - current_len,
                             def->fmt, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3], cmd->args[4], cmd->args[5]);
                    break;
                }
                /* glClearColor: clamp each RGB channel and rebuild source */
                if (def->type == CMD_CLEAR_COLOR) {
                    int clamped = 0;
                    for (int ci = 0; ci < 3; ci++) {
                        if (cmd->args[ci] > CP_CLEAR_MAX_V) {
                            cmd->args[ci] = CP_CLEAR_MAX_V;
                            clamped = 1;
                        }
                    }
                    if (clamped) {
                        snprintf(cmd->source, sizeof(cmd->source), "%s", ind);
                        size_t cl = strlen(cmd->source);
                        snprintf(cmd->source + cl, sizeof(cmd->source) - cl,
                                 def->fmt, cmd->args[0], cmd->args[1],
                                 cmd->args[2], cmd->args[3]);
                        if (!cmd->has_vars)
                            set_status("glClearColor: channels clamped to 0.15 max");
                    }
                }
                return 1;
            }
            cmd->num_args = repl_eval_parse_exprs(args, cmd->args, def->num_args, vars, num_vars);
            if (cmd->num_args < def->num_args)
                set_incomplete_arg_count_status(def->name, def->num_args, cmd->num_args);
            else
                set_status(def->usage);
            return 0;
        }
    }

    /* glMaterialf(face, pname, param) */
    if (strcmp(func, "glMaterialf") == 0) {
        char a1[64] = "", a2[64] = "", a3[MAX_LINE_LEN] = "";
        char *comma1 = strchr(args, ',');
        char *comma2 = comma1 ? strchr(comma1 + 1, ',') : NULL;

        if (!comma1 || !comma2) { set_status("Usage: glMaterialf(face, pname, params...)"); return 0; }

        int l1 = (int)(comma1 - args);
        if (l1 >= (int)sizeof(a1)) l1 = (int)sizeof(a1) - 1;
        strncpy(a1, args, l1); a1[l1] = '\0';

        int l2 = (int)(comma2 - (comma1 + 1));
        if (l2 >= (int)sizeof(a2)) l2 = (int)sizeof(a2) - 1;
        strncpy(a2, comma1 + 1, l2); a2[l2] = '\0';

        strncpy(a3, comma2 + 1, sizeof(a3) - 1);

        char *p1 = a1; while (*p1 == ' ') p1++;
        int e1 = (int)strlen(p1); while (e1 > 0 && p1[e1-1] == ' ') p1[--e1] = '\0';
        char *p2 = a2; while (*p2 == ' ') p2++;
        int e2 = (int)strlen(p2); while (e2 > 0 && p2[e2-1] == ' ') p2[--e2] = '\0';

        GLenum face = 0, pname = 0;
        int found1 = 0, found2 = 0;

        const EnumEntry *face_types = repl_face_type_entries();
        const EnumEntry *material_params = repl_material_param_entries();
        for (int i = 0; face_types[i].name; i++) {
            if (strcmp(p1, face_types[i].name) == 0) { face = face_types[i].value; found1 = 1; break; }
        }
        for (int i = 0; material_params[i].name; i++) {
            if (strcmp(p2, material_params[i].name) == 0) { pname = material_params[i].value; found2 = 1; break; }
        }

        if (!found1) { set_status("face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK"); return 0; }
        if (!found2) { set_status("pname: GL_DIFFUSE, GL_AMBIENT, GL_SPECULAR, GL_SHININESS..."); return 0; }

        {
            char verr[128];
            if (!repl_eval_validate_expression_idents(a3, vars, num_vars, verr, sizeof(verr))) {
                set_status(verr); return 0;
            }
        }
        float parsed_args[8];
        int num_parsed = repl_eval_parse_exprs(a3, parsed_args, 8, vars, num_vars);
        if (num_parsed != 1 && num_parsed != 4) {
            set_status("Expected 1 or 4 float values");
            return 0;
        }

        cmd->type = CMD_MATERIALF;
        cmd->valid = 1;
        cmd->mode = face;
        cmd->args[0] = (float)pname;
        for (int k = 0; k < num_parsed; k++) cmd->args[k + 1] = parsed_args[k];
        cmd->num_args = num_parsed + 1;
        cmd->has_vars = input_has_any_visible_vars(a3, vars, num_vars);

        if (num_parsed == 1) {
            snprintf(cmd->source, sizeof(cmd->source), "%sglMaterialf(%s, %s, %g);", indent, p1, p2, parsed_args[0]);
        } else {
            snprintf(cmd->source, sizeof(cmd->source), "%sglMaterialfv(%s, %s, (GLfloat[]){%g, %g, %g, %g});",
                     indent, p1, p2, parsed_args[0], parsed_args[1], parsed_args[2], parsed_args[3]);
        }

        return 1;
    }

    /* glPointParameterfv(pname, const, linear, quadratic) -
     * only GL_POINT_DISTANCE_ATTENUATION (size *= 1 / sqrt(const + linear*d + quadratic*d*d)) */
    if (strcmp(func, "glPointParameterfv") == 0) {
        char a1[64] = "", rest[MAX_LINE_LEN] = "";
        char *comma = strchr(args, ',');
        if (!comma) {
            set_status("Usage: glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)");
            return 0;
        }
        int l1 = (int)(comma - args);
        if (l1 >= (int)sizeof(a1)) l1 = (int)sizeof(a1) - 1;
        strncpy(a1, args, l1); a1[l1] = '\0';
        strncpy(rest, comma + 1, sizeof(rest) - 1);

        char *p1 = a1; while (*p1 == ' ') p1++;
        int e1 = (int)strlen(p1); while (e1 > 0 && p1[e1 - 1] == ' ') p1[--e1] = '\0';

        GLenum pname = 0;
        int found = 0;
        const EnumEntry *point_param_pnames = repl_point_param_pname_entries();
        for (int i = 0; point_param_pnames[i].name; i++) {
            if (strcmp(p1, point_param_pnames[i].name) == 0) {
                pname = point_param_pnames[i].value;
                found = 1;
                break;
            }
        }
        if (!found) {
            set_status("pname: GL_POINT_DISTANCE_ATTENUATION");
            return 0;
        }

        {
            char verr[128];
            if (!repl_eval_validate_expression_idents(rest, vars, num_vars, verr, sizeof(verr))) {
                set_status(verr); return 0;
            }
        }
        float parsed_args[4];
        int num_parsed = repl_eval_parse_exprs(rest, parsed_args, 4, vars, num_vars);
        if (num_parsed != 3) {
            set_status("Expected 3 floats: const, linear, quadratic attenuation coefficients");
            return 0;
        }

        cmd->type = CMD_POINT_PARAMETER_FV;
        cmd->valid = 1;
        cmd->mode = pname;
        cmd->args[0] = parsed_args[0];
        cmd->args[1] = parsed_args[1];
        cmd->args[2] = parsed_args[2];
        cmd->num_args = 3;
        cmd->has_vars = (num_vars > 0);

        char ind[32]; repl_source_scope_cmd_indent(source_line_idx, ind, sizeof(ind));
        snprintf(cmd->source, sizeof(cmd->source),
                 "%sglPointParameterfv(%s, (GLfloat[]){%g, %g, %g});",
                 ind, p1, parsed_args[0], parsed_args[1], parsed_args[2]);
        return 1;
    }

    /* glPushMatrix() */
    if (strcmp(func, "glPushMatrix") == 0) {
        cmd->type = CMD_PUSH_MATRIX;
        cmd->valid = 1;
        snprintf(cmd->source, sizeof(cmd->source), "%sglPushMatrix();", indent);
        return 1;
    }

    /* glPopMatrix() */
    if (strcmp(func, "glPopMatrix") == 0) {
        cmd->type = CMD_POP_MATRIX;
        cmd->valid = 1;
        snprintf(cmd->source, sizeof(cmd->source), "%sglPopMatrix();", indent);
        return 1;
    }

    /* funcN([expr, ...]) - function call */
    if (strncmp(func, "func", 4) == 0 && func[4] >= '0' && func[4] <= '9' &&
        func[5] == '\0' && open_p && close_p) {
        int fn = func[4] - '0';
        float dummy_vals[MAX_EXPR_VARS];
        int arg_count = 0;
        if (args[0] != '\0') {
            char verr[128];
            if (!repl_eval_validate_expression_idents(args, vars, num_vars, verr, sizeof(verr))) {
                set_status(verr); return 0;
            }
        }
        if (!parse_expr_list_exact(args, dummy_vals, MAX_EXPR_VARS,
                                   vars, num_vars, &arg_count)) {
            set_status("Invalid function call arguments");
            return 0;
        }

        /* In strict mode (commit path), require a matching CMD_FUNC_DEF
         * for top-level calls - same rule as `x = expr;` needing `float
         * x;`.  Inside a function body (block depth > 0) forward refs
         * are still allowed so mutual recursion keeps working. */
        if (ctx && ctx->strict_refs && repl_source_scope_block_depth_at(source_line_idx) == 0) {
            int def_exists = 0;
            for (int di = 0; di < repl_state_document_count(); di++) {
                if (!repl_state_document_cmds_mut()[di].valid) continue;
                if (repl_state_document_cmds_mut()[di].type != CMD_FUNC_DEF) continue;
                if ((int)repl_state_document_cmds_mut()[di].args[0] != fn) continue;
                def_exists = 1;
                break;
            }
            if (!def_exists) {
                char buf[96];
                snprintf(buf, sizeof(buf),
                         "undefined function 'func%d' - define it first", fn);
                set_status(buf);
                return 0;
            }
        }

        cmd->type = CMD_CALL;
        cmd->valid = 1;
        cmd->args[0] = (float)fn;
        cmd->num_args = arg_count;
        cmd->has_vars = input_has_any_visible_vars(args, vars, num_vars);

        int fdepth = repl_source_scope_block_depth_at(source_line_idx);
        int bb = repl_source_scope_in_begin_block_at(source_line_idx);
        int ind_v = (bb ? 4 : 2) + fdepth * 2;
        char ind_str[32];
        if (ind_v > (int)sizeof(ind_str) - 1) ind_v = (int)sizeof(ind_str) - 1;
        memset(ind_str, ' ', ind_v);
        ind_str[ind_v] = '\0';

        char raw_args[MAX_LINE_LEN];
        strncpy(raw_args, args, sizeof(raw_args) - 1);
        raw_args[sizeof(raw_args) - 1] = '\0';
        trim_in_place(raw_args);
        if (arg_count > 0) {
            if (!repl_format_fits(cmd->source, sizeof(cmd->source),
                                  "%sfunc%d(%s);", ind_str, fn, raw_args)) {
                set_status("Command too long");
                return 0;
            }
        } else if (!repl_format_fits(cmd->source, sizeof(cmd->source),
                                     "%sfunc%d();", ind_str, fn)) {
            set_status("Command too long");
            return 0;
        }
        return 1;
    }

    /* gluBegin(GLU_POLYGON) - start a tessellated polygon */
    if (strcmp(func, "gluBegin") == 0) {
        char *a = args; while (*a && isspace((unsigned char)*a)) a++;
        if (strncmp(a, "GLU_POLYGON", 11) == 0) {
            cmd->type = CMD_TESS_BEGIN_POLYGON;
            cmd->valid = 1;
            snprintf(cmd->source, sizeof(cmd->source), "%sgluBegin(GLU_POLYGON);", tess_indent);
            return 1;
        }
        if (strncmp(a, "GLU_CONTOUR", 11) == 0) {
            cmd->type = CMD_TESS_BEGIN_CONTOUR;
            cmd->valid = 1;
            snprintf(cmd->source, sizeof(cmd->source), "%sgluBegin(GLU_CONTOUR);", tess_indent);
            return 1;
        }
        set_status("Usage: gluBegin(GLU_POLYGON) or gluBegin(GLU_CONTOUR)");
        return 0;
    }

    /* gluEnd() - end tessellator contour or polygon.
     * Indent at the *enclosing* level (tess_depth - 1), same logic as glEnd()
     * always being at 2-space rather than the 4-space inside a glBegin block.
     * Formula: 2 + 2*(tess-1) + 2*block  (begin depth excluded). */
    if (strcmp(func, "gluEnd") == 0 || strcmp(p, "gluEnd()") == 0) {
        cmd->type = CMD_TESS_END;
        cmd->valid = 1;
        {
            int td = repl_source_scope_tess_scope_depth_at(source_line_idx);
            if (td > 0) td--;
            int kd = repl_source_scope_block_depth_at(source_line_idx);
            int spaces = 2 + 2 * td + 2 * kd;
            char close_ind[32];
            if (spaces > (int)sizeof(close_ind) - 1) spaces = (int)sizeof(close_ind) - 1;
            memset(close_ind, ' ', (size_t)spaces);
            close_ind[spaces] = '\0';
            snprintf(cmd->source, sizeof(cmd->source), "%sgluEnd();", close_ind);
        }
        return 1;
    }

    /* gluColor(r, g, b[, a]) - set per-vertex color for tessellator */
    if (strcmp(func, "gluColor") == 0) {
        {
            char verr[128];
            if (!repl_eval_validate_expression_idents(args, vars, num_vars, verr, sizeof(verr))) {
                set_status(verr); return 0;
            }
        }
        cmd->num_args = repl_eval_parse_exprs(args, cmd->args, 4, vars, num_vars);
        if (cmd->num_args >= 3) {
            if (cmd->num_args < 4) cmd->args[3] = 1.0f;
            cmd->num_args = 4;
            cmd->type = CMD_TESS_COLOR;
            cmd->valid = 1;
            cmd->has_vars = input_has_any_visible_vars(args, vars, num_vars);
            snprintf(cmd->source, sizeof(cmd->source),
                     "%sgluColor(%g, %g, %g, %g);",
                     tess_indent, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
            return 1;
        }
        set_status("Usage: gluColor(r, g, b) or gluColor(r, g, b, a)");
        return 0;
    }

    /* goto label - jump to a named label.
     *
     * Current limitations:
     * - top-level only; flatten rejects labels/gotos inside functions
     * - executor updates control flow, assignments, and if-conditions, but
     *   variable-driven GL commands inside goto loops are still using their
     *   flattened args rather than being re-evaluated per jump
     * - replay intentionally does not model dynamic goto traces
     */
    if (strncmp(p, "goto ", 5) == 0) {
        const char *lname = p + 5;
        while (*lname && isspace((unsigned char)*lname)) lname++;
        /* Extract clean label name (strip trailing ; or whitespace) */
        char clean_lname[64]; int ll = 0;
        while (ll < 63 && lname[ll] && lname[ll] != ';' && !isspace((unsigned char)lname[ll])) {
            clean_lname[ll] = lname[ll]; ll++;
        }
        clean_lname[ll] = '\0';
        if (ll > 0) {
            cmd->type = CMD_GOTO;
            cmd->valid = 1;
            int fdepth = repl_source_scope_block_depth_at(source_line_idx);
            int bb_v = repl_source_scope_in_begin_block_at(source_line_idx);
            int ind_v = (bb_v ? 4 : 2) + fdepth * 2;
            char ind_str[32];
            if (ind_v > (int)sizeof(ind_str) - 1) ind_v = (int)sizeof(ind_str) - 1;
            memset(ind_str, ' ', ind_v); ind_str[ind_v] = '\0';
            snprintf(cmd->source, sizeof(cmd->source), "%sgoto %s;", ind_str, clean_lname);
            return 1;
        }
    }

    /* :label or label: - define a label */
    if ((p[0] == ':' && p[1] && !isspace((unsigned char)p[1])) ||
        (len > 1 && p[len - 1] == ':' && !isspace((unsigned char)p[0]))) {
        cmd->type = CMD_LABEL;
        cmd->valid = 1;
        /* labels go at column 0 in C */
        if (p[0] == ':') {
            snprintf(cmd->source, sizeof(cmd->source), "%s:", p + 1);
        } else {
            char label[64];
            int n = 0;
            while (n < (int)sizeof(label) - 1 &&
                   p[n] && p[n] != ':' && !isspace((unsigned char)p[n])) {
                label[n] = p[n];
                n++;
            }
            label[n] = '\0';
            if (n <= 0)
                return 0;
            snprintf(cmd->source, sizeof(cmd->source), "%s:", label);
        }
        return 1;
    }

unknown_command:
    set_status("Unknown cmd. Try glVertex3f, glBegin, glEnable, glShadeModel, ...");
    return 0;
}

int repl_parser_parse_command(const char *line, GLCmd *cmd) {
    ReplParseContext ctx = { repl_state_edit_line(), NULL, 0, 0 };
    return parse_command(line, cmd, &ctx);
}

int repl_parser_parse_command_with_vars(const char *line, GLCmd *cmd,
                                 ExprVar *vars, int num_vars) {
    ReplParseContext ctx = { repl_state_edit_line(), vars, num_vars, 0 };
    return parse_command(line, cmd, &ctx);
}

int repl_parser_parse_command_ctx(const char *line, ReplParsedLine *out,
                           const ReplParseContext *ctx) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    int r = parse_command(line, &out->cmd, ctx);
    if (r)
        repl_command_store_source_to_line(out->text, sizeof(out->text),
                                         out->cmd.source);
    return r;
}
