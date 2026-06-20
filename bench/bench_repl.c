/*
 * bench_repl.c - Runtime benchmarks for the REPL parse / flatten / replay
 * pipeline.
 *
 * The benchmark binary links against the same CORE_TEST_OBJS the unit tests
 * use, so it works under both the normal GL-headers build and the GL-stubs
 * build (`make bench USE_GL_STUBS=1`).
 *
 * In the stubs build every gl* call is an inline no-op that only ticks a
 * per-function counter (see tests/gl-stubs/include/GL/gl_stub_counts.h), so timings
 * measure pure C-level cost. In the real-GL build we create a real GL
 * context up front (via GLUT) so sub-benchmarks that drive actual draw
 * calls - notably `fade_batches` via `repl_execute_program() per fade batch` -
 * have somewhere to emit to; without a current context those calls are
 * undefined behaviour rather than measurable work.
 *
 * Sub-benchmarks (names match the `--only` filter strings and the printed
 * labels):
 *   parse_lines       - repl_parse_command on every example line
 *   feed_examples     - full editor_feed_line path on every example
 *   flatten_examples  - load each example then call repl_flatten_commands
 *   source_scope_query- sweep source-scope depth/scope queries over a deep
 *                       synthetic document (isolates the amortized-O(1)
 *                       prefix-depth cache lookup the Finding-4 view refactor
 *                       must preserve)
 *   source_scope_churn- invalidate + query per op (isolates the O(N) prefix
 *                       cache rebuild paid once per document change)
 *   normalize_large_doc- parse+normalize a line against a large live document
 *                       (integration guard: the normalize entry binds a fresh
 *                       source-scope view per call after Finding 4)
 *   replay_examples   - start a replay and step it to completion
 *   replay_long       - feed a synthetic large scene and step replay to end
 *   fade_batches      - drive repl_execute_program() per fade batch with a packed
 *                       batch buffer whose old_pcs sit deep in a long flat
 *                       command stream (exercises the per-batch prefix
 *                       walk that dominates late-replay fade-in cost).
 *
 * Output is one line per sub-benchmark with mean / min / iterations / per-op
 * cost. CSV mode (`--csv`) is suitable for diffing across machines.
 *
 * Currently this measures wall time only. See `feature/benchmark-metrics.md`
 * for the plan to add machine-agnostic metrics (perf instruction counts,
 * cycles, normalized op-rate).
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "editor/state.h"         /* editor_state_edit_line */
#include <math.h>
#include "repl/flatten.h"
#include "repl/pipeline.h"
#include "repl/state_notify.h"
#include "repl/eval.h"
#include "repl/example_loader.h"  /* repl_load_example_lines_for_test */
#include "repl/examples.h"
#include "repl/executor.h"
#include "repl/load.h"           /* repl_load_apply_line — uncapped doc build */
#include "repl/normalize.h"      /* repl_parse_and_normalize_strict */
#include "repl/parser.h"
#include "repl/source_scope.h"   /* prefix-depth queries + cache invalidate */
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "app/glr_ctrl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>          /* uname() for the machine-id CSV preamble */
#include <time.h>

#ifdef GL_STUBS
#include <GL/gl_stub_counts.h>
#else
/* Real-GL build: pull in GLUT so we can create an actual current
 * context before running sub-benchmarks that emit draw calls. The
 * stub headers deliberately do NOT get included here - the Makefile
 * picks between stub and system GL headers via -I ordering. */
#include "gl_includes.h"
#endif

/* ---- Timekeeping ------------------------------------------------------- */

