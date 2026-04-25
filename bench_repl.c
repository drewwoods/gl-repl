/*
 * bench_repl.c - Runtime benchmarks for the REPL parse / flatten / replay
 * pipeline.
 *
 * The benchmark binary links against the same CORE_TEST_OBJS the unit tests
 * use, so it works under both the normal GL-headers build and the GL-stubs
 * build (`make bench USE_GL_STUBS=1`).
 *
 * In the stubs build every gl* call is an inline no-op that only ticks a
 * per-function counter (see include/GL/gl_stub_counts.h), so timings
 * measure pure C-level cost. In the real-GL build we create a real GL
 * context up front (via GLUT) so sub-benchmarks that drive actual draw
 * calls - notably `fade_batches` via `render_replay_fade_pass()` -
 * have somewhere to emit to; without a current context those calls are
 * undefined behaviour rather than measurable work.
 *
 * Sub-benchmarks (names match the `--only` filter strings and the printed
 * labels):
 *   parse_lines       - repl_parse_command on every example line
 *   feed_examples     - full feed_line path on every example
 *   flatten_examples  - load each example then call repl_flatten_commands
 *   replay_examples   - start a replay and step it to completion
 *   replay_long       - feed a synthetic large scene and step replay to end
 *   fade_batches      - drive render_replay_fade_pass() with a packed
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

#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_eval.h"
#include "repl_examples.h"
#include "repl_parser.h"
#include "scene_render.h"
#include "repl_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef OPENGL_VIBE_USE_GL_STUBS
#include <GL/gl_stub_counts.h>
#else
/* Real-GL build: pull in GLUT so we can create an actual current
 * context before running sub-benchmarks that emit draw calls. The
 * stub headers deliberately do NOT get included here - the Makefile
 * picks between stub and system GL headers via -I ordering. */
