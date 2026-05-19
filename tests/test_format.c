/*
 * test_format.c - Tests for the command indentation logic in src/repl/format.c
 *
 * Build:
 *   gcc -Wall -std=c2x -I. -Isrc -o test_format tests/test_format.c src/repl/format.c -lm
 *
 * Run:
 *   ./test_format
 */
#include "repl/format.h"
#include "support/test_harness.h"

#include <stdio.h>

/* ---- Test harness ------------------------------------------------------ */

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_INT(label, got, expected) do { \
    TEST_ASSERT_INT(&g_harness, label, got, expected); \
} while (0)

#define ASSERT_STR(label, got, expected) do { \
    TEST_ASSERT_STR(&g_harness, label, got, expected); \
} while (0)

/* Helper: run repl_format_indent and return a static buffer with the result */
static const char *indent_str(const ReplFmtCmd *cmds, int n) {
    static char buf[64];
    repl_format_indent(cmds, n, buf, sizeof(buf));
    return buf;
}
static const char *end_str(const ReplFmtCmd *cmds, int n) {
    static char buf[64];
    repl_format_end_indent(cmds, n, buf, sizeof(buf));
    return buf;
}
static const char *tess_end_str(const ReplFmtCmd *cmds, int n) {
    static char buf[64];
    repl_format_tess_end_indent(cmds, n, buf, sizeof(buf));
    return buf;
}

/* ---- Helpers to build ReplFmtCmd sequences --------------------------------- */

static ReplFmtCmd V(ReplFmtType t) { return (ReplFmtCmd){ t, 1 }; }
static ReplFmtCmd INV(ReplFmtType t) { return (ReplFmtCmd){ t, 0 }; }  /* invalid/deleted */

/* ---- Test cases -------------------------------------------------------- */

static void test_empty_sequence(void) {
    printf("test_empty_sequence\n");
    ReplFmtCmd seq[] = {};
    ASSERT_INT("tess_depth empty", repl_format_tess_depth(seq, 0), 0);
    ASSERT_INT("begin_depth empty", repl_format_begin_depth(seq, 0), 0);
    ASSERT_STR("repl_format_indent empty", indent_str(seq, 0), "  ");
}

static void test_after_tess_polygon(void) {
    printf("test_after_tess_polygon\n");
    /* Sequence: gluBegin(GLU_POLYGON) */
    ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON) };
    int n = 1;
    ASSERT_INT("tess_depth after polygon", repl_format_tess_depth(seq, n), 1);
    ASSERT_INT("begin_depth after polygon", repl_format_begin_depth(seq, n), 0);
    ASSERT_STR("repl_format_indent after polygon", indent_str(seq, n), "    ");
}

static void test_after_tess_contour(void) {
    printf("test_after_tess_contour\n");
    /* Sequence: gluBegin(GLU_POLYGON); gluBegin(GLU_CONTOUR) */
    ReplFmtCmd seq[] = {
        V(REPL_FMT_TESS_BEGIN_POLYGON),
        V(REPL_FMT_TESS_BEGIN_CONTOUR),
    };
    int n = 2;
    ASSERT_INT("tess_depth after contour", repl_format_tess_depth(seq, n), 2);
    ASSERT_INT("begin_depth after contour", repl_format_begin_depth(seq, n), 0);
    ASSERT_STR("repl_format_indent after contour", indent_str(seq, n), "      ");  /* 6 spaces */
}

static void test_after_gl_begin_inside_tess(void) {
    printf("test_after_gl_begin_inside_tess\n");
    /* Sequence: gluBegin(GLU_POLYGON); gluBegin(GLU_CONTOUR); glBegin(GL_TRIANGLES) */
    ReplFmtCmd seq[] = {
        V(REPL_FMT_TESS_BEGIN_POLYGON),
        V(REPL_FMT_TESS_BEGIN_CONTOUR),
        V(REPL_FMT_GL_BEGIN),
    };
    int n = 3;
    ASSERT_INT("tess_depth inside glBegin", repl_format_tess_depth(seq, n), 2);
    ASSERT_INT("begin_depth inside glBegin", repl_format_begin_depth(seq, n), 1);
    ASSERT_STR("repl_format_indent inside glBegin", indent_str(seq, n), "        ");  /* 8 spaces */
}