static double now_seconds(void) {
    struct timespec ts;
    /* CLOCK_MONOTONIC is the right pick for a benchmark wall clock: it is
     * not affected by NTP slew the way CLOCK_REALTIME is, and it does not
     * stop the way CLOCK_PROCESS_CPUTIME_ID can across sleeps. We stay
     * single-threaded so process vs. wall doesn't differ in practice. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- Result reporting -------------------------------------------------- */

typedef struct {
    const char *name;       /* sub-benchmark identifier */
    const char *unit;       /* "lines", "examples", "steps", "scenes", etc. */
    double      total_sec;  /* wall time across all iterations */
    long long   iters;      /* outer iterations */
    long long   ops;        /* total work units performed across iters */
    double      min_sec;    /* fastest single iteration */
} BenchResult;

static int g_csv = 0;

static void report(BenchResult r) {
    double per_iter_ms = (r.iters > 0) ? (r.total_sec * 1000.0 / (double)r.iters) : 0.0;
    double per_op_us = (r.ops > 0) ? (r.total_sec * 1e6 / (double)r.ops) : 0.0;
    double ops_per_sec = (r.total_sec > 0) ? ((double)r.ops / r.total_sec) : 0.0;

    if (g_csv) {
        printf("%s,%s,%lld,%lld,%.6f,%.6f,%.4f,%.4f,%.0f\n",
               r.name, r.unit,
               r.iters, r.ops,
               r.total_sec, r.min_sec * 1000.0,
               per_iter_ms, per_op_us, ops_per_sec);
        return;
    }

    printf("  %-22s  iters=%-5lld  ops=%-9lld  total=%8.3f ms  "
           "min=%8.3f ms  per-iter=%8.3f ms  per-%-9s=%9.3f us  "
           "rate=%10.0f %s/s\n",
           r.name,
           r.iters, r.ops,
           r.total_sec * 1000.0,
           r.min_sec * 1000.0,
           per_iter_ms,
           r.unit, per_op_us,
           ops_per_sec, r.unit);
}

/* ---- Test fixture helpers --------------------------------------------- */

/* Mirror declare_test_vars() from the test suites - examples reference
 * these single-letter identifiers freely and parsing them otherwise fails
 * the repl_eval_validate_expression_idents() check. We declare them once at startup
 * and re-declare after each glr_ctrl_reset_all() call (reset wipes the
 * predef table). */
static const char *const k_test_idents[] = {
    "x", "y", "z", "i", "j", "k", "a", "b", "c", "n",
};
static const int k_num_test_idents =
    (int)(sizeof(k_test_idents) / sizeof(k_test_idents[0]));

static void declare_test_idents(void) {
    char err[128];
    for (int i = 0; i < k_num_test_idents; i++)
        repl_eval_declare_predef_var(k_test_idents[i], err, sizeof(err));
}

static void fresh_repl(void) {
    glr_ctrl_reset_all();
    declare_test_idents();
}

static int example_line_count(int idx) {
    const char *const *lines = repl_example_lines(idx);
    int n = 0;
    if (!lines) return 0;
    while (lines[n]) n++;
    return n;
}

static long long total_example_lines(void) {
    long long total = 0;
    int n = repl_example_count();
    for (int i = 0; i < n; i++)
        total += example_line_count(i);
    return total;
}

/* ---- bench: parse single lines via repl_parse_command ------------------ */

/* This bypasses the commit dispatch chain and exercises only the parser
 * path. It is meant to bracket how much of "editor_feed_line cost" is parser vs.
 * everything else (normalization, var declaration, dirty marking, etc.). */
static BenchResult bench_parse_lines(int iters) {
    /* Build a flat array of lines from every example so we touch every
     * grammar branch (vertices, transforms, gluXxx, for, func, if, vars,
     * comments, blank lines). */
    int n_examples = repl_example_count();
    long long total_lines = total_example_lines();
    const char **flat = (const char **)malloc(sizeof(*flat) * (size_t)total_lines);
    long long flat_n = 0;
    for (int e = 0; e < n_examples; e++) {
        const char *const *ls = repl_example_lines(e);
        if (!ls) continue;
        for (int i = 0; ls[i]; i++)
            flat[flat_n++] = ls[i];
    }

    /* Predeclare a common subset of identifiers so the validator does
     * not reject `x` etc. on the lines that use them. This is NOT
     * exhaustive - examples that reference other locals/params will
     * still fail validation, but the parser work we're measuring
     * (tokenize, expression parse, normalize) runs regardless of that
     * result. */
    fresh_repl();

    BenchResult r = { .name = "parse_lines", .unit = "lines",
                      .min_sec = 1e18 };
    GLCmd cmd;

    ReplParseContext bench_ctx = { 0, NULL, 0, 0, NULL, 0 };
    ReplParsedLine pl;
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (long long i = 0; i < flat_n; i++) {
            memset(&cmd, 0, sizeof(cmd));
            (void)repl_parser_parse_command_ctx(flat[i], &pl, &bench_ctx);
            cmd = pl.cmd;
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += flat_n;
        r.iters++;
    }

    free(flat);
    return r;
}

/* ---- bench: full editor_feed_line path on every example ---------------------- */

static BenchResult bench_feed_examples(int iters) {
    int n_examples = repl_example_count();
    long long lines_per_iter = total_example_lines();

    BenchResult r = { .name = "feed_examples", .unit = "lines",
                      .min_sec = 1e18 };

    /* load_example_lines() already resets repl_state_document_cmds_mut() / repl_state_flat_program_count() and
     * calls init_predef_vars(), so an extra fresh_repl() before each
     * load would just bill duplicate reset work to this benchmark.
     * Examples declare their own float vars, so we don't need
     * declare_test_idents() here either. */
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int e = 0; e < n_examples; e++) {
            repl_load_example_lines_for_test(repl_example_lines(e));
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += lines_per_iter;
        r.iters++;
    }
    return r;
}

/* ---- bench: flatten cost on a fixed scene ----------------------------- */

/* A verbatim, frozen copy of the "Animated wave surface" built-in example (a
 * nested-for surface that unrolls to a large flat program). Hardcoded here so
 * the flatten benchmark's workload is fixed regardless of changes to the
 * built-in example list — adding, reordering, or editing examples in
 * src/repl/examples.c can't move this number. The // camera / // @cfg metadata
 * lines are kept so repl_load_example_lines_for_test strips them exactly as the
 * real example loader does, preserving an identical post-load command set. */
static const char *const k_flatten_bench_scene[] = {
    "// camera",
    "glTranslatef(0.0f, 0.0f, -5.0f);",
    "glRotatef(20.0f, 1.0f, 0.0f, 0.0f);",
    "glRotatef(30.0f, 0.0f, 1.0f, 0.0f);",
    "glTranslatef(0.0f, 0.0f, 0.0f);",
    "",
    "// @cfg vertex_points = 0",
    "static float grid, extent, x, y, z, invGradMag; // strip cell index, world extent, vertex coords, 1/|gradient|",
    "static float amp = 0.4;    // wave amplitude (peak |y|)",
    "static float freq = 2.5;   // spatial frequency along x and z",
    "static float zPhase = 0.7; // z-axis time phase: z evolves slower than x (<1)",
    "glClearColor(0.1, 0.1, 0.1, 1.0);",
    "// Animated surface: y = sin(freq*x + t) * cos(freq*z + zPhase*t) * amp",
    "// Drawn as a triangle strip per row with analytic per-vertex normals.",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "glEnable(GL_NORMALIZE);",
    "glEnable(GL_LIGHT3);",
    "glEnable(GL_LIGHT2);",
    "glEnable(GL_LIGHT1);",
    "glEnable(GL_LIGHT0);",
    "glShadeModel(GL_SMOOTH);",
    "grid = 16;     // rows/cols of strip cells",
    "extent = 3.0;  // total surface width along x and z",
    "for(i, 0, grid) {",
        "glBegin(GL_TRIANGLE_STRIP);",
        "for(j, 0, grid+1) {",
            "// row i (z fixed), step j across x; emit two vertices per j to feed the strip",
            "x = -extent/2 + extent*j/grid;",
            "z = -extent/2 + extent*i/grid;",
            "y = sin(x*freq + t)*cos(z*freq + zPhase*t)*amp;",
            "// gradient = (dy/dx, 0, dy/dz); normal = (-dy/dx, 1, -dy/dz) / |...|.",
            "// invGradMag = 1 / sqrt(1 + amp^2 * freq^2 * (cos^2*cos^2 + sin^2*sin^2)).",
            "invGradMag = 1.0/sqrt(1 + amp*amp*freq*freq*(cos(x*freq + t)*cos(x*freq + t)*cos(z*freq + zPhase*t)*cos(z*freq + zPhase*t) + sin(x*freq + t)*sin(x*freq + t)*sin(z*freq + zPhase*t)*sin(z*freq + zPhase*t)));",
            "glNormal3f(-amp*freq*cos(x*freq + t)*cos(z*freq + zPhase*t)*invGradMag, invGradMag, amp*freq*sin(x*freq + t)*sin(z*freq + zPhase*t)*invGradMag);",
            "glColor3f(0.35 + 0.35*sin(x + t), 0.5 + 0.3*cos(z + t*0.5), 0.65 + 0.25*sin(x*z + t));",
            "glVertex3f(x, y, z);",
            "// second strip vertex: same x, but z stepped to row i+1",
            "z = -extent/2 + extent*(i + 1)/grid;",
            "y = sin(x*freq + t)*cos(z*freq + zPhase*t)*amp;",
            "invGradMag = 1.0/sqrt(1 + amp*amp*freq*freq*(cos(x*freq + t)*cos(x*freq + t)*cos(z*freq + zPhase*t)*cos(z*freq + zPhase*t) + sin(x*freq + t)*sin(x*freq + t)*sin(z*freq + zPhase*t)*sin(z*freq + zPhase*t)));",
            "glNormal3f(-amp*freq*cos(x*freq + t)*cos(z*freq + zPhase*t)*invGradMag, invGradMag, amp*freq*sin(x*freq + t)*sin(z*freq + zPhase*t)*invGradMag);",
            "glColor3f(0.35 + 0.35*sin(x + t), 0.5 + 0.3*cos(z + t*0.5), 0.65 + 0.25*sin(x*z + t));",
            "glVertex3f(x, y, z);",
        "}",
        "glEnd();",
    "}",
    NULL
};

