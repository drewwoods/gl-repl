/*
 * test_format.c — Tests for the command indentation logic in cmd_format.c
 *
 * Build:
 *   gcc -Wall -std=c2x -o test_format test_format.c cmd_format.c -lm
 *
 * Run:
 *   ./test_format
 */
#include "cmd_format.h"

#include <stdio.h>
#include <string.h>

/* ---- Test harness ------------------------------------------------------ */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define ASSERT_INT(label, got, expected) do { \
    g_tests_run++; \
    if ((got) == (expected)) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        printf("  FAIL [%s]: got %d, expected %d\n", (label), (got), (expected)); \
    } \
} while (0)

#define ASSERT_STR(label, got, expected) do { \
    g_tests_run++; \
    if (strcmp((got), (expected)) == 0) { \
        g_tests_passed++; \
    } else { \
        g_tests_failed++; \
        printf("  FAIL [%s]: got \"%s\" (%d spaces), expected \"%s\" (%d spaces)\n", \
               (label), (got), (int)strlen(got), (expected), (int)strlen(expected)); \
    } \
} while (0)

/* Helper: run fmt_indent and return a static buffer with the result */
static const char *indent_str(const FmtCmd *cmds, int n) {
    static char buf[64];
    fmt_indent(cmds, n, buf, sizeof(buf));
    return buf;
}
static const char *end_str(const FmtCmd *cmds, int n) {
    static char buf[64];
    fmt_end_indent(cmds, n, buf, sizeof(buf));
    return buf;
}
static const char *tess_end_str(const FmtCmd *cmds, int n) {
    static char buf[64];
    fmt_tess_end_indent(cmds, n, buf, sizeof(buf));
    return buf;
}

/* ---- Helpers to build FmtCmd sequences --------------------------------- */

static FmtCmd V(FmtType t) { return (FmtCmd){ t, 1 }; }
static FmtCmd INV(FmtType t) { return (FmtCmd){ t, 0 }; }  /* invalid/deleted */

/* ---- Test cases -------------------------------------------------------- */

static void test_empty_sequence(void) {
    printf("test_empty_sequence\n");
    FmtCmd seq[] = {};
    ASSERT_INT("tess_depth empty", fmt_tess_depth(seq, 0), 0);
    ASSERT_INT("begin_depth empty", fmt_begin_depth(seq, 0), 0);
    ASSERT_STR("fmt_indent empty", indent_str(seq, 0), "  ");
}

static void test_after_tess_polygon(void) {
    printf("test_after_tess_polygon\n");
    /* Sequence: gluBegin(GLU_POLYGON) */
    FmtCmd seq[] = { V(FMT_TESS_BEGIN_POLYGON) };
    int n = 1;
    ASSERT_INT("tess_depth after polygon", fmt_tess_depth(seq, n), 1);
    ASSERT_INT("begin_depth after polygon", fmt_begin_depth(seq, n), 0);
    ASSERT_STR("fmt_indent after polygon", indent_str(seq, n), "    ");
}

static void test_after_tess_contour(void) {
    printf("test_after_tess_contour\n");
    /* Sequence: gluBegin(GLU_POLYGON); gluBegin(GLU_CONTOUR) */
    FmtCmd seq[] = {
        V(FMT_TESS_BEGIN_POLYGON),
        V(FMT_TESS_BEGIN_CONTOUR),
    };
    int n = 2;
    ASSERT_INT("tess_depth after contour", fmt_tess_depth(seq, n), 2);
    ASSERT_INT("begin_depth after contour", fmt_begin_depth(seq, n), 0);
    ASSERT_STR("fmt_indent after contour", indent_str(seq, n), "      ");  /* 6 spaces */
}

