# Historic Bench Trend

## Context

Right now `make bench` produces wall-time numbers for one commit. There is no easy way to ask "when did `flatten_examples` get slower?" or "did last week's refactor regress `replay_long`?" - answering either today requires manually checking out old SHAs, rebuilding, and eyeballing the diff.

This plan adds two pieces that together produce a historical view:

1. A **sampler** (C binary) that walks git history, runs `bench_repl` at chosen commits, and persists the CSV.
2. A **viewer** (C/GLUT binary) that plots that CSV with fixed-function OpenGL - matching the project's aesthetic and avoiding new toolchain deps.

Everything is C. No Python, no shell glue beyond the existing `scripts/build-historical.sh`.

Sampling is bounded by a user-specified minimum gap (commits *or* days, picked per-run); regions with a large relative delta between adjacent samples are then bisected down to that gap floor. The viewer toggles its X-axis between commit date and chronological SHA index.

The headless infrastructure already exists: `bench_repl` supports `--csv` and `USE_GL_STUBS=1`, and `scripts/build-historical.sh` already builds arbitrary SHAs in isolated worktrees with compat headers spliced in. This plan wires those together.

## Architecture

```
       git log (via popen)
          │
          ▼
  ┌──────────────────────┐    build     ┌────────────────────────────────────────┐
  │  bench_trend_sample  │ ───────────► │  scripts/build-historical.sh           │
  │  (C binary)          │              │     --at <sha> bench_repl USE_GL_STUBS=1│
  │                      │      ↓                                                │
  │                      │   ┌──────────────────────────────────────────────┐    │
  │                      │ ──┤  .compat-scratch/worktrees/<sha>/            │    │
  │                      │   │     build/bin/bench_repl --csv --iters N    │    │
  │                      │   └──────────────────────────────────────────────┘    │
  └──────────┬───────────┘                                                       │
             │   pure CSV on stdout                                              │
             ▼                                                                   │
  bench/trend_results.csv  ◄──reads──  bench_trend_view  (GLUT, fixed-function GL)
   (canonical store)
```

The CSV is the contract between the two binaries. The sampler is idempotent: it loads the existing CSV, decides which SHAs still need measuring, and only invokes the builder for those. Build and run are kept separate so the build script's diagnostic stdout never contaminates the CSV stream.

## Piece 1 - Sampler (`tools/bench_trend_sample/`)

Single binary, two source files for testability:

- `tools/bench_trend_sample/main.c` - `argv` parsing (`getopt_long`), git + build orchestration via `popen`, CSV file I/O, and the work loop. Calls into the pure helpers in `logic.c`.
- `tools/bench_trend_sample/logic.c` + `logic.h` - pure decision functions (initial-pick, work-set, pair-trips, bisection-step). Linked directly by the C unit test below, so they're exercised without spawning processes.

### CLI

```
./bench_trend_sample [options]
    --from REF              # default: earliest commit introducing the bench harness
                            # (searches both bench/bench_repl.c and the older root
                            # bench_repl.c - see "Default --from" below)
    --to REF                # default: HEAD
    --min-gap-commits N     # mutually exclusive with --min-gap-days
    --min-gap-days D
    --inflection-pct PCT    # default 10.0
    --iters N               # forwarded to bench (default 5)
    --max-bisect N          # cap on bisection sample count (default 64)
    --csv PATH              # default: bench/trend_results.csv
    --broken PATH           # default: bench/trend_broken.txt
    --retry-broken          # flush trend_broken.txt for this run
    --dry-run               # print what would be sampled, don't build
```

Exactly one of `--min-gap-commits` / `--min-gap-days` is required (matches the "user picks per-run" answer). Implemented with the standard `getopt_long` shape used elsewhere in the project.

**Default `--from`.** The bench harness was introduced earlier than its current path: commit `0764803` on 2026-05-01 moved it from a root-level `bench_repl.c` (added ~2026-04-18) into `bench/bench_repl.c`. A default that keys off `bench/bench_repl.c` alone silently skips ~two weeks of pre-move history. The sampler instead resolves the default by running:

```
git log --diff-filter=A --reverse --first-parent --format='%H %ct' \
    -- bench/bench_repl.c bench_repl.c
```

and taking the *earliest* of any returned commits as the default `<from>`. (`--diff-filter=A` matches the "add" of the file at its first appearance under either path.) If neither path matches (e.g. running against an unrelated repo), the sampler errors out with a hint to pass `--from` explicitly.

### CSV schema

`bench/trend_results.csv` - one row per (sha, benchmark):

```
sha,date_epoch,date_iso,bench_name,unit,iters,ops,total_sec,min_iter_ms,per_iter_ms,per_op_us,ops_per_sec
```

`sha` is the 12-char short hash.

**Two date columns.** `date_epoch` is the committer date as Unix epoch seconds (from `git log --format='%ct'`). `date_iso` is the committer date as ISO 8601 (`%cI`, e.g. `2026-04-13T10:19:03+02:00`).

Why both: ISO 8601 *strings* do **not** lex-sort to UTC chronological order when adjacent commits carry different timezone offsets - this repo already has first-parent commits where a `+02:00` author lands a few minutes before a `-04:00` author, and lex sort flips them. So the sampler always sorts and the viewer always plots on `date_epoch` (a monotonic integer). `date_iso` exists for `git diff` / `grep` / human inspection of the CSV - opaque epochs make the file useless to read by eye, and the cost of an extra column is one strftime call per row at write time.

The last 9 columns are the existing `bench_repl --csv` columns verbatim. `bench_repl` currently emits seven sub-benchmarks (`parse_lines`, `feed_examples`, `flatten_examples`, `spike_flatten_largest`, `replay_examples`, `replay_long`, `fade_batches` - see `bench/bench_repl.c:802-823`), so a SHA fully covered at this build contributes seven rows. Both sampler and viewer treat the bench-name set as data, not as a hard-coded constant, so adding or removing a `bench_repl` sub-benchmark needs no code change here.

The sampler rewrites the file sorted by (`date_epoch` ascending, `bench_name` ascending) after each successful run so diffs and viewer reads stay stable. A sibling `bench/trend_broken.txt` records broken SHAs as `<sha> <reason>` lines (e.g. `a1b2c3d4e5f6 csv unsupported`, `a1b2c3d4e5f6 build failed`, `a1b2c3d4e5f6 binary not found`).

### Build vs Run separation (correctness fix)

The sampler invokes `build-historical.sh --at <sha> bench_repl USE_GL_STUBS=1` strictly to *build* the historical bench binary; it discards that command's stdout (the build script prints `build-historical: ...` diagnostic lines there). Then it computes the worktree path the same way the script does - `.compat-scratch/worktrees/<short-12-sha>/` (the formula at `scripts/build-historical.sh:218-219`) - and `popen`s the binary directly with `--csv --iters N`. That stream is pure CSV and parses with `fgets` + a column scanner.

Why this matters:
- `make bench` is a *runner* target that prints its own `REPL benchmarks (iters=...)` header before the CSV body - using it would force CSV-shape filtering on every line. Going via `make bench-repl` plus direct exec gives us a clean stream.
- `--csv` is a recent addition to `bench_repl.c`. Old SHAs may lack it (`getopt` rejects, exit non-zero or empty stdout). The sampler detects this and writes `<sha> csv unsupported` to `trend_broken.txt` rather than spinning forever.
- **Binary location varies across SHAs.** The modern Makefile puts the output at `build/$(BUILD)$(if USE_GL_STUBS,-gl-stubs)/bench_repl` (Makefile:588-589 - `OBJDIR == BINDIR`, no `bin/` subdir). Older SHAs used `build/bench_repl` or root-level `bench_repl`. The sampler probes for the binary in this fallback order, taking the first that is executable:
   1. `<worktree>/build/release-gl-stubs/bench_repl`  *(modern default with USE_GL_STUBS=1)*
   2. `<worktree>/build/release/bench_repl`  *(modern default without stubs, in case future runs go that way)*
   3. `<worktree>/build/bin/bench_repl`  *(some historical layouts)*
   4. `<worktree>/build/bench_repl`  *(older layouts)*
   5. `<worktree>/bench_repl`  *(root-linked, oldest)*

   If none exists, log `<sha> binary not found` to `trend_broken.txt`. The probe runs each invocation - no caching - so a SHA-specific layout choice doesn't get baked in by accident.

