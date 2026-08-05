/*
 * tests/test_export_trace_parity.c - cross-checks that running a REPL
 * program through repl_execute_program() and running the same program
 * through repl_export_save_output() + cc + execution produces the same
 * sequence of stub GL calls.
 *
 * Stub-only test (linked only when USE_GL_STUBS=1) because both legs
 * need the gl_stub_counts[] counters to be live.
 *
 * For each program in the curated table the test:
 *   1. feeds each line through editor_feed_line();
 *   2. writes the program to a temp file via repl_export_save_output();
 *   3. shells out to cc, compiling that file + the stubs + the trace
 *      driver into a child binary (with -Dmain=app_main so the exported
 *      file's GLUT main() is renamed out of the way);
 *   4. runs BOTH legs over g_time_samples as successive frames of one
 *      session - the REPL leg through repl_refresh_flat_program() (so the
 *      value-only rebake path is exercised, not just a full flatten), the
 *      child by re-assigning `t` and calling draw_scene() again;
 *   5. subtracts the fixed display()-boilerplate counts that the REPL
 *      executor never emits (g_display_header glClear / glLoadIdentity /
 *      glPushAttrib and g_footer_pre_init glPopAttrib);
 *   6. compares counters AND the per-call argument traces. Known fungible
 *      pairs like glColor3f vs glColor4f are reconciled from one table
 *      (g_fusion_pairs) on both sides, because the executor folds 3f into
 *      4f at execute time while the exporter writes 3f literally.
 *
 * Comparing the traces is what makes this a semantic test rather than a
 * structural one: counters agree whenever the same calls are made, so a
 * scene whose numbers are all wrong - a frozen vertex, a stale matrix -
 * used to pass here with every counter matching.
 *
 * Pass --full to walk every built-in example (slow: one cc invocation
 * per example). Default is the curated set, which covers the
 * representative bug-prone shapes (loop body in glBegin, function call,
 * GLUT solid, label, transforms).
 */
#include "editor/input.h"
#include "app/glr_ctrl.h"
#include "app/glr_state.h"

#include "repl/eval.h"
#include "repl/examples.h"
#include "repl/executor.h"
#include "repl/export.h"
#include "repl/pipeline.h"
#include "repl/state.h"
#include "repl/state_owners.h"
#include "support/scene_corpus.h"
#include "source_document.h"
#include "ui/app/state.h"

#include <GL/gl_stub_counts.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* This test runs its own four-bucket tally (PASS/FAIL/XFAIL/XPASS) and
 * doesn't use support/test_harness.h - the asserts-passed/total model
 * doesn't fit XFAIL/XPASS cleanly, and the exit code is what
 * scripts/run-tests.sh actually checks. */

#define g_status (ui_state_status_mut()->text)

typedef struct {
    const char *name;
    const char *const *lines;   /* NULL-terminated */
    const char *expected_fail;  /* NULL => no annotation; non-NULL => XFAIL
                                 * if traces mismatch, XPASS if they match. */
} TraceProgram;

/* Four-bucket parity result. PASS / FAIL behave the obvious way. XFAIL
 * is a mismatch that we've explicitly annotated as expected (won't fail
 * the test, won't dump diff noise). XPASS is a match for a case that
 * has an annotation: the annotation is now stale and should be removed,
 * so we treat it as a loud failure to keep the annotation list honest. */
typedef enum {
    PARITY_PASS,
    PARITY_FAIL,
    PARITY_XFAIL,
    PARITY_XPASS,
} ParityResult;

/* Curated programs. Each exercises a shape the executor / exporter
 * could plausibly disagree on. Keep them small - every entry triggers
 * one cc invocation. */
static const char *prog_triangle[] = {
    "glBegin(GL_TRIANGLES);",
    "glVertex3f(0, 0, 0);",
    "glVertex3f(1, 0, 0);",
    "glVertex3f(0, 1, 0);",
    "glEnd();",
    NULL
};

static const char *prog_loop_in_begin[] = {
    /* The exact bug shape: a for-loop body emitting glVertex inside
     * glBegin/glEnd. Pre-fix the executor's auto-close of glBegin
     * (triggered by glRasterPos3f) tore down the block and only the
     * first vertex survived. */
    "glBegin(GL_POINTS);",
    "for(i, 0, 5) {",
    "glVertex3f(i, 0, 0);",
    "}",
    "glEnd();",
    NULL
};

static const char *prog_color_normal[] = {
    "glBegin(GL_TRIANGLES);",
    "glColor3f(1, 0, 0);",
    "glNormal3f(0, 0, 1);",
    "glVertex3f(0, 0, 0);",
    "glColor4f(0, 1, 0, 0.5);",
    "glVertex3f(1, 0, 0);",
    "glVertex3f(0, 1, 0);",
    "glEnd();",
    NULL
};

static const char *prog_transforms[] = {
    "glPushMatrix();",
    "glRotatef(45, 0, 0, 1);",
    "glScalef(2, 2, 2);",
    "glTranslatef(0, 1, 0);",
    "glBegin(GL_LINES);",
    "glVertex3f(0, 0, 0);",
    "glVertex3f(1, 0, 0);",
    "glEnd();",
    "glPopMatrix();",
    NULL
};

static const char *prog_glut_cube[] = {
    "glEnable(GL_DEPTH_TEST);",
    "glutSolidCube(1);",
    NULL
};

static const char *prog_function_call[] = {
    "func0(z) {",
    "glVertex3f(0, 0, z);",
    "}",
    "glBegin(GL_POINTS);",
    "func0(1);",
    "func0(2);",
    "func0(3);",
    "glEnd();",
    NULL
};

static const char *prog_label_rasterpos[] = {
    /* label() and glRasterPos3f both outside glBegin (the parser rejects
     * them inside, see test_repl_core_commit). The exporter's label()
     * helper goes through vsnprintf + per-char glutBitmapCharacter,
     * matching the executor's CMD_LABEL handling. */
    "glRasterPos3f(0, 0, 0);",
    "label(\"hi\");",
    NULL
};

