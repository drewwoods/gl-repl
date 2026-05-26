# Historic Bench Trend

## Context

Right now `make bench` produces wall-time numbers for one commit. There is no easy way to ask "when did `flatten_examples` get slower?" or "did last week's refactor regress `replay_long`?" — answering either today requires manually checking out old SHAs, rebuilding, and eyeballing the diff.

This plan adds two pieces that together produce a historical view:

1. A **sampler** (C binary) that walks git history, runs `bench_repl` at chosen commits, and persists the CSV.
2. A **viewer** (C/GLUT binary) that plots that CSV with fixed-function OpenGL — matching the project's aesthetic and avoiding new toolchain deps.

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

## Piece 1 — Sampler (`tools/bench_trend_sample/`)

Single binary, two source files for testability:

- `tools/bench_trend_sample/main.c` — `argv` parsing (`getopt_long`), git + build orchestration via `popen`, CSV file I/O, and the work loop. Calls into the pure helpers in `logic.c`.
- `tools/bench_trend_sample/logic.c` + `logic.h` — pure decision functions (initial-pick, work-set, pair-trips, bisection-step). Linked directly by the C unit test below, so they're exercised without spawning processes.

### CLI

```
./bench_trend_sample [options]
    --from REF              # default: first commit that touches bench/bench_repl.c
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

### CSV schema

`bench/trend_results.csv` — one row per (sha, benchmark):

```
sha,date,bench_name,unit,iters,ops,total_sec,min_iter_ms,per_iter_ms,per_op_us,ops_per_sec
```

`sha` is the 12-char short hash. `date` is the committer date as ISO 8601 (`%Y-%m-%dT%H:%M:%S%z`) stored verbatim as a string — ISO 8601 lex-sorts correctly, so file sort and series order are stringly cheap; `strptime` + `mktime` only happens in the viewer at load time for the X-axis scale.

The last 9 columns are the existing `bench_repl --csv` columns verbatim. `bench_repl` currently emits seven sub-benchmarks (`parse_lines`, `feed_examples`, `flatten_examples`, `spike_flatten_largest`, `replay_examples`, `replay_long`, `fade_batches` — see `bench/bench_repl.c:802-823`), so a fully-measured SHA contributes seven rows. Both sampler and viewer treat the bench-name set as data, not as a hard-coded constant, so adding or removing a `bench_repl` sub-benchmark needs no code change here.

The sampler rewrites the file sorted by (date ascending, bench_name ascending) after each successful run so diffs and viewer reads stay stable. A sibling `bench/trend_broken.txt` records broken SHAs as `<sha> <reason>` lines (e.g. `a1b2c3d4e5f6 csv unsupported`, `a1b2c3d4e5f6 build failed`, `a1b2c3d4e5f6 binary not found`).

### Build vs Run separation (correctness fix)

The sampler invokes `build-historical.sh --at <sha> bench_repl USE_GL_STUBS=1` strictly to *build* the historical bench binary; it discards that command's stdout (the build script prints `build-historical: ...` diagnostic lines there). Then it computes the worktree path the same way the script does — `.compat-scratch/worktrees/<short-12-sha>/` (the formula at `scripts/build-historical.sh:218-219`) — and `popen`s the binary directly: `<worktree>/build/bin/bench_repl --csv --iters N`. That stream is pure CSV and parses with `fgets` + a column scanner.

Why this matters:
- `make bench` is a *runner* target that prints its own `REPL benchmarks (iters=...)` header before the CSV body — using it would force CSV-shape filtering on every line. Going via `make bench_repl` plus direct exec gives us a clean stream.
- `--csv` is a recent addition to `bench_repl.c`. Old SHAs may lack it (`getopt` rejects, exit non-zero or empty stdout). The sampler detects this and writes `<sha> csv unsupported` to `trend_broken.txt` rather than spinning forever.
- Binary location has shifted historically. Fall back order: `<worktree>/build/bin/bench_repl`, `<worktree>/build/bench_repl`, `<worktree>/bench_repl`. If none exists, log `<sha> binary not found` to `trend_broken.txt`.

### `fade_batches` under stubs (caveat)

`fade_batches` calls `bench_gl_context_init` (`bench/bench_repl.c:818`). Under `USE_GL_STUBS=1` the GL init is a no-op and every `glBegin`/`glVertex` becomes a counter-incrementing inline stub. So the sub-benchmark measures *CPU* work only, not GPU time. This is fine for tracking REPL-side fade scheduling regressions but is **not** comparable across machines with different GL drivers, and shouldn't be read as a graphics-performance signal. The plan accepts this trade-off because (a) it lets the sampler run on the headless `gracemont` box and (b) the regressions we actually care about (parser, flatten, executor) are CPU-bound anyway. The viewer's existing NaN/series-gap handling already covers a SHA where `fade_batches` is absent entirely.

### Algorithm

1. **Enumerate (inclusive of `<from>`).** Spawn `git log --format='%H %cI' <to> --reverse --first-parent` and drop everything before `<from>` (inclusive). Equivalently, when `<from>` has a parent, `git log <from>^..<to>`; when it's the root commit, `git log <to>`. The default `<from>` (first commit touching `bench/bench_repl.c`) must itself be sampleable, so an exclusive `<from>..<to>` is wrong. First-parent keeps merge bubbles from inflating the sample set.
2. **Initial pick.** Walk the list chronologically. Always pick the first commit. Then pick the next commit whose gap from the last pick *meets or exceeds* the min-gap (commit count for `--min-gap-commits`, calendar days between committer dates for `--min-gap-days`). Always include the last commit even if it sits inside the gap, so the trend line reaches HEAD.
3. **Compute the work set.** Load existing rows from the CSV. A SHA is *fully covered* at the requested `--iters` only if it has one row per `bench_name` that `bench_repl` would emit at this build *and* every row's `iters` matches. The expected `bench_name` set comes from the most recent run already in the CSV (or, on a cold CSV, from the first newly-built SHA — bootstrapping). Re-runs with a different `--iters` therefore re-measure even SHAs that appear; a partial SHA (crashed mid-write, or bench list grew) is treated as missing and re-measured. SHAs in `trend_broken.txt` are skipped unless `--retry-broken` flushes it.
4. **Sample.** For each work-set SHA: run the historical build, exec the binary, parse CSV stdout. If a SHA is being re-measured at a new `iters`, drop its old rows first so the file doesn't accumulate duplicates. On any failure, append `<sha> <reason>` to `trend_broken.txt` and continue.
5. **Bisect via a worklist (not recursion).** Push every adjacent measured pair `(A, B)` (sorted by date) onto a FIFO. Pop one at a time:
   - Compute the per-benchmark relative delta `|B.per_op_us − A.per_op_us| / A.per_op_us` for every shared `bench_name`.
   - If no benchmark trips the threshold, drop the pair.
   - Otherwise pick the midpoint commit (median commit by date in the first-parent slice strictly between A and B). If no such commit exists, or if the resulting `(A,M)` or `(M,B)` gap would fall below the min-gap, drop the pair — gap floor reached.
   - Otherwise measure M, then push **both** `(A, M)` and `(M, B)` back onto the worklist. Both sides must be re-checked: a regression bracketed inside A→M can coexist with another bracketed inside M→B, and skipping the still-tripping side silently leaves an unresolved high-delta gap.
   - Bound the total bisection budget with `--max-bisect N` (default 64) so a pathologically noisy series can't fork the worklist forever.
6. **Persist.** Rewrite the CSV sorted by (date ascending, bench_name ascending).

### Pure helpers tested separately

`tests/test_bench_trend_sample.c` — same harness pattern as `tests/test_repl_core_parse.c` etc. Built by Make, run from `make test` (added to `TEST_BINS`). Links `tools/bench_trend_sample/logic.o` directly. Covered functions:

- `trend_pick_initial_commits()` — inclusive range; root-commit edge case; min-gap floor; HEAD always included.
- `trend_pick_initial_days()` — calendar-day spacing; irregular commit timing; HEAD always included.
- `trend_compute_work_set()` — SHA absent → returned; SHA present at different `iters` → returned; SHA with partial `bench_name` coverage → returned; SHA fully matching → omitted; broken SHA → omitted unless `--retry-broken`.
- `trend_pair_trips()` — threshold semantics; per-benchmark deltas; bench-name set intersection between A and B.
- `trend_bisect_step()` — both subranges re-queued when both trip; min-gap floor terminates; no-midpoint case drops the pair; `max_bisect` budget enforced.

These are zero-cost: no git, no make, no GL — the fixtures are in-memory tuples of `(sha, datetime, per_op_us[])`.

## Piece 2 — Viewer (`tools/bench_trend_view/`)

Renamed from `bench_trend` to `bench_trend_view` so the sampler/viewer pair doesn't read as `bench_trend` vs `bench-trend` (easy typo). Standalone GLUT binary, single source file, mirrors `tools/scene_demo/scene_demo.c` for structure.

### Dependencies

Header-only: `src/ui/core/gl_2d.h` (provides `gl2d_begin`, `gl2d_end`, `gl2d_draw_string`). No object deps from `src/` — the viewer is cheap to build because it doesn't share state with the REPL. `glBegin(GL_LINE_STRIP)` / `glVertex2f` / `glEnd` for the plot lines; `gl2d_draw_string` for axis labels. No `src/ui/core/theme.h` — pick fixed colors locally to keep deps zero.

### Data model (in-memory)

```c
#define MAX_BENCHES   16   /* generous cap; bench_repl currently emits 7 */
#define MAX_SAMPLES   4096

