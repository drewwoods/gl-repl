## Scope and Overlay Maintainability - one cursor-scope query, one vertex numbering, one outline walk

## Status - PARTIALLY IMPLEMENTED (items 1, 2, 4, 5, 6, 7, 8 and 10 are done)

Items 1, 2, and 4 implement the shared cursor-scope and outline paths.
Item 10's [overlay scope contract](../../ARCHITECTURE.md#overlay-scope-contract)
defines the four scopes, primitive-specific rules, and numbering;
[the user guide](../../USER_GUIDE.md#cursor-guides--vertex-overlays) summarizes
the behavior. Closing lines are inclusive, and a nested `glEnd()` restores
its enclosing GLU context. Remaining items are listed below.

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
| 1 | One cursor-block query - DONE | removes the drift behind most of the GLU bugs | ~1 day | - |
| 2 | Shared GLU depth helper everywhere, with a guard | stops the contour/polygon `gluEnd()` mix-up recurring | 2-3 h | - |
| 3 | Vertex numbering computed once | gutter, labels, filters cannot disagree | 2-3 days | 1 |
| 4 | Collect outlines once, draw twice - DONE | removes three near-copy passes; measured costs below | 2-3 days | - (measure first) |
| 5 | Doc links stop churning on line numbers - DONE | no more anchor-only diffs and gate failures | 0.5 h / 0.5 day | - |
| 6 | Build traps: make version, bench `--only`, single-test target - DONE | no stale binaries, no silent no-ops | ~1 h each | - |
| 7 | Test fixtures from real flatten output; named GL enums in traces - DONE | fixtures cannot drift from production | ongoing | - |
| 8 | Overlay setup out of `glr_ctrl.c` - DONE | smaller controller, fewer `_for_test` seams | ~0.5 day | best after 1 |
| 9 | Habits: present-tense comments, one worktree per session | less comment rot, clean benchmarks | - | - |
| 10 | Written spec: overlay scope per primitive kind - DONE | one source of truth for items 1, 3, 4 and their tests | 1-2 h | - |
| 11 | Adjacent: workspace manifest forward compatibility | new builds' workspaces stay readable by older builds | ~1 h | - |

Suggested order: **10, 2, 5, 6** first (10 is the spec items 1, 3 and 4 are
checked against; the rest are independent quick wins), then **1**, then
**8**, then **3** and **4** (4 can go any time; benchmark first). Item 7 rides
along with whichever item next touches a test; item 11 is independent.

---

### 1. One cursor-block query - DONE

#### Previous implementation

Five places computed "the block around this source line", each with its own
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

#### Migration (implemented)

1. **Source-scope API and unit tests (`src/repl/source_scope.h`, `src/repl/source_scope.c`, `tests/test_repl_core_internal.c`).**
   - Declare `ReplSourceExtent`, `ReplCursorScope`, `repl_source_scope_view_cursor_scope()`, and the live-document wrapper `repl_source_scope_cursor_scope(int line_idx, ReplCursorScope *out)`. Invalid extents initialize to `{ 0, -1, -1 }` so callers that copy `start`/`end` unconditionally keep `-1` when out of scope.
   - Implement all three extents with one helper over the existing prefix arrays in `ReplSourceScopeView` (no new `MAX_EDITOR_COMMANDS` arrays on the stack-allocated view):
     ```c
     static ReplSourceExtent source_scope_extent_from_prefix(
             const int *prefix, int count, int line_idx, int min_depth) {
         ReplSourceExtent ext = { 0, -1, -1 };
         int s, e;

         if (!prefix || line_idx < 0 || line_idx >= count)
             return ext;
         if (prefix[line_idx] < min_depth && prefix[line_idx + 1] < min_depth)
             return ext;

         s = line_idx;
         while (s > 0 && prefix[s] >= min_depth)
             s--;
         e = line_idx;
         while (e + 1 < count && prefix[e + 1] >= min_depth)
             e++;
         ext.valid = 1;
         ext.start = s;
         ext.end = e;
         return ext;
     }
     ```
     Call it with `(view->begin_depth_prefix, view->count, line_idx, 1)` for `begin_block`, `(view->tess_depth_prefix, view->count, line_idx, 1)` for `tess_polygon`, and `(view->tess_depth_prefix, view->count, line_idx, 2)` for `tess_contour`:
     - Outside a block, both `prefix[line_idx]` (depth before) and `prefix[line_idx + 1]` (depth after) are `< min_depth`, returning in $O(1)$.
     - On a head row, `prefix[line_idx] < min_depth` and `prefix[line_idx + 1] >= min_depth`; on a closing `glEnd()` / `gluEnd()` row, `prefix[line_idx] >= min_depth` and `prefix[line_idx + 1] < min_depth` (inclusive close).
     - Adjacent blocks or back-to-back contours drop below `min_depth` at the shared boundary index (`prefix[k] == 0` between blocks/polygons, `prefix[k] == 1` between contours), so neither `s` nor `e` bleeds across neighbours.
     - Unterminated blocks keep `prefix[count] >= min_depth`, so `e` stops at `count - 1`.
     - Because `tess_depth_prefix` is built with `repl_tess_advance_depth()`, a contour outside a polygon stays at depth `0` (invalid) and a nested `gluBegin(GLU_POLYGON)` stays at depth `1` (absorbed into the enclosing polygon and closed by a single polygon `gluEnd()`).
   - Add a table-driven test in `tests/test_repl_core_internal.c` covering: cutout (two contours + comment between), extrusion (`glBegin` strip nested in a contour, reporting all three extents simultaneously), single-contour polygon, a polygon inside a `for`, unterminated `glBegin` / polygon / contour, and malformed input (contour outside a polygon, nested polygon, and two `glBegin` rows with no `glEnd()` between them - `begin_depth_prefix` reaches 2 and the `min_depth = 1` scan reports one merged extent; assert that rather than leaving it unspecified).

2. **Switch the controller scanners (`src/app/glr_ctrl.c`).**
   - In `glr_ctrl_fill_cursor_block_state()` and `glr_ctrl_build_ui_snapshot()`, call `repl_source_scope_cursor_scope()` and populate `cursor_source_block_*` / `cursor_source_tess_*` and `snap->current_begin_block_*` / `snap->current_tess_block_*` from `scope.begin_block` and `scope.tess_polygon`.
   - Delete `glr_ctrl_current_begin_block_source_extent()` and `glr_ctrl_current_tess_block_source_extent()`.

3. **Switch the flatten-query scanners (`src/repl/flatten_query.c`, `tests/test_repl_core_internal.c`).**
   - In `repl_flatten_refresh_current_block_highlight()`, replace `flatten_current_begin_block_source_extent()` with `repl_source_scope_cursor_scope(edit_line_idx, &scope)` and pass `scope.begin_block.start` / `scope.begin_block.end` to `flatten_find_flat_begin_block_extent()`.
   - In `flatten_find_flat_begin_block_extent()`, handle unterminated blocks without relying on `source_end < 0` (since `ReplSourceExtent.end` is `doc_count - 1` when unterminated): if the inner scan from `i + 1` reaches `flat_cmd_count` without encountering any `CMD_END`, return `[i, flat_cmd_count - 1]`.
   - In `repl_flatten_cursor_tess_contour()`, replace `cursor_tess_contour_source_begin()` with `repl_source_scope_cursor_scope()` and read `scope.tess_contour.start` when `scope.tess_contour.valid`.
   - Delete `flatten_current_begin_block_source_extent()` and `cursor_tess_contour_source_begin()`.
   - Update the `glEnd()` assertion in `tests/test_repl_core_internal.c`: with `glEnd()` now inclusive in `begin_block`, `repl_flatten_cursor_polygon(14)` resolves the block's final primitive (`p.valid == 1, p.first == 4, p.last == 7`) per the overlay scope contract.

4. **Preserve enclosing GLU context across nested `glBegin`/`glEnd` in walker and gutter (`src/subsystems/replay/replay.c`, `src/ui/app/repl_code_panel.c`, `scripts/check/check-tess-depth.py`, `tests/test_ui.c`).**
   - In `replay_walk_user_vertices()` (`src/subsystems/replay/replay.c`):
     - Track the enclosing tess polygon's state alongside the immediate block (`in_begin_block`, `tess_poly_selected`, `tess_poly_vertex_idx`).
     - Remove the two `tess_depth = 0;` resets at `CMD_BEGIN` and `CMD_END`, and remove the corresponding `("src/subsystems/replay/replay.c", "tess_depth", "0"): 2` entry from `EXCEPTIONS` in `scripts/check/check-tess-depth.py`.
     - At `CMD_TESS_BEGIN_POLYGON`: once `state.block_selected` holds the polygon's verdict, capture `tess_poly_selected = state.block_selected` and `tess_poly_vertex_idx = 0`. It must be captured here, before any nested `CMD_BEGIN` overwrites `state.block_selected` with the strip's verdict; the `CMD_END` restore below depends on it.
     - At `CMD_BEGIN`: if `tess_depth > 0` and not already in a `begin_block`, save `tess_poly_vertex_idx = state.vertex_idx_in_block` before resetting `state.vertex_idx_in_block = 0` and computing the strip's `state.block_selected`.
     - At `CMD_END`: clear `in_begin_block` and `state.primitive_mode = 0`. If `tess_depth > 0`, restore `state.in_block = 1`, `state.block_selected = tess_poly_selected`, and `state.vertex_idx_in_block = tess_poly_vertex_idx`; otherwise clear `state.in_block = 0` and `state.block_selected = selected_block_only ? 0 : 1`.
     - At `CMD_TESS_END`: when `old_depth == 1 && tess_depth == 0`, clear `tess_poly_selected` and `tess_poly_vertex_idx`, and if `!in_begin_block`, clear `state.in_block = 0` and `state.block_selected`.
   - In `ReplCodePanelWalkState` (`src/ui/app/repl_code_panel.c`):
     - Add `tess_vnum` and `tess_vnums_exact` to save the enclosing polygon's shared display counter while walking a nested `CMD_BEGIN` block.
     - At `CMD_BEGIN`: if `state->owner == REPL_CODE_PANEL_VOWNER_TESS`, save `tess_vnum = state->vnum` and `tess_vnums_exact = state->vnums_exact` before switching `state->owner = REPL_CODE_PANEL_VOWNER_BEGIN` and resetting `state->vnum = 0`.
     - At `CMD_FOR_BEGIN`: if `state->tess_depth > 0`, also mark `state->tess_vnums_exact = 0`.
     - At `CMD_END`: if `state->tess_depth > 0`, restore `state->owner = REPL_CODE_PANEL_VOWNER_TESS`, `state->vnum = state->tess_vnum`, and `state->vnums_exact = state->tess_vnums_exact && (state->loop_depth == 0)` instead of resetting `owner` to `NONE`.
   - Extend `test_vertex_gutter_labels_glbegin_inside_tess_polygon()` in `tests/test_ui.c` with a `gluVertex` row after the nested strip's `glEnd()` to assert that it resumes the polygon's shared display run (`v1`) and matches the scene walker.

5. **Unify the flat-side cursor matchers (`src/repl/flatten_query.h`, `src/repl/flatten_query.c`, `src/subsystems/edit_overlays/edit_overlays.c`, `src/subsystems/replay/replay.c`).**
   - **Today the two matchers disagree for `glBegin` blocks, and the merge must pick the walker's rule.** The replay walker (`CMD_BEGIN` in `replay_walk_user_vertices()`) lets the cursor's `glBegin` source extent **decide alone** whenever `cursor_source_block_valid`; it falls back to the flat walk only without one. `outline_block_matches_cursor()` never consults the `glBegin` source extent - it checks flat commands only. The walker's rule is deliberate: `test_vertex_numbers_use_source_begin_block()` in `tests/test_edit_overlays.c` pins that a flat block range landing inside an earlier block must not label that block. A helper that ORs the source match with the flat walk would select that earlier block and fail the test.
   - **Per-kind rule (record it in the overlay scope contract with this step):**
     - `glBegin` block: when `cursor_source_block_valid`, the block matches iff its source extent matches (`repl_cursor_source_begin_head_matches()`); otherwise the flat walk decides.
     - Tess polygon: the source tess-head match **or** the flat walk. The OR is what lets a cursor on a comment line between contours select its polygon (`05f2c7a9`); there is no stale-range hazard because the tess-head match compares source indices only.
   - Add the shared helpers to `src/repl/flatten_query.c` (declared in `flatten_query.h`; not inline - the block matcher loops over the flat program):
     - `repl_cursor_flat_cmd_matches(FlatProgramView program, const CursorBlockState *cursor, int flat_idx)` - call-site provenance (`call_src_cmd_idx`, `root_call_src_cmd_idx`), `cursor_func_scope_mask`, `cursor_block_begin..cursor_block_end`, and `src_cmd_idx == edit_line_idx`.
     - `repl_cursor_source_begin_head_matches(FlatProgramView program, const CursorBlockState *cursor, int begin_idx)` - the `glBegin` source-extent matcher (today `replay_walk_source_block_matches_cursor()` in `replay.c`): any unrolled copy whose `CMD_BEGIN` has `src_cmd_idx == cursor_source_block_begin` and whose closing `CMD_END` (if present) has `src_cmd_idx == cursor_source_block_end`.
     - `repl_cursor_flat_block_matches(FlatProgramView program, const CursorBlockState *cursor, int begin_idx, int is_tess)` - applies the per-kind rule above: for `!is_tess` with a valid source extent, return `repl_cursor_source_begin_head_matches()` and stop; for `is_tess`, return 1 on `repl_cursor_source_tess_head_matches()`; otherwise walk the flat block from `begin_idx` to its `CMD_END` or polygon `CMD_TESS_END` (`repl_tess_advance_depth() == 0`) testing `repl_cursor_flat_cmd_matches()`.
   - Replace `outline_cmd_matches_cursor()` / `outline_block_matches_cursor()` in `edit_overlays.c` and `replay_walk_flat_cmd_matches_cursor()` / `replay_walk_source_block_matches_cursor()` / `replay_walk_block_matches_cursor()` in `replay.c` with calls to those helpers.
   - **Visible change:** the outline highlight adopts the `glBegin` source-extent rule the labels already follow, so highlight and labels select the same copies. Extend the gutter/scene parity sweep (or add a sibling) to assert that the outline-highlighted blocks and the labelled blocks agree for every cursor line of the GLU concave-arrow examples, and keep `test_vertex_numbers_use_source_begin_block()` passing unchanged.

#### Implementation notes

`repl_source_scope_view_cursor_scope()` and its live wrapper return all three
inclusive extents from the existing prefix arrays. No arrays were added to
`ReplSourceScopeView`. The controller scanners and both flatten-query source
scanners are removed. Unterminated flat blocks map through the last flat row.

Shared flat matchers now serve replay, outlines, and normal arrows. Item 4's
collector uses the additional `repl_cursor_source_begin_matches()` primitive
with its recorded head/close, so source-authoritative matching adds no flat
rescan. Restarts and unfinished blocks use the same fallback as the walker.

The walker, gutter, and normal-arrow gate restore the enclosing GLU context
after a nested strip. The walker applies transforms outside immediate blocks,
including inside contours. Tests cover source boundaries and malformed input,
live-cache invalidation, stale flat ranges, tail vertices with and without
polygon vertices before the strip, and every cursor line of the three GLU
arrow examples. The existing source-extent label regression stays unchanged.

Validation: all 89 ASan/UBSan test binaries pass locally and under GCC on
Gracemont. The native application builds; C99 and the ownership/contract
guards pass on both platforms. The old replay immediate-block reset exception
is removed from `check-tess-depth`.

Acceptance: the full stub gate (`make test-stubs`, including `check-tess-depth` without the `replay.c` exception) plus the new unit tests; no other scan of `CMD_BEGIN`/`CMD_END` or `CMD_TESS_*` pairs for "the block around a line" remains outside `source_scope.c` and the shared flat-side matchers. The code panel's gutter walk still steps through `CMD_BEGIN`/`CMD_END` after this item, but to **number** vertices (step 4 adjusts it), not to find the cursor's block; replacing that walk is item 3.

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
walker's former immediate-block reset exception was removed by item 1.

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
test that now guards it (`glr_overlay_fill_cursor_block_state` driven from
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

### 4. Collect outlines once, draw them twice - DONE

#### Previous implementation

`edit_overlays.c` had three near-copy walkers - `render_outlines_glbegin_pass`,
`render_outlines_tess_pass`, `render_outlines_glut_pass` - each re-applying
tracked transforms and replaying clip / cull / stencil / color-mask state.
Since `4058f9fa` each ran in two phases (plain, then highlight) so a
highlight always won its edges, and selection (`last_matching_block_head`,
`last_selected_glut_shape`) made further GL-free walks.

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

#### Implementation

`outline_collect` replaces the three walkers and their selection scans.
Bounded static arrays hold batches, linked vertex streams, scope records,
CPU matrices, and scoped GL state. Empty polygons retain scope records so
an empty final instance still suppresses earlier highlights. Interleaved
immediate blocks and GLU contours retain separate vertex streams.

Matrices stay relative to the entry camera until `glLoadIdentity` makes them
absolute. Shared CPU translate/scale helpers update all four rows, including
projective matrices. There is no modelview readback. Clip records pair an
equation with its set-time matrix; inherited equations are acquired at most
once per collection. Attribute pops restore only the mask's state groups.

Drawing visits every plain batch before every highlight, retaining the
immediate/GLU/GLUT family order and changing GL state only when needed.
Line width stays live across each phase instead of resetting between batches.
Single-polygon draws retain fan anchors and odd strip winding; contours lift
culling; stencil writes stay disabled. Replay preview, homogeneous vertices,
RGB masking, and unfinished blocks retain their existing behavior.

Validation: local and isolated GCC ASan/UBSan stub suites passed all 89
binaries; the focused overlay suite passed 579/579;
C99 and ownership guards passed. A native driver differential passed
524/524 checks. An independent native rendering comparison covered nine
fixtures, four scopes, and three highlight modes: 108/108 images matched
byte for byte, with no GL errors.

Acceptance: every outline trace test in `tests/test_edit_overlays.c` green
(ordering, culling lift, clip suspend, GL/GLU parity); `overlay_outlines`
highlight rows within ~10% of the plain row.

#### Benchmark verification

Initial macOS timings were discarded because a software update was active.
The reported comparison used quiet Gracemont: Intel N95, GCC 14.2.0, Mesa
25.2.8 hardware GL through X11/GLX, vendored freeglut, release `-O2`.
Baseline: `1c633050`. Both versions used the unchanged frozen benchmark
corpus and `bench_repl --only overlay_outlines --iters 50 --csv`.

One warm-up per binary preceded five alternating before/after pairs, each
iteration drawing 50 frames. No builds or tests overlapped timing. A
30-second idle check preceded warm-up; quiet intervals between pairs and
afterward measured at least 97.7% CPU idle and at most 0.13 MB/s disk traffic.
During timing, other processes used at most 2.42% of total CPU and 0.58 MB/s
disk traffic.

Values below are microseconds/frame: the median of five minimum 50-frame
batch times, divided by 50. This measures steady submission cost without
first-use driver initialization skewing the small scenes' means. Each cell
is **before -> after**.

| Scene | Plain | Last instance | Single polygon | All instances | Whole scene |
|---|---|---|---|---|---|
| Wave | 33.82 -> 16.14 | 53.85 -> 17.51 | 54.30 -> 17.65 | 52.84 -> 16.53 | 52.85 -> 16.51 |
| Torus | 61.03 -> 28.03 | 97.82 -> 29.49 | 98.49 -> 29.72 | 96.80 -> 28.44 | 96.83 -> 28.33 |
| Sponge | 475.20 -> 372.76 | 499.59 -> 374.38 | 499.96 -> 376.92 | 513.39 -> 371.54 | 514.11 -> 372.18 |
| Extrusion | 3.17 -> 4.53 | 6.44 -> 4.99 | 6.87 -> 5.16 | 6.44 -> 4.96 | 6.43 -> 4.94 |

Highlight submission improved 23-70%. The approximate 10% target holds
except for single-polygon extrusion: 14.0% above plain, an extra 0.63 us
for the selected-primitive redraw. Collection adds 1.36 us to that scene's
plain row; its highlight rows still improve 23-25%. These small-scene costs
remain a measured tradeoff of collecting batches. The width assertions
count draws under the expected width, preserving style coverage when one
setter serves several batches.

---

### 5. Doc links stop churning on line numbers - DONE (option A)

`make check` (and so `test` / `test-stubs`) runs
`check-doc-links.py --rewrite-paths --repoint` in place of the strict check
when `DOC_LINKS_FIX=1` - the default, except when `CI` is set. Only the
lossless repairs run; a link whose label is gone still fails, and
`make fix-doc-links` (which may strip) stays a deliberate act. The gate
prints a note when it repaired something. `.githooks/pre-push` passes
`DOC_LINKS_FIX=0`, so a push checks strictly.

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

### 6. Build traps - DONE

- GNU make < 4 prints a warning once per top-level run (`MAKE_VERSION_WARN=0`
  silences it). Measured on macOS's 3.81: a prerequisite touched 0.3 s after
  its target is reported up to date. It stays a warning because make 4.x on
  APFS has not been confirmed to compare sub-second mtimes on this machine.
- `bench_extedit` accepts `--only` with bench_repl's semantics; its rows
  answer to `extedit_small` / `extedit_typical` / `extedit_large` or the
  group `extedit`, and a filter that selects none prints nothing.
- `make check` runs every guard and lists the red ones at the end;
  `test-stubs` runs the tests even when the gate is red, repeats the red
  guard names after the test output, and fails if either failed.
- `make test-one T=test_edit_overlays [ARGS=...]` builds the binary in
  `build/debug-nosan-gl-stubs/` and runs it; documented in CLAUDE.md.

Original notes:

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

### 7. Test policy and fixtures - DONE

The policy is in [`docs/CONTRIBUTING.md`](../../CONTRIBUTING.md#test-policy).
The pieces below are implemented; the original notes follow them.

#### Implementation notes

- [`tests/support/overlay_fixture.h`](../../../tests/support/overlay_fixture.h)
  turns source rows into an `OverlayWalkCtx` through the frame's own steps:
  `glr_ctrl_reset_all`, `editor_feed_line`, `repl_flatten_commands`,
  `repl_flatten_refresh_current_block_highlight`, then
  `glr_overlay_fill_walk_ctx`. `overlay_fixture_load` /
  `overlay_fixture_fill` split the load from the per-cursor, per-scope fill
  for sweeps, and `OVERLAY_FIXTURE(&ctx, cursor, scope, "row;", ...)` does both.
  The fill leaves the live presentation scope as it found it.
- The GLU outline tests in `tests/test_edit_overlays.c` use it: the contour
  outline test, plain-outline parity, culling lift, and highlight-after-plain
  ordering. The two-contours-share-one-`gluEnd()` fixture is gone; its
  assertions run on a real two-contour polygon, whose single-polygon contour
  ranges now come from the pipeline (and are asserted) instead of being typed
  in. A comment row between two contours has a real-source test beside the
  hand-built matcher test. Hand-built arrays remain where the test needs a
  flat range that deliberately disagrees with the source extent, or two flat
  instances of one source row.
- [`tests/support/gl_trace.h`](../../../tests/support/gl_trace.h) provides
  `trace_fmt()` / `TRACE_FMT()`; every enum literal in the overlay trace
  expectations (`glBegin 9`, `glEnable 2884`, the stencil and clip-plane
  codes) names its constant.
- The gutter/scene sweep in `tests/test_ui.c` runs every cursor row of the
  three GLU arrow examples through all four scopes at both placements. The
  scene side is the real label pass, read from the stub trace, so the
  placement-dependent numbering is the production formula. Per the contract:
  Last instance and All instances at vertex draw the gutter's numbers; a
  decluttered Last instance draws some of them (decluttering may omit a
  crowded label, never renumber one); Whole scene at vertex draws every
  vertex, the gutter's numbers among them, whatever the cursor; Single polygon
  draws a subsequence of the gutter's numbers; decluttered All instances and
  Whole scene are one global run. The Last-instance default is still checked
  row by row against the vertex walker. These scenes hold one copy of each
  block, so `test_label_numbers_over_unrolled_copies` in
  `tests/test_edit_overlays.c` covers the copy rules on a real unrolled loop.

#### Original notes

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
  fills an `OverlayWalkCtx` through `glr_overlay_fill_walk_ctx`. Default
  to it; keep hand-built arrays for shapes the parser cannot produce.
- **Named enums in traces.** Expectations such as `"glBegin 9"` and
  `"glEnable 2884"` need a lookup to read. A small formatting helper
  (`trace_fmt(buf, "glBegin %u", GL_POLYGON)`) keeps the stub's numeric
  output and makes the test say `GL_POLYGON`.

---

### 8. Overlay setup out of `glr_ctrl.c` - DONE

`src/app/glr_ctrl.c` was ~5,500 lines. Its overlay setup - the cursor block
fill and its `_for_test` seam, the `OverlayWalkCtx` fill (used by
`bench_repl`), the cursor-line guide snapshot with its live-input arg-slot
parser, and the overlay pack builder - is a self-contained unit that tests and
`bench_repl` call directly. It lives in its own file under `src/app/`, and the
test seam is gone.

#### Implementation notes

[`glr_overlay_setup.{c,h}`](../../../src/app/glr_overlay_setup.h) owns the unit
and gives its public functions the file's `glr_overlay_` prefix.
`glr_overlay_fill_cursor_block_state()` and `glr_overlay_fill_walk_ctx()` are
the two fills. The file-static `glr_overlay_build_guide_snapshot()`,
`fill_guide_arg_slots()` and `parse_arg_slots()` build the guide snapshot. The
frame's `OverlaySnapshotPack` is file-static behind `glr_overlay_pack()` (the
stable pointer the scene-config builder installs as `user_data` on the two
post-overlay hooks) and `glr_overlay_build_pack(cfg)` (called last in the
builder, after the config fields it copies are assigned). The extent scanners
went with item 1, so they are not part of the move; `light_slot_from_text()` and
`glr_ctrl_build_focus_vertex()` feed the render config rather than the pack and
stay in the controller. The move changes no behavior.

`glr_ctrl_cursor_block_state_for_test()` is deleted. Its callers
(`tests/test_ui.c`, `tests/test_edit_overlays.c`, `tests/test_replay_walk.c`)
want only the `CursorBlockState`, and `glr_overlay_fill_walk_ctx()` would also
resolve the single-polygon ranges and presentation toggles they ignore, so the
cursor fill is public instead. The seam refreshed the flat block cache itself;
callers now call `repl_flatten_refresh_current_block_highlight()` first, the
contract the walk-context fill, `bench_repl` and the frame path already follow.

The Makefile source lists need no entry: `src/app/*.c` is a wildcard, and no
demo list links the controller (`REPL_DEMO_DEP_SRCS` and the other demo lists
name no `src/app` file). `test_replay_walk` includes `glr_overlay_setup.c` as a
unit to reach `parse_arg_slots` and filters that object out of its link; it
used to include all of `glr_ctrl.c` for the same helper.
`check-app-boot-band` and the rest of `check-state-ownership` pass unchanged.

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
