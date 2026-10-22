## Scope and Overlay Maintainability - one cursor-scope query, one vertex numbering, one outline walk

## Status - NOT STARTED

Written 2026-10-22, after a day of GLU-tessellator overlay fixes. The scope is
deliberately narrow: only the code that work touched - cursor block
resolution, vertex numbering, the outline overlay passes, `bench_repl`, and
the build/doc tooling around them. It is not a review of the whole tree.

The commits that motivate it, in order:

| Commit | What it fixed | Root cause it points at |
|---|---|---|
| `c48e392e` | gutter showed no vertex numbers inside `gluBegin(GLU_POLYGON)` | the gutter's block gate knew `glBegin` only (item 1) |
| `fb2cc2a8` | cursor in a second contour selected no polygon - no highlight, no labels | two block-match scans stopped at the first contour's `gluEnd()` (items 1, 2) |
| `05f2c7a9`, `7a6b03f3` | gutter and scene vertex numbers disagreed | four independent vertex counters (item 3) |
| `3388794c`, `f157bbef` | tess highlight scope, then line-mode polygons | the tess pass diverged from the `glBegin` pass (item 4) |
| `4058f9fa`, `55fc98d6` | highlights lost shared edges to later plain outlines | one walk drawing plain and highlight in program order (item 4) |
| `7091d684`, `30fb6b9b` | plain GLU outlines had their own color and width | same divergence (item 4) |

