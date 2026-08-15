/*
 * bench_extedit.c - The BYOE stage-2.5 gate: how long a watched content update
 * costs the main thread.
 *
 * docs/plans/active/BYOE.md makes stage 2.5 (the live WIP sidecar) the one
 * stage gated on a number rather than on design, and is specific about which
 * number: p95 **total content-update latency**, sampled **only on real content
 * updates**, across small / typical / large scenes, against a ~8 ms
 * main-thread budget. The reason for each of those qualifiers is a way the
 * obvious measurement lies:
 *
 *   - Reflatten alone is not the cost. It excludes the file read and hash, the
 *     atomic import, the undo-history capture, the D7 notifications - which
 *     together are most of the work. So this times the whole extedit section
 *     (`glr_extedit_poll`, the span gl_repl.c brackets with
 *     PROF_EXTERNAL_EDIT), and then again with the reflatten the reload forces
 *     onto the same frame. Two rows, because the budget is a frame budget: the
 *     `poll` row is what the section costs, the `frame` row is what the user
 *     waits for.
 *   - Sampling every frame buries the answer. In steady state the poll is one
 *     stat() and a compare, so a p95 over all frames measures the gate, not
 *     the update. Only frames that actually reload are recorded, and a case
 *     whose reload count does not match its sample count fails the run.
 *
 * Under stage 2.5 this cost is paid **per keystroke**, not per `:w`, which is
 * why the number matters at all. The default case publishes `<file>.wip`
 * atomically (temp + rename, trailing `// @cursor`) and asserts
 * `wip_updates` so the timed frames are the shipped content-update path:
 * row-map init, hole substitution, quiet import, one session snapshot,
 * not the saved-file proxy that stage 1 already ran. `--saved` keeps that
 * proxy for comparison.
 *
 * Scene sizes come from the shipped catalog rather than from synthetic
 * documents: "typical" has to mean something, and the catalog is the only
 * corpus that can say what.
 *
 * Not in `make bench`'s comparison set the way bench_repl's rows are. This is
 * a one-question harness whose answer is recorded in the plan; it stays in the
 * tree so the number can be re-taken when the machinery changes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gl_includes.h"

#if !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "app/glr_ctrl.h"
#include "app/glr_extedit.h"
#include "app/glr_modal.h"
#include "config.h"
#include "editor/state.h"
#include "repl/bootstrap.h"
#include "repl/example_loader.h"
#include "repl/examples.h"
#include "repl/export.h"
#include "repl/pipeline.h"
#include "repl/scenes.h"
#include "repl/state_owners.h"
#include "repl/state_views.h"

#define BENCH_SCENE_PATH "/tmp/gl_repl_bench_extedit.glr"
#define BENCH_WIP_PATH   BENCH_SCENE_PATH ".wip"
#define BENCH_WIP_TMP    BENCH_WIP_PATH ".tmp"
#define BENCH_MAX_LINES  (MAX_EDITOR_COMMANDS + 64)
#define BENCH_MAX_SAMPLES 4096

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int g_csv = 0;
/* Default is the shipped Stage 2.5 sidecar. `--saved` times the `:w` proxy
 * instead, which skips row-map init / hole substitution / quiet import. */
static int g_saved = 0;
/* A case that cannot be measured invalidates the answer, so it fails the run
 * rather than printing a row nobody can tell apart from a fast one. */
static int g_broken = 0;

/* ---- samples ------------------------------------------------------------ */

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile on the sorted samples. With the sample counts here
 * (hundreds) an interpolating definition would move p95 by less than the
 * run-to-run spread, and nearest-rank has the property that matters for a
 * budget check: the value it reports is one that actually happened. */
static double percentile(double *sorted, int n, double frac) {
    int rank;
    if (n <= 0)
        return 0.0;
    rank = (int)(frac * (double)n + 0.999999);
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
}

typedef struct {
    double v[BENCH_MAX_SAMPLES];
    int    n;
} BenchSamples;

static void samples_add(BenchSamples *s, double sec) {
    if (s->n < BENCH_MAX_SAMPLES)
        s->v[s->n++] = sec;
}

