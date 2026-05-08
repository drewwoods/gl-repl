# REPL Architecture

> For the quick module map, see [`MODULES.md`](MODULES.md). For the staged
> controller extraction plan, see
> [`feature/push-architecture-refinement.md`](feature/push-architecture-refinement.md).

## Direction

This document follows the controller-first direction from
`feature/push-architecture-refinement.md`.

The older "generic scene callback plus `ReplGeometryRenderPlan`" direction is
superseded. This is a one-frontend REPL sample, so the useful boundary is
between the REPL model/controller and the rendering views. The goal is not to
turn `scene_*` into a plugin host.

Current code already routes frame wiring through `imrepl_ctrl.c`. `repl_core.c`
now keeps the REPL model/pipeline wrappers, while `src/scene/render.c` consumes
explicit per-frame config. Phase 2 is still in progress; remaining work is
mostly about shrinking transitional state/config surfaces and removing
allowlisted view-layer state mutations.

## Ownership Model

```text
repl_*        = language, source model, flat program, replay model, input/model controllers
imrepl_ctrl   = app-frame controller between REPL state and scene/UI rendering
scene_*       = 3D stage: camera, projection, frame setup, decorators, 3D overlays
ui_*          = 2D editor chrome: code panel, menus, overlays, popups, HUDs
sample.c/h    = current GLUT app shell and legacy shared header
imrepl.c/h    = future app shell/shared header name, replacing sample.c/h
```

The prefix is an ownership signal, not a generic sample prefix. New `repl_*`
modules should own REPL language, editor, source, workspace, replay, or command
model behavior. App-shell services belong under `imrepl_*` after the deferred
rename, and generic infrastructure should keep neutral names such as `prof`.
`repl_audio` is a legacy name to revisit during the namespace audit rather
than a precedent for unrelated app services.

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
2. Add a named runtime slice in `repl_state.h`, wire it into
   `static ReplRuntimeState g_repl_state;`, and say whether the read path is
   currently `facade-backed`, `direct-runtime`, or `value-getter`.
3. Keep mutations on the owner side. Scene/UI renderers read snapshots only;
   render-time discoveries return through output structs that the controller
   actualizes back into state.
4. Extend the ownership tests in the same change: keep
   `repl_state_capture()`, `repl_state_restore()`, and `repl_state_reset_all()`
   current for runtime slices, and add focused behavior coverage in the
   module's own tests.

## Core Tenets

1. **The REPL owns the user program.** It parses source, stores source commands,
   flattens loops/functions/conditionals, owns predefined variables, and owns
   replay policy.
2. **The executor is the narrow live-GL gate for user geometry.**
   `repl_executor.c` turns a flat program into OpenGL calls. General `repl_*`
   modules should not casually call OpenGL.
3. **The scene owns the stage, not the editor.** It sets viewport, clear,
   projection, camera, accumulation, baseline lighting, grid, axes, backdrop,
   light indicators, orbit target, and 3D overlay passes from config.
4. **The UI owns screen-space presentation.** UI renderers draw code rows,
   menus, popups, color picker, help, status, and profile views from snapshots
   and route mutations through REPL-owned actions or stores.
5. **The controller is the mixed layer.** The frame controller builds scene and
   UI inputs from REPL state, calls the scene renderer, then calls UI renderers.
   This role belongs in `imrepl_ctrl.c`.
6. **Replay is REPL policy.** Replay state machine, PC, mode, baseline values,
   and fade/highlight decisions belong in `repl_replay.c` or follow-up replay
   planning code. Any scene use of replay data should be via snapshots or
   documented transitional helpers.

## Target Frame Pipeline

Top-level frame orchestration belongs in the controller:

