# Decoupling repl_demo from gl-repl — execution plan

> Supersedes the chokepoint-numbered draft. Reordered as an execution
> sequence with maintainer corrections applied across two review
> rounds: stub count (17 not 12); reset rename
> (`repl_state_reset_program` + `glr_app_reset_all`); non-editor
> commit API as the target; config split by owner *as a temporary
> intermediate* — presentation/render slugs land in `repl_config`
> initially and migrate to `glr_config` in step 7 once `glr_state.c`
> exists; `repl_export.c` stays whole and opaque to header content
> (`ReplExportProperties` + `ReplExportCameraBlock`). Round-2 review
> caught: step 4 was overcounting (`ui_state_viewport` /
> `ui_state_code_panel` survive past step 4 — they're pulled by
> `repl_export.c`'s layout calls, not by config); step 3 was missing
> the `set_status` sites in `repl_scenes.c` (8) and
> `repl_example_loader.c` (1); step 5 assumed pure structured-block
> validators that don't yet exist (extraction is a prerequisite,
> now step 5a); autocomplete provider registration was leaking into
> the program-only reset; step 7's summary row still said "split
> `repl_export.c`" while its body had switched to "keep whole."
> All addressed below.

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
concrete misplacement or unfinished refactor in the existing code,
not a structural problem with the pipeline itself. The ordering
below is chosen so each step lands without depending on the next.
Step 7 is a follow-on that does not clear stubs — by step 6 the
count is already zero — but it finishes the ownership split that
step 4 only takes to the catalog layer.

| Step | Chokepoint | Stubs cleared | Effort | Feasibility |
|---|---|---|---|---|
| 1 | `repl_compile_dispatch` location | 1 | Low | High — known TODO in source comment |
| 2 | UI chrome sync + full-world reset out of `repl_state.c` (also evicts autocomplete-provider registration from the program-only reset) | 5 | Low | High — pure layering inversion |
| 3 | Pipeline `set_status()` → diagnostic sinks across **6 TUs** (~16 sites) | 1 | Medium | High — pattern proven in `repl_parser.c`; site list is broader than the round-1 draft claimed |
| 4 | **Temporary** catalog split (`repl_config` vs `glr_config`); presentation/render slugs land in `repl_config` for now and migrate to `glr_config` in step 7 | 6 | Medium | Medium — intermediate ownership is acknowledged, not the endpoint |
| 5a | Extract pure structured-block validators (`repl_compile_close_brace` / `_if_block` / `_func_def` / `_for_loop`) from the editor-side `editor_compile_*` wrappers | 0 | Medium | Medium — separates pure validation from cursor/insert-mode side effects |
| 5b | Build the non-editor source-load / commit API on top of the extracted validators; convert example loader + importer to use it | 1 (`feed_line`) | Medium | Medium — supports multi-line block structures because 5a already extracted them |
| 6 | Move reformatter + scene cursor restore out of pipeline TUs into the editor / controller | 1 (`load_line_to_input`) | Medium | Medium — overlaps with R10-phase2..5 in ARCHITECTURE.md |
| 7 | Move `presentation` + `render` slices to a new `glr_state.c`; migrate slugs from `repl_config` to `glr_config`; make `repl_export.c` fully opaque to its environment via `ReplExportProperties` + `ReplExportCameraBlock` + caller-passed layout hints; split `repl_scenes.c` slot snapshots into REPL programs + glr cfg-property bags | 2 (`ui_state_viewport`, `ui_state_code_panel`) | High | Medium — large, but matches the existing `glr_*` ownership convention |

Totals reconcile: 1 + 5 + 1 + 6 + 0 + 1 + 1 + 2 = **17 stubs**.

Trajectory: 17 → 16 (step 1) → 11 (step 2) → 10 (step 3) → 4 (step 4) → 4 (step 5a, no stub change) → 3 (step 5b) → 2 (step 6) → **0** (step 7).

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

**Fix — rename, do not preserve the misleading name; evict the
autocomplete-registration leak at the same time.**

- Rename `repl_state.c::repl_state_reset_all` →
  `repl_state_reset_program`. New scope: REPL slices only (program,
  predef vars, scenes, autonormal, source scope, eval predef storage,
  flat dirty flag). No peer / UI / editor calls remain.
- Introduce `glr_app_reset_all` (in `glr_ctrl.c` or a new
  `glr_app.c` if a top-level shell module is wanted) that calls
  `repl_state_reset_program()` *and* the four peer/UI/editor resets.
  This is the function the live sample calls; the demo never calls it.
- **Move `repl_autocomplete_register_provider()` out of the reset.**
  It currently runs inside `repl_state_init_defaults` /
  `repl_state_reset_all` (`repl_state.c:569` and `:593`), which means
  every program reset re-registers a provider that calls
  `editor_completion_register` (`src/editor/completion.c:11`). That's
  editor-side coupling masquerading as REPL initialization. Today it
  doesn't show up in `stubs.c` because `src/editor/completion.c` is
  in the demo link set, but it violates the program-only-reset
  boundary. Move the registration to one-shot app/editor startup
  (called from `glr_app_reset_all` or once during `glr_ctrl_init`).
  Tests that exercise REPL state in isolation should not need a
  completion provider; tests that exercise autocomplete should
  install one explicitly.
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
forwarder to `ui_state_status_set`. Pipeline modules call it from
**~16 sites across 6 TUs**:

