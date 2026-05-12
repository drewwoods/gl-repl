# REPL Architecture

> For the quick module map, see [`MODULES.md`](MODULES.md). For the
> staged controller-extraction history (now landed), see
> [`done/push-architecture-refinement.md`](done/push-architecture-refinement.md).

## Direction

This document follows the controller-first direction originally laid
out in `done/push-architecture-refinement.md` (the plan
shipped; the doc lives in `done/` as design history).

The older "generic scene callback plus `ReplGeometryRenderPlan`" direction is
superseded. This is a one-frontend REPL sample, so the useful boundary is
between the REPL model/controller and the rendering views. The goal is not to
turn `scene_*` into a plugin host.

Current code already routes frame wiring through `src/app/glr_ctrl.c`. `src/repl/core.c`
now keeps the REPL model/pipeline wrappers, while `src/scene/render.c` consumes
explicit per-frame config. Phase 2 is still in progress; remaining work is
mostly about shrinking transitional state/config surfaces and removing
allowlisted view-layer state mutations.

## Ownership Model

```text
repl_*        = language, source model, flat program, replay model, input/model controllers
glr_*         = app-shell namespace: app router (glr_ctrl), camera (glr_camera),
                menu/config actions (glr_actions, glr_config), CLI debug dumps (glr_debug)
editor_*      = text-document model + controller (under src/editor/)
scene_*       = 3D stage: camera, projection, frame setup, decorators, 3D overlays
ui_*          = 2D editor chrome: code panel, menus, overlays, popups, HUDs
sample.c/h    = current GLUT entry point and legacy shared header
                (rename to a `glr_*`-namespaced shell is on the open list)
```

The prefix is an ownership signal, not a generic sample prefix. New `repl_*`
modules should own REPL language, editor, source, workspace, replay, or command
model behavior. App-shell services belong under `glr_*`. Generic infrastructure
keeps neutral names such as `prof`. `audio` (formerly `repl_audio`) and
`sample` are the remaining names that fall outside this scheme; both are slated
for the namespace audit rather than serving as precedents.

The main design rule:

```text
The REPL owns the user program.
The scene owns the 3D stage.
The UI owns the 2D editor/view.
The controller translates REPL state into per-frame view inputs.
```

Under Option B, scene modules may consume `FlatProgramView`, `CmdType`, and
other command-domain data when that data is already present in the
`SceneRenderConfig` or a derived frame snapshot. They should not fetch REPL
globals or call `repl_state_*` APIs directly during rendering.

## Adding Or Migrating An Owner Module

When a module starts owning mutable REPL state, follow the Stage-1 template:

1. Put the live bytes in `ReplRuntimeState` unless the state is intentionally a
   sidecar such as undo rings or user-scene slots. If it is a sidecar, call
   that out explicitly instead of describing it as runtime-state migration.
2. Add a named runtime slice in `src/repl/state.h`, wire it into
   `static ReplRuntimeState g_repl_state;`, and say whether the read path is
   currently `facade-backed`, `direct-runtime`, or `value-getter`.
3. Keep mutations on the owner side. Scene/UI renderers read snapshots only;
   render-time discoveries return through output structs that the controller
   actualizes back into state.
4. Extend the ownership tests in the same change: keep
   `repl_state_capture()`, `repl_state_restore()`, and
   `repl_state_reset_program()` (REPL-only) / `glr_app_reset_all()`
   (full-world) current for runtime slices, and add focused behavior
   coverage in the module's own tests.

## Core Tenets

1. **The REPL owns the user program.** It parses source, stores source commands,
   flattens loops/functions/conditionals, owns predefined variables, and owns
   replay policy.
2. **The executor is the narrow live-GL gate for user geometry.**
   `src/repl/executor.c` turns a flat program into OpenGL calls. General `repl_*`
   modules should not casually call OpenGL.
3. **The scene owns the stage, not the editor.** It sets viewport, clear,
   projection, camera, accumulation, baseline lighting, grid, axes, backdrop,
   light indicators, orbit target, and 3D overlay passes from config.
4. **The UI owns screen-space presentation.** UI renderers draw code rows,
   menus, popups, color picker, help, status, and profile views from snapshots
   and route mutations through REPL-owned actions or stores.
5. **The controller is the mixed layer.** The frame controller builds scene and
   UI inputs from REPL state, calls the scene renderer, then calls UI renderers.
   This role belongs in `src/app/glr_ctrl.c`.
6. **Replay is REPL policy.** Replay state machine, PC, mode, baseline values,
   and fade/highlight decisions belong in `src/widgets/replay.c` or follow-up replay
   planning code. Any scene use of replay data should be via snapshots or
   documented transitional helpers.

## Target Frame Pipeline

Top-level frame orchestration belongs in the controller:

```text
sample.c GLUT display callback (future `glr` shell)
  -> glr_ctrl_display_frame          (sample.c calls controller directly; no shim)
        -> tick profiling
        -> rebuild autonormals if dirty
        -> rebuild flat program if dirty                          [PROF_FLATTEN]
        -> push editor snapshots (transformers / highlights /     [PROF_SNAPSHOT*]
           virtual lines via repl_replay_annotations_prepare)
        -> save live predefined variable values
        -> prepare replay frame if replay is active
        -> update export/camera strings
        -> build SceneRenderConfig from REPL state                [PROF_SNAPSHOT_SCENE_CONFIG]
        -> build UiRenderSnapshot from REPL state                 [PROF_SNAPSHOT_UI]
        -> scene_render_3d_scene(&scene_cfg)                      [PROF_SCENE_3D]
        -> ui_panels_render_code_panel(&ui_snap)                  [PROF_CODE_PANEL]
        -> ui_*_render(&ui_snap) overlays                         [PROF_UI_PANELS]
        -> ui_profile_panel_render(&ui_snap)
        -> restore flat count and predefined variable values
```

Profile sections wrap each producer so snapshot construction time is
visible: `PROF_SNAPSHOT` is the aggregate, with sub-sections for
transformers, highlights, virtual lines, scene config, and ui snapshot
(see `prof.h`).

The scene frame consumes the explicit config:

```text
scene_render_3d_scene(&scene_cfg)
  -> set viewport
  -> resolve and apply clear color from scene_cfg.flat_program
  -> for each accumulation sample:
       -> prepare FrameRenderContext from scene_cfg
       -> apply projection using scene-local jitter
       -> apply camera and quality flags
       -> set up baseline lighting/material state
       -> execute user geometry through the narrow execution boundary
       -> invoke optional `post_fill_fn` (controller's replay-fade overlay)
       -> render backdrop, grid, axes, orbit target
       -> render REPL-aware 3D overlays from frame snapshots
       -> render light indicators and other scene foreground helpers
       -> accumulate sample if accumulation AA is active
```

The exact ordering may preserve current visuals. The ownership rule still
holds: the controller prepares the data, the scene decides where stage and
overlay passes occur, and the REPL owns the command/replay semantics behind the
data.

## Two-Level Command Model

The REPL keeps source commands and flattened commands separate.

```text
source commands
  one visible/editor line per command

flattened commands
  loops expanded
  functions inlined
  conditionals resolved
  provenance retained
```

Source commands are the editing model.

