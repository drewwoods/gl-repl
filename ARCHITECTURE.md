# REPL Architecture

## Overview

The immediate-mode REPL is now split across focused translation units instead
of one monolithic `repl_core.c`.

- `repl_core.c`: parser, normalization, flattening, execution, replay, example
  loading orchestration, and OpenGL initialization.
- `repl_search.c`: search state, match navigation, and search-mode keyboard
  handling.
- `repl_export.c`: fixed scaffold strings, init bootstrap tables, import/export
  translation, save/load, and code-panel dump helpers.
- `repl_editor.c`: editor state, undo/redo, selection, clipboard, commit
  helpers, feed-line entrypoint, and GLUT input handlers.
- `repl_eval.c`: expression parsing and evaluation.
- `ui_panels.c`: 2D code/search/help/config/variable-panel rendering and panel
  hit-testing.
- `scene_render.c`: 3D scene rendering, overlays, and replay HUD.
- `sample.c`: application entrypoint and GLUT callback wiring.

The public API is still `repl_core.h`. Cross-module runtime/test helpers live in
`repl_core_internal.h`. Shared globals and UI-visible state still live in
`sample.h`.

## Two-Level Command Model

The REPL keeps **source commands** and **flattened commands** as separate
arrays. Everything else (execution, replay, normal recomputation, overlays)
reads from the flattened array.

- `g_cmds[MAX_COMMANDS]` — source-level. One entry per line visible in the
  code panel. Holds parsed type/args, the normalized `source[]` text, and
  flags (`has_vars`, `valid`, `is_auto`). Editor mutations touch only this
  array.
- `g_flat_cmds[MAX_COMMANDS]` — expanded. For-loops unrolled, function calls
  inlined, if-conditions resolved. Each flat cmd carries `src_cmd_idx`
  (owning source line), `call_src_cmd_idx` (immediate call site), and
  `func_scope_mask` (active function scopes) so the editor can highlight
  the right source line when the cursor lands on a flattened command.
- **Rebuild trigger:** every mutation sets `g_flat_dirty = 1` (via
  `mark_normals_dirty()`); `flatten_commands()` rebuilds the flat array on
  the next frame before rendering.

Keeping the two levels separate means edits are cheap, execution reads a
flat stream, and replay can step by flat-command without worrying about
loop/function structure.

## Command Lifecycle

A single REPL line progresses through five stages, owned by different
modules:

1. **Input** — `repl_editor.c` accumulates characters into `g_input`.
2. **Commit** — `;` (or Enter in overwrite mode, or programmatic
   `feed_line()`) runs the commit handler chain (see
   [Structured block commits](#structured-block-commits) below).
3. **Parse** — `repl_parse_command()` in `repl_core.c` matches the line to
   a `CmdType`, evaluates argument expressions via `repl_eval.c`, and
   stores results into `GLCmd.args[]` / `GLCmd.source[]`.
4. **Flatten** — `flatten_range()` recursively expands source commands,
   capped at 100k visits and recursion depth 32.
5. **Execute** — `execute_commands()` walks `g_flat_cmds[]` and emits GL
   calls. Commands flagged `has_vars` are re-evaluated each frame so
   animated expressions (e.g. `t`) stay live.

Stages 1–2 live in `repl_editor.c`; 3–5 live in `repl_core.c`. This is the
load-bearing boundary — outside code that needs to inject commands should
do so through `feed_line()` rather than poking `g_cmds[]` directly, so
every path shares the same parse/normalize/flatten guarantees.

## Data Flow

### Edit and commit path

1. GLUT keyboard/mouse callbacks enter through `repl_editor.c`.
2. Editor commit helpers convert input text into commands by calling
   `repl_parse_command*()` or `repl_parse_and_normalize()` in `repl_core.c`.
3. Parsed commands are stored in `g_cmds[]`.
4. `mark_normals_dirty()` and `g_flat_dirty` invalidate downstream derived
   state.

### Execution path

1. `flatten_commands()` in `repl_core.c` expands loops, functions, conditionals,
   and variable-driven commands into `g_flat_cmds[]`.
2. `scene_render.c` prepares the frame and calls `execute_commands()`.
3. `execute_commands()` issues fixed-function OpenGL calls against the flattened
   command stream.

### Search path

1. `repl_search.c` scans `g_cmds[].source`.
2. Search hit movement calls `navigate_to_line()` from `repl_editor.c`.
3. `ui_panels.c` renders search highlights from the shared search globals.

### Import/export path

1. `repl_export.c` owns the fixed scaffold shown in the code panel and emitted
   to exported C.
2. `save_output()` writes scaffold text plus translated command bodies.
3. `load_from_file()` converts exported C back into REPL lines and replays them
   through `feed_line()` from `repl_editor.c`, so import uses the same commit
   rules as interactive editing.

## Ownership

### `repl_core.c`

Owns the semantic model and execution pipeline.

- `g_cmds[]`, `g_num_cmds`
- `g_flat_cmds[]`, `g_num_flat_cmds`
- flattening helpers and scope/depth caches
- parser tables and command normalization
- replay engine and replay stepping
- example orchestration and GL init

### `repl_editor.c`

Owns transient editor and interaction state.

- `g_input`, `g_input_len`, `g_cursor_pos`
- `g_edit_line`, `g_inserting`, newline buffer
- undo/redo ring
- selection and clipboard state
- code-panel scroll and resize state
- variable-drag/config-menu interaction state
- feed-line and block-commit helpers
- keyboard, special-key, mouse, motion, and timer callbacks

### `repl_search.c`

Owns source-text search state.

- `g_search_*`
- row/text lookup helpers
- next/previous match navigation
- search-mode key handling

### `repl_export.c`

Owns generated scaffold and import/export plumbing.

- `g_header_pre`, `g_render_state_lines`, `g_lookat`, `g_header_post`
- init bootstrap tables and helpers
- `save_output()`, `load_from_file()`
- import translation helpers
- code-panel dump helpers

## Shared State Rules

- `sample.h` remains the single shared runtime/UI header.
- `repl_core_internal.h` is the internal bridge for non-public helpers needed by
  tests or sibling `.c` files.
- The `CFG_DEFAULT_*` macro block in `sample.h` is also the shared source of
   truth for example-owned scene-presentation defaults. Reuse those macros from
   `repl_core.c` initializers, example reset helpers, and focused tests instead
   of duplicating literals.
- No extra per-module headers are required for the split.
- Parser/execution internals stay in `repl_core.c`; editor/search/export call
  into them rather than duplicating logic.

## Key Pipelines

### Structured block commits

`repl_editor.c` handles user-facing block syntax:

- `for(...) { ... }`
- `funcN(...) { ... }`
- `if(...) { ... }`
- closing `}`
- `float name[, ...];` declarations
- `name = expr;` assignments to predefined variables

Historically each dispatch site (`;` key, Enter in insert mode, Enter in
overwrite mode, `feed_line()`) open-coded the handler chain. That is now
consolidated into four helpers at the top of `repl_editor.c`:

- `try_commit_var_statements()` — float decl, then assign
- `try_commit_block_structs()` — close-brace, for, func, if
- `try_commit_any()` — both groups in canonical order
- `try_commit_var_statements_then_insert()` — var variant that flips to
  insert mode on success, used by the overwrite-mode Enter key

Adding a new statement handler means extending the right helper, not
chasing the call sites. Ordering within a helper is load-bearing:
`try_commit_float_decl` must precede `try_assign_variable` so that
`float x;` is not misread as an assignment to an identifier named
`"float"`.

These helpers update `g_cmds[]` directly, but they still rely on parser
and scope helpers from `repl_core.c` for validation and normalization.

### Float variable declarations

`float name[, ...];` lines commit to a dedicated `CMD_VAR_DECLARE` entry
rather than running through the general parser. Two structural choices
live here:

- **Placement rule:** new declarations are inserted at the top of
  non-decl code (index of the first non-`CMD_VAR_DECLARE` cmd), regardless
  of cursor position. That guarantees every reference in source order
  follows its declaration, so init expressions only see predef vars
  declared above them. Editing an existing decl still overwrites in
  place.
- **No-op at execution time:** `execute_commands()` and `flatten_range()`
  skip `CMD_VAR_DECLARE`. Registration into `g_predef_vars[]` happens at
  commit time via `declare_predef_var()` in `repl_eval.c`, so the
  evaluator sees the variable before the flat stream references it.

`delete_cmd_range()` in `repl_editor.c` refuses to remove a declaration
whose name is still referenced elsewhere (via `source_uses_ident()` in
`repl_eval.c`).

### Editing existing lines

Navigating onto an existing line loads `g_cmds[i].source` into `g_input`
with the trailing `;` and whitespace stripped, so re-committing goes
through the no-semicolon code path. Every commit handler that looks for
`;` must also accept end-of-string as a valid terminator. This invariant
is shared by all dispatch sites.

### Undo / redo

`repl_editor.c` owns fixed-size circular buffers of editor snapshots.

- `UndoSnapshot` captures `g_cmds[]`, `g_num_cmds`, `g_edit_line`, and
  `g_predef_vars[]` values — enough to restore the full editor state.
- Any mutation (delete, paste, reformat, etc.) calls
  `push_undo_snapshot()` before changing state. Pushing clears the redo
  stack, which is the usual "diverged history" rule.
- Ctrl+Z pops the undo stack and moves the current state to the redo
  stack; Ctrl+Y does the reverse.

### Flattening

`flatten_range()` in `repl_core.c` is still the only place that expands:

- `CMD_FOR_BEGIN .. CMD_FOR_END`
- `CMD_FUNC_DEF .. CMD_FUNC_END`
- `CMD_IF_BEGIN .. CMD_IF_END`
- variable-dependent commands

That preserves a single semantic source of truth for execution, replay, and
export-derived behavior.

### Transform edit guides

When the cursor sits on a committed `glTranslatef`, `glRotatef`, or
`glScalef` line, `scene_render.c` renders an overlay showing that
command's effect. Guides live in the vertex-dots pass because it already
walks `g_flat_cmds[]` and tracks the matrix stack.

Gating: `g_show_guides` is on, `!replaying`, `g_cmds[g_edit_line].valid`,
and the input buffer still matches the normalized committed source
(mirrors the `unmodified` check in `repl_editor.c` so mid-keystroke
edits suppress the guide).

Flat-cmd scan: the matching flat cmd is found by flat execution order
— **not** by comparing `src_cmd_idx > g_edit_line`. Function-call
expansions carry the callee's `src_cmd_idx`, so a numeric comparison
would skip past them. Instead, locate the first flat cmd with
`src_cmd_idx == g_edit_line`, then take the next valid flat cmd whose
`src_cmd_idx` differs as the start of the post-cursor walk.

The guide's starting point is `p_after = M_after · origin`, where
`M_after` is the product of transforms that come after the cursor in
execution order, up to (but not including) the first geometry-emitting
command (`is_geometry_emit_cmd`: `CMD_BEGIN`, `CMD_GLU_*`,
`CMD_GLUT_TORUS`, `CMD_TESS_BEGIN_POLYGON`). Stopping at the first
emit prevents transforms following an intervening draw from bleeding
into the guide.

Two render modes, chosen via the `g_xform_guide_mode` config toggle
("Xform guide mode"):

- **World (0, default)** — render in world axes at world origin.
  Matches strict OpenGL reverse-order semantics: for cursor command
  `C_k`, vertices are computed as `M_1 · M_2 · ... · M_n · v`, so
  `C_k` acts on the point `M_after · origin`. Pre-cursor transforms
  wrap this sub-expression later and don't move the guide. The
  camera-view matrix is snapshotted before any user transforms and
  reloaded via `glLoadMatrixf(tg_cam_view)` when drawing the guide.

- **Frame (1)** — render at a scene-world anchor derived from the
  **full pre-cursor modelview** (translations, rotations, and
  scales). The anchor is computed by
  `compute_before_cursor_origin()`, which walks all flat cmds
  before the cursor in a fresh identity matrix (via
  `apply_tracked_transform_cmd` so push/pop scopes correctly) and
  reads `(m[12], m[13], m[14])` — i.e. where the pre-cursor
  modelview places the origin. Only the position is used; the
  guide itself is still drawn with world-axis orientation
  (`glTranslatef(anchor)` after `glLoadMatrixf(tg_cam_view)`), so
  arrows/arcs still read along world axes. This lines up the
  anchor with geometry rendered by prior `func0()` / draw calls
  even when pre-cursor rotations rotate the sub-frame.

Per-command helpers:

Shared visual style: shafts and arcs use an **axes-pulse-style**
overlay — a dim solid base line (or arc) at `alpha≈0.30`, with a
bright `sin(π·phase)` dot sweeping `a→b` and a short trail behind
it, driven by `g_anim_time`. Helpers live alongside the per-command
helpers: `xform_axis_color()` maps `(|x|,|y|,|z|)/max` → RGB, and
`draw_pulse_segment()` renders the dim base + traveling dot + trail
for a straight segment. The rotate guide inlines an arc-based
variant that walks the pre-sampled Rodrigues arc points.

Per-command helpers:

- `draw_translate_guide` — pulse shaft from `p_after` to
  `p_after + (tx,ty,tz)` with a 4-fin arrowhead at the tip. Shaft
  color comes from the translation vector via `xform_axis_color`;
  arrowhead uses a brighter tint of the same color.
- `draw_rotate_guide` — axis stub through the local origin plus a
  48-segment arc swept by Rodrigues rotation. Arc, axis stub, and
  endpoint dots are tinted from the rotation axis
  (`xform_axis_color(ax,ay,az)`). The pulse dot sweeps along the
  arc; the trail is a short strip of arc samples between the
  trail-phase and dot-phase points so curvature is preserved. The
  on-axis degenerate case (where `p_after` lies on the rotation
  axis) is not specially handled — the arc simply collapses.
- `draw_scale_guide` — pulse shaft from `p_after` to
  `(sx·x, sy·y, sz·z)` with arrowhead. Shaft color comes from
  `xform_axis_color(sx-1, sy-1, sz-1)` so the color highlights the
  axes that deviate from identity. Degenerate case (`p_after` at
  origin) falls back to a 3-axis gizmo: gray unit reference segment
  plus a pulsing arrow per axis in that axis's own color (X=red,
  Y=green, Z=blue) to `(sx,0,0)`, `(0,sy,0)`, `(0,0,sz)`.

Adding a new transform-guide type: extend the cmd-type gate in the
setup block and the dispatch switch at the draw site in
`render_3d_scene()`, and add a new `draw_<name>_guide` helper
alongside the existing three.

### Replay

Replay state and stepping remain in `repl_core.c`, but editor callbacks in
`repl_editor.c` drive it.

- editor input toggles replay modes and stepping
- replay helpers rebuild source-line highlighting state
- `scene_render.c` consumes replay state to draw the HUD and replay overlays

Under the hood, replay works by clamping how much of `g_flat_cmds[]`
`execute_commands()` will emit on each frame:

- `g_replay_state` is OFF / PLAYING / PAUSED / DONE; `g_replay_pc` is a
  program counter into `g_flat_cmds[]`; `g_replay_speed` is a playback
  multiplier.
- During playback, `g_num_flat_cmds` is clamped to `replay_exec_limit()`
  so only commands up to the PC contribute to the fill pass.
- `g_replay_fade_batches[]` is a circular buffer of recent geometry
  snapshots. Old batches fade out as new geometry appears and are drawn
  in a separate blended pass (`execute_replay_fade_batches()`) after the
  main fill. This is what produces the trailing-ghost look without
  changing how the main executor walks the flat array.

## Startup and Examples

`load_initial_commands()` remains in `repl_core.c`.

- If an import file is provided and `load_from_file()` succeeds, startup uses
  the imported session.
- Otherwise core loads a built-in example.
- `load_example_lines()` performs a metadata pre-pass before feeding the
   remaining source through `feed_line()`.
- Built-in examples can begin with contiguous leading `// @cfg slug = value`
   lines, followed optionally by a 5-line `// camera` preset block.
- Leading example `@cfg` lines are parsed through
   `parse_workspace_header_line()` from `repl_export.c`, but only for
   scene-presentation slugs allowed by the example loader.
- That leading metadata is consumed as loader-only state and does not appear in
   the code panel.
- On each example load, core resets the allowed non-camera scene-presentation
   settings to the `CFG_DEFAULT_*` values from `sample.h` before applying any
   leading example `@cfg` metadata. This keeps unspecified settings from leaking
   between examples.
- Camera is intentionally excluded from that reset; examples keep the current
   `g_cam_*` state unless the explicit `// camera` block is present.
- `restore_user_scene()` still restores commands and predefined variables only,
   so leaving an example does not restore prior camera or presentation state.
- Example geometry lines are still committed through `feed_line()`, so
   examples, imported files, and interactive editing share the same commit
   pipeline once loader metadata has been consumed.

## Test Orchestration

The Makefile builds each standalone `test_*` binary normally, then delegates
suite execution to `scripts/run-tests.sh`.

- `TEST_BINS` is the canonical ordered list for the full suite.
- `CORE_TEST_BINS` is derived from `TEST_BINS` by filtering out the lightweight
  standalone tests (`test_eval`, `test_format`, and `test_repl_audio`).
- Per-test `*_OBJS`, `*_LDLIBS`, and `*_RUN` variables feed a single
  `.SECONDEXPANSION` link recipe for all test binaries.
- `TEST_RUNNER_CASES` is generated from `TEST_BINS` and each test's `*_RUN`
  command, so build output, replayed logs, and final summaries stay in one
  predictable order.
- `make test` and `make test-detailed` export shared environment needed by the
  example export/compile tests, then call the runner.
- `TEST_JOBS=N` limits concurrent test binaries. Leaving it empty runs all
  test binaries concurrently.

The runner prevents interleaved output by redirecting each test binary's
stdout/stderr into `build/test-logs/run-*/<name>.log`, recording its exit status
beside that log, and replaying logs in Makefile order after all launched jobs
finish. This keeps parallel execution fast while preserving the readable
per-suite transcript expected from the old serial runner.

Global stats are collected from the final `passed/total` line emitted by each
test binary. The runner reports both binary-level status and assertion-level
totals, for example:

- `test binaries: 14 total, 14 passed, 0 failed`
- `tests: 1501 total, 1501 passed, 0 failed`

If a binary exits nonzero, the runner still prints its buffered log and includes
the binary in the global failure count. If a binary lacks a parseable
`passed/total` summary, it is counted at the binary level and called out as an
unknown assertion-count source.

Coverage builds intentionally run tests serially via `make test BUILD=coverage
TEST_JOBS=1`. Do not parallelize coverage unless the coverage output paths are
made safe for concurrent `.gcda` writes.

## Extension Guide

### Add a new command

1. Add the `CmdType` in `sample.h`.
2. Add the matching name to the `cmd_type_name()` table in `repl_core.c` in
   the same order as the enum — `repl_debug_dump_editor()` and
   `repl_debug_dump_flat_commands()` index into it, so a missing or
   misordered entry silently mislabels every subsequent command in the
   debug dump (see commit `abccf5c` for `CMD_FRONT_FACE`).
3. Extend parser handling in `repl_core.c`.
4. Extend execution handling in `repl_core.c`.
5. Extend flattening if the command affects control flow or local scope.
6. Extend export/import in `repl_export.c` if the command must round-trip.
7. Extend autocomplete or editor affordances in `repl_core.c` /
   `repl_editor.c` if needed.

#### Impact of missing new command in `cmd_type_name()`
1. Every type name after CMD_VAR_DECLARE in the enum would be shifted by one.
   The names[] array is positional — indexed by the integer value of CmdType.
   With the entry missing, CMD_LABEL (enum value 27) would print as
   "CMD_VAR_DECLARE", CMD_GOTO as "CMD_LABEL", CMD_GLU_SPHERE as "CMD_GOTO",
   and so on down the line. Debug dumps would show the wrong type for ~18
   command types.
2. Out-of-bounds read on the last enum value. The bounds check at
   repl_core.c:751 is if (t >= 0 && t < CMD_TYPE_COUNT), which allows index
   CMD_TYPE_COUNT - 1 (i.e. CMD_CLEAR_COLOR). But the array only had
   CMD_TYPE_COUNT - 1 elements, so that access reads one past the end —
   undefined behavior, likely garbage or a crash.

No effect on rendering, parsing, or command execution — cmd_type_name is only
called from repl_debug_dump_editor() and repl_debug_dump_flat_commands(). So
the REPL would work fine until someone hit Ctrl+D to debug, at which point the
output would be misleading (wrong names) and potentially crash on the last
command type.

### Add a new editor interaction

1. Add state and handlers in `repl_editor.c`.
2. Add rendering or hit-testing in `ui_panels.c` if the feature is visible.
3. Only promote new helpers into `repl_core_internal.h` when another module
   truly needs them.

### Add a new exported scaffold feature

1. Add or update the scaffold strings in `repl_export.c`.
2. Keep the code-panel preview and file exporter using the same source of
   truth.
3. Preserve round-tripping through `load_from_file()` whenever possible.

### Add a new predefined variable

There are two variable classes in the REPL:

- predefined globals stored in `g_predef_vars[]`
- scoped locals introduced by `for(...)` loop indices and `funcN(...)`
  parameters

Top-level assignments like `foo = 1;` only target predefined globals. Loop
indices and function parameters are collected as local `ExprVar` scopes during
flattening; they are not added to the global variable table.

To add a new predefined global:

1. Increase `MAX_PREDEF_VARS` in `repl_eval.h` if you need more slots.
2. Add the new variable name to the `names[]` array in `init_predef_vars()` in
   `repl_eval.c`.
3. Keep names within the `ExprVar.name[16]` storage limit.
4. If you add many globals, also check `MAX_WORKSPACE_HEADER_LINES` in
   `sample.h`, because workspace save/load persists globals through `// @var`
   header lines.

Existing export/import and persistence paths already iterate
`g_predef_vars[]`, so new predefined globals automatically:

- participate in expression evaluation
- round-trip through workspace headers in `repl_export.c`
- emit as file-scope globals in exported C
- reset in exported `reset_repl_vars()`

Special case: `t` is wired into animation/time helpers in `repl_core.c`. New
variables are plain globals unless you add similar runtime plumbing.

## Current Split Intent

The split is structural, not behavioral.

- `repl_core.h` stays stable.
- Editor, search, export, and execution each have a clear home.
- Import/export and example loading still go through the same command-commit
  path used by live editing.
- Tests remain the behavioral contract for the refactor.
