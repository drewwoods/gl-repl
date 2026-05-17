/*
 * src/repl/parser.c - REPL text command parser.
 *
 * Converts one source line into a GLCmd and canonical source text. The parse
 * context carries the source-line index used for scope-sensitive indentation
 * plus the loop/function locals visible at that line.
 */
#include "repl/parser.h"

#include "repl/command_spec.h"
#include "repl/core_internal.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"

#include "config.h" /* CP_CLEAR_MAX_V */

#include <stdarg.h>
#include <stdlib.h>  /* strtod (strict bool-slot numeric literal) */

/* Phase H.5 commit 40 introduced the err_buf seam. Phase I commit
 * 42a migrated every caller to provide a buffer. Commit 42b (this
 * one) drops the legacy set_status fallback: the parser writes to
 * ctx->err_buf when available, and otherwise no-ops. Diagnostics
 * never leave the parser as side effects on REPL state.
 *
 * Commit 42c adds a hard guard so set_status calls cannot return
 * to src/repl/parser.c. */
static void parser_emit_error_v(const ReplParseContext *ctx,
                                const char *fmt, va_list ap) {
    if (!ctx || !ctx->err_buf || ctx->err_sz <= 0) {
        /* No buffer: the caller deliberately discarded diagnostics
         * (or hasn't been migrated). Either way, the parser does
         * not touch global status. */
        return;
    }
    vsnprintf(ctx->err_buf, (size_t)ctx->err_sz, fmt, ap);
}

static void parser_emit_error(const ReplParseContext *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    parser_emit_error_v(ctx, fmt, ap);
    va_end(ap);
}

static void parser_emit_error_static(const ReplParseContext *ctx, const char *msg) {
    parser_emit_error(ctx, "%s", msg ? msg : "");
}

static void set_incomplete_missing_paren_status(const ReplParseContext *ctx,
                                                const char *func) {
    if (func && func[0])
        parser_emit_error(ctx,
                          "Incomplete command: missing ')' in %s(...)", func);
    else
        parser_emit_error_static(ctx, "Incomplete command: missing ')'");
}