static void test_after_gl_begin_inside_tess(void) {
    printf("test_after_gl_begin_inside_tess\n");
    /* Sequence: gluBegin(GLU_POLYGON); gluBegin(GLU_CONTOUR); glBegin(GL_TRIANGLES) */
    FmtCmd seq[] = {
        V(FMT_TESS_BEGIN_POLYGON),
        V(FMT_TESS_BEGIN_CONTOUR),
        V(FMT_GL_BEGIN),
    };
    int n = 3;
    ASSERT_INT("tess_depth inside glBegin", fmt_tess_depth(seq, n), 2);
    ASSERT_INT("begin_depth inside glBegin", fmt_begin_depth(seq, n), 1);
    ASSERT_STR("fmt_indent inside glBegin", indent_str(seq, n), "        ");  /* 8 spaces */
}

static void test_gl_begin_indent(void) {
    printf("test_gl_begin_indent\n");
    /* glBegin itself sits at tess=2, begin=0 (it hasn't been added yet) */
    FmtCmd seq[] = {
        V(FMT_TESS_BEGIN_POLYGON),
        V(FMT_TESS_BEGIN_CONTOUR),
        /* glBegin not yet added; computing indent at this position */
    };
    int n = 2;
    /* glBegin uses fmt_indent (begin not open yet) → 2+2*2+2*0 = 6 */
    ASSERT_STR("glBegin indent at tess=2", indent_str(seq, n), "      ");  /* 6 spaces */
}

static void test_gl_end_indent(void) {
    printf("test_gl_end_indent\n");
    /* glEnd is called when inside tess=2, begin=1.
     * It should align with its matching glBegin at 6 spaces. */
    FmtCmd seq[] = {
        V(FMT_TESS_BEGIN_POLYGON),
        V(FMT_TESS_BEGIN_CONTOUR),
        V(FMT_GL_BEGIN),
    };
    int n = 3;
    /* fmt_end_indent: 2 + 2*tess = 2+4 = 6 (no begin depth) */
    ASSERT_STR("glEnd indent at tess=2 begin=1", end_str(seq, n), "      ");  /* 6 spaces */
}

static void test_glu_end_contour_indent(void) {
    printf("test_glu_end_contour_indent\n");
    /* gluEnd() closes the contour. tess=2 at this point. */
    FmtCmd seq[] = {
        V(FMT_TESS_BEGIN_POLYGON),
        V(FMT_TESS_BEGIN_CONTOUR),
    };
    int n = 2;
    /* fmt_tess_end_indent: 2 + 2*(tess-1) = 2+2*1 = 4 */
    ASSERT_STR("gluEnd(contour) indent", tess_end_str(seq, n), "    ");  /* 4 spaces */
}

static void test_glu_end_polygon_indent(void) {
    printf("test_glu_end_polygon_indent\n");
    /* gluEnd() closes the polygon. tess=1 at this point (contour already closed). */
    FmtCmd seq[] = {
        V(FMT_TESS_BEGIN_POLYGON),
    };
    int n = 1;
    /* fmt_tess_end_indent: 2 + 2*(1-1) = 2 */
    ASSERT_STR("gluEnd(polygon) indent", tess_end_str(seq, n), "  ");  /* 2 spaces */
}

