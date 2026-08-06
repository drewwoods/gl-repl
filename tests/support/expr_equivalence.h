#ifndef TESTS_SUPPORT_EXPR_EQUIVALENCE_H
#define TESTS_SUPPORT_EXPR_EQUIVALENCE_H

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "repl/eval.h"   /* repl_scan_next_arg_delim, ident predicates */

/* Two REPL spellings of one call, collapsed so text comparisons can tell
 * "different program" from "different characters".
 *
 * Export/import is meaning-preserving, not character-preserving, and there
 * is exactly one place where that is visible in the source text today:
 * builtins with an optional trailing argument. `rand` and `rand2` are the
 * only two (arity_min 1, arity_max 2). The evaluator reads a missing `iter`
 * out of a zero-filled args[], but the exported C helper's signature is
 * fixed, so repl_eval_expr_to_c has to write the default out - and
 * repl_eval_c_expr_to_repl deliberately does not take it back off, because
 * it cannot tell a default that lowering added from one the user wrote.
 * A scene spelling `rand(x)` therefore comes back from an export/import
 * round trip as `rand(x, 0)`.
 *
 * Both spellings are legal in a scene and neither is canonical, so any test
 * that compares REPL source text across an export/import boundary has to
 * collapse them rather than pick a winner. The rule lives here, in one
 * place, so a second such test does not re-derive it - and so the next
 * person to add an optional-argument builtin finds the test-side
 * consequence next to the rule instead of after a mystifying one-line
 * diff. See repl_eval_expr_to_c / repl_eval_c_expr_to_repl in
 * src/repl/eval.h for the export side of the same story.
 *
 * Run this AFTER float-literal canonicalization: that is what turns a
 * `0.0` second argument into the `0` matched below.
 *
 * Returns a malloc'd copy the caller frees, or NULL. */
static inline char *repl_test_collapse_optional_args(const char *src) {
    /* Byte ranges to drop, collected before anything is copied. Two ranges
     * can nest but never partially overlap - an inner call's `, 0` lies
     * inside the outer call's first argument, so it is strictly earlier -
     * which is what lets the copy pass below just test membership. */
    enum { MAX_DROPS = 64 };
    struct { size_t from, to; } drop[MAX_DROPS];
    int n_drop = 0;
    size_t len, i, o;
    int in_comment = 0;
    char *out;

    if (!src) return NULL;
    len = strlen(src);
    out = (char *)malloc(len + 1);
    if (!out) return NULL;

    for (i = 0; i < len; i++) {
        const char *at = src + i;
        int name_len = 0;

        /* A call named in prose is not a call. */
        if (src[i] == '\n') { in_comment = 0; continue; }
        if (in_comment) continue;
        if (at[0] == '/' && at[1] == '/') { in_comment = 1; continue; }

        if (strncmp(at, "rand2", 5) == 0) name_len = 5;
        else if (strncmp(at, "rand", 4) == 0) name_len = 4;
        if (!name_len || at[name_len] != '(') continue;
        if (i > 0 && repl_eval_is_ident_continue((unsigned char)src[i - 1]))
            continue;

        {
            const char *d1 = repl_scan_next_arg_delim(at + name_len + 1);
            const char *a2, *d2;
            if (*d1 != ',') continue;
            a2 = d1 + 1;
            d2 = repl_scan_next_arg_delim(a2);
            while (a2 < d2 && isspace((unsigned char)*a2)) a2++;
            /* Exactly two arguments, the second a bare zero. */
            if (*d2 != ')' || d2 - a2 != 1 || *a2 != '0') continue;
            if (n_drop >= MAX_DROPS) continue;
            drop[n_drop].from = (size_t)(d1 - src);
            drop[n_drop].to = (size_t)(d2 - src);
            n_drop++;
        }
    }

    for (i = 0, o = 0; i < len; i++) {
        int dropped = 0;
        for (int d = 0; d < n_drop; d++) {
            if (i >= drop[d].from && i < drop[d].to) { dropped = 1; break; }
        }
        if (!dropped)
            out[o++] = src[i];
    }
    out[o] = '\0';
    return out;
}

/* Named so a failing comparison can say what it collapsed before deciding
 * the two texts differ - a diff that shows identical-looking lines is the
 * signature of a normalization the reader does not know about. */
#define REPL_TEST_EQUIVALENCE_NOTE \
    "text compared after collapsing rand/rand2's optional argument " \
    "(tests/support/expr_equivalence.h)"

#endif /* TESTS_SUPPORT_EXPR_EQUIVALENCE_H */