static const char *prog_stencil_mask[] = {
    /* The two-pass masking shape, and the only place the exported C of a
     * stencil scene is compiled at all: it proves the export writer emits
     * every stencil call (and that the prologue still compiles with them
     * present). Argument values are compared here too, so the masks and
     * ref values are pinned on both legs; the ref-truncation semantics
     * themselves live in test_repl_flatten_rebake.c and
     * test_repl_export_all_commands.c. */
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);",
    "glEnable(GL_STENCIL_TEST);",
    "glStencilFunc(GL_ALWAYS, 1, 0xFF);",
    "glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);",
    "glStencilMask(0xFF);",
    "glBegin(GL_TRIANGLES);",
    "glVertex3f(0, 0, 0);",
    "glVertex3f(1, 0, 0);",
    "glVertex3f(0, 1, 0);",
    "glEnd();",
    "glStencilMask(0x00);",
    "glStencilFunc(GL_EQUAL, 1, 0xFF);",
    "glutSolidCube(1);",
    "glDisable(GL_STENCIL_TEST);",
    NULL
};

static const char *prog_func_locals[] = {
    /* Function-scoped locals. This is the only place the generated
     * `float u = 0.0f;` declaration is actually handed to cc, so it is
     * what proves the export writer emits compilable C for them. The
     * read-before-write on the first vertex is deliberate: it is the case
     * the zero initializer exists for - bare `float u;` would be
     * undefined there, while the REPL binds it to 0. */
    "glBegin(GL_TRIANGLES);",
    "func0(k) {",
    "float u;",
    "glVertex3f(u, 0, 0);",
    "u = k;",
    "glVertex3f(u, 0, 0);",
    "}",
    "func0(1);",
    "func0(2);",
    "glEnd();",
    NULL
};

/* Everything the counters are structurally blind to, in one program:
 * a scratch matrix (glMultMatrixf(A) reads the array rather than any
 * expression slot, so its 16 cells only reach the flat command through a
 * snapshot), a time-varying cell, and a per-iteration write inside a loop.
 * Every t produces the same call sequence and the same counts here - only
 * the numbers move - so this case passes trivially without the trace
 * comparison and without the multi-t sampling. */
static const char *prog_scratch_matrix[] = {
    "A[0] = 1;",  "A[1] = 0;",         "A[2] = 0;",  "A[3] = 0;",
    "A[4] = sin(t) * 0.5;",            "A[5] = 1;",
    "A[6] = 0;",  "A[7] = 0;",
    "A[8] = 0;",  "A[9] = 0;",         "A[10] = 1;", "A[11] = 0;",
    "A[12] = 0;", "A[13] = 0;",        "A[14] = 0;", "A[15] = 1;",
    "for(k, 0, 3) {",
    "A[13] = cos(t + k);",
    "glPushMatrix();",
    "glMultMatrixf(A);",
    "glBegin(GL_POINTS);",
    "glVertex3f(k, 0, 0);",
    "glEnd();",
    "glPopMatrix();",
    "}",
    NULL
};

/* Material colors are an array arg too, and a scene that picks its
 * material by a time-driven branch keeps every counter fixed while the
 * values swing. */
static const char *prog_material_branch[] = {
    "float h;",
    "h = abs(sin(t)) * 2;",
    "if (h > 1.0) {",
    "glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, (GLfloat[]){0.9, 0.2, 0.3, 1.0});",
    "} else {",
    "glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, (GLfloat[]){0.2, 0.7, 0.9, 1.0});",
    "}",
    "glBegin(GL_POINTS);",
    "glVertex3f(h, 0, 0);",
    "glEnd();",
    NULL
};

static const TraceProgram g_curated[] = {
    { "triangle",            prog_triangle,        NULL },
    { "loop_in_begin",       prog_loop_in_begin,   NULL },
    { "color_normal",        prog_color_normal,    NULL },
    { "transforms",          prog_transforms,      NULL },
    { "glut_cube",           prog_glut_cube,       NULL },
    { "function_call",       prog_function_call,   NULL },
    { "label_rasterpos",     prog_label_rasterpos, NULL },
    { "stencil_mask",        prog_stencil_mask,    NULL },
    { "func_locals",         prog_func_locals,     NULL },
    { "scratch_matrix",      prog_scratch_matrix,  NULL },
    { "material_branch",     prog_material_branch, NULL },
};
static const int g_curated_count = (int)(sizeof(g_curated)/sizeof(g_curated[0]));

/* Expected-fail annotations for built-in examples (consulted in --full
 * mode). Keyed by repl_example_name(). A mismatch on an annotated
 * example is reported as XFAIL (quiet, doesn't fail the test). A *match*
 * on an annotated example is reported as XPASS, which DOES fail the
 * test - the annotation has gone stale and needs to be removed. Keeping
 * this table small and high-quality is the whole point. */
static const struct {
    const char *name;
    const char *reason;
} g_example_xfail[] = {
    { "Bezier curve with guides",
      "fractional loop step: the exported C runs one iteration more than "
      "the REPL (101 curve points vs 100). NOT a scene bug - both legs "
      "accumulate u += 0.01f identically; only the bound differs. flatten "
      "stops at `val < end - 1e-6f` (flatten.c, repl_flatten_range), "
      "because float accumulation lands u on 0.99999994 and a bare bound "
      "would run a 101st iteration nobody wrote; write_for_begin_as_c "
      "emits the bare `u < 1`. Any scene with a non-integer step hits it; "
      "this is just the only one in the catalog. Fixed once in a23047a6 "
      "(guard emitted into the exported for-header, stripped back off on "
      "import so the round trip stays text-stable) and reverted right "
      "after: the cost is `- 1e-6f` in every loop header of every file a "
      "user exports and reads, plus an import-side strip that has to undo "
      "the exporter's parens exactly, and that is too much surface to "
      "carry for one scene's 101st point. Revisit if a scene ever depends "
      "on the exact iteration count, or if the exported header gains a "
      "helper macro that can hide the guard." },
};
static const int g_example_xfail_count =
    (int)(sizeof(g_example_xfail)/sizeof(g_example_xfail[0]));

