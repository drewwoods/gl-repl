/*
 * src/repl/parser.c - REPL text command parser.
 *
 * Converts one source line into a GLCmd and canonical source text. The parse
 * context carries the source-line index used for scope-sensitive indentation
 * plus the loop/function locals visible at that line.
 */
#include <stdio.h>
#include <string.h>
#include "repl/parser.h"
#include "repl/text_helpers.h"

#include "repl/command_spec.h"
#include "repl/color_limits.h"
#include "repl/stencil_limits.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/source_scope.h"
#include "repl/util.h"          /* repl_format_fits / repl_copy_string_fits */

#include "config.h" /* REPL_ENUM_ARG_MAX */

#include <stdarg.h>
#include <stdlib.h>  /* strtod (strict bool-slot numeric literal) */

/* Per-slot text buffer width used by the table-driven enum-command path.
 * Sizes the scratch arrays that hold one positional argument's raw source
 * text and its canonicalized emit text. Bitfield slots can contain every
 * supported token joined by `|`, so this is line-sized rather than sized for
 * one enum token. */
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

static void write_text(char *out, int sz, const char *fmt, ...);

/* Fire the parse context's expression-span capture (if any) over a whole
 * NUL-terminated helper buffer. The sink compiles the span during the
 * call, so handing out pointers into parser-local buffers is safe. */
static void parser_capture_expr_span(const ReplParseContext *ctx,
                                     ReplExprRole role, int ordinal,
                                     const char *text) {
    if (ctx && ctx->capture && ctx->capture->fn)
        ctx->capture->fn(ctx->capture->user_data, role, ordinal,
                         text, text + strlen(text));
}

static void parser_emit_error_static(const ReplParseContext *ctx, const char *msg) {
    parser_emit_error(ctx, "%s", msg ? msg : "");
}

static int parser_validate_expression_idents(const char *src,
                                             const ExprVar *vars,
                                             int num_vars,
                                             const ReplParseContext *ctx) {
    char err[REPL_DIAG_TEXT_MAX];

    if (repl_eval_validate_expression_idents(
            &(ReplExprIdentValidationConfig){
                .src = src,
                .vars = vars,
                .num_vars = num_vars,
                .predef = repl_eval_predef_view(),
                .err = err,
                .errsz = (int)sizeof(err),
            }))
        return 1;

    parser_emit_error_static(ctx, err);
    return 0;
}

static const ReplSourceScopeView *parser_source_scope(const ReplParseContext *ctx) {
    return ctx ? ctx->source_scope : NULL;
}

static int parser_scope_block_depth_at(const ReplParseContext *ctx, int pos) {
    return repl_source_scope_view_block_depth_at(parser_source_scope(ctx), pos);
}

static int parser_scope_in_begin_block_at(const ReplParseContext *ctx, int pos) {
    return repl_source_scope_view_in_begin_block_at(parser_source_scope(ctx), pos);
}

static int parser_scope_in_loop_at(const ReplParseContext *ctx, int pos) {
    return repl_source_scope_view_in_loop_at(parser_source_scope(ctx), pos);
}

static void parser_scope_cmd_indent(const ReplParseContext *ctx,
                                    int pos, char *buf, int buf_sz) {
    repl_source_scope_view_cmd_indent(parser_source_scope(ctx), pos, buf, buf_sz);
}

static void parser_scope_begin_indent(const ReplParseContext *ctx,
                                      int pos, char *buf, int buf_sz) {
    repl_source_scope_view_begin_indent(parser_source_scope(ctx), pos, buf, buf_sz);
}

static void parser_scope_tess_close_indent(const ReplParseContext *ctx,
                                           int pos, char *buf, int buf_sz) {
    repl_source_scope_view_tess_close_indent(parser_source_scope(ctx), pos,
                                             buf, buf_sz);
}

static void parser_scope_cmd_tess_indent(const ReplParseContext *ctx,
                                         int pos, char *buf, int buf_sz) {
    repl_source_scope_view_cmd_tess_indent(parser_source_scope(ctx), pos,
                                           buf, buf_sz);
}

static void parser_scope_matrix_close_indent(const ReplParseContext *ctx,
                                             int pos, char *buf, int buf_sz) {
    repl_source_scope_view_matrix_close_indent(parser_source_scope(ctx), pos,
                                               buf, buf_sz);
}

static const char *parser_func_alias_get(const ReplParseContext *ctx, int slot) {
    if (!ctx || !ctx->func_aliases.names)
        return NULL;
    if (slot < 0 || slot >= ctx->func_aliases.count ||
        slot >= REPL_FUNC_SLOT_COUNT)
        return NULL;
    return ctx->func_aliases.names[slot][0]
        ? ctx->func_aliases.names[slot]
        : NULL;
}

static int parser_func_alias_lookup_slot(const ReplParseContext *ctx,
                                         const char *name) {
    if (!ctx || !ctx->func_aliases.names || !name || !*name)
        return -1;
    int count = ctx->func_aliases.count;
    if (count > REPL_FUNC_SLOT_COUNT)
        count = REPL_FUNC_SLOT_COUNT;
    for (int slot = 0; slot < count; slot++) {
        if (ctx->func_aliases.names[slot][0] &&
            strcmp(ctx->func_aliases.names[slot], name) == 0)
            return slot;
    }
    return -1;
}

static int parser_parse_func_name_token(const ReplParseContext *ctx,
                                        const char **p_inout, int *fn) {
    const char *p = *p_inout;
    char ident[REPL_FUNC_NAME_MAX];
    int kind = repl_scan_func_name_token(&p, fn, ident);
    if (kind == 0) return 0;
    if (kind == 2) {
        int slot = parser_func_alias_lookup_slot(ctx, ident);
        if (slot < 0) return 0;
        if (fn) *fn = slot;
    }
    *p_inout = p;
    return 1;
}

