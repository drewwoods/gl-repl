#include "repl/export_internal.h"
#include "repl/text_helpers.h"
#include "repl/util.h"          /* repl_format_fits */

/* Emit a CMD_FOR_BEGIN as C89: a marker-tagged scope brace + hoisted
 * `float i;` decl (C89 has no for-init declarations; the markers let
 * import_is_c89_loop_marker_line drop the scaffolding on re-import),
 * then the `for (...)` header. Two paths: a has_vars loop re-extracts
 * the start/end/step expression text from the source line and runs it
 * through the expression-to-C translator (so `t`-animated bounds export
 * live); a constant loop re-parses the header to floats and emits
 * literals. Half-open REPL semantics map to `<` / `>` by step sign. */
static void write_for_begin_as_c(FILE *f, const GLCmd *cmd,
                                 const char *source_text) {
    char var_name[REPL_PREDEF_NAME_MAX];
    const char *p = source_text;
    int indent = 0;
    while (p[indent] && isspace((unsigned char)p[indent])) indent++;

    char ind[REPL_INDENT_TEXT_MAX];
    if (indent > (int)sizeof(ind) - 1) indent = (int)sizeof(ind) - 1;
    memset(ind, ' ', (size_t)indent);
    ind[indent] = '\0';

    if (cmd->has_vars) {
        const char *hp = p;
        while (*hp && *hp != '(') hp++;
        if (*hp) hp++;
        while (*hp && isspace((unsigned char)*hp)) hp++;
        int var_name_idx = 0;
        while (*hp && (isalnum((unsigned char)*hp) || *hp == '_') &&
               var_name_idx < (int)sizeof(var_name) - 1)
            var_name[var_name_idx++] = *hp++;
        var_name[var_name_idx] = '\0';
        while (*hp && isspace((unsigned char)*hp)) hp++;
        if (*hp == ',') hp++;

        char start_s[128] = "", end_s[128] = "", step_s[128] = "";
        int nargs = 0;
        char *dests[] = { start_s, end_s, step_s };
        int dsizes[] = { (int)sizeof(start_s), (int)sizeof(end_s), (int)sizeof(step_s) };
        while (*hp && *hp != ')' && nargs < 3) {
            while (*hp && isspace((unsigned char)*hp)) hp++;
            const char *as = hp;
            hp = repl_scan_next_arg_delim(hp);
            int alen = (int)(hp - as);
            while (alen > 0 && isspace((unsigned char)as[alen - 1])) alen--;
            if (alen > dsizes[nargs] - 1) alen = dsizes[nargs] - 1;
            memcpy(dests[nargs], as, (size_t)alen);
            dests[nargs][alen] = '\0';
            nargs++;
            if (*hp == ',') hp++;
        }

        char c_start[128], c_end[128], c_step[128];
        repl_eval_expr_to_c(start_s, c_start, sizeof(c_start));
        repl_eval_expr_to_c(end_s, c_end, sizeof(c_end));
        if (step_s[0])
            repl_eval_expr_to_c(step_s, c_step, sizeof(c_step));
        else
            strncpy(c_step, "1.0f", sizeof(c_step));

        if (var_name[0] == '\0') {
            export_write_c89_line(f, source_text);
            return;
        }

        fprintf(f, "%s{ /* %s */\n", ind, REPL_EXPORT_C89_LOOP_SCOPE_MARKER);
        fprintf(f, "%sfloat %s; /* %s */\n",
                ind, var_name, REPL_EXPORT_C89_LOOP_VAR_MARKER);
        float step_v = cmd->args[2];
        if (step_v >= 0) {
            fprintf(f, "%sfor (%s = %s; %s < %s; %s += %s) {\n",
                    ind, var_name, c_start, var_name, c_end, var_name, c_step);
        } else {
            fprintf(f, "%sfor (%s = %s; %s > %s; %s += %s) {\n",
                    ind, var_name, c_start, var_name, c_end, var_name, c_step);
        }
        return;
    }

    float start_v, end_v, step_v;
    const char *body;
    if (repl_eval_parse_for_header(
            &(ReplForHeaderParseConfig){
                .input = p,
                .var_name = var_name,
                .var_sz = (int)sizeof(var_name),
                .start = &start_v,
                .end = &end_v,
                .step = &step_v,
                .body_start = &body,
            })) {
        char start_s[EXPORT_FLOAT_TEXT_MAX], end_s[EXPORT_FLOAT_TEXT_MAX];
        repl_format_source_float(start_s, sizeof(start_s), start_v);
        repl_format_source_float(end_s, sizeof(end_s), end_v);
        fprintf(f, "%s{ /* %s */\n", ind, REPL_EXPORT_C89_LOOP_SCOPE_MARKER);
        fprintf(f, "%sfloat %s; /* %s */\n",
                ind, var_name, REPL_EXPORT_C89_LOOP_VAR_MARKER);
        if (step_v == 1.0f) {
            fprintf(f, "%sfor (%s = %s; %s < %s; %s += 1.0f) {\n",
                    ind, var_name, start_s, var_name, end_s, var_name);
        } else if (step_v == -1.0f) {
            fprintf(f, "%sfor (%s = %s; %s > %s; %s -= 1.0f) {\n",
                    ind, var_name, start_s, var_name, end_s, var_name);
        } else {
            char step_s[EXPORT_FLOAT_TEXT_MAX];
            repl_format_source_float(step_s, sizeof(step_s), step_v);
            fprintf(f, "%sfor (%s = %s; %s %s %s; %s += %s) {\n",
                    ind, var_name, start_s, var_name,
                    step_v > 0 ? "<" : ">", end_s, var_name, step_s);
        }
    } else {
        export_write_c89_line(f, source_text);
    }
}