static void test_gl_begin_indent(void) {
    printf("test_gl_begin_indent\n");
    /* glBegin itself sits at tess=2, begin=0 (it hasn't been added yet) */
    ReplFmtCmd seq[] = {
        V(REPL_FMT_TESS_BEGIN_POLYGON),
        V(REPL_FMT_TESS_BEGIN_CONTOUR),
        /* glBegin not yet added; computing indent at this position */
    };
    int n = 2;
    /* glBegin uses repl_format_indent (begin not open yet) → 2+2*2+2*0 = 6 */
    ASSERT_STR("glBegin indent at tess=2", indent_str(seq, n), "      ");  /* 6 spaces */
}

static void test_gl_end_indent(void) {
    printf("test_gl_end_indent\n");
    /* glEnd is called when inside tess=2, begin=1.
     * It should align with its matching glBegin at 6 spaces. */
    ReplFmtCmd seq[] = {
        V(REPL_FMT_TESS_BEGIN_POLYGON),
        V(REPL_FMT_TESS_BEGIN_CONTOUR),
        V(REPL_FMT_GL_BEGIN),
    };
    int n = 3;
    /* repl_format_end_indent: 2 + 2*tess = 2+4 = 6 (no begin depth) */
    ASSERT_STR("glEnd indent at tess=2 begin=1", end_str(seq, n), "      ");  /* 6 spaces */
}

static void test_glu_end_contour_indent(void) {
    printf("test_glu_end_contour_indent\n");
    /* gluEnd() closes the contour. tess=2 at this point. */
    ReplFmtCmd seq[] = {
        V(REPL_FMT_TESS_BEGIN_POLYGON),
        V(REPL_FMT_TESS_BEGIN_CONTOUR),
    };
    int n = 2;
    /* repl_format_tess_end_indent: 2 + 2*(tess-1) = 2+2*1 = 4 */
    ASSERT_STR("gluEnd(contour) indent", tess_end_str(seq, n), "    ");  /* 4 spaces */
}

static void test_glu_end_polygon_indent(void) {
    printf("test_glu_end_polygon_indent\n");
    /* gluEnd() closes the polygon. tess=1 at this point (contour already closed). */
    ReplFmtCmd seq[] = {
        V(REPL_FMT_TESS_BEGIN_POLYGON),
    };
    int n = 1;
    /* repl_format_tess_end_indent: 2 + 2*(1-1) = 2 */
    ASSERT_STR("gluEnd(polygon) indent", tess_end_str(seq, n), "  ");  /* 2 spaces */
}

