# Replay Function Call And Vertex Guide Debugging Audit

Status: done
Date: 2026-06-06

## Intent

Replay is already useful for stepping through flattened geometry, but function
reuse and recursion make it hard to see which source invocation produced the
current command. Vertex replay also shows the active vertex without applying
the existing transform-guide affordances to that exact replay vertex.

This audit covers six requested improvements:

- Req 1 — Highlight the function call site during replay.
- Req 2 — Show function call depth, especially for recursive functions.
- Req 3 — Apply transformation guides to the specific vertex selected by replay.
- Req 4 — When the cursor is on a vertex line, highlight the transform lines that
  affect it (live REPL), made flat-accurate across function boundaries and given
  a distinct marker color.
- Req 5 — Apply that same affecting-transform highlight to the replay-focused
  vertex (replay).
- Req 6 — Make replay transforms look like the live transform guides: while
  replay is focused on a vertex, resolve a transform focus for that vertex and
  show the live animated guide there; this **replaces** req 3's static frame
  axes.

**Implementation status (2026-06-06).** Reqs 1–3 are implemented and committed
on `main` (`34df87f3` call-site highlight, `fd571604` call depth, `0cb8c6d4`
frame axes, `4b5d050e` stale-`pc` clamp for replay vertex focus), verified via
headless OSMesa capture. Req 3's static frame axes are an interim visual that
**req 6 supersedes** with the live-style transform guides. Req 4 is now
implemented: `repl_find_affecting_transforms_for_flat_vertex()` /
`repl_find_affecting_transforms_flat()` (`src/repl/autonormal.c`) resolve the
affecting transforms through the flat program so a vertex inside a funcN body
picks up calling-scope transforms; `glr_ctrl_push_highlights()` uses the flat
resolver (source-walk fallback only when the flat program is empty), and the
affecting-transform marker is recolored vivid amber and reprioritized above the
feeding-color/normal markers (`src/ui/app/repl_code_panel.c`). Tests added in
`tests/test_repl_core_commit.c`. Req 5 is now implemented:
`glr_ctrl_push_highlights()` suppresses the edit-cursor affecting-transform set
during replay and instead pushes `HIGHLIGHT_AFFECTING_TRANSFORM` for
`replay_focus_vertex_flat_idx()` via the req-4 exact-flat resolver, so the
transforms shaping the replayed vertex show regardless of cursor position
(`test_replay_focus_vertex_affecting_transforms` in `tests/test_glr_ctrl.c`).
Req 6 is now implemented and **all six reqs are complete**:
`scene_transform_guides_prepare()` (`src/scene/guides/transform_guides.c`) gains
a replay branch that picks a transform focus for the replay vertex —
cursor-on-transform (nearest matching expansion) or the nearest in-scope
affecting transform — and `scene_transform_guides_render_if_due()` anchors the
existing translate/scale/rotate guide on the replay vertex (drawn in the
transform's local frame). The static `scene_replay_frame_axes_render()` and its
`edit_overlays.c` call are removed. The in-scope transform selection lives in
the scene module as a small flat-program walk (no repl/core dependency;
`scene_demo` still links). Tests added in `tests/test_scene_guides.c` (replay
prepare cases — default nearest, cursor-on-transform, no-transform, popped
scope — plus a GL_STUBS render smoke test).

## Current Architecture Notes

The flat command model already carries useful function provenance:

- `src_cmd_idx`: source line that produced the flat command.
- `call_src_cmd_idx`: immediate `funcN(...)` call site.
- `root_call_src_cmd_idx`: outermost call site for nested function expansion.
- `func_scope_mask`: function bodies active while the command was flattened.

`glr_ctrl_push_highlights()` currently pushes only the replay PC source-line
highlight via `replay_src_line()`. It does not surface the active call site,
even though the flat command usually has enough provenance for non-recursive
and nested-call cases.

Replay state currently tracks `pc` and `src_line_idx`, but not a dedicated
"current flat command" or call-stack snapshot. `replay_update_after_pc_change`
derives `src_line_idx` from the last meaningful source command in the current
step range. That is enough for current code-panel highlighting, but too lossy
for exact invocation context.

Any feature that needs the active flat command should share one replay-focus
contract instead of indexing the flat array with `replay_pc()`. Add narrow
helpers in the replay subsystem:

- `replay_focus_flat_idx()`: last replay focus candidate in the current replay
  step, using the same focus-candidate predicate as `src_line_idx`.
- `replay_focus_vertex_flat_idx()`: last `repl_cmd_emits_vertex()` command in
  the current replay step; `-1` outside vertex replay mode.

Both helpers should derive the current step range from replay state by factoring
the existing `replay_prev_limit(pc)` logic, so seek, step-back, pause, and
normal advance all produce the same focus without relying on fade batches or a
transient `old_pc` local.

The transform-guide path already has most of the rendering machinery needed:
`edit_overlays_render_cursor_guides()` walks the flat program, applies tracked
modelview transforms, and uses `cursor_guide_snapshot_with_flat_args()` to
substitute evaluated flat vertex/normal args. The replay case is currently
gated off in `scene_transform_guides_prepare()` and the geometry-guide branch
skips while `snapshot->replaying` is true.

## Effort Assessment

### 1. Highlight the active function call site

Effort: low, roughly 0.5-1 day.

Recommended implementation:

- Add highlight kinds for replay call-site markers, for example
  `HIGHLIGHT_REPLAY_CALL_SITE` and optionally
  `HIGHLIGHT_REPLAY_ROOT_CALL_SITE`.
- Add UI marker/background colors in `repl_code_panel.c` that are distinct
  from the existing green replay PC highlight.
- In `glr_ctrl_push_highlights()`, inspect `replay_focus_flat_idx()` and, when
  it resolves to a valid flat command, push highlights for:
  - `flat.call_src_cmd_idx` as the immediate call site.
  - `flat.root_call_src_cmd_idx` as the root call site when it differs.
- Keep the current `HIGHLIGHT_REPLAY_PC` body-line highlight unchanged.

Expected behavior:

- A reused function highlights the specific `funcN(...)` call whose expansion
  is currently being replayed.
- Nested calls can show both the immediate call and the root call, if distinct.
- Top-level non-function commands behave exactly as they do now.

Tests:

- Add a replay/highlight test for one function called twice; replaying each
  expansion should highlight the matching call line.
- Add a nested function call test where immediate and root call sites differ.
- Verify existing replay PC highlighting still wins or coexists cleanly in the
  code-panel marker priority rules.

### 2. Show function call depth

Effort: medium for a practical v1, roughly 1-2 days. High for exact recursive
call-stack display, roughly 3-5 days.

Current limitation:

`func_scope_mask` can say "this command is inside func0/func1", but it cannot
distinguish repeated recursive invocations of the same function body. For
example, recursive `func0(n)` expansions at depth 1, 2, and 3 all share the
same function scope bit.

Recommended v1:

- Add a `call_depth` provenance field to `GLCmd`.
- Thread the existing flatten recursion depth into `flat_cmd_set_provenance()`
  and every `flatten_append_cmd()` call.
- Surface depth during replay as a compact annotation or status segment, for
  example `depth 3`, only when the current command came from a function call.
- Keep call-site highlighting based on the existing immediate/root source
  indices.

Full exact version:

- Add a bounded call-stack snapshot per flat command, such as arrays of
  `{func_slot, call_src_idx}` plus a stack depth.
- Use that stack to display exact invocation ancestry for recursion and nested
  reused functions.
- Update debug dump output so provenance is inspectable from CLI diagnostics.

Tests:

- Extend recursive function tests to assert `call_depth` increases across
  recursive expansion.
- Add a nested reused-function test proving depth and call-site provenance are
  stable when two call sites expand the same function body.
- Add a flatten-depth-limit test to ensure the added metadata stays bounded
  when recursive expansion aborts at the existing depth limit.

### 3. Net frame axes at the current replay vertex

Effort: medium, roughly 1-2 days.

Scope decision (2026-06-06): the chosen visual is **net frame axes** — draw the
vertex's local coordinate frame (the accumulated-modelview origin plus colored
X/Y/Z axis lines), **not** the full per-op animated translate/rotate/scale
guides. This is the simplest, clearest "where am I in the transform stack" cue
and keeps the new draw code small. The fuller "reuse all of
`cursor_guide_snapshot_with_flat_args()` / per-op guides during replay" option
was considered and deliberately deferred in favor of the axes-only visual.

