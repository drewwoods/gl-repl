# Decoupling repl_demo from gl-repl — execution plan

> Supersedes the chokepoint-numbered draft. Same six chokepoints, but
> reordered as an execution sequence and with maintainer corrections
> applied (stub count, reset rename, non-editor commit API as the
> target, config split by owner as the endpoint).

## Baseline

`tools/repl_demo` was introduced in commit `dded827` as the REPL-pipeline
analog of `tools/teapot_demo`: prove that parse → command store →
flatten → execute can build and run without the editor input dispatch,
the controller, or the UI. The link set today is **30 dependency TUs +
`repl_demo.c` + `stubs.c` = 32 object files** (vs. 64 for the full
sample). `tools/repl_demo/stubs.c` defines **17 externally-visible
symbols** (15 functions + 2 data: `g_cfg_items[]` and `CFG_ITEM_COUNT`)
verified via `nm build/release-gl-stubs/tools/repl_demo/stubs.o`. The
commit message's "12 no-op shims" undercounted; the real number is 17.

The 17 stubs cluster into six chokepoints. Each chokepoint is one
concrete misplacement or unfinished refactor in the existing code, not
a structural problem with the pipeline itself. The ordering below is
chosen so each step lands without depending on the next.

| Step | Chokepoint | Stubs cleared | Effort | Feasibility |
|---|---|---|---|---|
| 1 | `repl_compile_dispatch` location | 1 | Low | High — known TODO in source comment |
| 2 | UI chrome sync + full-world reset out of `repl_state.c` | 5 | Low | High — pure layering inversion |
| 3 | Pipeline `set_status()` → diagnostic sinks | 1 | Medium | High — pattern already proven in `repl_parser.c` |
| 4 | Split config catalog vs live-app mutation by owner | 8 | Medium | Medium — needs a real ownership decision |
| 5 | Non-editor source-load / commit API for examples + imports | 1 (`feed_line`) | Medium-High | Medium — must support multi-line block structures |
| 6 | Split `repl_core.c` / `repl_export.c` to remove last editor-shaped helpers | 1 (`load_line_to_input`) | Medium | Medium — overlaps with R10-phase2..5 in ARCHITECTURE.md |

Totals reconcile: 1 + 5 + 1 + 8 + 1 + 1 = **17 stubs**.

---

## Step 1 — Move `repl_compile_dispatch` into the REPL compile module

**Why first.** This is the lowest-risk move and clears one of the most
embarrassing edges: a "pure validation" module that has a hard
forward-reference into editor services.

**Current state.** `repl_compile.c:43-65` carries an explicit comment
acknowledging the misplacement:

> the dispatch function is grammar-side; its current location is a
> historical artifact and should migrate to repl_compile.c in a
> follow-up.

The body lives in `src/editor/services.c`. `repl_compile_toggle_comment`
in `repl_compile.c` calls it as a fallback, which is what makes the
demo stub a hard reference rather than a weak symbol.

**Fix.** Move the body of `repl_compile_dispatch` from
`src/editor/services.c` into `repl_compile.c`. Drop the forward
declaration. Confirm the demo's stub goes away (rebuild without it,
expect a *redefinition* error if the move was successful).

**Stubs cleared:** `repl_compile_dispatch`.

---

## Step 2 — Move UI chrome sync and full-world reset out of `repl_state.c`

**Why now.** Both moves are mechanical and both operate on
`repl_state.c` as their source file, so doing them together avoids two
churn passes on the same TU. Together they delete five stubs.

### 2a. Rename and split the reset chokepoint

**Current state.** `repl_state.c:572-595` defines `repl_state_reset_all()`
which resets REPL state *and* drives four non-REPL owners:
`ui_state_reset`, `variable_panel_state_reset`,
`editor_help_session_reset`, `repl_editor_reset_transients`. The last
of those itself reaches into five subsystems (`editor_commit_reset_transients`
+ `glr_camera_controls_reset` + `ui_menu_bar_close` +
`color_picker_close` + `glr_ctrl_router_reset_code_panel_drag`).

**Fix — rename, do not preserve the misleading name.**

- Rename `repl_state.c::repl_state_reset_all` →
  `repl_state_reset_program`. New scope: REPL slices only (program,
  predef vars, scenes, autonormal, source scope, eval predef storage,
  flat dirty flag, autocomplete provider registration). No peer / UI /
  editor calls remain.