static int parser_func_def_exists(const ReplParseContext *ctx, int fn) {
    const ReplSourceScopeView *scope = parser_source_scope(ctx);
    if (!scope || !scope->cmds)
        return 0;
    for (int di = 0; di < scope->count; di++) {
        if (!scope->cmds[di].valid) continue;
        if (scope->cmds[di].type != CMD_FUNC_DEF) continue;
        if ((int)scope->cmds[di].args[0] != fn) continue;
        return 1;
    }
    return 0;
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
        "glPopAttrib",
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
static int resolve_enum_arg_slot(const char *raw, int slot_idx,
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
        if (!parser_validate_expression_idents(raw, vars, num_vars, ctx))
            return 0;
        if (input_has_any_visible_vars(raw, vars, num_vars)) {
            parser_emit_error_static(ctx, as->usage);
            return 0;
        }
        /* Boolean-mask slot: accept only a well-formed numeric literal,
         * then reverse-map. repl_eval_parse_exprs / repl_eval_expr do
         * NOT require full input consumption (eval.c) - they evaluate
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

    case REPL_ENUM_SLOT_ENUM_BITFIELD: {
        /* `A | B | ...` - every term must be a token from this slot's
         * table (the exact-match above already handled the single-token
         * case, so a `|` is normally present by the time we get here), or
         * the slot's optional all-bits alias. Numeric and expression input
         * stay rejected: a mask is not a quantity the REPL can animate, and
         * accepting one would let a typo'd literal enable a bit the table
         * deliberately omits.
         *
         * Emission is table order with duplicates dropped. When an all-bits
         * alias is configured, any spelling that resolves to the complete
         * table union emits that compact alias. */
        unsigned mask = 0;
        unsigned all_mask = 0;
        const char *p = raw;
        if (as->bitfield_all_alias) {
            for (int i = 0; as->enums && as->enums[i].name; i++)
                all_mask |= (unsigned)as->enums[i].value;
        }
        /* Unconditional first iteration, and one more after every `|`:
         * an empty term ("A |", "| A", "A || B") must reach the len <= 0
         * rejection rather than be skipped by a loop guard. */
        for (;;) {
            const char *bar = strchr(p, '|');
            const char *term_end = bar ? bar : p + strlen(p);
            char term[REPL_ENUM_ARG_MAX];
            int len;

            while (p < term_end && isspace((unsigned char)*p)) p++;
            len = (int)(term_end - p);
            while (len > 0 && isspace((unsigned char)p[len - 1])) len--;
            if (len <= 0 || len >= (int)sizeof(term)) {
                parser_emit_error_static(ctx, as->usage);
                return 0;
            }
            memcpy(term, p, (size_t)len);
            term[len] = '\0';

            int found = 0;
            if (as->bitfield_all_alias &&
                strcmp(term, as->bitfield_all_alias) == 0) {
                mask |= all_mask;
                found = 1;
            }
            for (int i = 0; !found && as->enums && as->enums[i].name; i++) {
                if (strcmp(term, as->enums[i].name) == 0) {
                    mask |= (unsigned)as->enums[i].value;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                parser_emit_error_static(ctx, as->usage);
                return 0;
            }
            if (!bar)
                break;
            p = bar + 1;
        }

        int off = 0;
        emit[0] = '\0';
        if (as->bitfield_all_alias && mask == all_mask) {
            if (snprintf(emit, (size_t)emit_sz, "%s",
                         as->bitfield_all_alias) >= emit_sz) {
                parser_emit_error_static(ctx, as->usage);
                return 0;
            }
            *out_val = (float)mask;
            return 1;
        }
        for (int i = 0; as->enums && as->enums[i].name; i++) {
            if (!(mask & (unsigned)as->enums[i].value))
                continue;
            off += snprintf(emit + off, (size_t)(emit_sz - off), "%s%s",
                            off ? " | " : "", as->enums[i].name);
            if (off >= emit_sz) {
                parser_emit_error_static(ctx, as->usage);
                return 0;
            }
        }
        *out_val = (float)mask;
        return 1;
    }

    case REPL_ENUM_SLOT_ENUM_OR_EXPR: {
        if (!parser_validate_expression_idents(raw, vars, num_vars, ctx))
            return 0;
        float fv;
        if (repl_eval_parse_exprs(raw, &fv, 1, vars, num_vars) != 1) {
            parser_emit_error_static(ctx, as->usage);
            return 0;
        }
        /* Expression-resolved slot (the token table didn't match): the
         * compiled path re-evaluates exactly this slot. Token-matched
         * slots are never captured - their baked enum value is the
         * command's constant argument. */
        parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG, slot_idx, raw);
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

/* Format one float arg for canonical line text. Round-trip exact
 * (strtof(text) == v) via repl_format_source_float: for a !has_vars
 * command the canonical text is the only source-of-record of a folded
 * constant expression, and the flatten force-reparse path re-evaluates
 * that text expecting the committed bits - a lossy "%g" here (6
 * significant digits) made e.g. `glColor3f(0.92*0.55, ...)` commit
 * arg bits its own canonical text no longer reproduced. */
static const char *fmt_source_float(char buf[REPL_SOURCE_FLOAT_TEXT_MAX],
                                    float v) {
    repl_format_source_float(buf, REPL_SOURCE_FLOAT_TEXT_MAX, v);
    return buf;
}

static void format_std_command_text(char *out, int out_sz,
                                    const char *indent,
                                    const ReplStdCommandSpec *def,
                                    const float *args) {
    int off;
    int ai = 0;
    const char *f;

    if (!out || out_sz <= 0 || !def || !def->fmt)
        return;

    off = snprintf(out, (size_t)out_sz, "%s", indent ? indent : "");
    if (off < 0 || off >= out_sz)
        return;

    /* def->fmt is a printf template whose only directives are one %g
     * per argument; substitute each with the round-trip-exact form. */
    for (f = def->fmt; *f && off < out_sz - 1; f++) {
        if (f[0] == '%' && f[1] == 'g' && ai < def->num_args) {
            char fbuf[REPL_SOURCE_FLOAT_TEXT_MAX];
            int n = snprintf(out + off, (size_t)(out_sz - off), "%s",
                             fmt_source_float(fbuf, args[ai]));
            if (n < 0 || n >= out_sz - off) {
                out[out_sz - 1] = '\0';
                return;
            }
            off += n;
            ai++;
            f++;               /* skip the 'g' */
        } else {
            out[off++] = *f;
        }
    }
    out[off] = '\0';
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
    if (num_slots == 3 && def->fmt) {
        snprintf(out, (size_t)out_sz, def->fmt, indent,
                 slot_emit[0], slot_emit[1], slot_emit[2]);
        return;
    }
    if (num_slots == 4 && def->fmt) {
        snprintf(out, (size_t)out_sz, def->fmt, indent,
                 slot_emit[0], slot_emit[1], slot_emit[2], slot_emit[3]);
        return;
    }

    /* Current specs top out at 4 slots; keep a generic join so any enum
     * command without a dedicated format string stays readable. */
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
            if (!resolve_enum_arg_slot(slot_raw[slot], slot, &def->args[slot],
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
                parser_scope_begin_indent(ctx, source_line_idx, ind,
                                          sizeof(ind));
            } else {
                parser_scope_cmd_indent(ctx, source_line_idx, ind,
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
 * ctx->err_buf). `indent` is the pre-computed source-scope indent
 * string for source_line_idx. */

/* gluColor(r, g, b[, a]) - per-vertex tessellator color. Alpha defaults to
 * 1.0 when omitted; num_args is always canonicalized to 4. */
static int parse_glu_color(const char *args, GLCmd *cmd,
                           char *text_out, int text_sz,
                           const char *tess_indent,
                           const ReplParseContext *ctx) {
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;

    if (!parser_validate_expression_idents(args, vars, num_vars, ctx))
        return 0;
    parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                             0, args);
    cmd->num_args = repl_eval_parse_exprs(args, cmd->args, 4, vars, num_vars);
    if (cmd->num_args >= 3) {
        if (cmd->num_args < 4) cmd->args[3] = 1.0f;
        cmd->num_args = 4;
        cmd->type = CMD_TESS_COLOR;
        cmd->valid = 1;
        cmd->has_vars = input_has_any_visible_vars(args, vars, num_vars);
        if (text_out && text_sz > 0) {
            char b0[REPL_SOURCE_FLOAT_TEXT_MAX], b1[REPL_SOURCE_FLOAT_TEXT_MAX];
            char b2[REPL_SOURCE_FLOAT_TEXT_MAX], b3[REPL_SOURCE_FLOAT_TEXT_MAX];
            write_text(text_out, text_sz, "%sgluColor(%s, %s, %s, %s);",
                       tess_indent,
                       fmt_source_float(b0, cmd->args[0]),
                       fmt_source_float(b1, cmd->args[1]),
                       fmt_source_float(b2, cmd->args[2]),
                       fmt_source_float(b3, cmd->args[3]));
        }
        return 1;
    }
    parser_emit_error_static(ctx, "Usage: gluColor(r, g, b) or gluColor(r, g, b, a)");
    return 0;
}

static int parse_label_like(const char *args, GLCmd *cmd,
                             char *text_out, int text_sz,
                             const char *indent,
                             const ReplParseContext *ctx,
                             CmdType type, const char *func_name,
                             int max_sub_args) {
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;
    char fmt_str[GLUT_BITMAP_FMT_MAX] = "";
    char post_args[MAX_LINE_LEN] = "";
    char split_err[REPL_DIAG_TEXT_MAX] = "";

    if (!repl_label_split_args_named(args,
                                     fmt_str, (int)sizeof(fmt_str),
                                     post_args, (int)sizeof(post_args),
                                     split_err, (int)sizeof(split_err),
                                     func_name)) {
        parser_emit_error_static(ctx, split_err);
        return 0;
    }

    float subs[REPL_CONSOLE_MAX_SUB_ARGS] = {0};
    int sub_count = 0;
    if (post_args[0]) {
        if (!parser_validate_expression_idents(post_args, vars, num_vars, ctx))
            return 0;
        parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                                 0, post_args);
        float subs_full[REPL_CONSOLE_MAX_SUB_ARGS + 4];
        int parsed = repl_eval_parse_exprs(
            post_args, subs_full,
            (int)(sizeof(subs_full) / sizeof(subs_full[0])),
            vars, num_vars);
        if (parsed > max_sub_args) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "%s: too many args (max %d)",
                     func_name,
                     max_sub_args);
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
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "%s: only %%f and %%%% allowed in format",
                     func_name);
            parser_emit_error_static(ctx, buf);
            return 0;
        }
    }
    if (pct_count != sub_count) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "%s: format expects %d arg%s, got %d",
                 func_name,
                 pct_count, pct_count == 1 ? "" : "s", sub_count);
        parser_emit_error_static(ctx, buf);
        return 0;
    }

    cmd->type = type;
    cmd->valid = 1;
    for (int i = 0; i < sub_count; i++) cmd->args[i] = subs[i];
    cmd->num_args = sub_count;
    cmd->has_vars = input_has_any_visible_vars(post_args, vars, num_vars);
    repl_copy_string_fits(cmd->payload.label.fmt, sizeof(cmd->payload.label.fmt), fmt_str);

    if (text_out && text_sz > 0) {
        int off = snprintf(text_out, (size_t)text_sz,
                           "%s%s(\"%s\"", indent, func_name, fmt_str);
        for (int i = 0; i < sub_count && off < (int)text_sz - 6; i++) {
            char fbuf[REPL_SOURCE_FLOAT_TEXT_MAX];
            off += snprintf(text_out + off, (size_t)(text_sz - off),
                            ", %s", fmt_source_float(fbuf, subs[i]));
        }
        snprintf(text_out + off, (size_t)(text_sz - off), ");");
    }
    return 1;
}

static int parse_label(const char *args, GLCmd *cmd,
                       char *text_out, int text_sz,
                       const char *indent,
                       const ReplParseContext *ctx) {
    return parse_label_like(args, cmd, text_out, text_sz, indent, ctx,
                            CMD_LABEL, "label", GLUT_BITMAP_MAX_SUB_ARGS);
}

static int parse_console(const char *args, GLCmd *cmd,
                         char *text_out, int text_sz,
                         const char *indent,
                         const ReplParseContext *ctx) {
    return parse_label_like(args, cmd, text_out, text_sz, indent, ctx,
                            CMD_CONSOLE, "console", REPL_CONSOLE_MAX_SUB_ARGS);
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
 * caller validates pname its own way - parse_materialfv looks it up in
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
    if (!parser_validate_expression_idents(text, vars, num_vars, ctx))
        return -1;
    return repl_eval_parse_exprs(text, out_args, max_args, vars, num_vars);
}

