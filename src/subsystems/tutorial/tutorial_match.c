#include "subsystems/tutorial/tutorial_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

void tutorial_set_expected_message(TutorialMatchResult *result,
                                          TutorialMatchKind kind,
                                          const char *expected) {
    if (!result)
        return;

    result->kind = kind;
    if (!expected)
        expected = "";
    snprintf(result->message, sizeof(result->message), "expected: %s", expected);
}

void tutorial_normalize_text(const char *src, char *dst, size_t dst_size) {
    size_t out = 0;
    const char *start = src ? src : "";
    const char *end = start + strlen(start);

    while (*start && isspace((unsigned char)*start))
        start++;
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    if (end > start && end[-1] == ';') {
        end--;
        while (end > start && isspace((unsigned char)end[-1]))
            end--;
    }

    if (dst_size == 0)
        return;

    while (start < end && out + 1 < dst_size) {
        unsigned char ch = (unsigned char)*start++;
        if (isspace(ch))
            continue;
        dst[out++] = (char)ch;
    }
    dst[out] = '\0';
}

TutorialMatchResult tutorial_match(const char *expected, const char *got) {
    TutorialMatchResult result = {
        .kind = TUT_MATCH_OK,
        .message = "",
    };
    char normalized_expected[MAX_LINE_LEN];
    char normalized_got[MAX_LINE_LEN];

    tutorial_normalize_text(expected, normalized_expected,
                            sizeof(normalized_expected));
    tutorial_normalize_text(got, normalized_got, sizeof(normalized_got));

    if (normalized_got[0] == '\0') {
        tutorial_set_expected_message(&result, TUT_MISMATCH_EMPTY, expected);
        return result;
    }
    if (strcmp(normalized_expected, normalized_got) == 0)
        return result;

    tutorial_set_expected_message(&result, TUT_MISMATCH_SHAPE, expected);
    return result;
}

int tutorial_shadow_suffix(const char *input, char *out, size_t out_size) {
    const char *expected;
    char synth_expected[REPL_PREDEF_NAME_MAX + 32];

    if (out && out_size > 0)
        out[0] = '\0';
    if (!out || out_size == 0)
        return 0;
    if (!tutorial_active())
        return 0;

    expected = tutorial_current_expected_text();
    if (!expected) {
        /* REQUIRE_VAR steps carry no fixed expected text — the user can
         * satisfy them by typing `name = expr;` OR by dragging the
         * variable-panel slider. Synthesize the canonical `name = target`
         * here so the autocomplete shadow text passively teaches the
         * typing path; the slider remains a parallel option the prompt
         * (instruction comment + status hint) advertises. */
        TutorialRuntimeState state = tutorial_state_view();
        if (repl_tutorial_step_kind(state.tutorial_idx, state.step) !=
            TUTORIAL_STEP_KIND_REQUIRE_VAR)
            return 0;
        const char *name = repl_tutorial_step_var_name(state.tutorial_idx, state.step);
        if (!name || !name[0])
            return 0;
        float target = repl_tutorial_step_var_target(state.tutorial_idx, state.step);
        snprintf(synth_expected, sizeof synth_expected, "%s = %g",
                 name, (double)target);
        expected = synth_expected;
    }

    const char *inp_start = input ? input : "";
    const char *inp_end = inp_start + strlen(inp_start);
    // Trim leading whitespace from input
    while (*inp_start && isspace((unsigned char)*inp_start))
        inp_start++;
    // Trim trailing whitespace and trailing semicolon from input
    while (inp_end > inp_start && isspace((unsigned char)inp_end[-1]))
        inp_end--;
    if (inp_end > inp_start && inp_end[-1] == ';') {
        inp_end--;
        while (inp_end > inp_start && isspace((unsigned char)inp_end[-1]))
            inp_end--;
    }

    const char *exp_start = expected;
    const char *exp_end = exp_start + strlen(exp_start);
    // Trim leading whitespace from expected
    while (*exp_start && isspace((unsigned char)*exp_start))
        exp_start++;
    // Trim trailing whitespace and trailing semicolon from expected
    while (exp_end > exp_start && isspace((unsigned char)exp_end[-1]))
        exp_end--;
    if (exp_end > exp_start && exp_end[-1] == ';') {
        exp_end--;
        while (exp_end > exp_start && isspace((unsigned char)exp_end[-1]))
            exp_end--;
    }

    const char *p_inp = inp_start;
    const char *p_exp = exp_start;

    while (p_inp < inp_end) {
        // Skip whitespace in input
        while (p_inp < inp_end && isspace((unsigned char)*p_inp))
            p_inp++;
        // Skip whitespace in expected
        while (p_exp < exp_end && isspace((unsigned char)*p_exp))
            p_exp++;

        if (p_inp == inp_end)
            break;

        if (p_exp == exp_end) {
            // input has more non-whitespace chars than expected
            return 0;
        }

        if (*p_inp != *p_exp) {
            // Mismatch
            return 0;
        }

        p_inp++;
        p_exp++;
    }

    // If we matched the normalized prefix, the shadow suffix is the remainder of expected.
    // Copy from p_exp to the very end of expected to include original trailing whitespace/semicolons.
    snprintf(out, out_size, "%s", p_exp);
    return 1;
}