static void write_for_end_as_c(FILE *f, const char *source_text) {
    int indent = 0;

    while (source_text[indent] && isspace((unsigned char)source_text[indent]))
        indent++;

    export_write_c89_line(f, source_text);
    fprintf(f, "%.*s} /* %s */\n",
            indent, source_text, REPL_EXPORT_C89_LOOP_SCOPE_MARKER);
}

static int cmd_type_is_tess(CmdType t) {
    return t == CMD_TESS_BEGIN_POLYGON ||
           t == CMD_TESS_BEGIN_CONTOUR ||
           t == CMD_TESS_END ||
           t == CMD_TESS_NORMAL ||
           t == CMD_TESS_COLOR ||
           t == CMD_TESS_VERTEX;
}

int export_uses_tess_commands(void) {
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds()[cmd_idx].valid)
            continue;
        if (cmd_type_is_tess(repl_state_document_cmds()[cmd_idx].type))
            return 1;
    }

    return 0;
}

/* Marks a generated normal (GLCmd.is_auto) so import can restore the flag.
 * The tess arms build their own lowered C, so the marker is appended to the
 * finished line rather than to REPL source text; write_normal_as_c below
 * carries the immediate-mode half. */
static const char *tess_auto_marker(const GLCmd *cmd) {
    return cmd->is_auto ? " /* " REPL_EXPORT_AUTO_NORMAL_MARKER " */" : "";
}

static int write_tess_source_as_c(FILE *f, const GLCmd *cmd,
                                  const char *source_text) {
    char payload[MAX_LINE_LEN];
    char raw_args[4][MAX_LINE_LEN];
    char c_args[4][MAX_LINE_LEN];
    int arg_count;

    if (!repl_extract_paren_payload(source_text, payload, sizeof(payload)))
        return 0;

    arg_count = split_top_level_args(payload, raw_args, 4);
    if (arg_count < 0)
        return 0;

    for (int arg_idx = 0; arg_idx < arg_count; arg_idx++)
        repl_eval_expr_to_c(raw_args[arg_idx], c_args[arg_idx], sizeof(c_args[arg_idx]));

    switch (cmd->type) {
    case CMD_TESS_NORMAL:
        if (arg_count != 3)
            return 0;
        fprintf(f, "      { _tn[0] = %s; _tn[1] = %s; _tn[2] = %s; }%s\n",
                c_args[0], c_args[1], c_args[2], tess_auto_marker(cmd));
        return 1;
    case CMD_TESS_COLOR:
        if (arg_count == 3) {
            strncpy(c_args[3], "1", sizeof(c_args[3]) - 1);
            c_args[3][sizeof(c_args[3]) - 1] = '\0';
            arg_count = 4;
        }
        if (arg_count != 4)
            return 0;
        fprintf(f,
                "      { _tc[0] = %s; _tc[1] = %s; _tc[2] = %s; _tc[3] = %s; }\n",
                c_args[0], c_args[1], c_args[2], c_args[3]);
        return 1;
    case CMD_TESS_VERTEX:
        if (arg_count != 3)
            return 0;
        fprintf(f,
                "      {TessVertex*_v=&_tv[_tv_n++];"
                "_v->pos[0]=%s;_v->pos[1]=%s;_v->pos[2]=%s;"
                "memcpy(_v->normal,_tn,sizeof _v->normal);"
                "memcpy(_v->color,_tc,sizeof _v->color);"
                "gluTessVertex(g_tess,_v->pos,_v);}\n",
                c_args[0], c_args[1], c_args[2]);
        return 1;
    default:
        return 0;
    }
}

void format_cmd_source_as_c(char *out, size_t out_sz,
                            const char *source_text,
                            int translate_exprs) {
    char c_src[MAX_LINE_LEN];

    if (!out || out_sz == 0)
        return;

    if (translate_exprs)
        repl_eval_expr_to_c(source_text, c_src, sizeof(c_src));
    else {
        strncpy(c_src, source_text, sizeof(c_src) - 1);
        c_src[sizeof(c_src) - 1] = '\0';
    }

    snprintf(out, out_sz, "%s", c_src);
}

static void write_cmd_source_as_c(FILE *f, const char *source_text,
                                  int translate_exprs) {
    char out[MAX_LINE_LEN];

    format_cmd_source_as_c(out, sizeof(out), source_text, translate_exprs);
    export_write_c89_line(f, out);
}

/* A normal the autonormal pass generated, tagged so import can restore
 * is_auto. Without the tag a reloaded scene's normals read as hand-written,
 * and the pass then refuses to touch them (a hand-written normal owns its
 * block) - the normals silently freeze at their exported values.
 *
 * The marker is emitted in REPL `//` form and converted to a C89 block
 * comment by export_write_c89_line, which is also what lets import's
 * block-comment normalizer hand it back as `// @auto`. An is_auto row has
 * no trailing comment of its own to collide with - the pass rewrites the
 * row's text wholesale on every recompute - but a row that somehow carries
 * one keeps it and goes untagged rather than emitting two comments. */
static void write_normal_as_c(FILE *f, const GLCmd *cmd,
                              const char *source_text, int translate_exprs) {
    char out[MAX_LINE_LEN];

    format_cmd_source_as_c(out, sizeof(out), source_text, translate_exprs);
    /* Plain strstr is exact here: a normal's args are numeric expressions,
     * so there is no string literal for a bare "//" to hide inside. */
    if (cmd->is_auto && !strstr(out, "//")) {
        size_t len = strlen(out);
        snprintf(out + len, sizeof(out) - len, " // %s",
                 REPL_EXPORT_AUTO_NORMAL_MARKER);
    }
    export_write_c89_line(f, out);
}


