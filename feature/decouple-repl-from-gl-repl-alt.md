# Decoupling `repl_demo` from `gl-repl` - Execution Plan

## Goal

`tools/repl_demo` is the REPL-pipeline counterpart to
`tools/teapot_demo`: it should prove that

```text
parse -> command store -> flatten -> execute
```

can build and run without editor input dispatch, the GLUT controller, UI
renderers, or app-owned state. The demo intentionally still links
`src/editor/state.c` because canonical source text lives in the editor buffer;
the boundary is "no editor input/controller/UI/app shell", not "no editor text
storage".

The current `tools/repl_demo/stubs.c` is therefore a useful ledger: every stub
is a hard symbol dependency from the REPL pipeline into another owner. The plan
below removes those edges while preserving the full app's behavior and save-file
compatibility.

## Baseline

Current `repl_demo` link shape:

- 30 dependency translation units from `REPL_DEMO_DEP_SRCS`
- `tools/repl_demo/repl_demo.c`
- `tools/repl_demo/stubs.c`
- 32 object files total

`tools/repl_demo/stubs.c` currently defines 17 externally visible symbols:

| Stub(s) | Root cause |
|---|---|
| `repl_compile_dispatch` | grammar dispatcher lives in editor services |
| `ui_state_reset`, `variable_panel_state_reset`, `editor_help_session_reset`, `repl_editor_reset_transients` | `repl_state_reset_all()` resets non-REPL owners |
| `ui_state_code_panel_mut` | `repl_state_sync_ui_chrome()` writes UI chrome from `repl_state.c` |
| `ui_state_status_set` | pipeline TUs call `set_status()` |
| `g_cfg_items`, `CFG_ITEM_COUNT`, `audio_get_cfg_mode`, `audio_set_cfg_mode`, `variable_panel_view_mut`, `ui_state_profile_panel_mut` | `repl_export.c` / `repl_scenes.c` use `glr_config` directly for `@cfg` |
| `feed_line` | examples/imports drive editor input dispatch |
| `load_line_to_input` | reformat/scene load restore editor input from REPL TUs |
| `ui_state_viewport`, `ui_state_code_panel` | `repl_export.c` calls `ui_layout_*` for viewport/panel geometry |

Stub trajectory:

```text
17 -> 16 -> 11 -> 10 -> 4 -> 4 -> 3 -> 2 -> 0
      S1   S2   S3   S4   S5a  S5b  S6  S7
```

## Step Summary

| Step | Change | Stubs cleared |
|---|---|---:|
| 1 | Move `repl_compile_dispatch` into REPL compile code | 1 |
| 2 | Split REPL-only reset from app reset; move UI chrome sync and autocomplete registration out of `repl_state.c` | 5 |
| 3 | Replace pipeline `set_status()` calls with diagnostics/sinks | 1 |
| 4 | Introduce opaque `ReplExportConfig` items; owners fill/apply config outside `repl_export.c` | 6 |
| 5a | Extract pure structured-block validators from editor compile wrappers | 0 |
| 5b | Add non-editor source-load/commit API; examples/imports stop calling `feed_line` | 1 |
| 6 | Move reformatter and scene cursor restore out of REPL pipeline TUs | 1 |
| 7 | Move app-state slices to `glr_state`; pass layout/export environment explicitly | 2 |

## Step 1 - Move `repl_compile_dispatch`

`repl_compile.c` is the pure validation owner, but
`repl_compile_toggle_comment()` calls `repl_compile_dispatch()`, whose body is
currently in `src/editor/services.c`. The source comment already says this is a
historical artifact.

Fix:

- Move the implementation of `repl_compile_dispatch()` into `repl_compile.c`.
- Leave `src/editor/services.c` as a thin caller of the REPL-owned dispatcher.
- Remove the demo stub.

Guard impact:

- Existing `check-no-set-status-in-compile-apply` keeps the moved code pure.

## Step 2 - Split Reset and UI Chrome Sync

### 2a. Rename and split reset

`repl_state_reset_all()` currently resets REPL state and also calls UI, peer,
and editor-input reset functions. That name should not be reused for a REPL-only
operation.

Fix:

- Rename the REPL-only function to `repl_state_reset_program()`.
- Scope it to REPL-owned slices only: document, flat program, variables, scenes,
  source-scope cache, eval storage binding, dirty flags.
- Introduce `glr_app_reset_all()` in app/controller code. It calls
  `repl_state_reset_program()` plus UI/editor/peer resets.
