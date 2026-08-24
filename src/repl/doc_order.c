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
    case REPL_DOC_ORDER_DISPLAY_REQUIRED:
        return "every .glr scene requires `display() { ... }` around its "
               "camera and body";
    case REPL_DOC_ORDER_DISPLAY_MISPLACED:
        return "`display() {` must appear exactly once after all declarations "
               "and function definitions";
    case REPL_DOC_ORDER_DISPLAY_UNCLOSED:
        return "the explicit `display() {` frame is not closed";
    case REPL_DOC_ORDER_CONTENT_AFTER_DISPLAY:
        return "executable content appears after the explicit display frame";
    default:
        return "document order";
    }
}

static int order_line_equals(const char *line, const char *want) {
    const char *start = line;
    const char *end;
    size_t want_len;

    if (!line || !want)
        return 0;
    while (*start && isspace((unsigned char)*start))
        start++;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    want_len = strlen(want);
    return (size_t)(end - start) == want_len &&
           strncmp(start, want, want_len) == 0;
}

int repl_doc_order_line_is_display_open(const char *line) {
    return order_line_equals(line, "display() {");
}

int repl_doc_order_last_line_was_wrapper(const ReplDocOrder *ord) {
    return ord && ord->last_line_was_wrapper;
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

/* How a top-level line relates to opening a definition block. */
typedef enum {
    ORDER_DEF_NO = 0,
    ORDER_DEF_YES,      /* `name(...) {` or the alias form `name {` */
    ORDER_DEF_PENDING   /* `name(...)` - a definition iff `{` follows */
} OrderDefKind;

/* Purely lexical - the loader rejects a malformed definition on its own
 * terms. The pending case is the hand-formatted split-brace layout:
 *
 *     func0(x)
 *     {
 *
 * which the REPL accepts and normalizes. Classifying it as body code the
 * moment it is seen produces a diagnostic that blames the wrong line - it
 * tells the author a later camera row is out of phase when their real
 * problem is that their function definition was not recognised. A bare
 * `glVertex3f(0, 0, 0)` has the same shape (the trailing `;` is optional in
 * REPL source), so this cannot be decided without seeing the next line. */
static OrderDefKind order_def_kind(const char *p) {
    char last;

    if (order_is_control_head(p))
        return ORDER_DEF_NO;
    if (!(isalpha((unsigned char)*p) || *p == '_'))
        return ORDER_DEF_NO;
    last = order_last_code_char(p);
    if (last == '{')
        return ORDER_DEF_YES;
    if (last == ')')
        return ORDER_DEF_PENDING;
    return ORDER_DEF_NO;
}

/* Classify a top-level line. `*out_pending` reports the undecided
 * split-brace case, which the caller resolves against the next line. */
static ReplDocPhase order_classify(const char *line, int is_camera_row,
                                   int *out_pending) {
    const char *p = line;
    OrderDefKind def;

    *out_pending = 0;
    if (is_camera_row)
        return REPL_DOC_PHASE_CAMERA;
    while (*p && isspace((unsigned char)*p)) p++;
    if (repl_scan_decl_float_prefix(p, NULL))
        return REPL_DOC_PHASE_DECLS;
    def = order_def_kind(p);
    if (def == ORDER_DEF_YES)
        return REPL_DOC_PHASE_FUNCS;
    if (def == ORDER_DEF_PENDING) {
        *out_pending = 1;
        return REPL_DOC_PHASE_FUNCS;   /* provisional; resolved next line */
    }
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

static void order_report_rule(ReplDocOrder *ord, ReplDocOrderRule rule,
                              int line_no, int conflict_line_no) {
    char msg[256];

    ord->violations++;
    if (!ord->sink)
        return;
    if (conflict_line_no > 0)
        snprintf(msg, sizeof(msg), "%s (line %d established the conflict).",
                 repl_doc_order_rule_text(rule), conflict_line_no);
    else
        snprintf(msg, sizeof(msg), "%s.", repl_doc_order_rule_text(rule));
    ord->sink(ord->sink_userdata, rule, line_no, conflict_line_no, msg);
}

/* The first character of a line's code portion, or 0 when there is none. */
static char order_first_code_char(const char *p) {
    int i;

    for (i = 0; p[i]; i++) {
        if (isspace((unsigned char)p[i]))
            continue;
        if (p[i] == '/' && (p[i + 1] == '/' || p[i + 1] == '*'))
            return 0;
        return p[i];
    }
    return 0;
}

/* Advance (or reject against) the phase counter. Returns 1 when the line is
 * in phase. */
static int order_commit(ReplDocOrder *ord, ReplDocPhase phase, int line_no) {
    if (phase < ord->phase) {
        order_report(ord, phase, line_no);
        return 0;
    }
    if (phase > ord->phase) {
        ord->phase                  = phase;
        ord->phase_line[(int)phase] = line_no;
    }
    return 1;
}

int repl_doc_order_offer(ReplDocOrder *ord, const char *line, int line_no,
                         int is_camera_row) {
    ReplDocPhase phase;
    int depth_at_start;
    int executable;
    int pending = 0;
    int ok = 1;

    if (!ord || !line)
        return 1;

    ord->last_line_was_wrapper = 0;

    if (repl_doc_order_line_is_display_open(line)) {
        ord->last_line_was_wrapper = 1;
        if (ord->pending_line > 0 || ord->depth != 0 || ord->in_display ||
            ord->display_open_line > 0 || ord->phase > REPL_DOC_PHASE_FUNCS) {
            order_report_rule(ord, REPL_DOC_ORDER_DISPLAY_MISPLACED, line_no,
                              ord->phase_line[ord->phase]);
            return 0;
        }
        ord->display_open_line = line_no;
        ord->in_display = 1;
        return 1;
    }

    if (ord->in_display && ord->depth == 0 && order_line_equals(line, "}")) {
        if (ord->pending_line > 0) {
            ok = order_commit(ord, REPL_DOC_PHASE_BODY, ord->pending_line);
            ord->pending_line = 0;
        }
        ord->last_line_was_wrapper = 1;
        ord->in_display = 0;
        ord->display_close_line = line_no;
        return ok;
    }

    depth_at_start = ord->depth;
    executable     = repl_line_is_executable(line, ord->in_block_comment);
    ord->depth    += repl_code_brace_delta(line, &ord->in_block_comment);

    /* Comments and blank lines carry no phase: they are skipped entirely,
     * never advance the counter, and are legal anywhere. That single rule is
     * what lets an author document any block without fighting the format. */
    if (!executable)
        return 1;

    if (ord->display_close_line > 0) {
        order_report_rule(ord, REPL_DOC_ORDER_CONTENT_AFTER_DISPLAY, line_no,
                          ord->display_close_line);
        return 0;
    }

    /* Inside a block, every line belongs to whatever opened it; the opener
     * itself was already checked. */
    if (depth_at_start > 0)
        return 1;

    /* Resolve a deferred split-brace header against this line: a `{` here
     * makes the previous line a definition, anything else makes it body. */
    if (ord->pending_line > 0) {
        int opens = order_first_code_char(line) == '{';
        int deferred = ord->pending_line;
        int committed;

        ord->pending_line = 0;
        if (opens && ord->in_display) {
            order_report_rule(ord, REPL_DOC_ORDER_FUNC_LATE, deferred,
                              ord->display_open_line);
            return 0;
        }
        committed = order_commit(ord, opens ? REPL_DOC_PHASE_FUNCS
                                            : REPL_DOC_PHASE_BODY, deferred);
        if (!committed)
            ok = 0;
        if (opens)
            return ok;             /* the brace belongs to that definition */
    }

    phase = order_classify(line, is_camera_row, &pending);
    if (pending) {
        /* Undecided until the next line. A file that ends here is malformed
         * on the loader's own terms, so nothing is lost by not classifying
         * it. This must precede the in-display definition check: an ordinary
         * semicolon-less body call has the same provisional shape. */
        ord->pending_line = line_no;
        return ok;
    }
    if (ord->in_display && phase <= REPL_DOC_PHASE_FUNCS) {
        order_report_rule(ord,
                          phase == REPL_DOC_PHASE_DECLS
                              ? REPL_DOC_ORDER_DECL_LATE
                              : REPL_DOC_ORDER_FUNC_LATE,
                          line_no, ord->display_open_line);
        return 0;
    }
    if (!ord->in_display && ord->display_open_line == 0 &&
        (phase == REPL_DOC_PHASE_CAMERA || phase == REPL_DOC_PHASE_BODY) &&
        !ord->display_required_reported) {
        order_report_rule(ord, REPL_DOC_ORDER_DISPLAY_REQUIRED, line_no,
                          ord->phase_line[REPL_DOC_PHASE_FUNCS]);
        ord->display_required_reported = 1;
        ok = 0;
    }
    if (!order_commit(ord, phase, line_no))
        ok = 0;
    return ok;
}

int repl_doc_order_finish(ReplDocOrder *ord, int eof_line_no) {
    int ok = 1;

    if (!ord)
        return 1;
    if (ord->display_open_line == 0 && !ord->display_required_reported) {
        order_report_rule(ord, REPL_DOC_ORDER_DISPLAY_REQUIRED, eof_line_no,
                          ord->phase_line[REPL_DOC_PHASE_FUNCS]);
        ord->display_required_reported = 1;
        ok = 0;
    }
    if (ord->display_open_line > 0 && ord->display_close_line == 0) {
        order_report_rule(ord, REPL_DOC_ORDER_DISPLAY_UNCLOSED, eof_line_no,
                          ord->display_open_line);
        ok = 0;
    }
    return ok;
}