Flattened commands are the execution, replay, export, and 3D annotation model.

Code outside the command pipeline should use `FlatProgramView` or a snapshot
derived from it instead of poking raw global arrays.

### Editor-owned text (post `feature/editor-owns-text.md`)

`GLCmd` is a pure parse-result struct: `type`, `args[]`, validity / vars
flags, and provenance fields (`src_cmd_idx`, `call_src_cmd_idx`, etc.).
There is no `source[]` member. Per-line text lives in
`ReplEditorBuffer.lines[MAX_COMMANDS][MAX_LINE_LEN]` inside
`ReplRuntimeState`. The parser returns both the `GLCmd` and the
canonical text in `ReplParsedLine { GLCmd cmd; char text[MAX_LINE_LEN] }`;
commit code passes both to text-aware command-store APIs
(`repl_command_store_*_with_line[s]`) so the editor buffer moves in
lockstep with the command array.

Persistence sidecars carry parallel `lines[][]` arrays:

| Persisted form | Module |
|---|---|
| Undo / redo snapshots | `repl_undo` (`ReplUndoSnapshot.editor_lines`) |
| User-scene slots | `repl_scenes` (workspace save / load + LRU eviction) |
| Clipboard cmds | `ReplClipboardState.lines` |
| Single-file / workspace export | `repl_export` (no extra sidecar; export reads `editor_buffer`) |

Flat commands have no text of their own. `repl_flat_cmd_text(flat_cmd)`
maps a flat command to its source-buffer line via `src_cmd_idx`.

## Controller-Pushed Editor Snapshots

The controller treats per-frame UI overlay data as snapshots it builds
once and the UI consumes read-only. The snapshot family lives in
`src/ui/editor.h`:

| List | Push helper | What it carries |
|---|---|---|
| `EditorTransformerList editor_transformers` | `glr_ctrl_push_color_transformers()` | One entry per editable color command (line idx + r/g/b/a/has_alpha/is_clear). Drives inline swatch render and color-picker hit-test. Future kinds: numeric slider. |
| `EditorHighlightList editor_highlights` | `glr_ctrl_push_highlights()` | Feeding-normal cmd, feeding-color cmd, replay PC, search match, selection. Rendered as gutter accents and row backgrounds. |
| `EditorVirtualLineList editor_virtual_lines` | `repl_replay_annotations_prepare()` (via `_refresh_virtual_lines()`) | Replay-time annotation rows (substitution + evaluation) attached to the current source line. Layout, scroll, hit-test, and render all read from this list, so virtual-row counts have one source of truth (`repl_replay_annotation_extra_rows_for_line()` counts the list). |

All three lists are stored on `ReplRuntimeState` as named slices and
exposed via `repl_state_editor_*()` accessors (read-only view in
`src/repl/state_views.h`, mutating clear/append in `src/repl/state_owners.h`).
`UiRenderSnapshot.editor_transformers / editor_highlights /
editor_virtual_lines` are pointers into those slices.

## Command Lifecycle

A user line follows this path:

```text
input text
  -> commit handler
  -> parser
  -> source command store
  -> flatten
  -> scene config / overlay snapshots
  -> executor boundary
```

Owned stages:

| Stage | Owner |
|-------|-------|
| GLUT input dispatch (cross-subsystem routing) | `src/app/glr_ctrl.c` |
| Editor text-document input + commit orchestration | `editor_input.c` + `editor_commit.c` |
| Parsing | `src/repl/parser.c` |
| Validation / compilation (pure, returns `ReplCompiledChange`) | `src/repl/compile.c` |
| Apply (writes `ReplState` only) | `src/repl/apply.c` |
| Source command mutation (low-level shifts) | `src/repl/command_store.c` |
| Source scope/depth | `src/repl/source_scope.c` |
| Flattening | `src/repl/flatten.c` |
| User geometry execution | `src/repl/executor.c` |
| Export/import | `src/repl/export.c` |

Note: `repl_editor.{c,h}` and `repl_commit.{c,h}` are deleted (Phase J1
+ Phase H.5). Their responsibilities split into the entries above.
`check-no-repl-editor-input-shim` and `check-no-repl-commit` hard-guard
against either filename returning.

Outside code that needs to inject commands should use the public command/input
paths instead of directly mutating command arrays.

## Controller Layer

The controller layer is the home for app-frame wiring that used to live in
`src/repl/core.c`.

Responsibilities:

* rebuild flat program and autonormals when dirty
* prepare replay frame clamps and restore state after rendering
* build `SceneRenderConfig` and any guide/focus snapshots from REPL state
* call `scene_render_3d_scene(&config)`
* call UI renderers in the correct order
* keep profiling section boundaries around scene and UI rendering

`src/app/glr_ctrl.c` may include both REPL headers and scene/UI headers. Ordinary REPL
model modules should not.

`sample.c` and `sample.h` still carry the app entry point and shared legacy
types/constants. A future `glr_*`-namespaced rename of the shell is open work
(see R8 in *Open Refactor Edges* below); it is intentionally a separate
mechanical cleanup after controller extraction, because `sample.h` is included
broadly.

## Scene Render Config

`SceneRenderConfig` is the scene's explicit per-frame input. In Option B it is
allowed to carry REPL-aware data because this sample has one frontend and no
plugin host requirement.

The controller builds the config once per frame, and `scene_render_3d_scene()`
consumes it directly without calling back into REPL globals or rebuilding the
frame inputs itself. The config currently carries the execute callback,
`FlatProgramView`, viewport, camera, animation, quality flags, lighting,
backdrop, overlay toggles, replay/HUD layout, grid tables, cursor-block
metadata, and the `SceneFocusVertex` / `SceneGuideSnapshot` snapshots needed by
3D overlays.

Scene-local accumulation jitter no longer lives in the config. Derived
per-pass data belongs in `FrameRenderContext`, for example camera world height,
focus vertex, and other values that helper renderers should share.

## Scene Layer

Scene modules own 3D rendering and 3D helper visuals.

Responsibilities:

* viewport and projection setup
* camera transform
* accumulation-buffer sampling with scene-local jitter
* baseline scene lighting and material state
* grid, axes, backdrop, light indicators, orbit target
* REPL-aware 3D overlays while they remain under `scene_*`
* replay fade rendering until the replay simplification follow-up moves it

Neutral scene modules such as `src/scene/grid.c`, `src/scene/axes.c`,
`src/scene/backdrop.c`, and `src/scene/lights.c` should remain free of REPL
state access. REPL-aware overlays now live with the controller
(`geometry_guides.c` / `transform_guides.c` at the repo root) and consume
the explicit `SceneGuideSnapshot` rather than pulling globals directly.

## UI Layer

The UI layer owns 2D editor rendering.

Responsibilities:

* code panel
* menus and dropdowns
* search slot
* autocomplete popup
* variable panel
* color picker
* help overlay
* profile HUD
* status banners and other screen-space overlays

UI renderers draw from a single per-frame `UiRenderSnapshot` (defined in
`src/ui/snapshot.h`) that the controller builds once via
`glr_ctrl_build_ui_snapshot()` and passes to every `ui_*_render*()`
entry point. Render code does not call `repl_state_*()` directly. The
`check-ui-no-repl-state-read` Makefile guard enforces the snapshot-shaped
signature for audited renderers.