- Update production startup to call the app reset when it needs full-world
  behavior. Tests choose the reset matching their scope.

Do not make `repl_state_init_defaults()` call `glr_app_reset_all()`; that would
recreate the dependency in the opposite direction. `repl_state_*` APIs stay
REPL-only.

### 2b. Move autocomplete registration

`repl_autocomplete_register_provider()` calls into the editor completion
registry. Today it is invoked by REPL state initialization/reset. That does not
show up as a stub because `src/editor/completion.c` is linked by the demo, but
it is still not program-state initialization.

Fix:

- Move provider registration to app/editor startup.
- Autocomplete tests that need the provider install it explicitly.

### 2c. Move UI chrome sync

`repl_state_sync_ui_chrome()` mirrors presentation fields into `UiState`. That
is controller work.

Fix:

- Move the body to `glr_ctrl.c` or an app-side helper.
- After this move, `repl_state.c` has no `ui_state_*` references.

Stubs cleared:

`ui_state_reset`, `variable_panel_state_reset`,
`editor_help_session_reset`, `repl_editor_reset_transients`,
`ui_state_code_panel_mut`.

## Step 3 - Replace Pipeline `set_status()`

`repl_core.c` defines `set_status()` as a UI status forwarder. Linked pipeline
TUs call it from roughly 16 sites:

- `repl_core.c`: normalize parse error, startup banner
- `repl_executor.c`: goto loop limit reached
- `repl_flatten.c`: flatten error
- `repl_export.c`: export/import diagnostics
- `repl_scenes.c`: scene/workspace status messages
- `repl_example_loader.c`: example-load status message

Fix:

- `repl_execute_program()` writes diagnostics to an optional buffer/callback in
  `ReplExecutionOptions`.
- Flattening returns diagnostics in `ReplFlattenResult` / an out param instead
  of calling `set_status()`.
- Export/import APIs return diagnostics through a `ReplDiagnostic` or caller
  buffer.
- Scene and example APIs return status text to their caller; the controller
  decides whether to surface it.
- Move the startup banner to app startup.
- Delete `set_status()` from `repl_core.c` once no linked pipeline TU calls it.

Stubs cleared:

`ui_state_status_set`.

Guard work:

- Add `check-no-set-status-in-pipeline` covering `repl_core.c`,
  `repl_executor.c`, `repl_flatten.c`, `repl_export.c`, `repl_scenes.c`, and
  `repl_example_loader.c`.

## Step 4 - Make Header State Opaque to `repl_export`

This is the key ownership fix.

Generating/importing the C file is mostly REPL/file-format work, but the file
header serializes state owned by other modules. Today `repl_export.c` reaches
into `glr_config` and `GlrConfigItem` to emit/apply `@cfg` lines, and it also
knows how to apply camera header lines. That makes `repl_export.c` explicitly
aware of app-owned modules such as camera, audio, profile panel, and variable
panel.

The desired boundary is:

```text
owners -> fill opaque header data -> repl_export writes file grammar
repl_export parses file grammar -> owners apply their own header data
```

`repl_export.c` owns the line shape. It does not own header semantics.

### 4a. Introduce neutral header data

Add two neutral payloads to `repl_export.h`.

`@cfg` uses a small key/value bag:

```c
#define REPL_EXPORT_CONFIG_KEY_MAX   48
#define REPL_EXPORT_CONFIG_VALUE_MAX 48
#define REPL_EXPORT_CONFIG_MAX       64

typedef struct {
    char key[REPL_EXPORT_CONFIG_KEY_MAX];
    char value[REPL_EXPORT_CONFIG_VALUE_MAX];
} ReplExportConfigItem;

typedef struct {
    ReplExportConfigItem items[REPL_EXPORT_CONFIG_MAX];
    int count;
} ReplExportConfig;

void        repl_export_config_clear(ReplExportConfig *cfg);
int         repl_export_config_set(ReplExportConfig *cfg,
                                   const char *key,
                                   const char *value);
const char *repl_export_config_get(const ReplExportConfig *cfg,
                                   const char *key);
int         repl_export_config_at(const ReplExportConfig *cfg, int idx,
                                  const char **key_out,
                                  const char **value_out);
```

Start with key/value, not callbacks. Current `@cfg` payloads are simple scalar
slug/value pairs. A callback registry adds init-order coupling and makes tests
less inspectable. If a future config value needs structured serialization, add
an owner-side encoder that still writes into the neutral bag.

