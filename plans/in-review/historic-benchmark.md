# Historic Bench Trend

## Context

Right now `make bench` produces wall-time numbers for one commit. There is no easy way to ask "when did `flatten_examples` get slower?" or "did last week's refactor regress `replay_long`?" — answering either today requires manually checking out old SHAs, rebuilding, and eyeballing the diff.

This plan adds two pieces that together produce a historical view:

1. A **sampler** that walks git history, runs `make bench` at chosen commits, and persists the CSV.
2. A **viewer** that plots that CSV with fixed-function OpenGL (matching the project's aesthetic and avoiding new toolchain deps).

Sampling is bounded by a user-specified minimum gap (commits *or* days, picked per-run); regions with a large relative delta between adjacent samples are then bisected down to that gap floor. The viewer toggles its X-axis between commit date and chronological SHA index.

The headless infrastructure already exists: `bench_repl` supports `--csv` and `USE_GL_STUBS=1`, and `scripts/build-historical.sh` already builds arbitrary SHAs in isolated worktrees with compat headers spliced in. This plan wires those together.

## Architecture

```
       git log
          │
          ▼
  scripts/bench_trend_sample.py ──► bench/trend_results.csv ◄── tools/bench_trend/bench_trend.c
          │                              (canonical store)             (fixed-function GL viewer)
          ▼
  scripts/build-historical.sh --at <sha> bench USE_GL_STUBS=1 BENCH_ARGS="--csv --iters N"
```

The CSV is the contract between the two halves. The sampler is idempotent: it loads the existing CSV, decides which SHAs still need measuring, and only invokes the builder for those.

## Piece 1 — Sampler (`scripts/bench_trend_sample.py`)

Python 3 stdlib only (`subprocess`, `csv`, `argparse`, `datetime`, `pathlib`) — keeps with the project's no-extra-deps convention seen in `scripts/cflow_to_*.py`.

### CLI

```
scripts/bench_trend_sample.py
    [--from REF]                # default: first commit that has bench/bench_repl.c
    [--to REF]                  # default: HEAD
    [--min-gap-commits N]       # mutually exclusive with --min-gap-days
    [--min-gap-days D]
    [--inflection-pct PCT]      # default 10.0
    [--iters N]                 # forwarded to bench (default 5)
    [--csv PATH]                # default: bench/trend_results.csv
    [--dry-run]                 # print what would be sampled, don't build
```

Exactly one of `--min-gap-commits` / `--min-gap-days` is required (matches the "user picks per-run" answer).

### CSV schema

`bench/trend_results.csv` — one row per (sha, benchmark):

```
sha,date,bench_name,unit,iters,ops,total_sec,min_iter_ms,per_iter_ms,per_op_us,ops_per_sec
```

`sha` is the 12-char short hash, `date` is the committer date as ISO 8601 (`%Y-%m-%dT%H:%M:%S%z`). The last 9 columns are the existing `bench_repl --csv` columns verbatim. `bench_repl` currently emits seven sub-benchmarks (`parse_lines`, `feed_examples`, `flatten_examples`, `spike_flatten_largest`, `replay_examples`, `replay_long`, `fade_batches` — see `bench/bench_repl.c:802-823`), so a fully-measured SHA contributes seven rows; both the sampler and viewer treat the bench-name set as data, not as a hard-coded constant, so adding or removing a `bench_repl` sub-benchmark needs no code change here.

The sampler rewrites the file sorted by `date` ascending (and by `bench_name` within a SHA) after each successful run so diffs and viewer reads stay stable. A sibling `bench/trend_broken.txt` records SHAs where the build failed, so we don't retry them on every run.

### Algorithm

1. **Enumerate (inclusive of `<from>`).** `git log --format='%H %cI' <to> --reverse --first-parent` then drop everything before `<from>` (inclusive). Equivalently: if `<from>` has a parent, `git log <from>^..<to>`; if it is the root commit, `git log <to>`. The default `<from>` (first commit that touches `bench/bench_repl.c`) must itself be sampleable, so an exclusive `<from>..<to>` is wrong. (First-parent keeps merge bubbles from inflating the sample set.)
2. **Initial pick.** Walk the list in chronological order. Always pick the first commit. Then pick the next commit whose gap from the last pick *meets or exceeds* the min-gap (commit count for `--min-gap-commits`, calendar days between committer dates for `--min-gap-days`). Always include the last commit even if it sits inside the gap, so the trend line reaches HEAD.
3. **Compute the work set.** Load existing rows from the CSV. A SHA is *fully covered* at the requested `--iters` only if it has one row per `bench_name` that `bench_repl` would emit at this build *and* every row's `iters` column matches. The expected `bench_name` set comes from the most recent run already in the CSV (or, on a cold CSV, from the first newly-built SHA). Re-runs with a different `--iters` therefore re-measure even SHAs that already appear, and a partial SHA (e.g. crashed mid-write, or bench list grew) is treated as missing and re-measured. SHAs in `trend_broken.txt` are skipped regardless. Treat the broken list as a hint, not a permanent ban — `--retry-broken` flushes it for the run.
4. **Sample.** For each work-set SHA, run:
   ```
   ./scripts/build-historical.sh --at <sha> bench USE_GL_STUBS=1 BENCH_ARGS="--csv --iters {iters}"
   ```
   Capture stdout, parse the CSV lines (skip the header), append rows. If a SHA is being re-measured at a new `iters`, drop its old rows first so the file doesn't accumulate duplicates. On non-zero exit, append to `trend_broken.txt` and continue.
5. **Bisect via a worklist (not recursion).** Push every adjacent measured pair `(A, B)` (sorted by date) onto a FIFO. Pop one at a time; for each:
   - Compute the per-benchmark relative delta `|B.per_op_us − A.per_op_us| / A.per_op_us` for every shared `bench_name`.
   - If no benchmark trips the threshold, drop the pair.
   - Otherwise, pick the midpoint commit (the median commit by date in the first-parent slice strictly between A and B). If no such commit exists, or if the resulting (A,M) or (M,B) gap would fall below the min-gap, drop the pair — we've hit the floor.
   - Otherwise measure M, then push **both** `(A, M)` and `(M, B)` back onto the worklist. Both sides must be re-checked: a regression bracketed inside A→M can coexist with another bracketed inside M→B, and skipping the still-tripping side silently leaves an unresolved high-delta gap.
   - Bound the total bisection budget with a `--max-bisect N` cap (default 64) so a pathologically noisy series can't fork the worklist forever.
6. **Persist.** Rewrite the CSV sorted by (date ascending, bench_name ascending).

### Reused helpers

- `scripts/build-historical.sh` — already does worktree isolation under `.compat-scratch/worktrees/<sha>/` and forwards make args. Don't reinvent.
- `bench_repl --csv` — already emits the 9-column row we want. Don't reformat.

## Piece 2 — Viewer (`tools/bench_trend/bench_trend.c`)

Standalone GLUT binary, single source file. Mirrors `tools/scene_demo/scene_demo.c` for structure: `main()` does `glutInit`, registers display/reshape/keyboard callbacks, enters `glutMainLoop()`.

### Dependencies

Header-only: `src/ui/core/gl_2d.h` (provides `gl2d_begin`, `gl2d_end`, `gl2d_draw_string`). No object deps from `src/` — the viewer needs to be cheap to build because it doesn't share state with the REPL. `glBegin(GL_LINE_STRIP)` / `glVertex2f` / `glEnd` for the plot lines; `gl2d_draw_string` for axis labels. No `src/ui/core/theme.h` — pick six fixed colors locally to keep deps zero.

### Data model (in-memory)

The viewer discovers the bench-name set from the CSV at load time so a new sub-benchmark (e.g. the existing `spike_flatten_largest`, or anything added later) shows up automatically.

```c
#define MAX_BENCHES   16   /* generous cap; bench_repl currently emits 7 */
#define MAX_SAMPLES   4096

typedef struct {
    char sha[16];
    time_t date;
    double per_op_us[MAX_BENCHES];   /* NaN where this SHA lacks that bench */
} TrendSample;

static TrendSample g_samples[MAX_SAMPLES];
static int g_sample_count;
static char  g_bench_names[MAX_BENCHES][32];   /* populated from CSV */
static int   g_bench_count;
```

Loaded once at startup from `bench/trend_results.csv` (path overridable via `argv[1]`). Loader walks the rows, interns each unique `bench_name` into `g_bench_names[]`, and stores `per_op_us` into the matching column. Samples are sorted by date ascending; columns the SHA lacks remain NaN and are skipped when drawing that series' line strip (so older SHAs predating a benchmark draw a gap, not a spurious zero).

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
│ T:time  S:sha-index   1-6:focus bench   0:all   q:quit           │ ← key hints
└──────────────────────────────────────────────────────────────────┘
```

- **X axis (time mode).** Linear from `g_samples[0].date` to `g_samples[n-1].date`. Tick every ~80px, labeled with `YYYY-MM-DD`.
- **X axis (sha-index mode).** Linear from 0 to n-1. Tick every ~80px, labeled with the short SHA.
- **Y axis.** Linear, auto-scaled to `[0, 1.1 * max(per_op_us across visible benches)]`. Tick every ~50px, labeled in µs.
- **Series.** For each visible benchmark, one `GL_LINE_STRIP` connecting the samples (in date order) whose `per_op_us` column for that benchmark is not NaN, plus a small `GL_POINTS` marker at each sample. NaN samples break the strip into segments so older SHAs that predate a benchmark don't get a misleading sloped line down to zero.
- **Color palette.** A fixed table of up to `MAX_BENCHES` (16) saturated RGB triples chosen for distinguishability on the dark UI (red, orange, yellow, green, cyan, blue, magenta, plus muted variants). The first `g_bench_count` slots are used in load order.

### Controls

| Key | Action |
|-----|--------|
| `t` | Switch X-axis to date |
| `s` | Switch X-axis to chronological SHA index |
| `1`–`9` | Show only the Nth benchmark (no-op if `N > g_bench_count`) |
| `0` | Show all benchmarks |
| `n` / `p` | Next / previous benchmark filter (works past 9) |
| `r` | Reload CSV (so the viewer can be kept open while the sampler runs) |
| `q` / Esc | Quit |

`glutPostRedisplay()` after each key. Hover / mouse selection deferred to a follow-up — the toggle + filter set covers the stated requirement.

### Why not `gl_2d.h`'s line helpers

There aren't any — `gl_2d.h` only provides `gl2d_panel_frame` (line *loops*). Series plotting goes through raw `glBegin(GL_LINE_STRIP)`. That's fine; the file stays small.

## Piece 3 — Makefile wiring

Add to `Makefile`, mirroring `scene_demo`:

```makefile
# --- bench_trend viewer ------------------------------------------
BENCH_TREND_OBJS = $(OBJDIR)/tools/bench_trend/bench_trend.o

$(BINDIR)/bench_trend: $(BENCH_TREND_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(BENCH_TREND_OBJS) $(GL_LDFLAGS)

bench_trend: FORCE $(BINDIR)/bench_trend ## Build the historic bench trend viewer.
	ln -sfn $(BINDIR)/bench_trend $@

# --- sampler runner ----------------------------------------------
# Usage: make bench-trend MIN_GAP_COMMITS=20 INFLECTION_PCT=10
bench-trend:
	python3 scripts/bench_trend_sample.py \
	    $(if $(MIN_GAP_COMMITS),--min-gap-commits $(MIN_GAP_COMMITS)) \
	    $(if $(MIN_GAP_DAYS),--min-gap-days $(MIN_GAP_DAYS)) \
	    $(if $(INFLECTION_PCT),--inflection-pct $(INFLECTION_PCT)) \
	    $(if $(BENCH_ITERS),--iters $(BENCH_ITERS))

# --- sampler unit tests ------------------------------------------
test_bench_trend_sample:
	python3 -m unittest tests.test_bench_trend_sample
```

Also wire `bench_trend` into the existing cleanup machinery:

- **`ROOT_BIN_LINKS`** (Makefile:664) currently reads `gl-repl scene_demo repl_demo editor_demo`. Append `bench_trend` so the `clean` rule removes the root symlink.
- **`clean`** rule (Makefile:1307) explicitly lists `*.dSYM` paths per tool — add `bench_trend.dSYM` alongside `scene_demo.dSYM` etc. so debug bundles on macOS get cleaned up too.

C99 ratchet: `tools/bench_trend/bench_trend.c` automatically picks up `make check-c99` because the ratchet syntax-checks the `tools/` set. Nothing else to do.

`.gitignore` additions (current file at `.gitignore:1-26` covers the other demo binaries but not the trend artifacts):

```
/bench_trend
/bench_trend.dSYM
bench/trend_results.csv
bench/trend_broken.txt
```

(`bench/trend_results.csv` may eventually be committed as a baseline, but ship the feature with it gitignored so individual runs don't churn the repo.)

## Critical files

- **New** `scripts/bench_trend_sample.py` — sampler (Python 3 stdlib only).
- **New** `tools/bench_trend/bench_trend.c` — GLUT viewer (single file, ~300–400 lines).
- **New** `tests/test_bench_trend_sample.py` — pure-Python unittest coverage of the sampler decision functions.
- **Edit** `Makefile` — add `bench_trend` build rule and `bench-trend` / `test_bench_trend_sample` runner targets (near the existing `scene_demo` / `bench` rules around lines 751 and 1244); append `bench_trend` to `ROOT_BIN_LINKS` (line 664); add `bench_trend.dSYM` to the `clean` rule (line 1307).
- **Edit** `.gitignore` — add `/bench_trend`, `/bench_trend.dSYM`, `bench/trend_results.csv`, `bench/trend_broken.txt`.
- **New (runtime, gitignored)** `bench/trend_results.csv` — sampler output; canonical store.
- **New (runtime, gitignored)** `bench/trend_broken.txt` — broken-SHA blacklist.

Reused without modification: `scripts/build-historical.sh`, `bench/bench_repl.c` (the `--csv` mode is already there), `src/ui/core/gl_2d.h`.

## Verification

End-to-end smoke (on the macOS host that has GL):

1. `make bench_trend` — builds the viewer cleanly under `-std=c99`.
2. `make bench-trend MIN_GAP_COMMITS=50 INFLECTION_PCT=15` — populates `bench/trend_results.csv` for the last ~10–20 commits in a few minutes (each SHA: ~5 s build + ~6 s bench).
3. `./bench_trend` — window opens with one colored line strip per discovered `bench_name` (seven at time of writing).
4. Press `t` then `s` — X-axis swaps between dates and SHA indices; the curve shape is preserved.
5. Press `3`, `0` — first isolates the third benchmark, then restores all.
6. Re-run `make bench-trend` with the same args — no new builds (idempotent), CSV unchanged.
7. Re-run with the same args but a different `--iters` (e.g. `BENCH_ITERS=20` after a `BENCH_ITERS=5` baseline) — every SHA is re-measured, old rows replaced, no duplicates left behind.
8. Re-run with a smaller `MIN_GAP_COMMITS=10` and a synthetic regression commit between two already-sampled SHAs — the bisection picks up the midpoint and the curve gets a new vertex. Then craft a second synthetic regression so both sub-ranges trip; confirm both midpoints get measured (worklist, not recursion).

Headless check (on gracemont):

```bash
ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
    git pull --ff-only origin main && \
    make check-c99 && \
    python3 scripts/bench_trend_sample.py --min-gap-commits 30 --dry-run'
```

`make check-c99` confirms the new viewer compiles under real GCC; `--dry-run` confirms the sampler walks history without invoking builds.

Unit tests for the sampler — `tests/test_bench_trend_sample.py`, Python `unittest`, run via `python3 -m unittest tests.test_bench_trend_sample`. The risky parts are pure logic and trivial to fixture (commit lists are mocked tuples of `(sha, datetime)`; CSV state is a list of dicts). Coverage required:

- **Inclusive range.** `pick_initial(<from>, <to>, ...)` includes both endpoints, including the case where `<from>` is the root commit (no parent).
- **Min-gap selection — commits.** Given a list of 100 sequential commits and `min_gap_commits=20`, expect indices 0, 20, 40, 60, 80, 99 (inclusive of last).
- **Min-gap selection — days.** Given irregularly-spaced commits and `min_gap_days=7`, the selection respects calendar-day deltas and always includes the last commit.
- **Idempotency keying.** `missing_shas(existing_rows, requested_shas, iters, expected_bench_names)` returns:
  - A SHA absent from `existing_rows`.
  - A SHA present at a *different* `iters`.
  - A SHA whose rows cover only a subset of `expected_bench_names` (partial / crashed).
  - …and does **not** return a SHA whose rows fully match.
- **Bisection worklist.** Construct a 5-sample series where two non-adjacent gaps trip the threshold. Confirm the worklist measures both midpoints (not just one), respects the `min_gap` floor, and terminates within `max_bisect`. A separate case confirms that when both `(A,M)` and `(M,B)` trip after sampling M, both get re-queued.
- **Broken-SHA skip.** A SHA in `trend_broken.txt` is omitted from the work set unless `--retry-broken` is set.

These run with no GL, no git, no Make — they exercise the pure decision functions. They live in `tests/` next to the existing C test sources but use Python; the Makefile gets a small `test_bench_trend_sample` PHONY target that shells out to `python3 -m unittest`.

The viewer itself stays uncovered by automated tests (interactive GLUT) — the smoke walk above is its check. If the sampler grows (e.g. perf-counter columns from the Tier 2 plan in `plans/done/benchmark-metrics.md`), extend the Python suite.
