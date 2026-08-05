/*
 * tests/test_camera_header.c - The shared @camera reader, on its own.
 *
 * Line fixtures with no loader involvement: every acceptance rule, every
 * rejection rule, the region/depth contract, and the deferred pose merge.
 * The loader-level behaviour lives in test_camera_header_parity.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "repl/camera_header.h"
#include "repl/doc_order.h"
#include "support/camera_bridge_stub.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond)      TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, want)  TEST_ASSERT_INT(&g_harness, label, got, want)

/* ----- helpers ---------------------------------------------------------- */

static ReplCameraHeader g_hdr;

static void hdr_reset(void) {
    repl_camera_header_init(&g_hdr);
}

static ReplCameraLineResult offer(const char *line) {
    return repl_camera_header_offer(&g_hdr, line, 1);
}

/* Offer one line to a fresh reader and report the result. */
static ReplCameraLineResult offer_alone(const char *line) {
    hdr_reset();
    return offer(line);
}

static ReplCameraRule rule_of(const char *line) {
    hdr_reset();
    if (offer(line) != REPL_CAMERA_LINE_REJECTED)
        return REPL_CAMERA_RULE_NONE;
    return g_hdr.diags[0].rule;
}

#define ASSERT_RULE(what, line, expected) \
    ASSERT_INT(what, (int)rule_of(line), (int)(expected))

/* ----- accepted forms --------------------------------------------------- */