int find_export_block_end(int begin_idx) {
    int depth = 1;

    for (int cmd_idx = begin_idx + 1; cmd_idx < repl_state_document_count(); cmd_idx++) {
        CmdType t = repl_state_document_cmds()[cmd_idx].type;
        if (repl_cmd_is_block_head(t)) depth++;
        else if (repl_cmd_is_block_end(t)) {
            if (--depth == 0)
                return cmd_idx;
        }
    }

    return repl_state_document_count();
}

static int comment_run_attached_func_idx(int start, int end_idx) {
    int cmd_idx = start;
    while (cmd_idx < end_idx && cmd_idx < repl_state_document_count() &&
           repl_state_document_cmds()[cmd_idx].valid &&
           (repl_state_document_cmds()[cmd_idx].type == CMD_COMMENT ||
            repl_state_document_cmds()[cmd_idx].type == CMD_EMPTY))
        cmd_idx++;
    if (cmd_idx > start && cmd_idx < end_idx && cmd_idx < repl_state_document_count() &&
        repl_state_document_cmds()[cmd_idx].valid && repl_state_document_cmds()[cmd_idx].type == CMD_FUNC_DEF)
        return cmd_idx;
    return -1;
}

static void write_func_body_marker(FILE *f, int func_idx) {
    int slot;

    if (func_idx < 0 || func_idx >= repl_state_document_count())
        return;
    if (!repl_state_document_cmds()[func_idx].valid ||
        repl_state_document_cmds()[func_idx].type != CMD_FUNC_DEF)
        return;

    slot = (int)repl_state_document_cmds()[func_idx].args[0];
    fprintf(f, "  /* @%s %d */\n",
            REPL_SNIPPET_DIRECTIVE_FUNC_BODY, slot);
}

/* The per-command writer for the exported display() body: emit one
 * source command as standalone C. One independent case per command
 * family - most canonical REPL text is already valid C and passes
 * through the default arm (expression-to-C translated when the command
 * carries vars); the exceptions each own a case: var declares become
 * `// @declare` markers (locals would shadow the file-scope globals),
 * tess commands expand to the gluTess* call sequences, and label()
 * keeps its format string byte-exact. Block structure (for/func) is the
 * caller's job. The import translators in src/repl/import.c are the
 * inverses of these arms - change them in pairs to keep export/import
 * round-trips lossless. */
