/*
 * test_repl_code_panel_syntax.c - per-kind argument syntax classifier.
 *
 * Drives ui_repl_code_panel_classify_syntax() directly with representative
 * REPL display lines and asserts the (text, kind) of each emitted span, plus
 * the class-nudge color model. Comment suppression is category-driven, so it
 * is checked via repl_cmd_type_category() rather than a rendered row.
 */
#include "repl/command_spec.h"
#include "ui/repl_code_panel.h"
#include "support/test_harness.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(name, cond) TEST_ASSERT_TRUE(&g_harness, name, cond)

#define MAX_SPANS 32

/* Exact whole-token lookup: return the first span whose text equals tok.
 * Safe because spans are only emitted for argument tokens (never for the
 * keyword / function-call name), so our test lines have no ambiguity. */
static const ReplSyntaxSpan *find_tok(const char *text,
                                      const ReplSyntaxSpan *spans, int n,
                                      const char *tok) {
    int len = (int)strlen(tok);

    for (int i = 0; i < n; i++) {
        if (spans[i].len == len &&
            strncmp(text + spans[i].start, tok, (size_t)len) == 0)
            return &spans[i];
    }
    return NULL;
}

static void check_line(const char *label, const char *text,
                       const char *tok, ReplSyntaxKind expect_kind) {
    ReplSyntaxSpan spans[MAX_SPANS];
    int n = ui_repl_code_panel_classify_syntax(text, spans, MAX_SPANS);
    const ReplSyntaxSpan *s = find_tok(text, spans, n, tok);
    char name[160];

    snprintf(name, sizeof(name), "%s: '%s' is kind %d", label, tok,
             (int)expect_kind);
    ASSERT_TRUE(name, s != NULL && s->kind == expect_kind);
}

static void check_no_span(const char *label, const char *text,
                          const char *tok) {
    ReplSyntaxSpan spans[MAX_SPANS];
    int n = ui_repl_code_panel_classify_syntax(text, spans, MAX_SPANS);
    char name[160];

    snprintf(name, sizeof(name), "%s: '%s' has no span", label, tok);
    ASSERT_TRUE(name, find_tok(text, spans, n, tok) == NULL);
}

