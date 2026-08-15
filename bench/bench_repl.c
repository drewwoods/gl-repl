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
 * `make bench-web` builds this same file to wasm and runs it under node,
 * because wasm cost is not a fixed multiple of native cost: measured against
 * one machine's native release build, per-op cost came out ~1.2x on
 * replay_long but ~2.2x on normalize_large_doc, which is enough to reorder
 * what looks expensive. Three things to know before reading those numbers:
 *
 *   - They are comparable web-to-web only. Every example-driven row uses the
 *     same frozen 40-scene corpus from bench/bench-data, so native and web runs do
 *     not drift when the live examples catalog changes. The synthetic-document
 *     rows (source_scope_*, normalize_large_doc, reformat_large_doc,
 *     replay_long) build their own input as before.
 *   - fade_batches does not run. node has no GPU and no WebGL context, so the
 *     one sub-benchmark that emits real draw calls skips itself. Nothing here
 *     sees the gl4es -> WebGL2 -> browser-GL cost of the actual draw path.
 *   - Rows that emit GL incidentally (replay_long) still link real gl4es, so
 *     they carry its client-side work against no live context, where the
 *     native build with no context is closer to a no-op. Treat that row's
 *     web/native ratio with more suspicion than the pure-CPU ones.
 *
 * Sub-benchmarks (names match the `--only` filter strings and the printed
 * labels):
 *   parse_lines       - repl_parse_command on every example line
 *   feed_examples     - full editor_feed_line path on every example
 *   flatten_examples  - load each example then call repl_flatten_commands
 *   flatten_grass     - full flatten of the "Swaying grass field (rand + t)"
 *                       built-in, resolved by display name
 *   flatten_orrery    - full flatten of the "Orrery (labels track 3D orbits)"
 *                       built-in, resolved by display name
 *   flatten_corpus    - full flatten of every frozen benchmark scene
 *                       index (human output sorted by minimum time)
 *   flatten_phases    - per-phase split (reparse / scalar assign / scratch
 *                       assign / derived remainder) of a full flatten, read
 *                       from the existing PROF_FLATTEN_* profiler sections
 *   flatten_refresh   - t refresh of Grass, Orrery, and value-only Wave
 *                       through the production full-vs-rebake boundary
 *   flatten_whale     - full flatten of the dynamic-topology Whale scene at
 *                       several `t` values; reports the flat count per sample
 *   slider_drag       - variable-panel drag: 100 motion events timed apart
 *                       from the single mouse-up persistence edit, for a
 *                       value-only and a structural variable
 *   source_scope_query- sweep source-scope depth/scope queries over a deep
 *                       synthetic document (isolates the amortized-O(1)
 *                       prefix-depth cache lookup)
 *   source_scope_churn- invalidate + query per op (isolates the O(N) prefix
 *                       cache rebuild paid once per document change)
 *   normalize_large_doc- parse+normalize a line against a large live document
 *                       (integration guard: the normalize entry must reuse the
 *                       warm live source-scope cache)
 *   reformat_large_doc - whole-document reformat over the same large live
 *                       document (direct guard for the user-visible reformat
 *                       path that calls normalize once per row)
 *   replay_examples   - start a replay and step it to completion
 *   replay_long       - feed a synthetic large scene and step replay to end
 *   cpuprof_sample    - the profiler's own per-frame cost: bin lookup, full
 *                       histogram record (rotating + dependency-chained),
 *                       live prof_begin/prof_end pair, nesting-guard A/B,
 *                       frame tick, and the legend-hover stats readback. The
 *                       only row here that measures instrumentation rather
 *                       than product work.
 *   fade_batches      - drive repl_execute_program() per fade batch with a packed
 *                       batch buffer whose old_pcs sit deep in a long flat
 *                       command stream (exercises the per-batch prefix
 *                       walk that dominates late-replay fade-in cost).
 *
 * Output is one line per sub-benchmark with mean / min / iterations / per-op
 * cost. CSV mode (`--csv`) is suitable for diffing across machines.
 *
 * This benchmark reports wall time only. The CSV output is intended for
 * comparing like-for-like runs on the same machine and build.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "editor/input.h"         /* editor_feed_line */
#include "editor/state.h"         /* editor_state_edit_line */
#include <math.h>
#include "repl/flatten.h"
#include "repl/flatten_query.h"
#include "repl/pipeline.h"
#include "repl/reformat.h"
#include "repl/state_notify.h"
#include "repl/eval.h"
#include "repl/example_loader.h"  /* repl_load_example_lines */
#include "bench-data/bench_examples.h"
#include "repl/executor.h"
#include "repl/load.h"           /* repl_load_apply_line - uncapped doc build */
#include "repl/normalize.h"      /* repl_parse_and_normalize_strict */
#include "repl/parser.h"
#include "repl/source_scope.h"   /* prefix-depth queries + cache invalidate */
#include "repl/state_views.h"     /* repl_state_normals_dirty - source-dirty probe */
#include "repl/state_owners.h"    /* repl_state_normals_dirty_clear,
                                     repl_state_flat_program_{dirty,clear_dirty} */
#include "subsystems/variable_panel/variable_panel_state.h"
#include "repl/time.h"           /* repl_set_time - Whale topology sweep */
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "support/cpuprof.h"     /* PROF_FLATTEN_* phase readback */
#include "support/histogram.h"   /* sampler-overhead rows */
#include "app/glr_ctrl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>          /* uname() for the machine-id CSV preamble */
#include <time.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>           /* host identification for the CSV preamble */
#endif

#ifdef GL_STUBS
#include <GL/gl_stub_counts.h>
#else
/* Real-GL build: pull in GLUT so we can create an actual current
 * context before running sub-benchmarks that emit draw calls. The
 * stub headers deliberately do NOT get included here - the Makefile
 * picks between stub and system GL headers via -I ordering. */
#include "gl_includes.h"
#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#endif
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

/* Set when a named frozen-scene case cannot be resolved. main() returns
 * non-zero so a damaged benchmark snapshot fails the run loudly instead of
 * silently dropping a benchmark row. */
static int g_case_missing = 0;

/* The benchmark corpus is intentionally separate from the live example
 * catalog. These accessors keep the benchmark loops readable while making
 * it impossible for a live-catalog edit to change their workload. */
static int bench_example_count(void) {
    return g_bench_example_count;
}

static const char *const *bench_example_lines(int idx) {
    return (idx >= 0 && idx < g_bench_example_count)
        ? g_bench_examples[idx].lines : NULL;
}

static const char *bench_example_name(int idx) {
    return (idx >= 0 && idx < g_bench_example_count)
        ? g_bench_examples[idx].name : NULL;
}

/* Resolve a benchmark case by its exact frozen-corpus display name. Indices are
 * deliberately not hard-coded: the frozen manifest owns their order. Absence
 * is always a hard error - a damaged benchmark snapshot must not silently drop
 * a row. */
static int example_index_by_name(const char *display_name) {
    int n = bench_example_count();
    for (int i = 0; i < n; i++) {
        const char *name = bench_example_name(i);
        if (name && strcmp(name, display_name) == 0)
            return i;
    }
    fprintf(stderr, "ERROR: benchmark case not found: \"%s\"\n", display_name);
    g_case_missing = 1;
    return -1;
}

/* Post-load runtime baseline. flatten writes g_predef_vars[] on
 * CMD_VAR_ASSIGN and the scratch arrays on CMD_SCRATCH_ASSIGN, so every
 * repeated flatten must start from the same values or each sample measures a
 * different workload (and, for the scratch examples, a different one every
 * time). */
typedef struct {
    float predef[MAX_PREDEF_VARS];
    float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
} BenchBaseline;