static void write_canonical_cmd_as_c(FILE *f, const GLCmd *cmd, int cmd_idx,
                                     int for_depth, int *tess_depth) {
    const char *source_text = export_document_text(cmd_idx);

    switch (cmd->type) {
    case CMD_EMPTY:
        fputc('\n', f);
        break;
    case CMD_COMMENT:
        export_write_c89_line(f, source_text);
        break;
    case CMD_VAR_DECLARE: {
        if (cmd->var_idx == REPL_VAR_IDX_LOCAL) {
            /* A function-scoped local is a real C automatic written at its
             * body position - there is no file-scope static for it to
             * shadow, so no marker is needed.
             *
             * The zero-initializers are not cosmetic. The REPL binds every
             * local to 0.0f on call entry, so a bare `float a, b;` would
             * read 0 in the REPL and be *undefined* in the generated file.
             * docs/ARCHITECTURE.md makes behavior parity a contract rather
             * than a preference, and undefined behavior is precisely the
             * one thing the REPL cannot reproduce - so "match C" was never
             * available as a resolution. Import lowers this form back to
             * `float a;`, which keeps the round-trip idempotent. */
            /* The emitted line is *wider than its source*: the source row
             * can already run to MAX_LINE_LEN, and each name grows by
             * strlen(" = 0.0f"). Building into a MAX_LINE_LEN buffer would
             * silently truncate, and what falls off the end is the
             * trailing comment. The static assert keeps that arithmetic
             * honest if either limit moves. */
            char line[MAX_LINE_LEN * 2];
            /* export_document_text() returns "" for every row it cannot
             * resolve, never NULL - so this and the indent scan below both
             * dereference unconditionally. */
            const char *comment = strstr(source_text, "//");
            int indent = 0;
            int off;

            STATIC_ASSERT(MAX_LINE_LEN + MAX_NAMES_PER_DECL * 7 + 2 <=
                              MAX_LINE_LEN * 2,
                          "local decl export buffer must fit the widened line");

            while (source_text[indent] &&
                   isspace((unsigned char)source_text[indent]))
                indent++;
            if (indent > (int)sizeof(line) - 1)
                indent = (int)sizeof(line) - 1;
            memset(line, ' ', (size_t)indent);
            off = indent;
            off += snprintf(line + off, sizeof(line) - (size_t)off, "float ");
            for (int di = 0; di < cmd->payload.decl.count &&
                             off < (int)sizeof(line) - 8; di++)
                off += snprintf(line + off, sizeof(line) - (size_t)off,
                                "%s%s = 0.0f", di ? ", " : "",
                                cmd->payload.decl.names[di]);
            if (!repl_format_fits(line + off, sizeof(line) - (size_t)off,
                                  ";%s%s", comment ? " " : "",
                                  comment ? comment : "")) {
                /* Unreachable given the assert above; refuse to emit a
                 * half-written declaration rather than truncate one. */
                snprintf(line + off, sizeof(line) - (size_t)off, ";");
            }
            export_write_c89_line(f, line);
            break;
        }
        /* Variables are emitted as file-scope statics by write_predef_var_globals().
         * We cannot write a local float declaration here because it would shadow
         * the file-scope global.  Instead, emit a special REPL marker comment so
         * the importer can recreate the CMD_VAR_DECLARE when loading back into the
         * REPL without creating a C local variable.
         *
         * We no longer emit initializers (`name=value`) in the @declare comment.
         * The values are instead stashed from the static float declarations at
         * import time. */
        fprintf(f, "  /* @%s", REPL_SNIPPET_DIRECTIVE_DECLARE);
        for (int di = 0; di < cmd->payload.decl.count; di++) {
            fprintf(f, " %s", cmd->payload.decl.names[di]);
        }
        /* Round-trip the @tune / @config tags: import re-attaches them to the
         * reconstructed decl line. Import's name loop stops at `@`, so the
         * trailing tokens detach cleanly. */
        if (repl_eval_line_has_tune_tag(source_text))
            fprintf(f, " @tune");
        if (repl_eval_line_has_config_tag(source_text))
            fprintf(f, " @config");
        fprintf(f, " */\n");
        break;
    }
    case CMD_VAR_ASSIGN: {
        char c_src[MAX_LINE_LEN];
        repl_eval_expr_to_c(source_text, c_src, sizeof(c_src));
        export_write_c89_line(f, c_src);
        break;
    }
    case CMD_SCRATCH_ASSIGN: {
        char c_src[MAX_LINE_LEN];
        repl_eval_expr_to_c(source_text, c_src, sizeof(c_src));
        export_write_c89_line(f, c_src);
        break;
    }
    case CMD_SCRATCH_BLOCK_ASSIGN:
        /* No source-text fallback: the REPL form is not C, so a row this
         * writer cannot lower would export as something that does not
         * compile. Dropping it is equally wrong, so emit the block's baked
         * cell values - the same shape, minus the animation. */
        if (!write_scratch_block_as_c89(f, source_text)) {
            int indent = 0;
            int cell;
            while (source_text[indent] == ' ' || source_text[indent] == '\t')
                indent++;
            fprintf(f, "%.*s", indent, source_text);
            for (cell = 0; cell < (int)cmd->args[2]; cell++) {
                char v[EXPORT_FLOAT_TEXT_MAX];
                repl_format_source_float(v, sizeof(v),
                                         cmd->payload.scratch_block.v[cell]);
                /* Cast rather than an `f` suffix: repl_format_source_float
                 * drops the fractional part of a whole number, and `2f` is
                 * not a C literal. */
                fprintf(f, "%s%c[%d] = (float)%s;",
                        cell ? " " : "",
                        (char)('A' + (int)cmd->args[0]),
                        (int)cmd->args[1] + cell, v);
            }
            fprintf(f, "\n");
        }
        break;
    case CMD_CALL:
        write_cmd_source_as_c(f, source_text, 1);
        break;
    case CMD_MATERIALFV:
        if (!write_materialfv_as_c89(f, source_text))
            write_cmd_source_as_c(f, source_text, cmd->has_vars);
        break;
    case CMD_POINT_PARAMETER_FV:
        if (!write_point_parameterfv_as_c89(f, source_text))
            write_cmd_source_as_c(f, source_text, cmd->has_vars);
        break;
    case CMD_CLIP_PLANE:
        if (!write_clip_plane_as_c89(f, source_text))
            write_cmd_source_as_c(f, source_text, cmd->has_vars);
        break;
    case CMD_MULT_MATRIXF:
        /* The array form is already C: `glMultMatrixf(A);` against the
         * scratch global the needs pass declared for it. */
        if (repl_cmd_mult_matrix_from_array(cmd) ||
            !write_mult_matrixf_as_c89(f, source_text))
            write_cmd_source_as_c(f, source_text, cmd->has_vars);
        break;
    case CMD_FOG_FV:
        if (!write_fog_fv_as_c89(f, source_text))
            write_cmd_source_as_c(f, source_text, cmd->has_vars);
        break;
    case CMD_TESS_BEGIN_POLYGON:
        fprintf(f, "  { _tv_n = 0; gluTessBeginPolygon(g_tess, NULL); }\n");
        *tess_depth = 1;
        break;
    case CMD_TESS_BEGIN_CONTOUR:
        fprintf(f, "    gluTessBeginContour(g_tess);\n");
        *tess_depth = 2;
        break;
    case CMD_TESS_END:
        if (*tess_depth == 2) {
            fprintf(f, "    gluTessEndContour(g_tess);\n");
            *tess_depth = 1;
        } else {
            fprintf(f, "  gluTessEndPolygon(g_tess);\n");
            *tess_depth = 0;
        }
        break;
    case CMD_NORMAL3F:
        write_normal_as_c(f, cmd, source_text, for_depth > 0 || cmd->has_vars);
        break;
    case CMD_TESS_NORMAL:
        if (!write_tess_source_as_c(f, cmd, source_text)) {
            char x[EXPORT_FLOAT_TEXT_MAX], y[EXPORT_FLOAT_TEXT_MAX], z[EXPORT_FLOAT_TEXT_MAX];
            repl_format_source_float(x, sizeof(x), cmd->args[0]);
            repl_format_source_float(y, sizeof(y), cmd->args[1]);
            repl_format_source_float(z, sizeof(z), cmd->args[2]);
            fprintf(f, "      { _tn[0] = %s; _tn[1] = %s; _tn[2] = %s; }%s\n",
                    x, y, z, tess_auto_marker(cmd));
        }
        break;
    case CMD_TESS_COLOR:
        if (!write_tess_source_as_c(f, cmd, source_text)) {
            char r[EXPORT_FLOAT_TEXT_MAX], g[EXPORT_FLOAT_TEXT_MAX],
                 b[EXPORT_FLOAT_TEXT_MAX], a[EXPORT_FLOAT_TEXT_MAX];
            repl_format_source_float(r, sizeof(r), cmd->args[0]);
            repl_format_source_float(g, sizeof(g), cmd->args[1]);
            repl_format_source_float(b, sizeof(b), cmd->args[2]);
            repl_format_source_float(a, sizeof(a), cmd->args[3]);
            fprintf(f,
                    "      { _tc[0] = %s; _tc[1] = %s; _tc[2] = %s; _tc[3] = %s; }\n",
                    r, g, b, a);
        }
        break;
    case CMD_TESS_VERTEX:
        if (!write_tess_source_as_c(f, cmd, source_text)) {
            char x[EXPORT_FLOAT_TEXT_MAX], y[EXPORT_FLOAT_TEXT_MAX], z[EXPORT_FLOAT_TEXT_MAX];
            repl_format_source_float(x, sizeof(x), cmd->args[0]);
            repl_format_source_float(y, sizeof(y), cmd->args[1]);
            repl_format_source_float(z, sizeof(z), cmd->args[2]);
            fprintf(f,
                    "      {TessVertex*_v=&_tv[_tv_n++];"
                    "_v->pos[0]=%s;_v->pos[1]=%s;_v->pos[2]=%s;"
                    "memcpy(_v->normal,_tn,sizeof _v->normal);"
                    "memcpy(_v->color,_tc,sizeof _v->color);"
                    "gluTessVertex(g_tess,_v->pos,_v);}\n",
                    x, y, z);
        }
        break;
    case CMD_LABEL: {
        /* Split format / post-args, translate the post-args
         * through repl_eval_expr_to_c, and re-emit with the
         * format string preserved byte-exact. The default branch
         * can't be used: repl_eval_expr_to_c walks the whole line
         * (including string contents) and would rewrite substrings
         * like "sin" or "PI" appearing inside the format string.
         *
         * Emits `label(...)`, a REPL-specific primitive whose
         * standalone-C definition is provided by a static wrapper
         * emitted in the file's prologue when needs_label is set.
         * See write_label_helper. */
        const char *open_p = strchr(source_text, '(');
        const char *close_p = open_p ? strrchr(source_text, ')') : NULL;
        if (open_p && close_p && close_p > open_p) {
            int prefix_len = (int)(open_p - source_text) + 1;
            int args_len = (int)(close_p - (open_p + 1));
            char args_str[MAX_LINE_LEN];
            if (args_len < 0) args_len = 0;
            if (args_len >= (int)sizeof(args_str))
                args_len = (int)sizeof(args_str) - 1;
            memcpy(args_str, open_p + 1, (size_t)args_len);
            args_str[args_len] = '\0';

            char fmt[GLUT_BITMAP_FMT_MAX] = "";
            char post[MAX_LINE_LEN] = "";
            char split_err[REPL_DIAG_TEXT_MAX] = "";
            if (repl_label_split_args(args_str,
                                      fmt, (int)sizeof(fmt),
                                      post, (int)sizeof(post),
                                      split_err, (int)sizeof(split_err))) {
                char post_c[MAX_LINE_LEN] = "";
                if (post[0])
                    repl_eval_expr_to_c(post, post_c, sizeof(post_c));
                {
                    char label_line[MAX_LINE_LEN];
                    int take = prefix_len;
                    if (take >= (int)sizeof(label_line))
                        take = (int)sizeof(label_line) - 1;
                    memcpy(label_line, source_text, (size_t)take);
                    label_line[take] = '\0';
                    snprintf(label_line + take, sizeof(label_line) - (size_t)take,
                             "\"%s\"%s%s);",
                             fmt, post_c[0] ? ", " : "", post_c);
                    export_write_c89_line(f, label_line);
                }
                break;
            }
        }
        /* Fallback: emit raw text - the importer handles it via the
         * default repl_eval_c_expr_to_repl path. */
        export_write_c89_line(f, source_text);
        break;
    }
    default:
        write_cmd_source_as_c(f, source_text, for_depth > 0 || cmd->has_vars);
        break;
    }
}