/* WCAG relative luminance + contrast ratio. */
static float srgb_lin(float c) {
    return c <= 0.03928f ? c / 12.92f
                         : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float rel_lum(const float rgb[3]) {
    return 0.2126f * srgb_lin(rgb[0]) +
           0.7152f * srgb_lin(rgb[1]) +
           0.0722f * srgb_lin(rgb[2]);
}

static float contrast_ratio(const float a[3], const float b[3]) {
    float la = rel_lum(a) + 0.05f;
    float lb = rel_lum(b) + 0.05f;
    return la > lb ? la / lb : lb / la;
}

int main(void) {
    /* glVertex3f(x + sin(t), y*2, z) */
    {
        const char *t = "glVertex3f(x + sin(t), y*2, z)";
        check_no_span("vertex", t, "glVertex3f");
        check_no_span("vertex", t, "sin");           /* fn call */
        check_line("vertex", t, "x", REPL_SYNTAX_VARIABLE);
        check_line("vertex", t, "t", REPL_SYNTAX_VARIABLE);
        check_line("vertex", t, "y", REPL_SYNTAX_VARIABLE);
        check_line("vertex", t, "2", REPL_SYNTAX_LITERAL);
        check_line("vertex", t, "z", REPL_SYNTAX_VARIABLE);
    }

    /* for(i, 0, 100, 2) { */
    {
        const char *t = "for(i, 0, 100, 2) {";
        check_no_span("for", t, "for");
        check_line("for", t, "i", REPL_SYNTAX_VARIABLE);
        check_line("for", t, "0", REPL_SYNTAX_LITERAL);
        check_line("for", t, "100", REPL_SYNTAX_LITERAL);
        check_line("for", t, "2", REPL_SYNTAX_LITERAL);
    }

    /* glEnable(GL_BLEND) */
    {
        const char *t = "glEnable(GL_BLEND)";
        check_no_span("enable", t, "glEnable");
        check_line("enable", t, "GL_BLEND", REPL_SYNTAX_CONSTANT);
    }

    /* glRotatef(t, 0, 1, 0) with PI/TAU constants */
    {
        const char *t = "glScalef(PI, TAU, 1.5)";
        check_line("const", t, "PI", REPL_SYNTAX_CONSTANT);
        check_line("const", t, "TAU", REPL_SYNTAX_CONSTANT);
        check_line("const", t, "1.5", REPL_SYNTAX_LITERAL);
    }

    /* label("a=%f", x): the whole quoted run is one literal */
    {
        const char *t = "label(\"a=%f\", x)";
        check_no_span("label", t, "label");
        check_line("label", t, "\"a=%f\"", REPL_SYNTAX_LITERAL);
        check_line("label", t, "x", REPL_SYNTAX_VARIABLE);
    }

    /* Scratch array: A is a variable, indices/rhs are literals */
    {
        const char *t = "A[0] = 1;";
        check_line("scratch", t, "A", REPL_SYNTAX_VARIABLE);
        check_line("scratch", t, "0", REPL_SYNTAX_LITERAL);
        check_line("scratch", t, "1", REPL_SYNTAX_LITERAL);
    }

    /* float decl: 'float' keyword is structural, names are variables */
    {
        const char *t = "float radius, k;";
        check_no_span("decl", t, "float");
        check_line("decl", t, "radius", REPL_SYNTAX_VARIABLE);
        check_line("decl", t, "k", REPL_SYNTAX_VARIABLE);
    }

    /* Comment suppression is category-driven (the wiring skips CMD_COMMENT
     * rows entirely so the whole line keeps the comment color). */
    ASSERT_TRUE("comment category maps to CMD_CAT_COMMENT",
                repl_cmd_type_category(CMD_COMMENT) == CMD_CAT_COMMENT);

    /* Color model: each kind nudged 22% toward the class color, and the
     * three kinds stay distinct for a given class. */
    {
        float lit[3];
        float con[3];
        float var[3];

        ui_repl_code_panel_syntax_kind_rgb(REPL_SYNTAX_LITERAL,
                                           CMD_CAT_DEFAULT, lit);
        ui_repl_code_panel_syntax_kind_rgb(REPL_SYNTAX_CONSTANT,
                                           CMD_CAT_DEFAULT, con);
        ui_repl_code_panel_syntax_kind_rgb(REPL_SYNTAX_VARIABLE,
                                           CMD_CAT_DEFAULT, var);

        /* literal base (0.93,0.66,0.40) nudged 22% toward (0.70,0.70,0.70) */
        ASSERT_TRUE("literal r nudged",
                    fabsf(lit[0] - (0.93f + (0.70f - 0.93f) * 0.22f)) < 1e-4f);
        ASSERT_TRUE("kinds distinct (lit vs var)",
                    fabsf(lit[0] - var[0]) > 0.05f ||
                    fabsf(lit[2] - var[2]) > 0.05f);
        ASSERT_TRUE("kinds distinct (con vs var)",
                    fabsf(con[0] - var[0]) > 0.05f ||
                    fabsf(con[1] - var[1]) > 0.05f);
    }

    /* Optional contrast guard: every (kind x class) final color must stay
     * legible against the code-panel background (src/ui/text_panel.c:571,
     * glColor4f(0.06, 0.06, 0.10, ...)). Threshold 3.0 ~= WCAG AA for
     * large text; guards future palette / nudge tweaks. */
    {
        const float bg[3] = { 0.06f, 0.06f, 0.10f };

        for (int k = 0; k < REPL_SYNTAX_KIND_COUNT; k++) {
            for (int c = 0; c < CMD_CAT_COUNT; c++) {
                float rgb[3];
                float ratio;
                char name[96];

                ui_repl_code_panel_syntax_kind_rgb((ReplSyntaxKind)k,
                                                   (CmdSyntaxCategory)c, rgb);
                ratio = contrast_ratio(rgb, bg);
                snprintf(name, sizeof(name),
                         "contrast kind %d class %d >= 3.0 (%.2f)",
                         k, c, ratio);
                ASSERT_TRUE(name, ratio >= 3.0f);
            }
        }
    }

    return test_harness_report(&g_harness, "repl_code_panel_syntax");
}