static void baseline_capture(BenchBaseline *b) {
    repl_copy_predef_values(b->predef, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(b->scratch);
}

static void baseline_restore(const BenchBaseline *b) {
    repl_restore_predef_values(b->predef, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(b->scratch);
}

static int example_line_count(int idx) {
    const char *const *lines = bench_example_lines(idx);
    int n = 0;
    if (!lines) return 0;
    while (lines[n]) n++;
    return n;
}

static long long total_example_lines(void) {
    long long total = 0;
    int n = bench_example_count();
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
    int n_examples = bench_example_count();
    long long total_lines = total_example_lines();
    const char **flat = (const char **)malloc(sizeof(*flat) * (size_t)total_lines);
    long long flat_n = 0;
    for (int e = 0; e < n_examples; e++) {
        const char *const *ls = bench_example_lines(e);
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
    int n_examples = bench_example_count();
    long long lines_per_iter = total_example_lines();

    BenchResult r = { .name = "feed_examples", .unit = "lines",
                      .min_sec = 1e18 };

    /* load_example_lines() already resets repl_state_document_cmds() / repl_state_flat_program_count() and
     * calls init_predef_vars(), so an extra fresh_repl() before each
     * load would just bill duplicate reset work to this benchmark.
     * Examples declare their own float vars, so we don't need
     * declare_test_idents() here either. */
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int e = 0; e < n_examples; e++) {
            repl_load_example_lines(bench_example_lines(e));
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

/* A verbatim, frozen copy of the "Wave surface" built-in example (a
 * nested-for surface that unrolls to a large flat program). Hardcoded here so
 * the flatten benchmark's workload is fixed regardless of changes to the
 * built-in example list - adding, reordering, or editing examples in
 * src/repl/examples.c can't move this number. The @camera / @cfg metadata
 * lines are kept and ordered as the real example loader expects, so camera
 * rows are stripped while the post-load command set stays fixed. */
static const char *const k_flatten_bench_scene[] = {
    "// @cfg vertex_points = 0",
    "static float grid, extent, x, y, z, invGradMag; // strip cell index, world extent, vertex coords, 1/|gradient|",
    "static float amp = 0.4;    // wave amplitude (peak |y|)",
    "static float freq = 2.5;   // spatial frequency along x and z",
    "static float zPhase = 0.7; // z-axis time phase: z evolves slower than x (<1)",
    "glTranslatef(0.0f, 0.0f, -5.0f);   // @camera dist",
    "glRotatef(20.0f, 1.0f, 0.0f, 0.0f);   // @camera rx",
    "glRotatef(30.0f, 0.0f, 1.0f, 0.0f);   // @camera ry",
    "glTranslatef(0.0f, 0.0f, 0.0f);   // @camera pan",
    "",
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
    repl_load_example_lines(k_flatten_bench_scene);

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
             * repl_state_document_cmds()[]), so we don't need to toggle any dirty flag
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

/* ---- bench: full flatten of the frozen benchmark scenes ---------------- */

/* Shared kernel for the named frozen-scene cases, flatten_corpus, and
 * flatten_phases: load one frozen scene, then time `inner` full flattens per
 * timer sample, each starting from the identical post-load baseline. */
static BenchResult bench_flatten_one(const char *bench_name, int example_idx,
                                     int iters, int inner, int *flat_cmds_out) {
    BenchResult r = { .name = bench_name, .unit = "flattens",
                      .min_sec = 1e18 };
    BenchBaseline base;

    repl_load_example_lines(bench_example_lines(example_idx));
    baseline_capture(&base);
    /* One warm flatten outside the timer so the source-scope depth cache is
     * built and the first timed sample isn't billed for it. */
    repl_flatten_commands(editor_state_edit_line());

    /* The timer brackets each flatten alone: the baseline restore between
     * them is setup, not workload, and leaving it inside would bill it to
     * flatten and desynchronize this row from the flatten_phases split. */
    for (int it = 0; it < iters; it++) {
        double sample = 0.0;
        for (int k = 0; k < inner; k++) {
            baseline_restore(&base);
            double t0 = now_seconds();
            repl_flatten_commands(editor_state_edit_line());
            sample += now_seconds() - t0;
        }
        if (sample < r.min_sec) r.min_sec = sample;
        r.total_sec += sample;
        r.ops += inner;
        r.iters++;
    }

    if (flat_cmds_out)
        *flat_cmds_out = repl_state_flat_program_count();
    baseline_restore(&base);
    return r;
}

/* Cold-cache variant of bench_flatten_one: each timed flatten starts from an
 * invalidated expression cache, so the sample includes rebuilding every
 * line's compiled programs (the cost an edit pays on its next frame). The
 * warm rows above never see this - their cache builds once before the timer
 * - so an edit-time regression cannot hide inside a steady-state number. */
static BenchResult bench_flatten_one_cold(const char *bench_name,
                                          int example_idx, int iters,
                                          int inner, int *flat_cmds_out) {
    BenchResult r = { .name = bench_name, .unit = "flattens",
                      .min_sec = 1e18 };
    BenchBaseline base;

    repl_load_example_lines(bench_example_lines(example_idx));
    baseline_capture(&base);
    repl_flatten_commands(editor_state_edit_line());

    for (int it = 0; it < iters; it++) {
        double sample = 0.0;
        for (int k = 0; k < inner; k++) {
            baseline_restore(&base);
            repl_expr_cache_invalidate(repl_expr_cache_live());
            double t0 = now_seconds();
            repl_flatten_commands(editor_state_edit_line());
            sample += now_seconds() - t0;
        }
        if (sample < r.min_sec) r.min_sec = sample;
        r.total_sec += sample;
        r.ops += inner;
        r.iters++;
    }

    if (flat_cmds_out)
        *flat_cmds_out = repl_state_flat_program_count();
    baseline_restore(&base);
    return r;
}

/* Named real-scene case, resolved by exact display name. A missing case is a
 * hard error (g_case_missing), never a silently skipped row. Reports two
 * rows: the warm compiled full flatten (the steady-state animated-frame
 * cost) and the cold-cache full flatten (the first frame after an edit). */
static void run_named_flatten(const char *bench_name, const char *display_name,
                              int iters) {
    int idx = example_index_by_name(display_name);
    int flat_cmds = 0;
    char cold_name[96];
    if (idx < 0)
        return;
    report(bench_flatten_one(bench_name, idx, iters, 16, &flat_cmds));
    if (!g_csv)
        fprintf(stderr, "  (%s: \"%s\" idx=%d, flat_cmds=%d)\n",
                bench_name, display_name, idx, flat_cmds);
    snprintf(cold_name, sizeof(cold_name), "%s_cold", bench_name);
    report(bench_flatten_one_cold(cold_name, idx, iters, 16, NULL));
}

/* ---- bench: full flatten of every frozen benchmark scene --------------- */

#define CORPUS_ROW_NAME_MAX 32

typedef struct {
    int        example_idx;
    int        flat_cmds;
    double     mean_ms;    /* reported: mean per-iteration wall time */
    double     min_op_us;  /* ranking key: fastest observed per-flatten time */
    BenchResult result;
    char       row_name[CORPUS_ROW_NAME_MAX];
} CorpusCase;

/* Rank by the minimum per-flatten time, not the mean. Benchmark noise is
 * one-sided - a scheduling stall can only add time - so the minimum is the
 * least-contaminated estimate of a scene's cost. The mean remains in the
 * reported columns, but a single descheduled iteration should not determine
 * the human-readable order. */
static int corpus_case_cmp_slower_first(const void *a, const void *b) {
    double da = ((const CorpusCase *)a)->min_op_us;
    double db = ((const CorpusCase *)b)->min_op_us;
    if (da < db) return 1;
    if (da > db) return -1;
    return 0;
}

/* Time a full flatten of every frozen benchmark scene. Human output lists all
 * cases sorted slowest-first by minimum per-flatten time; CSV emits one row per
 * frozen-corpus index in manifest order, preceded by a `#` comment carrying
 * the display name so the existing CSV columns stay unchanged. */
static void bench_flatten_corpus(int iters) {
    enum { CORPUS_INNER = 8 };
    int n = bench_example_count();
    CorpusCase *cases = (CorpusCase *)malloc(sizeof(*cases) * (size_t)n);
    if (!cases)
        return;

    for (int e = 0; e < n; e++) {
        cases[e].example_idx = e;
        cases[e].flat_cmds = 0;
        snprintf(cases[e].row_name, sizeof(cases[e].row_name),
                 "flatten_corpus_%02d", e);
        cases[e].result = bench_flatten_one(cases[e].row_name, e, iters,
                                            CORPUS_INNER, &cases[e].flat_cmds);
        cases[e].result.name = cases[e].row_name;
        cases[e].mean_ms = (cases[e].result.iters > 0)
                         ? cases[e].result.total_sec * 1000.0 /
                           (double)cases[e].result.iters
                         : 0.0;
        /* min_sec is the fastest iteration, i.e. a batch of CORPUS_INNER
         * flattens; normalize to one flatten so the key is comparable to the
         * per_op_us column. */
        cases[e].min_op_us = (cases[e].result.iters > 0)
                           ? cases[e].result.min_sec * 1e6 /
                             (double)CORPUS_INNER
                           : 0.0;
    }

    if (g_csv) {
        for (int e = 0; e < n; e++) {
            printf("# %s = %s (flat_cmds=%d)\n", cases[e].row_name,
                   bench_example_name(e), cases[e].flat_cmds);
            report(cases[e].result);
        }
    } else {
        /* Sorted slowest-first by min-per-flatten; per_op_us below is the mean
         * and may disagree on a contended machine (that is why it is not the
         * key). Print the key so the ordering is checkable by eye. */
        qsort(cases, (size_t)n, sizeof(*cases), corpus_case_cmp_slower_first);
        for (int e = 0; e < n; e++) {
            printf("  # %s = %s (flat_cmds=%d, min-per-flatten=%.3f us)\n",
                   cases[e].row_name,
                   bench_example_name(cases[e].example_idx),
                   cases[e].flat_cmds, cases[e].min_op_us);
            report(cases[e].result);
        }
    }
    free(cases);
}

/* ---- bench: per-phase split of a full flatten -------------------------- */

/* Splits one full flatten into the three eval-heavy leaf phases the
 * PROF_FLATTEN_* probes already accumulate, plus the derived structural
 * remainder (loop/if/call iteration + append). Reads prof_section_last_us
 * after each flatten - repl_flatten_program commits the accumulators once per
 * call - so no new timers are added inside the evaluator primitives. */
static void bench_flatten_phases_case(const char *label, int example_idx,
                                      int iters) {
    enum { INNER = 16 };
    BenchBaseline base;
    char names[5][48];
    BenchResult rows[5];
    static const char *const suffix[5] = {
        "total", "reparse", "var_assign", "scratch_assign", "remainder"
    };

    for (int p = 0; p < 5; p++) {
        snprintf(names[p], sizeof(names[p]), "%s_%s", label, suffix[p]);
        rows[p] = (BenchResult){ .name = names[p], .unit = "flattens",
                                 .min_sec = 1e18 };
    }

    repl_load_example_lines(bench_example_lines(example_idx));
    baseline_capture(&base);
    repl_flatten_commands(editor_state_edit_line());

    for (int it = 0; it < iters; it++) {
        double sample[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };

        for (int k = 0; k < INNER; k++) {
            baseline_restore(&base);
            double t0 = now_seconds();
            repl_flatten_commands(editor_state_edit_line());
            sample[0] += now_seconds() - t0;
            sample[1] += prof_section_last_us(PROF_FLATTEN_REPARSE) * 1e-6;
            sample[2] += prof_section_last_us(PROF_FLATTEN_VAR_ASSIGN) * 1e-6;
            sample[3] += prof_section_last_us(PROF_FLATTEN_SCRATCH_ASSIGN) * 1e-6;
        }
        sample[4] = sample[0] - sample[1] - sample[2] - sample[3];
        if (sample[4] < 0.0) sample[4] = 0.0;

        for (int p = 0; p < 5; p++) {
            if (sample[p] < rows[p].min_sec) rows[p].min_sec = sample[p];
            rows[p].total_sec += sample[p];
            rows[p].ops += INNER;
            rows[p].iters++;
        }
    }
    baseline_restore(&base);

    for (int p = 0; p < 5; p++)
        report(rows[p]);
}

static void bench_flatten_phases(int iters) {
    static const struct { const char *label; const char *display; } cases[] = {
        { "phase_grass",  "Swaying grass field (rand + t)" },
        { "phase_orrery", "Orrery (labels track 3D orbits)" },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int idx = example_index_by_name(cases[c].display);
        if (idx < 0) continue;
        bench_flatten_phases_case(cases[c].label, idx, iters);
    }
}

/* ---- bench: production refresh boundary on frozen animated scenes ------ */

static BenchResult bench_flatten_refresh_one(const char *name,
                                             const char *const *scene,
                                             int iters) {
    enum { INNER = 32 };
    BenchResult r = { .name = name, .unit = "refreshes", .min_sec = 1e18 };
    BenchBaseline base;
    int t_idx;
    float sample_time;
    ReplFlatRefreshKind expected;
    int t_structural;
    int rebake_ok;

    repl_load_example_lines(scene);
    baseline_capture(&base);
    repl_flatten_commands(editor_state_edit_line());
    repl_state_flat_program_clear_dirty();
    t_idx = repl_state_variables().time_var_idx;
    if (t_idx < 0 || t_idx >= MAX_PREDEF_VARS) {
        fprintf(stderr, "ERROR: %s has no t slot\n", name);
        g_case_missing = 1;
        return r;
    }
    sample_time = base.predef[t_idx] + 0.75f;
    {
        ReplExprDepMask t_bit = (ReplExprDepMask)1u << t_idx;
        t_structural =
            (repl_state_flat_program_structural_dep_mask() & t_bit) != 0;
        rebake_ok = repl_state_flat_program_rebake_ok();
        if (t_structural)
            expected = REPL_FLAT_REFRESH_FULL;
        else if ((repl_state_flat_program_value_dep_mask() & t_bit) &&
                 rebake_ok)
            expected = REPL_FLAT_REFRESH_REBAKE;
        else
            expected = REPL_FLAT_REFRESH_NONE;
    }
    if (expected == REPL_FLAT_REFRESH_NONE) {
        fprintf(stderr, "ERROR: %s does not depend on t\n", name);
        g_case_missing = 1;
        return r;
    }

    for (int it = 0; it < iters; it++) {
        double sample = 0.0;
        for (int k = 0; k < INNER; k++) {
            ReplFlatRefreshKind kind;
            baseline_restore(&base);
            repl_set_time(sample_time + 0.01f * (float)(k & 1));
            double t0 = now_seconds();
            kind = repl_refresh_flat_program(editor_state_edit_line());
            sample += now_seconds() - t0;
            if (kind != expected) {
                fprintf(stderr,
                        "ERROR: %s expected refresh route %d, got %d\n",
                        name, (int)expected, (int)kind);
                g_case_missing = 1;
                baseline_restore(&base);
                return r;
            }
        }
        if (sample < r.min_sec) r.min_sec = sample;
        r.total_sec += sample;
        r.ops += INNER;
        r.iters++;
    }
    baseline_restore(&base);
    if (!g_csv)
        fprintf(stderr,
                "  (%s: flat_cmds=%d, route=%s%s, rebake_ok=%d)\n",
                name, repl_state_flat_program_count(),
                expected == REPL_FLAT_REFRESH_REBAKE ? "rebake" : "full",
                t_structural ? ", t-structural" : "", rebake_ok);
    return r;
}

static void bench_flatten_refresh(int iters) {
    static const struct { const char *row; const char *display; } cases[] = {
        { "refresh_grass", "Swaying grass field (rand + t)" },
        { "refresh_orrery", "Orrery (labels track 3D orbits)" },
        { "refresh_wave", "Wave surface (analytic normals)" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int idx = example_index_by_name(cases[i].display);
        if (idx >= 0)
            report(bench_flatten_refresh_one(cases[i].row,
                                             bench_example_lines(idx), iters));
    }
}

/* ---- bench: refresh boundary on a variable-panel scrub ----------------- */

/* The `t` cases above measure the animation clock. This one measures the
 * other live-edit motion: dragging one global's slider. A global that feeds a
 * function-scoped local reports a structural dependency, so scrubbing it
 * takes a full flatten; a value-only global can rebake in place. The printed
 * route is the part to watch, and the timing shows its cost. */
static BenchResult bench_refresh_slider_one(const char *name,
                                            const char *const *scene,
                                            const char *var_name,
                                            ReplFlatRefreshKind expected,
                                            int iters) {
    enum { INNER = 32 };
    BenchResult r = { .name = name, .unit = "refreshes", .min_sec = 1e18 };
    BenchBaseline base;
    ReplFlatRefreshKind classified;
    ReplExprDepMask bit;
    int structural, slot;

    repl_load_example_lines(scene);
    baseline_capture(&base);
    repl_flatten_commands(editor_state_edit_line());
    repl_state_flat_program_clear_dirty();

    slot = repl_eval_find_predef_var_idx(var_name);
    if (slot < 0 || slot >= MAX_PREDEF_VARS) {
        fprintf(stderr, "ERROR: %s has no '%s' slot\n", name, var_name);
        g_case_missing = 1;
        return r;
    }
    bit = (ReplExprDepMask)1u << slot;
    structural = (repl_state_flat_program_structural_dep_mask() & bit) != 0;
    if (structural)
        classified = REPL_FLAT_REFRESH_FULL;
    else if ((repl_state_flat_program_value_dep_mask() & bit) &&
             repl_state_flat_program_rebake_ok())
        classified = REPL_FLAT_REFRESH_REBAKE;
    else {
        fprintf(stderr, "ERROR: %s does not depend on '%s'\n", name, var_name);
        g_case_missing = 1;
        return r;
    }
    if (classified != expected) {
        fprintf(stderr,
                "ERROR: %s expected dependency route %d, classified as %d\n",
                name, (int)expected, (int)classified);
        g_case_missing = 1;
        return r;
    }

    for (int it = 0; it < iters; it++) {
        double sample = 0.0;
        for (int k = 0; k < INNER; k++) {
            ReplFlatRefreshKind kind;
            baseline_restore(&base);
            /* One drag step: nudge the value, then refresh exactly as the
             * panel's motion handler does. */
            g_predef_vars_mut[slot].value =
                base.predef[slot] * (1.0f + 0.01f * (float)((k & 7) + 1));
            repl_state_notify_predef_value_changed(slot);
            double t0 = now_seconds();
            kind = repl_refresh_flat_program(editor_state_edit_line());
            sample += now_seconds() - t0;
            if (kind != expected) {
                fprintf(stderr,
                        "ERROR: %s expected refresh route %d, got %d\n",
                        name, (int)expected, (int)kind);
                g_case_missing = 1;
                baseline_restore(&base);
                return r;
            }
        }
        if (sample < r.min_sec) r.min_sec = sample;
        r.total_sec += sample;
        r.ops += INNER;
        r.iters++;
    }
    baseline_restore(&base);
    if (!g_csv)
        fprintf(stderr, "  (%s: flat_cmds=%d, var=%s, route=%s%s)\n",
                name, repl_state_flat_program_count(), var_name,
                expected == REPL_FLAT_REFRESH_REBAKE ? "rebake" : "full",
                structural ? ", structural" : "");
    return r;
}

static void bench_refresh_slider(int iters) {
    static const struct {
        const char *row;
        const char *display;
        const char *var;
        ReplFlatRefreshKind expected;
    } cases[] = {
        /* Both sides of the routing rule, and the first row is the one that
         * keeps this honest: without a case that still REBAKEs, an
         * "everything became structural" regression would leave every route
         * assertion satisfied because every pinned expectation would be FULL.
         *
         * Wave's `amplitude` scales already-emitted vertex/normal/colour arguments
         * through global scratch only - no local, no loop bound, no condition
         * - so it is genuinely value-only.
         *
         * The two orrery rows are both structural, for different reasons worth
         * keeping visible: EARTH_RATE feeds planetKepler()'s local `th`
         * *directly*, while ORB_SCALE reaches planet()'s local `th` only
         * transitively, by way of the global `orbitR` that `th` reads. */
        { "slider_wave_value", "Wave surface (analytic normals)",
          "amplitude", REPL_FLAT_REFRESH_REBAKE },
        { "slider_orrery_local", "Orrery (labels track 3D orbits)",
          "EARTH_RATE", REPL_FLAT_REFRESH_FULL },
        { "slider_orrery_transitive", "Orrery (labels track 3D orbits)",
          "ORB_SCALE", REPL_FLAT_REFRESH_FULL },
        { "slider_grass", "Swaying grass field (rand + t)", "field",
          REPL_FLAT_REFRESH_FULL },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int idx = example_index_by_name(cases[i].display);
        if (idx >= 0)
            report(bench_refresh_slider_one(cases[i].row,
                                            bench_example_lines(idx),
                                            cases[i].var, cases[i].expected,
                                            iters));
    }
}

/* ---- bench: dynamic-topology full flatten (Whale) ---------------------- */

/* Whale's droplets are gated by per-particle `if((t - spawnDelay) > 0 && ...)`
 * conditions, so its *flat-command count* changes with `t` as particles enter
 * and leave their active interval. That makes it the canary for the rebake
 * fast path: a `t` change here can never reuse the existing flat topology, so
 * this case always measures a warm compiled FULL flatten, never a rebake.
 *
 * We sample several `t` values, report the flat count alongside each timing,
 * and fail the run unless at least two samples disagree on the count (i.e. the
 * scene really is dynamic-topology). */
static void bench_flatten_whale(int iters) {
    static const float k_times[] = { 0.0f, 0.4f, 1.0f, 2.0f, 4.0f, 8.0f };
    enum { N_TIMES = (int)(sizeof(k_times) / sizeof(k_times[0])), INNER = 8 };
    int idx = example_index_by_name("Whale (particle system + lit model)");
    int flat_counts[N_TIMES];
    BenchBaseline base;
    char names[N_TIMES][40];

    if (idx < 0)
        return;

    repl_load_example_lines(bench_example_lines(idx));
    baseline_capture(&base);
    repl_set_time(k_times[0]);
    repl_flatten_commands(editor_state_edit_line());

    for (int s = 0; s < N_TIMES; s++) {
        BenchResult r;
        snprintf(names[s], sizeof(names[s]), "flatten_whale_t%.1f",
                 (double)k_times[s]);
        r = (BenchResult){ .name = names[s], .unit = "flattens",
                           .min_sec = 1e18 };

        for (int it = 0; it < iters; it++) {
            double sample = 0.0;
            for (int k = 0; k < INNER; k++) {
                baseline_restore(&base);
                repl_set_time(k_times[s]);
                double t0 = now_seconds();
                repl_flatten_commands(editor_state_edit_line());
                sample += now_seconds() - t0;
            }
            if (sample < r.min_sec) r.min_sec = sample;
            r.total_sec += sample;
            r.ops += INNER;
            r.iters++;
        }
        flat_counts[s] = repl_state_flat_program_count();

        if (g_csv)
            printf("# %s: flat_cmds=%d\n", names[s], flat_counts[s]);
        report(r);
        if (!g_csv)
            fprintf(stderr, "  (%s: flat_cmds=%d)\n", names[s], flat_counts[s]);
    }
    baseline_restore(&base);

    int distinct = 0;
    for (int s = 0; s < N_TIMES; s++) {
        int seen = 0;
        for (int p = 0; p < s; p++)
            if (flat_counts[p] == flat_counts[s]) { seen = 1; break; }
        if (!seen) distinct++;
    }
    if (distinct < 2) {
        fprintf(stderr,
                "ERROR: flatten_whale expected >= 2 distinct flat counts "
                "across the time grid, got %d - the dynamic-topology canary "
                "no longer varies with t\n", distinct);
        g_case_missing = 1;
    }
}

/* ---- bench: variable-panel slider drag transaction --------------------- */

/* Consume a pending flat-program rebuild the way glr_ctrl_display_frame() does:
 * full dirty (source edit or structural value change) runs a full flatten; a
 * value-only change recorded in args_dirty_mask rebakes the stream in place;
 * otherwise the program is already current. Returns 1 if any rebuild ran, 0
 * if it was clean.
 *
 * This *is* the rebuild counter. Checking only repl_state_flat_program_dirty()
 * would undercount: a value-only slider change lands in args_dirty_mask, not
 * the full flag, so the `value` row's motion would drain nothing and report
 * zero refreshes. repl_state_normals_dirty() is likewise not the counter - it
 * gates autonormal recomputation, a different cache. Without this drain a
 * motion loop would time only pointer dispatch and omit the deferred
 * flatten/rebake work. */
static int drain_pending_flatten(void) {
    return repl_refresh_flat_program(editor_state_edit_line()) !=
           REPL_FLAT_REFRESH_NONE;
}

/* A slider drag is 100+ pointer-motion events and one mouse-up. Motion applies
 * the live value only; the declaration is rewritten once on release. A drag
 * must therefore perform zero source-dirty marks during motion and exactly
 * one on release.
 *
 * Each motion is followed by drain_pending_flatten(), so a timed motion is a
 * full event -> dirty -> frame-rebuild round trip, as the app performs it.
 *
 * Two cases, by what the dragged variable feeds:
 *   value      - `amp` only scales already-emitted vertices, so the flat
 *                topology stays fixed and values rebake in place;
 *   structural - `bound` is a loop bound, so a change must re-flatten fully.
 *
 * Read each row against its own baseline, not against the other row: the
 * scenes differ in trip count (40 vs bound=20) and in how many flattened
 * commands carry variable args, so the absolute value/structural gap reflects
 * scene size, not the rebake distinction.
 *
 * The mouse-up persistence edit is timed separately from the motion loop:
 * folding a once-per-drag source rewrite into a per-motion mean would hide it.
 */
static void bench_slider_drag_case(const char *label,
                                   const char *const *scene,
                                   const char *var_name,
                                   int iters) {
    enum { MOTIONS = 100 };
    char motion_name[48];
    char release_name[48];
    BenchResult motion, release;
    int var_idx;
    int source_dirty_during_motion = 0;
    int source_dirty_on_release = 0;
    long long motion_rebuilds = 0;

    snprintf(motion_name, sizeof(motion_name), "%s_motion", label);
    snprintf(release_name, sizeof(release_name), "%s_release", label);
    motion = (BenchResult){ .name = motion_name, .unit = "motions",
                            .min_sec = 1e18 };
    release = (BenchResult){ .name = release_name, .unit = "releases",
                             .min_sec = 1e18 };

    for (int it = 0; it < iters; it++) {
        double motion_sec = 0.0, release_sec = 0.0;
        double t0;

        /* Fresh document per iteration: the release edit rewrites the
         * declaration, so a reused document would drift its start value.
         * glr_ctrl_reset_all (not fresh_repl) - the scene declares its own
         * variable, and declare_test_idents would claim the name first and
         * make the `float ...` line a redeclaration error, leaving no
         * declaration row for the release edit to rewrite. */
        glr_ctrl_reset_all();
        variable_panel_set_visible(1);
        for (int i = 0; scene[i]; i++)
            editor_feed_line(scene[i]);
        repl_flatten_commands(editor_state_edit_line());

        var_idx = repl_eval_find_predef_var_idx(var_name);
        if (var_idx < 0) {
            fprintf(stderr, "ERROR: %s: variable '%s' not declared\n",
                    label, var_name);
            g_case_missing = 1;
            return;
        }

        variable_panel_handle_drag_begin(var_idx, /*log_mode=*/0, /*x=*/0);
        repl_state_normals_dirty_clear();
        /* Enter the loop with a clean flat program so the first motion's
         * rebuild is attributable to that motion and not to the load above. */
        drain_pending_flatten();

        t0 = now_seconds();
        for (int m = 1; m <= MOTIONS; m++) {
            glr_ctrl_router_handle_variable_panel_motion(m, 0);
            motion_rebuilds += drain_pending_flatten();
        }
        motion_sec = now_seconds() - t0;
        if (repl_state_normals_dirty())
            source_dirty_during_motion = 1;

        t0 = now_seconds();
        glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP);
        (void)drain_pending_flatten();
        release_sec = now_seconds() - t0;
        if (repl_state_normals_dirty())
            source_dirty_on_release = 1;

        if (motion_sec < motion.min_sec) motion.min_sec = motion_sec;
        motion.total_sec += motion_sec;
        motion.ops += MOTIONS;
        motion.iters++;

        if (release_sec < release.min_sec) release.min_sec = release_sec;
        release.total_sec += release_sec;
        release.ops += 1;
        release.iters++;
    }

    report(motion);
    report(release);

    /* The transaction invariant, asserted rather than just timed: a fast
     * motion loop that silently rewrote source would be a correctness bug the
     * timings alone would not reveal. */
    if (source_dirty_during_motion || !source_dirty_on_release) {
        fprintf(stderr,
                "ERROR: %s: expected zero source invalidations during motion "
                "and exactly one on release (motion=%d, release=%d)\n",
                label, source_dirty_during_motion, source_dirty_on_release);
        g_case_missing = 1;
    }
    /* Every motion moves the value, so every motion must have left a rebuild
     * for the frame to consume. If this drops to zero the timed region has
     * stopped containing a flatten and the row is measuring event routing
     * again - the exact defect this benchmark was rewritten to avoid. */
    {
        long long expect = (long long)MOTIONS * motion.iters;
        if (motion_rebuilds != expect) {
            fprintf(stderr,
                    "ERROR: %s: expected %lld flat-program rebuilds during "
                    "motion (one per motion), got %lld\n",
                    label, expect, motion_rebuilds);
            g_case_missing = 1;
        }
    }
    if (!g_csv)
        fprintf(stderr, "  (%s: %d motions, %lld rebuilds, source dirty during "
                        "motion=%d, on release=%d)\n",
                label, MOTIONS, motion_rebuilds, source_dirty_during_motion,
                source_dirty_on_release);
}

/* `amp` scales emitted vertices: value-only, no loop bound or condition. */
static const char *const k_slider_value_scene[] = {
    "float amp = 1;",
    "for(i, 0, 40) {",
        "glBegin(GL_TRIANGLES);",
        "glVertex3f(amp*i, 0, 0);",
        "glVertex3f(amp*i, amp, 0);",
        "glVertex3f(amp*i, 0, amp);",
        "glEnd();",
    "}",
    NULL,
};

/* `bound` is the loop bound: changing it changes the flat-command count. */
static const char *const k_slider_structural_scene[] = {
    "float bound = 20;",
    "for(i, 0, bound) {",
        "glBegin(GL_TRIANGLES);",
        "glVertex3f(i, 0, 0);",
        "glVertex3f(i, 1, 0);",
        "glVertex3f(i, 0, 1);",
        "glEnd();",
    "}",
    NULL,
};

static void bench_slider_drag(int iters) {
    bench_slider_drag_case("slider_value", k_slider_value_scene, "amp", iters);
    bench_slider_drag_case("slider_structural", k_slider_structural_scene,
                           "bound", iters);
}

/* ---- bench: cursor flat-cost query (statusbar budget readout) --------- */

/* Times repl_flatten_cost_at_line over every cursor line of the same
 * wave-surface scene flatten_examples uses, so the two rows compare
 * directly: the readout runs once per frame at snapshot build, the
 * flatten it annotates runs once per frame too. One op = one full
 * sweep of the document (every line classified + counted), i.e. a
 * deliberate worst case - the live app issues a single line per frame. */
static BenchResult bench_flat_cost_query(int iters) {
    BenchResult r = { .name = "flat_cost_query", .unit = "sweeps",
                      .min_sec = 1e18 };

    repl_load_example_lines(k_flatten_bench_scene);
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

/* ---- bench: stress the largest example's flatten ----------------------- */

/* This stress case picks the example with the highest flat-program count
 * after load and times repeated full flattens on it. It exercises the
 * editor-backed parse path at the largest built-in workload and reports
 * total / mean / minimum time so regressions remain visible.
 */
static BenchResult bench_spike_flatten_largest(int iters) {
    int n_examples = bench_example_count();
    int worst_idx = 0;
    int worst_flat = 0;
    float saved_vals[MAX_PREDEF_VARS];

    /* Pick the example with the largest flat-program count after load.
     * "Lines" alone underestimates: a short for-loop unrolls into many
     * flat commands, and that's what flatten actually walks. */
    for (int e = 0; e < n_examples; e++) {
        repl_load_example_lines(bench_example_lines(e));
        repl_flatten_commands(editor_state_edit_line());
        int n = repl_state_flat_program_count();
        if (n > worst_flat) {
            worst_flat = n;
            worst_idx = e;
        }
    }

    /* Reload the chosen example as the timed fixture. */
    repl_load_example_lines(bench_example_lines(worst_idx));

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
    int n_examples = bench_example_count();

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
            repl_load_example_lines(bench_example_lines(e));

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

/* Build a single scene that flattens to a large flat-cmd stream so we get a
 * longer-running replay workload with a fixed command shape.
 *
 * Sized to fit within MAX_FLAT_COMMANDS after expansion: when flatten would
 * exceed that limit,
 * flatten_append_cmd sets ctx->abort=1 and repl_flatten_program then
 * resets flat_count to 0 (capacity overflow is treated as a hard
 * failure, not a soft cap). The outer for-loop emits 12 flat cmds per
 * iteration, with 3 setup cmds before it (two CMD_VAR_DECLARE rows are
 * flatten-omitted): 12*340 + 3 = 4083. Keeping the loop count fixed makes
 * repeated runs use the same replay workload. */
static const char *const k_long_replay_scene[] = {
    "float a;",
    "float b;",
    "glClearColor(0.05, 0.05, 0.05, 1);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
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
     * measuring here. Re-using the same repl_state_document_cmds()[] across iterations is
     * fine because replay only mutates the replay state, not the source
     * commands. We mark repl_state_flat_program_dirty() between iterations so replay_start()
     * does a fresh flatten each time - that matches "what happens the
     * first time you press play". Note: replay_start() handles the
     * flatten itself and clears repl_state_flat_program_dirty(), so calling
     * repl_flatten_commands(editor_state_edit_line()) explicitly here would flatten twice. */
    repl_load_example_lines(k_long_replay_scene);

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
 * flat stream from index 0 up to pc *every call* - and in REPLAY_MODE_VERTEX
 * each step within that walk does its own O(pc) backward scan
 * (replay_find_open_begin_before / _tess_polygon_before), so the whole thing is
 * O(N^2) in the flat-program length, paid per frame and worst at the tail of a
 * long replay. This bench parks pc at the end of the long synthetic scene and
 * times the per-frame focus call, so the regression (and any fix) is visible as
 * a per-call number independent of the advance-step benchmarks above. */
static BenchResult bench_replay_focus(int iters) {
    BenchResult r = { .name = "replay_focus", .unit = "calls",
                      .min_sec = 1e18 };

    repl_load_example_lines(k_long_replay_scene);
    repl_mark_source_dirty();
    replay_start();

    /* Advance to the end so pc sits at the tail - the worst case the
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

    repl_load_example_lines(k_long_replay_scene);
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

/* Scene built to emit a long flat-command stream of many small primitives so
 * we can exercise the fade-batch rendering pass with large old_pc indices.
 * We unroll a for-loop that emits one triangle per iteration. Exceeding
 * MAX_FLAT_COMMANDS makes flatten abort and discard the partial flat stream
 * (flat_count -> 0). The loop body emits 11 flat cmds per iteration with 2
 * setup cmds before it (CMD_VAR_DECLARE rows are flatten-omitted):
 * 11*370 + 2 = 4072. */
static const char *const k_fade_bench_scene[] = {
    "float a;",
    "float b;",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
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

/* Populate a packed set of fade batches spaced near the tail of the flattened
 * command stream. Every batch's old_pc is large, so replay_find_open_* scans
 * exercise the full prefix cost. */
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
         * MAX_FLAT_COMMANDS is reduced. */
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

/* Drive the per-batch fade-render cost directly through
 * repl_execute_program(), with a skip-prefix and per-batch alpha. This keeps
 * the benchmark focused on the executor rather than the scene API. */
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

/* Refresh the fade plan from the live replay state - bench reinstalls
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

    /* Build the long scene and flatten once. The flatten pass runs inside
     * replay_start(); capture the resulting flat-program count before replay
     * playback changes the active limit. */
    repl_load_example_lines(k_fade_bench_scene);
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
     * replay speed (REPLAY_FADE_DURATION is 0.5s). age=0.25s -> alpha~0.5
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
     * full itersxinner call count. */
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
 * document, with no mutation in the loop, so the prefix-depth cache stays
 * warm. This isolates the amortized O(1) query cost. Recomputing the prefix
 * arrays per call instead of indexing the built cache produces a large
 * per-sweep increase. One operation is one full document sweep across five
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

/* Isolate the O(N) prefix-depth rebuild. Each operation invalidates the cache
 * as a document mutation does, then issues a query that forces a full
 * depth_cache_rebuild() over the document. One operation therefore measures
 * one invalidate plus one rebuild. */
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

/* Measure normalize/parse work against a large live document. The input line
 * is constant and has no visible variables, so the row isolates source-scope
 * work rather than expression evaluation. */
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

/* Measure the user-visible whole-document reformat path. Ctrl+Backslash
 * reformat-all normalizes once per command row, so unnecessary per-call
 * source-scope rebuilds show up as superlinear growth. One operation is one
 * full reformat pass over the deep document. */
static BenchResult bench_reformat_large_doc(int iters) {
    BenchResult r = { .name = "reformat_large_doc", .unit = "reformats",
                      .min_sec = 1e18 };
    int doc_count = load_deep_scope_doc(180);
    long long sink = 0;

    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        repl_reformat_program();
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops++;
        r.iters++;

        sink += repl_state_document_count();
        const char *line = editor_buffer_line(doc_count / 2);
        if (line)
            sink += (long long)strlen(line);
    }
    if (!g_csv)
        fprintf(stderr, "  (reformat_large_doc: doc rows=%d, checksum=%lld)\n",
                doc_count, sink);
    return r;
}

/* ---- bench: cpuprof instrumentation overhead --------------------------- */

/* What the profiler costs the frame it is measuring. Every other row in this
 * file measures work the app exists to do; these measure the tax on doing it,
 * so they are read against PROF_SECTION_COUNT rather than against each other:
 * a frame that samples every section pays the per-sample cost that many times,
 * plus one frame tick.
 *
 * The rows decompose one sample deliberately, because "prof_end got more
 * expensive" is only actionable if you can see which part:
 *
 *   cpuprof_bin_for_us    the log10 bin lookup alone. This is the bin half of
 *                         a record, and so also the shape of the whole record
 *                         before the running statistics were added.
 *   cpuprof_record        the whole record path - bin lookup, bin increment,
 *                         min/max/sum and the Welford mean/m2 update - with no
 *                         clock read, EMA or staleness, rotating across all
 *                         PROF_SECTION_COUNT histograms the way a frame does.
 *                         Minus the row above, this is what the statistics
 *                         cost per sample. The per-frame figure builds on it.
 *   cpuprof_record_chained the same call hammering one histogram, so Welford's
 *                         serial dependency stalls instead of resolving
 *                         between frames. Worst case, not the app's.
 *   cpuprof_sample_live   prof_begin/prof_end on the real clock: what a
 *                         section actually pays in a frame, bookkeeping plus
 *                         two mach_absolute_time/clock_gettime reads. The
 *                         denominator for whether the delta above matters.
 *   cpuprof_nest_inert / cpuprof_nest_guarded
 *                         the same pinned-clock child sample with no catalog
 *                         tree installed, then with its parent open in an
 *                         installed parent/child tree. Their delta isolates the
 *                         nesting guard's normal-path parent lookup from the
 *                         clock reads and from one-sided scheduler noise.
 *   cpuprof_frame_tick    prof_frame_tick(): the PROF_SECTION_COUNT staleness
 *                         sweep, the frame-time histogram record, and the
 *                         three FPS windows. Once per frame, not per section.
 *   cpuprof_stats_read    prof_section_stats() readback - the legend-hover
 *                         path, at most once per frame per hovered series.
 *
 * The first two rows call src/support/histogram.c directly rather than going
 * through prof_end(), because the clock read prof_end() wraps them in is both
 * the largest term in a live sample and the noisiest: it is platform overhead
 * nobody here can change, and on a loaded machine it comfortably hides a few
 * nanoseconds of arithmetic beside it.
 *
 * Not covered: the hover box's text drawing (src/ui/support/cpuprof.c). That
 * is GL/text work in the UI layer, it only runs while the pointer is over the
 * legend, and this binary has no UI objects linked. */

/* Elapsed times in us, spanning what the real sections cover: sub-microsecond
 * helpers through a 100 ms whole-frame stall. Spread on purpose - the bin
 * lookup is a log10 and the stats update carries two comparisons, so a table
 * of one repeated value would measure a perfectly predicted branch pattern
 * that no real section produces. */
static const double k_prof_sample_us[] = {
    0.05, 0.3, 0.8, 2.7, 5.1, 9.4, 41.0, 63.0,
    118.0, 250.0, 780.0, 1350.0, 3200.0, 16700.0, 22000.0, 104000.0
};
static const int k_prof_sample_n =
    (int)(sizeof(k_prof_sample_us) / sizeof(k_prof_sample_us[0]));

/* Samples per timed iteration. Large enough that one iteration is milliseconds
 * rather than a handful of clock ticks - a single sample is tens of
 * nanoseconds, well under the resolution of the benchmark's own clock. */
enum { PROF_BENCH_INNER = 20000 };

/* The section these rows drive. Any index works (the timer is an array), but
 * PROF_FRAME_TOTAL is instrumented only from the display callback, which this
 * binary never runs, so no other row's numbers pass through it. Each case
 * still calls prof_test_reset() when done. */
#define PROF_BENCH_SECTION PROF_FRAME_TOTAL

/* Adjacent real catalog slots make an unambiguous synthetic tree without
 * linking bench_repl to the app's display table: each child resolves to the
 * immediately preceding depth-0 parent, while every other slot is top-level.
 * Rotate across several children as a real frame rotates across sections; one
 * repeated histogram would serialize Welford's statistics update and could
 * hide independent guard instructions behind that dependency chain. Parent
 * resolution and profiler reset stay outside the timer, so these rows price
 * only the steady-state sample path. */
#define PROF_BENCH_NEST_PARENT PROF_RENDER3D
enum { PROF_BENCH_NEST_CHILD_COUNT = 16 };

static int bench_cpuprof_nesting_depth(ProfSection s) {
    int section_idx = (int)s;
    return section_idx > (int)PROF_BENCH_NEST_PARENT &&
           section_idx <= (int)PROF_BENCH_NEST_PARENT +
                          PROF_BENCH_NEST_CHILD_COUNT
               ? 1 : 0;
}

static BenchResult bench_cpuprof_bin_for_us(int iters) {
    BenchResult r = { .name = "cpuprof_bin_for_us", .unit = "samples",
                      .min_sec = 1e18 };
    long long sink = 0;
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < PROF_BENCH_INNER; k++)
            sink += histogram_bin_for_us(
                        k_prof_sample_us[k % k_prof_sample_n]);
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += PROF_BENCH_INNER;
        r.iters++;
    }
    if (!g_csv)
        fprintf(stderr, "  (cpuprof_bin_for_us: bin checksum=%lld)\n", sink);
    return r;
}

/* Private histograms, not the profiler's: these rows are about
 * histogram_record() itself, and a private accumulator keeps them from
 * perturbing (or being perturbed by) whatever the profiler has been sampling.
 * One per section, because how many distinct histograms are in flight is the
 * single biggest term in this function's cost - see below. */
static Histogram g_bench_hist[PROF_SECTION_COUNT];

/* Two rows for one function, because histogram_record() costs two very
 * different things depending on how far apart consecutive samples into the
 * *same* histogram are, and only one of those is what the app pays.
 *
 * Welford's update is a loop-carried dependency: mean is loaded, fed through
 * fsub -> fdiv -> fadd, and stored, and the next sample into that histogram
 * cannot start until it lands. Back-to-back on one histogram that chain is
 * ~26 cycles and it stalls; measured on an M2, it is long enough to hide the
 * entire bin path (log10, scale, bin store) underneath it - deleting the bin
 * division outright changes the row by nothing at all.
 *
 * The app never sees that. A frame walks PROF_SECTION_COUNT *different*
 * sections, and the next sample into any one of them is a frame away, so every
 * chain has long since resolved. Rotating across the sections reproduces that
 * and is the row to read for per-frame cost; the chained row is the worst case,
 * kept because it is the one that moves if the statistics update ever grows a
 * longer serial dependency. */
static BenchResult bench_cpuprof_record_rotating(int iters) {
    BenchResult r = { .name = "cpuprof_record", .unit = "samples",
                      .min_sec = 1e18 };
    for (int it = 0; it < iters; it++) {
        for (int s = 0; s < PROF_SECTION_COUNT; s++)
            histogram_clear(&g_bench_hist[s]);
        double t0 = now_seconds();
        for (int k = 0; k < PROF_BENCH_INNER; k++)
            histogram_record(&g_bench_hist[k % PROF_SECTION_COUNT],
                             k_prof_sample_us[k % k_prof_sample_n]);
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += PROF_BENCH_INNER;
        r.iters++;
    }
    return r;
}

static BenchResult bench_cpuprof_record_chained(int iters) {
    BenchResult r = { .name = "cpuprof_record_chained", .unit = "samples",
                      .min_sec = 1e18 };
    for (int it = 0; it < iters; it++) {
        /* Bins saturate at UINT32_MAX while the statistics keep accumulating,
         * so a long enough run would quietly start timing a cheaper path.
         * Clearing per iteration keeps every iteration the same workload. */
        histogram_clear(&g_bench_hist[0]);
        double t0 = now_seconds();
        for (int k = 0; k < PROF_BENCH_INNER; k++)
            histogram_record(&g_bench_hist[0],
                             k_prof_sample_us[k % k_prof_sample_n]);
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += PROF_BENCH_INNER;
        r.iters++;
    }
    return r;
}

static BenchResult bench_cpuprof_sample_live(int iters) {
    BenchResult r = { .name = "cpuprof_sample_live", .unit = "samples",
                      .min_sec = 1e18 };
    prof_test_reset();   /* real clock: no pinned value left over */
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < PROF_BENCH_INNER; k++) {
            prof_begin(PROF_BENCH_SECTION);
            prof_end(PROF_BENCH_SECTION);
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += PROF_BENCH_INNER;
        r.iters++;
    }
    /* An empty body samples near 0 us, which the bin lookup short-circuits on
     * (`!(us > MIN_US)` returns bin 0 without the log10) - so this row is the
     * clock read plus the *cheapest* bin path, and undercounts a real section
     * by the log10 that cpuprof_bin_for_us prices separately. Report what the
     * samples actually looked like so that reading is checkable. */
    if (!g_csv) {
        HistogramStats st;
        if (prof_section_stats(PROF_BENCH_SECTION, &st))
            fprintf(stderr,
                    "  (cpuprof_sample_live: n=%llu, min=%.3f us, "
                    "mean=%.3f us, max=%.3f us)\n",
                    st.count, st.min_us, st.mean_us, st.max_us);
    }
    prof_test_reset();
    return r;
}

/* Run one pinned-clock batch. Pinning makes prof_now_us() a cheap, identical
 * load in both cases, so the difference is not buried under two platform clock
 * reads per sample. The parent stays open across the timed child loop in the
 * guarded case, exercising the overwhelmingly common non-violation branch. */
static double bench_cpuprof_nesting_iteration(int guard_installed) {
    double t0;
    double dt;

    prof_test_reset();
    prof_test_set_now_us(1.0e9);
    if (guard_installed)
        prof_install_section_depth_fn(bench_cpuprof_nesting_depth);

    prof_begin(PROF_BENCH_NEST_PARENT);
    t0 = now_seconds();
    for (int k = 0; k < PROF_BENCH_INNER; k++) {
        ProfSection child = (ProfSection)(
            (int)PROF_BENCH_NEST_PARENT + 1 +
            k % PROF_BENCH_NEST_CHILD_COUNT);
        prof_begin(child);
        prof_end(child);
    }
    dt = now_seconds() - t0;
    prof_end(PROF_BENCH_NEST_PARENT);

    if (guard_installed && prof_nesting_violations() != 0) {
        fprintf(stderr,
                "ERROR: cpuprof nesting benchmark misconfigured: %d "
                "violation(s)\n",
                prof_nesting_violations());
        g_case_missing = 1;
    }
    prof_test_reset();
    return dt;
}

/* Alternate which case runs first so thermal drift and frequency changes do
 * not systematically favor the inert or guarded row. Each timed region still
 * contains PROF_BENCH_INNER samples, keeping the per-span delta measurable even
 * when it is only a few nanoseconds. */
static void bench_cpuprof_nesting_pair(int iters,
                                      BenchResult *inert,
                                      BenchResult *guarded) {
    *inert = (BenchResult){ .name = "cpuprof_nest_inert", .unit = "samples",
                            .min_sec = 1e18 };
    *guarded = (BenchResult){ .name = "cpuprof_nest_guarded",
                              .unit = "samples", .min_sec = 1e18 };

    for (int it = 0; it < iters; it++) {
        double inert_dt;
        double guarded_dt;

        if ((it & 1) == 0) {
            inert_dt = bench_cpuprof_nesting_iteration(0);
            guarded_dt = bench_cpuprof_nesting_iteration(1);
        } else {
            guarded_dt = bench_cpuprof_nesting_iteration(1);
            inert_dt = bench_cpuprof_nesting_iteration(0);
        }

        inert->total_sec += inert_dt;
        inert->ops += PROF_BENCH_INNER;
        inert->iters++;
        if (inert_dt < inert->min_sec) inert->min_sec = inert_dt;

        guarded->total_sec += guarded_dt;
        guarded->ops += PROF_BENCH_INNER;
        guarded->iters++;
        if (guarded_dt < guarded->min_sec) guarded->min_sec = guarded_dt;
    }
}

static BenchResult bench_cpuprof_frame_tick(int iters) {
    BenchResult r = { .name = "cpuprof_frame_tick", .unit = "ticks",
                      .min_sec = 1e18 };
    /* Pinned and advancing a frame at a time: on the real clock these ticks
     * would land microseconds apart, so the FPS buckets would never close and
     * the frame-time histogram would see one bin. */
    double now = 1.0e9;
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < PROF_BENCH_INNER; k++) {
            prof_test_set_now_us(now);
            prof_frame_tick();
            now += 16666.0;
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += PROF_BENCH_INNER;
        r.iters++;
    }
    prof_test_reset();
    return r;
}

static BenchResult bench_cpuprof_stats_read(int iters) {
    BenchResult r = { .name = "cpuprof_stats_read", .unit = "reads",
                      .min_sec = 1e18 };
    double sink = 0.0;
    HistogramStats st;

    /* Read a populated histogram: the getter is branch-per-count, so an
     * all-zero one is not the case the hover box hits. */
    prof_test_reset();
    prof_test_set_now_us(1.0e9);
    prof_begin(PROF_BENCH_SECTION);
    prof_test_set_now_us(1.0e9 + 5000.0);
    prof_end(PROF_BENCH_SECTION);
    prof_test_clear_now_us();

    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < PROF_BENCH_INNER; k++) {
            if (prof_section_stats(PROF_BENCH_SECTION, &st))
                sink += st.stddev_us + st.mean_us;
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += PROF_BENCH_INNER;
        r.iters++;
    }
    if (!g_csv)
        fprintf(stderr, "  (cpuprof_stats_read: checksum=%.3f)\n", sink);
    prof_test_reset();
    return r;
}

/* Fastest observed per-op cost, in nanoseconds. The minimum rather than the
 * mean, for the reason spelled out at corpus_case_cmp_slower_first(): benchmark
 * noise is one-sided, and at tens of nanoseconds per op a single descheduled
 * iteration moves the mean by more than the whole quantity being measured. */
static double prof_bench_min_ns(BenchResult r) {
    return (r.iters > 0) ? r.min_sec * 1e9 / (double)PROF_BENCH_INNER : 0.0;
}

static void bench_cpuprof_sample(int iters) {
    BenchResult bins, record, live, nest_inert, nest_guarded, tick;

    bins   = bench_cpuprof_bin_for_us(iters);        report(bins);
    record = bench_cpuprof_record_rotating(iters);   report(record);
    report(bench_cpuprof_record_chained(iters));
    live   = bench_cpuprof_sample_live(iters);       report(live);
    bench_cpuprof_nesting_pair(iters, &nest_inert, &nest_guarded);
    report(nest_inert);
    report(nest_guarded);
    tick   = bench_cpuprof_frame_tick(iters);        report(tick);
    report(bench_cpuprof_stats_read(iters));

    /* The two numbers these rows exist to produce, in ns because a per-op
     * column in us reads as a wall of zeroes at this scale. Human mode only -
     * CSV keeps its fixed columns, and every term below is derivable from
     * them. */
    if (!g_csv) {
        double bins_ns   = prof_bench_min_ns(bins);
        double record_ns = prof_bench_min_ns(record);
        double live_ns   = prof_bench_min_ns(live);
        double nest_inert_ns = prof_bench_min_ns(nest_inert);
        double nest_guarded_ns = prof_bench_min_ns(nest_guarded);
        double nest_delta_ns = nest_guarded_ns - nest_inert_ns;
        double stats_ns  = record_ns - bins_ns;
        double frame_us  = (live_ns * (double)PROF_SECTION_COUNT
                            + prof_bench_min_ns(tick)) / 1000.0;

        printf("  # cpuprof: rotating record %.1f ns/sample = %.1f bins + "
               "%.1f stats; live begin/end pair %.1f ns\n",
               record_ns, bins_ns, stats_ns, live_ns);
        printf("  # cpuprof: %d sections + 1 tick = %.2f us/frame "
               "(%.3f%% of 16.67 ms), of which stats %.2f us\n",
               (int)PROF_SECTION_COUNT, frame_us,
               frame_us / 16666.0 * 100.0,
               stats_ns * (double)PROF_SECTION_COUNT / 1000.0);
        printf("  # cpuprof: installed-tree nesting lookup %+.1f ns/sample "
               "(guarded %.1f - inert %.1f)\n",
               nest_delta_ns, nest_guarded_ns, nest_inert_ns);
    }
}

/* ---- main -------------------------------------------------------------- */

static void print_csv_header(void) {
    printf("name,unit,iters,ops,total_sec,min_iter_ms,per_iter_ms,"
           "per_op_us,ops_per_sec\n");
}

/* Identify the host so a captured baseline CSV records where its numbers
 * came from (timings are only comparable on the same machine). In CSV mode
 * the line is `#`-prefixed so it precedes the header as a skippable comment;
 * in human mode it is a plain banner line.
 *
 * uname() is useless for this under Emscripten: it answers
 * "emscripten Emscripten <ver> wasm32" on every machine, so every web
 * baseline would claim the same host. Ask the JS runtime instead, and tag
 * the line `wasm/` so a web row can never be mistaken for a native one. */
static void print_machine_info(void) {
    const char *prefix = g_csv ? "# " : "";
#if defined(__EMSCRIPTEN__)
    const char *host = emscripten_run_script_string(
        "(function(){"
        "try{var os=require('os');"
        "return os.hostname()+' '+process.platform+' '+os.release()+' '+"
        "os.arch()+' node-'+process.versions.node+' v8-'+process.versions.v8;}"
        "catch(e){"
        "return (typeof navigator!=='undefined'&&navigator.userAgent)||"
        "'unknown-js-host';}})()");
    printf("%smachine: wasm/%s\n", prefix, host ? host : "unknown");
#else
    struct utsname u;
    if (uname(&u) == 0) {
        printf("%smachine: %s %s %s %s\n", prefix,
               u.nodename, u.sysname, u.release, u.machine);
    } else {
        printf("%smachine: unknown\n", prefix);
    }
#endif
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [--iters N] [--csv] [--only NAME[,NAME...]]\n"
        "  Available sub-benchmarks:\n"
        "    parse_lines       repl_parse_command on every example line\n"
        "    feed_examples     full editor_feed_line path on every example\n"
        "    flatten_examples  repl_flatten_commands per example\n"
        "    flatten_grass     full flatten of \"Swaying grass field (rand + t)\"\n"
        "                      (warm compiled row + a _cold row that rebuilds the\n"
        "                      expression cache inside the timer)\n"
        "    flatten_orrery    full flatten of \"Orrery (labels track 3D orbits)\"\n"
        "                      (same warm + _cold pair)\n"
        "    flatten_corpus    full flatten of every frozen benchmark scene\n"
        "    flatten_phases    reparse / assign / remainder split of a full flatten\n"
        "    flatten_refresh   production t refresh of Grass/Orrery/Wave (route shown)\n"
        "    refresh_slider    production refresh for one variable-panel drag step\n"
        "                      (route shown; the structural-dep trip-wire)\n"
        "    flatten_whale     dynamic-topology full flatten across a t grid\n"
        "    slider_drag       100-motion variable-panel drag + release persist\n"
        "    flat_cost_query   repl_flatten_cost_at_line over every cursor line\n"
        "    source_scope_query  source-scope depth queries over a deep document (warm cache)\n"
        "    source_scope_churn  source-scope cache rebuild per op (invalidate + query)\n"
        "    normalize_large_doc parse+normalize a line against a large live document\n"
        "    reformat_large_doc  reformat a large live document end-to-end\n"
        "    replay_examples   step replay through every example\n"
        "    replay_long       synthetic 600-iter for-loop replay\n"
        "    replay_focus      per-frame replay_focus_flat_idx() at the tail\n"
        "    replay_anchor     per-frame replay_focus_anchor_flat_idx() at the tail\n"
        "    cpuprof_sample    profiler instrumentation cost: bin lookup, full\n"
        "                      histogram record (rotating across sections, and\n"
        "                      dependency-chained on one), live begin/end pair,\n"
        "                      nesting-guard A/B, frame tick, stats readback\n"
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
 * On macOS, preflight CoreGraphics and skip this GL-only case when no
 * active display exists; Cocoa freeglut otherwise traps while querying
 * its empty screen list. Other window backends retain their native
 * no-display error, which is the cue to re-run with USE_GL_STUBS=1.
 *
 * Under Emscripten (`make bench-web`) there is no context to make current:
 * the run is headless under node, and Emscripten's JS GLUT reaches straight
 * for `document` inside glutInit, which throws. Skip before that happens.
 * Getting a real number here needs the wasm build driven in a browser
 * against a live WebGL2 canvas, which is a different harness. */
static int bench_gl_context_init(int *argc, char **argv) {
#if defined(__EMSCRIPTEN__)
    (void)argc; (void)argv;
    fprintf(stderr,
            "bench_repl: no GL context under node; skipping fade_batches\n");
    return 0;
#elif !defined(GL_STUBS)
    static int glut_inited = 0;
    if (glut_inited) return 1;
#ifdef __APPLE__
    {
        uint32_t display_count = 0;
        if (CGGetActiveDisplayList(0, NULL, &display_count) != kCGErrorSuccess ||
            display_count == 0) {
            fprintf(stderr,
                    "bench_repl: no active macOS display; skipping "
                    "fade_batches\n");
            return 0;
        }
    }
#endif
    glutInit(argc, argv);
    glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH | GLUT_STENCIL);
    glutInitWindowSize(1, 1);
    glutCreateWindow("bench_repl");
    glut_inited = 1;
    return 1;
#else
    (void)argc; (void)argv;
    return 1;
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
               iters, bench_example_count(), total_example_lines());
    }

    if (wants(only, "parse_lines"))
        report(bench_parse_lines(iters));
    if (wants(only, "feed_examples"))
        report(bench_feed_examples(iters));
    if (wants(only, "flatten_examples"))
        report(bench_flatten_examples(iters));
    if (wants(only, "flatten_grass"))
        run_named_flatten("flatten_grass", "Swaying grass field (rand + t)",
                          iters);
    if (wants(only, "flatten_orrery"))
        run_named_flatten("flatten_orrery", "Orrery (labels track 3D orbits)",
                          iters);
    if (wants(only, "flatten_corpus"))
        bench_flatten_corpus(iters);
    if (wants(only, "flatten_phases"))
        bench_flatten_phases(iters);
    if (wants(only, "flatten_refresh"))
        bench_flatten_refresh(iters);
    if (wants(only, "refresh_slider"))
        bench_refresh_slider(iters);
    if (wants(only, "flatten_whale"))
        bench_flatten_whale(iters);
    if (wants(only, "slider_drag"))
        bench_slider_drag(iters);
    if (wants(only, "flat_cost_query"))
        report(bench_flat_cost_query(iters));
    if (wants(only, "source_scope_query"))
        report(bench_source_scope_query(iters));
    if (wants(only, "source_scope_churn"))
        report(bench_source_scope_churn(iters));
    if (wants(only, "normalize_large_doc"))
        report(bench_normalize_large_doc(iters));
    if (wants(only, "reformat_large_doc"))
        report(bench_reformat_large_doc(iters));
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
    /* After the rows that read prof_section_last_us (flatten_phases): these
     * cases drive the timer directly and call prof_test_reset() when done. */
    if (wants(only, "cpuprof_sample"))
        bench_cpuprof_sample(iters);

    // GL benchmarks
    if (wants(only, "fade_batches")) {
        if (bench_gl_context_init(&argc, argv))
            report(bench_fade_batches(iters));
    }

    /* A named case that no longer resolves in the frozen corpus, or a Whale
     * scene that stopped varying its flat count with t, invalidates the
     * comparison this suite exists to support. Fail loudly. */
    return g_case_missing ? 1 : 0;
}