Three contract pitfalls to get right (verified against the code):

- **`replay_pc()` is an execution limit, not the active vertex.**
  `replay_next_vertex_limit()` returns `flat_idx + 1` for a vertex and
  `replay_advance()` stores that as `state->pc` (`replay_playback.c:235`,
  `:420`), so the PC points at the *next* command (or `flat_count`). Do **not**
  anchor on `replay_pc()` directly. Derive the current step begin by factoring
  the existing `replay_prev_limit(pc)` logic, then scan `[step_begin, pc)`
  backward. `replay_focus_flat_idx()` scans for the last replay focus candidate;
  `replay_focus_vertex_flat_idx()` scans for the last `repl_cmd_emits_vertex()`
  command. This keeps advance, seek, step-back, and paused replay aligned and
  avoids using fade batches as focus state.
- **The frame matrix does not include the vertex args.**
  `compute_before_cursor_matrix()` applies only the transforms *before* the
  flat index (`transform_guides.c:102`); its origin is the local
  transform-stack origin, not the vertex. The axes must be **translated to the
  focused vertex position** `(vx, vy, vz)` under that computed frame. Read the
  evaluated args from the focused flat vertex command — or, better, capture
  them in the replay vertex-walk `on_vertex` callback, where loop/function-local
  args are already evaluated for the current frame.