static const char *expected_fail_for_example(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_example_xfail_count; i++)
        if (strcmp(g_example_xfail[i].name, name) == 0)
            return g_example_xfail[i].reason;
    return NULL;
}

/* Counter pairs the executor folds together: CMD_COLOR3F -> glColor4f
 * (executor.c:422 routes 3f through glColor4f with g_execute_alpha_scale)
 * while the exporter emits glColor3f literally. Comparing as a sum
 * keeps both sides honest without bending one of them to the other. */
typedef struct { int a; int b; } StubFusionPair;
static const StubFusionPair g_fusion_pairs[] = {
    { GL_STUB_glColor3f, GL_STUB_glColor4f },
    /* CMD_MATERIALFV always parses as the compound-literal form, and the
     * executor then dispatches to whichever GL entry point matches the
     * unpacked count - glMaterialf for the one-float pnames like
     * GL_SHININESS (executor.c, CMD_MATERIALFV) - while the exporter
     * always writes glMaterialfv. Same GL state, different spelling; the
     * three arguments they do share must still agree. */
    { GL_STUB_glMaterialf, GL_STUB_glMaterialfv },
};
static const int g_fusion_pair_count =
    (int)(sizeof(g_fusion_pairs)/sizeof(g_fusion_pairs[0]));

static int stub_index_for_name(const char *name) {
    for (int i = 0; i < GL_STUB_COUNT_MAX; i++) {
        if (strcmp(gl_stub_count_name(i), name) == 0)
            return i;
    }
    return -1;
}

/* Reads the child's per-symbol dump (one "<name> <count>\n" line per
 * non-zero counter) into the target counter array. Unknown symbols are
 * ignored (the child only writes names from GL_STUB_COUNTER_LIST). */
static int read_child_counts(const char *path, unsigned long long *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    memset(out, 0, sizeof(unsigned long long) * GL_STUB_COUNT_MAX);
    char name[128];
    unsigned long long n;
    while (fscanf(f, "%127s %llu", name, &n) == 2) {
        int idx = stub_index_for_name(name);
        if (idx >= 0) out[idx] = n;
    }
    fclose(f);
    return 1;
}

/* Single-stage compile: the driver #includes the exported file via
 * -DEXPORTED_C so render_repl_geometry / reset_repl_vars (both static
 * in the exported TU) are visible. -Dmain=app_main renames the
 * exported file's GLUT main() out of the way; the driver's main()
 * is the real one. */
static int g_keep_traces = 0;  /* set by --keep-traces */

static void compose_compile_cmd(char *buf, size_t n,
                                const char *exported_c,
                                const char *bin_path,
                                const char *log_path) {
    snprintf(buf, n,
        "cc -Wall -std=c2x -O0 -g "
        "-DGL_STUBS -DGL_SILENCE_DEPRECATION -Wno-deprecated-declarations "
        "-Wno-unused-function -Wno-unused-variable "
        "-Itests/gl-stubs/include -Iinclude -I. -Isrc "
        "-DEXPORTED_C='\"%s\"' "
        "tests/export_trace_driver.c tests/gl-stubs/gl_stub_counts.c "
        "-lm -o '%s' >'%s' 2>&1",
        exported_c, bin_path, log_path);
}

/* ---- Trace (argument-value) comparison ---------------------------------
 *
 * Counters answer "were the same calls made"; they cannot answer "with the
 * same numbers". A frozen vertex and a moving one are one glVertex3f each,
 * so a scene can diverge completely from its exported C with every counter
 * in agreement - which is exactly how a REPL-only `fabs` (evaluating to 0.0f
 * while the exported C resolved libm's) and a stale glMultMatrixf payload
 * both stayed invisible here. The per-call traces both legs already write
 * carry the values; comparing them is the real assertion, with the counters
 * kept as the coarse first pass because their failure message is far easier
 * to read.
 *
 * Numeric fields compare with a tolerance instead of by text. The REPL bakes
 * each argument through its own float evaluator at flatten time while the
 * exported C recomputes the expression at runtime, so the two can legitimately
 * disagree in the last digit or two that %g prints. The tolerance is tight
 * enough that a genuinely different number cannot hide under it. */
#define TRACE_ABS_EPS 1e-5
#define TRACE_REL_EPS 1e-4
#define TRACE_MAX_FIELDS 24      /* glMultMatrixf: symbol + 16 cells */
#define TRACE_MAX_REPORTED 12    /* mismatch lines before truncating */

/* Whole-token equality: identical text (symbol names, integer codes), or
 * both parse as numbers that agree within tolerance. A token that is not
 * numeric on both sides only matches textually. */
static int trace_field_equal(const char *a, const char *b) {
    char *end_a = NULL, *end_b = NULL;
    double va, vb, diff, mag;

    if (strcmp(a, b) == 0)
        return 1;

    va = strtod(a, &end_a);
    vb = strtod(b, &end_b);
    if (!end_a || *end_a || end_a == a || !end_b || *end_b || end_b == b)
        return 0;   /* at least one side is not a bare number */

    diff = fabs(va - vb);
    if (diff <= TRACE_ABS_EPS)
        return 1;
    mag = fabs(va) > fabs(vb) ? fabs(va) : fabs(vb);
    return diff <= mag * TRACE_REL_EPS;
}

static int trace_split(char *line, char *fields[], int max) {
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, " \t\r\n", &save);
         tok && n < max;
         tok = strtok_r(NULL, " \t\r\n", &save))
        fields[n++] = tok;
    return n;
}