`UiRenderSnapshot` carries:

* by-value `Repl*State` slices (presentation, code_panel, render, replay,
  search, autocomplete, status, …) — small structs cheap to copy
* pointer-shaped read-only views (`ReplVariableView`, `ReplEditorInputView`,
  `ReplImportExportView`, `FlatProgramView`, `ReplPredefView`)
* document/flat metadata (`document_cmds`, `document_count`, `edit_line`,
  `flat_program_count`, …)
* user-scene names + slot-used flags
* the controller-pushed editor snapshot pointers
  (`editor_transformers`, `editor_highlights`, `editor_virtual_lines`)
* per-frame derived metadata so the render path never re-derives:
  `selection_active / selection_lo / selection_hi`,
  `active_indent_chars`, `trailing_indent_chars`, `in_begin_block`,
  `current_begin_mode`

Slices that would have been heavy to copy are deliberately excluded:
`ReplClipboardState` (~1.88 MB with the lines sidecar) is not on the
snapshot — the per-row selection band reads `selection_lo/_hi` instead.

**Two selection models, one clipboard.** `selection_lo/_hi` above is
the *line-range* selection used by gutter drag and the multi-line
clipboard (`anchor_idx`/`end_idx` on `ReplSelectionState`). The
*input-buffer* selection is a separate character-range model on
`ReplEditorInputState.anchor_pos`, scoped to the active edit row only
— shift+arrow, double-click word, drag-on-edit-row, and partial-line
copy/cut/paste all drive that anchor. The two share one tagged
clipboard object (`ReplClipboardState` carries an `EditorClipboardKind`
discriminator plus both a line array and an `input_text` slot) so
`Ctrl+V` after a partial copy pastes characters and `Ctrl+V` after a
line copy still pastes whole commands. Input selection wins over
line-range for `Ctrl+C` / `Ctrl+X` priority. See
[`done/editor-input-selection.md`](done/editor-input-selection.md)
for the full rules.

Mutations route through `repl_actions`, `repl_command_store`,
`variable_panel_drag`, or another REPL-owned mutation path. UI input
hit-tests (`*_hit_test`, `*_rect`) compute neutral `UiHit` values and
return — `glr_ctrl_router_handle_code_panel_hit` dispatches by
`UiHit.kind` to the owning subsystem (Phase J2). Render-side
discoveries (e.g. the editor cursor pixel computed during
`render_active_input_rows`) flow back through per-frame
`Ui*Output` structs that the controller actualizes after the render
call (Phase J4 introduced `UiCodePanelOutput`; the pattern is
hard-guarded by `check-output-actualization`). Two render-path live
reads remain (`repl_code_panel_document_build` /
`apply_follow_scroll` and `repl_replay_code_panel_get_command_display_text`);
both produce snap-equivalent results because they run after the
controller has finished updating live state, but converting them to
take a snapshot pointer is the next layer of cleanup.

## Replay Architecture

Replay is REPL-owned. The scene may render the current visual effect, but it
should not own replay policy.

R1 target from `done/push-architecture-refinement.md` (landed):

* controller builds a `ReplayFadePlan` snapshot once per frame (batches,
  alpha, skip limits, baseline predef values)
* scene iterates the snapshot and owns the GL pass orchestration without
  calling `repl_replay_*` or `repl_state_*`
* accumulation-AA settings are `SceneRenderConfig` fields set by the controller
* 2D replay HUD lives in `src/ui/replay_hud.c`, driven by config fields
* `scene_*.c` files contain no `repl_state_*` or `repl_replay_*` calls; once
  the relevant Phase 2 slice is complete, Makefile checks keep that true

## Boundary Rules

### Live OpenGL / GLU calls

Allowed:

```text
scene_*.c
ui_*.c
src/repl/executor.c
sample.c        GLUT/window lifecycle and buffer swap; future `glr` shell
```

Avoid live GL calls in all other `repl_*` files. Text emission of GL command
names in parser/export/example/spec code is not a live GL call.

### GLUT calls

Allowed:

```text
sample.c        GLUT callback registration, glutInit, buffer swap
                (the future `glr` shell takes over after the R8 rename)
src/app/glr_ctrl.c      GLUT modifier reads + cross-layer input routing
                (took over from the deleted repl_editor.c in Phase J1)
editor_input.c  glutGetModifiers via editor_get_modifiers (gated behind
                editor_input_enable_glut_modifier_reads so tests stay safe)
src/repl/executor.c tessellator callback setup only
```

### Controller-only scene wiring

After controller extraction, ordinary `repl_*` model files should not include
`scene_*.h`. `src/app/glr_ctrl.c` is the scene/UI frame-rendering exception.
`check-controller-boundaries` enforces this; cross-layer constants used by
both layers (e.g. `CFG_DEFAULT_MULTISAMPLE`, `REPL_OUTLINE_POLYGON_OFFSET_*`)
live in neutral headers (`src/app/glr_defaults.h`, `outline_offset.h`,
`src/scene/render_types.h`) that both sides include via existing transitive
paths.

Remaining `ui_*` include exceptions: `repl_actions.c` and `src/repl/export.c`.
The `repl_editor.c` exception is gone — that file is deleted (Phase J1).
The other two require separate cleanup tracks tied to the deferred
`repl_actions` rename and the export-as-its-own-feature split.

### Scene state access

Target rule: `scene_*` files consume `SceneRenderConfig`, `FrameRenderContext`,
or explicit snapshot structs. They should not call `repl_state_*` directly.

R11 in the refinement plan adds `check-state-boundaries` with transitional
allowlists. The strict no-exception version belongs to the end of the relevant
Phase 2 cleanup, not to the beginning.

### UI mutation boundary

`ui_*` renderers route REPL mutations through their owning peer or
through the editor commit pipeline (`repl_actions` for menu actions,
`editor_commit_apply_external_change` for picker writebacks,
`variable_panel_drag_*` for slider transactions, `replay_handle_*`
for replay buttons). `repl_state_*_mut()` accessors directly from
`ui_*` files are not permitted. The known historical violations in
`ui_color_picker`, `ui_panels`, and `ui_help_overlay` were all
closed; their work redistributed into peer subsystems (`color_picker`)
or generic renderers (`ui_tabbed_overlay` consuming
`UiOverlayContent` adapted by `glr_ctrl` from `repl_help_text`).

`ui_*.c` files include `src/repl/state_views.h` only, not `src/repl/state.h`
or `src/repl/state_owners.h`. `check-views-no-owners` enforces this;
`check-ui-returns-hits-only` (baseline 0/0) keeps any new mutator
out of the input + render paths;
`check-color-picker-ui-isolation` and `check-replay-ui-isolation`
audit the feature-UI prefixes.

### UI / scene independence

`ui_*` and `scene_*` are sibling view layers. They should not include each
other's headers. Shared render-neutral helpers belong in local shared headers
or project-wide `include/` only when broadly reusable.

## Standalone REPL Demo Coupling

`tools/repl_demo/repl_demo.c` is a negative boundary proof, not a packaged
REPL library. It proves that the core path it drives directly