```text
sample.c GLUT display callback (future: imrepl.c)
  -> imrepl_ctrl_display_frame          (sample.c calls controller directly; no shim)
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
| `EditorTransformerList editor_transformers` | `imrepl_ctrl_push_color_transformers()` | One entry per editable color command (line idx + r/g/b/a/has_alpha/is_clear). Drives inline swatch render and color-picker hit-test. Future kinds: numeric slider. |
| `EditorHighlightList editor_highlights` | `imrepl_ctrl_push_highlights()` | Feeding-normal cmd, feeding-color cmd, replay PC, search match, selection. Rendered as gutter accents and row backgrounds. |
| `EditorVirtualLineList editor_virtual_lines` | `repl_replay_annotations_prepare()` (via `_refresh_virtual_lines()`) | Replay-time annotation rows (substitution + evaluation) attached to the current source line. Layout, scroll, hit-test, and render all read from this list, so virtual-row counts have one source of truth (`repl_replay_annotation_extra_rows_for_line()` counts the list). |

All three lists are stored on `ReplRuntimeState` as named slices and
exposed via `repl_state_editor_*()` accessors (read-only view in
`repl_state_views.h`, mutating clear/append in `repl_state_owners.h`).
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
| GLUT input dispatch (cross-subsystem routing) | `imrepl_ctrl.c` |
| Editor text-document input + commit orchestration | `editor_input.c` + `editor_commit.c` |
| Parsing | `repl_parser.c` |
| Validation / compilation (pure, returns `ReplCompiledChange`) | `repl_compile.c` |
| Apply (writes `ReplState` only) | `repl_apply.c` |
| Source command mutation (low-level shifts) | `repl_command_store.c` |
| Source scope/depth | `repl_source_scope.c` |
| Flattening | `repl_flatten.c` |
| User geometry execution | `repl_executor.c` |
| Export/import | `repl_export.c` |

Note: `repl_editor.{c,h}` and `repl_commit.{c,h}` are deleted (Phase J1
+ Phase H.5). Their responsibilities split into the entries above.
`check-no-repl-editor-input-shim` and `check-no-repl-commit` hard-guard
against either filename returning.

Outside code that needs to inject commands should use the public command/input
paths instead of directly mutating command arrays.

## Controller Layer

The controller layer is the home for app-frame wiring that used to live in
`repl_core.c`.

Responsibilities:

* rebuild flat program and autonormals when dirty
* prepare replay frame clamps and restore state after rendering
* build `SceneRenderConfig` and any guide/focus snapshots from REPL state
* call `scene_render_3d_scene(&config)`
* call UI renderers in the correct order
* keep profiling section boundaries around scene and UI rendering

`imrepl_ctrl.c` may include both REPL headers and scene/UI headers. Ordinary REPL
model modules should not.

`sample.c` and `sample.h` still carry the app entry point and shared legacy
types/constants. Renaming them to `imrepl.c` and `imrepl.h` is intentionally a
separate mechanical cleanup after controller extraction, because `sample.h` is
included broadly.

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
`imrepl_ctrl_build_ui_snapshot()` and passes to every `ui_*_render*()`
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

Mutations route through `repl_actions`, `repl_command_store`,
`variable_panel_drag`, or another REPL-owned mutation path. UI input
hit-tests (`*_hit_test`, `*_rect`) compute neutral `UiHit` values and
return — `imrepl_ctrl_router_handle_code_panel_hit` dispatches by
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

R1 target in `feature/push-architecture-refinement.md`:

* controller builds a `ReplayFadePlan` snapshot once per frame (batches,
  alpha, skip limits, baseline predef values)
* scene iterates the snapshot and owns the GL pass orchestration without
  calling `repl_replay_*` or `repl_state_*`
* accumulation-AA settings are `SceneRenderConfig` fields set by the controller
* 2D replay HUD lives in `replay_ui_hud.c`, driven by config fields
* `scene_*.c` files contain no `repl_state_*` or `repl_replay_*` calls; once
  the relevant Phase 2 slice is complete, Makefile checks keep that true

## Boundary Rules

### Live OpenGL / GLU calls

Allowed:

```text
scene_*.c
ui_*.c
repl_executor.c
sample.c        GLUT/window lifecycle and buffer swap; future `imrepl.c`
```

Avoid live GL calls in all other `repl_*` files. Text emission of GL command
names in parser/export/example/spec code is not a live GL call.

### GLUT calls

Allowed:

```text
sample.c        GLUT callback registration, glutInit, buffer swap
imrepl.c        after the sample.c rename lands
imrepl_ctrl.c   GLUT modifier reads + cross-layer input routing
                (took over from the deleted repl_editor.c in Phase J1)