- Introduce `glr_app_reset_all` (in `glr_ctrl.c` or a new
  `glr_app.c` if a top-level shell module is wanted) that calls
  `repl_state_reset_program()` *and* the four peer/UI/editor resets.
  This is the function the live sample calls; the demo never calls it.
- Update every call site of `repl_state_reset_all` accordingly. Most
  are in tests; the production `repl_state_init_defaults` switches to
  `glr_app_reset_all` (or stays calling `repl_state_reset_program` if
  it's already past the controller boundary).

The rename is load-bearing: `repl_state_reset_all` *should not* mean
"REPL only" — it would be a foot-gun for any future caller that picks
the name based on what they want it to mean. The user-visible new
names express scope honestly.

### 2b. Move `repl_state_sync_ui_chrome` to the controller

**Current state.** `repl_state.c:556-561` mirrors
`ReplPresentationState.code_panel_layout` and `show_vertex_indices`
into `UiState.code_panel`. That's a REPL module reaching forward into
UI state — wrong direction. It is the only reason
`ui_state_code_panel_mut` is stubbed.

**Fix.** Move the body of `repl_state_sync_ui_chrome` to `glr_ctrl.c`.
The controller is the layer authorized to read REPL and write UI; it
is already where `glr_ctrl_build_ui_snapshot` does similar mirroring.
After the move, `repl_state.c` contains zero UiState symbol references.

**Stubs cleared by step 2:** `ui_state_reset`,
`variable_panel_state_reset`, `editor_help_session_reset`,
`repl_editor_reset_transients`, `ui_state_code_panel_mut`. (Five.)

---

## Step 3 — Replace pipeline `set_status()` calls with diagnostic sinks

**Why now.** Steps 1–2 don't depend on this; this one depends on
nothing. After it, `set_status` itself can move out of `repl_core.c`
entirely, which clears the path to the bigger split in step 6.

**Current state.** `repl_core.c:64-72` defines `set_status()` as a
forwarder to `ui_state_status_set`. Pipeline modules call it from 9
sites:

- `repl_core.c:364` — normalize parse error
- `repl_core.c:756` — startup banner ("Ready - type GL commands…")
- `repl_executor.c:559` — `goto: loop limit reached`
- `repl_flatten.c:639` — flatten error
- `repl_export.c:2937, 2957, 3188` — export I/O errors

The architecture already documents the cleaner pattern
(`ARCHITECTURE.md` lines 866-868): `repl_parser.c` writes diagnostics
into `ReplParseContext.err_buf`, the parser core has zero `set_status`
calls, and `check-no-set-status-in-repl-parser` enforces that at
**0/0**.

**Fix.** Apply the same pattern to executor, flatten, and export:

- `repl_execute_program`: extend `ReplExecutionOptions` with an
  optional `char *err_buf` / `int err_sz` (or a callback). The runtime
  goto-limit error writes there instead of calling `set_status`.
- `repl_flatten_commands`: return a status string in the existing
  `ReplFlattenResult` (already returned by `repl_flatten_commands`);
  delete the inline `set_status(result.status)`.
- `repl_export_save_output` / `repl_export_load_from_file`: return
  / out-param the diagnostic message instead of writing it directly.
- `repl_core.c::set_status` startup banner moves to whatever calls
  `repl_state_init_defaults` (likely the controller); remove it from
  the pipeline TU.
- After all 9 sites are converted, delete `set_status()` and its
  thunk from `repl_core.c`. Add `check-no-set-status-in-pipeline`
  guarding the executor / flatten / export TUs (analog of the existing
  parser guard).

**Stubs cleared:** `ui_state_status_set`.

---

## Step 4 — Split config catalog from live-app mutation, by owner

**Why now.** Steps 1–3 don't touch `glr_config.c` or its callers, so
this can land independently. It is also the one with the largest
single stub-count payoff.

**Current state.** `glr_config.c` is in the demo link set because
`repl_export.c` (`@cfg` save/load) and `repl_scenes.c` (per-scene cfg
snapshots) call `glr_config_get/set/items`. `glr_config.c::config_value_ptr`
is one big switch over every cfg key, so it transitively forces every
config-target module into the link set:

| Stub | Pulled in by |
|---|---|
| `g_cfg_items[]` / `CFG_ITEM_COUNT` | Defined in `glr_actions.c` (not linked) |
| `audio_get_cfg_mode` / `audio_set_cfg_mode` | `GLR_CONFIG_AUDIO_MODE` arm calls `audio.c` |
| `variable_panel_view_mut` | `GLR_CONFIG_VARIABLE_PANEL` arm reaches into peer state |
| `ui_state_profile_panel_mut` | `GLR_CONFIG_CPU_PROFILE` arm reaches into UiState |
| `ui_state_viewport`, `ui_state_code_panel` | `src/ui/layout.c` reads UiState (in link set) |

**Fix — split by owner; do not stop at a `*_value_ptr` cosmetic split.**

The cheap intermediate (split `config_value_ptr` into a REPL-only and
glr-only variant) is acceptable as a stepping stone but **must not be
the endpoint** because it preserves one mixed-owner config module.

The endpoint is two descriptor catalogs:

1. **`repl_config` catalog.** Owns the keys backed by `ReplState`
   slices: presentation flags (wireframe, grid theme, axes theme,
   backdrop, vertex labels/normals/outlines/points/guides, autonormal,
   highlight current poly, show light indicators, code-panel layout,
   wrap at comma) and the auto-time / replay flags that already live
   on REPL-owned subsystems. Lives next to `repl_state` /
   `repl_presentation`. The pipeline (`repl_export.c`, `repl_scenes.c`)
   iterates *only* this catalog when reading/writing `@cfg` directives.
2. **`glr_config` catalog.** Owns app-frame keys: camera auto-rotate,
   audio mode, CPU profile mode, variable-panel visibility, MSAA /
   line smooth / accum AA / point attenuation render flags, replay-UI
   chrome. Lives next to `glr_ctrl.c`. The pipeline does not include
   this catalog at all.

Save format compatibility: a single `@cfg <slug> = <int>` directive
already round-trips per slug, not per catalog. The importer can try
the REPL catalog first (which is all the pipeline knows about) and
ignore unknown slugs; the live sample's load path also asks the glr
catalog. So existing saved files keep working without a header bump.

After the split:

- `repl_export.c` and `repl_scenes.c` link only `repl_config.c`.
  Neither calls into `glr_config.c`.
- `glr_config.c` falls out of the demo link set entirely. With it goes
  every `audio_*` / `ui_state_profile_panel_mut` /
  `variable_panel_view_mut` / `ui_state_viewport` / `ui_state_code_panel`
  reference.

**Stubs cleared:** `g_cfg_items`, `CFG_ITEM_COUNT`,
`audio_get_cfg_mode`, `audio_set_cfg_mode`, `variable_panel_view_mut`,
`ui_state_profile_panel_mut`, `ui_state_viewport`, `ui_state_code_panel`.
(Eight.)

---

## Step 5 — Extract a non-editor source-load / commit API

**Why now.** Steps 1–4 land independently of this one, but step 5
*does* depend on step 3 (so the new commit entry returns diagnostics
through a sink rather than calling `set_status`). It also feeds step 6
by making the editor-free path the canonical loader for examples and
imports.

**Current state.** `feed_line` is the editor's programmatic commit
entry: it copies a line into the editor's input buffer and runs the
full `try_commit_*` chain (which mutates editor cursor / insert mode /
input buffer alongside the REPL state). The pipeline calls it from
four places:

- `repl_example_loader.c:247` — loading each line of a built-in example
- `repl_export.c:2606, 2611, 2614` — importer feeding parsed lines
- `repl_export.c:3051` — importing a function definition

The demo's `load_text_lines` proves the pure-pipeline path works for
flat commands (`repl_parser_parse_command_ctx` +
`repl_command_store_insert_one` + `editor_buffer_set_line`). But
`feed_line` also supports `for(...) { ... }`, `func0(...) { ... }`,
`if(...) { ... }`, `float x;` and `x = expr;` — multi-line block
structures and var-decl/var-assign flows that `try_commit_*` handles.
Restricting examples or saved files to flat-only would be a
regression.

**Fix — non-editor compile/apply/load API as the target, not a
fallback.**

Publish a leaner entry point — name TBD; sketch:

```c
ReplLoadResult repl_load_apply_line(const char *text,
                                    const ReplLoadContext *ctx,
                                    ReplDiagnostic *diag);
```

Semantics: same compile + apply + buffer-write transaction that
`try_commit_*` produces, but with editor-only side effects
(cursor target, insert-mode toggle, clear-input,
load-line-after-apply) suppressed. Internally this means:

- Reuse `repl_compile_*` (already pure) and `repl_apply_*` (already
  pure) to handle the source-array mutation.
- Reuse `editor_buffer_set_line` / `editor_buffer_apply_compiled_change`
  for text-buffer writes — the editor *buffer* is in the demo link
  set by design (it's where canonical line text lives); the editor
  *input dispatch* is what we're avoiding.
- For multi-line block structures, the underlying
  `repl_compile_close_brace` / `repl_compile_for_loop` /
  `repl_compile_func_def` / `repl_compile_if_block` validators
  already exist; the lean entry calls them directly without the
  `editor_compile_*` wrappers (which add `EditorCommitPlan`'s
  cursor/insert-mode side effects).
- For float-decl and var-assign, ditto — the
  `repl_compile_*` half is independent of the editor commit-plan
  half.
- Diagnostics flow through the new sink type (lines up with step 3).

The editor's `try_commit_*` chain becomes a thin wrapper: it builds
an `EditorCommitPlan`, calls `repl_load_apply_line` for the
compile+apply+buffer half, and applies the plan's editor side
effects.

Once `repl_load_apply_line` exists:

- Convert `repl_example_loader.c` to call it instead of `feed_line`.
  Examples no longer go through the editor input buffer.
- Convert `repl_export.c`'s importer call sites to call it. The C-to-REPL
  importer is a pipeline-level concern; it should not have been
  touching editor input dispatch.

**Stubs cleared:** `feed_line`.

(`load_line_to_input` is still pulled in by `repl_core.c::repl_reformat_commands`
and `repl_scenes.c::load_scene_from_slot`. Those are step 6.)

---

## Step 6 — Split `repl_core.c` / `repl_export.c` to remove the last editor-shaped helpers

**Why last.** Step 5 cleared the editor-driven portion of imports.
What remains in step 6 are two helpers whose editor-shape is *intrinsic*
(they want to set the editor cursor, not just commit a parsed line),
so the right answer is to move them out of `repl_core.c` /
`repl_scenes.c` rather than to give them a non-editor variant.

**Current state.**

- `repl_core.c:621` — `repl_reformat_commands` calls
  `load_line_to_input(repl_state_edit_line())` to restore the input
  buffer after reformatting all lines. The function is editor-shaped
  end-to-end: it touches `edit_line`, `insert_mode`,
  `editor_state_input_mut().input`, and the cursor position.
- `repl_scenes.c:232` — `load_scene_from_slot` calls
  `load_line_to_input(...)` to set the cursor on scene switch. Scene
  state legitimately includes an `edit_line`; restoring the input
  buffer to match it is editor business.

**Fix.**

- **Reformatter.** Move `repl_reformat_commands` (and its statics in
  `repl_core.c`) into `src/editor/`. The natural home is
  `src/editor/commit.c` or a new `src/editor/reformat.c`; either way
  it lives behind the editor boundary. This is on the open-edges list
  already (R10-phase2..5: "extract `repl_reformat.c`").
- **Scene loader cursor restore.** The mutation of `edit_line` / input
  buffer on slot load is editor business. Two viable shapes:
  - (a) `repl_scenes.c::load_scene_from_slot` becomes pure REPL
    (sets `edit_line` on `ReplState` and returns), and the
    controller / editor handles the `load_line_to_input` bridge after
    the slot load returns.
  - (b) The scene loader publishes a `ReplSceneLoadEffect` struct the
    way the codebase already publishes `EditorCommitPlan` / output
    structs; the controller actualizes the editor side.
  Either approach works; (a) is the smaller change.
- After both moves, `repl_core.c` and `repl_scenes.c` have no
  remaining `load_line_to_input` references. The stub (and the
  forward declaration in `repl_core_internal.h`) goes away.

**Stubs cleared:** `load_line_to_input`.

After step 6 the stub count is zero. The demo link set drops further
because `glr_config.c` is no longer pulled in (step 4) and the
reformatter / scene editor effects are no longer in the pipeline TUs
(step 6).

---

## Out of scope

Things the maintainer review correctly flagged as *not* the path
forward:

- **Restricting example/import format to flat lines** to avoid the
  block-structure problem. Step 5 covers it via the lean commit API
  instead — losing `for/if/func/float` support in saved files would
  be a regression.
- **Stopping at a `*_value_ptr` split for config.** Step 4 takes the
  table-by-owner approach as the endpoint; the value-ptr split is
  acceptable only as an intermediate step within step 4 if it shortens
  the diff.
- **Keeping the name `repl_state_reset_all` for the REPL-only reset.**
  Renamed to `repl_state_reset_program` in step 2a. The name change
  is the point.

## Notes on what *isn't* in this plan

The demo also pulls in `src/editor/state.c` (the editor *buffer*) by
design — canonical per-line text lives there per the `feature/editor-owns-text.md`
contract. Removing that is not a goal of this plan; it would mean
re-introducing `GLCmd.source[]` or otherwise duplicating the line text,
which the maintainers explicitly chose against. The "REPL pipeline
doesn't depend on the editor *input dispatch*" is the contract this
plan defends; "REPL pipeline doesn't depend on the editor *buffer*" is
not.

---

## Guard audit — does the existing Makefile catch coupling-displacement?

The greater goal is keeping REPL / editor / scene / UI as independent
modules. Each step in the plan moves a piece of code across a layer
boundary. The risk is that the *same* coupling re-emerges in the
destination module without anyone noticing. This section maps each
step to the existing guards that would catch displacement, and calls
out the gaps where new guards are needed.

### Existing guards relevant to the plan

`make check-state-ownership` runs 31 sub-targets. The ones that bear
on this plan:

| Guard | What it forbids |
|---|---|
| `check-controller-boundaries` | Only `glr_ctrl.c` (+ a small allowlist) may include `src/scene/` or `src/ui/` headers. Forbids new entries. |
| `check-pure-scene-no-repl-state` | `src/scene/*.c` may not reference any `repl_(state\|replay)_*` symbol. Hard. |
| `check-scene-no-repl-state-mut` | `src/scene/*.c` may not call `repl_state_*_mut*()`. Hard. |
| `check-state-boundaries` | Scene + state-neutral (cmd_format, prof) cannot include `repl_state.h`; UI can't mutate REPL state outside an allowlist. |
| `check-views-no-owners` | `src/scene/*.c` and `src/ui/*.c` cannot include `repl_state_owners.h`. Hard. |
| `check-ui-no-repl-state-mut` / `-read` | UI files cannot mutate or read live REPL state (snapshot only). Hard. |
| `check-no-set-status-in-compile-apply` | `repl_compile.c` and `repl_apply.c` cannot call `set_status`. Hard. |
| `check-no-set-status-in-repl-parser` | Ratchet (currently 1, target 0) on `repl_parser.c` set_status calls. |
| `check-state-c-shrinking` | `repl_state.c` line count ratchets down only. |
| `check-editor-ownership-budget` | Ratchet on `repl_state.c` calls into `ui_state_*` (currently 3, target 0) — every move-out in step 2 ticks this down. |
| `check-glr-ctrl-not-editor-mirror` | `glr_ctrl.c` cannot grow per-field editor wrappers. |
| `check-no-repl-editor-input-shim` | `src/editor/input.c` cannot delegate to legacy `repl_*_func` entry points. |
| `check-repl-no-direct-buffer-read` | `repl_*.c` readers must go through `EditorBufferView`, not `editor_buffer_line()`. |

### Per-step coverage

#### Step 1 — `repl_compile_dispatch` move

| Risk | Existing coverage | Gap |
|---|---|---|
| Moved code calls `set_status` | ✅ `check-no-set-status-in-compile-apply` (hard guard on `repl_compile.c`) | — |
| Moved code includes `editor_*` headers | ✅ `repl_compile.c` already takes `EditorBufferView` via `ReplCompileContext.text` — that's the sanctioned coupling | — |
| `editor_services.c` keeps a parallel dispatch helper | — | **GAP** — no guard prevents a stub from being left behind under a similar name |

Coverage: high. The compile-purity guard already forbids the worst regression. The risk of a leftover wrapper in `editor_services.c` is mitigated by the fact that the demo build itself fails if both definitions exist (multiple-definition link error).

#### Step 2 — Reset rename + `sync_ui_chrome` move

| Risk | Existing coverage | Gap |
|---|---|---|
| `repl_state.c` keeps calling peer/UI/editor reset functions | ✅ `check-editor-ownership-budget` (will go from 3 → 0 as the calls leave) and `check-state-c-shrinking` (line-count ratchet) | The budget guard's grep is narrow (`ui_state_*` only); it does not cover `variable_panel_state_*`, `editor_help_session_*`, `repl_editor_reset_transients` |
| `glr_app_reset_all` lives in the wrong file (e.g., grows back into `repl_state.c`) | ✅ `check-state-c-shrinking` would fire on growth | — |
| New `repl_state.c` helper writes UI state through a different forward decl | ✅ `check-editor-ownership-budget` for `ui_state_*` calls (ratchet) | **GAP** — no guard for non-`ui_state_*` peer mutators called from REPL TUs |
| Controller's new `repl_state_sync_ui_chrome` body re-introduces the wrong-direction read in `repl_state.c` | ✅ `check-state-c-shrinking` | — |

Coverage: medium. The line-count ratchet catches growth in `repl_state.c`; the ownership budget catches `ui_state_*` regression specifically. A symbol-level guard ("`repl_state.c` cannot reference `variable_panel_state_*`, `editor_help_session_*`, `replay_state_reset`, or `repl_editor_reset_transients`") would be the right complement and is already implied by the architecture's ownership rules.

#### Step 3 — Pipeline `set_status` → diagnostic sinks

| Risk | Existing coverage | Gap |
|---|---|---|
| `repl_executor.c` keeps calling `set_status` | — | **GAP** — extend `check-no-set-status-in-compile-apply` to executor |
| `repl_flatten.c` keeps calling `set_status` | — | **GAP** — same, extend to flatten |
| `repl_export.c` keeps calling `set_status` | — | **GAP** — same, extend to export |
| `repl_core.c` keeps the `set_status` thunk after callers leave | ✅ `check-state-c-shrinking` (line ratchet) | An explicit "`set_status` symbol gone from `repl_core.c`" check would be cleaner |

Coverage: low. The plan calls out the missing guard explicitly: extend the existing pattern (`check-no-set-status-in-compile-apply` is the template) to `repl_executor.c`, `repl_flatten.c`, `repl_export.c`, and eventually `repl_core.c`. Land the guard *with* step 3 — the script is a 5-line edit of the existing one.

#### Step 4 — Config split by owner

| Risk | Existing coverage | Gap |
|---|---|---|
| New `repl_config.c` includes `glr_config.h` | — | **GAP** — no guard yet (module doesn't exist) |
| New `repl_config.c` references `audio_*`, `ui_state_*`, peer state | ✅ `check-state-boundaries` partially covers (no `repl_state.h` from neutral files) | **GAP** — needs a positive include allowlist for `repl_config.c` |
| `glr_config.c` regrows REPL-only keys | — | **GAP** — needs a guard or convention |
| `repl_export.c` / `repl_scenes.c` silently start calling `glr_config_*` again | — | **GAP** — currently allowed; would defeat the split |

Coverage: low. Step 4 is the largest stub-count clear (8 stubs) and has the least existing protection — none of these modules exist yet. The right time to land guards is *as part of step 4*: add `check-repl-config-no-glr-config` (the new TU's include list is restricted) and `check-repl-export-no-glr-config` (the pipeline TUs cannot call `glr_config_*`).

#### Step 5 — Non-editor commit API

| Risk | Existing coverage | Gap |
|---|---|---|
| New entry point lands in `repl_core.c` and bloats it | ✅ `check-state-c-shrinking` (analog) — there's no current `repl_core.c` line ratchet, but R10-phase2..5 plans dissolution | **GAP** — `repl_core.c` has no shrinking ratchet today |
| New entry point includes `src/editor/input.h` (the dispatch boundary) | — | **GAP** — no guard against `repl_*.c` including `src/editor/input.h` |
| `repl_example_loader.c` and `repl_export.c` keep calling `feed_line` | ✅ `check-no-repl-editor-input-shim` covers `src/editor/input.c` only, not the call sites | **GAP** — symbol-level guard needed |

Coverage: low. Land two guards with step 5: (a) `check-repl-no-editor-input-include` — REPL TUs cannot `#include "src/editor/input.h"`; and (b) `check-no-feed-line-in-pipeline` — symbol-level guard against `feed_line(` calls in `repl_*.c` after the migration completes.

#### Step 6 — Reformatter + scene cursor restore extraction

| Risk | Existing coverage | Gap |
|---|---|---|
| `repl_core.c::repl_reformat_commands` body stays put | ✅ `check-state-c-shrinking` (analog needed for `repl_core.c`) | **GAP** — `repl_core.c` has no shrinking ratchet today |
| `repl_scenes.c` keeps `load_line_to_input` | — | **GAP** — symbol-level guard against `load_line_to_input(` calls in `repl_*.c` |
| Reformatter moved to `src/editor/` but pulls a REPL header it shouldn't | ✅ `check-state-boundaries` covers some of this | — |

Coverage: low. Same shape as step 5 — needs two new guards.

### The umbrella guard: stub-count ratchet

The cleanest meta-guard is a ratchet on `tools/repl_demo/stubs.c`
itself. **If any plan step regresses, the linker compensates by
needing more stubs.** A `check-repl-demo-stubs-shrinking` script
counts the externally-visible symbols in
`build/*-gl-stubs/tools/repl_demo/stubs.o` (or a textual count of
`^[A-Za-z_][A-Za-z0-9_]* [a-zA-Z_]+` definitions in `stubs.c`) and
fails if the count exceeds a baseline that ratchets downward only.

Today's baseline: **17**. After step 1 (target): 16. After step 2: 11.
After step 3: 10. After step 4: 2. After step 5: 1. After step 6: 0.

This guard catches at the symbol layer what include-based guards miss
at the header layer. It also matches the architectural commitment
already in `MODULES.md`:

> The `tools/repl_demo/stubs.c` file is the visible record of what
> the REPL pipeline pulls in beyond pure pipeline code; if it grows
> past ~15 entries the chokepoint deserves a structural split rather
> than more stubs.

Operationalize "visible record" as a ratchet. Add it to
`check-state-ownership` so it runs in the standard `make check`
batch.

### Summary of guard work to land alongside the plan

| New / extended guard | Step that needs it | Effort |
|---|---|---|
| `check-no-set-status-in-pipeline` (extend the compile-apply guard to executor/flatten/export/core) | 3 | 30 min — clone the existing script |
| `check-repl-no-editor-input-symbols` (`feed_line`, `load_line_to_input`, `repl_editor_reset_transients` not callable from `repl_*.c`) | 2, 5, 6 | 1 hr — symbol-level grep over REPL_SRCS |
| `check-repl-no-glr-config` (`repl_*.c` cannot reference `glr_config_*` after split) and reciprocal | 4 | 30 min |
| `check-repl-state-c-no-peer-resets` (extend `check-editor-ownership-budget` to peer/editor reset symbols) | 2 | 30 min — add new ratchet keys |
| `check-repl-core-c-shrinking` (clone of the `repl_state.c` ratchet for `repl_core.c`) | 3, 5, 6 | 15 min |
| `check-repl-demo-stubs-shrinking` (the umbrella ratchet) | all | 1 hr — count externally-visible symbols, ratchet down only |

None of these are large; each is a clone/extension of an existing
script. The most leveraged ones are the two ratchets
(`check-repl-core-c-shrinking` and the stubs counter) — they don't
require enumerating every forbidden symbol, they just refuse to let
the count regress.

### What the existing guards *do* protect against

The plan does not have to invent a fence around scene/UI re-coupling
into REPL — those edges are already locked down end-to-end:

- Scene cannot read `repl_state_*` (hard).
- Scene cannot mutate `repl_state_*` (hard).
- UI cannot mutate `repl_state_*` outside the allowlist
  (`check-ui-returns-hits-only` is at 0/0, hard from there).
- UI renderers must consume `UiRenderSnapshot`, not live state.
- `src/scene/*.c` and `src/ui/*.c` cannot include
  `repl_state_owners.h` (the mutator header).
- `glr_ctrl.c` cannot grow into a per-field editor mirror
  (`check-glr-ctrl-not-editor-mirror`).

So the plan's risk surface is asymmetric: the *destination* layers
(scene, UI) are well-fenced. The risk is that REPL pipeline TUs
quietly keep their backward edges to editor / UI / app-frame. The new
guards above close that asymmetry.