```text
parse -> command store -> flatten -> execute
```

can run without `src/app/glr_ctrl.c`, `src/editor/input.c`, `src/ui/*.c` renderers,
or `src/ui/replay_hud.c`. It does **not** yet prove that the REPL pipeline has a
stub-free link boundary. The current `REPL_DEMO_DEP_SRCS` object list still
pulls in several app/editor/UI-adjacent translation units, and
`tools/repl_demo/stubs.c` supplies the missing symbols when those owners are
intentionally left out.

Treat `tools/repl_demo/stubs.c` as a dependency ledger. The execution
plan in `feature/decouple-repl-from-gl-repl-alt.md` ratchets this
ledger from 17 stubs to 0 across seven steps. The current count
(verified via `nm` on `build/release-gl-stubs/tools/repl_demo/stubs.o`)
is **4**: steps 1, 2, 3, and 4 have landed.

### Stubbed Couplings

| Coupling root | Stubbed symbols | Why the link reaches it | Demo exercises it? | Removal path | Status |
|---|---|---|---|---|---|
| Global lifecycle reset | `ui_state_reset`, `variable_panel_state_reset`, `editor_help_session_reset`, `repl_editor_reset_transients` | (Historically) `repl_state_init_defaults()` called `repl_state_reset_all()`, which reset every app singleton / peer, not just `ReplState`. | Yes, every sample reset entered this path. | Split into pure `repl_state_reset_program()` and app-level `glr_app_reset_all()`. | ✅ **Cleared (step 2, commit `310eca0`).** Reset renamed; full-world reset moved to `src/app/glr_ctrl.c`. |
| UI chrome mirror | `ui_state_code_panel_mut` | `repl_state_sync_ui_chrome()` mirrored presentation fields into `UiState.code_panel`; `repl_state_reset_all()` called it. | Yes, via reset. | Move chrome mirroring to the controller. | ✅ **Cleared (step 2, commit `310eca0`).** Body moved to `glr_ctrl_sync_ui_chrome()` in `src/app/glr_ctrl.c`. |
| Compile dispatcher location | `repl_compile_dispatch` | `repl_compile_toggle_comment()` needed the float-decl / var-assign dispatcher, but the implementation lived in `src/editor/services.c`. | No; the demo does not toggle comments. The reference was still hard at link time. | Move the dispatcher into `src/repl/compile.c`. | ✅ **Cleared (step 1, commit `ef3fc09`).** Moved into `src/repl/compile.c`; declared in `src/repl/compile.h`. |
| Status relay | `ui_state_status_set` | Legacy `src/repl/core.c::set_status()` forwarded diagnostics to UI status text. | Only on errors or helper paths that report status. | Replace `set_status()` body with a callback dispatch (`repl_set_status_sink`); controller installs `ui_state_status_set` as the sink at app startup. Demo doesn't install one, so set_status is a no-op there. | ✅ **Cleared (step 3, commit `5f1b8bc`).** Callback branch chosen over per-function out-params; ~15 pipeline-side set_status call sites remain as future per-TU cleanup but no longer drag the stub. |
| Programmatic editor input | `feed_line`, `load_line_to_input` | `src/repl/export.c` imports exported files by feeding lines through the editor commit path; `src/repl/core.c` and scene activation paths also have legacy line-loading references. | No for the current static samples. | Extract pure structured-block validators (5a) and add a non-editor source-loader API (5b); move reformatter and scene cursor-restore out of REPL pipeline TUs (6). | Pending (steps 5a/5b/6). |
| Config descriptor table | `g_cfg_items`, `CFG_ITEM_COUNT` | `src/app/glr_config.c` iterated menu/action descriptors while parsing or applying config keys, but the table is defined in `src/app/glr_actions.c`. | Not for the current samples; relevant to `@cfg` metadata and export/import helpers. | Introduce a neutral `ReplExportConfig` bag in `src/repl/export.h` and a controller-installed bridge that fills/applies it; pipeline TUs (`src/repl/export.c`, `src/repl/scenes.c`) stop calling `glr_config_*`. | ✅ **Cleared (step 4, commit `b58cdef`).** Bridge installed in `glr_app_reset_all`; demo doesn't install one → @cfg is a no-op there. |
| App-owned config storage | `audio_get_cfg_mode`, `audio_set_cfg_mode`, `ui_state_profile_panel_mut`, `variable_panel_view_mut` | `src/app/glr_config.c` mapped config keys directly to storage owned by audio, UI profile panel, and the variable-panel peer. | Not for the current samples. | Same fix as the descriptor table row: pipeline TUs go through the `ReplExportConfigBridge` instead of calling `glr_config_*`. `src/app/glr_config.c` falls out of the demo link set, so its references to audio / profile / variable_panel disappear. | ✅ **Cleared (step 4, commit `b58cdef`).** |
| UI layout state | `ui_state_viewport`, `ui_state_code_panel` | `src/ui/layout.c` reads live `UiState`; `src/repl/export.c` uses layout helpers for export viewport sizing and code-panel visual dumps. | Not for the current samples. | Pass viewport/layout values to `repl_export` as explicit export inputs; move app-state slices to `src/app/glr_state.c`. | Pending (step 7). |

### Non-stubbed But Still Non-pipeline Link Dependencies

These objects are linked into `repl_demo` rather than stubbed:

| Object(s) | Why present | Removal path |
|---|---|---|
| `src/editor/state.c`, `src/editor/completion.c` | Per-line canonical text currently lives in `EditorState`; `repl_state_init_defaults()` also registers the REPL autocomplete provider with the editor completion registry. | Keeping editor-owned text makes this dependency intentional. Removing it means introducing a neutral source-document/text store owned by the REPL command document, then making the editor a controller/view over that store. That is feasible but large, because it cuts across commit, undo, scenes, clipboard, export, and tests. Autocomplete registration is easier: move it to app/editor startup instead of REPL state reset. |
| `src/widgets/replay.c`, `src/widgets/replay_state.c`, `src/repl/replay_annotations.c` | Replay is a peer subsystem, but reset and annotation helpers are still in the demo link set through broad REPL object selection. | Split app reset from REPL reset and keep annotation preparation out of the minimal demo object list unless the demo explicitly exercises replay. Medium effort. |
| `src/repl/export.c`, `src/repl/example_loader.c`, `src/repl/scenes.c`, `src/app/glr_camera.c`, `src/app/glr_config.c`, `src/ui/layout.c` | `src/repl/core.c` is still a residual helper bucket, and export/import/workspace/camera helpers sit behind it. The demo uses only a small subset (`repl_parse_and_normalize`, `cmd_type_name`), but the whole translation unit graph comes along. | Continue dissolving `src/repl/core.c`: move normalization into parser/format code, move startup/workspace helpers to scene/export owners, and split export generation from import and code-panel dump helpers. Medium effort, with high payoff for a clean demo link boundary. |
| `repl_autocomplete.c`, `src/repl/help_text.c` | REPL state initialization registers the completion provider, and help text is part of the broad demo object list. | Treat completion/help as optional app/editor services. Low to medium effort if reset is split first. |

### Practical Decoupling Sequence

The full plan lives in `feature/decouple-repl-from-gl-repl-alt.md`
(7 steps, stub trajectory 17 → 0). At a glance:

1. ✅ **Step 1 — Move `repl_compile_dispatch()` out of `src/editor/services.c`** (commit `ef3fc09`).
2. ✅ **Step 2 — Split `repl_state_reset_all()` into program-only reset
   and app-wide reset; move autocomplete registration and UI chrome
   sync to the app side** (commit `310eca0`).
3. ✅ **Step 3 — Route pipeline `set_status()` through a controller-
   installed callback sink; move startup banner to the controller**
   (commit `5f1b8bc`). Callback branch chosen over per-function
   out-params for cost reasons; per-TU conversion remains a future
   opportunity.
4. ✅ **Step 4 — Introduce neutral `ReplExportConfig` bag and bridge;
   make `src/repl/export.c` and `src/repl/scenes.c` opaque to cfg semantics**
   (commit `b58cdef`). Bridge installed by the controller; demo
   doesn't install one → @cfg path is a no-op there. `src/app/glr_config.c`
   falls out of the demo link set.
4a. ✅ **Step 4a — Camera-block neutralization** (commit `f23d866`).
   Same shape as the cfg bridge: `ReplExportCameraBlock` (4-line
   block) + `ReplExportCameraBridge` (fill_save_block /
   fill_display_block / fill_save_preamble / try_consume_import_line /
   reset_import). Bridge implementation lives in `src/app/glr_camera_export.c`.
   `src/repl/export.c` no longer references `glr_camera_*` — verified via
   `nm build/release-gl-stubs/repl_export.o`. Architectural cleanup
   only; no stub change. `src/app/glr_camera.c` stays in the demo link set
   until step 7 closes the last two doors (auto_rotate reset in
   `src/repl/state.c` + example camera presets in
   `src/repl/example_loader.c`).
5. Step 5a/5b — Extract pure structured-block validators from
   `editor_compile_*`; add non-editor source-load/commit API so
   examples and imports stop calling `feed_line`.
6. Step 6 — Move reformatter and scene cursor-restore out of REPL
   pipeline TUs into editor/controller code.
7. Step 7 — Move app-state slices (`presentation`, `render`) to a new
   `src/app/glr_state.c`; pass viewport/layout to `repl_export` as explicit
   export inputs.
6. Decide whether editor-owned text remains an explicit dependency. If the
   goal is a reusable standalone REPL library, create a neutral source-document
   owner and adapt the editor around it. If this remains a one-frontend sample,
   keeping `src/editor/state.c` in the demo link set is a defensible tradeoff
   as long as it is documented as intentional.

## Where To Put New Code

* New REPL syntax: `src/repl/parser.c`, `src/repl/command_spec.c`, `src/repl/compile.c`,
  `src/editor/commit.c`, `src/repl/flatten.c`, and `src/repl/executor.c` as needed.
* New user-geometry execution behavior: `src/repl/executor.c`.
* New 3D world decorator: `scene_*`.
* New 3D REPL-aware overlay: current home is still `scene_*`, consuming
  `FlatProgramView` or a snapshot from `SceneRenderConfig`.
* New 2D UI: `ui_*` renderer plus `repl_*` model/action code if mutation is
  required.
* New per-frame scene/UI wiring: `src/app/glr_ctrl.c`.
* New app lifecycle/window wiring: `sample.c` for now, future `glr` shell
  after the R8 rename.
* New command mutation: `repl_command_store_*`.

## Adding A New Command

This is the canonical checklist for adding a new GL/GLU/GLUT command, REPL
primitive (e.g. `label`), or math/expression function (e.g. `rand2`) to the
REPL. **Every numbered step is required** unless the note marks it optional.
Skipping any step ships a half-wired feature: a command that parses but has no
F1 help, no autocomplete, no replay annotation, or — worst — diverges between
the live REPL and exported `output.c`. The GLUT solid shapes
(`glutSolidCube`, `glutSolidSphere`, `glutSolidTeapot`, `glutSolidCone`) are
the canonical worked example for a GL command; `label` (REPL primitive) and
`rand2` (math function) are the worked examples for the two off-the-main-path
shapes that the recent commits tripped on.

> **What kind of thing am I adding?** The path branches at step 0.
>
> - **Bound GL/GLU/GLUT command** (most common — `glutSolidCube`,
>   `glRasterPos3f`, `glColor3f`, etc.) → steps 1–8 in order.
> - **REPL primitive** that compiles down to a custom helper at export time
>   (`label` is the only example today) → steps 1–4, 5 (with extra emphasis on
>   semantic parity), 6, 7, 8. Step 7 must include a hand-written export
>   helper because the line is not a real GL symbol.
> - **Math / expression function** (`rand`, `rand2`, `sin`, etc.) — these are
>   evaluated inline by `src/repl/eval.c`, never become a `CmdType`, and skip
>   steps 1, 2bc, 3, 4. They still need step 2a (autocomplete + F1 help) and
>   step 7 (export round-trip helper if non-trivial). See **Step 0b** below.

### 0a. Update CLAUDE.md's `## Supported Commands` block

The user-facing language reference at the bottom of `CLAUDE.md` is the
authoritative list of REPL-recognised commands. Add the new signature there
in the same style as the surrounding entries. Out-of-sync CLAUDE.md is a
common review-time finding.

### 0b. Math / expression functions take a different path

Functions evaluated inside expressions (e.g. `rand2(seed, iter)` inside
`glVertex3f(rand2(t, 0), …)`) do **not** become a `CmdType` and do **not**
go through `src/repl/executor.c`. They live entirely inside `src/repl/eval.c`:

1. Add the name to `k_reserved_idents[]` so the user can't shadow it with a
   `float` declaration.
2. Add a dispatch arm in `eval_primary` (string-compare on the function
   name) calling your evaluator helper.
3. Add an entry to the REPL→C identifier map (`{ "rand2", "repl_rand2f", 1 }`)
   and the inverse C→REPL map. The exporter uses these to translate
   call-site syntax in both directions.
4. Step 2a still applies — add a `k_func_completions[]` entry with
   `REPL_HELP_GROUP_MATH` so it shows up in F1 help and autocomplete.
5. Step 7 still applies — emit a standalone helper function from
   `src/repl/export.c` (gated on a `needs_*` flag detected via
   `export_text_uses_token("rand2(", …)`) so the exported file compiles
   without dragging the whole REPL runtime.

After step 0b, skip to step 2a, then jump to step 7. Steps 1, 2bc, 3, 4, 5,
and 6 do not apply to math functions.

### 1. `src/repl/command.h` — declare the type

Add a new `CmdType` enum entry in the `CMD_*` block, adjacent to related
commands. The enum drives switch dispatch everywhere. (`CmdType` lives in
`src/repl/command.h`; `sample.h` only re-exports it transitively via
`#include "repl/command.h"`.)

```c
CMD_GLUT_CUBE, CMD_GLUT_SPHERE, CMD_GLUT_TEAPOT, CMD_GLUT_CONE,
```

### 2. `src/repl/command_spec.c` — three additions

> **Required, not optional.** All three sub-tables feed different consumers.
> Without 2a the command is invisible in F1 help and Tab-completion; without
> 2b the parser has nothing to match against; without 2c the code-panel
> highlight color and indentation are wrong.