### `fade_batches` under stubs (caveat)

`fade_batches` calls `bench_gl_context_init` (`bench/bench_repl.c:818`). Under `USE_GL_STUBS=1` the GL init is a no-op and every `glBegin`/`glVertex` becomes a counter-incrementing inline stub. So the sub-benchmark measures *CPU* work only, not GPU time. This is fine for tracking REPL-side fade scheduling regressions but is **not** comparable across machines with different GL drivers, and shouldn't be read as a graphics-performance signal. The plan accepts this trade-off because (a) it lets the sampler run on the headless `gracemont` box and (b) the regressions we actually care about (parser, flatten, executor) are CPU-bound anyway. The viewer's existing NaN/series-gap handling already covers a SHA where `fade_batches` is absent entirely.

### Historical-build caveats - required patches and skip rules

Older SHAs cannot be built with a plain `make bench-repl` checkout, for three independent reasons. The sampler must apply or detect each before invoking the historical build. **Always consult `git log -p bench/bench_repl.c` (and the pre-2026-05 root-level `bench_repl.c` for the older history segment) when extending the skip / patch policy - that history is the source of truth for which SHAs need which treatment.**

**1. `scripts/build-historical.sh` is mandatory, not optional.**

The mention at the top of this plan ("Everything is C. No Python, no shell glue beyond the existing `scripts/build-historical.sh`") understates the dependency - the sampler **cannot** invoke a bare `make bench_repl USE_GL_STUBS=1` against an old SHA. Before April 2026 the REPL lived inside OpenGL-Vibe, and the historical Makefile resolves `PROJECT_ROOT := $(abspath ../../..)` expecting the OpenGL-Vibe parent directory to supply `gl_includes.h` and `miniaudio.h`. After the hoist those paths point nowhere; any `make` invocation against a pre-hoist SHA dies immediately on the missing header. `scripts/build-historical.sh` exists to bridge this - it creates a private worktree under `.compat-scratch/worktrees/<short-sha>/` and splices in compat headers so the historical Makefile's path math resolves. The sampler must drive the build via `scripts/build-historical.sh --at <sha> bench_repl USE_GL_STUBS=1` for every SHA, modern and historical; the modern path is a degenerate case of the same script (the script no-ops the splice when the SHA already vendors the headers).

**2. Replay-long / fade_batches scene-size patch (MAX_COMMANDS = 4096).**

`bench_repl.c`'s `replay_long` and `fade_batches` sub-benches each loop 600 iterations emitting 11-12 flat commands per iter, totalling ~7200 flat commands. The project capped `MAX_COMMANDS` at 4096 in commit `804d794` ("config: reduce max commands"). At SHAs *after* `804d794` but *before* `61daa23` (the fix), the bench compiles cleanly but produces *silently wrong output*: `flatten_append_cmd` hits the cap, sets `ctx->abort = 1`, and `repl_flatten_program` zeros `flat_count` - the bench then reports `ops=0` / `flat_cmds=0` for those two sub-benches at every SHA in that range.

A zero-ops row is not a useful data point - it's noise that distorts the trend curve. The sampler must either:

  a. Detect the bug (post-process the CSV: if `replay_long.ops == 0` or `fade_batches.ops == 0`, drop those columns from the row before write and log `<sha> replay_long/fade_batches cap-truncated` to `trend_broken.txt`); or
  b. Apply the iter-count patch in the private worktree before the build (the fix in `61daa23` shrinks `replay_long` to 340 iters / `fade_batches` to 370 iters). The patch is small (~21+/-14 across one file) and applies cleanly to every pre-fix SHA that compiles at all.