The existing `// camera` block is not a key/value pair. Keep it separate but
apply the same opacity rule:

```c
#define REPL_EXPORT_CAMERA_LINE_MAX 96
#define REPL_EXPORT_CAMERA_LINES    5

typedef struct {
    char lines[REPL_EXPORT_CAMERA_LINES][REPL_EXPORT_CAMERA_LINE_MAX];
    int present;
} ReplExportCameraBlock;
```

`repl_export.c` writes/parses the camera block text. Camera-owner code fills and
applies the block.

### 4b. Change export/import flow

Export:

```c
ReplExportConfig cfg;
ReplExportCameraBlock cam;
repl_export_config_clear(&cfg);
glr_export_config_fill(&cfg);        /* app side; not repl_export.c */
glr_camera_fill_export_block(&cam);  /* app side; not repl_export.c */
repl_export_save_output(path, editor_buffer_view(), &cfg, &cam, ...);
```

Inside `repl_export.c`:

```c
for each cfg item:
    fprintf(f, "// @cfg %s = %s\n", key, value);
```

Import:

```c
ReplExportConfig cfg_out;
ReplExportCameraBlock cam_out;
repl_export_load_from_file(path, &cfg_out, &cam_out, ...);
glr_export_config_apply(&cfg_out);    /* app side; not repl_export.c */
glr_camera_apply_export_block(&cam_out);
```

`repl_export.c` parses `// @cfg <key> = <value>` into the bag. It does not
string-match `wireframe`, `camera_rotate`, `audio`, or any other semantic slug.
It parses the camera block into `ReplExportCameraBlock` without interpreting the
GL transform strings.

### 4c. Apply the same rule to examples

Leading example metadata is currently parsed and applied inside
`repl_example_loader.c`. That is how `camera_rotate` currently works even though
it is backed by `glr_camera.c` state.

Preserve that behavior without keeping the dependency:

- `repl_example_loader.c` extracts leading `@cfg` lines into
  `ReplExportConfig`.
- If a leading `// camera` block is present, it extracts it into
  `ReplExportCameraBlock` instead of applying it directly.
- It returns both payloads as an example-load effect/result.
- The controller/app applies the config bag and camera block through owner code.
- Non-leading `@cfg` lines still remain ordinary comments.

This is the `camera_rotate` footgun: it must not be dropped just because it is
not REPL-owned. The neutral bag lets it survive without making the example
loader call `glr_config_*`. The camera block follows the same rule and avoids
direct `glr_camera_*` calls from the loader.

### 4d. Owner-side fill/apply table

Owner-side fill/apply code should cover every current `@cfg` slug. A concrete
starting map:

| Slug(s) | Owner applying semantics |
|---|---|
| `msaa`, `line_smooth`, `accum_aa`, `point_attenuation` | app/render state now; `glr_state` after step 7 |
| `wireframe`, `grid`, `grid_major`, `grid_extent`, `axes`, `vertex_guides`, `xform_guide_mode`, `light_indicators`, `poly_highlight`, `backdrop`, `auto_normals`, `vertex_labels`, `normal_vectors`, `vertex_outlines`, `vertex_points` | app presentation state now; `glr_state` after step 7 |
| `camera_rotate` | `glr_camera` |
| `auto_time` | REPL time/variable runtime owner |
| `replay`, `replay_mode`, `replay_expand` | replay peer |
| `variable_panel` | variable-panel peer |
| `cpu_profile` | UI/profile owner |
| `code_panel`, `wrap_at_commas` | editor/UI chrome owner |
| `audio` | audio service |

During Step 4, some storage still lives in `ReplState`. That is acceptable as a
temporary storage fact, but `repl_export.c` should still be opaque: the app-side
fill/apply helper may read/write the current storage until Step 7 relocates it.

### 4e. Remove `glr_config` from the demo link set

After `repl_export.c`, `repl_scenes.c`, and `repl_example_loader.c` no longer
call `glr_config_*` or reference `GlrConfigItem`, `glr_config.c` falls out of
`repl_demo`. Removing direct `glr_camera_*` calls is also part of the same
ownership cleanup, although it does not change the current stub count because
`glr_camera.c` is linked rather than stubbed.

Stubs cleared:

`g_cfg_items`, `CFG_ITEM_COUNT`, `audio_get_cfg_mode`, `audio_set_cfg_mode`,
`variable_panel_view_mut`, `ui_state_profile_panel_mut`.