**a. `k_func_completions[]`** — autocomplete prefix/hint entry **and** the
F1 help row. **This is the single source of truth for both surfaces** —
`repl_autocomplete.c` and `src/repl/help_text.c` both read this table. If the
new command isn't here, F1 will silently omit it and Tab won't complete
it, even if everything else works. The recent `rand2` / `glRasterPos3f` /
`label` commits all skipped this step and shipped half-visible features.

The prefix string (including the opening `(`) must match exactly what the
user types. The hint string is displayed inline; param names drive
Tab-cycle hints. The trailing two fields drive the help overlay:
`help_desc` is the right-column description (empty string `""` to render
the signature row alone, `NULL` to skip the entry from help entirely —
used for language-level entries like `func0() {` or `x =`), and
`help_group` (`REPL_HELP_GROUP_TOP` / `LIGHTING` / `GLUT_SHAPES` /
`GLU_TESS` / `MATH` / `NONE`) selects the section header. Multi-line help
descriptions use embedded `\n`; the renderer emits each segment as an
indented continuation row.

```c
{ "glutSolidCube(", "glutSolidCube(size)", 1, { "size" },
    "", REPL_HELP_GROUP_GLUT_SHAPES },
{ "rand2(",          "rand2(seed[, iter])", 2, { "seed", "iter" },
    "Deterministic pseudo-random float in [-1, 1] (signed variant of rand).",
    REPL_HELP_GROUP_MATH },
```

**b. `k_std_command_specs[]`** — parse spec used by `src/repl/parser.c` and the
autocomplete lookup. `num_args` must match the `%g` count in `fmt`. For
commands with `glEnable`/`glBlendFunc`-style enum arguments, append to
`k_enum_command_specs[]` instead and wire `enums1` / `enums2` to the
appropriate `ReplEnumEntry` tables.

```c
{ "glutSolidCube", CMD_GLUT_CUBE, 1, "glutSolidCube(%g);", "Usage: glutSolidCube(size)", 0 },
```

**c. `g_command_type_specs[]`** — formatting/indentation metadata plus the
syntax category that drives code-panel highlight color. The
`CMD_TYPE_SPEC(type, needs_semicolon, needs_block_indent, category)`
macro is keyed on the enum, so order is validated at compile time. Pick
the matching `CMD_CAT_*` from `src/repl/command_spec.h` (e.g.
`CMD_CAT_GLUT_SHAPE` for solid shapes, `CMD_CAT_VERTEX` for vertices,
`CMD_CAT_STATE` for `glEnable`-shaped state). Nearly all geometry
commands use `(1, 1, ...)` — needs semicolon, needs block indent.

```c
CMD_TYPE_SPEC(CMD_GLUT_CUBE, 1, 1, CMD_CAT_GLUT_SHAPE),
```

### 3. `src/repl/executor.c` — execute the command

Add a `case` block after the nearest related command. Call the GL/GLU/GLUT
function, casting `flat_cmds[pc].args[N]` to the correct C type (`(double)`,
`(int)`, etc.). Always close an open `glBegin` block first for shape commands.
If the command emits geometry that should be skipped during replay's
"already-rendered prefix" pass, also list the new `CMD_*` in the
`REPLAY_MODE_VERTEX` skip switch near the top of `execute_commands`
(alongside `CMD_VERTEX3F` / `CMD_GLUT_CUBE` / etc.).

```c
case CMD_GLUT_CUBE:
    if (in_begin) { glEnd(); in_begin = 0; }
    glutSolidCube((double)flat_cmds[pc].args[0]);
    break;
```

### 4. `src/repl/replay_annotations.c` — replay display format

Add a `case` that sets `*nargs_out` and returns a `printf`-style format string
for the replay annotation overlay.

```c
case CMD_GLUT_CUBE: *nargs_out = 1; return "glutSolidCube(%g);";
```

### 5. F1 help text — already wired (if step 2a is done)

Help is generated from `k_func_completions[]` (step 2a). The `help_desc`
+ `help_group` fields you set there feed the F1 overlay's Commands tab
automatically — `src/repl/help_text.c` walks the spec table, groups by
section, and emits one row per command. **Step 5 is a no-op only if step
2a is filled in correctly.** If F1 doesn't show the new command, you
forgot 2a; if it shows the signature with no description, your
`help_desc` is `NULL` instead of `""`; if it lands in the wrong section
header, your `help_group` is wrong.

A new help group (beyond `TOP` / `LIGHTING` / `GLUT_SHAPES` / `GLU_TESS`
/ `MATH` / `NONE`) requires:
- a new enum value in `ReplHelpGroup` in `src/repl/command_spec.h`
- a `help_group_header` case in `src/repl/help_text.c`

The hand-written language-level sections in `src/repl/help_text.c`
(`Math Expressions`, `Variables`, `For-Loops`, `Functions`, etc.)
remain manual since they document REPL syntax, not commands.

### 6. Stubs (only if adding a symbol not yet in the stub headers)

If the GL/GLU/GLUT function is new to the stub build:

**`tests/gl-stubs/include/GL/gl_stub_counts.h`** — append to `GL_STUB_COUNTER_LIST`:

```c
X(glutSolidTeapot)  \
X(glutSolidCone)
```

**`tests/gl-stubs/include/GL/freeglut.h`** (or `glu.h`) — add a no-op inline stub:

```c
static inline void glutSolidTeapot(double size) {
    gl_stub_tick(GL_STUB_glutSolidTeapot); (void)size;
}
```

Keep stubs minimal: model the signature, call `gl_stub_tick`, suppress
unused-parameter warnings with `(void)`, no real rendering.

### 7. Save/load round-trip — verify byte-for-byte and behavior parity

Most commands round-trip automatically: `src/repl/export.c` writes the
editor-buffer line text (`editor_buffer_view_line(view, cmd_idx)` —
`GLCmd.source[]` was removed in the editor-owns-text refactor) verbatim
into the exported `display()` body, and `repl_export_load_from_file`
feeds those lines back through the commit pipeline. You only need to
touch `src/repl/export.c` for commands with non-source-text encoding —
declarations (`@declare`), tess blocks, REPL primitives that need a
standalone helper, etc.

**Behavior parity is required, not just syntactic round-trip.** When the
exporter emits a helper function (`write_label_helper`, `write_rand_helper`,
etc.), the helper's behavior **must match the REPL executor case** to the
nearest visible bit. Examples of the kind of divergence that has shipped
and had to be patched:

- `label("%f", x)` rendering `1.000000` in exported output but `1` in the
  REPL because the REPL's CMD_LABEL case substitutes `%f` with `%g`
  formatting while the exported helper uses real `vsnprintf("%f", …)`.
  Fix: either match formatting in the helper, or change REPL semantics —
  but they must agree.
- A REPL primitive whose live executor relies on the per-frame state
  reset in `src/scene/render.c` (e.g. `glDisable(GL_LIGHTING)` baseline,
  default specular `{0.4,0.4,0.4,1}` and shininess `30`) but whose
  exported helper assumes the OpenGL defaults. Either replicate the
  per-frame reset in the exporter's `display()` (see
  `g_render_state_lines` and `emit_export_geometry_pass`), or make the
  REPL executor stop relying on an implicit baseline.

