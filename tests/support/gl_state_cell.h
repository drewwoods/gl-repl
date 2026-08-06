#ifndef TESTS_SUPPORT_GL_STATE_CELL_H
#define TESTS_SUPPORT_GL_STATE_CELL_H

#include <ctype.h>
#include <stdlib.h>

/* Compare two GL-state report value cells by what they say, not by how they
 * are typeset.
 *
 * The report prints every number in one fixed-width field (four decimals,
 * right-aligned in eight cells - see gl_state_fmt_float in
 * src/repl/gl_state_inspector.c). That is a layout decision: the popup solves
 * its columns from the widest value it holds and re-solves every frame while
 * `t` advances, so a width that tracked the magnitude made the whole table
 * breathe under a running scene. The cost is that the text of a cell is now
 * "(  0.1000,   0.1000,   0.1000,   1.0000)" where a case would rather write
 * "(0.1, 0.1, 0.1, 1)".
 *
 * Rewriting every expectation in the padded form would make dozens of cases
 * harder to read to pin a property none of them is about, and would make the
 * field width a fact restated in dozens of places instead of asserted once.
 * So cases compare through here - numeric runs by value, everything else
 * (enum names, GL_TRUE, the "(T, T, T, T)" masks) character for character -
 * and the field itself is the subject of exactly one test,
 * test_gl_state_value_field_width in tests/test_repl_state.c, which is where
 * a change to the format has to be acknowledged.
 *
 * The tolerance is relative, since four decimals is all the precision the
 * printed form carries in the first place. */
static int gl_state_cell_num_starts(const char *s) {
    if (*s == '-' || *s == '+')
        s++;
    if (*s == '.')
        s++;
    return isdigit((unsigned char)*s);
}

static int gl_state_cell_matches(const char *a, const char *b) {
    if (!a || !b)
        return a == b;
    for (;;) {
        while (*a == ' ') a++;
        while (*b == ' ') b++;
        if (!*a || !*b)
            break;
        if (gl_state_cell_num_starts(a) && gl_state_cell_num_starts(b)) {
            char *end_a = NULL;
            char *end_b = NULL;
            double va = strtod(a, &end_a);
            double vb = strtod(b, &end_b);
            double diff = va - vb;
            double mag = va < 0.0 ? -va : va;
            if (diff < 0.0) diff = -diff;
            if (diff > 5.0e-5 * (1.0 + mag))
                return 0;
            if (end_a == a || end_b == b)
                return 0;
            a = end_a;
            b = end_b;
            continue;
        }
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    return *a == '\0' && *b == '\0';
}

#endif /* TESTS_SUPPORT_GL_STATE_CELL_H */