static BenchResult bench_flatten_examples(int iters) {
    BenchResult r = { .name = "flatten_examples", .unit = "flattens",
                      .min_sec = 1e18 };

    /* Load the fixed scene fresh once so we are timing flatten alone, not
     * editor_feed_line plus flatten. load_example_lines() resets cmd state
     * itself; no separate fresh_repl() is needed. */
    repl_load_example_lines_for_test(k_flatten_bench_scene);

    /* Snapshot post-load predef values. flatten_range() writes
     * g_predef_vars[].value on CMD_VAR_ASSIGN (src/repl/core.c:2624), so
     * without restoring each iter sees drifted values and measures a different
     * workload than the first - several examples have self-referential-looking
     * assignments whose RHS depends on other predef vars. */
    float saved_vals[MAX_PREDEF_VARS];
    int saved_n = g_num_predef_vars;
    for (int i = 0; i < saved_n; i++)
        saved_vals[i] = g_predef_vars[i].value;

    /* Flatten `inner` times per timer sample so per-call granularity is well
     * above the clock's resolution; the timer brackets only the flatten loop. */
    int inner = 32;

    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < saved_n; i++)
            g_predef_vars_mut[i].value = saved_vals[i];

        double t0 = now_seconds();
        for (int k = 0; k < inner; k++) {
            /* Restore the post-load values before each flatten so every inner
             * iteration measures the same expression evaluation workload. The
             * restore is a 16-float copy, dwarfed by flatten itself. */
            for (int i = 0; i < saved_n; i++)
                g_predef_vars_mut[i].value = saved_vals[i];
            /* repl_flatten_commands(editor_state_edit_line()) -> flatten_commands() rebuilds
             * unconditionally (resets repl_state_flat_program_count() and walks
             * repl_state_document_cmds_mut()[]), so we don't need to toggle any dirty flag
             * here - doing so would just add unrelated side effects
             * (repl_state_normals_dirty(), depth cache invalidation) into the
             * timed region. */
            repl_flatten_commands(editor_state_edit_line());
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += inner;
        r.iters++;
    }

    if (!g_csv) {
        fprintf(stderr, "  (flatten_examples: fixed wave-surface scene, flat_cmds=%d)\n",
                repl_state_flat_program_count());
    }
    return r;
}

/* ---- bench: cursor flat-cost query (statusbar budget readout) --------- */

/* Times repl_flatten_cost_at_line over every cursor line of the same
 * wave-surface scene flatten_examples uses, so the two rows compare
 * directly: the readout runs once per frame at snapshot build, the
 * flatten it annotates runs once per frame too. One op = one full
 * sweep of the document (every line classified + counted), i.e. a
 * deliberate worst case — the live app issues a single line per frame. */
static BenchResult bench_flat_cost_query(int iters) {
    BenchResult r = { .name = "flat_cost_query", .unit = "sweeps",
                      .min_sec = 1e18 };

    repl_load_example_lines_for_test(k_flatten_bench_scene);
    repl_flatten_commands(editor_state_edit_line());
    int doc_count = repl_state_document_count();

    int inner = 32;
    /* The result is consumed (summed and reported) so the query can't
     * be optimized away. */
    long long sink = 0;

    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < inner; k++) {
            for (int line = 0; line < doc_count; line++) {
                ReplFlatCost cost = repl_flatten_cost_at_line(line);
                sink += cost.count;
            }
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += inner;
        r.iters++;
    }

    if (!g_csv) {
        fprintf(stderr, "  (flat_cost_query: %d lines/sweep, flat_cmds=%d, checksum=%lld)\n",
                doc_count, repl_state_flat_program_count(), sink);
    }
    return r;
}

/* ---- bench: spike — flatten the largest example, repeatedly ----------- */

/* Editor-owns-text spike pass/fail bar. The spike modifies flatten_range()
 * to re-parse every command from the editor buffer on each call (today the
 * no-vars path passes through). This bench picks the example with the
 * highest flat-program count after load and times flatten on it alone.
 *
 * Pass criterion (per the spike plan): mean flatten time under 4 ms (half
 * of one 120 fps frame). The bench reports total / mean / min / max so a
 * regression shows up directly even without the threshold.
 */