static void test_full_tess_scenario(void) {
    printf("test_full_tess_scenario - complete gluBegin/glBegin nesting\n");
    /*
     * Desired output from user spec:
     *   gluBegin(GLU_POLYGON);        ← 2 spaces  (tess=0)
     *     gluBegin(GLU_CONTOUR);      ← 4 spaces  (tess=1)
     *       gluNormal(0, 0, 1);       ← 6 spaces  (tess=2, begin=0)
     *       gluVertex(-1, -1, 0);     ← 6 spaces  (tess=2, begin=0)
     *       glBegin(GL_TRIANGLES);    ← 6 spaces  (tess=2, begin=0, not yet opened)
     *         glColor(1, 0.5, 0, 1);  ← 8 spaces  (tess=2, begin=1)
     *         glVertex(1, -1, 0);     ← 8 spaces  (tess=2, begin=1)
     *       glEnd();                  ← 6 spaces  (tess=2, repl_format_end_indent)
     *     gluEnd();                   ← 4 spaces  (tess=2, repl_format_tess_end_indent → tess-1=1)
     *   gluEnd();                     ← 2 spaces  (tess=1, repl_format_tess_end_indent → tess-1=0)
     */

    /* gluBegin(GLU_POLYGON) indent: sequence empty */
    {
        ReplFmtCmd seq[] = {};
        ASSERT_STR("gluBegin(POLYGON)", indent_str(seq, 0), "  ");
    }
    /* gluBegin(GLU_CONTOUR) indent: after POLYGON */
    {
        ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON) };
        ASSERT_STR("gluBegin(CONTOUR)", indent_str(seq, 1), "    ");
    }
    /* gluNormal: after POLYGON + CONTOUR */
    {
        ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_TESS_BEGIN_CONTOUR) };
        ASSERT_STR("gluNormal", indent_str(seq, 2), "      ");
    }
    /* glBegin: after POLYGON + CONTOUR (begin=0, not yet open) */
    {
        ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_TESS_BEGIN_CONTOUR) };
        ASSERT_STR("glBegin", indent_str(seq, 2), "      ");
    }
    /* glColor/glVertex: after POLYGON + CONTOUR + GL_BEGIN */
    {
        ReplFmtCmd seq[] = {
            V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_TESS_BEGIN_CONTOUR), V(REPL_FMT_GL_BEGIN),
        };
        ASSERT_STR("glColor inside glBegin", indent_str(seq, 3), "        ");
        ASSERT_STR("glVertex inside glBegin", indent_str(seq, 3), "        ");
    }
    /* glEnd: after POLYGON + CONTOUR + GL_BEGIN */
    {
        ReplFmtCmd seq[] = {
            V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_TESS_BEGIN_CONTOUR), V(REPL_FMT_GL_BEGIN),
        };
        ASSERT_STR("glEnd", end_str(seq, 3), "      ");
    }
    /* gluEnd(contour): after POLYGON + CONTOUR */
    {
        ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_TESS_BEGIN_CONTOUR) };
        ASSERT_STR("gluEnd(CONTOUR)", tess_end_str(seq, 2), "    ");
    }
    /* gluEnd(polygon): after POLYGON */
    {
        ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON) };
        ASSERT_STR("gluEnd(POLYGON)", tess_end_str(seq, 1), "  ");
    }
}

static void test_invalid_cmds_skipped(void) {
    printf("test_invalid_cmds_skipped\n");
    /* Invalid commands should not affect depth counting */
    ReplFmtCmd seq[] = {
        V(REPL_FMT_TESS_BEGIN_POLYGON),
        INV(REPL_FMT_TESS_BEGIN_CONTOUR),  /* deleted - should be ignored */
        V(REPL_FMT_TESS_BEGIN_CONTOUR),
    };
    int n = 3;
    ASSERT_INT("tess_depth with invalid entry", repl_format_tess_depth(seq, n), 2);
    ASSERT_STR("repl_format_indent with invalid entry", indent_str(seq, n), "      ");
}

static void test_underflow_clamps_to_zero(void) {
    printf("test_underflow_clamps_to_zero\n");
    /* More end-markers than begin-markers should not go below 0 */
    ReplFmtCmd seq[] = {
        V(REPL_FMT_TESS_END),
        V(REPL_FMT_TESS_END),
    };
    ASSERT_INT("tess_depth underflow", repl_format_tess_depth(seq, 2), 0);
    ASSERT_STR("repl_format_indent at underflow", indent_str(seq, 2), "  ");
}

static void test_no_tess_plain_begin(void) {
    printf("test_no_tess_plain_begin\n");
    /* Outside any tess scope: glBegin/glEnd indent as before */
    {
        ReplFmtCmd seq[] = {};
        ASSERT_STR("glBegin no tess", indent_str(seq, 0), "  ");
    }
    {
        ReplFmtCmd seq[] = { V(REPL_FMT_GL_BEGIN) };
        ASSERT_STR("vertex inside glBegin no tess", indent_str(seq, 1), "    ");
        ASSERT_STR("glEnd no tess", end_str(seq, 1), "  ");
    }
}

/* ---- repl_format_reindent_expr tests ------------------------------------------- */