static void report_row(const char *size, const char *scene, const char *phase,
                       int doc_rows, BenchSamples *s) {
    double p50, p95, worst, mean = 0.0;
    int i;

    qsort(s->v, (size_t)s->n, sizeof(s->v[0]), cmp_double);
    for (i = 0; i < s->n; i++)
        mean += s->v[i];
    mean = (s->n > 0) ? mean / (double)s->n : 0.0;
    p50   = percentile(s->v, s->n, 0.50);
    p95   = percentile(s->v, s->n, 0.95);
    worst = percentile(s->v, s->n, 1.00);

    if (g_csv) {
        printf("%s,\"%s\",%s,%d,%d,%.4f,%.4f,%.4f,%.4f\n", size, scene, phase,
               doc_rows, s->n, mean * 1000.0, p50 * 1000.0, p95 * 1000.0,
               worst * 1000.0);
        return;
    }
    printf("  %-8s %-34s %-6s rows=%-4d n=%-4d  p50=%7.3f  p95=%7.3f  "
           "max=%7.3f  mean=%7.3f  ms\n",
           size, scene, phase, doc_rows, s->n, p50 * 1000.0, p95 * 1000.0,
           worst * 1000.0, mean * 1000.0);
}

/* ---- the file under the editor ------------------------------------------ */

/* The exported scene, held in memory so a mutation is a rewrite rather than a
 * re-export - re-exporting per iteration would time the writer too. */
typedef struct {
    char *line[BENCH_MAX_LINES];
    int   count;
} BenchFile;

static void bench_file_free(BenchFile *bf) {
    int i;
    for (i = 0; i < bf->count; i++)
        free(bf->line[i]);
    bf->count = 0;
}

static int bench_file_read(BenchFile *bf, const char *path) {
    char buf[MAX_LINE_LEN * 2];
    FILE *f = fopen(path, "r");

    bf->count = 0;
    if (!f)
        return 0;
    while (bf->count < BENCH_MAX_LINES && fgets(buf, (int)sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        bf->line[bf->count] = (char *)malloc(len + 1);
        if (!bf->line[bf->count]) {
            fclose(f);
            bench_file_free(bf);
            return 0;
        }
        memcpy(bf->line[bf->count], buf, len + 1);
        bf->count++;
    }
    fclose(f);
    return bf->count > 0;
}

/* Rewrite the whole file with one line replaced, which is what an editor's
 * save looks like from the watcher's side: new mtime, new content hash, and
 * every byte re-read. `mutate_idx` names a row the mutation is known to leave
 * parseable; `seq` keeps every save distinct so no two iterations can collide
 * on content and skip the reload. */
static int bench_file_write(const BenchFile *bf, const char *path,
                            int mutate_idx, int seq) {
    FILE *f = fopen(path, "w");
    int i;

    if (!f)
        return 0;
    for (i = 0; i < bf->count; i++) {
        if (i == mutate_idx)
            fprintf(f, "// bench %06d\n", seq);
        else
            fprintf(f, "%s\n", bf->line[i]);
    }
    if (fclose(f) != 0)
        return 0;
    return 1;
}

/* The shipped sidecar: the same mutated payload, then `// @cursor`, written
 * to a sibling temp and renamed over `<file>.wip` so the watcher never
 * observes a truncate. */
static int bench_wip_write(const BenchFile *bf, int mutate_idx, int seq) {
    FILE *f = fopen(BENCH_WIP_TMP, "w");
    int i;

    if (!f)
        return 0;
    for (i = 0; i < bf->count; i++) {
        if (i == mutate_idx)
            fprintf(f, "// bench %06d\n", seq);
        else
            fprintf(f, "%s\n", bf->line[i]);
    }
    fprintf(f, "// @cursor 1 1\n");
    if (fclose(f) != 0) {
        (void)unlink(BENCH_WIP_TMP);
        return 0;
    }
    if (rename(BENCH_WIP_TMP, BENCH_WIP_PATH) != 0) {
        (void)unlink(BENCH_WIP_TMP);
        return 0;
    }
    return 1;
}

/* Which row can be overwritten with a comment without changing whether the
 * file parses? A row that already is a comment, and not one of the export's
 * own `@` directives - those carry scene metadata the reader consumes.
 *
 * Every exported scene has one: write_shape_helpers and the section banners
 * put plain comments in the body. Returning -1 (no such row) fails the case
 * rather than falling back to mutating code, which could turn a timing run
 * into a parse-failure run without saying so. */
static int bench_pick_mutable_row(const BenchFile *bf) {
    int i;
    for (i = bf->count - 1; i >= 0; i--) {
        const char *p = bf->line[i];
        while (*p == ' ' || *p == '\t')
            p++;
        if (p[0] != '/' || p[1] != '/')
            continue;
        p += 2;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '@')
            continue;         /* an export directive, not free text */
        return i;
    }
    return -1;
}