static BenchResult bench_spike_flatten_largest(int iters) {
    int n_examples = repl_example_count();
    int worst_idx = 0;
    int worst_flat = 0;
    float saved_vals[MAX_PREDEF_VARS];

    /* Pick the example with the largest flat-program count after load.
     * "Lines" alone underestimates: a short for-loop unrolls into many
     * flat commands, and that's what flatten actually walks. */
    for (int e = 0; e < n_examples; e++) {
        repl_load_example_lines_for_test(repl_example_lines(e));
        repl_flatten_commands(editor_state_edit_line());
        int n = repl_state_flat_program_count();
        if (n > worst_flat) {
            worst_flat = n;
            worst_idx = e;
        }
    }

    /* Reload the chosen example as the timed fixture. */
    repl_load_example_lines_for_test(repl_example_lines(worst_idx));

    int saved_n = g_num_predef_vars;
    for (int i = 0; i < saved_n; i++)
        saved_vals[i] = g_predef_vars[i].value;

    /* Inner loop runs flatten N times per timer sample so the per-call
     * granularity is well above the clock's resolution. The pass bar
     * compares mean per-call cost. */
    int inner = 1000;
    BenchResult r = { .name = "spike_flatten_largest", .unit = "flattens",
                      .min_sec = 1e18 };

    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < saved_n; i++)
            g_predef_vars_mut[i].value = saved_vals[i];

        double t0 = now_seconds();
        for (int k = 0; k < inner; k++) {
            for (int i = 0; i < saved_n; i++)
                g_predef_vars_mut[i].value = saved_vals[i];
            repl_flatten_commands(editor_state_edit_line());
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += inner;
        r.iters++;
    }

    if (!g_csv) {
        printf("  (largest example: idx=%d, flat_cmds=%d)\n",
               worst_idx, worst_flat);
    }
    return r;
}

/* ---- bench: replay every example end-to-end --------------------------- */

static BenchResult bench_replay_examples(int iters) {
    int n_examples = repl_example_count();

    BenchResult r = { .name = "replay_examples", .unit = "steps",
                      .min_sec = 1e18 };

    /* load_example_lines() leaves repl_state_flat_program_dirty()=1, and replay_start() will
     * flatten once on its own - calling repl_flatten_commands(editor_state_edit_line()) explicitly
     * beforehand would flatten twice, because repl_flatten_commands(editor_state_edit_line()) does
     * NOT clear repl_state_flat_program_dirty() (see src/repl/core.c:4462-4464 vs. :3265-3269). */
    for (int it = 0; it < iters; it++) {
        long long steps = 0;
        double t0 = now_seconds();
        for (int e = 0; e < n_examples; e++) {
            repl_load_example_lines_for_test(repl_example_lines(e));

            replay_start();
            int safety = repl_state_flat_program_count() + 1;
            ReplayRuntimeState replay = replay_state_view();
            while (replay.state == REPLAY_PLAYING && safety-- > 0) {
                replay_advance(repl_state_flat_program_view());
                steps++;
            }
            replay_stop();
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += steps;
        r.iters++;
    }
    return r;
}

/* ---- bench: long-running replay (synthetic large scene) --------------- */

/* Build a single scene that flattens to a large flat-cmd stream so we
 * get a longer-running replay test that is comparable across machines.
 *
 * Sized to fit within MAX_COMMANDS (4096) after expansion: when flatten
 * would exceed that, flatten_append_cmd sets ctx->abort=1 and
 * repl_flatten_program then resets flat_count to 0 (capacity overflow
 * is treated as a hard failure, not a soft cap). The outer for-loop
 * emits 12 flat cmds per iteration, with 3 setup cmds before it
 * (two CMD_VAR_DECLARE rows are flatten-omitted): 12*340 + 3 = 4083.
 *
 * Iteration count was 600 before MAX_COMMANDS dropped from 8192 to 4096
 * on 2026-04-30 (commit 804d794 "config: reduce max commands"); the
 * 7203-cmd expansion that used to fit no longer does. */
static const char *const k_long_replay_scene[] = {
    "glClearColor(0.05, 0.05, 0.05, 1);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "float a;",
    "float b;",
    "for(i, 0, 340) {",
        "a = i * 0.05;",
        "b = sin(a) * cos(a);",
        "glPushMatrix();",
        "glTranslatef(a, b, 0);",
        "glRotatef(i, 0, 1, 0);",
        "glColor3f(0.4, 0.6, 0.8);",
        "glBegin(GL_TRIANGLES);",
        "glVertex3f(0, 0, 0);",
        "glVertex3f(1, 0, 0);",
        "glVertex3f(0, 1, 0);",
        "glEnd();",
        "glPopMatrix();",
    "}",
    NULL,
};

static BenchResult bench_replay_long(int iters) {
    BenchResult r = { .name = "replay_long", .unit = "steps",
                      .min_sec = 1e18 };

    /* Load once outside the inner loop - editor_feed_line is not what we are
     * measuring here. Re-using the same repl_state_document_cmds_mut()[] across iterations is
     * fine because replay only mutates the replay state, not the source
     * commands. We mark repl_state_flat_program_dirty() between iterations so replay_start()
     * does a fresh flatten each time - that matches "what happens the
     * first time you press play". Note: replay_start() handles the
     * flatten itself and clears repl_state_flat_program_dirty(), so calling
     * repl_flatten_commands(editor_state_edit_line()) explicitly here would flatten twice. */
    repl_load_example_lines_for_test(k_long_replay_scene);

    /* Warm up via replay_start/replay_stop (not a bare
     * repl_flatten_commands) so the flatten's CMD_VAR_ASSIGN writes
     * to g_predef_vars don't leak into the timed iterations -
     * replay_start does its own predef snapshot/restore around the
     * flatten (src/repl/core.c:3264-3268). */
    repl_mark_source_dirty();
    replay_start();
    int flat_cmds = repl_state_flat_program_count();
    replay_stop();

    /* Snapshot post-load predef values. replay_advance() writes
     * g_predef_vars on CMD_VAR_ASSIGN during playback
     * (src/repl/core.c:3655-3656), so without restoring, each iteration
     * would start replay_start()'s baseline capture from progressively
     * drifted values. */
    float saved_vals[MAX_PREDEF_VARS];
    int saved_n = g_num_predef_vars;
    for (int i = 0; i < saved_n; i++)
        saved_vals[i] = g_predef_vars[i].value;

    for (int it = 0; it < iters; it++) {
        long long steps = 0;

        for (int i = 0; i < saved_n; i++)
            g_predef_vars_mut[i].value = saved_vals[i];
        repl_mark_source_dirty();
        double t0 = now_seconds();

        replay_start();
        int safety = repl_state_flat_program_count() + 1;
        ReplayRuntimeState replay = replay_state_view();
        while (replay.state == REPLAY_PLAYING && safety-- > 0) {
            replay_advance(repl_state_flat_program_view());
            steps++;
        }
        replay_stop();

        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += steps;
        r.iters++;
    }

    /* Diagnostic aside - useful for confirming the scene size, but gated
     * behind !g_csv so machine-parseable output stays clean on stderr too. */
    if (!g_csv) {
        fprintf(stderr, "  (replay_long scene flattened to %d flat cmds)\n",
                flat_cmds);
    }
    return r;
}

/* ---- bench: per-frame replay focus resolution ------------------------- */

/* glr_ctrl_push_highlights() calls replay_focus_flat_idx() once per rendered
 * frame while a replay is active. Commit 34df87f added that call. The function
 * derives the active step's begin via replay_prev_limit(pc), which re-walks the
 * flat stream from index 0 up to pc *every call* — and in REPLAY_MODE_VERTEX
 * each step within that walk does its own O(pc) backward scan
 * (replay_find_open_begin_before / _tess_polygon_before), so the whole thing is
 * O(N^2) in the flat-program length, paid per frame and worst at the tail of a
 * long replay. This bench parks pc at the end of the long synthetic scene and
 * times the per-frame focus call, so the regression (and any fix) is visible as
 * a per-call number independent of the advance-step benchmarks above. */
static BenchResult bench_replay_focus(int iters) {
    BenchResult r = { .name = "replay_focus", .unit = "calls",
                      .min_sec = 1e18 };

    repl_load_example_lines_for_test(k_long_replay_scene);
    repl_mark_source_dirty();
    replay_start();

    /* Advance to the end so pc sits at the tail — the worst case the
     * per-frame highlight push pays. State becomes REPLAY_DONE but stays
     * active until replay_stop(), and replay_focus_flat_idx() keys off
     * state->active + state->pc, so the focus call still does full work. */
    int safety = repl_state_flat_program_count() + 1;
    while (replay_state_view().state == REPLAY_PLAYING && safety-- > 0)
        replay_advance(repl_state_flat_program_view());
    int pc = replay_state_view().pc;

    int inner = 200;
    int last_focus = 0;
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < inner; k++)
            last_focus = replay_focus_flat_idx();
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += inner;
        r.iters++;
    }
    replay_stop();

    if (!g_csv) {
        fprintf(stderr, "  (replay_focus: pc=%d, focus_idx=%d)\n",
                pc, last_focus);
    }
    return r;
}