static void test_reindent_no_context(void) {
    printf("test_reindent_no_context\n");
    /* Outside any scope: should get 2-space indent */
    ReplFmtCmd seq[] = {};
    char out[64];
    repl_format_reindent_expr(seq, 0, "  glVertex3f(-1, 0, 0);", out, sizeof(out));
    ASSERT_STR("reindent no context", out, "  glVertex3f(-1, 0, 0);");
}

static void test_reindent_inside_gl_begin(void) {
    printf("test_reindent_inside_gl_begin\n");
    /* Inside a plain glBegin block: should get 4-space indent */
    ReplFmtCmd seq[] = { V(REPL_FMT_GL_BEGIN) };
    char out[64];
    /* raw_expr has wrong 2-space indent (saved from file) */
    repl_format_reindent_expr(seq, 1, "  glVertex3f(-1, 0, 0);", out, sizeof(out));
    ASSERT_STR("reindent inside glBegin", out, "    glVertex3f(-1, 0, 0);");
}

static void test_reindent_tess_contour_no_begin(void) {
    printf("test_reindent_tess_contour_no_begin\n");
    /* Inside tess contour (tess=2) but outside glBegin: 6-space */
    ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_TESS_BEGIN_CONTOUR) };
    char out[64];
    repl_format_reindent_expr(seq, 2, "  gluVertex(-1.2, -0.45, 0);", out, sizeof(out));
    ASSERT_STR("reindent tess=2 begin=0", out, "      gluVertex(-1.2, -0.45, 0);");
}

static void test_reindent_tess_contour_inside_begin(void) {
    printf("test_reindent_tess_contour_inside_begin\n");
    /* Inside tess contour (tess=2) AND inside glBegin: 8-space.
     * This is the key case from output.c where has_vars lines like
     * "glVertex3f(-1.2, -0.45, z)" were getting 2-space from the saved
     * file instead of the correct 8-space. */
    ReplFmtCmd seq[] = {
        V(REPL_FMT_TESS_BEGIN_POLYGON),
        V(REPL_FMT_TESS_BEGIN_CONTOUR),
        V(REPL_FMT_GL_BEGIN),
    };
    char out[64];
    /* Simulate load_from_file: raw line has old 2-space indent from file */
    repl_format_reindent_expr(seq, 3, "  glVertex3f(-1.2, -0.45, z);", out, sizeof(out));
    ASSERT_STR("reindent has_vars tess=2 begin=1", out,
               "        glVertex3f(-1.2, -0.45, z);");
}

static void test_reindent_strips_any_leading_whitespace(void) {
    printf("test_reindent_strips_any_leading_whitespace\n");
    /* Verify that various amounts of leading whitespace are stripped */
    ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON) };
    char out[64];

    /* 0 spaces */
    repl_format_reindent_expr(seq, 1, "glColor3f(1, 0, 0);", out, sizeof(out));
    ASSERT_STR("reindent strip 0 spaces", out, "    glColor3f(1, 0, 0);");

    /* 6 spaces (over-indented) */
    repl_format_reindent_expr(seq, 1, "      glColor3f(1, 0, 0);", out, sizeof(out));
    ASSERT_STR("reindent strip 6 spaces", out, "    glColor3f(1, 0, 0);");

    /* mixed tabs and spaces */
    repl_format_reindent_expr(seq, 1, "\t  glColor3f(1, 0, 0);", out, sizeof(out));
    ASSERT_STR("reindent strip tab+spaces", out, "    glColor3f(1, 0, 0);");
}

static void test_reindent_for_loop_body(void) {
    printf("test_reindent_for_loop_body\n");
    /* For-loop body commands also use repl_format_reindent_expr.
     * Even though they aren't has_vars per se, they always have their
     * source overwritten with the raw REPL expression text. */
    ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_TESS_BEGIN_CONTOUR) };
    char out[64];
    /* A for-loop body line referencing loop var "i": */
    repl_format_reindent_expr(seq, 2, "  gluVertex(cos(i), sin(i), 0);", out, sizeof(out));
    ASSERT_STR("reindent for-loop body tess=2", out,
               "      gluVertex(cos(i), sin(i), 0);");
}

/* ---- repl_format_tess_leaf_indent tests --------------------------------------- */