/* Shared scaffold for the compound-literal value arg of glMaterialfv /
 * glClipPlane: if `arg` starts with `prefix` (e.g. "(GLfloat[]){"),
 * extract the brace interior into interior_buf and point *out_to_parse
 * at it; otherwise leave *out_to_parse on the raw arg (flat shorthand).
 * `unclosed_msg` / `trailing_msg` are the per-command diagnostics. */
static int parse_compound_literal_arg(const char *arg,
                                      const char *prefix,
                                      const char *unclosed_msg,
                                      const char *trailing_msg,
                                      char *interior_buf,
                                      int interior_sz,
                                      const char **out_to_parse,
                                      const ReplParseContext *ctx) {
    size_t prefix_len = prefix ? strlen(prefix) : 0;

    if (!arg || !prefix || !interior_buf || interior_sz <= 0 || !out_to_parse)
        return 0;

    *out_to_parse = arg;
    if (strncmp(arg, prefix, prefix_len) != 0)
        return 1;

    {
        const char *istart = arg + prefix_len;
        int brace_depth = 1;
        const char *q = istart;
        while (*q && brace_depth > 0) {
            if (*q == '{') {
                brace_depth++;
            } else if (*q == '}') {
                brace_depth--;
                if (brace_depth == 0)
                    break;
            }
            q++;
        }
        if (brace_depth != 0) {
            parser_emit_error_static(ctx, unclosed_msg);
            return 0;
        }
        {
            size_t ilen = (size_t)(q - istart);
            if (ilen >= (size_t)interior_sz)
                ilen = (size_t)interior_sz - 1;
            memcpy(interior_buf, istart, ilen);
            interior_buf[ilen] = '\0';
        }
        {
            const char *tail = q + 1;
            while (*tail && isspace((unsigned char)*tail))
                tail++;
            if (*tail != '\0') {
                parser_emit_error_static(ctx, trailing_msg);
                return 0;
            }
        }
    }

    *out_to_parse = interior_buf;
    return 1;
}

/* glMaterialfv(face, pname, ...) - the one command whose value argument
 * is an aggregate. Accepts the canonical compound-literal form
 * `(GLfloat[]){r, g, b, a}` (1 element for GL_SHININESS, 4 for the RGBA
 * pnames) or the flat shorthand `face, pname, r, g, b, a`, which is
 * rewritten to the compound-literal form in the canonical text. */
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
    if (!parse_compound_literal_arg(val_arg, "(GLfloat[]){",
                                    "Unclosed (GLfloat[]){...} literal",
                                    "Trailing content after (GLfloat[]){...} compound literal",
                                    interior_buf,
                                    (int)sizeof(interior_buf),
                                    &to_parse, ctx))
        return 0;

    float parsed_args[8];
    int num_parsed = parse_canonical_float_list(to_parse, parsed_args, 8, vars, num_vars, ctx);
    if (num_parsed < 0) return 0;
    /* Value list lands at args[2..]; face/pname stay baked enum tokens. */
    parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                             2, to_parse);

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
        char b0[REPL_SOURCE_FLOAT_TEXT_MAX], b1[REPL_SOURCE_FLOAT_TEXT_MAX];
        char b2[REPL_SOURCE_FLOAT_TEXT_MAX], b3[REPL_SOURCE_FLOAT_TEXT_MAX];
        if (num_parsed == 1)
            snprintf(text_out, (size_t)text_sz,
                     "%sglMaterialfv(%s, %s, (GLfloat[]){%s});",
                     indent, face_str, pname_str,
                     fmt_source_float(b0, parsed_args[0]));
        else
            snprintf(text_out, (size_t)text_sz,
                     "%sglMaterialfv(%s, %s, (GLfloat[]){%s, %s, %s, %s});",
                     indent, face_str, pname_str,
                     fmt_source_float(b0, parsed_args[0]),
                     fmt_source_float(b1, parsed_args[1]),
                     fmt_source_float(b2, parsed_args[2]),
                     fmt_source_float(b3, parsed_args[3]));
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
    /* Shininess value lands at args[2]; face/pname stay baked. */
    parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                             2, val_arg);

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

    if (text_out && text_sz > 0) {
        char b0[REPL_SOURCE_FLOAT_TEXT_MAX];
        snprintf(text_out, (size_t)text_sz, "%sglMaterialf(%s, %s, %s);",
                 indent, face_str, pname_str,
                 fmt_source_float(b0, parsed_args[0]));
    }
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
    const char *to_parse = rest;
    char interior_buf[MAX_LINE_LEN];
    if (!parse_compound_literal_arg(rest, "(GLfloat[]){",
                                    "Unclosed (GLfloat[]){...} literal",
                                    "Trailing content after (GLfloat[]){...} compound literal",
                                    interior_buf,
                                    (int)sizeof(interior_buf),
                                    &to_parse, ctx))
        return 0;
    int num_parsed = parse_canonical_float_list(to_parse, parsed_args, 4, vars, num_vars, ctx);
    if (num_parsed < 0) return 0;
    /* Coefficients land at args[1..3]; the pname stays baked. */
    parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                             1, to_parse);

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
    cmd->has_vars = input_has_any_visible_vars(to_parse, vars, num_vars);

    if (text_out && text_sz > 0) {
        char b0[REPL_SOURCE_FLOAT_TEXT_MAX], b1[REPL_SOURCE_FLOAT_TEXT_MAX];
        char b2[REPL_SOURCE_FLOAT_TEXT_MAX];
        snprintf(text_out, (size_t)text_sz,
                 "%sglPointParameterfv(%s, (GLfloat[]){%s, %s, %s});",
                 indent, pname_str,
                 fmt_source_float(b0, parsed_args[0]),
                 fmt_source_float(b1, parsed_args[1]),
                 fmt_source_float(b2, parsed_args[2]));
    }
    return 1;
}

/* glClipPlane(plane, ...) - same aggregate-value shape as glMaterialfv.
 * Accepts the canonical compound-literal form `(GLdouble[]){a, b, c, d}`
 * or the flat shorthand `plane, a, b, c, d`, which is rewritten to the
 * compound-literal form in the canonical text. The equation is stored
 * as floats in args[1..4] (GL widens to double at the glClipPlane call
 * in the executor). */
static int parse_clip_plane(const char *args, GLCmd *cmd,
                            char *text_out, int text_sz,
                            const char *indent,
                            const ReplParseContext *ctx) {
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;
    char plane_str[REPL_ENUM_ARG_MAX] = "";
    char rest[MAX_LINE_LEN] = "";

    if (!split_two_args(args, plane_str, sizeof(plane_str), rest, sizeof(rest))) {
        parser_emit_error_static(ctx,
            "Usage: glClipPlane(plane, (GLdouble[]){a, b, c, d})");
        return 0;
    }

    GLenum plane = 0;
    int found = 0;
    const ReplEnumEntry *planes = repl_clip_plane_entries();
    for (int i = 0; planes[i].name; i++) {
        if (strcmp(plane_str, planes[i].name) == 0) {
            plane = planes[i].value;
            found = 1;
            break;
        }
    }
    if (!found) {
        parser_emit_error_static(ctx, "plane: GL_CLIP_PLANE0 .. GL_CLIP_PLANE5");
        return 0;
    }

    const char *to_parse = rest;
    char interior_buf[MAX_LINE_LEN];
    if (!parse_compound_literal_arg(rest, "(GLdouble[]){",
                                    "Unclosed (GLdouble[]){...} literal",
                                    "Trailing content after (GLdouble[]){...} compound literal",
                                    interior_buf,
                                    (int)sizeof(interior_buf),
                                    &to_parse, ctx))
        return 0;

    float parsed_args[5];
    int num_parsed = parse_canonical_float_list(to_parse, parsed_args, 5, vars, num_vars, ctx);
    if (num_parsed < 0) return 0;
    /* Coefficients land at args[1..4]; the plane enum stays baked. Keep
     * this custom aggregate parser on the same capture contract as
     * glMaterialfv and glPointParameterfv so warm flatten re-evaluates
     * variable-backed equations instead of retaining commit-time args. */
    parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                             1, to_parse);

    if (num_parsed != 4) {
        parser_emit_error_static(ctx,
            "Expected 4 floats: plane equation a*x + b*y + c*z + d >= 0");
        return 0;
    }

    cmd->type = CMD_CLIP_PLANE;
    cmd->valid = 1;
    cmd->args[0] = (float)plane;
    for (int k = 0; k < 4; k++) cmd->args[k + 1] = parsed_args[k];
    cmd->num_args = 5;
    cmd->has_vars = input_has_any_visible_vars(to_parse, vars, num_vars);

    if (text_out && text_sz > 0) {
        char b0[REPL_SOURCE_FLOAT_TEXT_MAX], b1[REPL_SOURCE_FLOAT_TEXT_MAX];
        char b2[REPL_SOURCE_FLOAT_TEXT_MAX], b3[REPL_SOURCE_FLOAT_TEXT_MAX];
        snprintf(text_out, (size_t)text_sz,
                 "%sglClipPlane(%s, (GLdouble[]){%s, %s, %s, %s});",
                 indent, plane_str,
                 fmt_source_float(b0, parsed_args[0]),
                 fmt_source_float(b1, parsed_args[1]),
                 fmt_source_float(b2, parsed_args[2]),
                 fmt_source_float(b3, parsed_args[3]));
    }
    return 1;
}