- **The cursor-guide walk early-stops during replay.**
  `on_cmd_render_cursor_guides()` computes
  `geometry_done = (geometry_guide_done || snapshot->replaying)`
  (`edit_overlays.c:498`); with no xform plan during replay
  (`scene_transform_guides_prepare()` returns early at
  `transform_guides.c:715`), `early_stop` fires on the *first* command and the
  walk never reaches the focus vertex. So the edit-overlay hook can't be reused
  as-is.

Recommended implementation:

- Add replay-specific focus metadata: a narrow accessor (or a field in
  `ReplayRuntimeState` updated alongside `src_line_idx`) that returns
  `replay_focus_flat_idx()` for call-site UI and `replay_focus_vertex_flat_idx()`
  for vertex axes. Vertex axes are vertex-mode only in v1; leave polygon mode
  unchanged.
- Extend `SceneGuideSnapshot` with `replay_focus_vertex_flat_idx` (and reuse the
  existing `replaying` flag), populated in `glr_ctrl_build_guide_snapshot()`
  when `replay_active()` and `replay_mode() == REPLAY_MODE_VERTEX`.
- Prefer a **dedicated replay axes walk** over reusing
  `edit_overlays_render_cursor_guides()`: a small walk that applies tracked
  transforms (same `apply_tracked_transform()` / `unwind_transform_stack()`
  helpers) up to `replay_focus_flat_idx` and fires `on_vertex` there, so the
  modelview frame *and* the evaluated `(vx, vy, vz)` are both in hand. If the
  shared walk is reused instead, add a `have_replay_axes` pending condition so
  it does not early-stop until the focus vertex has been drawn.
- Add a small `scene_replay_frame_axes_render()` (in `transform_guides.c` or a
  sibling `frame_axes.c`): compose `cam_view * frame`, load it, `glTranslatef`
  to `(vx, vy, vz)`, and draw three short colored X/Y/Z axis lines with the
  existing two-pass ghost+solid depth treatment. This is a separate path gated
  on `snapshot->replaying`, so it leaves the edit-time guard
  `if (snapshot->replaying ...) return 0;` in `scene_transform_guides_prepare()`
  intact (that guard correctly keeps edit-mode single-op guides off during
  replay).