Option (a) is simpler - no patch maintenance, no risk of merge skew across the SHA range - and is consistent with the existing `trend_broken.txt` skip convention. Option (b) gives complete trend coverage at the cost of carrying a patch file. The default recommendation is (a); the comment block at the patch-application site should reference commit `61daa23` so a future maintainer can adopt (b) if `replay_long` / `fade_batches` trend coverage becomes load-bearing.

**3. Bench-compile failures at certain SHAs (skip-only).**

Some historical SHAs fail to compile `bench_repl.c` outright - typically because the bench references a REPL symbol that was renamed or moved in a refactor that didn't update the bench in lockstep. These are not patchable without per-SHA case logic (the symbol shapes differ across each break window) and are not worth the maintenance burden.

The sampler should treat a non-zero exit from `scripts/build-historical.sh --at <sha> bench_repl USE_GL_STUBS=1` as "skip this SHA" and log `<sha> bench compile failure` to `trend_broken.txt`. The build script's stdout is captured to the same log line so the next maintainer can see whether it was a missing-symbol error, a header-path mismatch the compat splice didn't catch, or a genuine source bug. The existing CSV-unsupported and binary-not-found skip paths use the same shape; this is a third row in the same skip-reason taxonomy.

The bench file's `git log` (and the pre-hoist root-level `bench_repl.c` history) is the canonical reference for which SHAs need which treatment - including future skip-reason additions. The `trend_broken.txt` file is the operational record of which SHAs were skipped at sample time.

### Algorithm

1. **Enumerate (inclusive of `<from>`).** Spawn `git log --format='%H %cI' <to> --reverse --first-parent` and drop everything before `<from>` (inclusive). Equivalently, when `<from>` has a parent, `git log <from>^..<to>`; when it's the root commit, `git log <to>`. The default `<from>` (first commit touching `bench/bench_repl.c`) must itself be sampleable, so an exclusive `<from>..<to>` is wrong. First-parent keeps merge bubbles from inflating the sample set.
2. **Initial pick.** Walk the list chronologically. Always pick the first commit. Then pick the next commit whose gap from the last pick *meets or exceeds* the min-gap (commit count for `--min-gap-commits`, calendar days between committer dates for `--min-gap-days`). Always include the last commit even if it sits inside the gap, so the trend line reaches HEAD.
3. **Compute the work set.** Load existing rows from the CSV. A SHA is *covered* at the requested `--iters` when:
   - At least one row exists for that SHA, and
   - Every row's `iters` matches the requested value, and
   - The CSV's per-SHA bench-name set matches a recorded "complete run" sentinel for that SHA.

   The third condition addresses the bench-set-growth problem: the bench list changes over time (e.g. `spike_flatten_largest` was added recently), so older SHAs *legitimately* emit fewer rows than HEAD. A global "expected bench-name set" derived from the most recent run would force eternal re-measurement of those older SHAs. Instead, after each successful run the sampler writes a `bench/trend_runs.csv` companion file with one row per (sha, iters) holding the comma-separated bench-name list that SHA's `bench_repl` actually emitted plus a wall-clock timestamp of the run. Idempotency checks consult that companion: a SHA is covered iff its CSV rows match the recorded run set at the requested iters. A SHA with *no* companion entry (e.g. CSV manually pruned, or partial mid-crash) is re-measured. A SHA whose companion `iters` differs is re-measured. SHAs in `trend_broken.txt` are skipped unless `--retry-broken` flushes it.

   `trend_runs.csv` schema: `sha,iters,bench_names,run_started_iso,run_completed_iso`. Tiny file (one line per measured SHA). Gitignored alongside `trend_results.csv`.