/* True when this row is a function-scoped declaration the caller is
 * hoisting to the top of the emitted body. */
static int export_row_is_hoisted_local(int cmd_idx, int hoist_local_decls) {
    const GLCmd *cmd;

    if (!hoist_local_decls || cmd_idx < 0 ||
        cmd_idx >= repl_state_document_count())
        return 0;
    cmd = &repl_state_document_cmds()[cmd_idx];
    return cmd->valid && cmd->type == CMD_VAR_DECLARE &&
           cmd->var_idx == REPL_VAR_IDX_LOCAL;
}

void write_render_body_range_as_c(FILE *f, int start, int end_idx,
                                  int skip_func_defs, int hoist_local_decls) {
    int for_depth = 0;
    int tess_depth = 0;
    int prelude_end = start;

    /* C89 has no declaration-after-statement, and this exporter targets
     * C89 - it already hoists for-loop variables into a scope brace for
     * the same reason. A local declaration is normally the body's first
     * row, but nothing keeps it there: ordinary insert-mode editing can
     * push a statement ahead of one, and flatten binds it wherever it
     * lands. Preserve the full leading comment/blank/declaration prelude
     * in source order, then hoist any stray local declarations found below
     * it. The main pass skips both emitted prelude rows and declarations.
     * Semantics are unchanged: locals have no initializer to reorder, and
     * the REPL binds them all at call entry anyway. */
    if (hoist_local_decls) {
        while (prelude_end >= 0 && prelude_end < end_idx &&
               prelude_end < repl_state_document_count() &&
               repl_state_document_cmds()[prelude_end].valid &&
               (repl_state_document_cmds()[prelude_end].type == CMD_COMMENT ||
                repl_state_document_cmds()[prelude_end].type == CMD_EMPTY ||
                export_row_is_hoisted_local(prelude_end, hoist_local_decls))) {
            /* Contiguous comments go through the run writer, not one row at
             * a time: a multi-line run becomes a single C block comment,
             * and a body's leading run must not render differently from one
             * further down just because the prelude pass reached it first. */
            if (repl_state_document_cmds()[prelude_end].type == CMD_COMMENT) {
                int run_end = prelude_end + 1;
                while (run_end < end_idx &&
                       run_end < repl_state_document_count() &&
                       repl_state_document_cmds()[run_end].valid &&
                       repl_state_document_cmds()[run_end].type == CMD_COMMENT)
                    run_end++;
                export_write_comment_run_as_c(f, prelude_end, run_end);
                prelude_end = run_end;
                continue;
            }
            write_canonical_cmd_as_c(f,
                                     &repl_state_document_cmds()[prelude_end],
                                     prelude_end, 0, &tess_depth);
            prelude_end++;
        }
        for (int cmd_idx = prelude_end;
             cmd_idx >= 0 && cmd_idx < end_idx &&
             cmd_idx < repl_state_document_count(); cmd_idx++) {
            if (repl_state_document_cmds()[cmd_idx].type == CMD_FUNC_DEF) {
                cmd_idx = find_export_block_end(cmd_idx);
                continue;
            }
            if (!export_row_is_hoisted_local(cmd_idx, hoist_local_decls))
                continue;
            write_canonical_cmd_as_c(f, &repl_state_document_cmds()[cmd_idx],
                                     cmd_idx, 0, &tess_depth);
        }
    }

    for (int cmd_idx = start; cmd_idx < end_idx && cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!repl_state_document_cmds()[cmd_idx].valid) continue;
        if (cmd_idx < prelude_end) continue;
        if (export_row_is_hoisted_local(cmd_idx, hoist_local_decls)) continue;
        if (skip_func_defs &&
            (repl_state_document_cmds()[cmd_idx].type == CMD_COMMENT ||
             repl_state_document_cmds()[cmd_idx].type == CMD_EMPTY)) {
            int attached_func = comment_run_attached_func_idx(cmd_idx, end_idx);
            if (attached_func >= 0) {
                write_func_body_marker(f, attached_func);
                cmd_idx = find_export_block_end(attached_func);
                continue;
            }
        }
        switch (repl_state_document_cmds()[cmd_idx].type) {
        case CMD_FOR_BEGIN:
            write_for_begin_as_c(f, &repl_state_document_cmds()[cmd_idx],
                                 export_document_text(cmd_idx));
            for_depth++;
            break;
        case CMD_FOR_END:
            for_depth--;
            write_for_end_as_c(f, export_document_text(cmd_idx));
            break;
        case CMD_FUNC_DEF:
            if (skip_func_defs) {
                write_func_body_marker(f, cmd_idx);
                cmd_idx = find_export_block_end(cmd_idx);
            }
            break;
        case CMD_FUNC_END:
            break;
        case CMD_COMMENT: {
            int block_end = cmd_idx + 1;
            while (block_end < end_idx &&
                   block_end < repl_state_document_count()) {
                if (!repl_state_document_cmds()[block_end].valid) {
                    block_end++;
                    continue;
                }
                if (repl_state_document_cmds()[block_end].type != CMD_COMMENT)
                    break;
                block_end++;
            }
            export_write_comment_run_as_c(f, cmd_idx, block_end);
            cmd_idx = block_end - 1;
            break;
        }
        default:
            write_canonical_cmd_as_c(f, &repl_state_document_cmds()[cmd_idx],
                                     cmd_idx, for_depth, &tess_depth);
            break;
        }
    }
}