static void test_full_tess_scenario(void) {
    printf("test_full_tess_scenario — complete gluBegin/glBegin nesting\n");
    /*
     * Desired output from user spec:
     *   gluBegin(GLU_POLYGON);        ← 2 spaces  (tess=0)
     *     gluBegin(GLU_CONTOUR);      ← 4 spaces  (tess=1)
     *       gluNormal(0, 0, 1);       ← 6 spaces  (tess=2, begin=0)
     *       gluVertex(-1, -1, 0);     ← 6 spaces  (tess=2, begin=0)
     *       glBegin(GL_TRIANGLES);    ← 6 spaces  (tess=2, begin=0, not yet opened)
     *         glColor(1, 0.5, 0, 1);  ← 8 spaces  (tess=2, begin=1)
     *         glVertex(1, -1, 0);     ← 8 spaces  (tess=2, begin=1)
     *       glEnd();                  ← 6 spaces  (tess=2, fmt_end_indent)
     *     gluEnd();                   ← 4 spaces  (tess=2, fmt_tess_end_indent → tess-1=1)
     *   gluEnd();                     ← 2 spaces  (tess=1, fmt_tess_end_indent → tess-1=0)
     */

    /* gluBegin(GLU_POLYGON) indent: sequence empty */
    {
        FmtCmd seq[] = {};
        ASSERT_STR("gluBegin(POLYGON)", indent_str(seq, 0), "  ");
    }
    /* gluBegin(GLU_CONTOUR) indent: after POLYGON */
    {
        FmtCmd seq[] = { V(FMT_TESS_BEGIN_POLYGON) };
        ASSERT_STR("gluBegin(CONTOUR)", indent_str(seq, 1), "    ");
    }
    /* gluNormal: after POLYGON + CONTOUR */
    {
        FmtCmd seq[] = { V(FMT_TESS_BEGIN_POLYGON), V(FMT_TESS_BEGIN_CONTOUR) };
        ASSERT_STR("gluNormal", indent_str(seq, 2), "      ");
    }
    /* glBegin: after POLYGON + CONTOUR (begin=0, not yet open) */
    {
        FmtCmd seq[] = { V(FMT_TESS_BEGIN_POLYGON), V(FMT_TESS_BEGIN_CONTOUR) };
        ASSERT_STR("glBegin", indent_str(seq, 2), "      ");
    }
    /* glColor/glVertex: after POLYGON + CONTOUR + GL_BEGIN */
    {
        FmtCmd seq[] = {
            V(FMT_TESS_BEGIN_POLYGON), V(FMT_TESS_BEGIN_CONTOUR), V(FMT_GL_BEGIN),
        };
        ASSERT_STR("glColor inside glBegin", indent_str(seq, 3), "        ");
        ASSERT_STR("glVertex inside glBegin", indent_str(seq, 3), "        ");
    }
    /* glEnd: after POLYGON + CONTOUR + GL_BEGIN */
    {
        FmtCmd seq[] = {
            V(FMT_TESS_BEGIN_POLYGON), V(FMT_TESS_BEGIN_CONTOUR), V(FMT_GL_BEGIN),
        };
        ASSERT_STR("glEnd", end_str(seq, 3), "      ");
    }
    /* gluEnd(contour): after POLYGON + CONTOUR */
    {
        FmtCmd seq[] = { V(FMT_TESS_BEGIN_POLYGON), V(FMT_TESS_BEGIN_CONTOUR) };
        ASSERT_STR("gluEnd(CONTOUR)", tess_end_str(seq, 2), "    ");
    }
    /* gluEnd(polygon): after POLYGON */
    {
        FmtCmd seq[] = { V(FMT_TESS_BEGIN_POLYGON) };
        ASSERT_STR("gluEnd(POLYGON)", tess_end_str(seq, 1), "  ");
    }
}

static void test_invalid_cmds_skipped(void) {
    printf("test_invalid_cmds_skipped\n");
    /* Invalid commands should not affect depth counting */
    FmtCmd seq[] = {
        V(FMT_TESS_BEGIN_POLYGON),
        INV(FMT_TESS_BEGIN_CONTOUR),  /* deleted — should be ignored */
        V(FMT_TESS_BEGIN_CONTOUR),
    };
    int n = 3;
    ASSERT_INT("tess_depth with invalid entry", fmt_tess_depth(seq, n), 2);
    ASSERT_STR("fmt_indent with invalid entry", indent_str(seq, n), "      ");
}

static void test_underflow_clamps_to_zero(void) {
    printf("test_underflow_clamps_to_zero\n");
    /* More end-markers than begin-markers should not go below 0 */
    FmtCmd seq[] = {
        V(FMT_TESS_END),
        V(FMT_TESS_END),
    };
    ASSERT_INT("tess_depth underflow", fmt_tess_depth(seq, 2), 0);
    ASSERT_STR("fmt_indent at underflow", indent_str(seq, 2), "  ");
}

