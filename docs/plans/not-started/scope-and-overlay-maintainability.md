## Scope and Overlay Maintainability - one cursor-scope query, one vertex numbering, one outline walk

## Status - NOT STARTED (item 10, the written spec, is done)

No runtime item has started, so the plan stays in `not-started/`. Item 10
completed 2026-10-22: the
[overlay scope contract](../../ARCHITECTURE.md#overlay-scope-contract) defines
the four scopes, primitive-specific rules, numbering, and current gaps;
[the user guide](../../USER_GUIDE.md#cursor-guides--vertex-overlays) summarizes
the behavior. Closing lines are inclusive; a nested `glEnd()` must preserve
the enclosing GLU context. The runtime changes remain in item 1.

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

Reviewed the same day by the session that made `c48e392e`, `fb2cc2a8`,
`05f2c7a9`, `7a6b03f3` and `7568f1aa`. Its corrections are folded in below:
item 3's display rule, the flat-side matchers and walker quirk in items 1
and 3, items 10 and 11, and additions to items 6 and 7.

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
| 10 | Written spec: overlay scope per primitive kind - DONE | one source of truth for items 1, 3, 4 and their tests | 1-2 h | - |
| 11 | Adjacent: workspace manifest forward compatibility | new builds' workspaces stay readable by older builds | ~1 h | - |

Suggested order: **10, 2, 5, 6** first (10 is the spec items 1, 3 and 4 are
checked against; the rest are independent quick wins), then **1**, then
**8**, then **3** and **4** (4 can go any time; benchmark first). Item 7 rides
along with whichever item next touches a test; item 11 is independent.

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

Two more answer the same question on the **flat** side - is this flat block or
polygon the cursor's? - and are near-duplicates of each other:
`outline_block_matches_cursor()` in `edit_overlays.c` and
`replay_walk_block_matches_cursor()` in `src/subsystems/replay/replay.c`. Both
had the same contour-`gluEnd()` bug fixed in `fb2cc2a8`, and both now also
accept a polygon whose source extent holds the cursor
(`repl_cursor_source_tess_head_matches()` in `src/repl/flatten.h`, from
`05f2c7a9`), because a comment line between contours reaches no flat command.

The source-side scanners already **disagree** on one rule: the controller counts the `glEnd()`
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

**Decisions recorded by item 10** in the
[overlay scope contract](../../ARCHITECTURE.md#overlay-scope-contract):

*Is the closing line inside its block?* **Inclusive**: a cursor parked on
`glEnd()` / `gluEnd()` belongs to the block
it closes, which is what the gutter, the contour resolver, and users already
expect. That changes the flat-cache behavior the core-internal test pins;
update that test deliberately, in the same commit, with the reason.

*What does a `glEnd()` inside a GLU contour end?* The replay walker resets
`tess_depth` and `in_block` at `CMD_BEGIN` and `CMD_END`. In a strip nested in
a contour (the extrusion example), any `gluVertex` after the strip's
`glEnd()` is therefore unlabelled, and the enclosing polygon's own close is
never seen. The gutter copies this on purpose for parity (`05f2c7a9`:
"nothing after a glEnd is labelled until the next head"). This is an artifact:
`glEnd()` closes only the strip and must restore the enclosing GLU context.
Fix the walker and gutter in one commit, or the gutter/scene parity test fails.

#### Migration

1. Add the API plus a table-driven unit test: cutout (two contours),
   extrusion (strip nested in a contour), single-contour polygon, a polygon
   inside a `for`, an unterminated block, malformed input (contour outside a
   polygon, nested polygon).
2. Switch the two `glr_ctrl.c` scanners; delete them.
3. Switch the two `flatten_query.c` scanners; delete them.
4. Switch the code panel's gutter gate to the snapshot fields filled from the
   query (`current_begin_block_*`, `current_tess_block_*`).
5. Fold the two flat-side matchers into one helper next to the query. Keep
   the rule that a cursor on any source line inside a block's extent -
   comment lines included - selects that block, as the `glBegin` source
   extent and the tess-head match do today.

Acceptance: the full stub gate plus the new test; no other scan of
`CMD_BEGIN`/`CMD_END` or `CMD_TESS_*` pairs for "the block around a line"
remains outside `source_scope.c`.

---

### 2. Shared GLU depth helper everywhere - DONE

Source depth, executor/export transitions, hidden-line cleanup, and replay
polygon entries use `repl_tess_advance_depth()`. Transition actions and
caller-owned polygons in contour-only functions are preserved. Regression
coverage includes malformed source rows, polygon restarts, suppressed cursor
cleanup, multi-contour hidden-line passes, and contour-function export parity.

`check-tess-depth` runs in `check-state-ownership`. It permits helper calls,
zero initialization, and exact cache copies/lifecycle resets; it rejects
manual depth arithmetic even in files with an allowed copy or reset. The
walker's immediate-block resets remain item 1's work.

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
assignments or increments of a production variable named `*tess_depth*` that
bypass the helper. Allow zero initialization and exact cache copies/lifecycle
resets, rather than exempting whole files or the source prefix builder.

Acceptance: export trace parity (`tests/test_export_trace_parity.c`) and the
hidden-line tests unchanged; the guard red on a reintroduced `tess_depth++`.

---

### 3. Vertex numbering computed once

#### Today

At least five independent counters assign "the Nth vertex":

- the gutter walk in `src/ui/app/repl_code_panel.c`, which since `05f2c7a9`
  replays the walker's numbering rule;
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

#### The displayed number is the shared run - a user decision

**Do not "simplify" display numbers to per-family counts.** `c48e392e` did
exactly that in the gutter (glVertex and gluVertex in separate runs), and the
user rejected it: the gutter must show the scene's number, so that every
vertex in a block is unique. `05f2c7a9` reverted the gutter to that rule.

The displayed number is the replay walker's `vertex_idx_in_block`: **one run
per block, counting glVertex and gluVertex together, restarted by the
innermost head** (`glBegin` or `gluBegin(GLU_POLYGON)`). A `glBegin` strip
nested in a contour owns the contour's `gluVertex` rows.

Per-family ordinals exist too, but only for single-polygon **filtering**:
`immediate_idx_in_block` (glVertex-only) against the cursor quad's range, and
`tess_idx_in_polygon` (gluVertex-only, continuous across contours) against
the cursor contour's range.

#### Design

Number vertices once, when the flat program is built: a parallel array beside
the flat command array (not a new `GLCmd` field - the command struct is large
and `MAX_FLAT_COMMANDS` is 8192), holding **three ordinals per flat vertex
command**:

| ordinal | counts | used for |
|---|---|---|
| shared run | both families, per innermost head | **display**: gutter and scene labels |
| immediate | glVertex only, per `glBegin` block | single-polygon filter against the cursor primitive |
| tess | gluVertex only, per polygon, across contours | single-polygon filter against the cursor contour; contour highlight |

- A full flatten recomputes it; a value-only rebake cannot change topology,
  so it stays valid.
- The walker, resolvers, label filter and tess pass read it instead of
  counting.
- The gutter maps a source row to its flat instance's **shared-run** ordinal.
  That also lets it show exact numbers for the selected loop instance where
  it shows `vn` today.
- Whatever item 1 decides about a `glEnd()` inside a contour is applied here,
  in the one place the shared run is computed.

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
- **A red guard hides the test results.** The `test-stubs` check gate stops
  at the first red guard, before the tests and before the later guards. A
  doc-anchor shift from a header edit (item 5) is enough to hide every test
  result in the run. Unless option A from item 5 makes that guard heal
  itself, change the gate to run every guard, run the tests anyway, and
  report all the red guards at the end.
- **One test binary.** The stub objdir only resolves with `NO_SAN=1`, and the
  hyphenated twins only link. Add `make test-one T=test_edit_overlays`, which
  builds `build/debug-nosan-gl-stubs/$(T)` with `USE_GL_STUBS=1 BUILD=debug
  NO_SAN=1` and runs it; mention it in the CLAUDE.md test section.

---

### 7. Test policy and fixtures

- **UX behavior gets an invariant sweep over real scenes.** Hand-picked
  assertions can lock in a design choice as if it were a requirement:
  `05f2c7a9`'s nested-strip tests asserted "gluVertex hidden", which was the
  bug the user reported next (fixed in `7a6b03f3`). The tests that actually
  guarded behavior were sweeps over the real GLU catalog examples - every
  cursor line's gutter label equals the scene label (`tests/test_ui.c`), and
  the cursor's own vertex always stays in single-polygon scope
  (`test_single_polygon_keeps_cursor_vertex` in
  `tests/test_edit_overlays.c`). Make that the rule: a UX behavior gets an
  invariant sweep over catalog scenes, and example assertions only add to it.
- **Widen the gutter/scene parity sweep to all four scopes.** It runs in the
  default scope only (last-instance).
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

`src/app/glr_ctrl.c` is ~5,600 lines. Its overlay setup -
`glr_ctrl_fill_cursor_block_state()` (static) and its test seam
`glr_ctrl_cursor_block_state_for_test()`, `glr_ctrl_fill_overlay_walk_ctx()`
(public, used by `bench_repl`), the overlay pack builder, and (until
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

---

### 10. Written spec: overlay scope per primitive kind - DONE

Implemented in the [overlay scope contract](../../ARCHITECTURE.md#overlay-scope-contract)
and [user guide](../../USER_GUIDE.md#cursor-guides--vertex-overlays). These are
the source of truth for the remaining refactors; the context below records
why the spec was needed.

The single-polygon rules grew piecemeal across `fb2cc2a8`, `3388794c`,
`05f2c7a9` and `7a6b03f3`, and live only in code and test names. Today:

- a glVertex is narrowed to the cursor's GL primitive, compared in
  glVertex-only ordinals;
- a gluVertex is narrowed to the cursor's contour, compared in gluVertex-only
  ordinals - including one inside a nested strip;
- a cursor in no contour (the polygon's own begin/end line, between
  contours) keeps the whole polygon;
- the cursor's own vertex always stays in scope;
- labels and normal arrows share one predicate,
  `single_polygon_keeps_vertex()` in `edit_overlays.c`; the outline
  highlight follows the contour.

Write it down once - a table in `docs/ARCHITECTURE.md` (implementation
facts) with the user-facing summary in `docs/USER_GUIDE.md` - covering
`glBegin` blocks, tess polygons and contours, a strip nested in a contour,
and `glutSolid*` shapes, across all four scopes (last instance, all
instances, whole scene, single polygon) and all three consumers (labels,
normal arrows, outline highlight). Record item 1's two decisions there. Items
1, 3 and 4 are then checked against this table, and item 7's sweeps assert
it.

---

### 11. Adjacent: workspace manifest forward compatibility

Outside this plan's cursor/overlay scope, but found the same day
(`7568f1aa`, which added the workspace's active scene). The `.glr-workspace`
reader in `src/repl/workspace_io.c` rejects unknown fields ("Workspace
manifest has an unknown field"). So every new manifest field makes a
workspace saved by a newer build unreadable by an older one - `active=`
already did.

Pick one policy and write it next to the reader:

- **Ignore unknown keys** (warn once). Older builds load what they understand
  and drop the rest. That suits optional metadata such as the active scene.
- **Explicit version bump.** A `version=` line, with older builds refusing
  only newer *major* versions. That suits a field whose absence would change
  what loads.

Recommend ignoring unknown keys, plus a version line reserved for a future
breaking change. Add a round-trip test with an extra unknown field.

### Out of scope

Anything this work did not touch: the language pipeline beyond the
flatten queries, the UI beyond the code panel gutter, audio, the web build's
draw path. Those may well have similar duplication; this plan makes no claim
either way.
