# REPL Refactor Punch List

## Context

The companion doc `feature/repl-cleanup.md` is the strategic 10-stage
ownership-reorganization plan. This punch list is the tactical
counterpart: a prioritized set of concrete, behavior-preserving
extractions that can each land as a single reviewable commit *today*,
without committing to the larger context-object rewrite.

Four files still dominate the codebase: `scene_render.c` (2861),
`repl_export.c` (2541), `repl_core.c` (~1958), `repl_editor.c`
(1688). `ui_panels.c` dropped to 1237 LoC (from 4452) after Phase-7
extractions: help overlay, variable panel, autocomplete popup, and
inline rename. The highest-value remaining refactors
fall into two camps:

1. **Mechanical extractions** — self-contained features still living in
   the wrong file. Low risk, immediate LoC reduction, no behavior
   change.
2. **Pattern consolidation** — repeated boilerplate (per-theme switch
   cases, per-overlay traversal loops, repeated GL-pass emit blocks)
   that hurts every time someone adds a new theme/overlay.

This list is ordered by impact-per-effort. Pick one and execute it
in isolation; each item is sized to land as one `refactor:` commit.

> **Baseline note:** `make test-stubs TEST_JOBS=4` currently passes
> all 17 suites / 2315 tests cleanly. The "3 failing suites" caveat
> in `feature/repl-cleanup.md` is obsolete. Any new failure introduced
> by a punch-list item is a real regression.

---

## Tier 1 — Mechanical extractions (hours each, near-zero risk)

### 1. Extract help overlay → `repl_help_overlay.c` ✅ DONE

Landed as `refactor: extract help overlay` on branch
`immediate-mode-repl/repl-cleanup-2`. `render_help()` plus its
`_HELP_STR`/`_HELP_STR2` macros moved out of `ui_panels.c` (which
shrunk from 2051 → 1610 LoC). State globals (`g_show_help`,
`g_help_tab`, `g_help_scroll`) stayed in `repl_state.h` because they
are mutated from `repl_editor.c` and `repl_search.c`.

### 1b. Extract variable panel → `repl_variable_panel.c` ✅ DONE

Landed as `refactor: extract variable slider panel`. `render_var_panel()`,
`var_panel_rect()`, `var_panel_hit()`, the asinh slider math, and the
replay-lift easing state moved out of `ui_panels.c` (1610 → 1399 LoC).
The renderer is read-only on `g_predef_vars`; mutation stays with the
editor's drag handler (`g_drag_var` in `repl_editor.c`), satisfying
the "keep variable mutation outside the renderer" constraint.
`render_scene_status()` got its own section header in `ui_panels.c`
since the var-panel section that previously housed it is gone.

### 1c. Extract autocomplete popup → `repl_autocomplete_panel.c` ✅ DONE

Landed as `refactor: extract autocomplete popup renderer`. Pairs with
the existing model-only `repl_autocomplete.c`: the new module owns
*only* the floating popup render path; `repl_autocomplete.c` keeps
match building, selection state, ghost text, and parameter hints.
`ui_panels.c` 1399 → 1327 LoC. Inline ghost/hint text drawn next to
the input line stays in `ui_panels.c` (it needs the surrounding
code-panel row layout). Public entrypoint uses the newer module-
prefix convention: `repl_autocomplete_panel_render()` (replacing
the bare `render_autocomplete()`).

### 1e. Extract variable dragging → `repl_var_drag.c` ✅ DONE

Landed as `refactor: extract variable slider drag`. Closes the
REPL_REFACTOR_MAP open edge *"repl_editor.c still owns variable
slider dragging."* The four `g_drag_*` globals (storage) move to
the new file; the externs in `repl_state.h` and the `ReplUiState`
catalog entries in `repl_state.c` are unchanged, so the state-
access contract is preserved. Public API uses the newer prefix
convention: `repl_var_drag_begin(row, log_mode, x)`,
`repl_var_drag_motion(x)`, `repl_var_drag_reset()`,
`repl_var_drag_active()`, `repl_var_drag_active_var()`,
`repl_var_drag_log_mode()`. `repl_editor.c`'s mouse_func and
motion_func now call the API instead of touching state directly;
`repl_variable_panel.c` reads drag state through the accessors.
The value-writeback logic (linear/log mapping, `g_predef_vars`
write, matching `CMD_VAR_ASSIGN` source sync, `g_flat_dirty = 1`)
is now in one place instead of split across mouse-begin and
motion handlers.

### 1d. Extract inline rename → `repl_inline_rename.c` ✅ DONE