typedef struct {
    char   sha[16];
    char   date_iso[40];               /* raw ISO 8601, lex-sortable */
    time_t date_unix;                  /* parsed once at load, for X-axis */
    double per_op_us[MAX_BENCHES];     /* NaN where this SHA lacks that bench */
} TrendSample;

static TrendSample g_samples[MAX_SAMPLES];
static int g_sample_count;
static char  g_bench_names[MAX_BENCHES][32];   /* populated from CSV */
static int   g_bench_count;
```

Loaded once at startup from `bench/trend_results.csv` (path overridable via `argv[1]`). Loader walks rows, interns each unique `bench_name` into `g_bench_names[]`, stores `per_op_us` into the matching column. Samples sorted by `date_iso` ascending (lex sort gives chronological order). `strptime` + `mktime` runs once per sample to fill `date_unix`. Columns the SHA lacks remain `NAN` and are skipped when drawing that series — older SHAs predating a benchmark draw a gap, not a spurious zero.

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

- **X axis (time mode).** Linear from `g_samples[0].date_unix` to `g_samples[n-1].date_unix`. Tick every ~80 px, labeled with `YYYY-MM-DD`.
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

There aren't any — `gl_2d.h` only provides `gl2d_panel_frame` (line *loops*). Series plotting goes through raw `glBegin(GL_LINE_STRIP)`. That's fine; the file stays small.

## Piece 3 — Makefile wiring

Two binaries plus a test binary, mirroring `scene_demo` / `editor_demo`:

```makefile
# --- bench_trend_view (GLUT viewer) ----------------------------
BENCH_TREND_VIEW_OBJS = $(OBJDIR)/tools/bench_trend_view/bench_trend_view.o