/* The counter fusion has a trace twin. The executor routes CMD_COLOR3F
 * through glColor4f while the exporter writes glColor3f literally, so for
 * one source line the two legs name different symbols - g_fusion_pairs is
 * the same table compare_counts sums over. Treat such a pair as the same
 * call and compare only the arguments they share: the alpha the executor
 * appends has no counterpart in the C the exporter wrote, so demanding it
 * match would fail every colored scene. */
static int trace_symbols_fused(const char *a, const char *b) {
    for (int i = 0; i < g_fusion_pair_count; i++) {
        const char *na = gl_stub_count_name(g_fusion_pairs[i].a);
        const char *nb = gl_stub_count_name(g_fusion_pairs[i].b);
        if ((strcmp(a, na) == 0 && strcmp(b, nb) == 0) ||
            (strcmp(a, nb) == 0 && strcmp(b, na) == 0))
            return 1;
    }
    return 0;
}

static int trace_lines_equal(const char *a_line, const char *b_line) {
    char a_buf[512], b_buf[512];
    char *a_fields[TRACE_MAX_FIELDS], *b_fields[TRACE_MAX_FIELDS];
    int a_n, b_n, shared;

    snprintf(a_buf, sizeof a_buf, "%s", a_line);
    snprintf(b_buf, sizeof b_buf, "%s", b_line);
    a_n = trace_split(a_buf, a_fields, TRACE_MAX_FIELDS);
    b_n = trace_split(b_buf, b_fields, TRACE_MAX_FIELDS);

    if (a_n == 0 || b_n == 0)
        return a_n == b_n;

    if (strcmp(a_fields[0], b_fields[0]) != 0) {
        if (!trace_symbols_fused(a_fields[0], b_fields[0]))
            return 0;
        /* Fused: argument lists may differ in length by the folded-in
         * trailing arg. Everything they share must still agree. */
        shared = a_n < b_n ? a_n : b_n;
    } else {
        if (a_n != b_n)
            return 0;
        shared = a_n;
    }

    for (int i = 1; i < shared; i++)
        if (!trace_field_equal(a_fields[i], b_fields[i]))
            return 0;
    return 1;
}

/* Walk both trace files in lockstep. Returns 1 when every line agrees.
 * `details` behaves like compare_counts': stderr to print, NULL to stay
 * silent for XFAIL cases. Reports at most TRACE_MAX_REPORTED lines - past
 * that the traces have desynchronized and the remaining diff is noise the
 * FAIL branch's diff(1) hunk shows better. */
static int compare_traces(const char *case_name,
                          const char *repl_path, const char *child_path,
                          FILE *details) {
    FILE *fa = fopen(repl_path, "r");
    FILE *fb = fopen(child_path, "r");
    char la[512], lb[512];
    int line_no = 0, mismatches = 0;

    if (!fa || !fb) {
        if (details)
            fprintf(details, "  [%s] trace unreadable: %s\n", case_name,
                    !fa ? repl_path : child_path);
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }

    for (;;) {
        char *ga = fgets(la, sizeof la, fa);
        char *gb = fgets(lb, sizeof lb, fb);

        if (!ga && !gb)
            break;
        line_no++;
        if (!ga || !gb) {
            if (details)
                fprintf(details,
                        "  [%s] trace length differs at line %d: %s ended first\n",
                        case_name, line_no, !ga ? "repl" : "child");
            mismatches++;
            break;
        }
        if (!trace_lines_equal(la, lb)) {
            if (mismatches < TRACE_MAX_REPORTED && details) {
                la[strcspn(la, "\n")] = '\0';
                lb[strcspn(lb, "\n")] = '\0';
                fprintf(details,
                        "  [%s] trace line %d: repl=\"%s\" child=\"%s\"\n",
                        case_name, line_no, la, lb);
            }
            mismatches++;
        }
    }

    fclose(fa);
    fclose(fb);
    if (mismatches > TRACE_MAX_REPORTED && details)
        fprintf(details, "  [%s] ... %d more trace mismatches\n",
                case_name, mismatches - TRACE_MAX_REPORTED);
    return mismatches == 0;
}

/* Compare REPL-side and child counts. Returns 1 on full parity,
 * 0 on any mismatch. If `details` is non-NULL, one line per mismatched
 * counter is written to it; callers pass stderr to print, NULL to stay
 * silent (used for XFAIL cases so the diff noise stays quiet). */
static int compare_counts(const char *case_name,
                          const unsigned long long *repl_counts,
                          const unsigned long long *child_counts,
                          FILE *details) {
    int mismatches = 0;

    /* Walk fused pairs first: any counter that appears in a fusion pair
     * is excluded from the per-counter loop below. */
    char fused[GL_STUB_COUNT_MAX] = {0};
    for (int p = 0; p < g_fusion_pair_count; p++) {
        int a = g_fusion_pairs[p].a;
        int b = g_fusion_pairs[p].b;
        fused[a] = 1; fused[b] = 1;
        unsigned long long repl_sum  = repl_counts[a] + repl_counts[b];
        unsigned long long child_sum = child_counts[a] + child_counts[b];
        if (repl_sum != child_sum) {
            if (details) fprintf(details,
                "  [%s] fused %s+%s mismatch: repl=%llu child=%llu\n",
                case_name,
                gl_stub_count_name(a), gl_stub_count_name(b),
                repl_sum, child_sum);
            mismatches++;
        }
    }

    for (int i = 0; i < GL_STUB_COUNT_MAX; i++) {
        if (fused[i]) continue;
        if (repl_counts[i] != child_counts[i]) {
            if (details) fprintf(details,
                "  [%s] %s mismatch: repl=%llu child=%llu\n",
                case_name, gl_stub_count_name(i),
                repl_counts[i], child_counts[i]);
            mismatches++;
        }
    }
    return mismatches == 0;
}

