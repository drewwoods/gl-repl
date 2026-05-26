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

`sha` is the 12-char short hash, `date` is the committer date as ISO 8601 (`%Y-%m-%dT%H:%M:%S%z`). The last 9 columns are the existing `bench_repl --csv` columns verbatim. Six rows per measured SHA (one per sub-benchmark).

The sampler rewrites the file sorted by `date` ascending after each successful run so diffs and viewer reads stay stable. A sibling `bench/trend_broken.txt` records SHAs where the build failed, so we don't retry them on every run.

### Algorithm

1. **Enumerate.** `git log --format='%H %cI' <from>..<to> --reverse --first-parent`. (First-parent keeps merge bubbles from inflating the sample set.)
2. **Initial pick.** Walk the list; pick the first commit, then pick the next commit whose gap from the last pick exceeds the min-gap. Always include the last commit.
3. **Build the missing set.** Subtract SHAs already in the CSV and in `trend_broken.txt`.
4. **Sample.** For each missing SHA, run:
   ```
   ./scripts/build-historical.sh --at <sha> bench USE_GL_STUBS=1 BENCH_ARGS="--csv --iters {iters}"
   ```
   Capture stdout, parse the CSV lines, append rows. On non-zero exit, append to `trend_broken.txt` and continue.
5. **Bisect.** Walk adjacent measured pairs `(A, B)` sorted by date. For each pair, compute the per-benchmark relative delta `|B.per_op_us - A.per_op_us| / A.per_op_us`. If any of the six exceeds the threshold *and* the gap between A and B is still larger than the min-gap, pick the midpoint commit (median commit by date), measure it, then recurse only on whichever sub-pair `(A,M)` or `(M,B)` still trips the threshold. This is the targeted-bisection variant — half the work of always-bisect-both.
6. **Persist.** Write the CSV sorted by date.

### Reused helpers

- `scripts/build-historical.sh` — already does worktree isolation under `.compat-scratch/worktrees/<sha>/` and forwards make args. Don't reinvent.
- `bench_repl --csv` — already emits the 9-column row we want. Don't reformat.

## Piece 2 — Viewer (`tools/bench_trend/bench_trend.c`)

Standalone GLUT binary, single source file. Mirrors `tools/scene_demo/scene_demo.c` for structure: `main()` does `glutInit`, registers display/reshape/keyboard callbacks, enters `glutMainLoop()`.

### Dependencies

Header-only: `src/ui/core/gl_2d.h` (provides `gl2d_begin`, `gl2d_end`, `gl2d_draw_string`). No object deps from `src/` — the viewer needs to be cheap to build because it doesn't share state with the REPL. `glBegin(GL_LINE_STRIP)` / `glVertex2f` / `glEnd` for the plot lines; `gl2d_draw_string` for axis labels. No `src/ui/core/theme.h` — pick six fixed colors locally to keep deps zero.

### Data model (in-memory)

```c
typedef struct {
    char sha[16];
    time_t date;
    double per_op_us[6];  /* indexed by bench_name -> 0..5 */
} TrendSample;

static TrendSample g_samples[MAX_SAMPLES];
static int g_sample_count;
static const char *g_bench_names[6] = {
    "parse_lines", "feed_examples", "flatten_examples",
    "replay_examples", "replay_long", "fade_batches",
};
```

Loaded once at startup from `bench/trend_results.csv` (path overridable via `argv[1]`). Sorted by date ascending.

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
- **Series.** For each visible benchmark, one `GL_LINE_STRIP` connecting the samples in date order, plus a small `GL_POINTS` marker at each sample for sparse series.
- **Color palette.** Six fixed RGB triples chosen for distinguishability (red, orange, yellow, green, cyan, magenta — saturated for visibility on the existing dark UI).

### Controls

| Key | Action |
|-----|--------|
| `t` | Switch X-axis to date |
| `s` | Switch X-axis to chronological SHA index |
| `1`–`6` | Show only the Nth benchmark |
| `0` | Show all six |
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

bench_trend: FORCE $(BINDIR)/bench_trend
	ln -sfn $(BINDIR)/bench_trend $@

# --- sampler runner ----------------------------------------------
# Usage: make bench-trend MIN_GAP_COMMITS=20 INFLECTION_PCT=10
bench-trend:
	python3 scripts/bench_trend_sample.py \
	    $(if $(MIN_GAP_COMMITS),--min-gap-commits $(MIN_GAP_COMMITS)) \
	    $(if $(MIN_GAP_DAYS),--min-gap-days $(MIN_GAP_DAYS)) \
	    $(if $(INFLECTION_PCT),--inflection-pct $(INFLECTION_PCT)) \
	    $(if $(BENCH_ITERS),--iters $(BENCH_ITERS))
```

C99 ratchet: `tools/bench_trend/bench_trend.c` automatically picks up `make check-c99` because the ratchet syntax-checks the `tools/` set. Nothing else to do.

## Critical files

- **New** `scripts/bench_trend_sample.py` — sampler (Python 3 stdlib only).
- **New** `tools/bench_trend/bench_trend.c` — GLUT viewer (single file, ~300–400 lines).
- **Edit** `Makefile` — add `bench_trend` build rule and `bench-trend` runner target (near the existing `scene_demo` / `bench` rules around line 751 and 1244).
- **New (runtime, not committed initially)** `bench/trend_results.csv` — sampler output; safe to gitignore until we want a baseline.
- **New (runtime)** `bench/trend_broken.txt` — broken-SHA blacklist; also gitignored.

Reused without modification: `scripts/build-historical.sh`, `bench/bench_repl.c` (the `--csv` mode is already there), `src/ui/core/gl_2d.h`.

## Verification

End-to-end smoke (on the macOS host that has GL):

1. `make bench_trend` — builds the viewer cleanly under `-std=c99`.
2. `make bench-trend MIN_GAP_COMMITS=50 INFLECTION_PCT=15` — populates `bench/trend_results.csv` for the last ~10–20 commits in a few minutes (each SHA: ~5 s build + ~6 s bench).
3. `./bench_trend` — window opens, six colored line strips visible.
4. Press `t` then `s` — X-axis swaps between dates and SHA indices; the curve shape is preserved.
5. Press `3`, `0` — first isolates `flatten_examples`, then restores all six.
6. Re-run `make bench-trend` with the same args — no new builds (idempotent), CSV unchanged.
7. Re-run with a smaller `MIN_GAP_COMMITS=10` and a synthetic regression commit between two already-sampled SHAs — the bisection picks up the midpoint and the curve gets a new vertex.

Headless check (on gracemont):

```bash
ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
    git pull --ff-only origin main && \
    make check-c99 && \
    python3 scripts/bench_trend_sample.py --min-gap-commits 30 --dry-run'
```

`make check-c99` confirms the new viewer compiles under real GCC; `--dry-run` confirms the sampler walks history without invoking builds.

Unit-test scope (skip for v1):

- Targeted-bisection logic is small enough to verify by inspection on the dry-run output.
- Viewer is non-interactive enough that the smoke walk covers it.
- If the sampler grows (e.g. perf-counter columns from the Tier 2 plan in `plans/done/benchmark-metrics.md`), revisit.