static void test_roles_parse(void) {
    printf("--- camera roles parse ---\n");

    hdr_reset();
    ASSERT_INT("dist accepted",
               offer("glTranslatef(0.0f, 0.0f, -10.0f);   // @camera dist"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("rx accepted",
               offer("glRotatef(15.0f, 1.0f, 0.0f, 0.0f);   // @camera rx"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("ry accepted",
               offer("glRotatef(20.0f, 0.0f, 1.0f, 0.0f);   // @camera ry"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("spin accepted",
               offer("glRotatef(g_angle, 0.0f, 1.0f, 0.0f);   // @camera spin"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("pan accepted",
               offer("glTranslatef(-1.0f, 2.5f, -0.5f);   // @camera pan"),
               REPL_CAMERA_LINE_ACCEPTED);

    ASSERT_TRUE("dist is the negated z", fabsf(g_hdr.pose.dist - 10.0f) < 1e-4f);
    ASSERT_TRUE("rx is the angle", fabsf(g_hdr.pose.rx - 15.0f) < 1e-4f);
    ASSERT_TRUE("ry is the angle", fabsf(g_hdr.pose.ry - 20.0f) < 1e-4f);
    ASSERT_TRUE("pan is the negated target",
                fabsf(g_hdr.pose.tx - 1.0f) < 1e-4f &&
                fabsf(g_hdr.pose.ty + 2.5f) < 1e-4f &&
                fabsf(g_hdr.pose.tz - 0.5f) < 1e-4f);

    /* spin is write-only: it carries no pose and no seen-mask bit. */
    ASSERT_INT("seen mask is the four pose roles",
               (int)g_hdr.seen_mask, (int)REPL_CAMERA_MASK_POSE);
    ASSERT_INT("no diagnostics on a complete header", g_hdr.diag_count, 0);
}

/* Both comment syntaxes have to work: exported C rewrites every `//` into a
 * C89 block comment, so a reader that handled only `//` would silently drop
 * the camera from every exported file - this bug, with new syntax. */
static void test_both_comment_syntaxes(void) {
    printf("--- camera tags in both comment syntaxes ---\n");

    ASSERT_INT("dist in a block comment",
               offer_alone("glTranslatef(0, 0, -4);   /* @camera dist */"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("rx in a block comment",
               offer_alone("glRotatef(3, 1, 0, 0);   /* @camera rx */"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("ry in a block comment",
               offer_alone("glRotatef(3, 0, 1, 0);   /* @camera ry */"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("spin in a block comment",
               offer_alone("glRotatef(g_angle, 0, 1, 0);   /* @camera spin */"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("pan in a block comment",
               offer_alone("glTranslatef(1, 2, 3);   /* @camera pan */"),
               REPL_CAMERA_LINE_ACCEPTED);
}

/* The marker carries no meaning the tags do not already carry, so it is an
 * ordinary comment the reader never inspects - in either syntax. */
static void test_marker_is_an_ordinary_comment(void) {
    printf("--- camera marker is an ordinary comment ---\n");

    ASSERT_INT("// camera is not a camera line",
               offer_alone("// camera"), REPL_CAMERA_LINE_NOT_CAMERA);
    ASSERT_INT("/* camera */ is not a camera line",
               offer_alone("/* camera */"), REPL_CAMERA_LINE_NOT_CAMERA);
    ASSERT_INT("a decorated banner is not a camera line",
               offer_alone("// --- Camera -------------------"),
               REPL_CAMERA_LINE_NOT_CAMERA);
    ASSERT_INT("prose mentioning the camera is not a camera line",
               offer_alone("// The camera starts here."),
               REPL_CAMERA_LINE_NOT_CAMERA);
    ASSERT_INT("an untagged transform is not a camera line",
               offer_alone("glTranslatef(0, 0, -8);"),
               REPL_CAMERA_LINE_NOT_CAMERA);
}

/* ----- rejection rules -------------------------------------------------- */

static void test_rejection_rules(void) {
    printf("--- camera rejection rules ---\n");

    ASSERT_RULE("unknown role",
                "glRotatef(1, 0, 1, 0);   // @camera yaw",
                REPL_CAMERA_RULE_UNKNOWN_ROLE);
    ASSERT_RULE("dist on the wrong call",
                "glRotatef(1, 0, 1, 0);   // @camera dist",
                REPL_CAMERA_RULE_WRONG_CALL);
    ASSERT_RULE("rx on the wrong call",
                "glTranslatef(0, 0, -1);   // @camera rx",
                REPL_CAMERA_RULE_WRONG_CALL);
    ASSERT_RULE("non-literal argument",
                "glRotatef(20.0f * t, 0.0f, 1.0f, 0.0f);   // @camera ry",
                REPL_CAMERA_RULE_NON_LITERAL_ARG);
    ASSERT_RULE("a tagged ry whose argument is an identifier",
                "glRotatef(g_angle, 0.0f, 1.0f, 0.0f);   // @camera ry",
                REPL_CAMERA_RULE_NON_LITERAL_ARG);
    ASSERT_RULE("dist with a folded-in pan offset",
                "glTranslatef(0.0f, -2.5f, -10.0f);   // @camera dist",
                REPL_CAMERA_RULE_DIST_OFFSET);
    ASSERT_RULE("rx on the wrong axis",
                "glRotatef(15.0f, 0.0f, 1.0f, 0.0f);   // @camera rx",
                REPL_CAMERA_RULE_AXIS_MISMATCH);
    ASSERT_RULE("ry on the wrong axis",
                "glRotatef(15.0f, 1.0f, 0.0f, 0.0f);   // @camera ry",
                REPL_CAMERA_RULE_AXIS_MISMATCH);
    ASSERT_RULE("code after the call",
                "glRotatef(15.0f, 1.0f, 0.0f, 0.0f); glEnd();   // @camera rx",
                REPL_CAMERA_RULE_TRAILING_TEXT);
}

/* The spin hook's argument is tokenized, not string-compared. */
static void test_spin_argument(void) {
    printf("--- camera spin argument ---\n");

    ASSERT_INT("bare g_angle",
               offer_alone("glRotatef(g_angle, 0, 1, 0);   // @camera spin"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("parenthesized and spaced",
               offer_alone("glRotatef( ( g_angle ) , 0, 1, 0);// @camera spin"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_RULE("a longer identifier is not g_angle",
                "glRotatef(g_angle2, 0, 1, 0);   // @camera spin",
                REPL_CAMERA_RULE_SPIN_ARG);
    ASSERT_RULE("an expression in g_angle is not g_angle",
                "glRotatef(g_angle + 1, 0, 1, 0);   // @camera spin",
                REPL_CAMERA_RULE_SPIN_ARG);
    ASSERT_RULE("a bare float is not the hook",
                "glRotatef(45.0f, 0, 1, 0);   // @camera spin",
                REPL_CAMERA_RULE_SPIN_ARG);
}

/* A leading `-` is part of the literal, not garbage in front of it. */
static void test_negative_literals(void) {
    printf("--- camera negative literals ---\n");

    hdr_reset();
    ASSERT_INT("negative rx",
               offer("glRotatef(-15.0000f, 1.0f, 0.0f, 0.0f);// @camera rx"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_TRUE("negative rx keeps its sign",
                fabsf(g_hdr.pose.rx + 15.0f) < 1e-4f);
}

static void test_duplicate_and_order(void) {
    printf("--- camera duplicate + order rules ---\n");

    hdr_reset();
    ASSERT_INT("first dist accepted",
               offer("glTranslatef(0, 0, -4);   // @camera dist"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("second dist rejected",
               offer("glTranslatef(0, 0, -9);   // @camera dist"),
               REPL_CAMERA_LINE_REJECTED);
    ASSERT_INT("duplicate role diagnosed",
               (int)g_hdr.diags[0].rule, REPL_CAMERA_RULE_DUPLICATE_ROLE);
    ASSERT_TRUE("first dist wins", fabsf(g_hdr.pose.dist - 4.0f) < 1e-4f);

    /* The exported C executes these rows in place, so a different order
     * composes a different modelview - it is rejected, not reordered. */
    hdr_reset();
    ASSERT_INT("pan accepted first",
               offer("glTranslatef(1, 2, 3);   // @camera pan"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("rx after pan rejected",
               offer("glRotatef(15, 1, 0, 0);   // @camera rx"),
               REPL_CAMERA_LINE_REJECTED);
    ASSERT_INT("out-of-order role diagnosed",
               (int)g_hdr.diags[0].rule, REPL_CAMERA_RULE_ROLE_ORDER);
}

/* User geometry begins only once every camera row is behind it. */
static void test_split_by_executable_line(void) {
    printf("--- camera row after body code ---\n");

    hdr_reset();
    ASSERT_INT("dist accepted",
               offer("glTranslatef(0, 0, -4);   // @camera dist"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("a comment between rows is fine",
               offer("// still the camera block"),
               REPL_CAMERA_LINE_NOT_CAMERA);
    ASSERT_INT("a blank line between rows is fine",
               offer(""), REPL_CAMERA_LINE_NOT_CAMERA);
    ASSERT_INT("rx still accepted after them",
               offer("glRotatef(15, 1, 0, 0);   // @camera rx"),
               REPL_CAMERA_LINE_ACCEPTED);
    ASSERT_INT("body code is not a camera line",
               offer("glClear(GL_COLOR_BUFFER_BIT);"),
               REPL_CAMERA_LINE_NOT_CAMERA);
    ASSERT_INT("a camera row after body code is rejected",
               offer("glTranslatef(1, 2, 3);   // @camera pan"),
               REPL_CAMERA_LINE_REJECTED);
    ASSERT_INT("split-by-code diagnosed",
               (int)g_hdr.diags[0].rule, REPL_CAMERA_RULE_SPLIT_BY_CODE);
}

/* ----- regions and depth ------------------------------------------------ */

static void test_baseline_depth(void) {
    printf("--- camera region baselines ---\n");

    /* A .glr sits at raw depth 0. */
    hdr_reset();
    ASSERT_INT("glr baseline accepts at depth 0",
               offer("glTranslatef(0, 0, -4);   // @camera dist"),
               REPL_CAMERA_LINE_ACCEPTED);

    /* An exported .c puts the same rows inside display(), at raw depth 1. */
    hdr_reset();
    repl_camera_header_set_region(&g_hdr, REPL_CAMERA_REGION_DISPLAY);
    offer("void display(void) {");
    ASSERT_INT("display baseline accepts at depth 1",
               offer("  glTranslatef(0, 0, -4);   /* @camera dist */"),
               REPL_CAMERA_LINE_ACCEPTED);

    /* A hand-formatted file may put the brace on its own line, so the
     * baseline is captured lazily rather than at the opener. */
    hdr_reset();
    repl_camera_header_set_region(&g_hdr, REPL_CAMERA_REGION_DISPLAY);
    offer("void display(void)");
    offer("{");
    ASSERT_INT("split-brace display body still accepts",
               offer("  glTranslatef(0, 0, -4);   /* @camera dist */"),
               REPL_CAMERA_LINE_ACCEPTED);

    /* Nested blocks are below the baseline. */
    hdr_reset();
    offer("for(i, 0, 4) {");
    ASSERT_INT("a tag inside a nested block is rejected",
               offer("  glTranslatef(0, 0, -4);   // @camera dist"),
               REPL_CAMERA_LINE_REJECTED);
    ASSERT_INT("nested depth diagnosed",
               (int)g_hdr.diags[0].rule, REPL_CAMERA_RULE_NESTED_DEPTH);

    /* A funcN body is a nested block for the same reason - which is why
     * func_depth never caught a transform inside a top-level `for`. */
    hdr_reset();
    offer("func0(r) {");
    ASSERT_INT("a tag inside a funcN body is rejected",
               offer("  glRotatef(15, 1, 0, 0);   // @camera rx"),
               REPL_CAMERA_LINE_REJECTED);

    /* The region closes when depth falls below baseline, so a later function
     * at the same raw depth must not match a stale display baseline. */
    hdr_reset();
    repl_camera_header_set_region(&g_hdr, REPL_CAMERA_REGION_DISPLAY);
    offer("void display(void) {");
    offer("  glTranslatef(0, 0, -4);   /* @camera dist */");
    offer("}");
    offer("void other(void) {");
    ASSERT_INT("a tag in a later function is rejected",
               offer("  glRotatef(15, 1, 0, 0);   /* @camera rx */"),
               REPL_CAMERA_LINE_REJECTED);
}

static void test_snippet_regions(void) {
    printf("--- camera snippet regions ---\n");

    hdr_reset();
    repl_camera_header_set_region(&g_hdr, REPL_CAMERA_REGION_SNIPPET);
    ASSERT_INT("a tag inside a snippet is rejected",
               offer("glTranslatef(0, 0, -4);   // @camera dist"),
               REPL_CAMERA_LINE_REJECTED);
    ASSERT_INT("in-snippet diagnosed",
               (int)g_hdr.diags[0].rule, REPL_CAMERA_RULE_IN_SNIPPET);

    hdr_reset();
    repl_camera_header_set_region(&g_hdr, REPL_CAMERA_REGION_POST_SNIPPET);
    ASSERT_INT("a tag after the snippet is rejected",
               offer("glTranslatef(0, 0, -4);   // @camera dist"),
               REPL_CAMERA_LINE_REJECTED);
    ASSERT_INT("after-snippet diagnosed",
               (int)g_hdr.diags[0].rule, REPL_CAMERA_RULE_AFTER_SNIPPET);

    /* Rejected lines are *consumed*: that is what stops a malformed header
     * from landing in the document as geometry. */
    ASSERT_INT("rejected is not NOT_CAMERA",
               (int)REPL_CAMERA_LINE_REJECTED != (int)REPL_CAMERA_LINE_NOT_CAMERA,
               1);
}

/* ----- brace scanning --------------------------------------------------- */

static void test_brace_delta(void) {
    printf("--- code_brace_delta fixtures ---\n");
    int block = 0;

    ASSERT_INT("plain open brace",
               repl_code_brace_delta("func0(r) {", &block), 1);
    ASSERT_INT("plain close brace", repl_code_brace_delta("}", &block), -1);
    ASSERT_INT("brace in a line comment",
               repl_code_brace_delta("glEnd(); // close the { block", &block), 0);
    ASSERT_INT("brace in a block comment on one line",
               repl_code_brace_delta("glEnd(); /* reset {x,y} */", &block), 0);
    ASSERT_INT("brace in a string literal",
               repl_code_brace_delta("label(\"a { brace\");", &block), 0);
    ASSERT_INT("brace in a string with an escaped quote",
               repl_code_brace_delta("label(\"a \\\" { brace\");", &block), 0);

    /* Block-comment state has to survive across lines: a brace inside a
     * multi-line comment would otherwise mis-scope every tag below it. */
    block = 0;
    ASSERT_INT("block comment opens", repl_code_brace_delta("/* start", &block), 0);
    ASSERT_INT("block comment stays open", block, 1);
    ASSERT_INT("brace inside the open comment",
               repl_code_brace_delta(" * a { brace", &block), 0);
    ASSERT_INT("block comment closes",
               repl_code_brace_delta(" */ func0(r) {", &block), 1);
    ASSERT_INT("block comment state cleared", block, 0);

    ASSERT_INT("a comment is not executable",
               repl_line_is_executable("// nothing here", 0), 0);
    ASSERT_INT("a blank line is not executable",
               repl_line_is_executable("   ", 0), 0);
    ASSERT_INT("a preprocessor directive is not executable",
               repl_line_is_executable("#include <GL/gl.h>", 0), 0);
    ASSERT_INT("a command is executable",
               repl_line_is_executable("glEnd();", 0), 1);
}

/* ----- deferred merge --------------------------------------------------- */

static void test_partial_pose_merges(void) {
    printf("--- camera partial pose merge ---\n");
    ReplCameraPose destination;
    ReplCameraFinish fin;
    int missing = 0;
    int i;

    destination.dist = 5.0f; destination.rx = 20.0f; destination.ry = 30.0f;
    destination.tx = 1.0f;   destination.ty = 2.0f;  destination.tz = 3.0f;
    camera_bridge_stub_install(&destination);

    hdr_reset();
    offer("glTranslatef(0, 0, -9);   // @camera dist");
    fin = repl_camera_header_finish(&g_hdr, REPL_CAMERA_APPLY_IMPORT);

    ASSERT_INT("a partial header still applies", fin.pose_applied, 1);
    ASSERT_TRUE("the seen role wins", fabsf(fin.pose.dist - 9.0f) < 1e-4f);
    ASSERT_TRUE("unseen roles keep the destination",
                fabsf(fin.pose.rx - 20.0f) < 1e-4f &&
                fabsf(fin.pose.ry - 30.0f) < 1e-4f &&
                fabsf(fin.pose.tx - 1.0f) < 1e-4f &&
                fabsf(fin.pose.ty - 2.0f) < 1e-4f &&
                fabsf(fin.pose.tz - 3.0f) < 1e-4f);
    ASSERT_INT("the bridge is called exactly once",
               g_camera_bridge_stub.apply_count, 1);
    ASSERT_INT("the mode reaches the bridge",
               (int)g_camera_bridge_stub.mode, REPL_CAMERA_APPLY_IMPORT);

    for (i = 0; i < g_hdr.diag_count; i++)
        if (g_hdr.diags[i].rule == REPL_CAMERA_RULE_MISSING_ROLE)
            missing++;
    ASSERT_INT("one note per missing pose role", missing, 3);
    ASSERT_INT("missing-role notes are notes, not warnings",
               (int)repl_camera_rule_severity(REPL_CAMERA_RULE_MISSING_ROLE),
               (int)REPL_CAMERA_SEVERITY_NOTE);
    ASSERT_INT("a rejection outranks a note",
               (int)repl_camera_rule_severity(REPL_CAMERA_RULE_ROLE_ORDER),
               (int)REPL_CAMERA_SEVERITY_REJECTION);
    ASSERT_INT("a non-zero g_angle initializer warns",
               (int)repl_camera_rule_severity(REPL_CAMERA_RULE_G_ANGLE_INIT),
               (int)REPL_CAMERA_SEVERITY_WARNING);
}

/* A spin-only header carries no pose, so nothing is applied at all. */
static void test_spin_alone_applies_nothing(void) {
    printf("--- camera spin-only header ---\n");
    ReplCameraFinish fin;
    int i, missing = 0;

    camera_bridge_stub_install(NULL);
    hdr_reset();
    offer("glRotatef(g_angle, 0, 1, 0);   // @camera spin");
    fin = repl_camera_header_finish(&g_hdr, REPL_CAMERA_APPLY_IMPORT);

    ASSERT_INT("no pose role means no bridge call", fin.pose_applied, 0);
    ASSERT_INT("no apply recorded", g_camera_bridge_stub.apply_count, 0);
    for (i = 0; i < g_hdr.diag_count; i++)
        if (g_hdr.diags[i].rule == REPL_CAMERA_RULE_MISSING_ROLE)
            missing++;
    ASSERT_INT("an absent spin is never a missing role", missing, 0);
}

/* No bridge installed (the standalone demo, most tests): validate and
 * diagnose as usual, apply nothing, and still report the resolved pose. */
static void test_bridgeless_finish(void) {
    printf("--- camera finish without a bridge ---\n");
    ReplCameraFinish fin;

    repl_export_install_camera_bridge(NULL);
    hdr_reset();
    offer("glTranslatef(0, 0, -7);   // @camera dist");
    offer("glRotatef(11, 1, 0, 0);   // @camera rx");
    offer("glRotatef(22, 0, 1, 0);   // @camera ry");
    offer("glTranslatef(0, 0, 0);   // @camera pan");
    fin = repl_camera_header_finish(&g_hdr, REPL_CAMERA_APPLY_IMPORT);

    ASSERT_INT("nothing applied without a bridge", fin.pose_applied, 0);
    ASSERT_TRUE("the resolved pose is still reported",
                fabsf(fin.pose.dist - 7.0f) < 1e-4f &&
                fabsf(fin.pose.rx - 11.0f) < 1e-4f &&
                fabsf(fin.pose.ry - 22.0f) < 1e-4f);
    ASSERT_INT("all four pose roles seen",
               (int)fin.seen_mask, (int)REPL_CAMERA_MASK_POSE);
}

static void test_diagnostic_overflow(void) {
    printf("--- camera diagnostic overflow ---\n");
    int i;

    hdr_reset();
    /* Every one of these is a duplicate after the first, so they all reject. */
    for (i = 0; i < REPL_CAMERA_MAX_DIAGS + 5; i++)
        offer("glTranslatef(0.0f, -2.5f, -10.0f);   // @camera dist");

    ASSERT_INT("stored diagnostics truncate at the cap",
               g_hdr.diag_count, REPL_CAMERA_MAX_DIAGS);
    ASSERT_INT("the overflow count is exact",
               g_hdr.diag_overflow, 5);
}

/* ----- document order --------------------------------------------------- */

typedef struct {
    int              count;
    ReplDocOrderRule rule;
    int              line_no;
    int              conflict;
} OrderRecord;

static void order_sink(void *userdata, ReplDocOrderRule rule, int line_no,
                       int conflict_line_no, const char *message) {
    OrderRecord *rec = (OrderRecord *)userdata;

    (void)message;
    if (rec->count == 0) {
        rec->rule     = rule;
        rec->line_no  = line_no;
        rec->conflict = conflict_line_no;
    }
    rec->count++;
}

static OrderRecord check_order(const char *const *lines) {
    ReplDocOrder ord;
    ReplCameraHeader hdr;
    OrderRecord rec;
    int i;

    memset(&rec, 0, sizeof(rec));
    repl_camera_header_init(&hdr);
    repl_doc_order_init(&ord);
    repl_doc_order_set_sink(&ord, order_sink, &rec);
    for (i = 0; lines[i]; i++) {
        ReplCameraLineResult r = repl_camera_header_offer(&hdr, lines[i], i + 1);
        (void)repl_doc_order_offer(&ord, lines[i], i + 1,
                                   r != REPL_CAMERA_LINE_NOT_CAMERA);
    }
    return rec;
}

static void test_document_order(void) {
    printf("--- canonical document order ---\n");

    static const char *const canonical[] = {
        "// @cfg axes = 4",
        "static float a, b;",
        "",
        "// helper",
        "func0(r) {",
        "  glVertex3f(r, 0, 0);",
        "}",
        "",
        "glTranslatef(0, 0, -4);   // @camera dist",
        "glRotatef(15, 1, 0, 0);   // @camera rx",
        "glRotatef(20, 0, 1, 0);   // @camera ry",
        "glTranslatef(0, 0, 0);   // @camera pan",
        "",
        "glClear(GL_COLOR_BUFFER_BIT);",
        "func0(1.0);",
        NULL
    };
    ASSERT_INT("the canonical shape passes", check_order(canonical).count, 0);

    /* The tolerated edge: camera straight after the declarations, functions
     * after it. */
    static const char *const camera_early[] = {
        "static float a;",
        "glTranslatef(0, 0, -4);   // @camera dist",
        "func0(r) {",
        "  glVertex3f(r, 0, 0);",
        "}",
        "glClear(GL_COLOR_BUFFER_BIT);",
        NULL
    };
    ASSERT_INT("DECLS -> CAMERA -> FUNCS -> BODY is accepted",
               check_order(camera_early).count, 0);

    static const char *const decl_late[] = {
        "glClear(GL_COLOR_BUFFER_BIT);",
        "static float a;",
        NULL
    };
    {
        OrderRecord rec = check_order(decl_late);
        ASSERT_INT("a declaration after body code is rejected", rec.count, 1);
        ASSERT_INT("... with the DECL_LATE rule",
                   (int)rec.rule, (int)REPL_DOC_ORDER_DECL_LATE);
        ASSERT_INT("... naming its own line", rec.line_no, 2);
        ASSERT_INT("... and the line that established the phase",
                   rec.conflict, 1);
    }

    static const char *const func_late[] = {
        "glClear(GL_COLOR_BUFFER_BIT);",
        "func0(r) {",
        "  glVertex3f(r, 0, 0);",
        "}",
        NULL
    };
    {
        OrderRecord rec = check_order(func_late);
        ASSERT_INT("a function definition after body code is rejected",
                   rec.count, 1);
        ASSERT_INT("... with the FUNC_LATE rule",
                   (int)rec.rule, (int)REPL_DOC_ORDER_FUNC_LATE);
    }

    static const char *const camera_late[] = {
        "glClear(GL_COLOR_BUFFER_BIT);",
        "glTranslatef(0, 0, -4);   // @camera dist",
        NULL
    };
    {
        OrderRecord rec = check_order(camera_late);
        ASSERT_INT("a camera row after body code is rejected", rec.count, 1);
        ASSERT_INT("... with the CAMERA_LATE rule",
                   (int)rec.rule, (int)REPL_DOC_ORDER_CAMERA_LATE);
    }

    /* Comments and blank lines carry no phase and are legal anywhere - that
     * single rule is what lets an author document any block. */
    static const char *const commented[] = {
        "// about the declarations",
        "static float a;",
        "// about the body",
        "",
        "glClear(GL_COLOR_BUFFER_BIT);",
        "// a trailing note",
        NULL
    };
    ASSERT_INT("comments and blanks never advance the phase",
               check_order(commented).count, 0);

    /* A compound literal contains a brace and is emphatically not a
     * definition. */
    static const char *const compound_literal[] = {
        "glClear(GL_COLOR_BUFFER_BIT);",
        "glFogfv(GL_FOG_COLOR, (GLfloat[]){0.05, 0.06, 0.08, 1});",
        NULL
    };
    ASSERT_INT("a compound literal is body code, not a definition",
               check_order(compound_literal).count, 0);

    /* Every violation is reported in one pass: the migration is one list, not
     * one edit-reload cycle per line. */
    static const char *const many[] = {
        "glClear(GL_COLOR_BUFFER_BIT);",
        "static float a;",
        "static float b;",
        "static float c;",
        NULL
    };
    ASSERT_INT("one pass reports every violation", check_order(many).count, 3);
}

int main(void) {
    printf("=== camera header reader ===\n");
    test_roles_parse();
    test_both_comment_syntaxes();
    test_marker_is_an_ordinary_comment();
    test_rejection_rules();
    test_spin_argument();
    test_negative_literals();
    test_duplicate_and_order();
    test_split_by_executable_line();
    test_baseline_depth();
    test_snippet_regions();
    test_brace_delta();
    test_partial_pose_merges();
    test_spin_alone_applies_nothing();
    test_bridgeless_finish();
    test_diagnostic_overflow();
    test_document_order();
    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "camera_header");
}