editor_input.c  glutGetModifiers via editor_get_modifiers (gated behind
                editor_input_enable_glut_modifier_reads so tests stay safe)
repl_executor.c tessellator callback setup only
```

### Controller-only scene wiring

After controller extraction, ordinary `repl_*` model files should not include
`scene_*.h`. `imrepl_ctrl.c` is the scene/UI frame-rendering exception.
`check-controller-boundaries` enforces this; cross-layer constants used by
both layers (e.g. `CFG_DEFAULT_MULTISAMPLE`, `REPL_OUTLINE_POLYGON_OFFSET_*`)
live in neutral headers (`repl_presentation.h`, `src/scene/render_types.h`)
that both sides include via existing transitive paths.

Remaining `ui_*` include exceptions: `repl_actions.c` and `repl_export.c`.
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
`UiOverlayContent` produced by `repl_help_text`).

`ui_*.c` files include `repl_state_views.h` only, not `repl_state.h`
or `repl_state_owners.h`. `check-views-no-owners` enforces this;
`check-ui-returns-hits-only` (baseline 0/0) keeps any new mutator
out of the input + render paths;
`check-color-picker-ui-isolation` and `check-replay-ui-isolation`
audit the feature-UI prefixes.

### UI / scene independence

`ui_*` and `scene_*` are sibling view layers. They should not include each
other's headers. Shared render-neutral helpers belong in local shared headers
or project-wide `include/` only when broadly reusable.

## Where To Put New Code

* New REPL syntax: `repl_parser.c`, `repl_command_spec.c`, `repl_commit.c`,
  `repl_flatten.c`, and `repl_executor.c` as needed.
* New user-geometry execution behavior: `repl_executor.c`.
* New 3D world decorator: `scene_*`.
* New 3D REPL-aware overlay: current home is still `scene_*`, consuming
  `FlatProgramView` or a snapshot from `SceneRenderConfig`.
* New 2D UI: `ui_*` renderer plus `repl_*` model/action code if mutation is
  required.
* New per-frame scene/UI wiring: `imrepl_ctrl.c`.
* New app lifecycle/window wiring: `sample.c` for now, `imrepl.c` after the
  deferred rename.
* New command mutation: `repl_command_store_*`.

## Adding A New Command

This is the canonical checklist for adding a new GL/GLU/GLUT command to the
REPL. Every bullet is required unless the note says otherwise. The GLUT solid
shapes (`glutSolidCube`, `glutSolidSphere`, `glutSolidTeapot`, `glutSolidCone`)
are a recent worked example.

### 1. `repl_command.h` — declare the type

Add a new `CmdType` enum entry in the `CMD_*` block, adjacent to related
commands. The enum drives switch dispatch everywhere. (`CmdType` lives in
`repl_command.h`; `sample.h` only re-exports it transitively via
`#include "repl_command.h"`.)

```c
CMD_GLUT_CUBE, CMD_GLUT_SPHERE, CMD_GLUT_TEAPOT, CMD_GLUT_CONE,
```

### 2. `repl_command_spec.c` — three additions

