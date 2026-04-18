/*
 * bench_repl.c — Runtime benchmarks for the REPL parse / flatten / replay
 * pipeline.
 *
 * The benchmark binary links against the same CORE_TEST_OBJS the unit tests
 * use, so it works under both the normal GL-headers build and the GL-stubs
 * build (`make bench USE_GL_STUBS=1`). It is intentionally non-rendering:
 * it measures parsing, loading/flattening, and replay state-machine
 * advancement. It does NOT drive `execute_commands()` or
 * `execute_replay_fade_batches()` — those are the GL-emit paths and are
 * out of scope for a parser/replay benchmark.
 *
 * Sub-benchmarks (names match the `--only` filter strings and the printed
 * labels):
 *   parse_lines       — repl_parse_command on every example line
 *   feed_examples     — full feed_line path on every example
 *   flatten_examples  — load each example then call repl_flatten_commands
 *   replay_examples   — start a replay and step it to completion
 *   replay_long       — feed a synthetic large scene and step replay to end
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

#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_eval.h"
#include "repl_examples.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
           "min=%8.3f ms  per-iter=%8.3f ms  per-%s=%9.3f us  "
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

/* Mirror declare_test_vars() from the test suites — examples reference
 * these single-letter identifiers freely and parsing them otherwise fails
 * the validate_expression_idents() check. We declare them once at startup
 * and re-declare after each repl_reset_state() call (reset wipes the
 * predef table). */
static const char *const k_test_idents[] = {
    "x", "y", "z", "i", "j", "k", "a", "b", "c", "n",
};
static const int k_num_test_idents =
    (int)(sizeof(k_test_idents) / sizeof(k_test_idents[0]));

static void declare_test_idents(void) {
    char err[128];
    for (int i = 0; i < k_num_test_idents; i++)
        declare_predef_var(k_test_idents[i], err, sizeof(err));
}

static void fresh_repl(void) {
    repl_reset_state();
    declare_test_idents();
}

static int example_line_count(int idx) {
    const char *const *lines = repl_examples_lines(idx);
    int n = 0;
    if (!lines) return 0;
    while (lines[n]) n++;
    return n;
}

static long long total_example_lines(void) {
    long long total = 0;
    int n = repl_examples_count();
    for (int i = 0; i < n; i++)
        total += example_line_count(i);
    return total;
}

/* ---- bench: parse single lines via repl_parse_command ------------------ */

/* This bypasses the commit dispatch chain and exercises only the parser
 * path. It is meant to bracket how much of "feed_line cost" is parser vs.
 * everything else (normalization, var declaration, dirty marking, etc.). */
