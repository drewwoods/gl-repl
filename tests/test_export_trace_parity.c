/*
 * tests/test_export_trace_parity.c — cross-checks that running a REPL
 * program through repl_execute_program() and running the same program
 * through repl_export_save_output() + cc + execution produces the same
 * sequence of stub GL calls.
 *
 * Stub-only test (linked only when USE_GL_STUBS=1) because both legs
 * need the gl_stub_counts[] counters to be live.
 *
 * For each program in the curated table the test:
 *   1. resets the REPL, feeds each line through editor_feed_line(),
 *      flattens, calls repl_execute_program(), and snapshots
 *      gl_stub_counts into repl_counts[];
 *   2. writes the program to a temp file via repl_export_save_output();
 *   3. shells out to cc, compiling that file + the stubs + the trace
 *      driver into a child binary (with -Dmain=app_main so the exported
 *      file's GLUT main() is renamed out of the way);
 *   4. runs the child, parses the per-symbol counts it dumps;
 *   5. subtracts the fixed display()-boilerplate counts that the REPL
 *      executor never emits (g_display_header glClear / glLoadIdentity /
 *      glPushAttrib and g_footer_pre_init glPopAttrib);
 *   6. compares — known fungible pairs like glColor3f vs glColor4f get
 *      summed before comparing because the executor folds 3f into 4f at
 *      execute time.
 *
 * Pass --full to walk every built-in example (slow: one cc invocation
 * per example). Default is the curated set, which covers the
 * representative bug-prone shapes (loop body in glBegin, function call,
 * GLUT solid, label, transforms).
 */
#include "editor/input.h"
#include "app/glr_ctrl.h"
#include "app/glr_state.h"
#include "repl/core.h"
#include "repl/core_internal.h"
#include "repl/examples.h"
#include "repl/executor.h"
#include "repl/export.h"
#include "repl/pipeline.h"
#include "repl/state.h"
#include "source_document.h"
#include "ui/state.h"

#include <GL/gl_stub_counts.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* This test runs its own four-bucket tally (PASS/FAIL/XFAIL/XPASS) and
 * doesn't use support/test_harness.h — the asserts-passed/total model
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
 * could plausibly disagree on. Keep them small — every entry triggers
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

static const TraceProgram g_curated[] = {
    { "triangle",            prog_triangle,        NULL },
    { "loop_in_begin",       prog_loop_in_begin,   NULL },
    { "color_normal",        prog_color_normal,    NULL },
    { "transforms",          prog_transforms,      NULL },
    { "glut_cube",           prog_glut_cube,       NULL },
    { "function_call",       prog_function_call,   NULL },
    { "label_rasterpos",     prog_label_rasterpos, NULL },
};
static const int g_curated_count = (int)(sizeof(g_curated)/sizeof(g_curated[0]));

/* Expected-fail annotations for built-in examples (consulted in --full
 * mode). Keyed by repl_examples_name(). A mismatch on an annotated
 * example is reported as XFAIL (quiet, doesn't fail the test). A *match*
 * on an annotated example is reported as XPASS, which DOES fail the
 * test — the annotation has gone stale and needs to be removed. Keeping
 * this table small and high-quality is the whole point. */
