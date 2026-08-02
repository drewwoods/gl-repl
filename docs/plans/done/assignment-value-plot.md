# Assignment value plot (right-click a `var = expr;` row)

**Status: complete** (2026-07-28) - implemented, tested, documented; landed in
`7580a9a8`. Archived here for the design rationale; the as-built result and the
deltas from the approved plan are in the "Implementation log" at the bottom.

## Context

Debugging a REPL scene today gives you *one* number per assignment: the replay
annotation (`// x = ... = 3.7`) shows the most recent execution at or before
`replay.pc`, and only while replay is active with `expand_args`. There is no way
to see the *shape* of a value - how `x` sweeps across the 200 iterations of a
loop this frame, or how it drifts frame to frame.

This adds a floating plot panel: right-click an assignment row, get its values
plotted, with min / max / mean / stddev. The intent is a feel for the numbers,
not sample-accurate capture. It is a debugging surface, so it must cost nothing
when closed.

**The key enabling fact** (from exploration): flatten already materializes every
execution instance of an assignment as its own flat command with the evaluated
value baked into `args[0]` (`src/repl/flatten.c:1169-1182`; the executor only
applies it, `src/repl/executor.c:887-908`). So capture is a **read-only scan of
the existing flat program** - no executor hook, no new bookkeeping, and
literally zero cost when the panel is closed.

## Decisions (settled with the user)

- **X axis**: execution index within the captured frame. When the row executes
  ≤1× per frame the panel switches X to *captured frames* (a time series), with
  the axis relabelled. Mode flips clear the buffer.
- **One plot at a time.** Right-clicking another assignment retargets it;
  right-clicking the same row again closes it (mirrors the GL-state inspector
  toggle at `src/app/glr_ctrl_router.c:1341-1345`).
- **Capture rate** - Once / 1 Hz / Every frame - is a **mouse-only chip in the
  panel header**. No `GlrConfigKey`, no keymap slot, **no example-golden churn**.
- **Match on `CMD_VAR_ASSIGN` / `CMD_SCRATCH_ASSIGN` only** (per the user).
  Globals, function-scoped locals and `A[i] = …` all work; the value lives in
  `args[0]` for var assigns and `args[2]` for scratch assigns.
- Panel is roughly FPS-panel sized (250 px wide) and joins the existing
  auto-stacked overlay column - not an anchored popup, so it never occludes the
  code you are debugging.

## New modules

### 1. `src/support/runstats.{c,h}` - neutral running statistics

`Histogram` cannot be reused: its bins are log-spaced over `[1 us, 1 s]` and
positive-only, while assignment values are signed and arbitrary. But its *stats*
half is exactly what we need, so extract it:

```c
typedef struct { unsigned long long count; double min, max, sum, mean, m2; } RunStats;
typedef struct { unsigned long long count; double min, max, sum, mean, variance, stddev; } RunStatsSummary;

void runstats_record(RunStats *s, double value);
void runstats_clear(RunStats *s);
void runstats_read(const RunStats *s, RunStatsSummary *out);
```

Body is the Welford update lifted verbatim from `src/support/histogram.c:57-65`
and the n-1 read from `:88-102` (keep the `m2 > 0.0` guard and the comments).
Then **`histogram.c` delegates**: replace `Histogram`'s
`count/min_us/max_us/sum/mean/m2` fields with a single `RunStats stats;` and have
`histogram_record` / `_clear` / `_read_stats` call through. Those fields are
documented private (`histogram.h:43-47`) and only `histogram.c` touches them, so
this is mechanical and fully covered by the existing histogram tests in
`tests/test_ui_cpuprof.c`.

### 2. `src/subsystems/assign_plot/assign_plot.{c,h}` - capture engine

Peer subsystem alongside `replay/`, `variable_panel/` (per `docs/MODULES.md`),
with an `assign_plot_state_view()` accessor per the repo's facade convention.
Owns:

```c
typedef enum { ASSIGN_PLOT_RATE_ONCE=0, ASSIGN_PLOT_RATE_1HZ, ASSIGN_PLOT_RATE_FRAME,
               ASSIGN_PLOT_RATE_COUNT } AssignPlotRate;
typedef enum { ASSIGN_PLOT_X_EXEC=0, ASSIGN_PLOT_X_FRAME } AssignPlotXMode;

#define ASSIGN_PLOT_COLS 192          /* ~1 column per plot pixel */
typedef struct { float lo, hi; } AssignPlotColumn;   /* min/max envelope */
```

- `assign_plot_open(int line_idx)` / `_close()` / `_toggle(line_idx)` - stores
  `line_idx` plus the row's LHS token (from the editor buffer text, which is
  where per-line canonical text lives - *not* on `GLCmd`). The LHS token doubles
  as the panel title and as the drift check.
- `assign_plot_set_rate(AssignPlotRate)` / `_cycle_rate(+1|-1)` - resets stats
  and buffer (a rate change redefines the window).
- `assign_plot_capture(double now_us)` - **the whole hot path**:
  1. `if (!g_open) return;` ← the zero-cost gate.
  2. Rate gate: ONCE → only if not yet captured; 1HZ → only if
     `now_us - last >= 1e6`; FRAME → always. Clock is a *parameter*, so the
     module is deterministic under test with no `prof_test_*`-style hook.
  3. Validate the target: `repl_state_document_cmd_at(line_idx)` must still be
     `CMD_VAR_ASSIGN`/`CMD_SCRATCH_ASSIGN` with the same LHS token, else close.
  4. Single pass over `repl_state_flat_program_cmds()[0 ..
     repl_state_flat_program_count())` - deliberately the *full* count, not
     `replay_exec_limit()`, so the plot shows the whole frame during replay -
     collecting `args[0]` (or `args[2]`) wherever
     `src_cmd_idx == line_idx && type` matches.
  5. `N >= 2` → X_EXEC: clear columns, fold the N values into
     `ASSIGN_PLOT_COLS` min/max columns (column `c` covers
     `[c*N/COLS, (c+1)*N/COLS)`); `N <= 1` → X_FRAME: append one column
     (`lo == hi`) to a ring. Mode change clears the buffer + stats.
  6. Feed **every** sample to `runstats_record` - decimation affects only the
     plot, never the statistics. This is what makes min/max/mean/stddev honest
     while the curve stays impressionistic.

Cost when open is one O(flat_count) pass - same order as one render walk, and
only once a second at the default rate.

### 3. `src/ui/support/assign_plot.{c,h}` - the panel renderer

Sits beside `src/ui/support/cpuprof.c` and links from `{support, ui/support,
ui/core}` only. Pure over a flat view (`check-views-flat`): title string,
`const AssignPlotColumn *cols`, `count`, `RunStatsSummary`, rate, x-mode,
`panel_x/y`, `window_w/h`, `pointer_x/y`.

Geometry, modelled on `ui_fps_panel_render` (`src/ui/support/cpuprof.c:647-770`)
and the histogram panel's axis/tick work (`:1246-1571`):

```
ASSIGN_PLOT_PANEL_W   250      (matches FPS + variable panel column width)
ASSIGN_PLOT_HEADER_H   20      title (fit_label'd) + [x]
ASSIGN_PLOT_CTRL_H     16      [rate: 1 Hz]   [reset]
ASSIGN_PLOT_PLOT_H     90
ASSIGN_PLOT_GUTTER     40      y labels are signed %.3g, wider than FPS's 28
ASSIGN_PLOT_XAXIS_H    12      "exec # (n=240)" | "frames"
ASSIGN_PLOT_STATS_H    26      2 rows x 2 key/value pairs
ASSIGN_PLOT_BOTTOM_PAD  8
```

- Reuse `gl2d_panel_frame` / `gl2d_draw_string` / `ui_clr_a` / `fit_label` /
  `ui_pixel_center`, and the "sunken plot well" idiom (a second `panel_frame` at
  `UI_TOK_SUNKEN 0.4` / `UI_TOK_BORDER 0.6`).