Landed as `refactor: extract inline scene rename`. Rename state
(`g_rename_slot`, `g_rename_buf`, `g_rename_len`), the filesystem-
safe char filter, and the Enter/Esc/backspace/printable key handlers
moved out of `ui_panels.c` (1327 → 1237 LoC). The five public
entrypoints were renamed `ui_panels_*` → `repl_inline_rename_*` to
match the newer module-prefix convention (`_active`, `_begin`,
`_cancel`, `_handle_key`, `_handle_special`). Callers updated in
`repl_editor.c`, `repl_actions.c`, and two test files
(`test_repl_editor.c`, `test_repl_core_extra.c`). Rename has no
dedicated render pass — the buffer is surfaced through
`set_status()` into the existing status strip, so this is an input
module, not a panel; that matches the model/render split rule
(no OpenGL calls in the new file).

### 2. Extract color picker → `ui_color_picker.c`

- **File:lines:** `ui_panels.c:2113–2445` (~250 LoC: state globals,
  HSV helpers, `render_color_picker()`, `color_picker_press()`,
  `color_picker_motion()`, `color_picker_write_cmd()`)
- **Why:** Self-contained widget. State globals are already
  prefix-isolated (`g_cp_*`). HSV math is unique to this widget.
  Touches no other UI module.
- **Extract:** All `g_cp_*` globals, `cp_hsv_to_rgb`, `cp_rgb_to_hsv`,
  `cp_ring`, the four `color_picker_*` entry points, write-back logic.
- **Verify:** `make test-stubs`, then `./sample`, open the color
  picker via its trigger, drag through HSV, confirm `glColor*` call
  commits back to the buffer.

### 3. Extract menu/dropdown rendering → `ui_menu.c`

- **File:lines:** `ui_panels.c:1225–1540` and `3241–3353`
  (menubar rects, hit-tests, dropdown rect/item-hit, item label/
  shortcut/activate dispatch, example dropdown render — ~280 LoC)
- **Why:** Menu state machine is a coherent island. `repl_actions.c`
  already owns the side-effects; the rendering+layout+hit-test still
  lives in `ui_panels.c`. Pulling them out closes the
  `REPL_REFACTOR_MAP.md` "Open Edge" labelled *"ui_panels.c still
  owns menu layout, hit testing."*
- **Extract:** `g_open_menu` state, `menubar_rects`,
  `menubar_menu_hit`, `menubar_pin_hit`, `menu_dropdown_rect`,
  `menu_dropdown_item_hit`, `menu_item_count/label/shortcut/activate`,
  `render_example_dropdown`.
- **Verify:** Click each top-level menu (File/Examples/Config), verify
  dropdown geometry; click a pinned button (Replay/Scene); click
  outside to dismiss.

---

## Tier 2 — Pattern consolidation (1–2 days, medium impact)

### 4. Data-drive grid + axes themes in `scene_render.c`

- **File:lines:**
  - `draw_grid()`: `scene_render.c:23–540` (~517 LoC, 9-way switch on
    `GridTheme`)
  - `draw_axes()`: `scene_render.c:556–870` (~314 LoC, 5-way switch on
    `AxesTheme`)
- **Why:** The CLAUDE.md "Adding Grid/Axes Themes" recipe currently
  tells contributors to copy a switch case. Each grid case repeats
  the same `for (float v = -extent; v <= extent; v += step)` outer
  loop, varying only the per-vertex color/alpha and the origin-axis
  highlight. Each axes case repeats the X/Y/Z draw three times with
  different color tuples. ~400 LoC of structural duplication.
- **Extract pattern:** `struct GridTheme { void (*color_fn)(float v,
  float extent, float rgba_out[4]); int draws_origin_axes; ... };`
  array indexed by enum. Single `draw_grid_lines(theme)` consumes
  the table. Same shape for `AxesTheme`. Adding a theme becomes
  "add an entry to the table" instead of "add a switch case."
- **Verify:** F3 cycles every grid theme, F4 cycles every axes theme,
  visual A/B against current build for each. **Needs eyes on the
  screen** — diff-only review will not catch a per-theme glow
  regression.

### 5. Vertex overlay visitor in `scene_render.c`

- **File:lines:** `draw_vertex_numbers()` (871–924), `draw_normal_vectors()`
  (925–978), `draw_vertex_guides()` (1050–1123), `draw_normal_guides()`
  (1124–~1250). All four open with the same boilerplate: push matrix,
  walk `g_flat_cmds[]`, track transforms, detect block boundaries,
  extract vertex/normal slots, pop matrix.