/* True when `args` is nothing but an identifier - the shape that selects
 * glMultMatrixf's scratch-array form. Anything else (a brace literal, a
 * number, a subscript, a comma list) falls through to the value form,
 * which produces the better diagnostic for it. */
static int mult_matrixf_lone_ident(const char *args,
                                   char *name, int name_sz) {
    const char *p = args;
    int n = 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (!repl_eval_is_ident_start((unsigned char)*p))
        return 0;
    while (*p && n < name_sz - 1 &&
           repl_eval_is_ident_continue((unsigned char)*p))
        name[n++] = *p++;
    name[n] = '\0';
    while (*p && isspace((unsigned char)*p)) p++;
    return *p == '\0';
}

/* glMultMatrixf - post-multiply the current matrix by a column-major 4x4,
 * in either of two argument forms:
 *
 *   glMultMatrixf(A)                        a scratch array (A, B, or C),
 *                                           its 16 cells written the
 *                                           ordinary way by `A[k] = ...`
 *                                           lines ahead of this one
 *   glMultMatrixf((GLfloat[]){m0, ..., m15}) the 16 values inline
 *
 * plus the flat shorthand `glMultMatrixf(m0, ..., m15)`, rewritten to the
 * compound-literal form in the canonical text - the glMaterialfv /
 * glClipPlane / glFogfv precedent.
 *
 * The forms differ in where the values come from, not in what reaches the
 * flat program: both land in payload.matrix. The array form stores only
 * the name (args[0]), which keeps the source line stable while the cells
 * behind it change, and flatten snapshots those cells when it emits the
 * flat command. The value form has no indirection to preserve, so the
 * parse fills the payload and the captured expression slots re-evaluate
 * it - that is what lets a literal matrix animate. */
static int parse_mult_matrixf(const char *args, GLCmd *cmd,
                              char *text_out, int text_sz,
                              const char *indent,
                              const ReplParseContext *ctx) {
    static const char k_usage[] =
        "Usage: glMultMatrixf(A) - a scratch array (A, B, or C) - or "
        "glMultMatrixf((GLfloat[]){m0, ..., m15}) - 16 column-major values";
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;
    char name[REPL_PREDEF_NAME_MAX] = "";

    cmd->type = CMD_MULT_MATRIXF;
    cmd->valid = 1;
    memset(&cmd->payload, 0, sizeof(cmd->payload));

    if (mult_matrixf_lone_ident(args, name, (int)sizeof(name))) {
        int array_idx = repl_eval_scratch_array_index(name);
        if (array_idx < 0) {
            parser_emit_error_static(ctx, k_usage);
            return 0;
        }
        cmd->args[0] = (float)array_idx;
        cmd->num_args = 1;
        /* No expression slots to re-evaluate - the line is a name. The
         * cells behind that name still change every frame, but they are
         * picked up by the flatten-time snapshot, which runs on every path
         * into the flat array including this command's has_vars=0 fast
         * path. */
        cmd->has_vars = 0;
        /* Identity until flatten fills it: a zeroed matrix would collapse
         * the scene, and this form's values never come from the parse. */
        cmd->payload.matrix.m[0] = 1.0f;
        cmd->payload.matrix.m[5] = 1.0f;
        cmd->payload.matrix.m[10] = 1.0f;
        cmd->payload.matrix.m[15] = 1.0f;

        write_text(text_out, text_sz, "%sglMultMatrixf(%s);", indent, name);
        return 1;
    }

    {
        const char *to_parse = args;
        char interior_buf[MAX_LINE_LEN];
        /* One past the cell count, so an over-long list is caught here
         * rather than silently truncated (the glClipPlane precedent). */
        float parsed[REPL_MATRIX_CELL_COUNT + 1];
        int num_parsed;

        if (!parse_compound_literal_arg(args, "(GLfloat[]){",
                                        "Unclosed (GLfloat[]){...} literal",
                                        "Trailing content after (GLfloat[]){...} compound literal",
                                        interior_buf,
                                        (int)sizeof(interior_buf),
                                        &to_parse, ctx))
            return 0;

        num_parsed = parse_canonical_float_list(to_parse, parsed,
                                                REPL_MATRIX_CELL_COUNT + 1,
                                                vars, num_vars, ctx);
        if (num_parsed < 0)
            return 0;
        if (num_parsed != REPL_MATRIX_CELL_COUNT) {
            parser_emit_error_static(ctx, k_usage);
            return 0;
        }
        /* The cells are the whole command, so they capture from ordinal 0.
         * args[] stays empty - the values do not fit in it - which is also
         * how the two forms are told apart downstream
         * (repl_cmd_mult_matrix_from_array). */
        parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                                 0, to_parse);

        cmd->num_args = 0;
        cmd->has_vars = input_has_any_visible_vars(to_parse, vars, num_vars);
        for (int k = 0; k < REPL_MATRIX_CELL_COUNT; k++)
            cmd->payload.matrix.m[k] = parsed[k];

        if (text_out && text_sz > 0) {
            char list[MAX_LINE_LEN];
            int used = 0;

            list[0] = '\0';
            for (int k = 0; k < REPL_MATRIX_CELL_COUNT; k++) {
                char b[REPL_SOURCE_FLOAT_TEXT_MAX];
                int wrote = snprintf(list + used, sizeof(list) - (size_t)used,
                                     "%s%s", k ? ", " : "",
                                     fmt_source_float(b, parsed[k]));
                if (wrote < 0 || wrote >= (int)sizeof(list) - used)
                    break;
                used += wrote;
            }
            snprintf(text_out, (size_t)text_sz,
                     "%sglMultMatrixf((GLfloat[]){%s});", indent, list);
        }
        return 1;
    }
}

/* glFogf(pname, value) - enum pname + one scalar expression, the
 * glMaterialf shape without the face arg. GL_FOG_MODE lives on glFogi
 * (it takes an enum, not a float) and GL_FOG_COLOR on glFogfv. */
static int parse_fogf(const char *args, GLCmd *cmd,
                      char *text_out, int text_sz,
                      const char *indent,
                      const ReplParseContext *ctx) {
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;
    char pname_str[REPL_ENUM_ARG_MAX] = "";
    char val_arg[MAX_LINE_LEN] = "";

    if (!split_two_args(args, pname_str, sizeof(pname_str), val_arg, sizeof(val_arg))) {
        parser_emit_error_static(ctx, "Usage: glFogf(pname, value)");
        return 0;
    }

    GLenum pname = 0;
    int found = 0;
    const ReplEnumEntry *pnames = repl_fog_f_pname_entries();
    for (int i = 0; pnames[i].name; i++) {
        if (strcmp(pname_str, pnames[i].name) == 0) {
            pname = pnames[i].value;
            found = 1;
            break;
        }
    }
    if (!found) {
        parser_emit_error_static(ctx,
            "pname: GL_FOG_DENSITY, GL_FOG_START, GL_FOG_END");
        return 0;
    }

    float parsed_args[2];
    int num_parsed = parse_canonical_float_list(val_arg, parsed_args, 2, vars, num_vars, ctx);
    if (num_parsed < 0) return 0;
    /* Value lands at args[1]; the pname stays baked. */
    parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                             1, val_arg);

    if (num_parsed != 1) {
        parser_emit_error_static(ctx, "Expected 1 float value");
        return 0;
    }

    cmd->type = CMD_FOG_F;
    cmd->valid = 1;
    cmd->args[0] = (float)pname;
    cmd->args[1] = parsed_args[0];
    cmd->num_args = 2;
    cmd->has_vars = input_has_any_visible_vars(val_arg, vars, num_vars);

    if (text_out && text_sz > 0) {
        char b0[REPL_SOURCE_FLOAT_TEXT_MAX];
        snprintf(text_out, (size_t)text_sz, "%sglFogf(%s, %s);",
                 indent, pname_str,
                 fmt_source_float(b0, parsed_args[0]));
    }
    return 1;
}

/* glFogfv(GL_FOG_COLOR, ...) - same aggregate-value shape as
 * glPointParameterfv. Accepts the canonical `(GLfloat[]){r, g, b, a}`
 * compound literal or the flat shorthand `GL_FOG_COLOR, r, g, b, a`,
 * rewritten to the compound-literal form in the canonical text. */