#include <gl_includes.h>
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

    /* load_example_lines() already resets repl_state_document_cmds_mut() / repl_state_flat_program_count() and
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
    float saved_vals[MAX_PREDEF_VARS];

    for (int it = 0; it < iters; it++) {
        double iter_sec = 0.0;
        for (int e = 0; e < n_examples; e++) {
            /* load_example_lines() resets cmd state itself; no separate
             * fresh_repl() is needed. */
            repl_load_example_lines_for_test(repl_examples_lines(e));

            /* Snapshot post-load predef values. flatten_range() writes
             * g_predef_vars[].value on CMD_VAR_ASSIGN (repl_core.c:2624),
             * so without restoring each iter sees drifted values and
             * measures a different workload than the first - several
             * examples have self-referential-looking assignments whose
             * RHS depends on other predef vars. */
            int saved_n = g_num_predef_vars;
            for (int i = 0; i < saved_n; i++)
                saved_vals[i] = g_predef_vars[i].value;

            double t0 = now_seconds();
            for (int k = 0; k < inner; k++) {
                /* Restore the post-load values before each flatten so
                 * every inner iteration measures the same expression
                 * evaluation workload. The restore is a 16-float copy,
                 * dwarfed by flatten itself. */
                for (int i = 0; i < saved_n; i++)
                    g_predef_vars[i].value = saved_vals[i];
                /* repl_flatten_commands() -> flatten_commands() rebuilds
                 * unconditionally (resets repl_state_flat_program_count() and walks
                 * repl_state_document_cmds_mut()[]), so we don't need to toggle any dirty flag
                 * here - doing so would just add unrelated side effects
                 * (repl_state_normals_dirty(), depth cache invalidation) into the
                 * timed region. */
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

    /* load_example_lines() leaves repl_state_flat_program_dirty()=1, and replay_start() will
     * flatten once on its own - calling repl_flatten_commands() explicitly
     * beforehand would flatten twice, because repl_flatten_commands() does
     * NOT clear repl_state_flat_program_dirty() (see repl_core.c:4462-4464 vs. :3265-3269). */
    for (int it = 0; it < iters; it++) {
        long long steps = 0;
        double t0 = now_seconds();
        for (int e = 0; e < n_examples; e++) {
            repl_load_example_lines_for_test(repl_examples_lines(e));

            replay_start();
            int safety = repl_state_flat_program_count() + 1;
            const ReplReplayRuntimeState *replay = repl_state_replay();
            while (*replay->state == REPLAY_PLAYING && safety-- > 0) {
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

/* Build a single scene that flattens to a very large repl_state_flat_program_cmds_mut()[] so we
 * get a longer-running replay test that is comparable across machines.
 *
 * We use one outer for-loop that emits a triangle per iteration. The
 * iteration count is capped at MAX_FLATTEN_VISITS (100k) inside
 * flatten_range, but repl_state_flat_program_cmds_mut()[] itself is bounded by MAX_COMMANDS
 * (typically 4096), so in practice we get around (MAX_COMMANDS / per-iter
 * cmds) iterations expanded. The benchmark adapts: it reads
 * repl_state_flat_program_count() after flatten and reports the actual flat-cmd count,
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

    /* Load once outside the inner loop - feed_line is not what we are
     * measuring here. Re-using the same repl_state_document_cmds_mut()[] across iterations is
     * fine because replay only mutates the replay state, not the source
     * commands. We mark repl_state_flat_program_dirty() between iterations so replay_start()
     * does a fresh flatten each time - that matches "what happens the
     * first time you press play". Note: replay_start() handles the
     * flatten itself and clears repl_state_flat_program_dirty(), so calling
     * repl_flatten_commands() explicitly here would flatten twice. */
    repl_load_example_lines_for_test(k_long_replay_scene);

    /* Warm up via replay_start/replay_stop (not a bare
     * repl_flatten_commands) so the flatten's CMD_VAR_ASSIGN writes
     * to g_predef_vars don't leak into the timed iterations -
     * replay_start does its own predef snapshot/restore around the
     * flatten (repl_core.c:3264-3268). */
    mark_normals_dirty();
    replay_start();
    int flat_cmds = repl_state_flat_program_count();
    replay_stop();

    /* Snapshot post-load predef values. replay_advance() writes
     * g_predef_vars on CMD_VAR_ASSIGN during playback
     * (repl_core.c:3655-3656), so without restoring, each iteration
     * would start replay_start()'s baseline capture from progressively
     * drifted values. */
    float saved_vals[MAX_PREDEF_VARS];
    int saved_n = g_num_predef_vars;
    for (int i = 0; i < saved_n; i++)
        saved_vals[i] = g_predef_vars[i].value;

    for (int it = 0; it < iters; it++) {
        long long steps = 0;

        for (int i = 0; i < saved_n; i++)
            g_predef_vars[i].value = saved_vals[i];
        mark_normals_dirty();
        double t0 = now_seconds();

        replay_start();
        int safety = repl_state_flat_program_count() + 1;
        const ReplReplayRuntimeState *replay = repl_state_replay();
        while (*replay->state == REPLAY_PLAYING && safety-- > 0) {
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

    /* Diagnostic aside - useful for confirming the scene size, but gated
     * behind !g_csv so machine-parseable output stays clean on stderr too. */
    if (!g_csv) {
        fprintf(stderr, "  (replay_long scene flattened to %d flat cmds)\n",
                flat_cmds);
    }
    return r;
}

/* ---- bench: replay fade-batch rendering path -------------------------- */

/* Scene built to emit a long flat-command stream of many small primitives
 * so we can exercise the fade-batch rendering pass with large old_pc
 * indices. We unroll a for-loop that emits one triangle per iteration;
 * flatten caps us at MAX_COMMANDS flat cmds regardless of the iteration
 * count, which is what we want for this benchmark - we just need a large
 * repl_state_flat_program_count(). */
static const char *const k_fade_bench_scene[] = {
    "glEnable(GL_DEPTH_TEST);",
    "glEnable(GL_LIGHTING);",
    "float a;",
    "float b;",
    "for(i, 0, 600) {",
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

static BenchResult bench_fade_batches(int iters) {
    BenchResult r = { .name = "fade_batches", .unit = "calls",
                      .min_sec = 1e18 };

    /* Build the long scene and flatten once. The flatten pass runs
     * inside replay_start(); we piggy-back on that to also capture
     * repl_state_flat_program_count() (replay_start clamps repl_state_flat_program_count() during
     * playback, so we snapshot before/after). */
    repl_load_example_lines_for_test(k_fade_bench_scene);
    mark_normals_dirty();
    replay_start();
    int flat_cmds = repl_state_flat_program_count();
    replay_stop();

    /* Re-flatten after replay_stop so repl_state_flat_program_count() is the full stream
     * (replay's clamp might still be in effect otherwise - we observed
     * flat_cmds via the post-start snapshot above). */
    mark_normals_dirty();
    repl_flatten_commands();
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

#ifdef OPENGL_VIBE_USE_GL_STUBS
    /* Reset right before the timed region so the counter dump below
     * reflects only the work driven by render_replay_fade_pass()
     * across the full iters×inner call count. repl_bench_fade_install
     * doesn't invoke any GL stubs, so including it in the counted
     * region is fine. */
    gl_stub_counts_reset();
#endif

    for (int it = 0; it < iters; it++) {
        double t0 = now_seconds();
        for (int k = 0; k < inner; k++) {
            /* Reinstall on every call so a fade batch that ticks its
             * age past REPLAY_FADE_DURATION doesn't silently reduce
             * the measured workload (replay_tick_fade_batches isn't
             * called here, but keeping the install call in-loop also
             * amortizes a tiny fraction of the setup cost into each
             * measurement - intentional, since a real replay frame
             * also re-registers batches as new steps arrive). */
            repl_bench_fade_install(old_pcs, new_pcs, installed_count, age);
            render_replay_fade_pass();
        }
        double dt = now_seconds() - t0;
        if (dt < r.min_sec) r.min_sec = dt;
        r.total_sec += dt;
        r.ops += inner;
        r.iters++;
    }

    repl_bench_fade_clear();

    if (!g_csv) {
        fprintf(stderr, "  (fade_batches: flat_cmds=%d, batches=%d, age=%.3f)\n",
                flat_cmds, installed_count, age);
#ifdef OPENGL_VIBE_USE_GL_STUBS
        fprintf(stderr, "  (GL stub calls per render_replay_fade_pass():)\n");
        gl_stub_counts_dump(stderr, "    ", r.ops);
#endif
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
        "    replay_long       synthetic 600-iter for-loop replay\n"
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
#ifndef OPENGL_VIBE_USE_GL_STUBS
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

    // GL benchmarks
    if (wants(only, "fade_batches")) {
        bench_gl_context_init(&argc, argv);
        report(bench_fade_batches(iters));
    }

    return 0;
}
