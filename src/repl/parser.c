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

/* Per-slot text buffer width used by the table-driven enum-command path.
 * Sizes the scratch arrays that hold one positional argument's raw source
 * text and its canonicalized emit text. Wide enough for the longest GL
 * enum token (~30 chars) plus inline expressions. */
#define ENUM_SLOT_TEXT_MAX REPL_ENUM_ARG_MAX

/* The parser writes diagnostics to ctx->err_buf when available, and
 * otherwise no-ops; diagnostics never leave the parser as side effects
 * on REPL state. A hard guard prevents set_status calls from returning
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
        char verr[REPL_DIAG_TEXT_MAX];
        if (!repl_eval_validate_expression_idents(raw, vars, num_vars,
                                                  verr, sizeof(verr))) {
            parser_emit_error_static(ctx, verr);
            return 0;
        }
        if (input_has_any_visible_vars(raw, vars, num_vars)) {
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
        char verr[REPL_DIAG_TEXT_MAX];
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
        /* %.*s precision (not bare %s) so -Wformat-truncation sees the
         * bound made explicit at the format level. snprintf already
         * truncates to emit_sz-1, but GCC's analyzer can't tell from
         * %s alone when raw is traced back through inlining to a
         * larger source-line buffer. */
        snprintf(emit, (size_t)emit_sz, "%.*s", emit_sz - 1, raw);
        if (input_has_any_visible_vars(raw, vars, num_vars))
            *any_vars = 1;
        return 1;
    }
    }

    parser_emit_error_static(ctx, as->usage);
    return 0;
}

static void format_std_command_text(char *out, int out_sz,
                                    const char *indent,
                                    const ReplStdCommandSpec *def,
                                    const float *args) {
    int off;

    if (!out || out_sz <= 0 || !def || !def->fmt)
        return;

    off = snprintf(out, (size_t)out_sz, "%s", indent ? indent : "");
    if (off < 0 || off >= out_sz)
        return;

    switch (def->num_args) {
    case 1:
        snprintf(out + off, (size_t)(out_sz - off), def->fmt, args[0]);
        break;
    case 2:
        snprintf(out + off, (size_t)(out_sz - off),
                 def->fmt, args[0], args[1]);
        break;
    case 3:
        snprintf(out + off, (size_t)(out_sz - off),
                 def->fmt, args[0], args[1], args[2]);
        break;
    case 4:
        snprintf(out + off, (size_t)(out_sz - off),
                 def->fmt, args[0], args[1], args[2], args[3]);
        break;
    case 5:
        snprintf(out + off, (size_t)(out_sz - off),
                 def->fmt, args[0], args[1], args[2], args[3], args[4]);
        break;
    case 6:
        snprintf(out + off, (size_t)(out_sz - off),
                 def->fmt, args[0], args[1], args[2],
                 args[3], args[4], args[5]);
        break;
    default:
        break;
    }
}

/* Split a table-driven enum command's argument list into exactly N
 * top-level fields. The delimiter scan is paren-aware, so expression-
 * accepting enum slots keep inner commas/parentheses intact. */
static int split_enum_command_args(const char *args, int num_slots,
                                   char slot_raw[MAX_ENUM_ARGS][ENUM_SLOT_TEXT_MAX]) {
    const char *s = args;

    for (int slot = 0; slot < num_slots; slot++) {
        while (*s && isspace((unsigned char)*s)) s++;

        const char *delim = repl_scan_next_arg_delim(s);
        int seg_len = (int)(delim - s);
        while (seg_len > 0 && isspace((unsigned char)s[seg_len - 1]))
            seg_len--;
        if (seg_len <= 0 || seg_len >= (int)sizeof(slot_raw[slot]))
            return 0;

        memcpy(slot_raw[slot], s, (size_t)seg_len);
        slot_raw[slot][seg_len] = '\0';

        if (slot < num_slots - 1) {
            if (*delim != ',')
                return 0;
            s = delim + 1;
            continue;
        }

        while (*delim && isspace((unsigned char)*delim)) delim++;
        if (*delim != '\0')
            return 0;
    }

    return 1;
}