/* Time samples both legs run, in order. A single frame at t=0 cannot see
 * anything that only goes wrong once time advances - the REPL's value-only
 * rebake path, for one, is not even reached until the second frame, so a
 * flat program that goes stale there looked perfect here. Extra samples
 * cost one child process and one REPL execute each; the compile, which
 * dominates the runtime, happens once for all of them. */
static const float g_time_samples[] = { 0.0f, 0.75f, 2.5f };
static const int g_time_sample_count =
    (int)(sizeof(g_time_samples)/sizeof(g_time_samples[0]));

/* Feed the program into a freshly-reset REPL. `z` and `i` are declared for
 * the curated programs that use them as plain scene variables. */
static void feed_program(const TraceProgram *prog) {
    char err[128];

    glr_ctrl_reset_all();
    repl_eval_declare_predef_var("z", err, sizeof err);
    repl_eval_declare_predef_var("i", err, sizeof err);
    g_status[0] = '\0';

    for (int li = 0; prog->lines[li]; li++)
        editor_feed_line(prog->lines[li]);
}

/* The REPL leg: one session, one frame per time sample, into a single
 * trace and a single set of cumulative counters - the shape the child
 * binary runs too.
 *
 * The refresh goes through repl_refresh_flat_program(), not a direct
 * repl_flatten_commands(), because that boundary is what the app calls and
 * it is the thing under test. It full-flattens the first frame and then
 * routes a value-only `t` change to the in-place rebake, so a rebake that
 * fails to update some baked value diverges from the exported C on frame 2
 * while frame 1 looks perfect. Calling flatten directly would rebuild the
 * program every frame and never exercise that path at all. */
static void run_repl_frames(const TraceProgram *prog,
                            const char *trace_path,
                            unsigned long long *counts_out) {
    feed_program(prog);

    /* Init+destroy bracket the executor's gluNewTess / gluTessCallback /
     * gluDeleteTess setup - these are REPL-internal, not user-emitted GL,
     * so reset counters AFTER init and snapshot BEFORE destroy to keep
     * the trace user-code-only. The trace file opens between reset and
     * execute for the same reason. */
    repl_executor_init_resources();
    gl_stub_counts_reset();
    gl_stub_trace_open(trace_path);
    for (int frame = 0; frame < g_time_sample_count; frame++) {
        repl_state_time_set(g_time_samples[frame]);
        repl_refresh_flat_program(editor_state_edit_line());
        gl_stub_trace_mark(frame, (double)g_time_samples[frame]);
        repl_execute_program(NULL);
    }
    gl_stub_trace_close();
    memcpy(counts_out, gl_stub_counts,
           sizeof(unsigned long long) * GL_STUB_COUNT_MAX);
    repl_executor_destroy_resources();
}

/* Run one program through both legs and compare. Returns the four-
 * bucket ParityResult so the caller can tally PASS/FAIL/XFAIL/XPASS.
 *
 * Infrastructure failures (compile error, child error, unreadable child
 * output) always return PARITY_FAIL regardless of the case's XFAIL
 * annotation - those are bugs in the test, not in the program, and
 * silencing them would defeat the point of the test. */