- Y scale: min/max over plotted columns, 5 % pad, rounded outward to a
  1/2/5×10^k step; 3-4 gridlines, right-aligned labels in the gutter; a brighter
  rule at y=0 when 0 is in range. **No forced zero baseline** - a value living in
  `[100, 101]` must still show its shape.
- Draw: `GL_QUADS` envelope band (`lo`→`hi`, additive, alpha 0.30) then a
  `GL_LINE_STRIP` through column midpoints (alpha 0.85). When `N <= COLS` every
  column has `lo == hi`, so the band collapses and the strip is exact - one code
  path for both X modes.
- Stats rows: `min` / `max` on row 1, `mean` / `sd` on row 2, formatted `%.4g`,
  plus `n=<count>`. Empty states `(collecting)` / `(not executed this frame)` in
  `UI_TOK_TEXT_PLACEHOLDER`, same early-return shape as the FPS panel.
- `ui_assign_plot_panel_hit_test()` mirrors the header/control geometry
  row-for-row and returns `UI_HIT_ASSIGN_PLOT_CLOSE` / `_RATE` / `_RESET`,
  declared as fixed offsets off `UI_HIT_CORE_COUNT` in the reserved subsystem
  range (the convention in `src/ui/subsystems/variable_panel.h:24-27`).

## Wiring

| Seam | Change |
|---|---|
| `src/ui/app/overlay_layout.h:28-35` | new `UI_OVERLAY_PANEL_ASSIGN_PLOT` id |
| `src/ui/app/overlay_layout.c:44-84`, `:102-113` | size request from `ui_assign_plot_panel_width()/_height()`; slot in the stack order (place next to `UI_OVERLAY_PANEL_VARIABLE`) - plus an `assign_plot_visible` field on `UiOverlayLayoutInputs` |
| `src/ui/app/state_types.h` | `UiAssignPlotState { visible; source_line_idx; }` beside `UiCommandDescriptionState:79-84` - chrome only; buffer + stats live in the subsystem |
| `src/app/glr_ctrl_router.c:1277-1306` | in `route_right_code_panel_hit`, **before** the description lookup: if the row's `GLCmd` is `CMD_VAR_ASSIGN`/`CMD_SCRATCH_ASSIGN` → `assign_plot_toggle(hit->line_idx)`, close the other two popups, `editor_request_redraw()`. Today this case falls into the inert `else` (there are no description records for either type), so nothing regresses. |
| `src/app/glr_ctrl_router.c:~1640`, `:1746` | route + dispatch the three new `UI_HIT_ASSIGN_PLOT_*` kinds |
| `src/ui/app/panels.c:702-793` | add the panel to `k_reverse_render_order` and the hit-test walk (so clicks land, and stray clicks inside the panel consume as `UI_HIT_OVERLAY_CHROME` rather than leaking to the scene) |
| `src/app/glr_ctrl.c:2269` | `assign_plot_capture(now_us)` immediately after `repl_refresh_flat_program(...)`, wrapped in `prof_begin/end(PROF_ASSIGN_PLOT_CAPTURE)` |
| `src/app/glr_ctrl.c:~2081`, `:2504-2521` | `glr_ctrl_build_assign_plot_view()` next to the other panel view builders; render call in the overlay block under `PROF_ASSIGN_PLOT` |
| `prof_sections.h`, `src/app/glr_prof.c:92` | two sections at **depth ≥ 1** (`PROF_ASSIGN_PLOT_CAPTURE` under the frame band, `PROF_ASSIGN_PLOT` under `PROF_UI_PANELS`) so no new histogram legend series appears and the profile panel's height is unchanged. Current count is 44 against the `<= 64` static assert - room is fine. |
| `Makefile` | `src/support/*.c` and `src/ui/support/*.c` are wildcarded, but the curated lists need the new files: `STATE_NEUTRAL_SRCS` (`:499`) for `runstats.c`, and `CPUPROF_DEMO_DEP_SRCS` (`:629`) + its `_OBJS` twin (`:1418`) since `histogram.c` now pulls in `runstats.c` - otherwise `check-cpuprof-demo-isolation` breaks |