static const struct {
    const char *name;
    const char *reason;
} g_example_xfail[] = {
    { "Bezier curve with guides",
      "for(u, 0, 1, 0.01) accumulator drifts past 1.0 in REPL eval vs "
      "native float loop; one side runs an extra iteration. Fix: use "
      "an integer counter and compute u = k * 0.01 inside the body." },
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

/* Counter pairs the executor folds together: CMD_COLOR3F → glColor4f
 * (executor.c:422 routes 3f through glColor4f with g_execute_alpha_scale)
 * while the exporter emits glColor3f literally. Comparing as a sum
 * keeps both sides honest without bending one of them to the other. */
typedef struct { int a; int b; } StubFusionPair;
static const StubFusionPair g_fusion_pairs[] = {
    { GL_STUB_glColor3f, GL_STUB_glColor4f },
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

/* Run one program through both legs and compare. Returns the four-
 * bucket ParityResult so the caller can tally PASS/FAIL/XFAIL/XPASS.
 *
 * Infrastructure failures (compile error, child error, unreadable child
 * output) always return PARITY_FAIL regardless of the case's XFAIL
 * annotation — those are bugs in the test, not in the program, and
 * silencing them would defeat the point of the test. */
static ParityResult run_one_case(const TraceProgram *prog) {
    pid_t pid = getpid();
    /* Example names can contain spaces, parens, slashes — anything goes
     * via repl_examples_name(). Build a safe path stem by mapping every
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

    /* REPL leg. */
    glr_app_reset_all();
    char err[128];
    repl_eval_declare_predef_var("z", err, sizeof err);
    repl_eval_declare_predef_var("i", err, sizeof err);
    g_status[0] = '\0';

    for (int li = 0; prog->lines[li]; li++) {
        editor_feed_line(prog->lines[li]);
    }
    repl_flatten_commands(editor_state_edit_line());

    /* Init+destroy bracket the executor's gluNewTess / gluTessCallback /
     * gluDeleteTess setup — these are REPL-internal, not user-emitted GL,
     * so reset counters AFTER init and snapshot BEFORE destroy to keep
     * the trace user-code-only. The trace file opens between reset and
     * execute for the same reason. */
    repl_executor_init_resources();
    gl_stub_counts_reset();
    gl_stub_trace_open(temp_repl_tr);
    repl_execute_program(NULL);
    gl_stub_trace_close();
    unsigned long long repl_counts[GL_STUB_COUNT_MAX];
    memcpy(repl_counts, gl_stub_counts, sizeof repl_counts);
    repl_executor_destroy_resources();

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

    snprintf(cmd, sizeof cmd, "'%s' '%s' '%s'",
             temp_bin, temp_out, temp_child_tr);
    rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  [%s] child rc=%d\n", prog->name, rc);
        return PARITY_FAIL;
    }

    unsigned long long child_counts[GL_STUB_COUNT_MAX];
    if (!read_child_counts(temp_out, child_counts)) {
        fprintf(stderr, "  [%s] child output unreadable: %s\n",
                prog->name, temp_out);
        return PARITY_FAIL;
    }

    /* Silent comparison first so XFAIL cases stay quiet. We'll re-call
     * with stderr for FAIL cases below to print the per-counter
     * mismatch lines. */
    int match = compare_counts(prog->name, repl_counts, child_counts, NULL);

    ParityResult result;
    if (match) {
        result = prog->expected_fail ? PARITY_XPASS : PARITY_PASS;
    } else {
        result = prog->expected_fail ? PARITY_XFAIL : PARITY_FAIL;
    }

    /* Output dispatch by bucket:
     *   PASS  — silent (only the run-counter prints later).
     *   XFAIL — one info line so the divergence stays visible, no diff.
     *   FAIL  — counter-mismatch lines + a unified diff hunk.
     *   XPASS — explicit "annotation stale" callout; no diff (the
     *           traces matched). */
    switch (result) {
    case PARITY_PASS:
        break;
    case PARITY_XFAIL:
        fprintf(stderr, "  XFAIL [%s] expected: %s\n",
                prog->name, prog->expected_fail);
        break;
    case PARITY_FAIL: {
        (void)compare_counts(prog->name, repl_counts, child_counts, stderr);
        char diff_cmd[1024];
        snprintf(diff_cmd, sizeof diff_cmd,
                 "diff --color=always -u '%s' '%s' | head -50 >&2",
                 temp_repl_tr, temp_child_tr);
        fprintf(stderr, "  [%s] trace diff (repl - / child +):\n",
                prog->name);
        (void)system(diff_cmd);
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

/* --full: walk repl_examples_*. Each example is constructed on the fly
 * from the (name, lines) pair plus an XFAIL annotation looked up in
 * g_example_xfail. */
static void run_examples(ParityTotals *totals) {
    int n = repl_examples_count();
    for (int i = 0; i < n; i++) {
        const char *name = repl_examples_name(i);
        const char *const *lines = repl_examples_lines(i);
        TraceProgram p = { name, lines, expected_fail_for_example(name) };
        tally(totals, run_one_case(&p));
    }
}

static void print_help(const char *argv0) {
    printf(
"Usage: %s [--full] [--help]\n"
"\n"
"Cross-checks REPL execution against the exported C trace by counting\n"
"per-symbol stub GL calls on both sides and asserting parity.\n"
"\n"
"For each program the test:\n"
"  1. feeds source lines through editor_feed_line(), flattens, snapshots\n"
"     gl_stub_counts[] around repl_execute_program();\n"
"  2. calls repl_export_save_output() to a temp file, shells out to cc\n"
"     to compile that file together with tests/export_trace_driver.c\n"
"     (which #includes the exported file so its static helpers\n"
"     render_repl_geometry / reset_repl_vars are visible);\n"
"  3. runs the child, reads its dumped counts, compares.\n"
"\n"
"glColor3f and glColor4f are summed before comparing because the\n"
"executor folds CMD_COLOR3F through glColor4f at execute time while\n"
"the exporter emits glColor3f literally.\n"
"\n"
"On count mismatch the test also runs diff(1) on the two per-call\n"
"trace files the stubs emitted (gl_stub_trace_fp dumps one\n"
"\"<symbol> <args>\" line per call) and pipes the first 50 lines of\n"
"the unified diff to stderr to localize the divergence.\n"
"\n"
"Cases listed in g_example_xfail (in the test source) are treated as\n"
"expected-to-fail. Buckets:\n"
"  PASS  — match, no annotation.\n"
"  FAIL  — mismatch, no annotation; fails the test, dumps diff.\n"
"  XFAIL — mismatch, has annotation; quiet info line, doesn't fail.\n"
"  XPASS — match, has annotation; the annotation is stale, remove it.\n"
"          Fails the test (loud) so the list doesn't accumulate cruft.\n"
"\n"
"Options:\n"
"  --full          After the curated table, also run every built-in\n"
"                  example via repl_examples_*. Slow: one cc invocation\n"
"                  per program. See plans/not-started/gl-stub-extensions.md.\n"
"  --keep-traces   On real FAIL, leave the .repl.tr and .child.tr trace\n"
"                  files in /tmp for inspection. XFAIL traces are still\n"
"                  unlinked (the divergence is expected).\n"
"  --help          Show this help and exit 0.\n"
"\n"
"Notes:\n"
"  Stub-only test (linked only when USE_GL_STUBS=1) because both legs\n"
"  need gl_stub_counts[] live. The cc invocation expects to run from\n"
"  the repo root; make test runs tests from there.\n",
        argv0 ? argv0 : "test_export_trace_parity");
}

int main(int argc, char **argv) {
    int full = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--full") == 0) {
            full = 1;
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

    ParityTotals totals = {0};
    for (int i = 0; i < g_curated_count; i++) {
        tally(&totals, run_one_case(&g_curated[i]));
    }
    if (full) run_examples(&totals);

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