static ParityResult run_one_case(const TraceProgram *prog) {
    pid_t pid = getpid();
    /* Example names can contain spaces, parens, slashes - anything goes
    * via repl_example_name(). Build a safe path stem by mapping every
     * non-alnum/dot/dash to '_'. Keeps the temp paths immune to shell
     * quoting surprises across the system() compile + run invocations. */
    char safe_name[128];
    {
        size_t out_idx = 0;
        for (const char *p = prog->name; *p && out_idx + 1 < sizeof safe_name; p++) {
            unsigned char c = (unsigned char)*p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '-')
                safe_name[out_idx++] = (char)c;
            else
                safe_name[out_idx++] = '_';
        }
        safe_name[out_idx] = '\0';
    }
    char temp_c[256], temp_bin[256], temp_out[256], temp_log[256];
    char temp_repl_tr[256], temp_child_tr[256];
    snprintf(temp_c,        sizeof temp_c,        "/tmp/test_trace_%d_%s.c",         (int)pid, safe_name);
    snprintf(temp_bin,      sizeof temp_bin,      "/tmp/test_trace_%d_%s.bin",       (int)pid, safe_name);
    snprintf(temp_out,      sizeof temp_out,      "/tmp/test_trace_%d_%s.txt",       (int)pid, safe_name);
    snprintf(temp_log,      sizeof temp_log,      "/tmp/test_trace_%d_%s.log",       (int)pid, safe_name);
    snprintf(temp_repl_tr,  sizeof temp_repl_tr,  "/tmp/test_trace_%d_%s.repl.tr",   (int)pid, safe_name);
    snprintf(temp_child_tr, sizeof temp_child_tr, "/tmp/test_trace_%d_%s.child.tr",  (int)pid, safe_name);

    /* Feed once so there is a document to export. Each t sample re-feeds
     * from scratch below; this call only has to leave the document live. */
    feed_program(prog);

    /* Export leg. */
    repl_export_save_output(temp_c, source_document_view(), NULL);

    char cmd[1024];
    compose_compile_cmd(cmd, sizeof cmd, temp_c, temp_bin, temp_log);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr,
            "  [%s] compile failed (rc=%d). cmd:\n    %s\n  log: %s\n",
            prog->name, rc, cmd, temp_log);
        return PARITY_FAIL;
    }

    /* Standalone compilation and execution test (Priority 8) */
    {
        char standalone_bin[256], standalone_log[256];
        snprintf(standalone_bin, sizeof standalone_bin, "/tmp/test_trace_%d_%s_standalone.bin", (int)pid, safe_name);
        snprintf(standalone_log, sizeof standalone_log, "/tmp/test_trace_%d_%s_standalone.log", (int)pid, safe_name);
        char standalone_cmd[1024];
        snprintf(standalone_cmd, sizeof standalone_cmd,
            "cc -Wall -std=c99 -O0 -g "
            "-DGL_STUBS -DGL_SILENCE_DEPRECATION -Wno-deprecated-declarations "
            "-Wno-unused-function -Wno-unused-variable "
            "-Itests/gl-stubs/include -Iinclude -I. -Isrc "
            "'%s' tests/gl-stubs/gl_stub_counts.c -lm -o '%s' >'%s' 2>&1",
            temp_c, standalone_bin, standalone_log);
        int rc_standalone = system(standalone_cmd);
        if (rc_standalone != 0) {
            fprintf(stderr, "  [%s] Standalone compilation failed (rc=%d). cmd:\n    %s\n  log: %s\n",
                    prog->name, rc_standalone, standalone_cmd, standalone_log);
            unlink(standalone_log);
            return PARITY_FAIL;
        }
        char run_cmd[512];
        snprintf(run_cmd, sizeof run_cmd, "'%s' >/dev/null 2>&1", standalone_bin);
        int run_rc = system(run_cmd);
        if (run_rc != 0) {
            fprintf(stderr, "  [%s] Standalone binary execution failed (rc=%d).\n", prog->name, run_rc);
            unlink(standalone_bin);
            unlink(standalone_log);
            return PARITY_FAIL;
        }
        unlink(standalone_bin);
        unlink(standalone_log);
    }

    /* Both legs now run every time sample as successive frames of one
     * session, so each produces a single multi-frame trace and one set of
     * cumulative counters. */
    unsigned long long repl_counts[GL_STUB_COUNT_MAX];
    unsigned long long child_counts[GL_STUB_COUNT_MAX];

    run_repl_frames(prog, temp_repl_tr, repl_counts);

    {
        int used = snprintf(cmd, sizeof cmd, "'%s' '%s' '%s'",
                            temp_bin, temp_out, temp_child_tr);
        for (int s = 0; s < g_time_sample_count && used > 0 &&
                        used < (int)sizeof cmd; s++)
            used += snprintf(cmd + used, sizeof cmd - (size_t)used, " %.9g",
                             (double)g_time_samples[s]);
    }
    rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  [%s] child rc=%d\n", prog->name, rc);
        return PARITY_FAIL;
    }

    if (!read_child_counts(temp_out, child_counts)) {
        fprintf(stderr, "  [%s] child output unreadable: %s\n",
                prog->name, temp_out);
        return PARITY_FAIL;
    }

    /* Silent comparison first so XFAIL cases stay quiet. We'll re-call
     * with stderr for FAIL cases below to print the per-counter mismatch
     * lines. Counts run first because "glVertex3f: repl=6 child=5"
     * localizes a structural divergence far faster than the first
     * differing trace line does; the traces then catch what the counters
     * are blind to, which is every wrong *value*. */
    int match = compare_counts(prog->name, repl_counts, child_counts, NULL) &&
                compare_traces(prog->name, temp_repl_tr, temp_child_tr, NULL);

    ParityResult result;
    if (match) {
        result = prog->expected_fail ? PARITY_XPASS : PARITY_PASS;
    } else {
        result = prog->expected_fail ? PARITY_XFAIL : PARITY_FAIL;
    }

    /* Output dispatch by bucket:
     *   PASS  - silent (only the run-counter prints later).
     *   XFAIL - one info line so the divergence stays visible, no diff.
     *   FAIL  - counter-mismatch lines + a unified diff hunk.
     *   XPASS - explicit "annotation stale" callout; no diff (the
     *           traces matched). */
    switch (result) {
    case PARITY_PASS:
        break;
    case PARITY_XFAIL:
        fprintf(stderr, "  XFAIL [%s] expected: %s\n",
                prog->name, prog->expected_fail);
        break;
    case PARITY_FAIL: {
        /* The `# frame N t V` marker lines locate the divergence in time;
         * a mismatch that first appears after frame 0 is the signature of
         * a stale value carried across frames rather than a wrong one
         * computed from the start. */
        (void)compare_counts(prog->name, repl_counts, child_counts, stderr);
        (void)compare_traces(prog->name, temp_repl_tr, temp_child_tr, stderr);
        /* Color only for a terminal: --color=always would otherwise write
         * escape codes into the log scripts/run-tests.sh captures. And
         * reset afterwards regardless - `head` truncates the diff at 50
         * lines, which can cut the stream after a color is set and before
         * diff's own reset, leaving the terminal stuck in that color. */
        int diff_color = isatty(STDERR_FILENO);
        char diff_cmd[1024];
        /* diff's stderr is dropped because closing the pipe early is the
         * plan, not a fault: head exits at 50 lines and diff then reports
         * "stdout: Broken pipe", which reads like a failure in the test. */
        snprintf(diff_cmd, sizeof diff_cmd,
                 "diff %s -u '%s' '%s' 2>/dev/null | head -50 >&2",
                 diff_color ? "--color=always" : "",
                 temp_repl_tr, temp_child_tr);
        fprintf(stderr, "  [%s] trace diff (repl - / child +):\n",
                prog->name);
        (void)system(diff_cmd);
        if (diff_color) {
            fputs("\033[0m", stderr);
            fflush(stderr);
        }
        break;
    }
    case PARITY_XPASS:
        fprintf(stderr,
                "  XPASS [%s] annotation is now stale, remove the "
                "g_example_xfail entry. Old reason: %s\n",
                prog->name, prog->expected_fail);
        break;
    }

    /* Best-effort cleanup. Keep trace files only for FAIL+--keep-traces;
     * XFAIL traces would just be noise (the divergence is expected). */
    unlink(temp_c); unlink(temp_bin); unlink(temp_out); unlink(temp_log);
    if (!g_keep_traces || result != PARITY_FAIL) {
        unlink(temp_repl_tr); unlink(temp_child_tr);
    } else {
        fprintf(stderr, "  [%s] traces kept: %s %s\n",
                prog->name, temp_repl_tr, temp_child_tr);
    }
    return result;
}