Deliberately **not** touched: `GlrConfigKey`, `g_cfg_items[]`, `keymap.h`,
`glr_defaults.h`, and the 39 example goldens. Mouse-only controls keep this
entirely out of the config/`@cfg` surface.

## Tests

- **`tests/test_assign_plot.c`** (core test - automatic via `CORE_TEST_BINS`):
  rate gating with an injected clock; envelope decimation (N ≫ COLS preserves
  true per-column min/max); X_FRAME ring wrap; mode flip clears; stats correct
  over **signed** values and independent of decimation; target-drift closes the
  panel; `assign_plot_capture` is a no-op when closed.
- **`tests/test_ui_assign_plot.c`** (GL-stub test, explicit `_OBJS` list and
  `filter-out` from `CORE_TEST_BINS` - copy the `test_ui_cpuprof` block at
  `Makefile:805`, `:811`, `:995-1004`): `#ifdef GL_STUBS` wrapper, `gl_stub_counts`
  assertions that the panel draws, hit-test geometry agrees with the render
  geometry for all three controls, empty-state paths.
- Extend `tests/test_overlay_layout.c` (new slot appears/disappears, no overlap)
  and `tests/test_ui_panels.c` (reverse-order routing).
- Extend `tests/test_glr_ctrl.c` (already has right-button coverage at
  `:1236-1541`): right-click an assignment row opens the panel; right-click again
  closes; right-click a `glVertex3f` row still opens nothing.
- Histogram delegation is regression-covered by the existing
  `tests/test_ui_cpuprof.c` stats tests.

## Verification

```bash
make check-c99 && make check-include-style
make test-stubs                      # ASan+UBSan, no GL libs
make test
make check-state-ownership           # incl. check-views-flat, check-cpuprof-demo-isolation
make gl-repl && ./gl-repl --example torus
```

Manual: load an example with a loop (`--example torus`), right-click the loop's
`var = …` row → panel appears in the overlay column showing the in-frame sweep;
click the rate chip through Once / 1 Hz / Every frame (right-click cycles back);
right-click a top-level `angle = t*30;` row → the same panel retargets and the X
axis relabels to `frames`.

For a deterministic headless check, add a small `GLR_OPEN_ASSIGN_PLOT=<line>`
env hook next to `glr_ctrl_open_gl_state_popup()` (`src/app/glr_ctrl.c:3655-3678`),
which resolves a source line to a pixel and issues a real right-button
down/up pair - then capture with the OSMesa build per the `gl-repl-capture`
skill. The pointer-script `rightclick` verb
(`src/app/glr_pointer_script.c:851`) covers the scripted-video path.

## Docs

`docs/MODULES.md` (three new module rows), `docs/USER_GUIDE.md` (right-click an
assignment; the rate chip), `docs/ARCHITECTURE.md` (capture model + why no
executor hook). **One line at most in `CLAUDE.md`** - it is the compact brief and
must stay that way.

## Known limitations (call out, don't fix now)

- Executions are plotted in flat order with no call-context split: a row inside
  `func0()` called from three sites shows all three runs concatenated. The
  provenance to separate them (`func_scope_mask`, `call_depth`,
  `call_src_cmd_idx`) is already on every flat command, so this is a later
  filter chip, not a redesign.
- `source_line_idx` shifts if rows are inserted above the target. The drift check
  closes the panel rather than silently plotting the wrong row - same tradeoff
  the GL-state inspector already makes.
- While paused, "Every frame" in X_FRAME mode appends identical values (a flat
  line). Honest, if uninteresting.

## Implementation log

All five steps landed, on top of `codex/prof-section-set`. `make test-stubs`
(ASan + UBSan): **76/76 binaries, 24714/24714 tests**. `make check`: **green,
77 checks**, including `check-views-flat`, `check-cpuprof-demo-isolation`,
`check-prof-sections-instrumented` (69 catalog rows) and `check-doc-links`.
Cocoa and OSMesa `make gl-repl` both clean, no new warnings.