/* Companion to replay_focus: replay_focus_anchor_flat_idx() is the other
 * replay-active call glr_ctrl_push_highlights() and the guide-snapshot build
 * make per frame. It carried the same O(N^2) replay_prev_limit(pc) walk, so it
 * gets its own tail-pc bench. */
static BenchResult bench_replay_anchor(int iters) {
    BenchResult r = { .name = "replay_anchor", .unit = "calls",
                      .min_sec = 1e18 };

    repl_load_example_lines_for_test(k_long_replay_scene);
    repl_mark_source_dirty();
    replay_start();

    int safety = repl_state_flat_program_count() + 1;
    while (replay_state_view().state == REPLAY_PLAYING && safety-- > 0)
        replay_advance(repl_state_flat_program_view());
    int pc = replay_state_view().pc;

    int inner = 200;
    int last_anchor = 0;
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < inner; k++)
            last_anchor = replay_focus_anchor_flat_idx();
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += inner;
        r.iters++;
    }
    replay_stop();

    if (!g_csv) {
        fprintf(stderr, "  (replay_anchor: pc=%d, anchor_idx=%d)\n",
                pc, last_anchor);
    }
    return r;
}

/* ---- bench: replay fade-batch rendering path -------------------------- */

/* Scene built to emit a long flat-command stream of many small primitives
 * so we can exercise the fade-batch rendering pass with large old_pc
 * indices. We unroll a for-loop that emits one triangle per iteration.
 * Sized to fit within MAX_COMMANDS (4096): exceeding it makes flatten
 * abort and discard the partial flat stream (flat_count → 0). The loop
 * body emits 11 flat cmds per iter with 2 setup cmds before it
 * (CMD_VAR_DECLARE rows are flatten-omitted): 11*370 + 2 = 4072.
 *
 * Iteration count was 600 before MAX_COMMANDS dropped from 8192 to 4096
 * on 2026-04-30 (commit 804d794 "config: reduce max commands"). */
static const char *const k_fade_bench_scene[] = {
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "float a;",
    "float b;",
    "for(i, 0, 370) {",
        "a = i * 0.01;",
        "b = sin(a);",
        "glPushMatrix();",
        "glTranslatef(a, b, 0);",
        "glColor3f(0.4, 0.6, 0.8);",
        "glBegin(GL_TRIANGLES);",
        "glVertex3f(0, 0, 0);",
        "glVertex3f(1, 0, 0);",
        "glVertex3f(0, 1, 0);",
        "glEnd();",
        "glPopMatrix();",
    "}",
    NULL,
};

/* Populate a packed set of fade batches spaced near the tail of the
 * flattened command stream. This mirrors "step N is late in a long
 * replay": every batch's old_pc is large, so the old per-batch
 * replay_find_open_* scans pay the full prefix cost. */