typedef struct {
    int pass, fail, xfail, xpass;
} ParityTotals;

static void tally(ParityTotals *t, ParityResult r) {
    switch (r) {
    case PARITY_PASS:  t->pass++;  break;
    case PARITY_FAIL:  t->fail++;  break;
    case PARITY_XFAIL: t->xfail++; break;
    case PARITY_XPASS: t->xpass++; break;
    }
}

/* --full: walk repl_example_*. Each example is constructed on the fly
 * from the (name, lines) pair plus an XFAIL annotation looked up in
 * g_example_xfail. */
static const char *parity_result_label(ParityResult r) {
    switch (r) {
    case PARITY_PASS:  return "PASS ";
    case PARITY_FAIL:  return "FAIL ";
    case PARITY_XFAIL: return "XFAIL";
    case PARITY_XPASS: return "XPASS";
    }
    return "?????";
}

/* One line per case, naming the corpus it came from. A bare "49/50 passed"
 * never said *what* the 50 were, and the two corpora this walks are easy to
 * confuse: --full adds the built-in catalog compiled into the binary, not
 * the .glr corpora under tests/scenes (those are `make test-scenes`). */
static void run_and_report(ParityTotals *totals, const TraceProgram *prog,
                           const char *corpus) {
    ParityResult r = run_one_case(prog);

    printf("  %s %-9s %s\n", parity_result_label(r), corpus, prog->name);
    fflush(stdout);
    tally(totals, r);
}

static void run_examples(ParityTotals *totals) {
    int n = repl_example_count();
    for (int i = 0; i < n; i++) {
        const char *name = repl_example_name(i);
        const char *const *lines = repl_example_lines(i);
        TraceProgram p = { name, lines, expected_fail_for_example(name) };
        run_and_report(totals, &p, "example");
    }
}

/* Walk a runtime .glr catalog - the corpora under tests/scenes - through the
 * same comparison. repl_example_lines() serves a runtime catalog exactly as
 * it serves the compiled-in one, so the only extra care is the source
 * format: a `.c` entry is a complete exported file that has to go through
 * the import path, and feeding it as REPL source would produce a
 * meaningless failure. Report those rather than run them.
 *
 * The runtime catalog replaces the built-in one while loaded, so callers run
 * this after any --full pass and clear it here. */
static void run_scene_dir(ParityTotals *totals, const char *dir) {
    const char *tag = repl_test_scene_corpus_tag(dir);
    char err[512];
    int n;

    err[0] = '\0';
    if (!repl_examples_load_dir(dir, err, sizeof err)) {
        fprintf(stderr, "  FAIL  %-9s catalog load failed for %s: %s\n",
                tag, dir, err[0] ? err : "unknown error");
        totals->fail++;
        return;
    }

    n = repl_example_count();
    printf("--- scene corpus %s: %d scenes ---\n", dir, n);
    for (int i = 0; i < n; i++) {
        const char *name = repl_example_name(i);
        TraceProgram p = { name, repl_example_lines(i),
                           expected_fail_for_example(name) };

        if (repl_example_source_format(i) != REPL_EXAMPLE_SOURCE_GLR) {
            printf("  SKIP  %-9s %s (exported-C entry; needs the import "
                   "path, not editor_feed_line)\n", tag, name);
            continue;
        }
        run_and_report(totals, &p, tag);
    }
    repl_examples_clear_runtime_catalog();
}

static void print_help(const char *argv0) {
    printf(
"Usage: %s [--full] [--help]\n"
"\n"
"Cross-checks REPL execution against the exported C by running both and\n"
"comparing the stub GL calls they make - the per-symbol counts AND the\n"
"argument values of every call.\n"
"\n"
"For each program the test:\n"
"  1. feeds source lines through editor_feed_line();\n"
"  2. calls repl_export_save_output() to a temp file, shells out to cc\n"
"     to compile that file together with tests/export_trace_driver.c\n"
"     (which #includes the exported file so its static helpers\n"
"     render_repl_geometry / reset_repl_vars are visible);\n"
"  3. runs both legs over several t values, as successive frames of one\n"
"     session, and compares counts then traces.\n"
"\n"
"Values, not just counts: a frozen vertex and a moving one are one\n"
"glVertex3f apiece, so counts alone pass a scene whose every number is\n"
"wrong. Numeric fields compare within a tolerance because the REPL bakes\n"
"values through its float evaluator while the exported C recomputes them.\n"
"\n"
"Several frames, not one: the REPL re-uses its flat program across frames\n"
"and only re-bakes the values, so a bake that goes stale is invisible on\n"
"frame 1 and shows up on frame 2. The traces carry `# frame N t V`\n"
"markers so a divergence reports when in time it started.\n"
"\n"
"glColor3f and glColor4f are reconciled on both sides because the\n"
"executor folds CMD_COLOR3F through glColor4f at execute time while\n"
"the exporter emits glColor3f literally; the trace comparison matches\n"
"them over their shared arguments and ignores the appended alpha.\n"
"\n"
"On mismatch the test prints the differing counters and trace lines, then\n"
"runs diff(1) on the two trace files and pipes the first 50 lines of the\n"
"unified diff to stderr to localize the divergence.\n"
"\n"
"Cases listed in g_example_xfail (in the test source) are treated as\n"
"expected-to-fail. Buckets:\n"
"  PASS  - match, no annotation.\n"
"  FAIL  - mismatch, no annotation; fails the test, dumps diff.\n"
"  XFAIL - mismatch, has annotation; quiet info line, doesn't fail.\n"
"  XPASS - match, has annotation; the annotation is stale, remove it.\n"
"          Fails the test (loud) so the list doesn't accumulate cruft.\n"
"\n"
"Every case prints one PASS/FAIL/XFAIL/XPASS line naming its corpus, and\n"
"the run opens with what it covers and closes with what it does not: the\n"
".glr corpora under tests/scenes are a separate lane (`make test-scenes`)\n"
"and are never walked here, --full or not.\n"
"\n"
"Options:\n"
"  --full          After the curated table, also run every built-in\n"
"                  example - the catalog compiled into the binary from\n"
"                  examples/catalog.ini, via repl_example_*. Slow: one cc\n"
"                  invocation per program; traces kept on failure.\n"
"  --scenes-dir D  Also walk the .glr catalog in directory D (a runtime\n"
"                  catalog, as `--examples-dir` loads). Repeatable. Runs\n"
"                  after --full, since a runtime catalog displaces the\n"
"                  built-in one while loaded.\n"
"  --keep-traces   On real FAIL, leave the .repl.tr and .child.tr trace\n"
"                  files in /tmp for inspection. XFAIL traces are still\n"
"                  unlinked (the divergence is expected).\n"
"  --help          Show this help and exit 0.\n"
"\n"
"Environment:\n"
"  This binary reads none. The compiler is a hardcoded `cc` (see\n"
"  compose_compile_cmd), unlike test_repl_core_examples, which honors\n"
"  REPL_EXPORT_CC and REPL_EXPORT_COMPILE_CFLAGS.\n"
"\n"
"  REPL_SCENE_CORPUS=1 walks the standard corpora under tests/scenes\n"
"  (the same pair test_repl_core_examples and test_camera_header_parity\n"
"  gate on it, listed in tests/support/scene_corpus.h), equivalent to\n"
"  passing --scenes-dir for each. An explicit --scenes-dir overrides it.\n"
"  Unset, no corpus is walked and only the curated table runs.\n"
"\n"
"Notes:\n"
"  Stub-only test (linked only when USE_GL_STUBS=1) because both legs\n"
"  need gl_stub_counts[] live. The cc invocation expects to run from\n"
"  the repo root; make test runs tests from there.\n",
        argv0 ? argv0 : "test_export_trace_parity");
}