4. **Sample.** For each work-set SHA: run the historical build, exec the binary, parse CSV stdout. If a SHA is being re-measured at a new `iters`, drop its old rows first so the file doesn't accumulate duplicates. On any failure, append `<sha> <reason>` to `trend_broken.txt` and continue.
5. **Bisect via a worklist (not recursion).** Push every adjacent measured pair `(A, B)` (sorted by date) onto a FIFO. Pop one at a time:
   - Compute the per-benchmark relative delta `|B.per_op_us − A.per_op_us| / A.per_op_us` for every shared `bench_name`.
   - If no benchmark trips the threshold, drop the pair.
   - Otherwise pick the midpoint commit (median commit by date in the first-parent slice strictly between A and B). If no such commit exists, or if the resulting `(A,M)` or `(M,B)` gap would fall below the min-gap, drop the pair - gap floor reached.
   - Otherwise measure M, then push **both** `(A, M)` and `(M, B)` back onto the worklist. Both sides must be re-checked: a regression bracketed inside A→M can coexist with another bracketed inside M→B, and skipping the still-tripping side silently leaves an unresolved high-delta gap.
   - Bound the total bisection budget with `--max-bisect N` (default 64) so a pathologically noisy series can't fork the worklist forever.
6. **Persist.** Rewrite the CSV sorted by (date ascending, bench_name ascending).

### Pure helpers tested separately

`tests/test_bench_trend_sample.c` - same harness pattern as `tests/test_repl_core_parse.c` etc. Built by Make, run from `make test` (added to `TEST_BINS`). Links `tools/bench_trend_sample/logic.o` directly. Covered functions:

- `trend_pick_initial_commits()` - inclusive range; root-commit edge case; min-gap floor; HEAD always included.
- `trend_pick_initial_days()` - calendar-day spacing; irregular commit timing; HEAD always included.
- `trend_compute_work_set()` - using the `trend_runs.csv` companion: SHA absent from runs file → returned; SHA present at different `iters` → returned; SHA whose CSV rows don't match its run-companion bench set → returned (partial / mid-crash); SHA whose run-companion records a smaller bench set than HEAD's *and* whose CSV rows fully match that recorded set → **omitted** (this is the bench-set-growth case; older SHAs legitimately emit fewer benches and must not be endlessly re-measured); broken SHA → omitted unless `--retry-broken`.
- `trend_pair_trips()` - threshold semantics; per-benchmark deltas; bench-name set intersection between A and B.
- `trend_bisect_step()` - both subranges re-queued when both trip; min-gap floor terminates; no-midpoint case drops the pair; `max_bisect` budget enforced.

These are zero-cost: no git, no make, no GL - the fixtures are in-memory tuples of `(sha, datetime, per_op_us[])`.

## Piece 2 - Viewer (`tools/bench_trend_view/`)

Renamed from `bench_trend` to `bench_trend_view` so the sampler/viewer pair doesn't read as `bench_trend` vs `bench-trend` (easy typo). Standalone GLUT binary, single source file, mirrors `tools/scene_demo/scene_demo.c` for structure.

### Dependencies

Header-only: `src/ui/core/gl_2d.h` (provides `gl2d_begin`, `gl2d_end`, `gl2d_draw_string`). No object deps from `src/` - the viewer is cheap to build because it doesn't share state with the REPL. `glBegin(GL_LINE_STRIP)` / `glVertex2f` / `glEnd` for the plot lines; `gl2d_draw_string` for axis labels. No `src/ui/core/theme.h` - pick fixed colors locally to keep deps zero.

### Data model (in-memory)

```c
#define MAX_BENCHES   16   /* generous cap; bench_repl currently emits 7 */
#define MAX_SAMPLES   4096

typedef struct {
    char    sha[16];
    long    date_epoch;                /* committer date, Unix seconds (CSV col 2) */
    char    date_label[16];            /* "YYYY-MM-DD" derived from epoch at load time */
    double  per_op_us[MAX_BENCHES];    /* NaN where this SHA lacks that bench */
} TrendSample;

static TrendSample g_samples[MAX_SAMPLES];
static int g_sample_count;
static char  g_bench_names[MAX_BENCHES][32];   /* populated from CSV */
static int   g_bench_count;
```

Loaded once at startup from `bench/trend_results.csv` (path overridable via `argv[1]`). Loader walks rows, interns each unique `bench_name` into `g_bench_names[]`, stores `per_op_us` into the matching column, parses the CSV's `date_epoch` column directly into `date_epoch` (no `strptime` - the sampler already converted to Unix seconds), and renders a short `YYYY-MM-DD` label via `strftime` once per sample for the X-axis. The CSV's `date_iso` column is read past for the format invariant but not consumed; the viewer is tz-agnostic by construction because the sort key is monotonic-integer epoch. Samples are sorted by `date_epoch` ascending. Columns the SHA lacks remain `NAN` and are skipped when drawing that series - older SHAs predating a benchmark draw a gap, not a spurious zero.