static int populate_late_batches(int flat_cmds, int *old_pcs, int *new_pcs,
                                 int max_batches) {
    /* Need at least two flat cmds to carve out an old_pc + new_pc pair.
     * Below that the tail-anchored math produces negative indices, so
     * bail out rather than installing bogus batch state. */
    if (flat_cmds < 2 || max_batches < 1)
        return 0;

    /* Anchor batches in the final quarter of the stream. Cap at
     * REPLAY_FADE_BATCH_MAX (the bench helper clamps too, but staying
     * inside the limit here keeps reporting honest). */
    int count = max_batches;
    int tail_start = flat_cmds * 3 / 4;
    int span = flat_cmds - tail_start;
    if (span < count * 2) {
        /* Fallback for tiny flat counts - keeps the bench usable even if
         * MAX_COMMANDS is reduced. */
        count = span / 2;
        if (count < 1) count = 1;
    }

    int step = span / count;
    if (step < 2) step = 2;

    for (int i = 0; i < count; i++) {
        int old_pc = tail_start + i * step;
        int new_pc = old_pc + 2;
        if (old_pc > flat_cmds - 2) old_pc = flat_cmds - 2;
        if (new_pc > flat_cmds) new_pc = flat_cmds;
        old_pcs[i] = old_pc;
        new_pcs[i] = new_pc;
    }
    return count;
}

/* The fade-render workload moved out of the scene module (it's now the
 * REPL controller's post_fill_fn). This bench drives the same per-batch
 * cost — repl_execute_program with a skip-prefix and per-batch alpha —
 * directly, without going through the scene API. The numbers are
 * comparable to the pre-move benchmark since the heavy lifting was
 * always repl_execute_program. */
static void bench_render_one_fade_batch(int new_pc, int skip_pc, float alpha,
                                        FlatProgramView program) {
    repl_execute_program(&(ReplExecutionOptions){
        .flat_cmd_count      = new_pc,
        .program             = program,
        .fade_alpha_scale    = alpha,
        .skip_geom_before_pc = skip_pc,
        .has_fade_context    = 1,
    });
}

/* Refresh the fade plan from the live replay state — bench reinstalls
 * fade batches every iteration, so the plan it walks must be rebuilt
 * each time. */
static int bench_refresh_fade_plan(FlatProgramView program, ReplayFadePlan *plan, int *base_limit_out) {
    memset(plan, 0, sizeof(*plan));
    *base_limit_out = 0;

    replay_copy_baseline_predef_snapshot(&plan->baseline_predef);

    if (!replay_has_active_fades())
        return 0;

    *base_limit_out = replay_fill_base_limit(program);

    ReplayFadeBatchView fade_batches = replay_fade_batches_view();
    int batch_count = replay_compute_fade_skip_limits(program, plan->skip_limits,
                                                           REPLAY_FADE_BATCH_MAX);
    if (batch_count > REPLAY_FADE_BATCH_MAX)
        batch_count = REPLAY_FADE_BATCH_MAX;

    plan->batch_count = batch_count;
    for (int i = 0; i < batch_count; i++) {
        const ReplayFadeBatch *batch = &fade_batches.batches[i];
        plan->batches[i] = *batch;
        plan->batch_alpha[i] = replay_batch_alpha(batch);
    }
    return batch_count;
}

static BenchResult bench_fade_batches(int iters) {
    BenchResult r = { .name = "fade_batches", .unit = "calls",
                      .min_sec = 1e18 };

    /* Build the long scene and flatten once. The flatten pass runs
     * inside replay_start(); we piggy-back on that to also capture
     * repl_state_flat_program_count() (replay_start clamps repl_state_flat_program_count() during
     * playback, so we snapshot before/after). */
    repl_load_example_lines_for_test(k_fade_bench_scene);
    repl_mark_source_dirty();
    replay_start();
    int flat_cmds = repl_state_flat_program_count();
    replay_stop();

    /* Re-flatten after replay_stop so repl_state_flat_program_count() is the full stream
     * (replay's clamp might still be in effect otherwise - we observed
     * flat_cmds via the post-start snapshot above). */
    repl_mark_source_dirty();
    repl_flatten_commands(editor_state_edit_line());
    flat_cmds = repl_state_flat_program_count();

    /* 32 batches mirrors a typical in-flight count at the default 30fps
     * replay speed (REPLAY_FADE_DURATION is 0.5s). age=0.25s → alpha≈0.5
     * so replay_batch_alpha() returns a non-zero value and the per-batch
     * body runs. */
    enum { N_BATCHES = 32 };
    int old_pcs[N_BATCHES];
    int new_pcs[N_BATCHES];
    int installed_count = populate_late_batches(flat_cmds, old_pcs, new_pcs, N_BATCHES);
    float age = 0.25f;

    /* Calls-per-iter stays fixed across runs so per-call numbers are
     * comparable. Bump up inner so a single iteration reliably exceeds
     * timer resolution even in stubbed / very fast builds. */
    int inner = 200;

    ReplayFadePlan plan;
    int base_limit;
    FlatProgramView program = repl_state_flat_program_view();

#ifdef GL_STUBS
    /* Reset right before the timed region so the counter dump below
     * reflects only the per-batch repl_execute_program work across the
     * full iters×inner call count. */
    gl_stub_counts_reset();
#endif

    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < inner; k++) {
            /* Reinstall on every call so a fade batch that ticks its
             * age past REPLAY_FADE_DURATION doesn't silently reduce
             * the measured workload. */
            replay_bench_fade_install(old_pcs, new_pcs, installed_count, age);
            int batch_count = bench_refresh_fade_plan(program, &plan, &base_limit);
            for (int b = 0; b < batch_count; b++) {
                float alpha = plan.batch_alpha[b];
                if (alpha <= 0.0f) continue;
                bench_render_one_fade_batch(plan.batches[b].new_pc,
                                            plan.skip_limits[b],
                                            alpha, program);
            }
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += inner;
        r.iters++;
    }

    replay_bench_fade_clear();

    if (!g_csv) {
        fprintf(stderr, "  (fade_batches: flat_cmds=%d, batches=%d, age=%.3f)\n",
                flat_cmds, installed_count, age);
#ifdef GL_STUBS
        fprintf(stderr, "  (GL stub calls per fade-batch frame:)\n");
        gl_stub_counts_dump(stderr, "    ", r.ops);
#endif
    }
    (void)base_limit;  /* not used by the bench but useful to track */
    return r;
}