static int export_copy_first_arg(const char **p_inout,
                                 char *out, size_t out_sz) {
    const char *p = *p_inout;
    const char *start;
    const char *delim;
    int len;

    while (*p && isspace((unsigned char)*p))
        p++;
    start = p;
    delim = repl_scan_next_arg_delim(p);
    if (*delim != ',')
        return 0;
    len = (int)(delim - start);
    while (len > 0 && isspace((unsigned char)start[len - 1]))
        len--;
    if (len <= 0 || len >= (int)out_sz)
        return 0;
    memcpy(out, start, (size_t)len);
    out[len] = '\0';
    *p_inout = delim + 1;
    return 1;
}

static int export_extract_vector_payload(const char *arg,
                                         char *out, size_t out_sz) {
    const char *open;
    const char *close;
    int len;

    while (*arg && isspace((unsigned char)*arg))
        arg++;
    open = strchr(arg, '{');
    close = strrchr(arg, '}');
    if (open && close && close > open) {
        len = (int)(close - open - 1);
        if (len <= 0 || len >= (int)out_sz)
            return 0;
        memcpy(out, open + 1, (size_t)len);
        out[len] = '\0';
        return 1;
    }

    len = (int)strlen(arg);
    while (len > 0 && isspace((unsigned char)arg[len - 1]))
        len--;
    if (len <= 0 || len >= (int)out_sz)
        return 0;
    memcpy(out, arg, (size_t)len);
    out[len] = '\0';
    return 1;
}