**a. `k_func_completions[]`** — autocomplete prefix/hint entry **and** the
F1 help row. The prefix string (including the opening `(`) must match
exactly what the user types. The hint string is displayed inline; param
names drive Tab-cycle hints. The trailing two fields drive the help
overlay: `help_desc` is the right-column description (empty string ""
to render the signature row alone, `NULL` to skip the entry from help
entirely — used for language-level entries like `func0() {` or `x =`),
and `help_group` (`REPL_HELP_GROUP_TOP` / `LIGHTING` / `GLUT_SHAPES` /
`GLU_TESS` / `NONE`) selects the section header. Multi-line help
descriptions use embedded `\n`; the renderer emits each segment as an
indented continuation row.

```c
{ "glutSolidCube(", "glutSolidCube(size)", 1, { "size" },
    "", REPL_HELP_GROUP_GLUT_SHAPES },
```

**b. `k_std_command_specs[]`** — parse spec used by `repl_parser.c` and the
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
the matching `CMD_CAT_*` from `repl_command_spec.h` (e.g.
`CMD_CAT_GLUT_SHAPE` for solid shapes, `CMD_CAT_VERTEX` for vertices,
`CMD_CAT_STATE` for `glEnable`-shaped state). Nearly all geometry
commands use `(1, 1, ...)` — needs semicolon, needs block indent.

```c
CMD_TYPE_SPEC(CMD_GLUT_CUBE, 1, 1, CMD_CAT_GLUT_SHAPE),
```

### 3. `repl_executor.c` — execute the command

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

### 4. `repl_replay_annotations.c` — replay display format

Add a `case` that sets `*nargs_out` and returns a `printf`-style format string
for the replay annotation overlay.

```c
case CMD_GLUT_CUBE: *nargs_out = 1; return "glutSolidCube(%g);";
```

### 5. F1 help text — already wired

Help is generated from `k_func_completions[]` (step 2a). The
`help_desc` + `help_group` fields you set there feed the F1 overlay's
Commands tab automatically — `repl_help_text.c` walks the spec table,
groups by section, and emits one row per command. No separate edit
needed unless your command lives in a *new* group (in which case add
both an enum value to `ReplHelpGroup` in `repl_command_spec.h` and a
`help_group_header` case in `repl_help_text.c`).

The hand-written language-level sections in `repl_help_text.c`
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

### 7. Save/load round-trip

Most commands round-trip automatically: `repl_export.c` writes
`GLCmd.source[]` verbatim into the exported `display()` body, and
`repl_export_load_from_file` feeds those lines back through the commit
pipeline. You only need to touch `repl_export.c` for commands with
non-source-text encoding — declarations (`@declare`), tess blocks, etc.
Add a focused round-trip case to `tests/test_repl_export_all_commands.c`
to keep coverage tight.

### Verify

```bash
make sample          # must be clean (no new warnings)
make test-stubs      # all tests must pass
```

## Open Refactor Edges

Completed (Phase 1 + most of Phase 2):

- ✅ Controller extraction, explicit `SceneRenderConfig` handoff,
  focus/guide snapshot construction, scene-local accumulation jitter, and
  app-shell shim removal (`sample.c` calls `imrepl_ctrl_*` directly).
- ✅ **R1** — Replay/HUD migration: controller builds `ReplayFadePlan`; scene
  iterates it; 2D HUD lives in `replay_ui_hud.c`. Scene files contain zero
  `repl_replay_*` and `repl_state_*` calls.
- ✅ **R2** — UI → REPL mutation holes closed end-to-end:
  - `src/ui/panels.c` is hit-test only (`check-ui-panels-no-mutators`).
  - The color picker now lives across `color_picker.c` (peer state +
    lifecycle + writeback through `editor_commit_apply_external_change`)
    and `color_picker_ui.c` (pure renderer + hit-test over a
    `ColorPickerView`); the picker UI carries no live state reads, no
    parser/compile/apply, no `set_status`. Locked in by
    `check-color-picker-ui-isolation`.
  - The legacy `ui_help_overlay` is gone — split into the generic
    `src/ui/tabbed_overlay.c` renderer (knows nothing about REPL) and the
    REPL-side `repl_help_text.c` producer that walks
    `k_func_completions[]` to assemble the F1 overlay's per-command
    rows. Adding a new GL command + `help_desc` + `help_group` to the
    spec entry now auto-populates F1.
  - All `ui_*.c` files have zero `_mut()` calls
    (`check-ui-returns-hits-only` baseline 0/0).