static int parse_fogfv(const char *args, GLCmd *cmd,
                       char *text_out, int text_sz,
                       const char *indent,
                       const ReplParseContext *ctx) {
    ExprVar *vars = ctx->vars;
    int num_vars = ctx->num_vars;
    char pname_str[REPL_ENUM_ARG_MAX] = "";
    char rest[MAX_LINE_LEN] = "";

    if (!split_two_args(args, pname_str, sizeof(pname_str), rest, sizeof(rest))) {
        parser_emit_error_static(ctx,
            "Usage: glFogfv(GL_FOG_COLOR, (GLfloat[]){r, g, b, a})");
        return 0;
    }

    GLenum pname = 0;
    int found = 0;
    const ReplEnumEntry *pnames = repl_fog_color_pname_entries();
    for (int i = 0; pnames[i].name; i++) {
        if (strcmp(pname_str, pnames[i].name) == 0) {
            pname = pnames[i].value;
            found = 1;
            break;
        }
    }
    if (!found) {
        parser_emit_error_static(ctx, "pname: GL_FOG_COLOR");
        return 0;
    }

    const char *to_parse = rest;
    char interior_buf[MAX_LINE_LEN];
    if (!parse_compound_literal_arg(rest, "(GLfloat[]){",
                                    "Unclosed (GLfloat[]){...} literal",
                                    "Trailing content after (GLfloat[]){...} compound literal",
                                    interior_buf,
                                    (int)sizeof(interior_buf),
                                    &to_parse, ctx))
        return 0;

    float parsed_args[5];
    int num_parsed = parse_canonical_float_list(to_parse, parsed_args, 5, vars, num_vars, ctx);
    if (num_parsed < 0) return 0;
    /* Color lands at args[1..4]; the pname stays baked. Same capture
     * contract as glMaterialfv / glClipPlane so warm flatten
     * re-evaluates variable-backed channels. */
    parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                             1, to_parse);

    if (num_parsed != 4) {
        parser_emit_error_static(ctx, "Expected 4 floats: r, g, b, a");
        return 0;
    }

    cmd->type = CMD_FOG_FV;
    cmd->valid = 1;
    cmd->args[0] = (float)pname;
    for (int k = 0; k < 4; k++) cmd->args[k + 1] = parsed_args[k];
    cmd->num_args = 5;
    cmd->has_vars = input_has_any_visible_vars(to_parse, vars, num_vars);

    if (text_out && text_sz > 0) {
        char b0[REPL_SOURCE_FLOAT_TEXT_MAX], b1[REPL_SOURCE_FLOAT_TEXT_MAX];
        char b2[REPL_SOURCE_FLOAT_TEXT_MAX], b3[REPL_SOURCE_FLOAT_TEXT_MAX];
        snprintf(text_out, (size_t)text_sz,
                 "%sglFogfv(%s, (GLfloat[]){%s, %s, %s, %s});",
                 indent, pname_str,
                 fmt_source_float(b0, parsed_args[0]),
                 fmt_source_float(b1, parsed_args[1]),
                 fmt_source_float(b2, parsed_args[2]),
                 fmt_source_float(b3, parsed_args[3]));
    }
    return 1;
}

/* A bare integer mask literal - decimal or 0x-prefixed - in 0..max_value.
 * `out_is_hex` (optional) reports which spelling the source used, for the
 * commands that echo the source form back into their canonical text.
 *
 * A NULL `usage` makes the probe silent: glLineStipple asks "is this slot a
 * hex literal?" and falls through to the expression path when it is not, so
 * a diagnostic there would fire on every animated pattern. */
static int parse_mask_value_literal(const char *text, long max_value,
                                    int *out_value, int *out_is_hex,
                                    const char *usage,
                                    const ReplParseContext *ctx) {
    char buf[REPL_ENUM_ARG_MAX];
    char *p = buf;
    char *end;
    long value;
    int is_hex = 0;
    size_t len;

    while (*text && isspace((unsigned char)*text)) text++;
    len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) len--;
    if (len == 0 || len >= sizeof(buf)) {
        if (usage) parser_emit_error_static(ctx, usage);
        return 0;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        is_hex = 1;
    value = strtol(p, &end, is_hex ? 16 : 10);
    if (end == p || *end != '\0' || value < 0 || value > max_value) {
        if (usage) parser_emit_error_static(ctx, usage);
        return 0;
    }
    if (out_value) *out_value = (int)value;
    if (out_is_hex) *out_is_hex = is_hex;
    return 1;
}

static int parse_stencil_value_literal(const char *text, int *out_value,
                                       const char *usage,
                                       const ReplParseContext *ctx) {
    return parse_mask_value_literal(text, REPL_STENCIL_VALUE_MAX, out_value,
                                    NULL, usage, ctx);
}

/* The 0..255 *quantity* slot shared by glStencilFunc's ref and
 * glClearStencil's value: a full expression (so it can animate), evaluated
 * here for the constant case and clamped by the same helper the flatten
 * post-evaluation fixup uses. A literal out of range is rejected - the user
 * is right there to be told; an animated one is clamped per frame instead,
 * because a per-frame parse error is not a usable failure mode.
 *
 * `capture_slot` is the arg index parser_capture_expr_span records, so the
 * compiled-expression cache compiles exactly the span this parse evaluated.
 * Callers own emitting the canonical text, which differs per command. */
/* Evaluate one expression-valued argument slot. Value policy (range,
 * truncation) and the expression-span capture belong to the caller, which
 * is what lets a caller reject an out-of-range literal without leaving a
 * compiled span behind for a line that never commits. */
static int parse_expr_arg_slot(const char *slot_text, const char *arity_msg,
                               float *out_value, int *out_has_vars,
                               const ReplParseContext *ctx) {
    float parsed[2];
    int parsed_count;

    if (!parser_validate_expression_idents(slot_text, ctx->vars, ctx->num_vars, ctx))
        return 0;
    parsed_count = parse_canonical_float_list(slot_text, parsed, 2,
                                              ctx->vars, ctx->num_vars, ctx);
    if (parsed_count < 0)
        return 0;
    if (parsed_count != 1) {
        parser_emit_error_static(ctx, arity_msg);
        return 0;
    }
    if (out_value) *out_value = parsed[0];
    if (out_has_vars)
        *out_has_vars = input_has_any_visible_vars(slot_text, ctx->vars,
                                                   ctx->num_vars);
    return 1;
}

static int parse_stencil_quantity_slot(const char *slot_text, int capture_slot,
                                       const char *range_msg,
                                       const char *arity_msg,
                                       int *out_value, int *out_has_vars,
                                       float *out_raw,
                                       const ReplParseContext *ctx) {
    float parsed[2];
    int has_vars;
    int value;

    if (!parse_expr_arg_slot(slot_text, arity_msg, &parsed[0], &has_vars, ctx))
        return 0;
    if (!has_vars && !repl_stencil_clamp_ref(parsed[0], &value)) {
        parser_emit_error_static(ctx, range_msg);
        return 0;
    }
    (void)repl_stencil_clamp_ref(parsed[0], &value);

    parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                             capture_slot, slot_text);
    if (out_value) *out_value = value;
    if (out_has_vars) *out_has_vars = has_vars;
    if (out_raw) *out_raw = parsed[0];
    return 1;
}

static int parse_clear_stencil(const char *args, GLCmd *cmd,
                               char *text_out, int text_sz,
                               const char *indent,
                               const ReplParseContext *ctx) {
    int value;
    int has_vars;
    float raw;
    char slot_raw[MAX_ENUM_ARGS][ENUM_SLOT_TEXT_MAX];

    if (!split_enum_command_args(args, 1, slot_raw)) {
        parser_emit_error_static(ctx, "Usage: glClearStencil(value)");
        return 0;
    }
    if (!parse_stencil_quantity_slot(slot_raw[0], 0,
                                     "value: must be in the range 0..255",
                                     "value: expected one numeric expression",
                                     &value, &has_vars, &raw, ctx))
        return 0;

    cmd->type = CMD_CLEAR_STENCIL;
    cmd->valid = 1;
    cmd->args[0] = (float)value;
    cmd->num_args = 1;
    cmd->has_vars = has_vars;
    if (text_out && text_sz > 0) {
        char value_text[REPL_SOURCE_FLOAT_TEXT_MAX];
        /* The canonical form can outgrow its source line - the interactive
         * `;` path hands us a line with no trailing semicolon - so refuse an
         * over-long line rather than store a truncated row. */
        if (!repl_format_fits(text_out, (size_t)text_sz,
                              "%sglClearStencil(%s);", indent,
                              has_vars ? slot_raw[0]
                                       : fmt_source_float(value_text, raw))) {
            parser_emit_error_static(ctx, "Command too long");
            return 0;
        }
    }
    return 1;
}