Add a focused round-trip case to `tests/test_repl_export_all_commands.c`
to keep coverage tight. The mega test compiles the exported `output.c`
standalone against vanilla freeglut — if your helper has wrong
assumptions about includes, missing symbols, or printf-format mismatches,
this test catches it. Adding the test is part of the step, not optional.

When emitting a custom helper (`label`, `rand2`, scratch arrays, tess):

1. Add a `needs_<name>` flag to `ExportNeeds` in `src/repl/export.c`.
2. Detect usage during the per-line scan with
   `export_text_uses_token("name(", source)`.
3. Emit the helper in the file prologue from a `write_<name>_helper`
   function, gated on the flag. Scope helper-only `#include`s
   (`<stdarg.h>`, `<stdio.h>`, etc.) to that helper section so non-using
   exports stay byte-identical to the pre-helper baseline.
4. Hook the helper section into the `g_export_scaffold_sections[]`
   table with an `enabled` predicate that reads the flag.

### 8. Verify

```bash
make sample           # must be clean (no new warnings)
make test-stubs       # all tests must pass
make sample USE_GL_STUBS=1   # verify stub build still links if step 6 changed

# Spot-check the new command end-to-end:
# - F1 overlay shows it with description in the expected group
# - Type the prefix; Tab fills the rest and the parameter hint shows
# - Replay (Ctrl+G) shows the command annotated correctly
# - Save (Ctrl+S) → reload (./sample output.c) → command appears identical
# - For commands with a custom export helper: gcc -c output.c against
#   vanilla freeglut succeeds, and on-screen output matches the REPL
```

## Open Refactor Edges

Completed (Phase 1 + most of Phase 2):

- ✅ Controller extraction, explicit `SceneRenderConfig` handoff,
  focus/guide snapshot construction, scene-local accumulation jitter, and
  app-shell shim removal (`sample.c` calls `glr_ctrl_*` directly).
- ✅ **R1** — Replay/HUD migration: controller builds `ReplayFadePlan`; scene
  iterates it; 2D HUD lives in `src/ui/replay_hud.c`. Scene files contain zero
  `repl_replay_*` and `repl_state_*` calls.
- ✅ **R2** — UI → REPL mutation holes closed end-to-end:
  - `src/ui/panels.c` is hit-test only (`check-ui-panels-no-mutators`).
  - The color picker now lives across `src/widgets/color_picker_state.c` (peer state +
    lifecycle + writeback through `editor_commit_apply_external_change`)
    and `src/ui/color_picker.c` (pure renderer + hit-test over a
    `ColorPickerView`); the picker UI carries no live state reads, no
    parser/compile/apply, no `set_status`. Locked in by
    `check-color-picker-ui-isolation`.
  - The legacy `ui_help_overlay` is gone — split into the generic
    `src/ui/tabbed_overlay.c` renderer (knows nothing about REPL), the
    REPL-side `src/repl/help_text.c` producer that walks
    `k_func_completions[]` to assemble the F1 overlay's per-command
    rows, and the `glr_ctrl` adapter that maps that neutral data to
    `UiOverlayContent`. Adding a new GL command + `help_desc` + `help_group` to the
    spec entry now auto-populates F1.
  - All `ui_*.c` files have zero `_mut()` calls
    (`check-ui-returns-hits-only` baseline 0/0).
- ✅ **R3** — `repl_layout.c` / `repl_layout.h` own `ui_layout_scene_rect` /
  `ui_layout_code_panel_rect`; non-UI callers include `repl_layout.h`.
- ✅ **R4** — `src/app/glr_ctrl.c` no longer includes `src/repl/core_internal.h`;
  `src/repl/pipeline.h` exists; `repl_eval_predef_view()` hides
  `g_predef_vars`. R4d (public-API audit) landed; only `bench_repl.c`
  retains a `src/repl/core_internal.h` include outside the test/REPL set.
- ✅ **R5** — `SceneRenderConfig` slimmed and reorganized into labeled
  sections; HUD fields moved to `UiReplayHudState`; `ReplayFadePlan` and
  accum-AA fields landed.
- ✅ **R6** — `src/repl/state.h` split into `src/repl/state_views.h` (read-only) and
  `src/repl/state_owners.h` (mutating); scene/UI files include only the views
  header; `src/repl/state.h` is a compatibility shim.
- ✅ **R7** — `check-pure-scene-no-repl-state`, `check-views-no-owners`,
  `check-ui-no-repl-state-mut`, and the `check-state-ownership` umbrella
  are wired into `make test`.

Still open:

- ⚠️ **R10-phase1** — Phase J1 obsoleted the original framing of this
  task: `repl_editor.c/h` is deleted. The remaining GLUT-flavored
  declarations in `src/repl/core.h` should be reviewed against the actual
  callers in `src/app/glr_ctrl.c` and `editor_input.c`; anything that's
  no longer reachable can be removed, and anything still in use can
  move to a more specific home (`editor_input.h` for editor input,
  `src/app/glr_ctrl.h` for controller routing).
- ❌ **R10-phase2..phase5** — Dissolve `src/repl/core.c` (~663 lines): move
  `repl_parse_and_normalize*` / `normalize_with_indent` /
  `parse_and_normalize_impl` to `src/repl/parser.c`; move `collect_visible_vars`
  to `src/repl/source_scope.c`; extract `repl_reformat.c`; move
  `load_initial_commands` / `scroll_to_display_function` to `src/repl/scenes.c`;
  move `current_begin_mode` / `count_vertices` to `src/repl/executor.c`; move
  debug dumps to `src/repl/state.c` or `repl_debug.c`.
- ❌ **R11 (tail)** — Shrink the surviving allowlists, mainly the
  `bench_repl.c` exception for `src/repl/core_internal.h`.
- ❌ **R12** — Consolidate truly public REPL APIs into one concise public
  header, grouped by implementation owner; keep internals out.
- ❌ **R8** — Rename `sample.c` / `sample.h` into the `glr_*` shell
  namespace (mechanical; last). The exact target name (`glr.c/h`,
  `glr_shell.c/h`, etc.) is open.
- ❌ **R9** — Optional: split `src/repl/export.c`.

The original parallel state-ownership track shipped between 2026-04
and 2026-05: by-value read getters, controller-actualized
`UiCodePanelOutput` (cursor pixel), per-frame `UiRenderSnapshot`
consumption by every `ui_*_render*()` entry point. The remaining
narrow items — possible rename of `state_views.h` / `state_owners.h`,
domain-helper audit, and explicit docs for the three capture/restore
boundaries (REPL document / editor session / undo ring) — live in
[`feature/state-ownership-finalize.md`](feature/state-ownership-finalize.md).
The original Stage 6 (rebuild `repl_undo` on `repl_state_capture`)
was abandoned: undo deliberately doesn't snapshot input/clipboard
state (see `done/editor-input-selection.md` Phase A item 6).

The `feature/editor-owns-text.md` track (Steps 2–6) is complete:

* `GLCmd.source[]` removed; per-line text owned by `ReplEditorBuffer`.
* Parser returns `ReplParsedLine`; commit-store APIs are text-aware.
* Text sidecars added to undo snapshots, user scenes, and clipboard.
* Color picker rebuilt as a controller-pushed `EditorTransformer` with
  store `replace_one(... line)` writeback (no more `set_color` API).
* Cross-line `EditorHighlight` snapshot replaces inline `repl_find_feeding_*`
  calls in render.
* Replay annotations move from inline row injection to controller-pushed
  `EditorVirtualLine` rows; layout / scroll / hit-test / render share one
  source-of-truth count.

The deferred sub-task is the color-scheme + syntax-keyword extraction
(also Step 6); revisit when a configurable theme has a real consumer.

## Known REPL Corner Cases & Coverage Gaps

The REPL pipeline has a handful of corner cases that deserve focused
regression tests. Each item below points at the load-bearing code so future
work can either close the gap or document the intentional behaviour.

### Documented but uncovered

- **Func alias slot exhaustion.** `try_commit_func_def` (in
  `src/editor/commit.c`) calls `repl_func_alias_first_free_slot()`; when all
  10 slots are taken it returns the diagnostic
  `"no free function slots (max %d)"`. No test fires this path — adding the
  10 distinct user-named func defs and asserting the 11th is rejected with
  this status would close the gap.
- **Func alias name collision.** `repl_func_alias_set` rejects assigning the
  same alias to two different slots (`existing >= 0 && existing != slot`).
  The path is exercised indirectly through workspace round-trips, but no
  focused test asserts the `"name '%s' already used"` diagnostic for the
  collision.
- **`label()` format-string boundaries.** `repl_label_split_args`
  (`src/repl/parser.c`) hard-rejects `(`, `)`, `\`, `,`, and `//` in the format
  string, plus formats longer than `GLUT_BITMAP_FMT_MAX - 1` characters and
  formats whose `%f` count diverges from the supplied substitution-arg count.
  `tests/test_repl_core_parse.c` covers `//`, `,`, `\`, missing close quote,
  arg-count mismatch, `%d` rejection, and >4 sub args; the `(`/`)` rejection
  and the 64-char length boundary are not tested.
- **Visit-budget vs depth-limit guards.** `src/repl/flatten.c` enforces
  `MAX_FLATTEN_CALL_DEPTH = 64` and `MAX_FLATTEN_VISIT_BUDGET = 200000`
  independently. The "runaway recursion" assertion in
  `tests/test_repl_core_commit.c` accepts either `"depth limit"` or
  `"visit budget"` in the status string, so a regression that loses one
  guard without the other would still pass. A non-recursive but heavily
  unrolled `for(i, 0, 1000000)` body would specifically hit the visit
  budget; a single deeply nested mutual-recursion would specifically hit the
  depth limit.

### Known TODO with no regression test yet

- **SET_VALUE drop on decl-row overwrite (different name).**
  `repl_compile_var_assign` in `src/repl/compile.c` documents (line ~889) that
  when an assignment overwrites a `CMD_VAR_DECLARE` whose dropped names
  include a name *other* than the assigned identifier, the salvage block
  reorders the `predef_op` list in a way that turns the SET_VALUE into a
  duplicate UNDECLARE. The behaviour is benign because `repl_apply_predef_ops`
  is idempotent, but the SET_VALUE for the assigned name is silently
  dropped on this path. The TODO calls for a focused test before fixing.

### Resolved — keep tests around

- Float-decl overwrite cascade (`tests/test_repl_editor.c`'s
  `overwrite shared` / `expand decl` cases).
- Predef-table full (`MAX_PREDEF_VARS`) — same file.
- LRU eviction when every non-home slot is occupied
  (`tests/test_repl_core_extra.c::test_user_scene_promote_*`).
- Func alias roundtrip and `if`/`for`/`goto` not hijacked
  (`tests/test_repl_core_io.c`).
- Replay state machine + fade batches (`tests/test_repl_replay.c`).

## Building Historical Checkouts

This repo was hoisted out of OpenGL-Vibe in April 2026. Pre-hoist
Makefiles resolved `REPO_INCLUDE := $(abspath ../../..)/include` and
expected to find OpenGL-Vibe's project-wide `gl_includes.h` and
`miniaudio.h` there. Modern HEAD vendors slim copies under `include/`
and reroutes the Makefile through `-Iinclude`, but historical SHAs
still encode the old layout, so `git checkout <old-sha> && make`
fails out of the box.

Use the compat shim. Two modes:

```sh
# Worktree mode — recommended. Run from a modern checkout (where the
# script and compat/ exist), pass the old SHA via --at, and the
# script handles the checkout for you in a private git worktree under
# .compat-scratch/worktrees/<sha>/. Your main checkout is untouched.
./scripts/build-historical.sh --at <old-sha> sample
./scripts/build-historical.sh --at <old-sha> test USE_GL_STUBS=1
./scripts/build-historical.sh --at <old-sha> --clean sample   # wipe worktree first

# In-place mode — only useful if you've already checked out the old SHA
# yourself, or are streaming the script from main (since the script is
# not tracked at older SHAs):
git checkout <old-sha>
git show main:scripts/build-historical.sh | sh -s -- sample
```

The script reads compat headers internally with
`git show main:compat/legacy-include/...`, so `compat/` doesn't need to
exist on disk in the old checkout — only in `main`'s tree. Run
`./scripts/build-historical.sh --help` for the full inline reference.

How it works:

1. Reads two compat headers from a configurable ref (`COMPAT_REF`,
   default `main`):
   - `compat/legacy-include/gl_includes.h` — the **fat** compat header.
     Older export templates relied on `gl_includes.h` transitively
     pulling in `<stdlib.h>`, `<stdio.h>`, `<string.h>`, `<math.h>` via
     OpenGL-Vibe's bundled utilities, so this one re-includes them
     directly.
   - `compat/legacy-include/miniaudio.h` (if present), else
     `include/miniaudio.h`.
2. Materialises both into `./.compat-scratch/include/` (untracked;
   already in HEAD's `.gitignore`).
3. Invokes `make` with `PROJECT_ROOT` and `REPO_INCLUDE` overridden to
   point at the scratch dir. Extra args after the script are forwarded
   verbatim to `make`.

Run `./scripts/build-historical.sh --help` for the full inline reference
including environment variables, examples, and known limitations (most
notably: the very first commit — the
`displaylist-dynamic-rendering → immediate-mode-repl` rename — uses
quoted `#include "gl_includes.h"` and won't be repaired by the shim).

The shim only fixes header layout. Other pre-existing breakage at
specific older SHAs (renamed symbols, broken examples) is intentionally
left alone — old SHAs are reference material, not a maintained build
target.

## Header Documentation Standard

Until R12 consolidates the REPL public surface, each public API header should
document:

1. Module responsibility and ownership boundary.
2. Lifecycle: initialization, per-frame calls, mutation rules.
3. Public types and what layer owns them.
4. Public functions, parameters, return values, and preconditions.
5. Important cross-module invariants, especially GL/state ownership and render
   ordering.

Long-form implementation notes belong in the implementation section or module
docs. The Phase 2 end state is one concise public REPL API header; verbose
per-module header prose should not become the permanent public surface.
