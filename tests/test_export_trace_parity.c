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
 *   1. resets the REPL, feeds each line through feed_line(),
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

#include "support/test_harness.h"

#include <GL/gl_stub_counts.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define g_status (ui_state_status_mut()->text)

typedef struct {
    const char *name;
    const char *const *lines; /* NULL-terminated */
} TraceProgram;

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
    { "triangle",            prog_triangle },
    { "loop_in_begin",       prog_loop_in_begin },
    { "color_normal",        prog_color_normal },
    { "transforms",          prog_transforms },
    { "glut_cube",           prog_glut_cube },
    { "function_call",       prog_function_call },
    { "label_rasterpos",     prog_label_rasterpos },
};
static const int g_curated_count = (int)(sizeof(g_curated)/sizeof(g_curated[0]));

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

/* Compare REPL-side and (boilerplate-subtracted) child counts. Returns
 * 1 on full parity, 0 on any mismatch, with an explanatory line on
 * stderr per mismatched counter. */
static int compare_counts(const char *case_name,
                          const unsigned long long *repl_counts,
                          const unsigned long long *child_counts) {
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
            fprintf(stderr,
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
            fprintf(stderr,
                "  [%s] %s mismatch: repl=%llu child=%llu\n",
                case_name, gl_stub_count_name(i),
                repl_counts[i], child_counts[i]);
            mismatches++;
        }
    }
    return mismatches == 0;
}

/* Run one program through both legs and compare. Returns 1 on parity,
 * 0 if anything failed along the way (compile error, run error,
 * count mismatch). */
static int run_one_case(const TraceProgram *prog) {
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
    snprintf(temp_c,   sizeof temp_c,   "/tmp/test_trace_%d_%s.c",   (int)pid, safe_name);
    snprintf(temp_bin, sizeof temp_bin, "/tmp/test_trace_%d_%s.bin", (int)pid, safe_name);
    snprintf(temp_out, sizeof temp_out, "/tmp/test_trace_%d_%s.txt", (int)pid, safe_name);
    snprintf(temp_log, sizeof temp_log, "/tmp/test_trace_%d_%s.log", (int)pid, safe_name);

    /* REPL leg. */
    glr_app_reset_all();
    char err[128];
    repl_eval_declare_predef_var("z", err, sizeof err);
    repl_eval_declare_predef_var("i", err, sizeof err);
    g_status[0] = '\0';

    for (int li = 0; prog->lines[li]; li++) {
        editor_feed_line(prog->lines[li]);
    }
    repl_flatten_commands();

    /* Init+destroy bracket the executor's gluNewTess / gluTessCallback /
     * gluDeleteTess setup — these are REPL-internal, not user-emitted GL,
     * so reset counters AFTER init and snapshot BEFORE destroy to keep
     * the trace user-code-only. */
    repl_executor_init_resources();
    gl_stub_counts_reset();
    repl_execute_program(NULL);
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
        return 0;
    }

    snprintf(cmd, sizeof cmd, "'%s' '%s'", temp_bin, temp_out);
    rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  [%s] child rc=%d\n", prog->name, rc);
        return 0;
    }

    unsigned long long child_counts[GL_STUB_COUNT_MAX];
    if (!read_child_counts(temp_out, child_counts)) {
        fprintf(stderr, "  [%s] child output unreadable: %s\n",
                prog->name, temp_out);
        return 0;
    }

    int ok = compare_counts(prog->name, repl_counts, child_counts);

    /* Best-effort cleanup; ignore errors. */
    unlink(temp_c); unlink(temp_bin); unlink(temp_out); unlink(temp_log);
    return ok;
}

/* --full: walk repl_examples_*. Each example is loaded as a single
 * pre-flattened name + line list, fed through editor_feed_line() in
 * the same shape as the curated cases. */
static void run_examples(void) {
    int n = repl_examples_count();
    for (int i = 0; i < n; i++) {
        const char *name = repl_examples_name(i);
        const char *const *lines = repl_examples_lines(i);
        TraceProgram p = { name, lines };
        char label[160];
        snprintf(label, sizeof label, "example/%s: REPL == exported C", name);
        ASSERT_TRUE(label, run_one_case(&p));
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
"  1. feeds source lines through feed_line(), flattens, snapshots\n"
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
"Options:\n"
"  --full     After the curated table, also run every built-in example\n"
"             via repl_examples_*. Slow: one cc invocation per program.\n"
"             Currently surfaces two known divergences (Bezier glVertex2f,\n"
"             orbit-plot glutBitmapCharacter) — left as test discoveries\n"
"             for follow-up; see plans/not-started/gl-stub-extensions.md.\n"
"  --help     Show this help and exit 0.\n"
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

    for (int i = 0; i < g_curated_count; i++) {
        char label[160];
        snprintf(label, sizeof label,
                 "curated/%s: REPL == exported C", g_curated[i].name);
        ASSERT_TRUE(label, run_one_case(&g_curated[i]));
    }

    if (full) run_examples();

    printf("test_export_trace_parity: %d/%d passed\n",
           g_harness.passed, g_harness.run);
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}