static int parse_stencil_func(const char *args, GLCmd *cmd,
                              char *text_out, int text_sz,
                              const char *indent,
                              const ReplParseContext *ctx) {
    char slot_raw[MAX_ENUM_ARGS][ENUM_SLOT_TEXT_MAX];
    const ReplEnumEntry *funcs = repl_depth_func_entries();
    GLenum func = 0;
    int found = 0;
    float raw_ref = 0.0f;
    int ref;
    int mask;
    int has_vars;

    if (!split_enum_command_args(args, 3, slot_raw)) {
        parser_emit_error_static(ctx, "Usage: glStencilFunc(func, ref, mask)");
        return 0;
    }
    for (int i = 0; funcs[i].name; i++) {
        if (strcmp(slot_raw[0], funcs[i].name) == 0) {
            func = funcs[i].value;
            found = 1;
            break;
        }
    }
    if (!found) {
        parser_emit_error_static(ctx,
            "func: GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS");
        return 0;
    }
    if (!parse_stencil_quantity_slot(slot_raw[1], 1,
                                     "ref: must be in the range 0..255",
                                     "ref: expected one numeric expression",
                                     &ref, &has_vars, &raw_ref, ctx))
        return 0;
    if (!parse_stencil_value_literal(slot_raw[2], &mask,
                                     "mask: decimal or 0xNN literal in 0..255", ctx))
        return 0;

    cmd->type = CMD_STENCIL_FUNC;
    cmd->valid = 1;
    cmd->args[0] = (float)func;
    cmd->args[1] = (float)ref;
    cmd->args[2] = (float)mask;
    cmd->num_args = 3;
    cmd->has_vars = has_vars;
    if (text_out && text_sz > 0) {
        char ref_text[REPL_SOURCE_FLOAT_TEXT_MAX];
        /* Wider than its source line: the mask is re-rendered as 0xNN (a
         * literal `5` grows by three chars) and the interactive `;` path
         * supplies no trailing semicolon. Reject rather than truncate. */
        if (!repl_format_fits(text_out, (size_t)text_sz,
                              "%sglStencilFunc(%s, %s, 0x%02X);",
                              indent, slot_raw[0],
                              has_vars ? slot_raw[1]
                                       : fmt_source_float(ref_text, raw_ref),
                              (unsigned)mask)) {
            parser_emit_error_static(ctx, "Command too long");
            return 0;
        }
    }
    return 1;
}

static int parse_stencil_mask(const char *args, GLCmd *cmd,
                              char *text_out, int text_sz,
                              const char *indent,
                              const ReplParseContext *ctx) {
    int mask;
    if (!parse_stencil_value_literal(args, &mask,
                                     "Usage: glStencilMask(mask) - mask is decimal or 0xNN in 0..255", ctx))
        return 0;
    cmd->type = CMD_STENCIL_MASK;
    cmd->valid = 1;
    cmd->args[0] = (float)mask;
    cmd->num_args = 1;
    cmd->has_vars = 0;
    write_text(text_out, text_sz, "%sglStencilMask(0x%02X);", indent,
               (unsigned)mask);
    return 1;
}

/* The stipple pattern is the 16 bits glLineStipple takes as a GLushort. */
#define REPL_LINE_STIPPLE_PATTERN_MAX 65535

/* glLineStipple(factor, pattern).
 *
 * Both slots are expressions (the pattern can animate), so this could be a
 * k_std_command_specs row - except that the pattern is a *bit pattern*, and
 * `0xAAAA` says "alternate on/off" in a way `43690` does not. The generic
 * std path re-renders every argument from its evaluated float, which would
 * turn that spelling into its decimal value on commit. So the slot is
 * probed for a bare hex literal first and echoed back as 0xNNNN; anything
 * else is the ordinary expression path and renders as its value.
 *
 * A constant pattern outside 0..65535 is rejected here rather than silently
 * wrapping in the executor's GLushort cast; an animated one is left alone,
 * because a per-frame parse error is not a usable failure mode. */
static int parse_line_stipple(const char *args, GLCmd *cmd,
                              char *text_out, int text_sz,
                              const char *indent,
                              const ReplParseContext *ctx) {
    static const char *k_usage =
        "Usage: glLineStipple(factor, pattern) - pattern is an expression, or a decimal or 0xNNNN literal in 0..65535";
    char slot_raw[MAX_ENUM_ARGS][ENUM_SLOT_TEXT_MAX];
    char pattern_text[ENUM_SLOT_TEXT_MAX];
    char factor_buf[REPL_SOURCE_FLOAT_TEXT_MAX];
    const char *factor_text;
    float factor = 0.0f;
    float pattern = 0.0f;
    int factor_has_vars = 0;
    int pattern_has_vars = 0;
    int pattern_bits = 0;
    int pattern_is_hex = 0;

    if (!split_enum_command_args(args, 2, slot_raw)) {
        parser_emit_error_static(ctx, k_usage);
        return 0;
    }
    if (!parse_expr_arg_slot(slot_raw[0], k_usage, &factor, &factor_has_vars, ctx))
        return 0;
    parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                             0, slot_raw[0]);

    if (parse_mask_value_literal(slot_raw[1], REPL_LINE_STIPPLE_PATTERN_MAX,
                                 &pattern_bits, &pattern_is_hex, NULL, ctx) &&
        pattern_is_hex) {
        pattern = (float)pattern_bits;
        snprintf(pattern_text, sizeof(pattern_text), "0x%04X",
                 (unsigned)pattern_bits);
    } else {
        if (!parse_expr_arg_slot(slot_raw[1], k_usage, &pattern,
                                 &pattern_has_vars, ctx))
            return 0;
        if (!pattern_has_vars &&
            (pattern < 0.0f || pattern > (float)REPL_LINE_STIPPLE_PATTERN_MAX)) {
            parser_emit_error_static(ctx, "pattern: must be in the range 0..65535");
            return 0;
        }
        parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST_LENIENT,
                                 1, slot_raw[1]);
        if (pattern_has_vars) {
            snprintf(pattern_text, sizeof(pattern_text), "%s", slot_raw[1]);
        } else {
            char pattern_buf[REPL_SOURCE_FLOAT_TEXT_MAX];
            snprintf(pattern_text, sizeof(pattern_text), "%s",
                     fmt_source_float(pattern_buf, pattern));
        }
    }

    cmd->type = CMD_LINE_STIPPLE;
    cmd->valid = 1;
    cmd->args[0] = factor;
    cmd->args[1] = pattern;
    cmd->num_args = 2;
    cmd->has_vars = (factor_has_vars || pattern_has_vars);

    factor_text = factor_has_vars ? slot_raw[0]
                                  : fmt_source_float(factor_buf, factor);
    if (text_out && text_sz > 0) {
        /* Wider than its source line: a hex pattern is re-rendered padded
         * to four digits and the interactive `;` path supplies no trailing
         * semicolon. Reject rather than store a truncated row. */
        if (!repl_format_fits(text_out, (size_t)text_sz,
                              "%sglLineStipple(%s, %s);", indent,
                              factor_text, pattern_text)) {
            parser_emit_error_static(ctx, "Command too long");
            return 0;
        }
    }
    return 1;
}

/* Dispatcher for the custom-branch commands that escape both spec
 * tables. Returns -1 when `func` matches none of them (the caller
 * falls through to the remaining parsers); otherwise the handler's
 * 0/1 result.
 *
 * label("fmt", a, b, c, d) is custom because one arg is a string
 * literal the std-table parsers don't tokenize and the
 * substitution-arg count is variable (forbidden inside the format
 * string: '//', '(', ')', ',', and any backslash - these keep the
 * string-unaware parser scaffolding honest; see
 * repl_label_split_args()). The others carry an aggregate value arg
 * (a compound literal) or, for glMaterialf, share glMaterialfv's
 * face/pname scaffold. */
