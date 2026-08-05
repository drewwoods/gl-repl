/*
 * src/repl/doc_order.c - The canonical `.glr` document order, enforced.
 *
 * See doc_order.h for the phase table and why the checker is `.glr`-scoped.
 */
#include "repl/doc_order.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "repl/camera_header.h"   /* repl_code_brace_delta, executability */
#include "repl/text_helpers.h"    /* repl_scan_decl_float_prefix */

void repl_doc_order_init(ReplDocOrder *ord) {
    if (!ord)
        return;
    memset(ord, 0, sizeof(*ord));
    ord->phase       = REPL_DOC_PHASE_NONE;
}

void repl_doc_order_set_sink(ReplDocOrder *ord, ReplDocOrderSink sink,
                             void *userdata) {
    if (!ord)
        return;
    ord->sink          = sink;
    ord->sink_userdata = userdata;
}

const char *repl_doc_order_rule_text(ReplDocOrderRule rule) {
    switch (rule) {
    case REPL_DOC_ORDER_DECL_LATE:
        return "variable declaration after later content. "
               "Order is: declarations, then function definitions, "
               "then camera and body";
    case REPL_DOC_ORDER_FUNC_LATE:
        return "function definition after body code. "
               "Move all function definitions above the body";
    case REPL_DOC_ORDER_CAMERA_LATE:
        return "camera row after body code. User geometry begins only once "
               "every camera row is behind it";
    default:
        return "document order";
    }
}

/* `for`, `if` and `while` open blocks that are body code, not definitions.
 * Everything else that opens a brace at top level and looks like a call
 * header is a function definition. */
static int order_is_control_head(const char *p) {
    static const char *const k_heads[] = { "for", "if", "while", "else" };
    size_t i;

    for (i = 0; i < sizeof(k_heads) / sizeof(k_heads[0]); i++) {
        size_t len = strlen(k_heads[i]);
        if (strncmp(p, k_heads[i], len) == 0 &&
            !(isalnum((unsigned char)p[len]) || p[len] == '_'))
            return 1;
    }
    return 0;
}

/* The last character of a line's code portion, or 0 when there is none.
 * `glFogfv(GL_FOG_COLOR, (GLfloat[]){0.05, 1});` contains a brace and is
 * emphatically not a definition, so "opens a block" has to mean the code
 * *ends* with the brace, not that a brace appears somewhere in it. */
static char order_last_code_char(const char *p) {
    char last = 0;
    int in_str = 0, in_chr = 0;
    int i;

    for (i = 0; p[i]; i++) {
        char c = p[i];
        if (in_str || in_chr) {
            if (c == '\\' && p[i + 1]) { i++; continue; }
            if (in_str && c == '"')  in_str = 0;
            if (in_chr && c == '\'') in_chr = 0;
            last = c;
            continue;
        }
        if (c == '/' && (p[i + 1] == '/' || p[i + 1] == '*'))
            break;
        if (c == '"')  in_str = 1;
        if (c == '\'') in_chr = 1;
        if (!isspace((unsigned char)c))
            last = c;
    }
    return last;
}

/* A definition header: an identifier, and code that ends by opening a block.
 * `name {` (the alias form) and `name(...) {` both count. Purely lexical -
 * the loader will reject a malformed one on its own terms. */
static int order_is_func_def(const char *p) {
    if (order_is_control_head(p))
        return 0;
    if (!(isalpha((unsigned char)*p) || *p == '_'))
        return 0;
    return order_last_code_char(p) == '{';
}

static ReplDocPhase order_classify(const char *line, int is_camera_row) {
    const char *p = line;

    if (is_camera_row)
        return REPL_DOC_PHASE_CAMERA;
    while (*p && isspace((unsigned char)*p)) p++;
    if (repl_scan_decl_float_prefix(p, NULL))
        return REPL_DOC_PHASE_DECLS;
    if (order_is_func_def(p))
        return REPL_DOC_PHASE_FUNCS;
    return REPL_DOC_PHASE_BODY;
}

static ReplDocOrderRule order_rule_for(ReplDocPhase phase) {
    switch (phase) {
    case REPL_DOC_PHASE_DECLS:  return REPL_DOC_ORDER_DECL_LATE;
    case REPL_DOC_PHASE_FUNCS:  return REPL_DOC_ORDER_FUNC_LATE;
    default:                    return REPL_DOC_ORDER_CAMERA_LATE;
    }
}

static void order_report(ReplDocOrder *ord, ReplDocPhase phase, int line_no) {
    ReplDocOrderRule rule = order_rule_for(phase);
    int conflict = ord->phase_line[ord->phase];
    char msg[256];

    ord->violations++;
    if (!ord->sink)
        return;
    snprintf(msg, sizeof(msg), "%s (line %d established the current phase).",
             repl_doc_order_rule_text(rule), conflict);
    ord->sink(ord->sink_userdata, rule, line_no, conflict, msg);
}

int repl_doc_order_offer(ReplDocOrder *ord, const char *line, int line_no,
                         int is_camera_row) {
    ReplDocPhase phase;
    int depth_at_start;
    int executable;
    int ok = 1;

    if (!ord || !line)
        return 1;

    depth_at_start = ord->depth;
    executable     = repl_line_is_executable(line, ord->in_block_comment);
    ord->depth    += repl_code_brace_delta(line, &ord->in_block_comment);

    /* Comments and blank lines carry no phase: they are skipped entirely,
     * never advance the counter, and are legal anywhere. That single rule is
     * what lets an author document any block without fighting the format. */
    if (!executable)
        return 1;

    /* Inside a block, every line belongs to whatever opened it; the opener
     * itself was already checked. */
    if (depth_at_start > 0)
        return 1;

    phase = order_classify(line, is_camera_row);

    if (phase < ord->phase) {
        /* The one tolerated edge: functions may follow a camera block that
         * was itself entered straight from the declarations. */
        if (phase == REPL_DOC_PHASE_FUNCS &&
            ord->phase == REPL_DOC_PHASE_CAMERA && ord->camera_from_decls) {
            /* in phase - do not lower the counter */
        } else {
            order_report(ord, phase, line_no);
            ok = 0;
        }
    } else if (phase > ord->phase) {
        if (phase == REPL_DOC_PHASE_CAMERA &&
            ord->phase <= REPL_DOC_PHASE_DECLS)
            ord->camera_from_decls = 1;
        ord->phase                   = phase;
        ord->phase_line[(int)phase]  = line_no;
    }
    return ok;
}