- **Why:** ~150 LoC of identical traversal scaffolding. Adding a fifth
  overlay (e.g. tangent vectors) means copy-pasting the loop again.
- **Extract:** `void overlay_walk_vertices(void (*on_vertex)(const
  GLCmd *cmd, const overlay_state *state, void *user), void *user)`.
  Each overlay becomes a small callback.
- **Verify:** Toggle each F-key overlay and confirm pixels match
  current build.

### 6. Workspace header read/write symmetry in `repl_export.c`

- **File:lines:** `save_output()` (~2230–2329) emits `@var`/`@cfg`/
  `@scene-name`/`@workspace-dir` headers; `load_from_file()`
  (~2331–2530) parses them. The directive set is duplicated by hand
  on both sides — a header added on one side without the other goes
  silently unused.
- **Why:** Save/load asymmetry is a class of bug that's invisible at
  diff-review time. A `WorkspaceHeader` struct + paired
  `read_workspace_header()` / `write_workspace_header()` makes the
  directive vocabulary explicit and forces both directions to stay
  in sync.
- **Verify:** `make test_repl_core_io`, then save → load round-trip
  every example via `./sample`, confirm camera/cfg/scene-name
  preserved.

---

## Tier 3 — Closing REPL_REFACTOR_MAP open edges

### 7. Extract variable dragging → `repl_var_drag.c`

- **File:lines:** `repl_editor.c:72–75` (state globals), `1341–1425`
  (LMB/RMB init), `1490–1525` (motion + writeback), plus
  `ui_panels.c:4023–4060` (read-only display references)
- **Why:** `REPL_REFACTOR_MAP.md` explicitly lists this as an open
  edge: *"repl_editor.c still owns variable slider dragging."* The
  state is `g_drag_var`, `g_drag_log_mode`, `g_drag_start_val`,
  `g_drag_start_x` — a complete micro-feature with no business in
  the input router.
- **Extract:** Owns the four `g_drag_*` globals; exports
  `repl_var_drag_begin(row, log_mode, x)`,
  `repl_var_drag_motion(x)`, `repl_var_drag_active()`,
  `repl_var_drag_active_var()`, `repl_var_drag_log_mode()`,
  `repl_var_drag_reset()`. Editor and `ui_panels.c` call through
  these instead of reading the globals.
- **Verify:** `make test-stubs`, then drag variable sliders in the
  UI with LMB (linear) and RMB (log), confirm writeback into the
  source line.

### 8. Extract `parse_command()` → `repl_parser.c`

- **File:lines:** `repl_core.c:886–1469` — 600-line giant switch on
  command string, evaluating arg expressions, populating `GLCmd.args`.
  Hot path for every keystroke that commits.
- **Why:** `repl_core.c` is supposed to be "parser, normalization,
  display, GL init, depth queries." Two of those (`flatten`,
  `executor`) already moved out. The parser itself — by far the
  biggest piece left — should follow. Purely organizational (no
  behavior change, no signature change for the public
  `repl_parse_command`), but it isolates 600 lines of a critical
  path into its own module that can be tested and reasoned about
  independently. Aligns with stage 4 of `repl-cleanup.md`.
- **Extract:** `parse_command()` and any static helpers it owns
  exclusively. `repl_core.c` keeps the public `repl_parse_command()`
  shim if needed for back-compat, otherwise the new module exports
  it directly.
- **Verify:** `make test_repl_core_parse`, `make test_repl_core_commit`,
  `make test_eval` (no new failures vs. baseline). Smoke-test
  `./sample` with each example.

---

## Not recommended for now

- **Global state clustering** (group `repl_state.h`'s 127 externs into
  context structs): high long-term value but enormous churn surface.
  This is what stage 2 of `repl-cleanup.md` plans more carefully —
  defer to that effort rather than attempting it as a punch-list
  item.
- **Keyboard dispatch table** (collapse the 22 `handle_*_key_route`
  functions in `repl_editor.c` into a table): the chain *is* the
  ordering policy and reading it sequentially is fairly clear today.
  Lower payoff than items 1–8.

---

## Verification umbrella

For any item picked:

1. `make test-stubs TEST_JOBS=4` — no *new* failures vs. the 3-suite
   baseline noted above.
2. `make sample && ./sample` — load an example, exercise the
   touched feature, confirm no visual or interactive regression.
3. For Tier 2 rendering changes (items 4 & 5), do an explicit A/B
   against the pre-refactor binary on every theme/overlay — diff
   review alone is not enough for pixel-level equivalence.
4. Single commit per item, conventional `refactor:` prefix per
   project CLAUDE.md.