Most of those were the *same* bug class: one question ("which block is the
cursor in?", "which vertex is this?", "how is this outline drawn?") answered
in several places that drifted apart.

### Summary

| # | Item | Payoff | Size | Depends on |
|---|---|---|---|---|
| 1 | One cursor-block query | removes the drift behind most of the GLU bugs | ~1 day | - |
| 2 | Shared GLU depth helper everywhere, with a guard | stops the contour/polygon `gluEnd()` mix-up recurring | 2-3 h | - |
| 3 | Vertex numbering computed once | gutter, labels, filters cannot disagree | 2-3 days | 1 |
| 4 | Collect outlines once, draw twice | removes three near-copy passes; highlight cost back near plain | 2-3 days | - (measure first) |
| 5 | Doc links stop churning on line numbers | no more anchor-only diffs and gate failures | 0.5 h / 0.5 day | - |
| 6 | Build traps: make version, bench `--only`, single-test target | no stale binaries, no silent no-ops | ~1 h each | - |
| 7 | Test fixtures from real flatten output; named GL enums in traces | fixtures cannot drift from production | ongoing | - |
| 8 | Overlay setup out of `glr_ctrl.c` | smaller controller, fewer `_for_test` seams | ~0.5 day | best after 1 |
| 9 | Habits: present-tense comments, one worktree per session | less comment rot, clean benchmarks | - | - |

Suggested order: **2, 5, 6** first (independent quick wins), then **1**, then
**8**, then **3** and **4** (4 can go any time; benchmark first). Item 7 rides
along with whichever item next touches a test.

---

### 1. One cursor-block query

#### Today

Five places compute "the block around this source line", each with its own
scan:

- `src/app/glr_ctrl.c` - `glr_ctrl_current_begin_block_source_extent()`
  (`glBegin` block, for the code panel snapshot and the overlay walk context)
  and `glr_ctrl_current_tess_block_source_extent()` (GLU polygon).
- `src/repl/flatten_query.c` - `flatten_current_begin_block_source_extent()`
  (feeds the flat current-block cache, single-polygon resolution, overlay
  highlight) and `cursor_tess_contour_source_begin()` (GLU contour).
- `src/ui/app/repl_code_panel.c` - the gutter walk's own begin/end state
  machine (`ReplCodePanelWalkState`).

They already **disagree** on one rule: the controller counts the `glEnd()`
line as inside its block (`edit_line <= i`), the flatten query does not
(`edit_line_idx < i`). Both behaviors are pinned by tests -
`test_vertex_gutter_labels_follow_cursor_begin_block` ("first block vertex
labelled when cursor is on its glEnd") in `tests/test_ui.c`, and "glEnd line
itself is outside the block" in `tests/test_repl_core_internal.c`.

`src/repl/source_scope.c` already owns block structure for the editor
(indentation, block extents, `tess_depth_prefix`), behind a per-document
prefix cache that is invalidated on every source change.

#### Design

Add one query to the source-scope module:

```c
typedef struct {
    int valid;
    int start;   /* head line (glBegin / gluBegin) */
    int end;     /* closing line; doc_count - 1 when unterminated */
} ReplSourceExtent;

typedef struct {
    ReplSourceExtent begin_block;   /* glBegin ... glEnd */
    ReplSourceExtent tess_polygon;  /* gluBegin(GLU_POLYGON) ... matching gluEnd */
    ReplSourceExtent tess_contour;  /* gluBegin(GLU_CONTOUR) ... its gluEnd */
} ReplCursorScope;

void repl_source_scope_view_cursor_scope(const ReplSourceScopeView *view,
                                         int line_idx, ReplCursorScope *out);
```

All three extents in one call, built on the existing prefix caches so a
cursor move is a lookup, not a document scan. Nested cases (a `glBegin`
strip inside a GLU contour, as in the extrusion example) report both.

**Decision to make first:** is the closing line inside its block? Recommend
**inclusive**: a cursor parked on `glEnd()` / `gluEnd()` belongs to the block
it closes, which is what the gutter, the contour resolver, and users already
expect. That changes the flat-cache behavior the core-internal test pins;
update that test deliberately, in the same commit, with the reason.

#### Migration

1. Add the API plus a table-driven unit test: cutout (two contours),
   extrusion (strip nested in a contour), single-contour polygon, a polygon
   inside a `for`, an unterminated block, malformed input (contour outside a
   polygon, nested polygon).
2. Switch the two `glr_ctrl.c` scanners; delete them.
3. Switch the two `flatten_query.c` scanners; delete them.
4. Switch the code panel's gutter gate to the snapshot fields filled from the
   query (`current_begin_block_*`, `current_tess_block_*`).

Acceptance: the full stub gate plus the new test; no other scan of
`CMD_BEGIN`/`CMD_END` or `CMD_TESS_*` pairs for "the block around a line"
remains outside `source_scope.c`.

---

### 2. Shared GLU depth helper everywhere

#### Today

`repl_tess_advance_depth()` in `src/repl/command.h` (promoted from the replay
subsystem in `fb2cc2a8`) is the one correct definition of tessellator
nesting: 0 outside, 1 in a polygon, 2 in a contour, with a contour's
`gluEnd()` returning to 1. It is used by the replay walker, fade and playback,
the outline overlays, and the code panel.

Still hand-rolled:

- `src/repl/source_scope.c` - plain `++`/`--` on any `TESS_BEGIN_*` /
  `TESS_END`. Correct for well-formed input, but disagrees with the helper on
  malformed input (a contour outside a polygon counts as nesting).
- `src/repl/executor.c`, `src/repl/export_cmd_writer.c`,
  `src/subsystems/hidden_lines/hidden_lines.c` - small state machines that
  also issue `gluTessBeginContour`/`gluTessEndPolygon` (or their exported
  text) at each transition. Correct today, but each re-derives the rule.

#### Design

Keep each site's transition actions; derive the depth from the helper:

```c
int prev = depth;
depth = repl_tess_advance_depth(cmd->type, depth);
if (prev == 1 && depth == 2) { /* contour opened  */ }
if (prev == 2 && depth == 1) { /* contour closed  */ }
if (prev == 1 && depth == 0) { /* polygon closed  */ }
```

Then add `check-tess-depth` to the `check-state-ownership` aggregate: fail on
assignments or increments of a variable named `*tess_depth*` outside
`command.h` and `source_scope.c`'s prefix builder (allowlist), the same shape
as the other grep guards.

Acceptance: export trace parity (`tests/test_export_trace_parity.c`) and the
hidden-line tests unchanged; the guard red on a reintroduced `tess_depth++`.

---

### 3. Vertex numbering computed once

#### Today

At least five independent counters assign "the Nth vertex":

- the gutter walk in `src/ui/app/repl_code_panel.c`;
- the replay vertex walker's `vertex_idx_in_block`, `immediate_idx_in_block`
  and `tess_idx_in_polygon` (`src/subsystems/replay/replay.h`);
- `repl_flatten_cursor_polygon()` (glBegin primitive ordinals) and
  `repl_flatten_cursor_tess_contour()` (contour ordinals) in
  `src/repl/flatten_query.c`;
- the tess outline pass's `tess_ord` in `edit_overlays.c`.

`05f2c7a9` and `7a6b03f3` were two rounds of making these agree. The parity
test that now guards it (`glr_ctrl_cursor_block_state_for_test` driven from
`tests/test_ui.c` and `tests/test_edit_overlays.c`) is the right acceptance
check for this item.

#### Design

Number vertices once, when the flat program is built: a parallel array beside
the flat command array (not a new `GLCmd` field - the command struct is large
and `MAX_FLAT_COMMANDS` is 8192), holding for each flat vertex command its
ordinal within its own primitive, counted per family (glVertex within its
`glBegin`, gluVertex within its polygon, continuous across contours).

- A full flatten recomputes it; a value-only rebake cannot change topology,
  so it stays valid.
- The walker, resolvers, label filter and tess pass read it instead of
  counting.
- The gutter maps a source row to its flat instance's ordinal. That also lets
  it show exact numbers for the selected loop instance where it shows `vn`
  today.

Acceptance: the gutter/scene parity test unchanged and green; the counters
listed above deleted.

---

### 4. Collect outlines once, draw them twice

#### Today

`edit_overlays.c` has three near-copy walkers - `render_outlines_glbegin_pass`,
`render_outlines_tess_pass`, `render_outlines_glut_pass` - each re-applying
tracked transforms and replaying clip / cull / stencil / color-mask state.
Since `4058f9fa` each runs in two phases (plain, then highlight) so a
highlight always wins its edges, and selection (`last_matching_block_head`,
`last_selected_glut_shape`) makes further GL-free walks.

Cost, from the `overlay_outlines` rows of `bench_repl` (native macOS, real GL,
us/frame, before -> after `4058f9fa`):

| scene | highlight off | last-instance | all-instances |
|---|---|---|---|
| Wave surface | 23.8 -> 22.7 | 31.3 -> 39.0 | 21.7 -> 37.5 |
| Parametric torus | 39.0 -> 36.1 | 54.1 -> 66.9 | 37.4 -> 67.5 |
| GLU extrusion | 4.6 -> 4.2 | 4.8 -> 5.4 | 4.3 -> 5.4 |

Minor in a 16.7 ms frame, but the duplication is the larger problem: GLU
outlines drifted from GL outlines in primitive, offset, width, color and
highlight scope until five separate commits pulled them back together.

#### Design

One walk records **outline batches**, then two loops draw them:

```c
typedef struct {
    unsigned char kind;        /* glBegin block, tess contour, glut shape */
    GLenum        mode;        /* glBegin mode; GL_POLYGON for a contour */
    int           first, count;/* range in a collected vertex array */
    int           flat_head;   /* for one-instance selection */
    int           contour_first_ord;
    unsigned char is_current;  /* resolved after the walk */
    int           matrix_idx;  /* into a matrix table */
    int           state_idx;   /* into an OverlayGlState table */
} OutlineBatch;
```

- Selection resolves after the walk, from the batch list itself (the last
  matching copy is simply the last matching batch), replacing the extra
  selection walks.
- Draw: every plain batch, then every highlight batch, changing GL state only
  between batches whose `state_idx` differs.

**Risks to settle in a spike first:**

- *Matrices.* Today the walk applies transforms to the live GL stack. Batches
  need the modelview captured CPU-side; do **not** read it back with
  `glGetFloatv` per batch (a sync point under gl4es, see the gl4es WebGL
  stall notes). Track the matrix in C alongside the existing
  `apply_tracked_transform`.
- *Clip planes.* `glClipPlane` equations are transformed by the modelview in
  effect when they are set. A batch's state must carry the eye-space
  equations, or replay them under their own matrix.
- *Storage.* Up to `MAX_FLAT_COMMANDS` batches and vertices; static buffers,
  like the label pass's.

Acceptance: every outline trace test in `tests/test_edit_overlays.c` green
(ordering, culling lift, clip suspend, GL/GLU parity); `overlay_outlines`
highlight rows within ~10% of the plain row.

---

### 5. Doc links stop churning on line numbers

#### Today

About 500 Markdown links carry `#L<n>` anchors. `check-doc-links` verifies
each link's label is on that line; `fix-doc-links` rewrites shifted anchors.
On 2026-10-22, four commits carried anchor-only edits to
`docs/ARCHITECTURE.md`, `CLAUDE.md` and `src/app/README.md` caused by
unrelated header changes, and the gate failed three times on them before the
repair was run by hand.

#### Options

- **A - self-healing gate (~0.5 h).** Have the developer gates
  (`test-stubs`, `test`) run `fix-doc-links` before `check-doc-links`, so the
  repair lands in the commit that caused it. `.githooks/pre-push` runs
  `make test-stubs` too, so gate the repair on a variable (say
  `DOC_LINKS_FIX=1` by default, `0` in the hook): a push must check strictly,
  never rewrite the working tree it is pushing from.
- **B - no committed line numbers (~0.5 day).** Links name the file;
  `check-doc-links` verifies the label resolves to exactly one definition in
  it (the identifier matching in `scripts/link-doc-identifiers.py` already
  does this). Costs GitHub's jump-to-line.

Recommend **A** now; revisit B if anchors keep producing noise.

---

### 6. Build traps

- **GNU make version.** macOS ships GNU make 3.81, whose timestamps are
  whole seconds: a source restored in the same second a build finished is
  treated as up to date, and the stale binary runs (it happened once on
  2026-10-22). Add a Makefile check that warns - or fails, with
  `brew install make` / `gmake` in the message - when `$(MAKE_VERSION)` is
  below 4. Verify that make 4.x on APFS actually compares sub-second mtimes
  before making it a hard error.
- **`make bench ARGS=--only ...`.** `ARGS` goes to every binary in
  `BENCH_BINS`, and `bench_extedit` rejects `--only`, so the target exits 2
  after `bench_repl` succeeded. Teach `bench_extedit` to accept `--only` with
  the same semantics (run the rows named, none if none match).
- **One test binary.** The stub objdir only resolves with `NO_SAN=1`, and the
  hyphenated twins only link. Add `make test-one T=test_edit_overlays`, which
  builds `build/debug-nosan-gl-stubs/$(T)` with `USE_GL_STUBS=1 BUILD=debug
  NO_SAN=1` and runs it; mention it in the CLAUDE.md test section.

---

### 7. Test fixtures

- **Fixtures from real flatten output.** Overlay tests hand-build flat
  command arrays with `mk_cmd`. One existing tess fixture had two contours
  sharing a single `TESS_END`, a shape flatten never produces. Add a helper
  in `tests/support/` that takes source lines and a cursor line, runs
  `glr_ctrl_reset_all` + `editor_feed_line` + `repl_flatten_commands`, and
  fills an `OverlayWalkCtx` through `glr_ctrl_fill_overlay_walk_ctx`. Default
  to it; keep hand-built arrays for shapes the parser cannot produce.
- **Named enums in traces.** Expectations such as `"glBegin 9"` and
  `"glEnable 2884"` need a lookup to read. A small formatting helper
  (`trace_fmt(buf, "glBegin %u", GL_POLYGON)`) keeps the stub's numeric
  output and makes the test say `GL_POLYGON`.

---

### 8. Overlay setup out of `glr_ctrl.c`

`src/app/glr_ctrl.c` is ~5,600 lines. Its overlay setup - the cursor block
fill, `glr_ctrl_fill_overlay_walk_ctx()`, the overlay pack builder, and (until
item 1 deletes them) the extent scanners - is a self-contained unit that
tests and `bench_repl` now call directly. Move it to its own file under
`src/app/` and drop `glr_ctrl_cursor_block_state_for_test` in favor of the
public fill.

Check before moving: the Makefile source lists (some lists are hand-maintained,
e.g. `REPL_DEMO_DEP_SRCS`), and the `check-app-boot-band` /
`check-state-ownership` guards. Do it after item 1 so less code moves.

---

### 9. Habits

- **Present-tense comments.** Comments that narrate history ("it used to
  run first...") rot the moment the next change lands. CLAUDE.md already
  says root docs describe the current design, with history in commits and
  `docs/plans/`; extend that to code comments in `docs/CONTRIBUTING.md`.
  Clean-up candidates from 2026-10-22: the comment on
  `render_single_polygon_highlight` and the `OutlinePhase` comment in
  `edit_overlays.c`, and the Bold-style test comment in
  `tests/test_edit_overlays.c`.
- **One worktree per parallel session.** On 2026-10-22 a forked session
  committed to the same `main` checkout between this work's commits. Nothing
  broke, but benchmark comparisons and gate runs assume nobody else is
  rebuilding the tree. `git worktree add ../gl-repl-<topic> -b <topic>` per
  session, integrated with `push HEAD:main` as the worktree notes describe.

### Out of scope

Anything this work did not touch: the language pipeline beyond the
flatten queries, the UI beyond the code panel gutter, audio, the web build's
draw path. Those may well have similar duplication; this plan makes no claim
either way.