/* ---- one case ----------------------------------------------------------- */

/* Bring the flat program current exactly as glr_ctrl_display_frame() does
 * after the poll. This is the other half of a content update: the reload marks
 * the flat program dirty and the same frame pays for the rebuild. */
static void settle_frame_work(void) {
    if (repl_state_normals_dirty()) {
        int edit_line = editor_state_edit_line();
        repl_recompute_autonormals(glr_state_presentation().autonormal,
                                   &edit_line);
        editor_state_edit_line_set(edit_line);
        repl_state_normals_dirty_clear();
    }
    (void)repl_refresh_flat_program(editor_state_edit_line());
}

static void run_case(const char *label, int example_idx, int iters) {
    BenchFile     bf;
    BenchSamples  poll_s, frame_s;
    GlrExtEditStats before, after;
    const char *scene = repl_example_name(example_idx);
    int mutate_idx, doc_rows, i;

    if (!scene)
        scene = "?";
    memset(&bf, 0, sizeof(bf));
    memset(&poll_s, 0, sizeof(poll_s));
    memset(&frame_s, 0, sizeof(frame_s));

    glr_extedit_set_enabled(0);
    glr_ctrl_reset_all();
    if (!repl_load_example(example_idx)) {
        fprintf(stderr, "ERROR: %s: example %d would not load\n", label,
                example_idx);
        g_broken = 1;
        return;
    }
    if (!repl_export_save_glr(BENCH_SCENE_PATH, source_document_view())) {
        fprintf(stderr, "ERROR: %s: could not export to %s\n", label,
                BENCH_SCENE_PATH);
        g_broken = 1;
        return;
    }
    if (!bench_file_read(&bf, BENCH_SCENE_PATH)) {
        fprintf(stderr, "ERROR: %s: could not read back %s\n", label,
                BENCH_SCENE_PATH);
        g_broken = 1;
        return;
    }
    mutate_idx = bench_pick_mutable_row(&bf);
    if (mutate_idx < 0) {
        fprintf(stderr, "ERROR: %s: no comment row to mutate\n", label);
        g_broken = 1;
        bench_file_free(&bf);
        return;
    }

    /* Adopt the exported file as the live scene, then arm. Binding stamps what
     * is on disk without reloading, so the first timed iteration is a genuine
     * external change and not a bind. A leftover sidecar at bind time is a
     * recovery offer, not a content update - delete it first. */
    (void)unlink(BENCH_WIP_PATH);
    (void)unlink(BENCH_WIP_TMP);
    glr_ctrl_reset_all();
    if (!repl_load_initial_commands(BENCH_SCENE_PATH)) {
        fprintf(stderr, "ERROR: %s: could not load %s as the live scene\n",
                label, BENCH_SCENE_PATH);
        g_broken = 1;
        bench_file_free(&bf);
        return;
    }
    doc_rows = repl_state_document_count();
    glr_extedit_set_enabled(1);
    glr_extedit_poll();
    glr_modal_cancel();
    settle_frame_work();

    before = glr_extedit_stats();
    for (i = 0; i < iters; i++) {
        double t0, t1, t2;
        int wrote = g_saved
            ? bench_file_write(&bf, BENCH_SCENE_PATH, mutate_idx, i + 1)
            : bench_wip_write(&bf, mutate_idx, i + 1);

        if (!wrote) {
            fprintf(stderr, "ERROR: %s: write failed at iteration %d\n", label,
                    i);
            g_broken = 1;
            break;
        }
        t0 = now_seconds();
        glr_extedit_poll();
        t1 = now_seconds();
        settle_frame_work();
        t2 = now_seconds();
        samples_add(&poll_s, t1 - t0);
        samples_add(&frame_s, t2 - t0);
    }
    after = glr_extedit_stats();

    /* Every iteration must have been a content update. A silently-deferred or
     * silently-failed publication would otherwise be timed as a fast frame
     * and pull p95 down - the exact way this measurement could flatter
     * itself. The WIP path asserts wip_updates; the saved-file proxy
     * asserts reloads. */
    if (g_saved) {
        if (after.reloads - before.reloads != poll_s.n) {
            fprintf(stderr,
                    "ERROR: %s: %d samples but %d reloads (%d failures) - "
                    "the timed frames were not all content updates\n",
                    label, poll_s.n, after.reloads - before.reloads,
                    after.failures - before.failures);
            g_broken = 1;
        }
    } else if (after.wip_updates - before.wip_updates != poll_s.n) {
        fprintf(stderr,
                "ERROR: %s: %d samples but %d wip_updates (%d failures, "
                "%d reloads) - the timed frames were not all sidecar "
                "content updates\n",
                label, poll_s.n, after.wip_updates - before.wip_updates,
                after.failures - before.failures,
                after.reloads - before.reloads);
        g_broken = 1;
    }

    report_row(label, scene, "poll", doc_rows, &poll_s);
    report_row(label, scene, "frame", doc_rows, &frame_s);

    glr_extedit_set_enabled(0);
    bench_file_free(&bf);
}