- `repl_core.c` (2): normalize parse error (`:364`), startup banner (`:756`)
- `repl_executor.c` (1): goto loop limit reached (`:559`)
- `repl_flatten.c` (1): flatten error (`:639`)
- `repl_export.c` (3): export I/O errors (`:2937`, `:2957`, `:3188`)
- `repl_scenes.c` (8): rename / save-default / save-workspace /
  load-default / load-workspace / promote-rejected / restore-rejected
  status messages (`:239`, `:335`, `:342`, `:378`, `:425`, `:457`,
  `:519`, `:534`)
- `repl_example_loader.c` (1): example-load status (`:285`)

The round-1 plan listed 9 sites and missed `repl_scenes.c` and
`repl_example_loader.c` entirely. Both are in the demo link set and
both keep the `set_status` thunk reachable; if step 3 only patches
core/executor/flatten/export it doesn't actually clear the
`ui_state_status_set` stub.

The architecture already documents the cleaner pattern
(`ARCHITECTURE.md` lines 866-868): `repl_parser.c` writes diagnostics
into `ReplParseContext.err_buf`, the parser core has zero `set_status`
calls, and `check-no-set-status-in-repl-parser` enforces that at
**0/0**.

**Fix.** Apply the same pattern to all six TUs:

- `repl_execute_program`: extend `ReplExecutionOptions` with an
  optional `char *err_buf` / `int err_sz` (or a callback). The runtime
  goto-limit error writes there instead of calling `set_status`.
- `repl_flatten_commands`: return a status string in the existing
  `ReplFlattenResult` (already returned by `repl_flatten_commands`);
  delete the inline `set_status(result.status)`.
- `repl_export_save_output` / `repl_export_load_from_file`: return
  / out-param the diagnostic message instead of writing it directly.
- `repl_scenes.c`: the scene-management entry points
  (`repl_user_scene_rename`, `repl_save_default_output`,
  `repl_save_workspace`, `repl_load_default_output`,
  `repl_load_workspace`, etc.) already return status; thread an
  out-param diagnostic for the cases that currently call `set_status`
  inline. The controller writes the status text after the call
  returns.
- `repl_example_loader.c`: the example-load path returns to the
  controller anyway; pass the status message back as an out-param
  instead of writing it inline.
- `repl_core.c::set_status` startup banner moves to whatever calls
  `repl_state_init_defaults` (likely the controller); remove it from
  the pipeline TU.
- After all 9 sites are converted, delete `set_status()` and its
  thunk from `repl_core.c`. Add `check-no-set-status-in-pipeline`
  guarding the executor / flatten / export TUs (analog of the existing
  parser guard).

**Stubs cleared:** `ui_state_status_set`.

---

## Step 4 — Temporary catalog split to unblock demo

**Why "temporary."** The honest endpoint for app/REPL ownership is
in step 7 (presentation/render slices move to `glr_state.c`).
Step 4's job is narrower: clear *config-owned* stubs from the demo
link set without waiting on the storage relocation. Presentation/
render slugs land in the **`repl_config`** catalog initially because
their backing storage is still in `g_repl_state`; in step 7 those
slugs migrate to `glr_config` once the storage moves and the neutral
export-property bag carries them across the file format. Calling
step 4 "split by owner" would mis-frame the temporary placement —
the slug ownership in step 4 reflects current storage, not target
ownership.

**Why now.** Steps 1–3 don't touch `glr_config.c` or its callers, so
this can land independently. It is also the largest single
stub-count payoff that's available without the step 7 storage work.

**Current state.** `glr_config.c` is in the demo link set because
`repl_export.c` (`@cfg` save/load) and `repl_scenes.c` (per-scene cfg
snapshots) call `glr_config_get/set/items`. `glr_config.c::config_value_ptr`
is one big switch over every cfg key, so it transitively forces every
config-target module into the link set:

| Stub | Pulled in by | Cleared in step? |
|---|---|---|
| `g_cfg_items[]` / `CFG_ITEM_COUNT` | Defined in `glr_actions.c` (not linked) | **4** |
| `audio_get_cfg_mode` / `audio_set_cfg_mode` | `GLR_CONFIG_AUDIO_MODE` arm calls `audio.c` | **4** |
| `variable_panel_view_mut` | `GLR_CONFIG_VARIABLE_PANEL` arm reaches into peer state | **4** |
| `ui_state_profile_panel_mut` | `GLR_CONFIG_CPU_PROFILE` arm reaches into UiState | **4** |
| `ui_state_viewport`, `ui_state_code_panel` | **NOT pulled by config — pulled by `repl_export.c` calling `ui_layout_scene_rect` (`:2786`) and `ui_layout_code_panel_rect` (`:3257`) for the code-panel dump path** | **7** (layout decoupling) |

The round-1 plan attributed all six surviving stubs to config and
claimed step 4 cleared 8. It actually clears 6: the two
`ui_state_*` viewport/panel stubs survive past step 4 because
`src/ui/layout.c` is in the demo link set transitively from
`repl_export.c`'s layout calls, not from `glr_config.c`. They get
cleared in step 7 alongside the cfg-opacity rework (see step 7
on `repl_export.c` opacity to its environment).

**Fix — temporary catalog split.**

Two descriptor catalogs:

1. **`repl_config` catalog.** Owns every cfg key whose backing
   storage is currently in `g_repl_state` (presentation flags,
   render flags, scene/replay toggles, code-panel chrome). Lives
   next to `repl_state.c`. The pipeline (`repl_export.c`,
   `repl_scenes.c`) iterates *only* this catalog when reading/writing
   `@cfg` directives. **The slug list in this catalog will shrink in
   step 7** as presentation/render slugs migrate to `glr_config`.
2. **`glr_config` catalog.** Owns app-frame keys whose backing
   storage is *not* in `g_repl_state`: camera auto-rotate, audio
   mode, CPU profile mode, variable-panel visibility. Lives next to
   `glr_ctrl.c`. The pipeline does not include this catalog at all.

The cheap intermediate (split `config_value_ptr` into a REPL-only
and glr-only variant) is acceptable as a stepping stone within
step 4 if it shortens the diff, but the endpoint here is two
descriptor tables.

Save format compatibility: a single `@cfg <slug> = <int>` directive
already round-trips per slug, not per catalog. The importer can try
the REPL catalog first (which is all the pipeline knows about) and
ignore unknown slugs; the live sample's load path also asks the glr
catalog. So existing saved files keep working without a header bump.
Step 7 preserves the same compatibility while migrating slugs across
catalogs.

After the split:

- `repl_export.c` and `repl_scenes.c` link only `repl_config.c`.
  Neither calls into `glr_config.c`.
- `glr_config.c` falls out of the demo link set. With it goes
  `audio_*`, `ui_state_profile_panel_mut`, `variable_panel_view_mut`.
- `ui_state_viewport` and `ui_state_code_panel` **remain** in
  the link set — they'll clear in step 7.

**Stubs cleared:** `g_cfg_items`, `CFG_ITEM_COUNT`,
`audio_get_cfg_mode`, `audio_set_cfg_mode`, `variable_panel_view_mut`,
`ui_state_profile_panel_mut`. (Six.)

---

## Step 5 — Extract a non-editor source-load / commit API

Step 5 splits into a prerequisite extraction phase (5a) and the
loader build (5b). The round-1 plan assumed the structured-block
validators were already pure and lived in `repl_compile.c`; they
are not — the only existing entry points for `close_brace`,
`if_block`, `func_def`, and `for_loop` are
`editor_compile_close_brace` / `_if_block` / `_func_def` /
`_for_loop` in `src/editor/commit.c` (declared in
`src/editor/commit.h:219..266`). Those mix pure validation with
`EditorCommitPlan` side effects (cursor target, insert-mode toggle,
clear-input, load-line-after-apply). The lean loader cannot call
them as-is without re-introducing the editor coupling we're trying
to remove.

### 5a. Extract pure structured-block validators

Land four new functions in `repl_compile.c`:

```c
ReplCompileResult repl_compile_close_brace(const char *input,
                                           const ReplCompileContext *ctx,
                                           ReplCompiledChange *out,
                                           char *err, int err_size);

ReplCompileResult repl_compile_if_block   (const char *input, ...);
ReplCompileResult repl_compile_func_def   (const char *input, ...);
ReplCompileResult repl_compile_for_loop   (const char *input, ...);
```

Each returns a `ReplCompiledChange` describing the source-array
mutation. No cursor, no insert mode, no input buffer, no
`set_status` (forbidden by the existing
`check-no-set-status-in-compile-apply` guard).

`editor_compile_*` becomes a thin wrapper that calls the
corresponding `repl_compile_*` and then attaches the editor side
effects (cursor target, insert-mode toggle, etc.) into an
`EditorCommitPlan`. The `try_commit_*` chain in `editor_commit.c`
already drives the plan — it doesn't change.