static void set_incomplete_arg_count_status(const ReplParseContext *ctx,
                                            const char *func,
                                            int expected, int got) {
    parser_emit_error(ctx,
                      "Incomplete command: %s expects %d argument%s (got %d)",
                      func, expected, expected == 1 ? "" : "s", got);
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
        "glLoadIdentity",
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
 * resolve_enum_arg_slot - resolve one positional enum argument.
 *
 * `raw` is the trimmed source token for this slot. Resolution order:
 *
 *   1. Exact enum-token lookup against as->enums (non-null for every
 *      kind). The matched token name is what gets emitted, so the
 *      user's GL_* spelling is always preserved and autocomplete stays
 *      table-driven. Token lookup always wins, which also makes the
 *      numeric reverse-map below header-independent in practice.
 *   2. On a token miss, behavior is keyed on the slot kind:
 *      - ENUM_ONLY: reject (no numeric, no expression). Byte-for-byte
 *        the historic behavior of every strict enum slot.
 *      - ENUM_OR_CONST_VALUE: parse a strict numeric literal (no
 *        runtime vars, no trailing junk) and reverse-map the value
 *        back into this slot's table; emit the canonical token name.
 *        The bool-slot policy: glDepthMask and the four glColorMask
 *        channels, so glDepthMask(1) / glColorMask(1, 0, 1, 0)
 *        canonicalize to GL_TRUE/GL_FALSE.
 *      - ENUM_OR_EXPR: evaluate a full expression (vars permitted),
 *        store the folded value, emit the typed source token verbatim,
 *        and set *any_vars when it references visible vars.
 *
 * Returns 1 with *out_val / emit[] filled, or 0 after emitting the
 * slot's diagnostic.
 */
static int resolve_enum_arg_slot(const char *raw,
                                 const ReplEnumArgSpec *as,
                                 ExprVar *vars, int num_vars,
                                 float *out_val,
                                 char *emit, int emit_sz,
                                 int *any_vars,
                                 const ReplParseContext *ctx) {
    for (int i = 0; as->enums && as->enums[i].name; i++) {
        if (strcmp(raw, as->enums[i].name) == 0) {
            *out_val = (float)as->enums[i].value;
            snprintf(emit, (size_t)emit_sz, "%s", as->enums[i].name);
            return 1;
        }
    }

    switch (as->kind) {
    case REPL_ENUM_SLOT_ENUM_ONLY:
        parser_emit_error_static(ctx, as->usage);
        return 0;

    case REPL_ENUM_SLOT_ENUM_OR_CONST_VALUE: {
        char verr[128];
        if (!repl_eval_validate_expression_idents(raw, vars, num_vars,
                                                  verr, sizeof(verr)) ||
            input_has_any_visible_vars(raw, vars, num_vars)) {
            parser_emit_error_static(ctx, as->usage);
            return 0;
        }
        /* Boolean-mask slot: accept only a well-formed numeric literal,
         * then reverse-map. repl_eval_parse_exprs / repl_eval_expr do
         * NOT require full input consumption (eval.c) — they evaluate
         * "1+" as 1 (trailing '+' with an empty 0 operand) and "1abc"
         * as 1, which would silently canonicalize garbage to GL_TRUE.
         * strtod with an end-of-token check rejects "1+", "1abc",
         * "1 2", etc. while still accepting 0/1/1.0. */
        float fv;
        {
            const char *q = raw;
            while (*q && isspace((unsigned char)*q)) q++;
            char *endp = NULL;
            double d = strtod(q, &endp);
            if (endp == q) {                 /* not a number at all */
                parser_emit_error_static(ctx, as->usage);
                return 0;
            }
            while (*endp && isspace((unsigned char)*endp)) endp++;
            if (*endp != '\0') {             /* trailing junk: "1+", "1abc" */
                parser_emit_error_static(ctx, as->usage);
                return 0;
            }
            fv = (float)d;
        }
        for (int i = 0; as->enums && as->enums[i].name; i++) {
            if ((float)as->enums[i].value == fv) {
                *out_val = (float)as->enums[i].value;
                snprintf(emit, (size_t)emit_sz, "%s", as->enums[i].name);
                return 1;
            }
        }
        parser_emit_error_static(ctx, as->usage);
        return 0;
    }

    case REPL_ENUM_SLOT_ENUM_OR_EXPR: {
        char verr[128];
        if (!repl_eval_validate_expression_idents(raw, vars, num_vars,
                                                  verr, sizeof(verr))) {
            parser_emit_error_static(ctx, verr);
            return 0;
        }
        float fv;
        if (repl_eval_parse_exprs(raw, &fv, 1, vars, num_vars) != 1) {
            parser_emit_error_static(ctx, as->usage);
            return 0;
        }
        *out_val = fv;
        snprintf(emit, (size_t)emit_sz, "%s", raw);
        if (input_has_any_visible_vars(raw, vars, num_vars))
            *any_vars = 1;
        return 1;
    }
    }

    parser_emit_error_static(ctx, as->usage);
    return 0;
}

/*
 * parse_command - Convert a single REPL text line into a GLCmd.
 *
 * text_out receives the canonical source form (indented, with trailing ';')
 * that was previously stored in GLCmd.source. text_sz is the capacity of
 * text_out. Callers that don't need the text may pass NULL/0.
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
 * Returns 1 on success (cmd populated), 0 on parse failure.
 * Diagnostics flow through ReplParseContext.err_buf when the caller
 * provides one. The legacy no-ctx wrappers surface to status; the
 * parser core itself never calls set_status.
 */
static int parse_command(const char *line, GLCmd *cmd,
                         char *text_out, int text_sz,
                         const ReplParseContext *ctx) {
    int source_line_idx = ctx ? ctx->source_line_idx : repl_state_edit_line();
    ExprVar *vars = ctx ? ctx->vars : NULL;
    int num_vars = ctx ? ctx->num_vars : 0;
    char buf[MAX_LINE_LEN];

    /* Null-safe text helpers: write to text_out when provided */
/* Every call passes at least one variadic arg after fmt, so plain
 * __VA_ARGS__ suffices — no GNU `, ##__VA_ARGS__` comma-elision (which
 * -std=c99 -pedantic-errors rejects). */
#define WRITE_TEXT(fmt, ...) do { \
    if (text_out && text_sz > 0) \
        snprintf(text_out, (size_t)text_sz, fmt, __VA_ARGS__); \
} while (0)
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;

    int len = (int)strlen(p);
    while (len > 0 && (p[len - 1] == ';' || isspace((unsigned char)p[len - 1])))
        p[--len] = '\0';

    cmd->valid = 0;
    cmd->num_args = 0;
    if (text_out && text_sz > 0) text_out[0] = '\0';

    if (len == 0) {
        cmd->type = CMD_EMPTY;
        cmd->valid = 1;
        cmd->is_auto = 0;
        cmd->num_args = 0;
        return 1;
    }

    /* Comment: line starts with // */
    if (p[0] == '/' && p[1] == '/') {
        cmd->type = CMD_COMMENT;
        cmd->valid = 1;
        cmd->is_auto = 0;
        cmd->num_args = 0;
        char indent[32];
        repl_source_scope_cmd_indent(source_line_idx, indent, sizeof(indent));
        WRITE_TEXT("%s%s", indent, p);
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
            set_incomplete_missing_paren_status(ctx, func);
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

    /* Table-driven parsing for enum commands (generalized N-arg).
     *
     * Every enum-backed command declares def->num_args positional slots
     * in def->args[]. Split the call's args into exactly that many
     * top-level fields (paren-aware), resolve each through the per-slot
     * resolver keyed on its ReplEnumSlotKind, and store every resolved
     * value uniformly in cmd->args[slot] with cmd->num_args set. There
     * is no cmd->mode for table-driven enum commands — the executor and
     * every other reader read args[]. Storing a GLenum in float args[]
     * is exact: all GL enums in use are < 2^24, so (GLenum)cmd->args[i]
     * round-trips losslessly. */
    for (const ReplEnumCommandSpec *def = repl_enum_command_specs(); def->name; def++) {
        if (strcmp(func, def->name) != 0)
            continue;
        if (def->num_args < 1)
            break; /* custom/metadata row (e.g. glMaterialf, num_args -2):
                    * not N-arg enum-parsed; a dedicated branch below
                    * handles it. */

        int n = def->num_args;
        if (n > MAX_ENUM_ARGS) n = MAX_ENUM_ARGS;

        char slot_raw[MAX_ENUM_ARGS][64];
        int  split_ok = 1;
        const char *s = args;
        for (int slot = 0; slot < n && split_ok; slot++) {
            while (*s && isspace((unsigned char)*s)) s++;
            const char *delim = repl_scan_next_arg_delim(s);
            int seg_len = (int)(delim - s);
            while (seg_len > 0 && isspace((unsigned char)s[seg_len - 1]))
                seg_len--;
            if (seg_len <= 0 || seg_len >= (int)sizeof(slot_raw[slot])) {
                split_ok = 0;
                break;
            }
            memcpy(slot_raw[slot], s, (size_t)seg_len);
            slot_raw[slot][seg_len] = '\0';
            if (slot < n - 1) {
                if (*delim != ',') { split_ok = 0; break; }
                s = delim + 1;
            } else {
                const char *t = delim;
                while (*t && isspace((unsigned char)*t)) t++;
                if (*t != '\0') split_ok = 0; /* extra trailing args */
            }
        }
        if (!split_ok) {
            parser_emit_error_static(ctx, def->args[0].usage
                                     ? def->args[0].usage : "Invalid arguments");
            return 0;
        }

        float slot_val[MAX_ENUM_ARGS];
        char  slot_emit[MAX_ENUM_ARGS][64];
        int   any_vars = 0;
        for (int slot = 0; slot < n; slot++) {
            if (!resolve_enum_arg_slot(slot_raw[slot], &def->args[slot],
                                       vars, num_vars, &slot_val[slot],
                                       slot_emit[slot],
                                       (int)sizeof(slot_emit[slot]),
                                       &any_vars, ctx))
                return 0;
        }

        cmd->type = def->type;
        cmd->valid = 1;
        cmd->num_args = n;
        for (int slot = 0; slot < n; slot++)
            cmd->args[slot] = slot_val[slot];
        if (any_vars)
            cmd->has_vars = 1;

        char ind[32];
        if (def->indent_type == 1) {
            /* glBegin-style indent: 2 + 2*tess + 2*block (begin depth excluded) */
            int td = repl_source_scope_tess_scope_depth_at(source_line_idx);
            int kd = repl_source_scope_block_depth_at(source_line_idx);
            int spaces = 2 + 2 * td + 2 * kd;
            if (spaces > (int)sizeof(ind) - 1) spaces = (int)sizeof(ind) - 1;
            memset(ind, ' ', (size_t)spaces);
            ind[spaces] = '\0';
        } else {
            repl_source_scope_cmd_indent(source_line_idx, ind, sizeof(ind));
        }
        if (text_out && text_sz > 0) {
            if (n == 1) {
                WRITE_TEXT(def->fmt, ind, slot_emit[0]);
            } else if (n == 2) {
                WRITE_TEXT(def->fmt, ind, slot_emit[0], slot_emit[1]);
            } else if (n == 4 && def->fmt) {
                WRITE_TEXT(def->fmt, ind, slot_emit[0], slot_emit[1],
                           slot_emit[2], slot_emit[3]);
            } else {
                /* No current command declares >2 enum slots; a generic
                 * "name(joined token names);" join keeps a future
                 * N-slot command honest without another fmt. */
                int off = snprintf(text_out, (size_t)text_sz, "%s%s(",
                                   ind, def->name);
                for (int slot = 0; slot < n && off < (int)text_sz - 4; slot++)
                    off += snprintf(text_out + off, (size_t)(text_sz - off),
                                    "%s%s", slot ? ", " : "", slot_emit[slot]);
                snprintf(text_out + off, (size_t)(text_sz - off), ");");
            }
        }
        return 1;
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
            WRITE_TEXT("%sglEnd();", end_ind);
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
                    parser_emit_error_static(ctx, verr); return 0;
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
                if (text_out && text_sz > 0) {
                    snprintf(text_out, (size_t)text_sz, "%s", ind);
                    size_t current_len = strlen(text_out);
                    switch (def->num_args) {
                    case 1:
                        snprintf(text_out + current_len, (size_t)(text_sz - (int)current_len),
                                 def->fmt, cmd->args[0]);
                        break;
                    case 2:
                        snprintf(text_out + current_len, (size_t)(text_sz - (int)current_len),
                                 def->fmt, cmd->args[0], cmd->args[1]);
                        break;
                    case 3:
                        snprintf(text_out + current_len, (size_t)(text_sz - (int)current_len),
                                 def->fmt, cmd->args[0], cmd->args[1], cmd->args[2]);
                        break;
                    case 4:
                        snprintf(text_out + current_len, (size_t)(text_sz - (int)current_len),
                                 def->fmt, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
                        break;
                    case 5:
                        snprintf(text_out + current_len, (size_t)(text_sz - (int)current_len),
                                 def->fmt, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3], cmd->args[4]);
                        break;
                    case 6:
                        snprintf(text_out + current_len, (size_t)(text_sz - (int)current_len),
                                 def->fmt, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3], cmd->args[4], cmd->args[5]);
                        break;
                    }
                }
                /* glClearColor: clamp each RGB channel and rebuild text */
                if (def->type == CMD_CLEAR_COLOR) {
                    int clamped = 0;
                    for (int ci = 0; ci < 3; ci++) {
                        if (cmd->args[ci] > CP_CLEAR_MAX_V) {
                            cmd->args[ci] = CP_CLEAR_MAX_V;
                            clamped = 1;
                        }
                    }
                    if (clamped) {
                        if (text_out && text_sz > 0) {
                            snprintf(text_out, (size_t)text_sz, "%s", ind);
                            size_t cl = strlen(text_out);
                            snprintf(text_out + cl, (size_t)(text_sz - (int)cl),
                                     def->fmt, cmd->args[0], cmd->args[1],
                                     cmd->args[2], cmd->args[3]);
                        }
                        if (!cmd->has_vars)
                            parser_emit_error_static(ctx, "glClearColor: channels clamped to 0.15 max");
                    }
                }
                return 1;
            }
            cmd->num_args = repl_eval_parse_exprs(args, cmd->args, def->num_args, vars, num_vars);
            if (cmd->num_args < def->num_args)
                set_incomplete_arg_count_status(ctx, def->name, def->num_args, cmd->num_args);
            else
                parser_emit_error_static(ctx, def->usage);
            return 0;
        }
    }

    /* label("fmt", a, b, c, d)
     *
     * printf-style text emission at the current raster position
     * (set by a preceding glRasterPos3f). Custom branch — not
     * table-driven — because one arg is a string literal that the
     * std-table parsers don't tokenize, and the substitution-arg
     * count is variable.
     *
     * Forbidden inside the format string: '/' followed by '/',
     * '(', ')', ',', and any backslash. These constraints keep the
     * surrounding (string-unaware) parser scaffolding honest.
     * See repl_label_split_args() for the split helper. */
    if (strcmp(func, "label") == 0) {
        char fmt_str[GLUT_BITMAP_FMT_MAX] = "";
        char post_args[MAX_LINE_LEN] = "";
        char split_err[128] = "";

        if (!repl_label_split_args(args,
                                   fmt_str, (int)sizeof(fmt_str),
                                   post_args, (int)sizeof(post_args),
                                   split_err, (int)sizeof(split_err))) {
            parser_emit_error_static(ctx, split_err);
            return 0;
        }

        /* Substitution args: 0..GLUT_BITMAP_MAX_SUB_ARGS floats.
         * Parse into an oversized buffer so we can distinguish "user
         * supplied 4 args" from "user supplied 5+ args" — passing
         * GLUT_BITMAP_MAX_SUB_ARGS to repl_eval_parse_exprs would
         * silently truncate. */
        float subs[GLUT_BITMAP_MAX_SUB_ARGS] = {0};
        int sub_count = 0;
        if (post_args[0]) {
            char verr[128];
            if (!repl_eval_validate_expression_idents(post_args, vars, num_vars,
                                                       verr, sizeof(verr))) {
                parser_emit_error_static(ctx, verr); return 0;
            }
            float subs_full[GLUT_BITMAP_MAX_SUB_ARGS + 4];
            int parsed = repl_eval_parse_exprs(
                post_args, subs_full,
                (int)(sizeof(subs_full) / sizeof(subs_full[0])),
                vars, num_vars);
            if (parsed > GLUT_BITMAP_MAX_SUB_ARGS) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "label: too many args (max %d)",
                         GLUT_BITMAP_MAX_SUB_ARGS);
                parser_emit_error_static(ctx, buf);
                return 0;
            }
            if (parsed < 0) parsed = 0;
            sub_count = parsed;
            for (int i = 0; i < sub_count; i++) subs[i] = subs_full[i];
        }

        /* %f count must match supplied sub args. %% is a literal '%'.
         * Anything else after '%' is rejected. */
        int pct_count = 0;
        for (int i = 0; fmt_str[i]; i++) {
            if (fmt_str[i] != '%') continue;
            char nx = fmt_str[i + 1];
            if (nx == 'f') { pct_count++; i++; }
            else if (nx == '%') { i++; }
            else {
                parser_emit_error_static(ctx,
                    "label: only %f and %% allowed in format");
                return 0;
            }
        }
        if (pct_count != sub_count) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "label: format expects %d arg%s, got %d",
                     pct_count, pct_count == 1 ? "" : "s", sub_count);
            parser_emit_error_static(ctx, buf);
            return 0;
        }

        cmd->type = CMD_LABEL;
        cmd->valid = 1;
        for (int i = 0; i < sub_count; i++) cmd->args[i] = subs[i];
        cmd->num_args = sub_count;
        cmd->has_vars = input_has_any_visible_vars(post_args, vars, num_vars);
        repl_copy_string_fits(cmd->text, sizeof(cmd->text), fmt_str);

        if (text_out && text_sz > 0) {
            int off = snprintf(text_out, (size_t)text_sz,
                               "%slabel(\"%s\"", indent, fmt_str);
            for (int i = 0; i < sub_count && off < (int)text_sz - 6; i++) {
                off += snprintf(text_out + off, (size_t)(text_sz - off),
                                ", %g", subs[i]);
            }
            snprintf(text_out + off, (size_t)(text_sz - off), ");");
        }
        return 1;
    }

    /* glMaterialf(face, pname, param) */
    if (strcmp(func, "glMaterialf") == 0) {
        char a1[64] = "", a2[64] = "", a3[MAX_LINE_LEN] = "";
        char *comma1 = strchr(args, ',');
        char *comma2 = comma1 ? strchr(comma1 + 1, ',') : NULL;

        if (!comma1 || !comma2) { parser_emit_error_static(ctx, "Usage: glMaterialf(face, pname, params...)"); return 0; }

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

        const ReplEnumEntry *face_types = repl_face_type_entries();
        const ReplEnumEntry *material_params = repl_material_param_entries();
        for (int i = 0; face_types[i].name; i++) {
            if (strcmp(p1, face_types[i].name) == 0) { face = face_types[i].value; found1 = 1; break; }
        }
        for (int i = 0; material_params[i].name; i++) {
            if (strcmp(p2, material_params[i].name) == 0) { pname = material_params[i].value; found2 = 1; break; }
        }

        if (!found1) { parser_emit_error_static(ctx, "face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK"); return 0; }
        if (!found2) { parser_emit_error_static(ctx, "pname: GL_DIFFUSE, GL_AMBIENT, GL_SPECULAR, GL_SHININESS..."); return 0; }

        {
            char verr[128];
            if (!repl_eval_validate_expression_idents(a3, vars, num_vars, verr, sizeof(verr))) {
                parser_emit_error_static(ctx, verr); return 0;
            }
        }
        float parsed_args[8];
        int num_parsed = repl_eval_parse_exprs(a3, parsed_args, 8, vars, num_vars);
        if (num_parsed != 1 && num_parsed != 4) {
            parser_emit_error_static(ctx, "Expected 1 or 4 float values");
            return 0;
        }

        /* Uniform args[] layout (no GLCmd.mode): args[0]=face,
         * args[1]=pname, args[2..]=value(s). GLenums fit float32
         * exactly (all < 2^24), so the casts round-trip losslessly. */
        cmd->type = CMD_MATERIALF;
        cmd->valid = 1;
        cmd->args[0] = (float)face;
        cmd->args[1] = (float)pname;
        for (int k = 0; k < num_parsed; k++) cmd->args[k + 2] = parsed_args[k];
        cmd->num_args = num_parsed + 2;
        cmd->has_vars = input_has_any_visible_vars(a3, vars, num_vars);

        if (num_parsed == 1) {
            WRITE_TEXT("%sglMaterialf(%s, %s, %g);", indent, p1, p2, parsed_args[0]);
        } else {
            WRITE_TEXT("%sglMaterialfv(%s, %s, (GLfloat[]){%g, %g, %g, %g});",
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
            parser_emit_error_static(ctx, "Usage: glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)");
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
        const ReplEnumEntry *point_param_pnames = repl_point_param_pname_entries();
        for (int i = 0; point_param_pnames[i].name; i++) {
            if (strcmp(p1, point_param_pnames[i].name) == 0) {
                pname = point_param_pnames[i].value;
                found = 1;
                break;
            }
        }
        if (!found) {
            parser_emit_error_static(ctx, "pname: GL_POINT_DISTANCE_ATTENUATION");
            return 0;
        }

        {
            char verr[128];
            if (!repl_eval_validate_expression_idents(rest, vars, num_vars, verr, sizeof(verr))) {
                parser_emit_error_static(ctx, verr); return 0;
            }
        }
        float parsed_args[4];
        int num_parsed = repl_eval_parse_exprs(rest, parsed_args, 4, vars, num_vars);
        if (num_parsed != 3) {
            parser_emit_error_static(ctx, "Expected 3 floats: const, linear, quadratic attenuation coefficients");
            return 0;
        }

        /* Uniform args[] layout (no GLCmd.mode): args[0]=pname,
         * args[1..3]=const/linear/quadratic attenuation coefficients. */
        cmd->type = CMD_POINT_PARAMETER_FV;
        cmd->valid = 1;
        cmd->args[0] = (float)pname;
        cmd->args[1] = parsed_args[0];
        cmd->args[2] = parsed_args[1];
        cmd->args[3] = parsed_args[2];
        cmd->num_args = 4;
        cmd->has_vars = (num_vars > 0);

        {
            char ind[32]; repl_source_scope_cmd_indent(source_line_idx, ind, sizeof(ind));
            WRITE_TEXT("%sglPointParameterfv(%s, (GLfloat[]){%g, %g, %g});",
                       ind, p1, parsed_args[0], parsed_args[1], parsed_args[2]);
        }
        return 1;
    }

    /* glPushMatrix() */
    if (strcmp(func, "glPushMatrix") == 0) {
        cmd->type = CMD_PUSH_MATRIX;
        cmd->valid = 1;
        WRITE_TEXT("%sglPushMatrix();", indent);
        return 1;
    }

    /* glPopMatrix() */
    if (strcmp(func, "glPopMatrix") == 0) {
        cmd->type = CMD_POP_MATRIX;
        cmd->valid = 1;
        WRITE_TEXT("%sglPopMatrix();", indent);
        return 1;
    }

    /* glLoadIdentity() */
    if (strcmp(func, "glLoadIdentity") == 0) {
        cmd->type = CMD_LOAD_IDENTITY;
        cmd->valid = 1;
        WRITE_TEXT("%sglLoadIdentity();", indent);
        return 1;
    }

    /* funcN([expr, ...]) or aliased call - function call.
     *
     * Recognises both the bare slot form `funcN` (N=0..9) and any user
     * alias registered through repl_func_alias_set. The slot index is
     * the load-bearing identity stored on the resulting CMD_CALL; the
     * alias is purely a parser/display layer. */
    int fn = -1;
    int matched_alias = 0;
    if (strncmp(func, "func", 4) == 0 && func[4] >= '0' && func[4] <= '9' &&
        func[5] == '\0') {
        fn = func[4] - '0';
    } else {
        int alias_slot = repl_func_alias_lookup_slot(func);
        if (alias_slot >= 0) {
            fn = alias_slot;
            matched_alias = 1;
        }
    }
    if (fn >= 0 && open_p && close_p) {
        float dummy_vals[MAX_EXPR_VARS];
        int arg_count = 0;
        if (args[0] != '\0') {
            char verr[128];
            if (!repl_eval_validate_expression_idents(args, vars, num_vars, verr, sizeof(verr))) {
                parser_emit_error_static(ctx, verr); return 0;
            }
        }
        if (!parse_expr_list_exact(args, dummy_vals, MAX_EXPR_VARS,
                                   vars, num_vars, &arg_count)) {
            parser_emit_error_static(ctx, "Invalid function call arguments");
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
                const char *alias = repl_func_alias_get(fn);
                if (alias)
                    snprintf(buf, sizeof(buf),
                             "undefined function '%s' - define it first", alias);
                else
                    snprintf(buf, sizeof(buf),
                             "undefined function 'func%d' - define it first", fn);
                parser_emit_error_static(ctx, buf);
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
        const char *alias = repl_func_alias_get(fn);
        char fn_token[REPL_FUNC_NAME_MAX + 8];
        if (alias)
            snprintf(fn_token, sizeof(fn_token), "%s", alias);
        else
            snprintf(fn_token, sizeof(fn_token), "func%d", fn);
        (void)matched_alias;
        if (text_out && text_sz > 0) {
            if (arg_count > 0) {
                if (!repl_format_fits(text_out, (size_t)text_sz,
                                      "%s%s(%s);", ind_str, fn_token, raw_args)) {
                    parser_emit_error_static(ctx, "Command too long");
                    return 0;
                }
            } else if (!repl_format_fits(text_out, (size_t)text_sz,
                                         "%s%s();", ind_str, fn_token)) {
                parser_emit_error_static(ctx, "Command too long");
                return 0;
            }
        }
        return 1;
    }

    /* gluBegin(GLU_POLYGON) - start a tessellated polygon */
    if (strcmp(func, "gluBegin") == 0) {
        char *a = args; while (*a && isspace((unsigned char)*a)) a++;
        if (strncmp(a, "GLU_POLYGON", 11) == 0) {
            cmd->type = CMD_TESS_BEGIN_POLYGON;
            cmd->valid = 1;
            WRITE_TEXT("%sgluBegin(GLU_POLYGON);", tess_indent);
            return 1;
        }
        if (strncmp(a, "GLU_CONTOUR", 11) == 0) {
            cmd->type = CMD_TESS_BEGIN_CONTOUR;
            cmd->valid = 1;
            WRITE_TEXT("%sgluBegin(GLU_CONTOUR);", tess_indent);
            return 1;
        }
        parser_emit_error_static(ctx, "Usage: gluBegin(GLU_POLYGON) or gluBegin(GLU_CONTOUR)");
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
            WRITE_TEXT("%sgluEnd();", close_ind);
        }
        return 1;
    }

    /* gluColor(r, g, b[, a]) - set per-vertex color for tessellator */
    if (strcmp(func, "gluColor") == 0) {
        {
            char verr[128];
            if (!repl_eval_validate_expression_idents(args, vars, num_vars, verr, sizeof(verr))) {
                parser_emit_error_static(ctx, verr); return 0;
            }
        }
        cmd->num_args = repl_eval_parse_exprs(args, cmd->args, 4, vars, num_vars);
        if (cmd->num_args >= 3) {
            if (cmd->num_args < 4) cmd->args[3] = 1.0f;
            cmd->num_args = 4;
            cmd->type = CMD_TESS_COLOR;
            cmd->valid = 1;
            cmd->has_vars = input_has_any_visible_vars(args, vars, num_vars);
            WRITE_TEXT("%sgluColor(%g, %g, %g, %g);",
                       tess_indent, cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
            return 1;
        }
        parser_emit_error_static(ctx, "Usage: gluColor(r, g, b) or gluColor(r, g, b, a)");
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
            {
                int fdepth = repl_source_scope_block_depth_at(source_line_idx);
                int bb_v = repl_source_scope_in_begin_block_at(source_line_idx);
                int ind_v = (bb_v ? 4 : 2) + fdepth * 2;
                char ind_str[32];
                if (ind_v > (int)sizeof(ind_str) - 1) ind_v = (int)sizeof(ind_str) - 1;
                memset(ind_str, ' ', ind_v); ind_str[ind_v] = '\0';
                WRITE_TEXT("%sgoto %s;", ind_str, clean_lname);
            }
            return 1;
        }
    }

    /* :label or label: - define a label */
    if ((p[0] == ':' && p[1] && !isspace((unsigned char)p[1])) ||
        (len > 1 && p[len - 1] == ':' && !isspace((unsigned char)p[0]))) {
        cmd->type = CMD_GOTO_LABEL;
        cmd->valid = 1;
        /* labels go at column 0 in C */
        if (p[0] == ':') {
            WRITE_TEXT("%s:", p + 1);
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
            WRITE_TEXT("%s:", label);
        }
        return 1;
    }

unknown_command:
    /* Recognise "I typed a math expression as a top-level command"
     * before the generic "unknown cmd" — otherwise `rand();` or
     * `sin(t);` get the same diagnostic as a misspelled GL call,
     * which is actively misleading. */
    if (func[0] && repl_eval_is_reserved_ident(func)) {
        if (open_p) {
            parser_emit_error(ctx,
                "'%s(...)' is an expression, not a command — assign it "
                "(e.g. 'x = %s(...);') or use it inside another expression",
                func, func);
        } else {
            parser_emit_error(ctx,
                "'%s' is a reserved name (constant or scratch array), "
                "not a command — use it inside an expression",
                func);
        }
        return 0;
    }
    parser_emit_error_static(ctx, "Unknown cmd. Try glVertex3f, glBegin, glEnable, glShadeModel, ...");
    return 0;

#undef WRITE_TEXT
}

/* User-facing name for commands the begin-scope rejection might surface.
 * The std/enum spec tables already carry these names but they're indexed
 * by string-search rather than CmdType; this small switch is the smallest
 * thing that makes the error message read naturally. */
static const char *cmd_display_name_for_begin_error(CmdType type) {
    switch (type) {
        case CMD_BEGIN:               return "glBegin";
        case CMD_POINT_SIZE:          return "glPointSize";
        case CMD_LINE_WIDTH:          return "glLineWidth";
        case CMD_POINT_PARAMETER_FV:  return "glPointParameterfv";
        case CMD_BLEND_FUNC:          return "glBlendFunc";
        case CMD_CLEAR_COLOR:         return "glClearColor";
        case CMD_DEPTH_MASK:          return "glDepthMask";
        case CMD_COLOR_MASK:          return "glColorMask";
        case CMD_GLUT_TORUS:          return "glutSolidTorus";
        case CMD_GLUT_CUBE:           return "glutSolidCube";
        case CMD_GLUT_SPHERE:         return "glutSolidSphere";
        case CMD_GLUT_TEAPOT:         return "glutSolidTeapot";
        case CMD_GLUT_CONE:           return "glutSolidCone";
        case CMD_RASTER_POS3F:        return "glRasterPos3f";
        case CMD_LABEL:               return "label";
        case CMD_TESS_BEGIN_POLYGON:  return "gluTessBeginPolygon";
        default:                      return repl_cmd_type_name(type);
    }
}

int repl_parser_parse_command_ctx(const char *line, ReplParsedLine *out,
                           const ReplParseContext *ctx) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!parse_command(line, &out->cmd, out->text, sizeof(out->text), ctx))
        return 0;

    /* Reject commands that real GL rejects with GL_INVALID_OPERATION inside
     * glBegin/glEnd. The executor used to defensively glEnd() the active
     * begin block before running them, which broke loop-body usage by
     * tearing down the block for subsequent glVertex calls. Now the parser
     * is the gate, so the executor can rely on the invariant. */
    if (ctx && !repl_cmd_type_valid_in_begin(out->cmd.type) &&
        repl_source_scope_in_begin_block_at(ctx->source_line_idx)) {
        parser_emit_error(ctx, "%s not valid inside glBegin/glEnd",
                          cmd_display_name_for_begin_error(out->cmd.type));
        return 0;
    }
    return 1;
}