- Preserve existing edit-mode guide behavior and the normal replay vertex-point
  overlay.

Expected behavior:

- While replay is active in vertex mode, three colored axis lines mark the
  current replay vertex's local frame, **anchored at the vertex position**
  `(vx, vy, vz)`, after the same modelview transforms used to render it.
  Function-local / loop vertex expressions anchor at their evaluated positions,
  not parse-time defaults.
- Existing cursor edit guides remain unchanged outside replay.

Tests:

- Assert the snapshot plumbing: `replay_focus_vertex_flat_idx` is populated only
  while replaying in vertex mode, in a controller-level test.
- Extend `test_replay_walk` or `test_edit_overlays` to assert the walker
  reaches the replay focus flat vertex with the transformed modelview.
- Visual check headless: OSMesa build (`make gl-repl FREEGLUT_OSMESA=1`), load
  an example with transforms, start replay, `kill -USR1 <pid>` to capture a PPM
  and confirm axes anchor on the PC vertex.

### 4. Flat-accurate, distinctly-colored affecting-transform highlight (live REPL)

Effort: low-medium, roughly 0.5-1 day.

Current state (already partly built):

- `repl_find_affecting_transforms()` (`src/repl/autonormal.c:388`) already
  highlights the in-scope `glTranslatef/glRotatef/glScalef` lines when the
  cursor is on a vertex / glut-solid line (`repl_cmd_consumes_current_color`),
  pushed as `HIGHLIGHT_AFFECTING_TRANSFORM` in `glr_ctrl_push_highlights()`
  (`src/app/glr_ctrl.c:372-378`) and rendered as the orange gutter marker
  (`MARKER_PRIORITY_AFFECTING_TRANSFORM`, `repl_code_panel.c`). So the base
  "cursor-on-vertex → affecting transforms" behavior exists.

Gaps to close (the requested change):

- **Cross-function (flat-accurate).** The current walk is source-order and
  breaks at `CMD_FUNC_DEF` (function bodies are opaque), so a vertex *inside* a
  `funcN` body does not get the calling-scope transforms highlighted. Add a
  flat-program resolver with two layers:
  - An exact flat-target helper, e.g.
    `repl_find_affecting_transforms_for_flat_vertex(flat_idx, out, cap)`, that
    accepts one concrete flat vertex / tess vertex / glut-solid command, walks
    backward from that flat index maintaining the same
    push/pop/`glLoadIdentity` bookkeeping, collects the in-scope transform flat
    commands, maps each back to `src_cmd_idx`, and dedups.
  - A live-cursor line wrapper, e.g.
    `repl_find_affecting_transforms_flat(line_idx, out, cap)`, that finds every
    exact flat target whose `src_cmd_idx == line_idx` and whose type consumes the
    current modelview state, then returns the union of affecting transform source
    lines across those expansions. This gives deterministic behavior for reused
    and recursive function body vertices: without a selected invocation, the
    live cursor view shows all transforms that affect any expansion of that
    source vertex line. Do **not** use `repl_flat_cmd_matches_cursor()` to choose
    these targets; it intentionally matches whole function scopes and call-site
    blocks, which is too broad for "this vertex line."
  Because the flat program already inlined functions and substituted args, the
  exact flat walk naturally includes both call-site-scope transforms and in-body
  transforms. Keep the source-walk as a fallback only when the flat program is
  unavailable/dirty.
- **Distinct color.** Give the affecting-transform marker a dedicated hue that
  reads clearly alongside the feeding-color (yellow) and feeding-normal (blue)
  markers — distinct from today's orange if needed — in
  `repl_code_panel_apply_command_overlays()`; revisit `MarkerPriority` ordering
  so the transform marker is not masked on lines that also feed color/normal.

Tests: a vertex inside a `funcN` body called from two different transformed
call sites highlights both call-site translate source lines in live-cursor mode;
nested/recursive cases union the applicable source transforms; top-level cases
stay unchanged. Extend `test_repl_autonormal` / `test_glr_ctrl`.

