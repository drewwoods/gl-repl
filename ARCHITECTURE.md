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
now keeps the REPL model/pipeline wrappers, while `scene_render.c` consumes
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
        -> rebuild flat program if dirty
        -> save live predefined variable values
        -> prepare replay frame if replay is active
        -> update export/camera strings
        -> build SceneRenderConfig from REPL state
        -> scene_render_3d_scene(&scene_cfg)
        -> render UI panels and overlays
        -> restore flat count and predefined variable values
```

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
       -> render replay fades/highlights while they remain in scene_render.c
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
| Input buffer and routing | `repl_editor.c` |
| Structured commits | `repl_commit.c` |
| Parsing | `repl_parser.c` |
| Source command mutation | `repl_command_store.c` |
| Source scope/depth | `repl_source_scope.c` |
| Flattening | `repl_flatten.c` |
| User geometry execution | `repl_executor.c` |
| Export/import | `repl_export.c` |

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

Neutral scene modules such as `scene_grid.c`, `scene_axes.c`,
`scene_backdrop.c`, and `scene_lights.c` should remain free of REPL state
access. Transitional REPL-aware scene files must consume snapshots rather than
pulling globals directly.

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
`ui_snapshot.h`) that the controller builds once via
`imrepl_ctrl_build_ui_snapshot()` and passes to every `ui_*_render*()`
entry point. Render code does not call `repl_state_*()` directly. The
`check-ui-no-repl-state-read` Makefile guard enforces the snapshot-shaped
signature for audited renderers.

Mutations route through `repl_actions`, `repl_command_store`,
`repl_var_drag`, or another REPL-owned mutation path. UI input bridges
(`*_hit`, `*_rect`, press/motion handlers) still query live state during
GLUT input dispatch; pulling those onto a deferred output channel is the
Phase C work in `feature/push-architecture-ui.md`.

## Replay Architecture

Replay is REPL-owned. The scene may render the current visual effect, but it
should not own replay policy.

R1 target in `feature/push-architecture-refinement.md`:

* controller builds a `ReplayFadePlan` snapshot once per frame (batches,
  alpha, skip limits, baseline predef values)
* scene iterates the snapshot and owns the GL pass orchestration without
  calling `repl_replay_*` or `repl_state_*`
* accumulation-AA settings are `SceneRenderConfig` fields set by the controller
* 2D replay HUD lives in `ui_replay_hud.c`, driven by config fields
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
sample.c
imrepl.c        after the sample.c rename lands
repl_editor.c   cross-layer input routing + REPL-internal dispatch (current);
                routing moves to imrepl_ctrl.c in Phase 2 (see R in refinement plan)
repl_executor.c tessellator callback setup only
```

### Controller-only scene wiring

After controller extraction, ordinary `repl_*` model files should not include
`scene_*.h`. `imrepl_ctrl.c` is the scene/UI frame-rendering exception.

Existing input/layout exceptions: `repl_editor.c`, `repl_actions.c`, and
`repl_export.c` still include selected `ui_*` headers. The `repl_editor.c`
exception is eliminated by Phase 2 input routing: the cross-layer priority
chain moves to `imrepl_ctrl.c`, after which `repl_editor.c` only needs
REPL-internal headers. The `repl_actions.c` and `repl_export.c` exceptions
require separate, independent cleanup.

### Scene state access

Target rule: `scene_*` files consume `SceneRenderConfig`, `FrameRenderContext`,
or explicit snapshot structs. They should not call `repl_state_*` directly.

R11 in the refinement plan adds `check-state-boundaries` with transitional
allowlists. The strict no-exception version belongs to the end of the relevant
Phase 2 cleanup, not to the beginning.

### UI mutation boundary

`ui_*` renderers must route all REPL mutations through `repl_actions`,
`repl_command_store`, `repl_var_drag`, or another REPL-owned mutation path.
Using `repl_state_*_mut()` accessors directly from `ui_*` files is not
permitted. Phase 2 R2 closes the known violations in `ui_color_picker.c`,
`ui_panels.c`, and `ui_help_overlay.c`.