If load exceeds `MAX_SAMPLES`, drop the oldest samples and print one stderr line: `bench_trend_view: sample cap reached (MAX_SAMPLES=4096), dropped N oldest`. Better than a silent truncation or a crash.

### Rendering (per frame)

`gl2d_begin(w, h)` → orthographic 0..w, 0..h, depth/lighting off. Layout:

```
┌──────────────────────────────────────────────────────────────────┐
│ Bench Trend  axis: TIME   filter: ALL   samples: 47              │ ← top status
├──────────────────────────────────────────────────────────────────┤
│      │                                                           │
│  µs  │   ─── parse_lines                                         │
│      │   ─── feed_examples                                       │
│      │   ─── flatten_examples                                    │
│      │   (line strips, color per benchmark)                      │
│      │                                                           │
│      └──────────────────────────────────────────────────────────  │ ← X axis
│        2026-01    2026-02    2026-03    2026-04    2026-05       │
├──────────────────────────────────────────────────────────────────┤
│ t:time  s:sha-index   1-9:focus bench   0:all   n/p:next/prev    │ ← key hints
└──────────────────────────────────────────────────────────────────┘
```

- **X axis (time mode).** Linear from `g_samples[0].date_epoch` to `g_samples[n-1].date_epoch`. Tick every ~80 px, labeled with `YYYY-MM-DD` (from each sample's pre-rendered `date_label`).
- **X axis (sha-index mode).** Linear from 0 to n−1. Tick every ~80 px, labeled with the short SHA.
- **Y axis.** Linear, auto-scaled to `[0, 1.1 * max(per_op_us across visible benches)]`. Tick every ~50 px, labeled in µs.
- **Series.** For each visible benchmark, one `GL_LINE_STRIP` connecting the samples (in date order) whose `per_op_us` for that benchmark is not `NAN`, plus a small `GL_POINTS` marker at each sample. NaN samples break the strip into segments so older SHAs that predate a benchmark don't get a misleading sloped line down to zero.
- **Color palette.** Fixed table of up to `MAX_BENCHES` (16) saturated RGB triples chosen for distinguishability on the dark UI (red, orange, yellow, green, cyan, blue, magenta, plus muted variants). The first `g_bench_count` slots are used in load order.

### Controls

| Key | Action |
|-----|--------|
| `t` | Switch X-axis to date |
| `s` | Switch X-axis to chronological SHA index |
| `1`–`9` | Show only the Nth benchmark (no-op if `N > g_bench_count`) |
| `0` | Show all benchmarks |
| `n` / `p` | Next / previous benchmark filter (covers past 9) |
| `r` | Reload CSV (viewer can stay open during a long sampling run) |
| `q` / Esc | Quit |

`glutPostRedisplay()` after each key. Hover / mouse selection deferred to a follow-up.

### Why not `gl_2d.h`'s line helpers

There aren't any - `gl_2d.h` only provides `gl2d_panel_frame` (line *loops*). Series plotting goes through raw `glBegin(GL_LINE_STRIP)`. That's fine; the file stays small.

## Piece 3 - Makefile wiring

Two binaries plus a test binary, mirroring `scene_demo` / `editor_demo`:

Tool binaries follow the `scene_demo` / `editor_demo` pattern (explicit `*_OBJS` plus explicit link rule). The unit test integrates via the existing `TEST_BINS` machinery (`built_binary` macro around `Makefile:1172`), which means lowercase per-target variables (`test_bench_trend_sample_OBJS`, `test_bench_trend_sample_LDLIBS`, `test_bench_trend_sample_RUN`) keyed off the target name - mirroring `test_scene_palette` and `test_eval` which bypass the `core_test_binary` defaults the same way (by defining variables manually around `Makefile:909` and being filtered out of `CORE_TEST_BINS`).

```makefile
# --- bench_trend_view (GLUT viewer) ----------------------------
BENCH_TREND_VIEW_BIN = $(BINDIR)/bench_trend_view
BENCH_TREND_VIEW_OBJS = $(OBJDIR)/tools/bench_trend_view/bench_trend_view.o

$(BENCH_TREND_VIEW_BIN): $(BENCH_TREND_VIEW_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(BENCH_TREND_VIEW_OBJS) $(GL_LDFLAGS)

bench_trend_view: FORCE $(BENCH_TREND_VIEW_BIN) ## Build the historic bench trend viewer.
	ln -sfn $(BENCH_TREND_VIEW_BIN) $@

# --- bench_trend_sample (CLI sampler) --------------------------
BENCH_TREND_SAMPLE_BIN = $(BINDIR)/bench_trend_sample
BENCH_TREND_SAMPLE_OBJS = $(OBJDIR)/tools/bench_trend_sample/main.o \
                          $(OBJDIR)/tools/bench_trend_sample/logic.o

$(BENCH_TREND_SAMPLE_BIN): $(BENCH_TREND_SAMPLE_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(BENCH_TREND_SAMPLE_OBJS) -lm

bench_trend_sample: FORCE $(BENCH_TREND_SAMPLE_BIN) ## Build the historic bench trend sampler.
	ln -sfn $(BENCH_TREND_SAMPLE_BIN) $@

# --- sampler unit test (lowercase target-keyed override; uses built_binary) ---
# Override the core_test_binary defaults so this test links only its own
# object plus tools/bench_trend_sample/logic.o (no CORE_TEST_OBJS pull-in).
test_bench_trend_sample_OBJS = $(OBJDIR)/tests/test_bench_trend_sample.o \
                               $(OBJDIR)/tools/bench_trend_sample/logic.o
test_bench_trend_sample_LDLIBS = -lm
test_bench_trend_sample_RUN ?= $(BINDIR)/test_bench_trend_sample
```

The two `_OBJS` / `_LDLIBS` / `_RUN` lines for the test go in the file *before* `built_binary` runs over `TEST_BINS` (i.e. above the `$(foreach test,...)` loop at `Makefile:1180`), mirroring how `test_eval_OBJS = ...` and `test_eval_RUN = ...` are declared around `Makefile:909`. Then add `test_bench_trend_sample` to `TEST_BINS` (around `Makefile:854-870` in the alphabetical-ish list), AND add it to the `filter-out` list for `CORE_TEST_BINS` (around `Makefile:873`) so `core_test_binary` doesn't overwrite its custom objects.

Also wire into the existing cleanup machinery:

- **`ROOT_BIN_LINKS`** (Makefile:881): append `bench_trend_view bench_trend_sample` so the `clean` rule removes the root symlinks.
- **`clean` rule** (Makefile:1754): add `bench_trend_view.dSYM` and `bench_trend_sample.dSYM` alongside the existing tool `.dSYM` paths.
- **`TEST_BINS`** (search Makefile for the variable): append `test_bench_trend_sample` so `make test` runs it under ASan+UBSan like the other test binaries.

`.gitignore` additions - leading slashes for root anchoring, matching the existing `/scene_demo` / `/repl_demo` / `/editor_demo` entries:

```
/bench_trend_view
/bench_trend_view.dSYM
/bench_trend_sample
/bench_trend_sample.dSYM
/bench/trend_results.csv
/bench/trend_runs.csv
/bench/trend_broken.txt
```

(`bench/trend_results.csv` may eventually be committed as a baseline, but ship the feature with it gitignored so individual runs don't churn the repo.)

C99 ratchet: the `tools/bench_trend_*/` sources and `tests/test_bench_trend_sample.c` are picked up automatically.

## Critical files

- **New** `tools/bench_trend_sample/main.c` - argv parsing, popen orchestration, CSV I/O, work loop (~300 lines).
- **New** `tools/bench_trend_sample/logic.c` + `logic.h` - pure decision functions (~200 lines).
- **New** `tools/bench_trend_view/bench_trend_view.c` - GLUT viewer (single file, ~300–400 lines).
- **New** `tests/test_bench_trend_sample.c` - C unittest exercising `logic.c` (~150 lines).
- **Edit** `Makefile` - build rules for both binaries and the test (near existing `scene_demo` rules around line 1068); append both binary names to `ROOT_BIN_LINKS` (line 881); add their `.dSYM` paths to `clean` (line 1754); add `test_bench_trend_sample` to `TEST_BINS` and filter it out of `CORE_TEST_BINS` so `make test` runs it with its custom objects.
- **Edit** `.gitignore` - add the seven entries above.
- **New (runtime, gitignored)** `bench/trend_results.csv` - sampler output; canonical measurement store (12 columns; `date_epoch` is the sort key, `date_iso` is for human inspection).
- **New (runtime, gitignored)** `bench/trend_runs.csv` - per-SHA companion recording the bench-name set that SHA's `bench_repl` actually emitted at a given `iters`; consulted for idempotency so older SHAs that legitimately emit fewer benches aren't endlessly re-measured.
- **New (runtime, gitignored)** `bench/trend_broken.txt` - broken-SHA blacklist with `<sha> <reason>` lines.

Reused without modification: `scripts/build-historical.sh`, `bench/bench_repl.c` (the `--csv` mode), `src/ui/core/gl_2d.h`.

## Verification

End-to-end smoke (macOS host with real GL):

1. `make bench-trend-sample bench-trend-view test-bench-trend-sample` - all three build clean under `-std=c99`; the unit-test target runs and passes.
2. `./bench_trend_sample --min-gap-commits 50 --inflection-pct 15` - populates `bench/trend_results.csv` for the recent history in a few minutes (each SHA: ~5 s build + ~6 s bench).
3. `./bench_trend_view` - window opens with one colored line strip per discovered `bench_name` (seven at time of writing).
4. Press `t` then `s` - X-axis swaps between dates and SHA indices; curve shape preserved.
5. Press `3`, `0` - first isolates the third benchmark, then restores all.
6. Press `r` after re-running the sampler in another shell - viewer picks up the new rows without restart.
7. Re-run `./bench_trend_sample` with the same args - no new builds (idempotent), CSV unchanged.
8. Re-run with `--iters 20` (was 5) - every SHA re-measured, old rows replaced, no duplicates left behind, `trend_runs.csv` rewritten with the new `iters`.
9. Re-run with `--min-gap-commits 10` and a fabricated regression commit between two existing samples - bisection picks the midpoint and the curve gets a new vertex. With a second fabricated regression spanning into both subranges of an earlier bisection, both midpoints get measured (worklist, not recursion).
10. Add a single fake row referencing a non-existent `bench_name` to the CSV by hand, restart the viewer - it picks up the 8th series automatically (dynamic bench_name discovery).
11. Sample over a range straddling the bench harness's path move (e.g. `--from <commit-pre-2026-05-01> --to HEAD`): the default `<from>` resolves via both `bench/bench_repl.c` and root `bench_repl.c`; pre-move SHAs build via the root path, post-move SHAs build via `bench/`. The pre-move SHAs that legitimately lacked `spike_flatten_largest` do not get re-measured on the next idempotent run (run-companion records the smaller bench set).
12. Drop two adjacent first-parent commits with different timezone offsets into the CSV by hand and reload the viewer - they appear in true UTC chronological order, not in lex order of the ISO strings (tz-flip resistance proof).

Headless (gracemont):

```bash
ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
    git pull --ff-only origin main && \
    make check-c99 test_bench_trend_sample && \
    ./bench_trend_sample --min-gap-commits 30 --dry-run'
```

`make check-c99` confirms both new tools compile under real GCC; `test_bench_trend_sample` exercises the pure logic under ASan+UBSan; `--dry-run` confirms the sampler walks history without invoking the historical build script.

The viewer is not unit-tested (interactive GLUT); the smoke walk above is its check. If the sampler grows (e.g. perf-counter columns from the Tier 2 plan in `plans/done/benchmark-metrics.md`), extend the C unit test.