/* ---- case selection ----------------------------------------------------- */

static int example_line_count(int idx) {
    const char *const *lines = repl_example_lines(idx);
    int n = 0;
    if (!lines)
        return 0;
    while (lines[n])
        n++;
    return n;
}

/* Smallest, median and largest of the shipped catalog, by authored line count.
 * Picked by size rather than by name so the three cases keep meaning
 * "small / typical / large" as the catalog grows, instead of pinning three
 * scenes that were representative once. */
static void pick_cases(int *small_idx, int *typical_idx, int *large_idx) {
    int n = repl_example_count();
    int i, best_small = -1, best_large = -1;
    int *order, *len;

    *small_idx = *typical_idx = *large_idx = -1;
    if (n <= 0)
        return;
    order = (int *)malloc((size_t)n * sizeof(*order));
    len   = (int *)malloc((size_t)n * sizeof(*len));
    if (!order || !len) {
        free(order);
        free(len);
        return;
    }
    for (i = 0; i < n; i++) {
        order[i] = i;
        len[i]   = example_line_count(i);
        if (best_small < 0 || len[i] < len[best_small]) best_small = i;
        if (best_large < 0 || len[i] > len[best_large]) best_large = i;
    }
    /* Insertion sort by length: n is the catalog size, and a qsort comparator
     * would need the lengths in a file static to reach them. */
    for (i = 1; i < n; i++) {
        int key = order[i], j = i - 1;
        while (j >= 0 && len[order[j]] > len[key]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
    *small_idx   = best_small;
    *typical_idx = order[n / 2];
    *large_idx   = best_large;
    free(order);
    free(len);
}

int main(int argc, char **argv) {
    int iters = 200;
    int small_idx, typical_idx, large_idx;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--csv") == 0) {
            g_csv = 1;
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = atoi(argv[++i]);
            if (iters < 1)
                iters = 1;
            if (iters > BENCH_MAX_SAMPLES)
                iters = BENCH_MAX_SAMPLES;
        } else if (strcmp(argv[i], "--saved") == 0) {
            g_saved = 1;
        } else {
            fprintf(stderr, "usage: %s [--csv] [--iters N] [--saved]\n",
                    argv[0]);
            return 2;
        }
    }

    pick_cases(&small_idx, &typical_idx, &large_idx);
    if (small_idx < 0) {
        fprintf(stderr, "ERROR: empty example catalog\n");
        return 1;
    }

    if (g_csv)
        printf("size,scene,phase,doc_rows,samples,mean_ms,p50_ms,p95_ms,max_ms\n");
    else
        printf("=== external-editor content-update latency "
               "(BYOE stage-2.5 gate, %s path, budget 8.000 ms) ===\n",
               g_saved ? "saved-file" : "WIP sidecar");

    run_case("small", small_idx, iters);
    run_case("typical", typical_idx, iters);
    run_case("large", large_idx, iters);

    (void)unlink(BENCH_SCENE_PATH);
    (void)unlink(BENCH_WIP_PATH);
    (void)unlink(BENCH_WIP_TMP);
    return g_broken ? 1 : 0;
}

#else  /* __EMSCRIPTEN__ */

/* glr_extedit_poll() compiles to `return;` on the web - no external editor, no
 * filesystem worth watching - so there is no latency to measure. The binary
 * still builds and links in the wasm lane, the same way test_glr_extedit stays
 * in it rather than joining WEB_TEST_EXCLUDE: linking is the claim being
 * checked, and a skip line says so out loud instead of printing a zero.
 */
int main(void) {
    printf("=== external-editor content-update latency ===\n");
    printf("  skipped: the web build watches nothing\n");
    return 0;
}

#endif