- ✅ **R3** — `repl_layout.c` / `repl_layout.h` own `ui_layout_scene_rect` /
  `ui_layout_code_panel_rect`; non-UI callers include `repl_layout.h`.
- ✅ **R4** — `imrepl_ctrl.c` no longer includes `repl_core_internal.h`;
  `repl_pipeline.h` exists; `repl_eval_predef_view()` hides
  `g_predef_vars`. R4d (public-API audit) landed; only `bench_repl.c`
  retains a `repl_core_internal.h` include outside the test/REPL set.
- ✅ **R5** — `SceneRenderConfig` slimmed and reorganized into labeled
  sections; HUD fields moved to `UiReplayHudState`; `ReplayFadePlan` and
  accum-AA fields landed.
- ✅ **R6** — `repl_state.h` split into `repl_state_views.h` (read-only) and
  `repl_state_owners.h` (mutating); scene/UI files include only the views
  header; `repl_state.h` is a compatibility shim.
- ✅ **R7** — `check-pure-scene-no-repl-state`, `check-views-no-owners`,
  `check-ui-no-repl-state-mut`, and the `check-state-ownership` umbrella
  are wired into `make test`.

Still open:

- ⚠️ **R10-phase1** — Phase J1 obsoleted the original framing of this
  task: `repl_editor.c/h` is deleted. The remaining GLUT-flavored
  declarations in `repl_core.h` should be reviewed against the actual
  callers in `imrepl_ctrl.c` and `editor_input.c`; anything that's
  no longer reachable can be removed, and anything still in use can
  move to a more specific home (`editor_input.h` for editor input,
  `imrepl_ctrl.h` for controller routing).
- ❌ **R10-phase2..phase5** — Dissolve `repl_core.c` (~663 lines): move
  `repl_parse_and_normalize*` / `normalize_with_indent` /
  `parse_and_normalize_impl` to `repl_parser.c`; move `collect_visible_vars`
  to `repl_source_scope.c`; extract `repl_reformat.c`; move
  `load_initial_commands` / `scroll_to_display_function` to `repl_scenes.c`;
  move `current_begin_mode` / `count_vertices` to `repl_executor.c`; move
  debug dumps to `repl_state.c` or `repl_debug.c`.
- ❌ **R11 (tail)** — Shrink the surviving allowlists, mainly the
  `bench_repl.c` exception for `repl_core_internal.h`.
- ❌ **R12** — Consolidate truly public REPL APIs into one concise public
  header, grouped by implementation owner; keep internals out.
- ❌ **R8** — Rename `sample.c` / `sample.h` to `imrepl.c` / `imrepl.h`
  (mechanical; last).
- ❌ **R9** — Optional: split `repl_export.c`.

A parallel state-ownership track lives in
`feature/gold-standard-state-ownership.md`. Stage 0/1 are complete; Stage 2
(by-value getters) is broadly applied with a few view-struct slices
remaining; Stage 7 (UI snapshot purity) is done at the render boundary —
every `ui_*_render*()` entry point consumes `const UiRenderSnapshot *snap`
built once per frame by `imrepl_ctrl_build_ui_snapshot()` (Phase B in
`feature/done/push-architecture-ui.md`); input-bridge helpers in `ui_*.c`
remain on live state pending Phase C. Stages 4 (cursor-pixel `Ui*Output`
actualization) and 6 (`repl_undo.c` consumes `repl_state_capture()`) are
still open.

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