int repl_label_split_args(const char *args,
                          char *fmt, int fmt_sz,
                          char *post, int post_sz,
                          char *err, int err_sz) {
    if (!args || !fmt || !post || fmt_sz <= 0 || post_sz <= 0) {
        if (err && err_sz > 0)
            snprintf(err, (size_t)err_sz, "internal: bad split-args buffers");
        return 0;
    }
    fmt[0] = post[0] = '\0';

    /* The format string must be the first arg. Skip leading
     * whitespace and require '"' as the first non-space char. */
    const char *p = args;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') {
        snprintf(err, (size_t)err_sz,
                 "Usage: label(\"fmt\", arg, ...)");
        return 0;
    }
    const char *quote_open = p;

    /* Closing quote — string contents allow no escapes by design,
     * so the next bare '"' ends the literal. */
    const char *quote_close = NULL;
    for (const char *q = quote_open + 1; *q; q++) {
        if (*q == '"') { quote_close = q; break; }
    }
    if (!quote_close) {
        snprintf(err, (size_t)err_sz,
                 "label: missing closing '\"'");
        return 0;
    }

    /* Validate format-string body. Forbidden chars protect the rest
     * of the codebase from having to be string-aware. */
    for (const char *q = quote_open + 1; q < quote_close; q++) {
        if (*q == '\\') {
            snprintf(err, (size_t)err_sz,
                     "label: backslash escapes not allowed");
            return 0;
        }
        if (q[0] == '/' && q + 1 < quote_close && q[1] == '/') {
            snprintf(err, (size_t)err_sz,
                     "label: '//' not allowed in format string");
            return 0;
        }
        if (*q == '(' || *q == ')' || *q == ',') {
            snprintf(err, (size_t)err_sz,
                     "label: '%c' not allowed in format string",
                     *q);
            return 0;
        }
    }

    int fmt_len = (int)(quote_close - (quote_open + 1));
    if (fmt_len >= fmt_sz) {
        snprintf(err, (size_t)err_sz,
                 "label: format too long (max %d)", fmt_sz - 1);
        return 0;
    }
    memcpy(fmt, quote_open + 1, (size_t)fmt_len);
    fmt[fmt_len] = '\0';

    /* Post-string segment: optional. If present, leading ',' is
     * required (separating "fmt" from the substitution args). */
    const char *after = quote_close + 1;
    while (*after && isspace((unsigned char)*after)) after++;
    if (*after) {
        if (*after != ',') {
            snprintf(err, (size_t)err_sz,
                     "Usage: label(\"fmt\", arg, ...)");
            return 0;
        }
        after++;
        while (*after && isspace((unsigned char)*after)) after++;
        int post_len = (int)strlen(after);
        if (post_len >= post_sz) {
            snprintf(err, (size_t)err_sz,
                     "label: args too long");
            return 0;
        }
        memcpy(post, after, (size_t)post_len);
        post[post_len] = '\0';
    }
    return 1;
}