/* ---- output.c import trace -------------------------------------------- */

static void test_output_c_import_trace(void) {
    printf("test_output_c_import_trace\n");
    /*
     * Trace load_from_file processing output.c line by line.
     * The snippet contains (relevant lines only):
     *
     *   glEnable/glShadeModel×7   → REPL_FMT_OTHER ×7
     *   z = -0.25                 → REPL_FMT_OTHER (var assign)
     *   // comment                → REPL_FMT_OTHER
     *   { gluTessBeginPolygon }   → CMD_TESS_BEGIN_POLYGON
     *   gluTessBeginContour       → CMD_TESS_BEGIN_CONTOUR
     *   glBegin(GL_QUAD_STRIP)    → CMD_GL_BEGIN
     *   { _tn[0]=0;... }          → CMD_TESS_NORMAL  (gluNormal)
     *   { _tc[0]=1;... }          → CMD_TESS_COLOR   (gluColor)
     *   { TessVertex...vertex }   → CMD_TESS_VERTEX  (gluVertex)
     *   glNormal3f(0, 0, 1)       → CMD_NORMAL3F
     *   glColor3f(1, 1, 1)        → CMD_COLOR3F
     *   glVertex3f(-1.2,-0.45, z) → CMD_VERTEX3F  (has_vars)
     *   glVertex3f(-1.2,-0.45, 0) → CMD_VERTEX3F
     *   { _tc[0]=1;... }          → CMD_TESS_COLOR
     *   { TessVertex...vertex }   → CMD_TESS_VERTEX
     *   glVertex3f(-1, -0.45, 0)  → CMD_VERTEX3F
     *   glEnd()                   → CMD_GL_END
     *   gluTessEndContour         → CMD_TESS_END
     *   gluTessEndPolygon         → CMD_TESS_END
     *
     * For indent purposes only POLYGON/CONTOUR/GL_BEGIN/GL_END/TESS_END matter.
     * We use 9 REPL_FMT_OTHER entries to represent the preamble.
     *
     * Expected indentation for each command:
     *   gluBegin(GLU_POLYGON)   ← tess_leaf(tess=0)           = 2
     *   gluBegin(GLU_CONTOUR)   ← tess_leaf(tess=1)           = 4
     *   glBegin(GL_QUAD_STRIP)  ← repl_format_indent(tess=2, begin=0) = 6
     *   gluNormal               ← tess_leaf(tess=2, begin=1→ignored) = 6
     *   gluColor                ← tess_leaf(tess=2, begin=1→ignored) = 6
     *   gluVertex               ← tess_leaf(tess=2, begin=1→ignored) = 6
     *   glNormal3f              ← repl_format_indent(tess=2, begin=1)  = 8
     *   glColor3f               ← repl_format_indent(tess=2, begin=1)  = 8
     *   glVertex3f (has_vars)   ← repl_format_indent(tess=2, begin=1)  = 8
     *   glVertex3f              ← repl_format_indent(tess=2, begin=1)  = 8
     *   gluColor  (second)      ← tess_leaf(tess=2, begin=1→ignored) = 6
     *   gluVertex (second)      ← tess_leaf(tess=2, begin=1→ignored) = 6
     *   glVertex3f              ← repl_format_indent(tess=2, begin=1)  = 8
     *   glEnd()                 ← repl_format_end_indent(tess=2)       = 6
     *   gluEnd() contour        ← tess_end(tess=2→tess-1=1)   = 4
     *   gluEnd() polygon        ← tess_end(tess=1→tess-1=0)   = 2
     */
    char buf[32];

    /* Build a sequence incrementally, mirroring repl_state_document_cmds_mut() as load_from_file fills it. */
    ReplFmtCmd seq[32];
    int n = 0;
    /* Preamble: 9 REPL_FMT_OTHER entries (glEnable×7, var, comment) */
    for (int i = 0; i < 9; i++) seq[n++] = V(REPL_FMT_OTHER);

    /* gluBegin(GLU_POLYGON): uses tess_leaf at n=9 */
    repl_format_tess_leaf_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: gluBegin(POLYGON)", buf, "  ");
    seq[n++] = V(REPL_FMT_TESS_BEGIN_POLYGON);

    /* gluBegin(GLU_CONTOUR): uses tess_leaf at n=10 */
    repl_format_tess_leaf_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: gluBegin(CONTOUR)", buf, "    ");
    seq[n++] = V(REPL_FMT_TESS_BEGIN_CONTOUR);

    /* glBegin(GL_QUAD_STRIP): uses repl_format_indent at n=11 */
    repl_format_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: glBegin", buf, "      ");
    seq[n++] = V(REPL_FMT_GL_BEGIN);

    /* gluNormal (_tn handler): must use tess_leaf at n=12 */
    repl_format_tess_leaf_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: gluNormal (_tn)", buf, "      ");
    seq[n++] = V(REPL_FMT_OTHER);  /* CMD_TESS_NORMAL */

    /* gluColor (_tc handler): must use tess_leaf at n=13 */
    repl_format_tess_leaf_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: gluColor (_tc)", buf, "      ");
    seq[n++] = V(REPL_FMT_OTHER);  /* CMD_TESS_COLOR */

    /* gluVertex (TessVertex handler): must use tess_leaf at n=14 */
    repl_format_tess_leaf_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: gluVertex (TessVertex)", buf, "      ");
    seq[n++] = V(REPL_FMT_OTHER);  /* CMD_TESS_VERTEX */

    /* glNormal3f (regular line): uses repl_format_indent at n=15 */
    repl_format_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: glNormal3f", buf, "        ");
    seq[n++] = V(REPL_FMT_OTHER);

    /* glColor3f (regular line): uses repl_format_indent at n=16 */
    repl_format_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: glColor3f", buf, "        ");
    seq[n++] = V(REPL_FMT_OTHER);

    /* glVertex3f has_vars (repl_format_reindent_from_parsed uses parsed source = 8-space) */
    repl_format_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: glVertex3f(has_vars)", buf, "        ");
    seq[n++] = V(REPL_FMT_OTHER);

    /* glVertex3f plain: repl_format_indent at n=18 */
    repl_format_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: glVertex3f plain", buf, "        ");
    seq[n++] = V(REPL_FMT_OTHER);

    /* gluColor second (_tc): tess_leaf at n=19 */
    repl_format_tess_leaf_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: gluColor (2nd)", buf, "      ");
    seq[n++] = V(REPL_FMT_OTHER);

    /* gluVertex second (TessVertex): tess_leaf at n=20 */
    repl_format_tess_leaf_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: gluVertex (2nd)", buf, "      ");
    seq[n++] = V(REPL_FMT_OTHER);

    /* glVertex3f: repl_format_indent at n=21 */
    repl_format_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: glVertex3f (3rd)", buf, "        ");
    seq[n++] = V(REPL_FMT_OTHER);

    /* glEnd(): repl_format_end_indent at n=22 */
    repl_format_end_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: glEnd()", buf, "      ");
    seq[n++] = V(REPL_FMT_GL_END);

    /* gluEnd() closing contour: tess_end at n=23 (tess=2 → 4-space) */
    repl_format_tess_end_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: gluEnd(CONTOUR)", buf, "    ");
    seq[n++] = V(REPL_FMT_TESS_END);

    /* gluEnd() closing polygon: tess_end at n=24 (tess=1 → 2-space) */
    repl_format_tess_end_indent(seq, n, buf, sizeof(buf));
    ASSERT_STR("import: gluEnd(POLYGON)", buf, "  ");
    seq[n++] = V(REPL_FMT_TESS_END);
}