/* ---- bench: source-scope prefix-depth queries ------------------------- */

/* Feed a large, deeply-nested document directly through the lean loader
 * (repl_load_apply_line), bypassing the example loader's EXAMPLE_BODY_LINES_MAX
 * (256) cap so the source-scope prefix-depth cache has a realistically large
 * scene to index. Each 16-line block nests for / push-matrix / begin, so the
 * block-depth, matrix-depth, and begin-block prefixes all vary across
 * positions. The body uses constants only (no var refs), so it commits cleanly
 * against the post-reset predef table. Returns the resulting document row
 * count. */
static int load_deep_scope_doc(int blocks) {
    static const char *const tmpl[] = {
        "for(i, 0, 2) {",
        "glPushMatrix();",
        "glTranslatef(1, 0, 0);",
        "glRotatef(1, 0, 1, 0);",
        "for(j, 0, 2) {",
        "glPushMatrix();",
        "glScalef(1, 1, 1);",
        "glBegin(GL_TRIANGLES);",
        "glVertex3f(0, 0, 0);",
        "glVertex3f(1, 0, 0);",
        "glVertex3f(0, 1, 0);",
        "glEnd();",
        "glPopMatrix();",
        "}",
        "glPopMatrix();",
        "}",
    };
    int tmpl_n = (int)(sizeof(tmpl) / sizeof(tmpl[0]));
    char err[128];
    int edit_line = 0;

    glr_ctrl_reset_all();
    for (int b = 0; b < blocks; b++)
        for (int i = 0; i < tmpl_n; i++)
            (void)repl_load_apply_line(tmpl[i], err, sizeof(err), &edit_line);
    return repl_state_document_count();
}

/* Sweep every source-scope depth/scope query over every position of a deep
 * document, with NO mutation in the loop, so the prefix-depth cache stays warm.
 * This isolates the **amortized O(1) query** cost — the property the Finding-4
 * source-scope view refactor must preserve. A regression that recomputes the
 * prefix arrays per call (instead of indexing a built cache) shows up here as a
 * large per-sweep blow-up. One op = one full document sweep across five
 * queries. */
static BenchResult bench_source_scope_query(int iters) {
    BenchResult r = { .name = "source_scope_query", .unit = "sweeps",
                      .min_sec = 1e18 };
    int doc_count = load_deep_scope_doc(180);

    /* Warm the cache so the timed loop measures the O(1) query, not the
     * one-shot O(N) rebuild (that is bench_source_scope_churn's job). */
    repl_source_scope_depth_cache_invalidate();
    (void)repl_source_scope_block_depth_at(0);

    int inner = 8;
    /* Result is summed and reported so the queries can't be optimized away. */
    long long sink = 0;
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < inner; k++) {
            for (int pos = 0; pos <= doc_count; pos++) {
                sink += repl_source_scope_block_depth_at(pos);
                sink += repl_source_scope_matrix_scope_depth_at(pos);
                sink += repl_source_scope_tess_scope_depth_at(pos);
                sink += repl_source_scope_in_begin_block_at(pos);
                sink += repl_source_scope_cmd_indent_chars(pos);
            }
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += inner;
        r.iters++;
    }
    if (!g_csv)
        fprintf(stderr, "  (source_scope_query: doc rows=%d, 5 queries x (rows+1)/sweep, checksum=%lld)\n",
                doc_count, sink);
    return r;
}

/* Isolates the O(N) prefix-depth REBUILD. Each op invalidates the cache (as a
 * document mutation does) then issues one query, forcing a full
 * depth_cache_rebuild() over the document. Guards the "build on (re)bind" cost
 * the Finding-4 view refactor must keep flat — a view bound once per frame and
 * queried many times should rebuild no more often than today's global. One op =
 * one invalidate + rebuild. */
static BenchResult bench_source_scope_churn(int iters) {
    BenchResult r = { .name = "source_scope_churn", .unit = "rebuilds",
                      .min_sec = 1e18 };
    int doc_count = load_deep_scope_doc(180);

    int inner = 200;
    long long sink = 0;
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < inner; k++) {
            repl_source_scope_depth_cache_invalidate();
            /* One invalidate forces the next query to rebuild the whole
             * prefix cache (O(N)); the spread of follow-up queries is warm
             * and cheap, and keeps the checksum robustly non-zero so the
             * nested structure is verified, without changing that the
             * rebuild dominates each op. Offsets dodge block boundaries
             * (depth 0) so the sum reflects real nesting. */
            for (int s = 0; s < 8; s++)
                sink += repl_source_scope_block_depth_at((doc_count * s) / 8 + 3);
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += inner;
        r.iters++;
    }
    if (!g_csv)
        fprintf(stderr, "  (source_scope_churn: doc rows=%d, %d rebuilds/sample, checksum=%lld)\n",
                doc_count, inner, sink);
    return r;
}

/* ---- bench: normalize a line against a large live document ------------- */

/* Integration guard for the Finding-4 per-call view bind. The source-scope
 * view refactor moved repl_parse_and_normalize{,_strict} off the warm
 * live-document cache (amortized O(1)) onto binding a fresh
 * ReplSourceScopeView against the live document per call (O(N) build over the
 * whole document). source_scope_query can't see this — it queries the live
 * wrapper directly — so this drives the normalize entry against a large
 * document to expose any per-call O(N) cost in the commit/parse path.
 *
 * Uses only the stable normalize/loader API (no ReplSourceScopeView types),
 * so the SAME source compiles and runs on the pre-Finding-4 tree
 * (bench_repl.c was untouched by the view-cache commits) for a true
 * before/after: run it at the commit before 18c9b742 to get the pre number. */