Stubs not cleared here:

`ui_state_viewport` and `ui_state_code_panel`. Those are caused by
`repl_export.c` calling `ui_layout_*`, not by config. Step 7 removes that edge.

## Step 5a - Extract Pure Structured-Block Validators

The lean loader in Step 5b needs pure block validators. They do not exist yet.
The current structured-block entry points are `editor_compile_close_brace`,
`editor_compile_if_block`, `editor_compile_func_def`, and
`editor_compile_for_loop`; they return `EditorCommitPlan`, which mixes a
`ReplCompiledChange` with editor cursor/input side effects.

Fix:

- Add pure `repl_compile_*` functions for close-brace, if-block, func-def, and
  for-loop syntax.
- Each returns `ReplCompiledChange`, diagnostics, and source-load effects. These
  effects are not editor UI effects; they describe loader placement only: next
  edit line, insert/overwrite mode, function-definition resume bookkeeping, and
  any "body lines follow" state currently encoded by editor cursor movement.
- `editor_compile_*` wrappers call the pure validator, translate the source-load
  effects into `EditorCommitPlan` post-effects, and add editor-only behavior
  such as input clearing, autocomplete clearing, status text, undo integration,
  and `load_line_to_input`.
- Existing editor behavior remains unchanged.

No stub count change.

Guard work:

- Extend compile guards so `repl_compile.c` cannot reference
  `EditorCommitPlan`, `editor_state_*_mut`, or editor input APIs. The new
  `ReplLoadEffects` type is allowed because it belongs to source loading, not
  editor UI state.

## Step 5b - Add a Non-editor Source-load API

`feed_line()` is editor input dispatch. It copies text into the input buffer and
runs the `try_commit_*` chain, including cursor and insert-mode effects.
Examples and import should not need editor input dispatch, but they still need
the same source-loading state machine.

Introduce an explicit load session plus compile/apply helpers:

```c
typedef struct {
    int edit_line;
    int insert_mode;
    int func_decl_resume_delta;
} ReplLoadSession;

typedef struct {
    ReplCompiledChange change;
    ReplLoadEffects    effects;
} ReplLoadPlan;

ReplLoadResult repl_load_compile_line(const char *text,
                                      const ReplLoadSession *session,
                                      ReplLoadPlan *out,
                                      ReplDiagnostic *diag);

int repl_load_apply_plan(ReplLoadSession *session,
                         const ReplLoadPlan *plan,
                         ReplDiagnostic *diag);

ReplLoadResult repl_load_apply_line(ReplLoadSession *session,
                                    const char *text,
                                    ReplDiagnostic *diag);
```

Semantics:

- `repl_load_compile_line()` dispatches through float-decl, assignment,
  structured-block validators from Step 5a, and normal GL command parsing.
- `repl_load_apply_plan()` preflights, writes the editor buffer text, applies
  predef/scratch operations, applies the REPL command-store mutation, and then
  updates only `ReplLoadSession` placement state.
- `repl_load_apply_line()` is the convenience path for examples/imports.

The loader is intentionally incremental, not a "batch until close brace" parser.
This matches the current `feed_line()` behavior: `for(i, 0, 4) {` inserts a
`CMD_FOR_BEGIN` plus placeholder `CMD_FOR_END`, then advances the load session
inside the block so subsequent body lines are inserted between them. Close-brace
and function-resume paths update loader placement state, but they do not
retroactively commit the whole block. Callers pass one `ReplLoadSession` through
the whole example/import stream.

The editor does not need to call the apply-line convenience. It can keep using
`EditorCommitPlan` and `editor_commit_apply_plan()` so undo and post-effects
stay editor-owned.

Convert:

- `repl_example_loader.c` from `feed_line()` to `repl_load_apply_line()`.
- `repl_export.c` importer call sites from `feed_line()` to
  `repl_load_apply_line()`.

Stubs cleared:

`feed_line`.

## Step 6 - Move Editor-shaped Helpers Out of Pipeline TUs

`load_line_to_input()` is editor input behavior. Remaining callers:

- `repl_core.c::repl_reformat_commands()` restores editor input after
  reformatting.
- `repl_scenes.c::load_scene_from_slot()` restores editor input after scene
  switch.

Fix:

- Move the reformatter implementation into `src/editor/` (likely
  `src/editor/reformat.c` or `src/editor/commit.c`).