static void test_tess_leaf_indent(void) {
    printf("test_tess_leaf_indent\n");

    /* Outside all scopes: 2-space */
    { ReplFmtCmd seq[] = {}; ASSERT_STR("tess_leaf empty", indent_str(seq, 0), "  "); }

    /* Inside tess polygon only (tess=1): 4-space */
    { ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON) };
      char buf[32]; repl_format_tess_leaf_indent(seq, 1, buf, sizeof(buf));
      ASSERT_STR("tess_leaf tess=1", buf, "    "); }

    /* Inside tess contour (tess=2): 6-space */
    { ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_TESS_BEGIN_CONTOUR) };
      char buf[32]; repl_format_tess_leaf_indent(seq, 2, buf, sizeof(buf));
      ASSERT_STR("tess_leaf tess=2", buf, "      "); }

    /* KEY: inside tess=2 AND inside glBegin - begin depth must NOT be added */
    { ReplFmtCmd seq[] = {
          V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_TESS_BEGIN_CONTOUR), V(REPL_FMT_GL_BEGIN) };
      char buf[32]; repl_format_tess_leaf_indent(seq, 3, buf, sizeof(buf));
      ASSERT_STR("tess_leaf tess=2 begin=1", buf, "      ");  /* 6, not 8 */
      /* Compare with repl_format_indent at same position: */
      repl_format_indent(seq, 3, buf, sizeof(buf));
      ASSERT_STR("repl_format_indent tess=2 begin=1", buf, "        ");  /* 8 */
    }
}

