# Replay Function Call And Vertex Guide Debugging Audit

Status: in-review
Date: 2026-06-06

## Intent

Replay is already useful for stepping through flattened geometry, but function
reuse and recursion make it hard to see which source invocation produced the
current command. Vertex replay also shows the active vertex without applying
the existing transform-guide affordances to that exact replay vertex.

This audit covers three requested improvements:

- Highlight the function call site during replay.
- Show function call depth, especially for recursive functions.
- Apply transformation guides to the specific vertex selected by replay.

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

## Suggested Implementation Order

1. Land the shared replay-focus helper and call-site highlighting first. This
   has the smallest surface area and uses provenance already present on flat
   commands.
2. Add `call_depth` as a v1 depth display. Defer full stack snapshots unless
   exact recursive ancestry is required in the UI.
3. Add replay-focused vertex guides after the replay focus flat-index contract
   is settled, because it touches replay state, overlay snapshots, and guide
   planning.

## Assumptions

- "Call site highlighted" means highlighting the source line containing
  `funcN(...)`, in addition to the function body line currently executing.
- "Call depth" means exposing nesting/recursion depth for commands produced by
  function expansion.
- Transform guides apply during vertex replay mode only in v1. Polygon replay
  mode remains unchanged.
- Existing untracked files `new_scene.c` and `parametric_torus_nested_for.c`
  are unrelated to this audit.