static void test_no_tess_plain_begin(void) {
    printf("test_no_tess_plain_begin\n");
    /* Outside any tess scope: glBegin/glEnd indent as before */
    {
        FmtCmd seq[] = {};
        ASSERT_STR("glBegin no tess", indent_str(seq, 0), "  ");
    }
    {
        FmtCmd seq[] = { V(FMT_GL_BEGIN) };
        ASSERT_STR("vertex inside glBegin no tess", indent_str(seq, 1), "    ");
        ASSERT_STR("glEnd no tess", end_str(seq, 1), "  ");
    }
}

/* ---- fmt_reindent_expr tests ------------------------------------------- */

static void test_reindent_no_context(void) {
    printf("test_reindent_no_context\n");
    /* Outside any scope: should get 2-space indent */
    FmtCmd seq[] = {};
    char out[64];
    fmt_reindent_expr(seq, 0, "  glVertex3f(-1, 0, 0);", out, sizeof(out));
    ASSERT_STR("reindent no context", out, "  glVertex3f(-1, 0, 0);");
}

static void test_reindent_inside_gl_begin(void) {
    printf("test_reindent_inside_gl_begin\n");
    /* Inside a plain glBegin block: should get 4-space indent */
    FmtCmd seq[] = { V(FMT_GL_BEGIN) };
    char out[64];
    /* raw_expr has wrong 2-space indent (saved from file) */
    fmt_reindent_expr(seq, 1, "  glVertex3f(-1, 0, 0);", out, sizeof(out));
    ASSERT_STR("reindent inside glBegin", out, "    glVertex3f(-1, 0, 0);");
}

static void test_reindent_tess_contour_no_begin(void) {
    printf("test_reindent_tess_contour_no_begin\n");
    /* Inside tess contour (tess=2) but outside glBegin: 6-space */
    FmtCmd seq[] = { V(FMT_TESS_BEGIN_POLYGON), V(FMT_TESS_BEGIN_CONTOUR) };
    char out[64];
    fmt_reindent_expr(seq, 2, "  gluVertex(-1.2, -0.45, 0);", out, sizeof(out));
    ASSERT_STR("reindent tess=2 begin=0", out, "      gluVertex(-1.2, -0.45, 0);");
}

static void test_reindent_tess_contour_inside_begin(void) {
    printf("test_reindent_tess_contour_inside_begin\n");
    /* Inside tess contour (tess=2) AND inside glBegin: 8-space.
     * This is the key case from output.c where has_vars lines like
     * "glVertex3f(-1.2, -0.45, z)" were getting 2-space from the saved
     * file instead of the correct 8-space. */
    FmtCmd seq[] = {
        V(FMT_TESS_BEGIN_POLYGON),
        V(FMT_TESS_BEGIN_CONTOUR),
        V(FMT_GL_BEGIN),
    };
    char out[64];
    /* Simulate load_from_file: raw line has old 2-space indent from file */
    fmt_reindent_expr(seq, 3, "  glVertex3f(-1.2, -0.45, z);", out, sizeof(out));
    ASSERT_STR("reindent has_vars tess=2 begin=1", out,
               "        glVertex3f(-1.2, -0.45, z);");
}

static void test_reindent_strips_any_leading_whitespace(void) {
    printf("test_reindent_strips_any_leading_whitespace\n");
    /* Verify that various amounts of leading whitespace are stripped */
    FmtCmd seq[] = { V(FMT_TESS_BEGIN_POLYGON) };
    char out[64];

    /* 0 spaces */
    fmt_reindent_expr(seq, 1, "glColor3f(1, 0, 0);", out, sizeof(out));
    ASSERT_STR("reindent strip 0 spaces", out, "    glColor3f(1, 0, 0);");

    /* 6 spaces (over-indented) */
    fmt_reindent_expr(seq, 1, "      glColor3f(1, 0, 0);", out, sizeof(out));
    ASSERT_STR("reindent strip 6 spaces", out, "    glColor3f(1, 0, 0);");

    /* mixed tabs and spaces */
    fmt_reindent_expr(seq, 1, "\t  glColor3f(1, 0, 0);", out, sizeof(out));
    ASSERT_STR("reindent strip tab+spaces", out, "    glColor3f(1, 0, 0);");
}