static void test_glu_vs_gl_indent_contrast(void) {
    printf("test_glu_vs_gl_indent_contrast\n");
    /* Demonstrates the distinction from the user's desired layout:
     *
     *   gluBegin(GLU_POLYGON);         ← 2-space  tess_leaf(tess=0)
     *     gluBegin(GLU_CONTOUR);       ← 4-space  tess_leaf(tess=1)
     *       glBegin(GL_QUAD_STRIP);    ← 6-space  repl_format_indent(tess=2, begin=0)
     *       gluNormal(0, 0, 1);        ← 6-space  tess_leaf(tess=2, begin=1→ignored)
     *       gluVertex(-1, -1, 0);      ← 6-space  tess_leaf(tess=2, begin=1→ignored)
     *         glColor3f(1, 0, 0);      ← 8-space  repl_format_indent(tess=2, begin=1)
     *         glVertex3f(-1, -1, 0);   ← 8-space  repl_format_indent(tess=2, begin=1)
     *       glEnd();                   ← 6-space  repl_format_end_indent(tess=2)
     *     gluEnd();                    ← 4-space  tess_end(tess=2 → tess-1=1)
     *   gluEnd();                      ← 2-space  tess_end(tess=1 → tess-1=0)
     */
    ReplFmtCmd polygon[]  = {};
    ReplFmtCmd contour[]  = { V(REPL_FMT_TESS_BEGIN_POLYGON) };
    ReplFmtCmd gl_begin[] = { V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_TESS_BEGIN_CONTOUR) };
    ReplFmtCmd inside[]   = { V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_TESS_BEGIN_CONTOUR),
                          V(REPL_FMT_GL_BEGIN) };
    char buf[32];

    /* gluBegin(GLU_POLYGON) opener: tess_leaf at tess=0 */
    repl_format_tess_leaf_indent(polygon, 0, buf, sizeof(buf));
    ASSERT_STR("gluBegin(POLYGON) opener", buf, "  ");

    /* gluBegin(GLU_CONTOUR) opener: tess_leaf at tess=1 */
    repl_format_tess_leaf_indent(contour, 1, buf, sizeof(buf));
    ASSERT_STR("gluBegin(CONTOUR) opener", buf, "    ");

    /* glBegin(GL_QUAD_STRIP): repl_format_indent at tess=2, begin=0 */
    repl_format_indent(gl_begin, 2, buf, sizeof(buf));
    ASSERT_STR("glBegin at tess=2", buf, "      ");

    /* gluNormal/gluVertex: tess_leaf at tess=2, begin=1 → 6 */
    repl_format_tess_leaf_indent(inside, 3, buf, sizeof(buf));
    ASSERT_STR("gluNormal at tess=2 begin=1", buf, "      ");

    /* glColor3f/glVertex3f: repl_format_indent at tess=2, begin=1 → 8 */
    repl_format_indent(inside, 3, buf, sizeof(buf));
    ASSERT_STR("glVertex3f at tess=2 begin=1", buf, "        ");

    /* glEnd(): repl_format_end_indent at tess=2, begin=1 → 6 */
    repl_format_end_indent(inside, 3, buf, sizeof(buf));
    ASSERT_STR("glEnd at tess=2 begin=1", buf, "      ");

    /* gluEnd() closing contour: tess_end at tess=2 → 4 */
    repl_format_tess_end_indent(gl_begin, 2, buf, sizeof(buf));
    ASSERT_STR("gluEnd(contour)", buf, "    ");

    /* gluEnd() closing polygon: tess_end at tess=1 → 2 */
    repl_format_tess_end_indent(contour, 1, buf, sizeof(buf));
    ASSERT_STR("gluEnd(polygon)", buf, "  ");
}