static int try_parse_custom_arg_command(const char *func, const char *args,
                                        GLCmd *cmd,
                                        char *text_out, int text_sz,
                                        const char *indent,
                                        const ReplParseContext *ctx) {
    if (strcmp(func, "label") == 0)
        return parse_label(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "console") == 0)
        return parse_console(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "glMaterialfv") == 0)
        return parse_materialfv(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "glMaterialf") == 0)
        return parse_materialf(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "glPointParameterfv") == 0)
        return parse_point_parameter_fv(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "glClipPlane") == 0)
        return parse_clip_plane(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "glMultMatrixf") == 0)
        return parse_mult_matrixf(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "glFogf") == 0)
        return parse_fogf(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "glFogfv") == 0)
        return parse_fogfv(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "glLineStipple") == 0)
        return parse_line_stipple(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "glClearStencil") == 0)
        return parse_clear_stencil(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "glStencilFunc") == 0)
        return parse_stencil_func(args, cmd, text_out, text_sz, indent, ctx);
    if (strcmp(func, "glStencilMask") == 0)
        return parse_stencil_mask(args, cmd, text_out, text_sz, indent, ctx);
    return -1;
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
        if (!parser_validate_expression_idents(args, vars, num_vars, ctx))
            return 0;
    }
    if (!parse_expr_list_exact(args, dummy_vals, MAX_EXPR_VARS,
                               vars, num_vars, &arg_count)) {
        parser_emit_error_static(ctx, "Invalid function call arguments");
        return 0;
    }

    if (ctx->strict_refs && parser_scope_block_depth_at(ctx, source_line_idx) == 0) {
        int def_exists = parser_func_def_exists(ctx, fn);
        if (!def_exists) {
            char buf[96];
            const char *alias = parser_func_alias_get(ctx, fn);
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
    parser_scope_cmd_indent(ctx, source_line_idx, ind_str, (int)sizeof(ind_str));

    char raw_args[MAX_LINE_LEN];
    strncpy(raw_args, args, sizeof(raw_args) - 1);
    raw_args[sizeof(raw_args) - 1] = '\0';
    trim_in_place(raw_args);
    const char *alias = parser_func_alias_get(ctx, fn);
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
 *      funcN calls, glu* tessellator commands)
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

/* No-arg matrix-stack commands: glPushMatrix / glPopMatrix / glLoadIdentity,
 * plus glPopAttrib (the no-arg glPushAttrib partner). Returns 1 if `func`
 * matched (cmd + text populated), 0 otherwise. glPushMatrix opens an indent
 * scope like glBegin, so its body lands one level deeper (handled by the
 * precomputed source-scope `indent`); glPopMatrix aligns with its matching
 * glPushMatrix - one matrix level shallower - mirroring how glEnd lines up
 * with glBegin. glPopAttrib takes the plain `indent` (glPushAttrib opens no
 * indent scope). */
static int parse_matrix_stack_cmd(const char *func, GLCmd *cmd,
                                  char *text_out, int text_sz,
                                  const char *indent,
                                  const ReplParseContext *ctx,
                                  int source_line_idx) {
    if (strcmp(func, "glPushMatrix") == 0) {
        cmd->type = CMD_PUSH_MATRIX;
        cmd->valid = 1;
        write_text(text_out, text_sz, "%sglPushMatrix();", indent);
        return 1;
    }
    if (strcmp(func, "glPopMatrix") == 0) {
        char close_ind[REPL_INDENT_TEXT_MAX];
        parser_scope_matrix_close_indent(ctx, source_line_idx, close_ind,
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
    /* glPopAttrib() rides here (no-arg, plain command indent) rather than
     * next to glEnd so parse_command stays under its size baseline. Unlike
     * glPopMatrix it takes no close-scope indent: glPushAttrib opens no
     * indent scope by design (see add-push-attrib.md). */
    if (strcmp(func, "glPopAttrib") == 0) {
        cmd->type = CMD_POP_ATTRIB;
        cmd->valid = 1;
        write_text(text_out, text_sz, "%sglPopAttrib();", indent);
        return 1;
    }
    return 0;
}

static void strip_trailing_comment(char *p) {
    int in_str = 0;
    for (char *q = p; *q; q++) {
        if (*q == '"') {
            in_str = !in_str;
        } else if (!in_str && q[0] == '/' && q[1] == '/') {
            *q = '\0';
            break;
        }
    }
}

/* The REPL line parser: match one trimmed source line to a CmdType,
 * evaluate its argument expressions, and emit the canonical line text.
 * Deliberately one flat dispatcher - each command match is independent,
 * so the high branch count is breadth, not depth. The order is:
 *
 *   1. trim; `// comment` short-circuit (before stripping `;` - a comment
 *      may end with `;` as prose); then strip trailing `;` / empty line
 *   2. split `func(args)` and reject trailing garbage after the ')'
 *   3. table-driven enum commands (glEnable, glDepthFunc, ...) via
 *      try_parse_table_driven_enum_command
 *   4. table-driven float-arg commands (k_std_command_specs)
 *   5. hand-written matchers for the irregular forms: glEnd, label(),
 *      glMaterialfv/f, glPointParameterfv, matrix stack, gluBegin/End/
 *      Color, break / continue
 *   6. `unknown_command:` fallback - recognize a bare math expression
 *      and suggest assigning it, else emit the per-context error
 *
 * On success the matched arm sets cmd->type/args/valid and writes the
 * canonical text (indent derived from source scope); a trailing `// ...`
 * comment from the input is re-attached at the single exit point. */
static int check_trailing_garbage(const char *after, const ReplParseContext *ctx) {
    while (*after && isspace((unsigned char)*after)) after++;
    if (*after == ';') after++;
    while (*after && isspace((unsigned char)*after)) after++;
    if (*after != '\0' && !(after[0] == '/' && after[1] == '/')) {
        parser_emit_error_static(ctx, "unexpected text after ')'");
        return 0;
    }
    return 1;
}

/* Bare-keyword statements: the REPL forms that are a word rather than a
 * `name(args)` call - `break`, `continue` and `return`. Split out of
 * parse_command so this keyword set can grow without the dispatcher doing.
 *
 * Returns 1 when the line parsed, 0 when it was one of these forms but
 * invalid (diagnostic already emitted), and -1 when it is none of them -
 * the caller then continues into its unknown-command reporting. `p` is
 * the trimmed line with any trailing `;` already stripped. */
static int parse_keyword_statement(const char *p, GLCmd *cmd,
                                   char *text_out, int text_sz,
                                   const ReplParseContext *ctx,
                                   int source_line_idx) {
    /* break / continue - leave the enclosing for-loop, or skip to its next
     * iteration. Both are resolved entirely in flatten (which unrolls the
     * loop), so neither ever reaches the flat program or the executor.
     *
     * The enclosing-loop check needs a source-scope view to answer; a
     * context-free parse (tests, tools) has no document to walk, so it
     * accepts the line and leaves the diagnostic to flatten, which fails
     * the frame if the statement really has no loop to bind to. */
    if (strcmp(p, "break") == 0 || strcmp(p, "continue") == 0) {
        int is_break = (p[0] == 'b');
        if (!ctx->pending_loop_body && parser_source_scope(ctx) &&
            !parser_scope_in_loop_at(ctx, source_line_idx)) {
            parser_emit_error(ctx,
                "'%s' is only valid inside a for-loop body", p);
            return 0;
        }
        cmd->type = is_break ? CMD_BREAK : CMD_CONTINUE;
        cmd->valid = 1;
        cmd->num_args = 0;
        {
            char ind_str[REPL_INDENT_TEXT_MAX];
            parser_scope_cmd_indent(ctx, source_line_idx, ind_str,
                                    (int)sizeof(ind_str));
            write_text(text_out, text_sz, "%s%s;", ind_str, is_break ? "break" : "continue");
        }
        return 1;
    }

    /* return - leave the enclosing function body, or (at top level) end the
     * frame early. Unlike break/continue there is no scope test to make:
     * both destinations are legal, and both are legal in the exported C
     * too, since a func def exports as `static void` and the display body
     * as `static void draw_scene(void)`. The one placement that is not
     * legal - inside a glBegin block - is rejected ahead of this by the
     * command spec's valid_in_begin flag, so it needs no check here.
     * Resolved entirely in flatten, like break/continue. */
    if (strcmp(p, "return") == 0) {
        cmd->type = CMD_RETURN;
        cmd->valid = 1;
        cmd->num_args = 0;
        {
            char ind_str[REPL_INDENT_TEXT_MAX];
            parser_scope_cmd_indent(ctx, source_line_idx, ind_str,
                                    (int)sizeof(ind_str));
            write_text(text_out, text_sz, "%sreturn;", ind_str);
        }
        return 1;
    }

    /* Keep C-style spellings from falling through to the generic command
     * error or the expression-only reserved-name diagnostic. `return` has no
     * value in the REPL, so the only valid spelling is `return;`. */
    if (strncmp(p, "return", 6) == 0 &&
        !repl_eval_is_ident_continue((unsigned char)p[6]) && p[6] != '\0') {
        parser_emit_error(ctx, "return takes no value - use 'return;'");
        return 0;
    }

    return -1;
}

/* Diagnose a reserved name that reached the unknown-command fallback.
 * Returns 1 when it emitted a diagnostic (caller fails the parse), 0 when
 * the name is not reserved and the generic "unknown cmd" applies.
 *
 * Recognising these before the generic message matters because otherwise
 * `rand();` or `sin(t);` get the same diagnostic as a misspelled GL call,
 * which is actively misleading. The three arms are three different
 * mistakes: a builtin call used as a statement (it produces a value -
 * assign it), a C keyword (the REPL has no such statement *and* it is not
 * an operand either), and a REPL constant or scratch array (an operand
 * typed where a command goes).
 *
 * `has_args` distinguishes `sin(t)` from a bare `sin`. */
static int parser_report_reserved_not_command(const char *func, int has_args,
                                              const ReplParseContext *ctx) {
    if (!func || !func[0] || !repl_eval_is_reserved_ident(func))
        return 0;

    if (has_args) {
        parser_emit_error(ctx,
            "'%s(...)' is an expression, not a command - assign it "
            "(e.g. 'x = %s(...);') or use it inside another expression",
            func, func);
    } else if (repl_eval_is_c_keyword(func)) {
        parser_emit_error(ctx,
            "'%s' is a C keyword - the REPL has no such statement", func);
    } else {
        parser_emit_error(ctx,
            "'%s' is a reserved name (constant or scratch array), "
            "not a command - use it inside an expression", func);
    }
    return 1;
}

/* Full-line comments are source rows, not statements. Keep their prose
 * outside the terminator stripping and expression parsing in parse_command,
 * while retaining the parser's canonical scope indentation. */
static int parse_full_line_comment(char *p, GLCmd *cmd,
                                   char *text_out, int text_sz,
                                   const ReplParseContext *ctx,
                                   int source_line_idx) {
    int len = (int)strlen(p);

    while (len > 0 && isspace((unsigned char)p[len - 1]))
        p[--len] = '\0';
    cmd->type = CMD_COMMENT;
    cmd->valid = 1;
    cmd->is_auto = 0;
    cmd->num_args = 0;
    {
        char indent[REPL_INDENT_TEXT_MAX];
        parser_scope_cmd_indent(ctx, source_line_idx, indent, sizeof(indent));
        write_text(text_out, text_sz, "%s%s", indent, p);
    }
    return 1;
}

static int parse_command(const char *line, GLCmd *cmd,
                         char *text_out, int text_sz,
                         const ReplParseContext *ctx) {
    /* All production / test callers pass a non-NULL context (the
     * legacy no-ctx wrappers were retired earlier). The
     * `repl_state_edit_line()` fallback that lived here - it was
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
 * __VA_ARGS__ suffices - no GNU `, ##__VA_ARGS__` comma-elision (which
 * -std=c99 -pedantic-errors rejects). */
#define WRITE_TEXT(fmt, ...) write_text(text_out, text_sz, fmt, __VA_ARGS__)
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;

    int len = (int)strlen(p);

    cmd->valid = 0;
    cmd->num_args = 0;
    if (text_out && text_sz > 0) text_out[0] = '\0';

    /* Comment short-circuit runs before the statement-terminator `;` strip.
     * A full-line comment may end with `;` as prose ("// note ends here;")
     * and that character is part of the comment body, not a terminator. Only
     * trailing whitespace is shed so the stored text stays what the author
     * wrote. */
    if (len > 0 && p[0] == '/' && p[1] == '/') {
        return parse_full_line_comment(p, cmd, text_out, text_sz, ctx,
                                       source_line_idx);
    }

    while (len > 0 && (p[len - 1] == ';' || isspace((unsigned char)p[len - 1])))
        p[--len] = '\0';

    if (len == 0) {
        cmd->type = CMD_EMPTY;
        cmd->valid = 1;
        cmd->is_auto = 0;
        cmd->num_args = 0;
        return 1;
    }

    /* Strip a trailing `// ...` line comment from the working buffer before
     * locating the arg-list parens below. parse_command finds the close
     * paren with strrchr(p, ')'), so a ')' inside the comment (e.g.
     * `glColor3f(1,0,0); // tint (a)`) would be mistaken for the command's
     * close paren and corrupt arg extraction. The caller re-attaches the
     * comment to the canonical text (repl_append_trailing_comment on the
     * original line), so dropping it here only affects parsing. The scan is
     * string-aware - a `//` inside a "..." literal is left in place, so
     * label("a // b") still reaches its dedicated "// forbidden" check. */
    {
        strip_trailing_comment(p);
        len = (int)strlen(p);
        while (len > 0 && (p[len - 1] == ';' || isspace((unsigned char)p[len - 1])))
            p[--len] = '\0';
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
        if (!check_trailing_garbage(close_p + 1, ctx)) {
            return 0;
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
            parser_scope_begin_indent(ctx, source_line_idx, end_ind,
                                      sizeof(end_ind));
            WRITE_TEXT("%sglEnd();", end_ind);
        }
        return 1;
    }

    /* Indent for gl commands: 2 + 2*tess + 2*begin */
    char indent_buf[REPL_INDENT_TEXT_MAX];
    parser_scope_cmd_indent(ctx, source_line_idx, indent_buf, sizeof(indent_buf));
    const char *indent = indent_buf;

    /* Indent for glu (tessellator) commands: 2 + 2*tess only.
     * glu commands belong to the tessellator scope, not the GL vertex block,
     * so glBegin depth is intentionally excluded. */
    char tess_indent_buf[REPL_INDENT_TEXT_MAX];
    parser_scope_cmd_tess_indent(ctx, source_line_idx, tess_indent_buf,
                                 sizeof(tess_indent_buf));
    const char *tess_indent = tess_indent_buf;

    /* Table-driven parsing for standard commands */
    for (const ReplStdCommandSpec *def = repl_std_command_specs(); def->name; def++) {
        if (strcmp(func, def->name) == 0) {
            if (!parser_validate_expression_idents(args, vars, num_vars, ctx))
                return 0;
            int exact_count = 0;
            if (parse_expr_list_exact(args, cmd->args, def->num_args,
                                      vars, num_vars, &exact_count) &&
                exact_count == def->num_args) {
                parser_capture_expr_span(ctx, REPL_EXPR_ROLE_CMD_ARG_LIST,
                                         0, args);
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
                        if (cmd->args[ci] > REPL_CLEAR_COLOR_MAX_V) {
                            cmd->args[ci] = REPL_CLEAR_COLOR_MAX_V;
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

    /* Custom-branch commands (label / glMaterialfv / glMaterialf /
     * glPointParameterfv / glClipPlane) - see the dispatcher for why
     * each escapes the tables. */
    {
        int custom_rv = try_parse_custom_arg_command(func, args, cmd,
                                                     text_out, text_sz,
                                                     indent, ctx);
        if (custom_rv >= 0)
            return custom_rv;
    }

    /* glPushMatrix() / glPopMatrix() / glLoadIdentity() */
    if (parse_matrix_stack_cmd(func, cmd, text_out, text_sz, indent,
                               ctx, source_line_idx))
        return 1;

    {
        int fn = -1;
        const char *func_cursor = func;
        if (parser_parse_func_name_token(ctx, &func_cursor, &fn) &&
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
            parser_scope_tess_close_indent(ctx, source_line_idx, close_ind,
                                           (int)sizeof(close_ind));
            WRITE_TEXT("%sgluEnd();", close_ind);
        }
        return 1;
    }

    /* gluColor(r, g, b[, a]) - set per-vertex color for tessellator */
    if (strcmp(func, "gluColor") == 0)
        return parse_glu_color(args, cmd, text_out, text_sz, tess_indent, ctx);

    {
        int kw = parse_keyword_statement(p, cmd, text_out, text_sz,
                                         ctx, source_line_idx);
        if (kw >= 0)
            return kw;
    }

unknown_command:
    if (parser_report_reserved_not_command(func, open_p != NULL, ctx))
        return 0;
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
        parser_scope_in_begin_block_at(ctx, ctx->source_line_idx)) {
        parser_emit_error(ctx, "%s not valid inside glBegin/glEnd",
                          repl_cmd_type_display_name(out->cmd.type));
        return 0;
    }

    /* Carry a trailing `// ...` comment from the input onto the canonical
     * text. parse_command rebuilds out->text from the parsed args (with
     * the trailing `;` for needs-semicolon commands) and drops the
     * comment; re-attaching it here means every consumer of the canonical
     * text - typed commit, reformat, the inline swatch, Enter/insert
     * commits, and verbatim C export - preserves the inline comment from
     * one place. Idempotent: a no-op when out->text already ends in a
     * comment (e.g. CMD_COMMENT lines, whose whole text is the comment). */
    if (!skip_text)
        repl_append_trailing_comment(out->text, sizeof(out->text), line);
    return 1;
}

int repl_label_split_args_named(const char *args,
                                char *fmt, int fmt_sz,
                                char *post, int post_sz,
                                char *err, int err_sz,
                                const char *func_name) {
    const char *name = func_name ? func_name : "label";
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
                 "Usage: %s(\"fmt\", arg, ...)", name);
        return 0;
    }
    const char *quote_open = p;

    /* Closing quote - string contents allow no escapes by design,
     * so the next bare '"' ends the literal. */
    const char *quote_close = NULL;
    for (const char *q = quote_open + 1; *q; q++) {
        if (*q == '"') { quote_close = q; break; }
    }
    if (!quote_close) {
        snprintf(err, (size_t)err_sz,
                 "%s: missing closing '\"'", name);
        return 0;
    }

    /* Validate format-string body. Forbidden chars protect the rest
     * of the codebase from having to be string-aware. */
    for (const char *q = quote_open + 1; q < quote_close; q++) {
        if (*q == '\\') {
            snprintf(err, (size_t)err_sz,
                     "%s: backslash escapes not allowed", name);
            return 0;
        }
        if (q[0] == '/' && q + 1 < quote_close && q[1] == '/') {
            snprintf(err, (size_t)err_sz,
                     "%s: '//' not allowed in format string", name);
            return 0;
        }
        if (*q == '(' || *q == ')' || *q == ',') {
            snprintf(err, (size_t)err_sz,
                     "%s: '%c' not allowed in format string",
                     name, *q);
            return 0;
        }
    }

    int fmt_len = (int)(quote_close - (quote_open + 1));
    if (fmt_len >= fmt_sz) {
        snprintf(err, (size_t)err_sz,
                 "%s: format too long (max %d)", name, fmt_sz - 1);
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
                     "Usage: %s(\"fmt\", arg, ...)", name);
            return 0;
        }
        after++;
        while (*after && isspace((unsigned char)*after)) after++;
        int post_len = (int)strlen(after);
        if (post_len >= post_sz) {
            snprintf(err, (size_t)err_sz,
                     "%s: args too long", name);
            return 0;
        }
        memcpy(post, after, (size_t)post_len);
        post[post_len] = '\0';
    }
    return 1;
}

int repl_label_split_args(const char *args,
                          char *fmt, int fmt_sz,
                          char *post, int post_sz,
                          char *err, int err_sz) {
    return repl_label_split_args_named(args, fmt, fmt_sz, post, post_sz, err, err_sz, "label");
}