- Make scene loading pure REPL: it restores program state and edit-line index,
  then returns an effect/result.
- The controller/editor actualizes the input-buffer restore after scene load.

Stubs cleared:

`load_line_to_input`.

## Step 7 - Move App State to `glr_state` and Opaque Layout Inputs

Steps 1-6 remove all but the layout stubs. Step 7 finishes the ownership split
and removes the last environment reads from `repl_export.c`.

### Current slice ownership

`ReplRuntimeState` in `repl_state.h` has seven slices:

| Slice | Genuinely REPL? | Contents |
|---|---|---|
| `document` | ✅ REPL | parsed source command array |
| `flat_program` | ✅ REPL | flattened executable command stream |
| `variables` | ✅ REPL | predef vars, scratch arrays A/B/C, func aliases, `time_playing` |
| `scenes` | ✅ REPL (mostly) | user-scene slots; per-slot cfg snapshot is app-state |
| `import_export` | mixed | scene-name hint (REPL), workspace_dir (app), pending workspace-header state |
| `presentation` | ❌ APP | wireframe, grid_theme, grid_major_idx, grid_extent_idx, axes_theme, backdrop_mode, show_vertex_{labels,normal_vectors,indices,outlines,points,guides}, show_light_indicators, highlight_current_poly, autonormal, code_panel_layout, wrap_at_comma |
| `render` | ❌ APP | multisample_enabled, line_smooth_enabled, accum_aa_enabled, point_attenuation_enabled |

Step 7 moves the two APP slices and splits the two mixed slices.

### 7a. Move app-state slices

`ReplRuntimeState` still contains slices that are app-frame state:

- `presentation`
- `render`
- app-owned pieces of scene config snapshots
- app-owned pieces of import/export state

Fix:

- Introduce `glr_state.c` / `glr_state.h` for app-frame presentation/render
  state.
- Leave `repl_state.c` with REPL language state: document, flat program,
  variables, program scene slots, and REPL-side import/export state.
- Move editor chrome fields such as code-panel layout/wrapping to the
  editor/UI owner rather than to `glr_state` if that better matches existing
  ownership.

### 7b. Split scene slot snapshots

`repl_scenes.c` should own program scene slots. App cfg snapshots should live in
an app-side scene companion, for example `glr_scenes.c`, using
`ReplExportConfig` bags.

The controller bundles both halves for F12 cycling and workspace save/load.

### 7c. Make layout an explicit export input

`repl_export.c` currently calls `ui_layout_scene_rect()` and
`ui_layout_code_panel_rect()`. That pulls `src/ui/layout.c`, then live
`UiState`, into the demo link set.

Fix:

- Add export options carrying the viewport/layout values export needs.
- The app/controller computes those values using UI layout code.
- `repl_export.c` consumes integers/options only; it does not call
  `ui_layout_*`.

Stubs cleared:

`ui_state_viewport`, `ui_state_code_panel`.

After Step 7, `repl_demo` no longer needs `tools/repl_demo/stubs.c` for current
pipeline behavior.

### What does *not* move

Step 7 does not touch:

- `EditorState` (cursor, selection, search, autocomplete, undo, editor buffer,
  transformers/highlights/virtual lines) — already in `src/editor/state.c`.
- `UiState` (viewport, pointer, status TTL, panel divider) — already in
  `src/ui/state.c`.
- Peer subsystems (`replay_state`, `variable_panel_state`, `color_picker_state`,
  `editor_help_session`) — already in their own files.
- `glr_camera` — already in `glr_camera.c`.
- `glr_config` (slug list adjusted in 7a; module itself stays put).

### Endpoint after Step 7

The four-owner contract from `MODULES.md` actually holds at the file layout,
not just the description:

```text
ReplState     -> repl_state.c       (REPL language state)
EditorState   -> src/editor/state.c (text-document model)
UiState       -> src/ui/state.c     (UI chrome)
GlrState      -> glr_state.c        (app-frame presentation/render)  <- NEW
+ peers       -> their own files    (replay, variable_panel, color_picker, ...)
```

## Save-file Compatibility

The line format remains unchanged:

```text
// @cfg key = value
// @scene-name ...
// @workspace-dir ...
// camera block
```

What changes is ownership of semantics:

- `repl_export.c` writes and parses the shape.
- Owners fill/apply config items and camera blocks.
- Unknown config keys are preserved in the bag during import and ignored by
  owners that do not recognize them.
- Existing examples and saved files keep working, including
  `@cfg camera_rotate = ...`.