/* ---- repl_format_reindent_from_parsed tests ----------------------------------- */

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
     * parse_command (using repl_format_tess_leaf_indent), and we preserve that. */
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

static void test_autonormal_indent(void) {
    printf("test_autonormal_indent\n");
    /* Autonormals are inserted immediately before a vertex command using
     * repl_format_indent at the insert position.  They must respect tess scope depth
     * the same as any other normal command. */

    /* Plain glBegin, no tess - normal at tess=0, begin=1 → 4 spaces */
    {
        ReplFmtCmd seq[] = { V(REPL_FMT_GL_BEGIN) };
        ASSERT_STR("autonormal plain begin", indent_str(seq, 1), "    ");
    }
    /* Inside tess contour + glBegin - tess=2, begin=1 → 8 spaces */
    {
        ReplFmtCmd seq[] = {
            V(REPL_FMT_TESS_BEGIN_POLYGON),
            V(REPL_FMT_TESS_BEGIN_CONTOUR),
            V(REPL_FMT_GL_BEGIN),
        };
        ASSERT_STR("autonormal tess=2 begin=1", indent_str(seq, 3), "        ");
    }
    /* Inside tess polygon only + glBegin - tess=1, begin=1 → 6 spaces */
    {
        ReplFmtCmd seq[] = { V(REPL_FMT_TESS_BEGIN_POLYGON), V(REPL_FMT_GL_BEGIN) };
        ASSERT_STR("autonormal tess=1 begin=1", indent_str(seq, 2), "      ");
    }
}

static void test_reindent_preserves_expression(void) {
    printf("test_reindent_preserves_expression\n");
    /* The expression part must be preserved exactly */
    ReplFmtCmd seq[] = {};
    char out[64];
    /* Expression with no semicolon */
    repl_format_reindent_expr(seq, 0, "glVertex3f(x, y, z)", out, sizeof(out));
    ASSERT_STR("reindent no semicolon", out, "  glVertex3f(x, y, z)");
    /* Expression with complex math */
    repl_format_reindent_expr(seq, 0, "  glVertex3f(cos(i)*r, sin(i)*r, z+0.1);",
                      out, sizeof(out));
    ASSERT_STR("reindent complex expr", out,
               "  glVertex3f(cos(i)*r, sin(i)*r, z+0.1);");
}

/* ---- main -------------------------------------------------------------- */

int main(void) {
    printf("=== repl_format tests ===\n\n");

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

    printf("\n--- output.c import trace ---\n\n");
    test_output_c_import_trace();

    printf("\n--- repl_format_tess_leaf_indent ---\n\n");
    test_tess_leaf_indent();
    test_glu_vs_gl_indent_contrast();

    printf("\n--- repl_format_reindent_from_parsed ---\n\n");
    test_reindent_from_parsed();

    printf("\n--- repl_format_reindent_expr ---\n\n");
    test_reindent_no_context();
    test_reindent_inside_gl_begin();
    test_reindent_tess_contour_no_begin();
    test_reindent_tess_contour_inside_begin();
    test_reindent_strips_any_leading_whitespace();
    test_reindent_for_loop_body();
    test_reindent_preserves_expression();

    printf("\n--- autonormal ---\n\n");
    test_autonormal_indent();

    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "repl_format");
}