static BenchResult bench_parse_lines(int iters) {
    /* Build a flat array of lines from every example so we touch every
     * grammar branch (vertices, transforms, gluXxx, for, func, if, vars,
     * comments, blank lines). */
    int n_examples = repl_examples_count();
    long long total_lines = total_example_lines();
    const char **flat = (const char **)malloc(sizeof(*flat) * (size_t)total_lines);
    long long flat_n = 0;
    for (int e = 0; e < n_examples; e++) {
        const char *const *ls = repl_examples_lines(e);
        if (!ls) continue;
        for (int i = 0; ls[i]; i++)
            flat[flat_n++] = ls[i];
    }

    /* Make every identifier the examples use available to the parser so
     * the validator does not reject `x` etc. The parser also accepts
     * unknown idents but emits a status string we'd be timing. */
    fresh_repl();

    BenchResult r = { .name = "parse_lines", .unit = "lines",
                      .min_sec = 1e18 };
    GLCmd cmd;

    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (long long i = 0; i < flat_n; i++) {
            memset(&cmd, 0, sizeof(cmd));
            (void)repl_parse_command(flat[i], &cmd);
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

/* ---- bench: full feed_line path on every example ---------------------- */

static BenchResult bench_feed_examples(int iters) {
    int n_examples = repl_examples_count();
    long long lines_per_iter = total_example_lines();

    BenchResult r = { .name = "feed_examples", .unit = "lines",
                      .min_sec = 1e18 };

    /* load_example_lines() already resets g_cmds / g_num_flat_cmds and
     * calls init_predef_vars(), so an extra fresh_repl() before each
     * load would just bill duplicate reset work to this benchmark.
     * Examples declare their own float vars, so we don't need
     * declare_test_idents() here either. */
    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int e = 0; e < n_examples; e++) {
            repl_load_example_lines_for_test(repl_examples_lines(e));
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += lines_per_iter;
        r.iters++;
    }
    return r;
}

/* ---- bench: flatten cost on each example ------------------------------ */

static BenchResult bench_flatten_examples(int iters) {
    int n_examples = repl_examples_count();

    BenchResult r = { .name = "flatten_examples", .unit = "examples",
                      .min_sec = 1e18 };

    /* Pre-load each example fresh so we are timing flatten alone, not
     * feed_line plus flatten. We re-run flatten `inner` times per example
     * to amortize the surrounding loop overhead. The timer is started
     * after the load so the flatten loop is the only thing being timed. */
    int inner = 32;

    for (int it = 0; it < iters; it++) {
        double iter_sec = 0.0;
        for (int e = 0; e < n_examples; e++) {
            /* load_example_lines() resets cmd state itself; no separate
             * fresh_repl() is needed. */
            repl_load_example_lines_for_test(repl_examples_lines(e));

            double t0 = now_seconds();
            for (int k = 0; k < inner; k++) {
                /* mark_normals_dirty() flips g_flat_dirty so that the
                 * next call actually rebuilds; otherwise flatten is a
                 * no-op after the first invocation. */
                mark_normals_dirty();
                repl_flatten_commands();
            }
            iter_sec += now_seconds() - t0;
        }
        if (iter_sec < r.min_sec) r.min_sec = iter_sec;
        r.total_sec += iter_sec;
        r.ops += (long long)n_examples * inner;
        r.iters++;
    }
    return r;
}

/* ---- bench: replay every example end-to-end --------------------------- */

static BenchResult bench_replay_examples(int iters) {
    int n_examples = repl_examples_count();

    BenchResult r = { .name = "replay_examples", .unit = "steps",
                      .min_sec = 1e18 };

    /* load_example_lines() leaves g_flat_dirty=1, and replay_start() will
     * flatten once on its own — calling repl_flatten_commands() explicitly
     * beforehand would flatten twice, because repl_flatten_commands() does
     * NOT clear g_flat_dirty (see repl_core.c:4462-4464 vs. :3265-3269). */
    for (int it = 0; it < iters; it++) {
        long long steps = 0;
        double t0 = now_seconds();
        for (int e = 0; e < n_examples; e++) {
            repl_load_example_lines_for_test(repl_examples_lines(e));

            replay_start();
            int safety = g_num_flat_cmds + 1;
            while (g_replay_state == REPLAY_PLAYING && safety-- > 0) {
                replay_advance();
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

/* Build a single scene that flattens to a very large g_flat_cmds[] so we
 * get a longer-running replay test that is comparable across machines.
 *
 * We use one outer for-loop that emits a triangle per iteration. The
 * iteration count is capped at MAX_FLATTEN_VISITS (100k) inside
 * flatten_range, but g_flat_cmds[] itself is bounded by MAX_COMMANDS
 * (typically 4096), so in practice we get around (MAX_COMMANDS / per-iter
 * cmds) iterations expanded. The benchmark adapts: it reads
 * g_num_flat_cmds after flatten and reports the actual flat-cmd count,
 * stepping replay over all of them. */
static const char *const k_long_replay_scene[] = {
    "glClearColor(0.05, 0.05, 0.05, 1);",
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "float a;",
    "float b;",
    "for(i, 0, 600) {",
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

    /* Load once outside the inner loop — feed_line is not what we are
     * measuring here. Re-using the same g_cmds[] across iterations is
     * fine because replay only mutates the replay state, not the source
     * commands. We mark g_flat_dirty between iterations so replay_start()
     * does a fresh flatten each time — that matches "what happens the
     * first time you press play". Note: replay_start() handles the
     * flatten itself and clears g_flat_dirty, so calling
     * repl_flatten_commands() explicitly here would flatten twice. */
    repl_load_example_lines_for_test(k_long_replay_scene);
    mark_normals_dirty();
    repl_flatten_commands();
    int flat_cmds = g_num_flat_cmds;

    for (int it = 0; it < iters; it++) {
        long long steps = 0;

        mark_normals_dirty();
        double t0 = now_seconds();

        replay_start();
        int safety = g_num_flat_cmds + 1;
        while (g_replay_state == REPLAY_PLAYING && safety-- > 0) {
            replay_advance();
            steps++;
        }
        replay_stop();

        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += steps;
        r.iters++;
    }

    /* Diagnostic aside — useful for confirming the scene size, but gated
     * behind !g_csv so machine-parseable output stays clean on stderr too. */
    if (!g_csv) {
        fprintf(stderr, "  (replay_long scene flattened to %d flat cmds)\n",
                flat_cmds);
    }
    return r;
}

/* ---- main -------------------------------------------------------------- */

static void print_csv_header(void) {
    printf("name,unit,iters,ops,total_sec,min_iter_ms,per_iter_ms,"
           "per_op_us,ops_per_sec\n");
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [--iters N] [--csv] [--only NAME[,NAME...]]\n"
        "  Available sub-benchmarks:\n"
        "    parse_lines       repl_parse_command on every example line\n"
        "    feed_examples     full feed_line path on every example\n"
        "    flatten_examples  repl_flatten_commands per example\n"
        "    replay_examples   step replay through every example\n"
        "    replay_long       synthetic 600-iter for-loop replay\n",
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

int main(int argc, char **argv) {
    int iters = 5;
    const char *only = NULL;

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

    init_predef_vars();
    fresh_repl();

    if (g_csv) {
        print_csv_header();
    } else {
        printf("REPL benchmarks (iters=%d, examples=%d, total_lines=%lld)\n",
               iters, repl_examples_count(), total_example_lines());
    }

    if (wants(only, "parse_lines"))
        report(bench_parse_lines(iters));
    if (wants(only, "feed_examples"))
        report(bench_feed_examples(iters));
    if (wants(only, "flatten_examples"))
        report(bench_flatten_examples(iters));
    if (wants(only, "replay_examples"))
        report(bench_replay_examples(iters));
    if (wants(only, "replay_long"))
        report(bench_replay_long(iters));

    return 0;
}