static void test_reindent_for_loop_body(void) {
    printf("test_reindent_for_loop_body\n");
    /* For-loop body commands also use fmt_reindent_expr.
     * Even though they aren't has_vars per se, they always have their
     * source overwritten with the raw REPL expression text. */
    FmtCmd seq[] = { V(FMT_TESS_BEGIN_POLYGON), V(FMT_TESS_BEGIN_CONTOUR) };
    char out[64];
    /* A for-loop body line referencing loop var "i": */
    fmt_reindent_expr(seq, 2, "  gluVertex(cos(i), sin(i), 0);", out, sizeof(out));
    ASSERT_STR("reindent for-loop body tess=2", out,
               "      gluVertex(cos(i), sin(i), 0);");
}

static void test_autonormal_indent(void) {
    printf("test_autonormal_indent\n");
    /* Autonormals are inserted immediately before a vertex command using
     * fmt_indent at the insert position.  They must respect tess scope depth
     * the same as any other normal command. */

    /* Plain glBegin, no tess — normal at tess=0, begin=1 → 4 spaces */
    {
        FmtCmd seq[] = { V(FMT_GL_BEGIN) };
        ASSERT_STR("autonormal plain begin", indent_str(seq, 1), "    ");
    }
    /* Inside tess contour + glBegin — tess=2, begin=1 → 8 spaces */
    {
        FmtCmd seq[] = {
            V(FMT_TESS_BEGIN_POLYGON),
            V(FMT_TESS_BEGIN_CONTOUR),
            V(FMT_GL_BEGIN),
        };
        ASSERT_STR("autonormal tess=2 begin=1", indent_str(seq, 3), "        ");
    }
    /* Inside tess polygon only + glBegin — tess=1, begin=1 → 6 spaces */
    {
        FmtCmd seq[] = { V(FMT_TESS_BEGIN_POLYGON), V(FMT_GL_BEGIN) };
        ASSERT_STR("autonormal tess=1 begin=1", indent_str(seq, 2), "      ");
    }
}

static void test_reindent_preserves_expression(void) {
    printf("test_reindent_preserves_expression\n");
    /* The expression part must be preserved exactly */
    FmtCmd seq[] = {};
    char out[64];
    /* Expression with no semicolon */
    fmt_reindent_expr(seq, 0, "glVertex3f(x, y, z)", out, sizeof(out));
    ASSERT_STR("reindent no semicolon", out, "  glVertex3f(x, y, z)");
    /* Expression with complex math */
    fmt_reindent_expr(seq, 0, "  glVertex3f(cos(i)*r, sin(i)*r, z+0.1);",
                      out, sizeof(out));
    ASSERT_STR("reindent complex expr", out,
               "  glVertex3f(cos(i)*r, sin(i)*r, z+0.1);");
}

/* ---- main -------------------------------------------------------------- */

int main(void) {
    printf("=== cmd_format tests ===\n\n");

    test_empty_sequence();
    test_after_tess_polygon();
    test_after_tess_contour();
    test_after_gl_begin_inside_tess();
    test_gl_begin_indent();
    test_gl_end_indent();
    test_glu_end_contour_indent();
    test_glu_end_polygon_indent();
    test_full_tess_scenario();
    test_invalid_cmds_skipped();
    test_underflow_clamps_to_zero();
    test_no_tess_plain_begin();

    printf("\n--- autonormal ---\n\n");
    test_autonormal_indent();

    printf("\n--- fmt_reindent_expr ---\n\n");
    test_reindent_no_context();
    test_reindent_inside_gl_begin();
    test_reindent_tess_contour_no_begin();
    test_reindent_tess_contour_inside_begin();
    test_reindent_strips_any_leading_whitespace();
    test_reindent_for_loop_body();
    test_reindent_preserves_expression();

    printf("\n=== Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0)
        printf(", %d FAILED", g_tests_failed);
    printf(" ===\n");

    return g_tests_failed > 0 ? 1 : 0;
}