static void format_enum_command_text(char *out, int out_sz,
                                     const char *indent,
                                     const ReplEnumCommandSpec *def,
                                     int num_slots,
                                     const char slot_emit[MAX_ENUM_ARGS][ENUM_SLOT_TEXT_MAX]) {
    int off;

    if (!out || out_sz <= 0 || !def)
        return;

    if (num_slots == 1 && def->fmt) {
        snprintf(out, (size_t)out_sz, def->fmt, indent, slot_emit[0]);
        return;
    }
    if (num_slots == 2 && def->fmt) {
        snprintf(out, (size_t)out_sz, def->fmt, indent,
                 slot_emit[0], slot_emit[1]);
        return;
    }
    if (num_slots == 4 && def->fmt) {
        snprintf(out, (size_t)out_sz, def->fmt, indent,
                 slot_emit[0], slot_emit[1], slot_emit[2], slot_emit[3]);
        return;
    }

    /* Current specs top out at 4 slots; keep a generic join so a future
     * enum command stays readable even before it gets a dedicated fmt. */
    off = snprintf(out, (size_t)out_sz, "%s%s(", indent, def->name);
    for (int slot = 0; slot < num_slots && off < out_sz - 4; slot++) {
        off += snprintf(out + off, (size_t)(out_sz - off),
                        "%s%s", slot ? ", " : "", slot_emit[slot]);
    }
    snprintf(out + off, (size_t)(out_sz - off), ");");
}

/* Attempt the generalized enum-command path.
 *
 * Returns 1 unless a matched enum command failed to parse. `*matched`
 * tells the caller whether this helper consumed the function name.
 * Metadata rows (`num_args < 1`) intentionally leave `*matched == 0`
 * so the dedicated ad-hoc branches below can keep handling them. */
static int try_parse_table_driven_enum_command(const char *func,
                                               const char *args,
                                               GLCmd *cmd,
                                               char *text_out, int text_sz,
                                               const ReplParseContext *ctx,
                                               int *matched) {
    int source_line_idx = ctx->source_line_idx;
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;

    if (matched)
        *matched = 0;

    for (const ReplEnumCommandSpec *def = repl_enum_command_specs(); def->name; def++) {
        if (strcmp(func, def->name) != 0)
            continue;
        if (def->num_args < 1)
            return 1;
        if (matched)
            *matched = 1;

        int num_slots = def->num_args;
        if (num_slots > MAX_ENUM_ARGS)
            num_slots = MAX_ENUM_ARGS;

        char slot_raw[MAX_ENUM_ARGS][ENUM_SLOT_TEXT_MAX];
        if (!split_enum_command_args(args, num_slots, slot_raw)) {
            parser_emit_error_static(ctx, def->args[0].usage
                                     ? def->args[0].usage : "Invalid arguments");
            return 0;
        }

        float slot_val[MAX_ENUM_ARGS];
        char slot_emit[MAX_ENUM_ARGS][ENUM_SLOT_TEXT_MAX];
        int any_vars = 0;
        for (int slot = 0; slot < num_slots; slot++) {
            if (!resolve_enum_arg_slot(slot_raw[slot], &def->args[slot],
                                       vars, num_vars, &slot_val[slot],
                                       slot_emit[slot],
                                       (int)sizeof(slot_emit[slot]),
                                       &any_vars, ctx)) {
                return 0;
            }
        }

        cmd->type = def->type;
        cmd->valid = 1;
        cmd->num_args = num_slots;
        for (int slot = 0; slot < num_slots; slot++)
            cmd->args[slot] = slot_val[slot];
        if (any_vars)
            cmd->has_vars = 1;

        if (text_out && text_sz > 0) {
            char ind[REPL_INDENT_TEXT_MAX];
            if (def->indent_type == 1) {
                repl_source_scope_begin_indent(source_line_idx, ind,
                                               sizeof(ind));
            } else {
                repl_source_scope_cmd_indent(source_line_idx, ind,
                                             sizeof(ind));
            }
            format_enum_command_text(text_out, text_sz, ind, def,
                                     num_slots, slot_emit);
        }
        return 1;
    }

    return 1;
}