/* Widest vector payload any command writes: glMultMatrixf's 4x4. */
#define EXPORT_VECTOR_ARGS_MAX REPL_MATRIX_CELL_COUNT

static int export_translate_vector_args(const char *payload,
                                        char out[][MAX_LINE_LEN],
                                        int expected_count) {
    char raw[EXPORT_VECTOR_ARGS_MAX][MAX_LINE_LEN];
    int count;
    int arg_idx;

    if (expected_count < 0 || expected_count > EXPORT_VECTOR_ARGS_MAX)
        return 0;
    count = split_top_level_args(payload, raw, EXPORT_VECTOR_ARGS_MAX);

    if (count != expected_count)
        return 0;
    for (arg_idx = 0; arg_idx < count; arg_idx++)
        repl_eval_expr_to_c(raw[arg_idx], out[arg_idx], sizeof(out[arg_idx]));
    return 1;
}

/* Shared front half of the write_*_as_c89 vector-command writers:
 * matches `prefix` after leading whitespace (indent width returned
 * through *out_indent for the caller's fprintf), extracts the payload
 * between the call's outer parens, peels `token_count` leading
 * comma-delimited args into tokens[], and copies the remaining vector
 * payload (brace block or bare expression list) into vector_payload.
 * Returns 1 on a structural match. */
static int export_parse_vector_call(const char *source_text,
                                    const char *prefix,
                                    char tokens[][MAX_LINE_LEN],
                                    int token_count,
                                    char *vector_payload,
                                    size_t vector_payload_sz,
                                    int *out_indent) {
    const char *p = source_text;
    const char *payload_start;
    const char *payload_end;
    char payload[MAX_LINE_LEN];
    const char *payload_p;
    int indent = 0;
    int payload_len;

    while (p[indent] && isspace((unsigned char)p[indent]))
        indent++;
    p += indent;
    if (strncmp(p, prefix, strlen(prefix)) != 0)
        return 0;
    payload_start = strchr(p, '(');
    payload_end = strrchr(p, ')');
    if (!payload_start || !payload_end || payload_end <= payload_start)
        return 0;
    payload_len = (int)(payload_end - payload_start - 1);
    if (payload_len <= 0 || payload_len >= (int)sizeof(payload))
        return 0;
    memcpy(payload, payload_start + 1, (size_t)payload_len);
    payload[payload_len] = '\0';

    payload_p = payload;
    for (int tok = 0; tok < token_count; tok++) {
        if (!export_copy_first_arg(&payload_p, tokens[tok], MAX_LINE_LEN))
            return 0;
    }
    if (!export_extract_vector_payload(payload_p, vector_payload,
                                       vector_payload_sz))
        return 0;
    *out_indent = indent;
    return 1;
}

int write_materialfv_as_c89(FILE *f, const char *source_text) {
    char tokens[2][MAX_LINE_LEN];  /* face, pname */
    char vector_payload[MAX_LINE_LEN];
    char c_args[4][MAX_LINE_LEN];
    int indent;
    int count;

    if (!export_parse_vector_call(source_text, "glMaterialfv", tokens, 2,
                                  vector_payload, sizeof(vector_payload),
                                  &indent))
        return 0;

    count = split_top_level_args(vector_payload, c_args, 4);
    if (count != 1 && count != 4)
        return 0;
    if (!export_translate_vector_args(vector_payload, c_args, count))
        return 0;

    if (count == 1) {
        fprintf(f, "%.*sglMaterialfv(%s, %s, %s(%s));\n",
                indent, source_text, tokens[0], tokens[1],
                REPL_EXPORT_GLFLOAT1_HELPER, c_args[0]);
    } else {
        fprintf(f, "%.*sglMaterialfv(%s, %s, %s(%s, %s, %s, %s));\n",
                indent, source_text, tokens[0], tokens[1],
                REPL_EXPORT_GLFLOAT4_HELPER,
                c_args[0], c_args[1], c_args[2], c_args[3]);
    }
    return 1;
}

int write_point_parameterfv_as_c89(FILE *f, const char *source_text) {
    char pname[1][MAX_LINE_LEN];
    char vector_payload[MAX_LINE_LEN];
    char c_args[4][MAX_LINE_LEN];
    int indent;

    if (!export_parse_vector_call(source_text, "glPointParameterfv", pname, 1,
                                  vector_payload, sizeof(vector_payload),
                                  &indent))
        return 0;
    if (!export_translate_vector_args(vector_payload, c_args, 3))
        return 0;

    fprintf(f, "%.*sglPointParameterfv(%s, %s(%s, %s, %s));\n",
            indent, source_text, pname[0], REPL_EXPORT_GLFLOAT3_HELPER,
            c_args[0], c_args[1], c_args[2]);
    return 1;
}

/* glClipPlane(plane, (GLdouble[]){a, b, c, d}) - compound literals are
 * C99, so the exported C89 line routes the equation through the
 * repl_gldouble4 helper instead. */
int write_clip_plane_as_c89(FILE *f, const char *source_text) {
    char plane[1][MAX_LINE_LEN];
    char vector_payload[MAX_LINE_LEN];
    char c_args[4][MAX_LINE_LEN];
    int indent;

    if (!export_parse_vector_call(source_text, "glClipPlane", plane, 1,
                                  vector_payload, sizeof(vector_payload),
                                  &indent))
        return 0;
    if (!export_translate_vector_args(vector_payload, c_args, 4))
        return 0;

    fprintf(f, "%.*sglClipPlane(%s, %s(%s, %s, %s, %s));\n",
            indent, source_text, plane[0], REPL_EXPORT_GLDOUBLE4_HELPER,
            c_args[0], c_args[1], c_args[2], c_args[3]);
    return 1;
}