$(BINDIR)/bench_trend_view: $(BENCH_TREND_VIEW_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(BENCH_TREND_VIEW_OBJS) $(GL_LDFLAGS)

bench_trend_view: FORCE $(BINDIR)/bench_trend_view ## Build the historic bench trend viewer.
	ln -sfn $(BINDIR)/bench_trend_view $@

# --- bench_trend_sample (CLI sampler) --------------------------
BENCH_TREND_SAMPLE_OBJS = $(OBJDIR)/tools/bench_trend_sample/main.o \
                          $(OBJDIR)/tools/bench_trend_sample/logic.o

$(BINDIR)/bench_trend_sample: $(BENCH_TREND_SAMPLE_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(BENCH_TREND_SAMPLE_OBJS) -lm

bench_trend_sample: FORCE $(BINDIR)/bench_trend_sample ## Build the historic bench trend sampler.
	ln -sfn $(BINDIR)/bench_trend_sample $@

# --- sampler unit tests ----------------------------------------
TEST_BENCH_TREND_SAMPLE_OBJS = $(OBJDIR)/tests/test_bench_trend_sample.o \
                               $(OBJDIR)/tools/bench_trend_sample/logic.o

test_bench_trend_sample: $(TEST_BENCH_TREND_SAMPLE_OBJS)
	$(CC) $(OBJ_CFLAGS) -o $@ $(TEST_BENCH_TREND_SAMPLE_OBJS) -lm
	./$@
```

Also wire into the existing cleanup machinery:

- **`ROOT_BIN_LINKS`** (Makefile:664): append `bench_trend_view bench_trend_sample` so the `clean` rule removes the root symlinks.
- **`clean` rule** (Makefile:1307): add `bench_trend_view.dSYM` and `bench_trend_sample.dSYM` alongside the existing tool `.dSYM` paths.
- **`TEST_BINS`** (search Makefile for the variable): append `test_bench_trend_sample` so `make test` runs it under ASan+UBSan like the other test binaries.

`.gitignore` additions — leading slashes for root anchoring, matching the existing `/scene_demo` / `/repl_demo` / `/editor_demo` entries:

```
/bench_trend_view
/bench_trend_view.dSYM
/bench_trend_sample
/bench_trend_sample.dSYM
/bench/trend_results.csv
/bench/trend_broken.txt
```

(`bench/trend_results.csv` may eventually be committed as a baseline, but ship the feature with it gitignored so individual runs don't churn the repo.)

C99 ratchet: the `tools/bench_trend_*/` sources and `tests/test_bench_trend_sample.c` are picked up automatically.

## Critical files

- **New** `tools/bench_trend_sample/main.c` — argv parsing, popen orchestration, CSV I/O, work loop (~300 lines).
- **New** `tools/bench_trend_sample/logic.c` + `logic.h` — pure decision functions (~200 lines).
- **New** `tools/bench_trend_view/bench_trend_view.c` — GLUT viewer (single file, ~300–400 lines).
- **New** `tests/test_bench_trend_sample.c` — C unittest exercising `logic.c` (~150 lines).
- **Edit** `Makefile` — build rules for both binaries and the test (near existing `scene_demo` rules around line 751); append both binary names to `ROOT_BIN_LINKS` (line 664); add their `.dSYM` paths to `clean` (line 1307); add `test_bench_trend_sample` to `TEST_BINS` so `make test` runs it.
- **Edit** `.gitignore` — add the six entries above.
- **New (runtime, gitignored)** `bench/trend_results.csv` — sampler output; canonical store.
- **New (runtime, gitignored)** `bench/trend_broken.txt` — broken-SHA blacklist with `<sha> <reason>` lines.

Reused without modification: `scripts/build-historical.sh`, `bench/bench_repl.c` (the `--csv` mode), `src/ui/core/gl_2d.h`.

## Verification

End-to-end smoke (macOS host with real GL):

1. `make bench_trend_sample bench_trend_view test_bench_trend_sample` — all three build clean under `-std=c99`; the unit-test target runs and passes.
2. `./bench_trend_sample --min-gap-commits 50 --inflection-pct 15` — populates `bench/trend_results.csv` for the recent history in a few minutes (each SHA: ~5 s build + ~6 s bench).
3. `./bench_trend_view` — window opens with one colored line strip per discovered `bench_name` (seven at time of writing).
4. Press `t` then `s` — X-axis swaps between dates and SHA indices; curve shape preserved.
5. Press `3`, `0` — first isolates the third benchmark, then restores all.
6. Press `r` after re-running the sampler in another shell — viewer picks up the new rows without restart.
7. Re-run `./bench_trend_sample` with the same args — no new builds (idempotent), CSV unchanged.
8. Re-run with `--iters 20` (was 5) — every SHA re-measured, old rows replaced, no duplicates left behind.
9. Re-run with `--min-gap-commits 10` and a fabricated regression commit between two existing samples — bisection picks the midpoint and the curve gets a new vertex. With a second fabricated regression spanning into both subranges of an earlier bisection, both midpoints get measured (worklist, not recursion).
10. Add a single fake row referencing a non-existent `bench_name` to the CSV by hand, restart the viewer — it picks up the 8th series automatically (dynamic bench_name discovery).

Headless (gracemont):

```bash
ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
    git pull --ff-only origin main && \
    make check-c99 test_bench_trend_sample && \
    ./bench_trend_sample --min-gap-commits 30 --dry-run'
```

`make check-c99` confirms both new tools compile under real GCC; `test_bench_trend_sample` exercises the pure logic under ASan+UBSan; `--dry-run` confirms the sampler walks history without invoking the historical build script.

The viewer is not unit-tested (interactive GLUT); the smoke walk above is its check. If the sampler grows (e.g. perf-counter columns from the Tier 2 plan in `plans/done/benchmark-metrics.md`), extend the C unit test.
