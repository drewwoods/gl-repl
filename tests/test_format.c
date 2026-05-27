/* test_format.c - focused tests for repl_format_reindent_from_parsed(). */
#include "repl/format.h"
#include "support/test_harness.h"

#include <stdio.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_INT(label, got, expected) do { \
    TEST_ASSERT_INT(&g_harness, label, got, expected); \
} while (0)

#define ASSERT_STR(label, got, expected) do { \
    TEST_ASSERT_STR(&g_harness, label, got, expected); \
} while (0)

static void test_reindent_from_parsed(void) {
    printf("test_reindent_from_parsed\n");
    char out[64];

    /* Simple case: parse_command produced 2-space indent */
    repl_format_reindent_from_parsed("  glVertex3f(0, 1, 0);",
                             "  glVertex3f(x, y, z);", out, sizeof(out));
    ASSERT_STR("reindent_from_parsed 2-space", out, "  glVertex3f(x, y, z);");

    /* 8-space parsed (gl command inside tess+begin), raw has 2-space */
    repl_format_reindent_from_parsed("        glVertex3f(0, 1, 0);",
                             "  glVertex3f(x, y, z);", out, sizeof(out));
    ASSERT_STR("reindent_from_parsed 8-space", out, "        glVertex3f(x, y, z);");

    /* 6-space parsed (glu command inside tess+begin), raw has 2-space.
     * This is the key case: gluNormal/gluColor/gluVertex get 6-space from
     * parse_command, and we preserve that. */
    repl_format_reindent_from_parsed("      gluVertex(0, 1, 0);",
                             "  gluVertex(x, y, z);", out, sizeof(out));
    ASSERT_STR("reindent_from_parsed 6-space glu", out, "      gluVertex(x, y, z);");

    /* 0-space parsed (label or similar) */
    repl_format_reindent_from_parsed("loop:",
                             "loop:", out, sizeof(out));
    ASSERT_STR("reindent_from_parsed 0-space", out, "loop:");

    /* raw has no leading whitespace */
    repl_format_reindent_from_parsed("    glColor3f(1, 0, 0);",
                             "glColor3f(r, g, b);", out, sizeof(out));
    ASSERT_STR("reindent_from_parsed raw no indent", out, "    glColor3f(r, g, b);");
}

static void test_reindent_tabs_and_empty_expr(void) {
    printf("test_reindent_tabs_and_empty_expr\n");
    char out[64];

    repl_format_reindent_from_parsed("\t  glVertex3f(0, 1, 0);",
                             " \tglVertex3f(x, y, z);", out, sizeof(out));
    ASSERT_STR("reindent preserves tab+space indent",
               out, "\t  glVertex3f(x, y, z);");

    repl_format_reindent_from_parsed("   ",
                             "  glEnd();", out, sizeof(out));
    ASSERT_STR("reindent whitespace-only parsed source",
               out, "   glEnd();");

    repl_format_reindent_from_parsed("    glColor3f(1, 0, 0);",
                             " \t ", out, sizeof(out));
    ASSERT_STR("reindent empty raw expr after strip", out, "    ");
}

static void test_reindent_truncation(void) {
    printf("test_reindent_truncation\n");
    char out[6];
    char tiny[1] = { 'x' };

    repl_format_reindent_from_parsed("   value",
                             "  abcde", out, sizeof(out));
    ASSERT_STR("reindent truncates after indent+expr budget", out, "   ab");

    repl_format_reindent_from_parsed("        value",
                             "abc", out, 4);
    ASSERT_STR("reindent truncates indent when buffer tiny", out, "   ");

    repl_format_reindent_from_parsed("  x",
                             "  y", tiny, sizeof(tiny));
    ASSERT_INT("reindent size-1 buffer is nul", tiny[0], '\0');
}

static void test_reindent_invalid_size_guard(void) {
    printf("test_reindent_invalid_size_guard\n");
    char out_zero[16] = "unchanged";
    char out_negative[16] = "untouched";

    repl_format_reindent_from_parsed("    glColor3f(1, 0, 0);",
                             "glColor3f(r, g, b);", out_zero, 0);
    ASSERT_STR("reindent size 0 leaves buffer alone", out_zero, "unchanged");

    repl_format_reindent_from_parsed("    glColor3f(1, 0, 0);",
                             "glColor3f(r, g, b);", out_negative, -1);
    ASSERT_STR("reindent negative size leaves buffer alone",
               out_negative, "untouched");
}

int main(void) {
    printf("=== repl_format tests ===\n\n");
    test_reindent_from_parsed();
    test_reindent_tabs_and_empty_expr();
    test_reindent_truncation();
    test_reindent_invalid_size_guard();

    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "repl_format");
}