The extraction is mechanical but real: each `editor_compile_*` body
is a few hundred lines mixing parsing, source-scope queries, and
editor mechanics. The split is clean — parsing and source-scope
queries stay (they're the validation half); cursor/insert-mode
fields move to the wrapper.

**Why this is its own phase.** The extraction can land independently
of 5b — it has no observable behavior change, every existing caller
keeps working through the wrappers, and it adds the four pure
entry points the lean loader will need. Doing 5a as a separate PR
means 5b's review can focus on the loader semantics rather than
the extraction mechanics.

**5a stub change:** none directly. The pure validators are not yet
called from the demo, so no `feed_line` reference is removed.
`feed_line` clears in 5b.

### 5b. Build the lean loader on top

**Why 5b depends on 5a and 3.** Step 3 makes the new commit entry
return diagnostics through a sink rather than calling `set_status`.
Step 5a provides the pure validators that the lean loader calls
without touching editor state. With both in place, 5b is the small
piece that wires it together.

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

- Reuse `repl_compile_float_decl` / `repl_compile_var_assign`
  (already pure) and `repl_apply_*` (already pure) to handle the
  source-array mutation.
- Reuse the validators extracted in 5a
  (`repl_compile_close_brace` / `_if_block` / `_func_def` /
  `_for_loop`) for multi-line block structures. The lean entry
  calls them directly — no `editor_compile_*` wrapper, no
  `EditorCommitPlan` cursor/insert-mode side effects.
- Reuse `editor_buffer_set_line` / `editor_buffer_apply_compiled_change`
  for text-buffer writes — the editor *buffer* is in the demo link
  set by design (it's where canonical line text lives); the editor
  *input dispatch* is what we're avoiding.
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

## Step 7 — Move misnamed app-state slices to `glr_state.c`

**Why a separate step.** Steps 1–6 reduce the stub count to zero
against the *current* file layout. Step 7 fixes a deeper naming
problem that the catalog split in step 4 only patches at the
descriptor layer: `repl_state.c` owns `g_repl_state`, but four of its
seven slices have nothing to do with the REPL language. They're
app-frame chrome that ended up there because `repl_state.c` was the
original (and for a while only) state owner; everything else
(`EditorState`, `UiState`, peer subsystems) was carved out around
them and the original misnomer was never undone. Step 4 splits the
*catalog* (`repl_config_set(WIREFRAME, …)` vs. `glr_config_set(…)`);
it does *not* split the *storage* — both still write into
`g_repl_state.presentation`. Step 7 finishes that split.

**Why last.** Step 7 propagates into `repl_export.c` and `repl_scenes.c`,
both of which steps 4 and 6 have already touched. Doing step 7 before
those is rework. Doing step 7 *after* them is mostly mechanical
because the catalog-side fence is already in place.

### Current state — slice ownership audit

`ReplRuntimeState` in `repl_state.h` has seven slices:

| Slice | Genuinely REPL? | What's in it |
|---|---|---|
| `document` | ✅ REPL | parsed source command array |
| `flat_program` | ✅ REPL | flattened executable command stream |
| `variables` | ✅ REPL | predef vars, scratch arrays A/B/C, func aliases, `time_playing` |
| `scenes` | ✅ REPL (mostly) | user-scene slots — but each slot bundles a per-scene cfg snapshot that is app-state |
| `import_export` | mixed | scene-name hint (REPL), workspace_dir (app), pending workspace-header state (mixed) |
| `presentation` | ❌ **APP** | wireframe, grid_theme, grid_major_idx, grid_extent_idx, axes_theme, backdrop_mode, show_vertex_{labels,normal_vectors,indices,outlines,points,guides}, show_light_indicators, highlight_current_poly, autonormal, code_panel_layout, wrap_at_comma |
| `render` | ❌ **APP** | multisample_enabled, line_smooth_enabled, accum_aa_enabled, point_attenuation_enabled |

Plus the controller-pushed editor-overlay lists (`editor_transformers`,
`editor_highlights`, `editor_virtual_lines`) that landed on
`ReplRuntimeState` because that was the only owner big enough to hold
them. They are editor concerns and should move to `EditorState` along
the way.

### Fix — new owner, neutral export bag, owner-fills/applies

**1. New `glr_state.c` / `glr_state.h`.** Owner sibling to
`repl_state.c`, `src/editor/state.c`, `src/ui/state.c`. Holds:

- `presentation` slice (verbatim relocation, no field changes)
- `render` slice (verbatim relocation)

`code_panel_layout` and `wrap_at_comma` are editor *chrome*, not
render config — those two move to `EditorState` (or `UiState`, if
that's where the existing chrome already lives). The rest of
`presentation` is genuinely scene-presentation cfg and belongs on
`glr_state`.

**2. `repl_state.c` shrinks.** What remains: `document`,
`flat_program`, `variables`, `scenes` (programs only), and the REPL
half of `import_export`. The file genuinely is REPL-language state
after step 7 — the name becomes correct without renaming. Don't
rename the file: it is two breaking changes for one cleanup, and
existing call sites churn for no semantic reason.

**3. `scenes` split into REPL programs + app cfg snapshots.** Two
parallel slot arrays:

- `repl_scenes.c` — owns the program half: per-scene
  `document_cmds[]`, `predef_vars[]`, `name`, `last_touch`.
- `glr_scenes.c` (new) — owns the app-cfg half: per-scene snapshot
  of the presentation/render values, stored as a
  `ReplExportProperties` (see #4) per slot.

The controller bundles the two halves at save/load time. F12 cycle
and the workspace iterator each call both halves under one
transaction. This split mirrors the catalog split from step 4: the
REPL pipeline only sees the program half.

**4. `repl_export.c` stays whole; gains a neutral header bag.** The
exported file is one cohesive document with a single line-tagged
grammar — splitting the writer/reader along app/REPL lines means
coordinating two file streams, which is awkward. The right inversion
is to keep `repl_export.c` whole and make it opaque to *what* the
headers carry.

`repl_export.c` keeps:
- `display()` body, REPL functions, predef-var globals, the per-line
  REPL → C translation (already file-format ownership)
- `@var <name> = <float>`, `@func N = name`, `@declare <name>`,
  `@scene-name <name>` directives — these are REPL language
  concepts, legitimately the file-format owner's business
- File-grammar parsing, line dispatch, format compatibility

`repl_export.c` *no longer* knows about:
- What `wireframe` / `grid_theme` / `auto_rotate` / etc. mean
- `glr_config_*` (forbidden by symbol guard, see guard audit)
- Any cfg slug semantics — only the `// @cfg <key> = <value>` line shape
- **Live viewport / panel geometry** — currently `repl_export.c:2786`
  calls `ui_layout_scene_rect` and `:3257` calls
  `ui_layout_code_panel_rect` from the code-panel dump path. Those
  pull `ui_state_viewport` and `ui_state_code_panel` into the demo
  link set transitively through `src/ui/layout.c`. Step 7 makes
  layout-as-environment opaque to `repl_export.c` the same way cfg
  is: the caller computes the rects (or the `int` values it cares
  about — typically just panel width) and passes them in as fields
  on the export-options struct. `repl_export.c` does not call
  `ui_layout_*` after step 7. This clears `ui_state_viewport` and
  `ui_state_code_panel` from the demo stubs.

New neutral abstraction in `repl_export.h`:

```c
typedef struct {
    char key  [REPL_EXPORT_KEY_MAX];      /* opaque slug */
    char value[REPL_EXPORT_VALUE_MAX];    /* decimal-encoded; opaque to repl_export */
} ReplExportProperty;

typedef struct {
    ReplExportProperty items[REPL_EXPORT_MAX_PROPS];
    int count;
} ReplExportProperties;

void        repl_export_props_clear(ReplExportProperties *p);
int         repl_export_props_set  (ReplExportProperties *p,
                                    const char *key, const char *value);
const char *repl_export_props_get  (const ReplExportProperties *p,
                                    const char *key);
int         repl_export_props_count(const ReplExportProperties *p);
int         repl_export_props_at   (const ReplExportProperties *p, int idx,
                                    const char **key_out,
                                    const char **value_out);
```

**Why a flat key/value bag, not a callback registry.** The cfg payload
is genuinely simple: ~25 slugs, all `int` 0..N, encoded as decimal
strings. A registered-callback dispatch through `repl_export` adds
init-order coupling (registry must be populated before any export),
indirection on a 5-line operation (callback ends up calling
`glr_state_apply(...)` anyway), and loss of inspectability (a flat
collection is trivially round-trippable in tests). The callback
approach pays off for non-trivial per-slug logic — nested structures,
versioned encoding, computed values. None of those apply here.

Export and import flow:

```c
/* Export — controller (or a small glr_export.c wrapper): */
ReplExportProperties props;
repl_export_props_clear(&props);
glr_state_fill_export_props(&props);   /* writes wireframe, grid_theme, … */
/* … each owner that wants header presence fills its own slugs … */
repl_export_save_output(path, &props, &camera_block, /* + REPL state */);

/* Inside repl_export.c — pure iteration, no slug knowledge: */
for (int i = 0; i < repl_export_props_count(&props); i++) {
    const char *k, *v;
    repl_export_props_at(&props, i, &k, &v);
    fprintf(f, "// @cfg %s = %s\n", k, v);
}

/* Import — controller: */
ReplExportProperties props_out;
ReplExportCameraBlock cam_out;
repl_export_load_from_file(path, &props_out, &cam_out, /* + REPL state out */);
glr_state_apply_export_props(&props_out);   /* reads its own slugs, ignores rest */
camera_apply_export_block(&cam_out);
```

Each owner only knows its own slugs. `repl_export.c` only knows the
line format.

**4a. Camera block — its own neutral struct.** The `// camera` block
is multi-line (5 raw `glTranslatef`/`glRotatef` strings) and doesn't
fit the flat key/value model cleanly. Keep it as a separate directive
type alongside the property bag, but apply the same opacity rule:
`repl_export.h` defines a neutral `ReplExportCameraBlock` (5 string
slots), the camera owner fills/reads it, `repl_export.c` writes/parses
the lines without interpreting them. Two abstractions instead of one,
but each stays clean.

```c
#define REPL_EXPORT_CAMERA_LINE_MAX 96
#define REPL_EXPORT_CAMERA_LINES    5

typedef struct {
    char lines[REPL_EXPORT_CAMERA_LINES][REPL_EXPORT_CAMERA_LINE_MAX];
    int  present;   /* 0 = no camera block in this file */
} ReplExportCameraBlock;
```

The camera owner provides:

```c
void camera_fill_export_block (ReplExportCameraBlock *out);
void camera_apply_export_block(const ReplExportCameraBlock *in);
```

`repl_export.c`'s writer emits the block verbatim if `present`;
the reader populates `lines[]` and sets `present` if a `// camera`
header is encountered. It does not parse the GL syntax inside the
lines — that is the camera owner's business.

**5. `import_export` slice split.** The mixed slice splits the same
way:

- `repl_state.c` keeps scene-name hint and any pending REPL-side
  parser state.
- `glr_state.c` keeps `workspace_dir` and pending app-cfg directives.

The save-file format is unchanged through all of step 7. Existing
`output.c` files round-trip without a header bump.

### Demo link-set impact

After step 7 the demo links `repl_state.c` and skips `glr_state.c`.
That's one fewer TU than after step 4 alone, *and* it eliminates a
class of future stub regressions: any cfg-key addition that targets
an app slice can no longer accidentally surface in the pipeline TUs.

The naming is also informative for new contributors: a file named
`repl_state.c` now actually contains REPL state, and a file named
`glr_state.c` actually contains app-frame state. The previous layout
required reading the slice names to know which was which.

### What does *not* move

- `EditorState` (cursor, selection, search, autocomplete, undo,
  editor buffer, transformers/highlights/virtual lines after
  step 7) — already in `src/editor/state.c`.
- `UiState` (viewport, pointer, status TTL, panel divider) — already
  in `src/ui/state.c`.
- Peer subsystems (`replay_state`, `variable_panel_state`,
  `color_picker_state`, `editor_help_session`) — already in their
  own files.
- `glr_camera` — already in `glr_camera.c`.
- `glr_config` (the catalog from step 4) — already in `glr_config.c`.

After step 7, the four-owner contract from `MODULES.md` actually
holds at the file layout, not just the description:

```
ReplState     → repl_state.c       (REPL language state)
EditorState   → src/editor/state.c (text-document model)
UiState       → src/ui/state.c     (UI chrome)
GlrState      → glr_state.c        (app-frame presentation/render)  ← NEW
+ peers       → their own files
```

### Effort and risk

- **Effort:** ~2–3 weeks. The file moves are mechanical; the
  `repl_export.c` opacity rework (cfg + camera + layout) and the
  `repl_scenes.c` per-slot snapshot rework are the work.
- **Risk:** medium. `@cfg` save-file compatibility is a footgun —
  any importer that loses an `@cfg` line silently drops a setting.
  Round-trip the entire `tests/test_repl_export_all_commands.c`
  corpus + every workspace in `examples/` before merging. Because
  the format is line-tagged, a missed slug surfaces as a missing
  state mutation, not corruption. The neutral property bag makes
  this test trivial: feed the bag in, read it out, compare.
- **Net stub change: 2.** `ui_state_viewport` and
  `ui_state_code_panel` clear because `repl_export.c` no longer
  calls `ui_layout_*`. (Round-1 plan claimed step 7 was structural-
  only with no stub change; this was wrong because it overlooked
  the layout pull-in that step 4 doesn't address.) After step 7
  the demo stub count is **0**.

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

#### Step 2 — Reset rename + `sync_ui_chrome` move + autocomplete-registration eviction

| Risk | Existing coverage | Gap |
|---|---|---|
| `repl_state.c` keeps calling peer/UI/editor reset functions | ✅ `check-editor-ownership-budget` (will go from 3 → 0 as the calls leave) and `check-state-c-shrinking` (line-count ratchet) | The budget guard's grep is narrow (`ui_state_*` only); it does not cover `variable_panel_state_*`, `editor_help_session_*`, `repl_editor_reset_transients` |
| `glr_app_reset_all` lives in the wrong file (e.g., grows back into `repl_state.c`) | ✅ `check-state-c-shrinking` would fire on growth | — |
| New `repl_state.c` helper writes UI state through a different forward decl | ✅ `check-editor-ownership-budget` for `ui_state_*` calls (ratchet) | **GAP** — no guard for non-`ui_state_*` peer mutators called from REPL TUs |
| Controller's new `repl_state_sync_ui_chrome` body re-introduces the wrong-direction read in `repl_state.c` | ✅ `check-state-c-shrinking` | — |
| `repl_state_reset_program` keeps calling `repl_autocomplete_register_provider` (or any other editor-side registration) | — | **GAP** — symbol guard "`repl_state_reset_program` body cannot call `*_register_provider*` / `editor_completion_*`" — the program-only reset must touch only REPL slices |

Coverage: medium. The line-count ratchet catches growth in `repl_state.c`; the ownership budget catches `ui_state_*` regression specifically. A symbol-level guard ("`repl_state.c` cannot reference `variable_panel_state_*`, `editor_help_session_*`, `replay_state_reset`, `repl_editor_reset_transients`, or `editor_completion_*`") would be the right complement and is already implied by the architecture's ownership rules.

#### Step 3 — Pipeline `set_status` → diagnostic sinks

| Risk | Existing coverage | Gap |
|---|---|---|
| `repl_executor.c` keeps calling `set_status` | — | **GAP** — extend `check-no-set-status-in-compile-apply` to executor |
| `repl_flatten.c` keeps calling `set_status` | — | **GAP** — same, extend to flatten |
| `repl_export.c` keeps calling `set_status` | — | **GAP** — same, extend to export |
| `repl_scenes.c` keeps calling `set_status` (8 sites) | — | **GAP** — same, extend to scenes |
| `repl_example_loader.c` keeps calling `set_status` | — | **GAP** — same, extend to example loader |
| `repl_core.c` keeps the `set_status` thunk after callers leave | ✅ `check-state-c-shrinking` (line ratchet) | An explicit "`set_status` symbol gone from `repl_core.c`" check would be cleaner |

Coverage: low. The guard `check-no-set-status-in-pipeline` must
cover **all six TUs** (`repl_executor.c`, `repl_flatten.c`,
`repl_export.c`, `repl_scenes.c`, `repl_example_loader.c`, and
eventually `repl_core.c` itself). The round-1 plan listed only the
first three; the omission was caught in round-2 review by checking
the live `set_status` call sites and finding 7 additional ones in
scenes + example loader. Land the guard *with* step 3 — the script
is a small extension of `check-no-set-status-in-compile-apply`.

#### Step 4 — Temporary catalog split

| Risk | Existing coverage | Gap |
|---|---|---|
| New `repl_config.c` includes `glr_config.h` | — | **GAP** — no guard yet (module doesn't exist) |
| New `repl_config.c` references `audio_*`, `ui_state_*`, peer state | ✅ `check-state-boundaries` partially covers (no `repl_state.h` from neutral files) | **GAP** — needs a positive include allowlist for `repl_config.c` |
| `glr_config.c` regrows REPL-only keys | — | **GAP** — needs a guard or convention |
| `repl_export.c` / `repl_scenes.c` silently start calling `glr_config_*` again | — | **GAP** — currently allowed; would defeat the split |

Coverage: low. Step 4 clears **6 stubs** (not 8 — the
round-1 plan overcounted by attributing `ui_state_viewport` and
`ui_state_code_panel` to the cfg-table coupling; those two are
actually pulled in by `repl_export.c`'s `ui_layout_*` calls and
clear in step 7). None of the affected modules exist yet, so the
right time to land guards is *as part of step 4*: add
`check-repl-config-no-glr-config` (the new TU's include list is
restricted) and `check-repl-export-no-glr-config` (the pipeline
TUs cannot call `glr_config_*`).

#### Step 5a — Pure structured-block validator extraction

| Risk | Existing coverage | Gap |
|---|---|---|
| New `repl_compile_close_brace` / `_if_block` / `_func_def` / `_for_loop` call `set_status` | ✅ `check-no-set-status-in-compile-apply` (hard guard on `repl_compile.c`) catches this on the destination side | — |
| New `repl_compile_*` body retains editor side effects (cursor target, insert-mode toggle) | — | **GAP** — symbol guard "`repl_compile.c` cannot reference `EditorCommitPlan` / `editor_*_mut*` / `repl_state_edit_line_*` / `editor_insert_mode*`" — the new validators must produce `ReplCompiledChange` only |
| `editor_compile_*` wrapper duplicates parsing logic instead of calling through to the new pure validator | ✅ `check-state-c-shrinking` would catch sustained growth in `src/editor/commit.c` | — |
| Wrapper drops the cursor / insert-mode side-effect attachment after extraction | — | **TEST GAP** — round-trip test that `try_commit_for_loop` still moves the cursor inside the new block, etc.; existing test coverage is reasonable but the extraction is the right time to verify |

Coverage: medium. The compile-purity guard already protects the
destination. The editor-side-effect-leak risk is the new one and
needs an explicit symbol guard. The extraction itself has no
observable behavior change, so the existing test suite will flag
behavioral regressions if any side-effect attachment is dropped.

#### Step 5b — Non-editor commit API

| Risk | Existing coverage | Gap |
|---|---|---|
| New entry point lands in `repl_core.c` and bloats it | ✅ `check-state-c-shrinking` (analog) — there's no current `repl_core.c` line ratchet, but R10-phase2..5 plans dissolution | **GAP** — `repl_core.c` has no shrinking ratchet today |
| New entry point includes `src/editor/input.h` (the dispatch boundary) | — | **GAP** — no guard against `repl_*.c` including `src/editor/input.h` |
| `repl_example_loader.c` and `repl_export.c` keep calling `feed_line` | ✅ `check-no-repl-editor-input-shim` covers `src/editor/input.c` only, not the call sites | **GAP** — symbol-level guard needed |

Coverage: low. Land two guards with step 5b: (a) `check-repl-no-editor-input-include` — REPL TUs cannot `#include "src/editor/input.h"`; and (b) `check-no-feed-line-in-pipeline` — symbol-level guard against `feed_line(` calls in `repl_*.c` after the migration completes.

#### Step 6 — Reformatter + scene cursor restore extraction

| Risk | Existing coverage | Gap |
|---|---|---|
| `repl_core.c::repl_reformat_commands` body stays put | ✅ `check-state-c-shrinking` (analog needed for `repl_core.c`) | **GAP** — `repl_core.c` has no shrinking ratchet today |
| `repl_scenes.c` keeps `load_line_to_input` | — | **GAP** — symbol-level guard against `load_line_to_input(` calls in `repl_*.c` |
| Reformatter moved to `src/editor/` but pulls a REPL header it shouldn't | ✅ `check-state-boundaries` covers some of this | — |

Coverage: low. Same shape as step 5 — needs two new guards.

#### Step 7 — Slice relocation to `glr_state.c` + neutral export bag

| Risk | Existing coverage | Gap |
|---|---|---|
| Some `presentation` / `render` field stays behind in `repl_state.c` (not all moved) | ✅ `check-state-c-shrinking` (line ratchet on `repl_state.c`) catches incomplete moves; the move only counts when both the field and its accessor leave | — |
| `repl_state.c` re-introduces a presentation/render reference after the move | — | **GAP** — symbol-level guard "`repl_state.c` cannot reference `glr_state_*`" closes this |
| `glr_state.c` calls into REPL pipeline mutators (e.g., to read predef vars) | ✅ `check-views-no-owners` analog needed for the new owner; otherwise none | **GAP** — extend `check-state-boundaries` to forbid `repl_state_*_mut*` calls from `glr_state.c` |
| New `glr_state.h` accidentally gets included by `src/scene/*.c` or `src/ui/*.c` | — | **GAP** — extend `check-views-no-owners` to also forbid `glr_state_owners.h` (or whatever the mutator header becomes named); presentation/render were *already* legitimately read by views via the snapshot, so the read header should remain includable |
| `repl_export.c` keeps reading `repl_state_presentation_*()` or `glr_state_*` after the relocation | — | **GAP** — symbol guard "`repl_export.c` cannot reference `glr_state_*`, `glr_config_*`, `glr_camera_*`, peer state, audio" — forces all cfg flow through `ReplExportProperties` and the camera block through `ReplExportCameraBlock` |
| `repl_export.c` reads cfg slugs by name (e.g. `strcmp(key, "wireframe")`) | — | **GAP** — symbol/literal guard rejecting known slug strings inside `repl_export.c`; if the file has to know "wireframe" exists, the abstraction has leaked |
| `repl_export.c` keeps calling `ui_layout_scene_rect` / `ui_layout_code_panel_rect` (the layout pull-in that step 4 *doesn't* address) | — | **GAP** — symbol guard "`repl_export.c` cannot reference `ui_layout_*`"; layout values must enter via the export-options struct, mirroring the cfg/camera opacity rule |
| `repl_scenes.c` keeps reading presentation/render after the per-slot snapshots become `ReplExportProperties` | — | **GAP** — symbol guard, same shape as the export guard |
| Owner fill/apply helpers grow inside `repl_export.c` instead of in their owners (e.g. someone adds `repl_export_props_fill_glr_state()`) | — | **GAP** — fill/apply helpers must live in their owners' TUs (`glr_state_fill_export_props`, `camera_fill_export_block`, etc.); a guard "`repl_export.c` defines no `*_fill_export_*` / `*_apply_export_*` symbols" keeps the rule one-way |
| `@cfg` save-file format silently drops a slug because the property bag wasn't filled by some owner | — | **TEST GAP** — round-trip every `glr_config_items()` slug through `glr_state_fill_export_props` → `repl_export_save` → `repl_export_load` → `glr_state_apply_export_props` and assert the value preserved. The neutral abstraction makes this test trivial: feed the bag in, read it out, compare |
| Camera block silently drops because someone forgot to pass `&cam_block` to the new save/load entry points | ✅ The new function signature requires it — the compiler catches a NULL/missing argument | — |

Coverage: low. Step 7 is the largest single restructure, and most of
the existing guards are scoped to `repl_*.c` files — they don't
automatically extend to a new `glr_state.c`. The fence has to be
duplicated for the new owner. The shape is identical (read-only
views header + mutator-only header + scene/UI can include views,
not owners), so the work is template-driven, not novel.

The most-leveraged guard for this step is the `repl_export.c` symbol
guard listed above: once `repl_export.c` cannot reference *any*
`glr_*` / peer / audio symbol, the compiler refuses to let the
property-bag abstraction be circumvented. Combined with the
fill/apply-helper-location guard (helpers live in the owner's TU,
not in `repl_export.c`), the abstraction stays one-way: owners
*push* to / *pull* from a neutral bag; `repl_export.c` is opaque to
slug semantics.

### The umbrella guard: stub-count ratchet

The cleanest meta-guard is a ratchet on `tools/repl_demo/stubs.c`
itself. **If any plan step regresses, the linker compensates by
needing more stubs.** A `check-repl-demo-stubs-shrinking` script
counts the externally-visible symbols in
`build/*-gl-stubs/tools/repl_demo/stubs.o` (or a textual count of
`^[A-Za-z_][A-Za-z0-9_]* [a-zA-Z_]+` definitions in `stubs.c`) and
fails if the count exceeds a baseline that ratchets downward only.

Today's baseline: **17**. After step 1 (target): 16. After step 2: 11.
After step 3: 10. After step 4: 4 (clears 6 — config-owned only;
`ui_state_viewport` and `ui_state_code_panel` survive past step 4).
After step 5a: 4 (no stub change; pure extraction). After step 5b:
3. After step 6: 2. After step 7: **0** (clears `ui_state_viewport`
and `ui_state_code_panel` via the layout-opacity rework alongside
the cfg-opacity rework). The ratchet stays at 0/0 thereafter.

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
| `check-no-set-status-in-pipeline` (extend the compile-apply guard to **6 TUs**: executor, flatten, export, scenes, example_loader, core) | 3 | 30 min — clone the existing script |
| `check-repl-no-editor-input-symbols` (`feed_line`, `load_line_to_input`, `repl_editor_reset_transients` not callable from `repl_*.c`) | 2, 5b, 6 | 1 hr — symbol-level grep over REPL_SRCS |
| `check-repl-state-no-editor-completion` (`repl_state.c` cannot reference `editor_completion_*` or `*_register_provider*` — the program-only reset must not register editor-side providers) | 2 | 15 min |
| `check-repl-no-glr-config` (`repl_*.c` cannot reference `glr_config_*` after split) and reciprocal | 4 | 30 min |
| `check-repl-state-c-no-peer-resets` (extend `check-editor-ownership-budget` to peer/editor reset symbols) | 2 | 30 min — add new ratchet keys |
| `check-repl-compile-no-editor-side-effects` (the new structured-block validators in `repl_compile.c` cannot reference `EditorCommitPlan`, `editor_*_mut*`, `editor_insert_mode*`, or `repl_state_edit_line_*`) | 5a | 30 min |
| `check-repl-core-c-shrinking` (clone of the `repl_state.c` ratchet for `repl_core.c`) | 3, 5b, 6 | 15 min |
| `check-repl-state-no-glr-state` (symbol guard: `repl_state.c` cannot reference `glr_state_*`) and reciprocal | 7 | 30 min |
| `check-glr-state-no-repl-mutators` (clone of `check-views-no-owners` shape, applied to the new owner) | 7 | 30 min — extend allowlist of files allowed to write `repl_state` |
| `check-repl-export-opaque-to-environment` (`repl_export.c` cannot reference `glr_state_*`, `glr_config_*`, `glr_camera_*`, peer state, `audio_*`, **or `ui_layout_*`**; cannot string-match known cfg slugs; cannot define `*_fill_export_*` / `*_apply_export_*` helpers — those live in owner TUs) | 7 | 1 hr — four orthogonal greps, one allowlist for the bag iterators |
| `check-repl-scenes-no-glr-state` (`repl_scenes.c` cannot reference `glr_state_*` after the per-slot snapshot becomes a `ReplExportProperties`) | 7 | 30 min |
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