/* `A[base] = {e0, ..., eN-1};` - an array is not assignable in C, so the
 * row lowers to the run of cell stores it means:
 *
 *   A[4] = {c, s, 0, 0};   ->   A[4] = c; A[5] = s; A[6] = 0.0f; A[7] = 0.0f;
 *
 * One C line per source row, which is what lets import fold the run back
 * into the block form (repl_import_fold_scratch_block). The base index is a
 * literal - compile rejects a computed one for exactly this reason - so
 * every subscript here is a constant and the base expression is never
 * evaluated more than once. */
int write_scratch_block_as_c89(FILE *f, const char *source_text) {
    char name[REPL_PREDEF_NAME_MAX];
    char index_expr[MAX_LINE_LEN];
    char rhs[MAX_LINE_LEN];
    ReplScratchBlockCell cells[REPL_SCRATCH_ARRAY_LEN];
    char out[MAX_LINE_LEN * 3];
    const char *comment;
    int base = 0;
    int count, indent, used = 0, k;

    if (!repl_extract_assignment_target_parts(source_text ? source_text : "",
                                              name, sizeof(name),
                                              index_expr, sizeof(index_expr),
                                              rhs, sizeof(rhs)))
        return 0;
    if (repl_eval_scratch_array_index(name) < 0)
        return 0;
    count = repl_split_scratch_block_rhs(rhs, cells, REPL_SCRATCH_ARRAY_LEN);
    if (count < 0)
        return 0;
    if (index_expr[0]) {
        char *end = NULL;
        long parsed = strtol(index_expr, &end, 10);
        if (!end || *end != '\0' || parsed < 0)
            return 0;
        base = (int)parsed;
    }
    if (base + count > REPL_SCRATCH_ARRAY_LEN)
        return 0;

    indent = 0;
    while (source_text[indent] == ' ' || source_text[indent] == '\t')
        indent++;

    out[0] = '\0';
    for (k = 0; k < count; k++) {
        char cell[MAX_LINE_LEN];
        char c_cell[MAX_LINE_LEN];
        int wrote;

        repl_scratch_block_cell_text(&cells[k], cell, sizeof(cell));
        repl_eval_expr_to_c(cell, c_cell, sizeof(c_cell));
        wrote = snprintf(out + used, sizeof(out) - (size_t)used, "%s%s[%d] = %s;",
                         k ? " " : "", name, base + k, c_cell);
        if (wrote < 0 || wrote >= (int)sizeof(out) - used)
            return 0;
        used += wrote;
    }

    comment = repl_line_trailing_comment(source_text);
    fprintf(f, "%.*s%s%s%s\n", indent, source_text, out,
            comment ? "  " : "", comment ? comment : "");
    return 1;
}

/* glMultMatrixf((GLfloat[]){m0, ..., m15}) - compound literals are C99,
 * so the exported C89 line routes the 16 cells through the repl_glfloat16
 * helper instead. Only the literal form comes here; `glMultMatrixf(A)` is
 * already valid C against the exported scratch global and takes the
 * default source-text path. */
int write_mult_matrixf_as_c89(FILE *f, const char *source_text) {
    char tokens[1][MAX_LINE_LEN];   /* unused: the call has no leading args */
    char vector_payload[MAX_LINE_LEN];
    char c_args[REPL_MATRIX_CELL_COUNT][MAX_LINE_LEN];
    int indent;

    if (!export_parse_vector_call(source_text, "glMultMatrixf", tokens, 0,
                                  vector_payload, sizeof(vector_payload),
                                  &indent))
        return 0;
    if (!export_translate_vector_args(vector_payload, c_args,
                                      REPL_MATRIX_CELL_COUNT))
        return 0;

    fprintf(f, "%.*sglMultMatrixf(%s(%s, %s, %s, %s, %s, %s, %s, %s, "
               "%s, %s, %s, %s, %s, %s, %s, %s));\n",
            indent, source_text, REPL_EXPORT_GLFLOAT16_HELPER,
            c_args[0], c_args[1], c_args[2], c_args[3],
            c_args[4], c_args[5], c_args[6], c_args[7],
            c_args[8], c_args[9], c_args[10], c_args[11],
            c_args[12], c_args[13], c_args[14], c_args[15]);
    return 1;
}

/* glFogfv(GL_FOG_COLOR, (GLfloat[]){r, g, b, a}) - compound literals
 * are C99, so the exported C89 line routes the color through the
 * repl_glfloat4 helper instead. */
int write_fog_fv_as_c89(FILE *f, const char *source_text) {
    char pname[1][MAX_LINE_LEN];
    char vector_payload[MAX_LINE_LEN];
    char c_args[4][MAX_LINE_LEN];
    int indent;

    if (!export_parse_vector_call(source_text, "glFogfv", pname, 1,
                                  vector_payload, sizeof(vector_payload),
                                  &indent))
        return 0;
    if (!export_translate_vector_args(vector_payload, c_args, 4))
        return 0;

    fprintf(f, "%.*sglFogfv(%s, %s(%s, %s, %s, %s));\n",
            indent, source_text, pname[0], REPL_EXPORT_GLFLOAT4_HELPER,
            c_args[0], c_args[1], c_args[2], c_args[3]);
    return 1;
}