Required tests:

- Round-trip every current config slug through fill -> export -> import -> apply.
- Include `camera_rotate` in example metadata tests.
- Verify non-leading `@cfg` lines still appear as comments in the code panel.
- Round-trip every existing exported example/workspace fixture.

## Guard Work

### Existing guards relevant to the plan

`make check-state-ownership` runs 31 sub-targets. The ones that bear on the
boundary this plan defends:

| Guard | What it forbids |
|---|---|
| `check-controller-boundaries` | Only `glr_ctrl.c` (+ small allowlist) may include `src/scene/` or `src/ui/` headers |
| `check-pure-scene-no-repl-state` | `src/scene/*.c` may not reference any `repl_(state\|replay)_*` symbol |
| `check-scene-no-repl-state-mut` | `src/scene/*.c` may not call `repl_state_*_mut*()` |
| `check-state-boundaries` | Scene + state-neutral cannot include `repl_state.h`; UI cannot mutate REPL state outside an allowlist |
| `check-views-no-owners` | `src/scene/*.c` and `src/ui/*.c` cannot include `repl_state_owners.h` |
| `check-ui-no-repl-state-mut` / `-read` | UI files cannot mutate or read live REPL state (snapshot only) |
| `check-no-set-status-in-compile-apply` | `repl_compile.c` and `repl_apply.c` cannot call `set_status` |
| `check-no-set-status-in-repl-parser` | Ratchet on `repl_parser.c` set_status calls (target 0) |
| `check-state-c-shrinking` | `repl_state.c` line count ratchets down only |
| `check-editor-ownership-budget` | Ratchet on `repl_state.c` calls into `ui_state_*` (target 0) |
| `check-glr-ctrl-not-editor-mirror` | `glr_ctrl.c` cannot grow per-field editor wrappers |
| `check-no-repl-editor-input-shim` | `src/editor/input.c` cannot delegate to legacy `repl_*_func` entry points |
| `check-repl-no-direct-buffer-read` | `repl_*.c` readers must go through `EditorBufferView` |

The destination layers (scene, UI) are already well-fenced: scene cannot
read or mutate REPL state, UI consumes snapshots only, neither can include
the owner-mutation header. The new guards below close the asymmetry on the
source side — REPL pipeline TUs keeping backward edges into editor input
dispatch, app shell, or UI state.

### New guards to land alongside the plan

Land guards with the step that makes the invariant true:

| Guard | Step |
|---|---|
| `check-repl-state-reset-program-pure` forbids peer/UI/editor reset and completion registration from `repl_state.c` | 2 |
| `check-no-set-status-in-pipeline` covers core, executor, flatten, export, scenes, example loader | 3 |
| `check-repl-export-opaque-to-header-state` forbids `repl_export.c` from referencing `glr_config_*`, `GlrConfigItem`, `g_cfg_items`, cfg slug semantics, `audio_*`, peer state, or `glr_camera_*` | 4 |
| `check-example-loader-no-header-semantics` forbids applying cfg/camera semantics in `repl_example_loader.c`; it may only collect opaque config and camera payloads | 4 |
| `check-repl-compile-no-editor-side-effects` keeps pure validators editor-effect-free | 5a |
| `check-no-feed-line-in-pipeline` forbids `feed_line(` in REPL pipeline TUs | 5b |
| `check-no-load-line-to-input-in-pipeline` forbids `load_line_to_input(` in REPL pipeline TUs | 6 |
| `check-repl-export-no-ui-layout` forbids `ui_layout_*` calls in `repl_export.c` | 7 |
| `check-repl-state-no-glr-state` and `check-glr-state-no-repl-mutators` keep the new state owner boundary honest | 7 |
| `check-repl-demo-stubs-shrinking` ratchets the demo stub symbol count from 17 to 0 | all |

The stub-count ratchet is the umbrella guard. Include it in
`check-state-ownership`; ratchet the expected count downward as each step lands.

## Out of Scope

- Removing `src/editor/state.c` from the demo. The editor buffer is the
  canonical text owner by design.
- Reintroducing source text onto `GLCmd`.
- Restricting examples/imported files to flat commands. The non-editor loader
  must support loops, functions, if-blocks, declarations, assignments, and
  ordinary GL commands.
- Having `repl_export.c` call owner callbacks directly. Owner code may wrap
  export/import, but `repl_export.c` itself should stay opaque to owner
  semantics.