static BenchResult bench_normalize_large_doc(int iters) {
    BenchResult r = { .name = "normalize_large_doc", .unit = "normalizes",
                      .min_sec = 1e18 };
    int doc_count = load_deep_scope_doc(180);
    int pos = doc_count / 2;
    const char *line = "glVertex3f(0, 0, 0);";

    int inner = 2000;
    long long sink = 0;
    GLCmd cmd;
    char text[MAX_LINE_LEN];
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < inner; k++) {
            memset(&cmd, 0, sizeof(cmd));
            /* No visible vars: the line is constant, so this measures the
             * normalize path's source-scope work, not expression eval. */
            sink += repl_parse_and_normalize_strict(line, pos, NULL, 0, 0,
                                                    &cmd, text, sizeof(text));
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += inner;
        r.iters++;
    }
    if (!g_csv)
        fprintf(stderr, "  (normalize_large_doc: doc rows=%d, %d normalizes/sample, checksum=%lld)\n",
                doc_count, inner, sink);
    return r;
}

/* ---- main -------------------------------------------------------------- */

static void print_csv_header(void) {
    printf("name,unit,iters,ops,total_sec,min_iter_ms,per_iter_ms,"
           "per_op_us,ops_per_sec\n");
}

/* Identify the host so a captured baseline CSV records where its numbers
 * came from (timings are only comparable on the same machine). In CSV mode
 * the line is `#`-prefixed so it precedes the header as a skippable comment;
 * in human mode it is a plain banner line. */
static void print_machine_info(void) {
    const char *prefix = g_csv ? "# " : "";
    struct utsname u;
    if (uname(&u) == 0) {
        printf("%smachine: %s %s %s %s\n", prefix,
               u.nodename, u.sysname, u.release, u.machine);
    } else {
        printf("%smachine: unknown\n", prefix);
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [--iters N] [--csv] [--only NAME[,NAME...]]\n"
        "  Available sub-benchmarks:\n"
        "    parse_lines       repl_parse_command on every example line\n"
        "    feed_examples     full editor_feed_line path on every example\n"
        "    flatten_examples  repl_flatten_commands per example\n"
        "    flat_cost_query   repl_flatten_cost_at_line over every cursor line\n"
        "    source_scope_query  source-scope depth queries over a deep document (warm cache)\n"
        "    source_scope_churn  source-scope cache rebuild per op (invalidate + query)\n"
        "    normalize_large_doc parse+normalize a line against a large live document\n"
        "    replay_examples   step replay through every example\n"
        "    replay_long       synthetic 600-iter for-loop replay\n"
        "    replay_focus      per-frame replay_focus_flat_idx() at the tail\n"
        "    replay_anchor     per-frame replay_focus_anchor_flat_idx() at the tail\n"
        "    fade_batches      replay fade-batch rendering with late old_pcs\n",
        prog);
}

static int wants(const char *filter, const char *name) {
    if (!filter || !*filter) return 1;
    size_t nlen = strlen(name);
    const char *p = filter;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t seg = comma ? (size_t)(comma - p) : strlen(p);
        if (seg == nlen && strncmp(p, name, seg) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* Real-GL build: create a minimal GLUT window so there is a current
 * GL context for sub-benchmarks that actually emit draw calls.
 * Without this, glBegin / glVertex / glEnd and friends hit whatever
 * the driver does with no context current - typically a silent no-op,
 * sometimes a segfault, never a representative timing.
 *
 * The stubs build skips this entirely: every gl* is an inline no-op
 * that ticks a counter and returns, so no context is needed (or
 * even available - the stub build intentionally has no GL libs).
 *
 * If the process has no display (e.g. headless CI without $DISPLAY),
 * glutInit() will exit non-zero with its own error message; that's
 * the cue to re-run with USE_GL_STUBS=1. */
static void bench_gl_context_init(int *argc, char **argv) {
#ifndef GL_STUBS
    static int glut_inited = 0;
    if (glut_inited) return;
    glutInit(argc, argv);
    glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH);
    glutInitWindowSize(1, 1);
    glutCreateWindow("bench_repl");
    glut_inited = 1;
#else
    (void)argc; (void)argv;
#endif
}

int main(int argc, char **argv) {
    int iters = 5;
    const char *only = NULL;

    /* Parse flags first so we know `--csv` etc. before printing.
     * glutInit() wants to see argv before we rewrite it, but it will
     * just ignore our flags and leave them in place. */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = atoi(argv[++i]);
            if (iters < 1) iters = 1;
        } else if (strcmp(argv[i], "--csv") == 0) {
            g_csv = 1;
        } else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    repl_eval_init_predef_vars();
    fresh_repl();

    if (g_csv) {
        print_machine_info();
        print_csv_header();
    } else {
        print_machine_info();
        printf("REPL benchmarks (iters=%d, examples=%d, total_lines=%lld)\n",
               iters, repl_example_count(), total_example_lines());
    }

    if (wants(only, "parse_lines"))
        report(bench_parse_lines(iters));
    if (wants(only, "feed_examples"))
        report(bench_feed_examples(iters));
    if (wants(only, "flatten_examples"))
        report(bench_flatten_examples(iters));
    if (wants(only, "flat_cost_query"))
        report(bench_flat_cost_query(iters));
    if (wants(only, "source_scope_query"))
        report(bench_source_scope_query(iters));
    if (wants(only, "source_scope_churn"))
        report(bench_source_scope_churn(iters));
    if (wants(only, "normalize_large_doc"))
        report(bench_normalize_large_doc(iters));
    if (wants(only, "spike_flatten_largest"))
        report(bench_spike_flatten_largest(iters));
    if (wants(only, "replay_examples"))
        report(bench_replay_examples(iters));
    if (wants(only, "replay_long"))
        report(bench_replay_long(iters));
    if (wants(only, "replay_focus"))
        report(bench_replay_focus(iters));
    if (wants(only, "replay_anchor"))
        report(bench_replay_anchor(iters));

    // GL benchmarks
    if (wants(only, "fade_batches")) {
        bench_gl_context_init(&argc, argv);
        report(bench_fade_batches(iters));
    }

    return 0;
}