int main(int argc, char **argv) {
    int full = 0;
    /* Explicit --scenes-dir wins; REPL_SCENE_CORPUS asks for the standard
     * pair, which is what `make test-scenes` sets. */
    const char *scene_dirs[16];
    int scene_dir_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--full") == 0) {
            full = 1;
        } else if (strcmp(argv[i], "--scenes-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--scenes-dir needs a directory\n");
                return 2;
            }
            if (scene_dir_count >=
                (int)(sizeof scene_dirs / sizeof scene_dirs[0])) {
                fprintf(stderr, "too many --scenes-dir arguments\n");
                return 2;
            }
            scene_dirs[scene_dir_count++] = argv[++i];
        } else if (strcmp(argv[i], "--keep-traces") == 0) {
            g_keep_traces = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            print_help(argv[0]);
            return 2;
        }
    }

    repl_eval_init_predef_vars();
    ui_state_viewport_set_size(1200, 800);

    /* The executor never calls glPointParameterfv directly - it goes through
     * a proc the controller resolves once the GL context is current, and
     * with none installed CMD_POINT_PARAMETER_FV is silently a no-op. The
     * exported C calls the symbol literally, so without this the two legs
     * disagree by construction on every point-attenuation scene. Installing
     * the stub entry point is what the controller does for a real context. */
    repl_executor_install_point_parameter_proc(glPointParameterfv);

    /* State the coverage up front, so a run is self-describing and the
     * corpora that are NOT walked here are named rather than assumed. */
    printf("--- export trace parity: %d curated programs%s ---\n",
           g_curated_count,
           full ? ", plus every built-in example" : " (pass --full for the "
                  "built-in examples too)");
    printf("--- %d frames per case at t =", g_time_sample_count);
    for (int i = 0; i < g_time_sample_count; i++)
        printf(" %g", (double)g_time_samples[i]);
    printf("; comparing call counts and argument values ---\n");

    ParityTotals totals = {0};
    for (int i = 0; i < g_curated_count; i++) {
        run_and_report(&totals, &g_curated[i], "curated");
    }
    if (full) {
        printf("--- built-in example catalog: %d scenes (compiled in from "
               "examples/catalog.ini) ---\n", repl_example_count());
        run_examples(&totals);
    }
    /* Scene corpora last: loading a runtime catalog displaces the built-in
     * one, so --full has to have finished with it first. */
    if (scene_dir_count == 0 && repl_test_scene_corpus_enabled()) {
        const char *const *dirs = repl_test_scene_corpus_dirs();
        for (int i = 0; dirs[i]; i++)
            run_scene_dir(&totals, dirs[i]);
    }
    for (int i = 0; i < scene_dir_count; i++)
        run_scene_dir(&totals, scene_dirs[i]);
    if (scene_dir_count == 0 && !repl_test_scene_corpus_enabled())
        printf("--- scene corpora not walked (REPL_SCENE_CORPUS unset; see "
               "--scenes-dir and `make test-scenes`) ---\n");

    int total = totals.pass + totals.fail + totals.xfail + totals.xpass;
    int ok    = totals.pass + totals.xfail;
    /* "%d/%d passed" shape so scripts/run-tests.sh picks up the
     * pass/total count for its summary; the bucket breakdown trails. */
    printf("test_export_trace_parity: %d/%d passed"
           " (pass=%d fail=%d xfail=%d xpass=%d)\n",
           ok, total,
           totals.pass, totals.fail, totals.xfail, totals.xpass);
    /* Real failures and stale XPASS annotations both fail the test.
     * XFAIL is the silent OK bucket the user can grow when a divergence
     * is understood and triaged; XPASS keeps that list honest. */
    return (totals.fail + totals.xpass) > 0 ? 1 : 0;
}