Files: `src/repl/autonormal.c`, `src/repl/core.h`, `src/app/glr_ctrl.c`,
`src/ui/app/repl_code_panel.c`.

### 5. Affecting-transform highlight for the replay-focused vertex (replay)

Effort: low, roughly 0.5 day (builds on req 4 + `replay_focus_vertex_flat_idx`).

Current state: affecting-transform highlights are pushed only for the **edit
cursor** (`glr_ctrl_push_highlights`, gated `!insert_mode` + `edit_line`).
During replay the cursor does not track the PC vertex, so the transforms
affecting the replayed vertex are not shown.

Recommended implementation:

- When `replay_active()`, additionally resolve and push
  `HIGHLIGHT_AFFECTING_TRANSFORM` for the transforms affecting the replay focus
  vertex. The flat vertex index is already available as
  `replay_focus_vertex_flat_idx()` (req 3), so reuse the req-4 flat resolver
  keyed directly off that flat index (no source→flat mapping needed). This
  composes with the existing replay call-site (req 1) and depth (req 2)
  highlights.
- Decide cursor-vs-replay precedence: during replay show the replay-vertex set
  (the cursor may be parked elsewhere); outside replay keep the cursor set.

Tests: replay test — step to a vertex inside a transformed and/or `funcN` scope;
assert the affecting transform source lines are highlighted from the replay
focus, independent of where the edit cursor sits.

Files: `src/app/glr_ctrl.c` (push during replay), reusing the req-4 resolver.

### 6. Live-style transform guides during replay (replaces req 3's frame axes)

Effort: medium, roughly 1-2 days.

Scope decision (2026-06-06): **replace** the static RGB frame axes from req 3.
During vertex replay, `replay_focus_vertex_flat_idx()` remains the emitted
vertex. Req 6 adds a **separate transform focus** for that vertex: if the user
navigates the cursor onto an affecting transform line while replay is active,
guide that transform for the current replay vertex; otherwise guide the nearest
in-scope affecting transform before the replay vertex. Show the live animated
transform guide (translate arrow / scale ticks / rotate arc) applied to the
**current replay vertex**, mirroring the live behavior where cursor-on-transform
shows the effect on the last applied vertex.

Current state: req 3 draws `scene_replay_frame_axes_render()` axes at the replay
vertex, and the live guides are hard-disabled during replay by the guard
`if (snapshot->replaying || !snapshot->show_guides) return 0;` in
`scene_transform_guides_prepare()` (`transform_guides.c:715`).

Recommended implementation:

- Split/generalize `scene_transform_guides_prepare()` so the replay path builds
  a plan from a transform focus instead of assuming `replay_focus_flat_idx()` is
  a transform. Two sub-cases:
  (a) the cursor is moved onto a transform line during replay → choose the exact
  flat transform with that `src_cmd_idx` that affects the current
  `replay_focus_vertex_flat_idx()` invocation, preferring the closest preceding
  match when the source transform appears in multiple expansions;
  (b) no cursor transform is selected → choose the nearest in-scope affecting
  transform before the current replay vertex. If no affecting transform exists,
  produce no replay transform-guide plan.
- Anchor the guide at the replay vertex: the FRAME-mode matrix and the
  after-cursor origin should be computed relative to
  `replay_focus_vertex_flat_idx()` rather than the edit-time after-cursor origin,
  so "impact on the current vertex" is literal.
- Reuse the existing `draw_translate_guide` / `draw_scale_guide` /
  `draw_rotate_guide`; relax the replay guard only for this path, keeping
  edit-mode behavior unchanged when not replaying.
- Remove `scene_replay_frame_axes_render()` and its dedicated call in
  `edit_overlays_render_cursor_guides()`. `replay_focus_vertex_flat_idx()`
  stays — it now anchors the guides (req 6) and feeds req 5.

Tests: `test_scene_guides` for the prepare path producing a transform-guide plan
during replay (cursor-selected transform and default nearest-affecting-transform
cases); regression that edit-mode guides are unchanged outside replay; headless
OSMesa visual check.