Target state after R2 and R6: `ui_*.c` files include `repl_state_views.h`
only, not `repl_state.h` or `repl_state_owners.h`. The `check-views-no-owners`
Makefile rule enforces this mechanically.

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

### 1. `sample.h` — declare the type

Add a new `CmdType` enum entry in the `CMD_*` block, adjacent to related
commands. The enum drives switch dispatch everywhere.

```c
CMD_GLUT_CUBE, CMD_GLUT_SPHERE, CMD_GLUT_TEAPOT, CMD_GLUT_CONE,
```

### 2. `repl_command_spec.c` — three additions

**a. `k_func_completions[]`** — autocomplete prefix/hint entry. The prefix
string (including the opening `(`) must match exactly what the user types.
The hint string is displayed inline; param names drive Tab-cycle hints.

```c
{ "glutSolidCube(",  "glutSolidCube(size)",  1, { "size" } },
```

**b. `g_std_command_specs[]`** — parse spec used by `repl_parser.c` and the
autocomplete lookup. `num_args` must match the `%g` count in `fmt`.

```c
{ "glutSolidCube", CMD_GLUT_CUBE, 1, "glutSolidCube(%g);", "Usage: glutSolidCube(size)", 0 },
```

For commands with `glEnable`/`glBlendFunc`-style enum arguments, add to
`g_enum_command_specs[]` instead and wire `enums1`/`enums2` to the
appropriate `EnumEntry` tables.

**c. `g_command_type_specs[]`** — formatting/indentation metadata for the
new `CmdType`. Nearly all geometry commands use `(1, 1)` (needs semicolon,
needs block indent).

```c
CMD_TYPE_SPEC(CMD_GLUT_CUBE, 1, 1),
```

### 3. `repl_executor.c` — execute the command

Add a `case` block after the nearest related command. Call the GL/GLU/GLUT
function, casting `flat_cmds[pc].args[N]` to the correct C type (`(double)`,
`(int)`, etc.). Always close an open `glBegin` block first for shape commands.

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

### 5. `ui_help_overlay.c` — help text

Add a line to the appropriate section of `g_commands_lines[]` (F1 overlay,
Commands tab). Group with related commands under the same section header.

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

### Verify

```bash
make sample          # must be clean (no new warnings)
make test-stubs      # all tests must pass
```

For commands that affect save/load round-trips, update the matching export
and import helpers in `repl_export.c`.

## Open Refactor Edges

Completed (Phase 1 + most of Phase 2):

- ✅ Controller extraction, explicit `SceneRenderConfig` handoff,
  focus/guide snapshot construction, scene-local accumulation jitter, and
  app-shell shim removal (`sample.c` calls `imrepl_ctrl_*` directly).
- ✅ **R1** — Replay/HUD migration: controller builds `ReplayFadePlan`; scene
  iterates it; 2D HUD lives in `ui_replay_hud.c`. Scene files contain zero
  `repl_replay_*` and `repl_state_*` calls.
- ✅ **R2** — UI → REPL mutation holes closed: `ui_color_picker`, `ui_panels`,
  `ui_help_overlay` route mutations through store/action APIs. UI files have
  zero `_mut()` calls.
- ✅ **R3** — `repl_layout.c` / `repl_layout.h` own `repl_layout_scene_rect` /
  `repl_layout_code_panel_rect`; non-UI callers include `repl_layout.h`.
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

- ⚠️ **R10-phase1** — Reassess: the GLUT decls in `repl_core.h`
  (`repl_keyboard_func`, `repl_special_func`, …) are not stale — they are
  implemented in `repl_editor.c` and called from `imrepl_ctrl.c` for
  cross-layer input dispatch. Decide between leaving them in `repl_core.h`
  until R10-phase5 dissolves it or moving them to `repl_editor.h`.
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
`feature/push-architecture-ui.md`); input-bridge helpers in `ui_*.c`
remain on live state pending Phase C. Stages 4 (cursor-pixel `Ui*Output`
actualization) and 6 (`repl_undo.c` consumes `repl_state_capture()`) are
still open.

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