| Step | Notes |
|---|---|
| `src/support/runstats.{c,h}` + `histogram.c` delegation | `Histogram` now holds one `RunStats`; record/clear/read delegate. Makefile curated lists updated (`STATE_NEUTRAL_SRCS`, four demo dep lists, four explicit `test_*_OBJS`). |
| `src/subsystems/assign_plot/assign_plot.{c,h}` | Capture engine. `src/subsystems/*/*.c` is wildcarded, so no Makefile change for the main binary. |
| `src/ui/support/assign_plot.{c,h}` | 250px panel beside the cpuprof panels. |
| Wiring | overlay-layout slot, snapshot fields, right-click branch, three hit kinds, controller capture + view + render, `GLR_OPEN_ASSIGN_PLOT` capture hook. |
| Tests | `tests/test_assign_plot.c` (78 assertions), `tests/test_ui_assign_plot.c` (38), plus new cases in `test_overlay_layout.c` and `test_glr_ctrl.c`. |
| Docs | `docs/MODULES.md`, `docs/ARCHITECTURE.md` ("Assignment Value Plot"), `docs/USER_GUIDE.md`, `docs/ADVANCED_USAGE.md`, one section in `CLAUDE.md`. |
| Screenshots | Two `scripts/docs-assets.sh` assets - `assign-plot` (exec-index axis) and `assign-plot-frames` (captures axis) - embedded in the user guide. Both outlast the splash, which dims the bottom-right corner the panel occupies; the frames one is wall-clock bound, so its `n=` varies by machine. |

### Deltas from the plan as approved

**1. Prof sections: dropped, then restored on a rebase.** The catalog was at
exactly 64 sections, the ceiling a `uint64_t` section mask enforced, so the
first pass shipped without them. This branch is rebased onto
`codex/prof-section-set`, whose multiword `ProfSectionSet` lifts that ceiling,
and the three sections are back: `PROF_ASSIGN_PLOT` (accumulated across both
phases via `prof_accum_*`, since the scan and the draw sit at opposite ends of
the frame) with `_CAPTURE` and `_PANEL` as nested leaves. The reset and the
commit are both gated on the panel being open and must stay that way -
committing an un-reset accumulator would pin the row at the previous plot's
time. `Assign Plot` now also appears as a depth-0 histogram series.

**2. No stored LHS token, and no `UiAssignPlotState`.** The plan had
`assign_plot_open()` capture the row's LHS text as title + drift check. That
would have made the peer depend on `src/editor` (`editor_buffer_line()`). The
subsystem now stores only `source_line_idx`; the controller re-derives the
title each frame into the snapshot. Side benefit: editing the row retitles the
plot instead of stranding a stale label. Consequently the drift check is
"still exists and still an assignment", not "still the same assignment" - and
the separate `UiAssignPlotState` chrome struct the plan called for is
redundant, since the subsystem already owns open/target.

**3. Two layout defects found only by rendering it.** Headless OSMesa capture
(`GLR_OPEN_ASSIGN_PLOT` + `FREEGLUT_CAPTURE_FRAMES`) showed the bottom Y label
colliding with the x-axis caption and the top one clipped by the control row;
gutter labels are now clamped into the plot band. The sample count also
overlapped the caption, so that row is now laid out count-first with the
caption fitted into what remains. Neither was visible from the unit tests -
worth remembering that this panel needs a look, not just a green suite.

**4. `GLR_OPEN_ASSIGN_PLOT` capture hook added.** Sibling of
`GLR_OPEN_GL_STATE`, routing a real synthetic right-click. This is what made
the two defects above findable without a window.

### Follow-ups deliberately not done

- Executions are plotted in flat order with no call-context split: a row inside
  `func0()` called from three sites shows all three runs concatenated. The
  provenance to separate them (`func_scope_mask`, `call_depth`,
  `call_src_cmd_idx`) is already on every flat command - a later filter chip,
  not a redesign.
- `source_line_idx` shifts if rows are inserted above the target; the plot
  follows the index, same tradeoff the GL-state inspector already makes.
- One plot at a time, as specified.