/* --- Extracted per-command handlers ---
 *
 * Each returns 1 (cmd populated) or 0 (parse failure, diagnostic in
 * ctx->err_buf). `indent` is the pre-computed indent string from
 * repl_source_scope_cmd_indent(source_line_idx). */

static int parse_label(const char *args, GLCmd *cmd,
                       char *text_out, int text_sz,
                       const char *indent,
                       const ReplParseContext *ctx) {
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;
    char fmt_str[GLUT_BITMAP_FMT_MAX] = "";
    char post_args[MAX_LINE_LEN] = "";
    char split_err[REPL_DIAG_TEXT_MAX] = "";

    if (!repl_label_split_args(args,
                               fmt_str, (int)sizeof(fmt_str),
                               post_args, (int)sizeof(post_args),
                               split_err, (int)sizeof(split_err))) {
        parser_emit_error_static(ctx, split_err);
        return 0;
    }

    float subs[GLUT_BITMAP_MAX_SUB_ARGS] = {0};
    int sub_count = 0;
    if (post_args[0]) {
        char verr[REPL_DIAG_TEXT_MAX];
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
    repl_copy_string_fits(cmd->payload.label.fmt, sizeof(cmd->payload.label.fmt), fmt_str);

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

static void trim_and_copy(char *dst, int dst_sz, const char *src, int src_len) {
    if (dst_sz <= 0) return;
    while (src_len > 0 && isspace((unsigned char)*src)) {
        src++;
        src_len--;
    }
    while (src_len > 0 && isspace((unsigned char)src[src_len - 1])) {
        src_len--;
    }
    if (src_len >= dst_sz) {
        src_len = dst_sz - 1;
    }
    memcpy(dst, src, (size_t)src_len);
    dst[src_len] = '\0';
}

static int split_three_args(const char *args,
                             char *a1, int a1_sz,
                             char *a2, int a2_sz,
                             char *rest, int rest_sz) {
    const char *comma1 = strchr(args, ',');
    if (!comma1) return 0;
    const char *comma2 = strchr(comma1 + 1, ',');
    if (!comma2) return 0;

    trim_and_copy(a1, a1_sz, args, (int)(comma1 - args));
    trim_and_copy(a2, a2_sz, comma1 + 1, (int)(comma2 - (comma1 + 1)));
    trim_and_copy(rest, rest_sz, comma2 + 1, (int)strlen(comma2 + 1));
    return 1;
}

static int split_two_args(const char *args,
                           char *a1, int a1_sz,
                           char *rest, int rest_sz) {
    const char *comma = strchr(args, ',');
    if (!comma) return 0;

    trim_and_copy(a1, a1_sz, args, (int)(comma - args));
    trim_and_copy(rest, rest_sz, comma + 1, (int)strlen(comma + 1));
    return 1;
}

/* Shared scaffold for glMaterialfv / glMaterialf parsing: splits
 * "face, pname, value-arg" and resolves the face token. Returns 1 on
 * success with face_str / pname_str / val_arg filled and *out_face set
 * to the resolved enum. Returns 0 after emitting `usage_msg` (on split
 * failure) or the canonical face-token error (on unknown face). The
 * caller validates pname its own way — parse_materialfv looks it up in
 * the material-params table; parse_materialf string-compares to
 * "GL_SHININESS". */
static int parse_face_pname(const char *args,
                            char *face_str, int face_sz,
                            char *pname_str, int pname_sz,
                            char *val_arg, int val_sz,
                            GLenum *out_face,
                            const ReplParseContext *ctx,
                            const char *usage_msg) {
    if (!split_three_args(args, face_str, face_sz,
                          pname_str, pname_sz, val_arg, val_sz)) {
        parser_emit_error_static(ctx, usage_msg);
        return 0;
    }
    const ReplEnumEntry *face_types = repl_face_type_entries();
    for (int i = 0; face_types[i].name; i++) {
        if (strcmp(face_str, face_types[i].name) == 0) {
            *out_face = face_types[i].value;
            return 1;
        }
    }
    parser_emit_error_static(ctx, "face: GL_FRONT, GL_BACK, GL_FRONT_AND_BACK");
    return 0;
}

static int parse_canonical_float_list(const char *text, float *out_args, int max_args,
                                      ExprVar *vars, int num_vars,
                                      const ReplParseContext *ctx) {
    char verr[REPL_DIAG_TEXT_MAX];
    if (!repl_eval_validate_expression_idents(text, vars, num_vars, verr, sizeof(verr))) {
        parser_emit_error_static(ctx, verr);
        return -1;
    }
    return repl_eval_parse_exprs(text, out_args, max_args, vars, num_vars);
}

static int parse_materialfv(const char *args, GLCmd *cmd,
                            char *text_out, int text_sz,
                            const char *indent,
                            const ReplParseContext *ctx) {
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;
    char face_str[REPL_ENUM_ARG_MAX] = "";
    char pname_str[REPL_ENUM_ARG_MAX] = "";
    char val_arg[MAX_LINE_LEN] = "";

    GLenum face = 0, pname = 0;
    if (!parse_face_pname(args, face_str, sizeof(face_str),
                          pname_str, sizeof(pname_str),
                          val_arg, sizeof(val_arg),
                          &face, ctx,
                          "Usage: glMaterialfv(face, pname, (GLfloat[]){...})"))
        return 0;

    int found2 = 0;
    const ReplEnumEntry *material_params = repl_material_param_entries();
    for (int i = 0; material_params[i].name; i++) {
        if (strcmp(pname_str, material_params[i].name) == 0) { pname = material_params[i].value; found2 = 1; break; }
    }
    if (!found2) { parser_emit_error_static(ctx, "pname: GL_DIFFUSE, GL_AMBIENT, GL_SPECULAR, GL_SHININESS..."); return 0; }

    const char *to_parse = val_arg;
    char interior_buf[MAX_LINE_LEN];
    static const char k_compound_prefix[] = "(GLfloat[]){";
    const size_t k_compound_prefix_len = sizeof k_compound_prefix - 1;
    if (strncmp(val_arg, k_compound_prefix, k_compound_prefix_len) == 0) {
        const char *istart = val_arg + k_compound_prefix_len;
        int brace_depth = 1;
        const char *q = istart;
        while (*q && brace_depth > 0) {
            if (*q == '{')      brace_depth++;
            else if (*q == '}') { brace_depth--; if (brace_depth == 0) break; }
            q++;
        }
        if (brace_depth != 0) {
            parser_emit_error_static(ctx, "Unclosed (GLfloat[]){...} literal");
            return 0;
        }
        size_t ilen = (size_t)(q - istart);
        if (ilen >= sizeof interior_buf) ilen = sizeof interior_buf - 1;
        memcpy(interior_buf, istart, ilen);
        interior_buf[ilen] = '\0';
        to_parse = interior_buf;
        const char *tail = q + 1;
        while (*tail && isspace((unsigned char)*tail)) tail++;
        if (*tail != '\0') {
            parser_emit_error_static(ctx, "Trailing content after (GLfloat[]){...} compound literal");
            return 0;
        }
    }

    float parsed_args[8];
    int num_parsed = parse_canonical_float_list(to_parse, parsed_args, 8, vars, num_vars, ctx);
    if (num_parsed < 0) return 0;

    if (num_parsed != 1 && num_parsed != 4) {
        parser_emit_error_static(ctx, "Expected 1 or 4 float values");
        return 0;
    }
    if (num_parsed == 1 && pname != GL_SHININESS) {
        parser_emit_error_static(ctx, "Only GL_SHININESS takes 1 float; other pnames need 4 RGBA floats");
        return 0;
    }

    cmd->type = CMD_MATERIALFV;
    cmd->valid = 1;
    cmd->args[0] = (float)face;
    cmd->args[1] = (float)pname;
    for (int k = 0; k < num_parsed; k++) cmd->args[k + 2] = parsed_args[k];
    cmd->num_args = num_parsed + 2;
    cmd->has_vars = input_has_any_visible_vars(to_parse, vars, num_vars);

    if (text_out && text_sz > 0) {
        if (num_parsed == 1)
            snprintf(text_out, (size_t)text_sz,
                     "%sglMaterialfv(%s, %s, (GLfloat[]){%g});",
                     indent, face_str, pname_str, parsed_args[0]);
        else
            snprintf(text_out, (size_t)text_sz,
                     "%sglMaterialfv(%s, %s, (GLfloat[]){%g, %g, %g, %g});",
                     indent, face_str, pname_str,
                     parsed_args[0], parsed_args[1], parsed_args[2], parsed_args[3]);
    }
    return 1;
}

static int parse_materialf(const char *args, GLCmd *cmd,
                           char *text_out, int text_sz,
                           const char *indent,
                           const ReplParseContext *ctx) {
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;
    char face_str[REPL_ENUM_ARG_MAX] = "";
    char pname_str[REPL_ENUM_ARG_MAX] = "";
    char val_arg[MAX_LINE_LEN] = "";

    GLenum face = 0;
    if (!parse_face_pname(args, face_str, sizeof(face_str),
                          pname_str, sizeof(pname_str),
                          val_arg, sizeof(val_arg),
                          &face, ctx,
                          "Usage: glMaterialf(face, GL_SHININESS, value)"))
        return 0;
    if (strcmp(pname_str, "GL_SHININESS") != 0) {
        parser_emit_error_static(ctx, "glMaterialf only accepts GL_SHININESS; use glMaterialfv for RGBA pnames");
        return 0;
    }

    float parsed_args[2];
    int num_parsed = parse_canonical_float_list(val_arg, parsed_args, 2, vars, num_vars, ctx);
    if (num_parsed < 0) return 0;

    if (num_parsed != 1) {
        parser_emit_error_static(ctx, "Expected 1 float value");
        return 0;
    }

    cmd->type = CMD_MATERIALF;
    cmd->valid = 1;
    cmd->args[0] = (float)face;
    cmd->args[1] = (float)GL_SHININESS;
    cmd->args[2] = parsed_args[0];
    cmd->num_args = 3;
    cmd->has_vars = input_has_any_visible_vars(val_arg, vars, num_vars);

    if (text_out && text_sz > 0)
        snprintf(text_out, (size_t)text_sz, "%sglMaterialf(%s, %s, %g);",
                 indent, face_str, pname_str, parsed_args[0]);
    return 1;
}

static int parse_point_parameter_fv(const char *args, GLCmd *cmd,
                                    char *text_out, int text_sz,
                                    const char *indent,
                                    const ReplParseContext *ctx) {
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;
    char pname_str[REPL_ENUM_ARG_MAX] = "";
    char rest[MAX_LINE_LEN] = "";

    if (!split_two_args(args, pname_str, sizeof(pname_str), rest, sizeof(rest))) {
        parser_emit_error_static(ctx,
            "Usage: glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)");
        return 0;
    }

    GLenum pname = 0;
    int found = 0;
    const ReplEnumEntry *point_param_pnames = repl_point_param_pname_entries();
    for (int i = 0; point_param_pnames[i].name; i++) {
        if (strcmp(pname_str, point_param_pnames[i].name) == 0) {
            pname = point_param_pnames[i].value;
            found = 1;
            break;
        }
    }
    if (!found) {
        parser_emit_error_static(ctx, "pname: GL_POINT_DISTANCE_ATTENUATION");
        return 0;
    }

    float parsed_args[4];
    int num_parsed = parse_canonical_float_list(rest, parsed_args, 4, vars, num_vars, ctx);
    if (num_parsed < 0) return 0;

    if (num_parsed != 3) {
        parser_emit_error_static(ctx,
            "Expected 3 floats: const, linear, quadratic attenuation coefficients");
        return 0;
    }

    cmd->type = CMD_POINT_PARAMETER_FV;
    cmd->valid = 1;
    cmd->args[0] = (float)pname;
    cmd->args[1] = parsed_args[0];
    cmd->args[2] = parsed_args[1];
    cmd->args[3] = parsed_args[2];
    cmd->num_args = 4;
    cmd->has_vars = input_has_any_visible_vars(rest, vars, num_vars);

    if (text_out && text_sz > 0)
        snprintf(text_out, (size_t)text_sz,
                 "%sglPointParameterfv(%s, (GLfloat[]){%g, %g, %g});",
                 indent, pname_str, parsed_args[0], parsed_args[1], parsed_args[2]);
    return 1;
}

static int parse_func_call(const char *args, int fn, GLCmd *cmd,
                           char *text_out, int text_sz,
                           const ReplParseContext *ctx) {
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;
    int source_line_idx = ctx->source_line_idx;

    float dummy_vals[MAX_EXPR_VARS];
    int arg_count = 0;
    if (args[0] != '\0') {
        char verr[REPL_DIAG_TEXT_MAX];
        if (!repl_eval_validate_expression_idents(args, vars, num_vars, verr, sizeof(verr))) {
            parser_emit_error_static(ctx, verr); return 0;
        }
    }
    if (!parse_expr_list_exact(args, dummy_vals, MAX_EXPR_VARS,
                               vars, num_vars, &arg_count)) {
        parser_emit_error_static(ctx, "Invalid function call arguments");
        return 0;
    }

    if (ctx->strict_refs && repl_source_scope_block_depth_at(source_line_idx) == 0) {
        int def_exists = 0;
        const GLCmd *document_cmds = repl_state_document_cmds();
        for (int di = 0; di < repl_state_document_count(); di++) {
            if (!document_cmds[di].valid) continue;
            if (document_cmds[di].type != CMD_FUNC_DEF) continue;
            if ((int)document_cmds[di].args[0] != fn) continue;
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

    char ind_str[REPL_INDENT_TEXT_MAX];
    repl_source_scope_cmd_indent(source_line_idx, ind_str, (int)sizeof(ind_str));

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
 *   5. Ad-hoc commands (glMaterialfv, glPointParameterfv, glPush/PopMatrix,
 *      funcN calls, glu* tessellator commands, goto/label)
 *
 * Returns 1 on success (cmd populated), 0 on parse failure.
 * Diagnostics flow through ReplParseContext.err_buf when the caller
 * provides one. The legacy no-ctx wrappers surface to status; the
 * parser core itself never calls set_status.
 */
static void write_text(char *out, int sz, const char *fmt, ...) {
    if (out && sz > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(out, (size_t)sz, fmt, ap);
        va_end(ap);
    }
}

/* No-arg matrix-stack commands: glPushMatrix / glPopMatrix / glLoadIdentity.
 * Returns 1 if `func` matched (cmd + text populated), 0 otherwise.
 * glPushMatrix opens an indent scope like glBegin, so its body lands one
 * level deeper (handled by repl_source_scope_cmd_indent via `indent`);
 * glPopMatrix aligns with its matching glPushMatrix — one matrix level
 * shallower — mirroring how glEnd lines up with glBegin. */
static int parse_matrix_stack_cmd(const char *func, GLCmd *cmd,
                                  char *text_out, int text_sz,
                                  const char *indent, int source_line_idx) {
    if (strcmp(func, "glPushMatrix") == 0) {
        cmd->type = CMD_PUSH_MATRIX;
        cmd->valid = 1;
        write_text(text_out, text_sz, "%sglPushMatrix();", indent);
        return 1;
    }
    if (strcmp(func, "glPopMatrix") == 0) {
        char close_ind[REPL_INDENT_TEXT_MAX];
        repl_source_scope_matrix_close_indent(source_line_idx, close_ind,
                                              sizeof(close_ind));
        cmd->type = CMD_POP_MATRIX;
        cmd->valid = 1;
        write_text(text_out, text_sz, "%sglPopMatrix();", close_ind);
        return 1;
    }
    if (strcmp(func, "glLoadIdentity") == 0) {
        cmd->type = CMD_LOAD_IDENTITY;
        cmd->valid = 1;
        write_text(text_out, text_sz, "%sglLoadIdentity();", indent);
        return 1;
    }
    return 0;
}

static int parse_command(const char *line, GLCmd *cmd,
                         char *text_out, int text_sz,
                         const ReplParseContext *ctx) {
    /* All production / test callers pass a non-NULL context (the
     * legacy no-ctx wrappers were retired earlier). The
     * `repl_state_edit_line()` fallback that lived here — it was
     * confirmed dead code, and keeping it would force the parser to
     * reach into REPL-state for cursor info that has no business
     * being parser-internal. */
    if (!ctx) return 0;
    int source_line_idx = ctx->source_line_idx;
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;
    char buf[MAX_LINE_LEN];

    /* Null-safe text helpers: write to text_out when provided */
/* Every call passes at least one variadic arg after fmt, so plain
 * __VA_ARGS__ suffices — no GNU `, ##__VA_ARGS__` comma-elision (which
 * -std=c99 -pedantic-errors rejects). */
#define WRITE_TEXT(fmt, ...) write_text(text_out, text_sz, fmt, __VA_ARGS__)
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
        char indent[REPL_INDENT_TEXT_MAX];
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

        /* Reject trailing garbage after the command's ')': only
         * whitespace, an optional ';', and a '// ...' line comment may
         * follow. Without this, e.g. `glutSolidSphere(1,10,10)fafa`
         * silently dropped the `fafa` and committed the command as if it
         * weren't there. (glMaterialfv has its own compound-literal tail
         * check; the general `(...)` command path had none.) */
        {
            const char *after = close_p + 1;
            while (*after && isspace((unsigned char)*after)) after++;
            if (*after == ';') after++;
            while (*after && isspace((unsigned char)*after)) after++;
            if (*after != '\0' && !(after[0] == '/' && after[1] == '/')) {
                parser_emit_error_static(ctx, "unexpected text after ')'");
                return 0;
            }
        }
    } else {
        if (!repl_copy_string_fits(func, sizeof(func), p))
            goto unknown_command;
    }

    /* Table-driven enum commands are a separate parser mode: match the
     * function token first, then split/resolve each positional enum slot. */
    {
        int enum_matched = 0;
        if (!try_parse_table_driven_enum_command(func, args, cmd,
                                                 text_out, text_sz,
                                                 ctx, &enum_matched))
            return 0;
        if (enum_matched)
            return 1;
    }

    /* glEnd() - aligns with its matching glBegin: 2 + 2*tess + 2*block (begin depth not added) */
    if (strcmp(func, "glEnd") == 0) {
        cmd->type = CMD_END;
        cmd->valid = 1;
        {
            char end_ind[REPL_INDENT_TEXT_MAX];
            repl_source_scope_begin_indent(source_line_idx, end_ind,
                                           sizeof(end_ind));
            WRITE_TEXT("%sglEnd();", end_ind);
        }
        return 1;
    }

    /* Indent for gl commands: 2 + 2*tess + 2*begin */
    char indent_buf[REPL_INDENT_TEXT_MAX];
    repl_source_scope_cmd_indent(source_line_idx, indent_buf, sizeof(indent_buf));
    const char *indent = indent_buf;

    /* Indent for glu (tessellator) commands: 2 + 2*tess only.
     * glu commands belong to the tessellator scope, not the GL vertex block,
     * so glBegin depth is intentionally excluded. */
    char tess_indent_buf[REPL_INDENT_TEXT_MAX];
    repl_source_scope_cmd_tess_indent(source_line_idx, tess_indent_buf, sizeof(tess_indent_buf));
    const char *tess_indent = tess_indent_buf;

    /* Table-driven parsing for standard commands */
    for (const ReplStdCommandSpec *def = repl_std_command_specs(); def->name; def++) {
        if (strcmp(func, def->name) == 0) {
            {
                char verr[REPL_DIAG_TEXT_MAX];
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
                if (text_out && text_sz > 0)
                    format_std_command_text(text_out, text_sz, ind, def,
                                            cmd->args);
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
                        if (text_out && text_sz > 0)
                            format_std_command_text(text_out, text_sz, ind,
                                                    def, cmd->args);
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
    if (strcmp(func, "label") == 0)
        return parse_label(args, cmd, text_out, text_sz, indent, ctx);

    if (strcmp(func, "glMaterialfv") == 0)
        return parse_materialfv(args, cmd, text_out, text_sz, indent, ctx);

    if (strcmp(func, "glMaterialf") == 0)
        return parse_materialf(args, cmd, text_out, text_sz, indent, ctx);

    if (strcmp(func, "glPointParameterfv") == 0)
        return parse_point_parameter_fv(args, cmd, text_out, text_sz, indent, ctx);

    /* glPushMatrix() / glPopMatrix() / glLoadIdentity() */
    if (parse_matrix_stack_cmd(func, cmd, text_out, text_sz, indent,
                               source_line_idx))
        return 1;

    {
        int fn = -1;
        const char *func_cursor = func;
        if (repl_parse_func_name_token(&func_cursor, &fn) &&
            *func_cursor == '\0' && open_p && close_p)
            return parse_func_call(args, fn, cmd, text_out, text_sz, ctx);
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
            char close_ind[REPL_INDENT_TEXT_MAX];
            repl_source_scope_tess_close_indent(source_line_idx, close_ind,
                                                (int)sizeof(close_ind));
            WRITE_TEXT("%sgluEnd();", close_ind);
        }
        return 1;
    }

    /* gluColor(r, g, b[, a]) - set per-vertex color for tessellator */
    if (strcmp(func, "gluColor") == 0) {
        {
            char verr[REPL_DIAG_TEXT_MAX];
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
        char clean_lname[REPL_GOTO_LABEL_MAX]; int ll = 0;
        while (ll < REPL_GOTO_LABEL_MAX - 1 && lname[ll] && lname[ll] != ';' && !isspace((unsigned char)lname[ll])) {
            clean_lname[ll] = lname[ll]; ll++;
        }
        clean_lname[ll] = '\0';
        if (ll > 0) {
            cmd->type = CMD_GOTO;
            cmd->valid = 1;
            {
                char ind_str[REPL_INDENT_TEXT_MAX];
                repl_source_scope_cmd_indent(source_line_idx, ind_str,
                                             (int)sizeof(ind_str));
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
            char label[REPL_GOTO_LABEL_MAX];
            int n = 0;
            while (n < (int)sizeof(label) - 1 &&
                   p[n] && p[n] != ':' && !isspace((unsigned char)p[n])) {
                label[n] = p[n];
                n++;
            }
            label[n] = '\0';
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

int repl_parser_parse_command_ctx(const char *line, ReplParsedLine *out,
                           const ReplParseContext *ctx) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    int skip_text = ctx && ctx->skip_text;
    char *text_out = skip_text ? NULL : out->text;
    int   text_sz  = skip_text ? 0 : (int)sizeof(out->text);
    if (!parse_command(line, &out->cmd, text_out, text_sz, ctx))
        return 0;

    /* Reject commands that real GL rejects with GL_INVALID_OPERATION inside
     * glBegin/glEnd. The executor used to defensively glEnd() the active
     * begin block before running them, which broke loop-body usage by
     * tearing down the block for subsequent glVertex calls. Now the parser
     * is the gate, so the executor can rely on the invariant. */
    if (ctx && !repl_cmd_type_valid_in_begin(out->cmd.type) &&
        repl_source_scope_in_begin_block_at(ctx->source_line_idx)) {
        parser_emit_error(ctx, "%s not valid inside glBegin/glEnd",
                          repl_cmd_type_display_name(out->cmd.type));
        return 0;
    }

    /* Carry a trailing `// ...` comment from the input onto the canonical
     * text. parse_command rebuilds out->text from the parsed args (with
     * the trailing `;` for needs-semicolon commands) and drops the
     * comment; re-attaching it here means every consumer of the canonical
     * text — typed commit, reformat, the inline swatch, Enter/insert
     * commits, and verbatim C export — preserves the inline comment from
     * one place. Idempotent: a no-op when out->text already ends in a
     * comment (e.g. CMD_COMMENT lines, whose whole text is the comment). */
    if (!skip_text)
        repl_append_trailing_comment(out->text, sizeof(out->text), line);
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