Files: `src/scene/guides/transform_guides.c` / `.h`,
`src/scene/guides/guides_shared.h`,
`src/subsystems/edit_overlays/edit_overlays.c`, `src/app/glr_ctrl.c`.

## Suggested Implementation Order

1. Land the shared replay-focus helper and call-site highlighting first. This
   has the smallest surface area and uses provenance already present on flat
   commands.
2. Add `call_depth` as a v1 depth display. Defer full stack snapshots unless
   exact recursive ancestry is required in the UI.
3. Add replay-focused vertex guides after the replay focus flat-index contract
   is settled, because it touches replay state, overlay snapshots, and guide
   planning.

Reqs 1-3 are done. The remaining order is 4 → 5 → 6:

4. Build the flat-accurate affecting-transform resolver and recolor the marker
   (live REPL). This is the foundation reused by req 5 and is independently
   shippable.
5. Reuse that resolver for the replay-focused vertex — small once req 4 and
   `replay_focus_vertex_flat_idx()` exist.
6. Replace the static frame axes with live-style transform guides during replay
   last, since it reworks the guide prepare path and removes req 3's interim
   `scene_replay_frame_axes_render()`.

## Assumptions

- "Call site highlighted" means highlighting the source line containing
  `funcN(...)`, in addition to the function body line currently executing.
- "Call depth" means exposing nesting/recursion depth for commands produced by
  function expansion.
- Transform guides apply during vertex replay mode only in v1. Polygon replay
  mode remains unchanged.
- "Affects the current line" (reqs 4/5) means the in-scope modelview transforms
  (`glTranslatef/glRotatef/glScalef`) accumulated before the vertex, honoring
  `glPushMatrix/glPopMatrix/glLoadIdentity` — now resolved through the flat
  program so calling-scope transforms for `funcN`-body vertices are included.
- Req 6 replaces (does not coexist with) req 3's static frame axes; the live
  animated guides are the single replay transform visual going forward. Replay's
  vertex focus remains the emitted vertex; req 6's transform focus is derived
  separately from the cursor-selected affecting transform or the nearest
  in-scope affecting transform before that vertex.
- Existing untracked files `new_scene.c` and `parametric_torus_nested_for.c`
  are unrelated to this audit.

## Follow-up: glut-solid replay anchors (2026-06-08)

Reqs 5/6 originally fired only when replay parked on a true vertex
(`repl_cmd_emits_vertex`: glVertex/gluVertex), even though vertex stepping
already stops on each `glutSolid*` (`replay_next_vertex_limit`) and the
live-cursor affecting-transform path (req 4) already covered glut solids via
`repl_cmd_consumes_current_color`. So replay parked on a cube/sphere showed no
affecting-transform highlight (req 5) and no transform guide (req 6) — an
asymmetry with the cursor path.

Resolved by broadening the replay focus to a **draw anchor** (a vertex *or* a
glutSolid*): `replay_focus_vertex_flat_idx()` → `replay_focus_anchor_flat_idx()`
keyed on `repl_cmd_consumes_current_color()` (`replay_playback.c`), the snapshot
field renamed to `replay_focus_anchor_flat_idx` (`guides_shared.h`), and the
req-6 prepare gate relaxed to the same predicate (`transform_guides.c`). Neither
consumer needs a vertex *position* (the highlight only walks transforms back
from the flat index; the guide draws in the transform's own frame), so a glut
solid — which carries shape params, not a position — works unchanged. The
req-5 resolver `repl_find_affecting_transforms_for_flat_vertex` already accepted
any `consumes_current_color` target, so it needed no change. gluVertex was
already covered (it is part of `repl_cmd_emits_vertex`). Tests:
`test_replay_focus_anchor_glut_solid` (`test_repl_replay.c`), a glut-solid
replay prepare case (`test_scene_guides.c`), and
`test_replay_focus_glut_solid_affecting_transforms` (`test_glr_ctrl.c`).
