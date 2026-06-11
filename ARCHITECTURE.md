# REPL Architecture

> For the quick module map, see [`MODULES.md`](MODULES.md). Each
> `src/` subsystem also carries its own `README.md`
> (`src/repl/`, `src/editor/`, `src/app/`, `src/scene/`, `src/ui/`,
> `src/subsystems/`) with the layer-local ownership notes. For the
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
                menu/config actions (glr_actions, glr_config), app-frame
                presentation/render-policy state (glr_state), source-document
                adapter (glr_source_document), CLI debug dumps (glr_debug)
editor_*      = text-document model + controller (under src/editor/), incl. the
                document cursor (edit-line) and the editable line buffer
scene_*       = 3D stage: camera, projection, frame setup, decorators, 3D overlays
ui_*          = 2D editor chrome: code panel, menus, overlays, popups, HUDs
gl_repl.c/h    = current GLUT entry point and legacy shared header
                (rename to a `glr_*`-namespaced shell is on the open list)
```

The prefix is an ownership signal, not a generic sample prefix. New `repl_*`
modules should own REPL language, editor, source, workspace, replay, or command
model behavior. App-shell services belong under `glr_*` — the audio service
moved into this scheme as `src/app/glr_audio.c` (`glr_audio_*`), resolving the
former neutral `audio.c` / `repl_audio` name. Generic infrastructure keeps
neutral names such as `prof`. `gl_repl.c/h` is the one remaining name outside
the scheme; it is slated for the R8 namespace rename rather than serving as a
precedent.

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

1. Put the live bytes in `ReplRuntimeState` only if the state is genuinely
   REPL-language/program state. App-frame presentation and render policy
   belongs on `glr_state` (`src/app/glr_state.c`), editor document/session
   state on `EditorState`, and intentional sidecars (undo rings,
   user-scene slots) stay separate — call those out explicitly instead of
   describing them as runtime-state migration. REPL-pipeline TUs must not
   reach `glr_state` (`check-repl-state-no-glr-state`).
2. Add a named runtime slice in `src/repl/state.h`, wire it into
   `static ReplRuntimeState g_repl_state;`, and say whether the read path is
   currently `facade-backed`, `direct-runtime`, or `value-getter`.
3. Keep mutations on the owner side. Scene/UI renderers read snapshots only;
   render-time discoveries return through output structs that the controller
   actualizes back into state.
4. Extend the ownership tests in the same change: keep
   `repl_state_capture()`, `repl_state_restore()`, and
   `repl_state_reset_program()` (REPL-only) / `glr_ctrl_reset_all()`
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
  and fade/highlight decisions belong in `src/subsystems/replay/` (primarily
  `replay_playback.c`, `replay_fade.c`, and `replay.c`). Any scene use of
  replay data should be via snapshots or
   documented transitional helpers.

## Target Frame Pipeline

Top-level frame orchestration belongs in the controller:

```text
gl_repl.c GLUT display callback (future `glr` shell)
  -> glr_ctrl_display_frame          (gl_repl.c calls controller directly; no shim)
        -> tick profiling
        -> rebuild autonormals if dirty
        -> rebuild flat program if dirty                          [PROF_FLATTEN]
        -> push editor snapshots (transformers / highlights /     [PROF_SNAPSHOT*]
           virtual lines via replay_annotations_prepare)
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
(see `src/support/cpuprof.h`).

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

### Flatten cache and render-pass reuse

`repl_flatten_commands()` is the expensive interpreter boundary. It expands
loops/functions/conditionals, evaluates expressions and variable/scratch
assignments against the current bindings, and stores resolved `GLCmd` records
in the flat program. For example:

```c
x = 2;
y = pow(2, x);
glVertex2f(x, y);
```

is cached as numeric flat commands: assignment records whose `args[]` carry
`2` and `4`, followed by a `CMD_VERTEX2F` whose `args[]` are `{ 2, 4 }`.
The cache is not an OpenGL display list, VBO, or already-submitted driver
command stream; `src/repl/executor.c` still walks the cached `GLCmd[]` and
emits calls such as `glVertex2f(cmd.args[0], cmd.args[1])`.

`glr_ctrl_display_frame()` rebuilds the flat program only when it is dirty.
While animation is playing, advancing `t` marks the flat program dirty, so
expressions that depend on `t` re-evaluate once for that frame. Accumulation
AA, replay overlay passes, and vertex outlines reuse the same frame-level
`FlatProgramView`/snapshot instead of reparsing, reflattening, or re-evaluating
expressions per sample. Those passes may reapply precomputed assignment
commands from `args[]` while walking the flat stream, but the frame/probe
side-effect brackets restore predefined variables and scratch arrays so
self-referential assignments do not compound across AA samples.

### Editor-owned text (post `feature/editor-owns-text.md` + `feature/source-document-port.md`)

`GLCmd` is a pure parse-result struct: `type`, `args[]`, validity / vars
flags, and provenance fields (`src_cmd_idx`, `call_src_cmd_idx`, etc.).
There is no `source[]` member. Per-line canonical text lives in
`EditorBuffer.lines[MAX_COMMANDS][MAX_LINE_LEN]` inside **`EditorState`**
(`src/editor/state.c`), the editor's writable document model — *not* in
`ReplRuntimeState`. The parser returns both the `GLCmd` and the canonical
text in `ReplParsedLine { GLCmd cmd; char text[MAX_LINE_LEN] }`; commit
code passes both to text-aware command-store APIs
(`repl_command_store_*_with_line[s]`) so the text buffer moves in lockstep
with the command array.

**The neutral source-document port.** The REPL pipeline must not depend on
`EditorState`, so it never touches the editor buffer directly. Instead it
reads and mutates source text through the neutral port in
`source_document.h`:

* Reads go through `source_document_view()` → `SourceTextView` (a
  `const char (*lines)[MAX_LINE_LEN]` + count), sliced by
  `source_text_line(view, idx)` (out-of-range returns `""`). Consumers:
  `executor.c` (display text), `export.c`, `flatten.c`/`core.c` (reparse),
  `compile.c`, `src/subsystems/replay/replay_annotations.c`.
* Mutations go through `source_document_apply_change()` /
  `source_document_insert_line()` / `_replace_line()` / `_load_lines()` /
  `_clear()`, driven by a `SourceTextChange` descriptor.

Hosts provide the backing implementation by link-time symbol resolution,
not a runtime callback table:

| Host | Backing implementation |
|---|---|
| Full app | `src/app/glr_source_document.c` — forwards to `EditorState` |
| Standalone `repl_demo` | `tools/repl_demo/source_document.c` — tiny editor-free line store |
| Tests | whichever adapter the scenario links |

`check-repl-no-direct-editor`, `check-repl-no-direct-buffer-read`,
`check-no-store-text-api`, and `check-source-document-port-owners` (all in
the `check-state-ownership` gate) enforce that `src/repl/*` reaches text
only through this port.

Persistence sidecars carry parallel `lines[][]` arrays:

| Persisted form | Module |
|---|---|
| Undo / redo snapshots | `editor_undo` (`EditorUndoSnapshot.editor_lines`) |
| User-scene slots | `repl_scenes` (workspace save / load + LRU eviction) |
| Clipboard cmds | `EditorClipboardState.lines` |
| Single-file / workspace export | `repl_export` (no extra sidecar; reads the source-document port) |

Flat commands have no text of their own. A flat command maps to its
source line through `src_cmd_idx`, resolved via
`source_text_line(source_document_view(), src_cmd_idx)`.

### Document cursor ownership (post `feature/edit-line-ownership.md`)

The active edit-line cursor is **editor-owned**: it lives in
`EditorState.document.edit_line_idx` (`EditorDocumentState`) and is read
and written through `editor_state_edit_line()` / `_set()` / `_clamp()`.
There is no `repl_state_edit_line()` and no cursor pointer inside
`ReplCommandStore`. The REPL pipeline never reaches into editor cursor
storage:

* The parse / compile / flatten / load layers take the cursor as an
  **explicit `int` parameter** (and cursor-shifting store/apply ops update
  a caller-owned `int *cursor_inout`).
* Higher-level pipeline entry points that genuinely need to move the
  cursor (e.g. `scenes.c`) go through the `ReplHostEffects`
  `edit_line_get` / `edit_line_set` hooks (`repl_dispatch_edit_line_*`),
  which are no-ops when no host bridge is installed.

This keeps invariant β (REPL → editor symbol references forbidden) intact;
`check-repl-no-direct-editor` is the build guard. See
[`done/edit-line-ownership.md`](done/edit-line-ownership.md).

## Controller-Pushed Editor Snapshots

The controller treats per-frame UI overlay data as snapshots it builds
once and the UI consumes read-only. The snapshot family lives in
`src/ui/app/editor.h`:

| List | Push helper | What it carries |
|---|---|---|
| `UiTransformerList editor_transformers` | `glr_ctrl_push_color_transformers()` | One entry per editable color command (line idx + r/g/b/a/has_alpha/is_clear). Drives inline swatch render and color-picker hit-test. Future kinds: numeric slider. |
| `UiHighlightList editor_highlights` | `glr_ctrl_push_highlights()` | Feeding-normal cmd, feeding-color cmd, replay PC, search match, selection. Rendered as gutter accents and row backgrounds. |
| `UiVirtualLineList editor_virtual_lines` | `replay_annotations_prepare()` (via `_refresh_virtual_lines()`) | Replay-time annotation rows (substitution + evaluation) attached to the current source line. Layout, scroll, hit-test, and render all read from this list, so virtual-row counts have one source of truth (`replay_annotation_extra_rows_for_line()` counts the list). |

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

`gl_repl.c` and `gl_repl.h` still carry the app entry point and shared legacy
types/constants. A future `glr_*`-namespaced rename of the shell is open work
(see R8 in *Open Refactor Edges* below); it is intentionally a separate
mechanical cleanup after controller extraction, because `gl_repl.h` is included
broadly.

### Runtime GL Capability Detection

GL feature availability that varies by *runtime context* (not by build) is
detected once in `glr_ctrl_init_gl()` — the first point at which the GL
context is current — and pushed into the GL-free REPL/scene layers through
setters and `SceneRenderConfig`, never re-queried per frame.

The first case is **`glPointParameterfv`** (distance-attenuated point
size), core GL 1.4 but absent on some legacy contexts. Detection:

```
supported = GL_VERSION >= 1.4
          || glutExtensionSupported("GL_ARB_point_parameters")
          || glutExtensionSupported("GL_EXT_point_parameters")
```

The version check comes first on purpose: an ARB/EXT-only test
false-negatives on a 1.4+ core context that doesn't advertise the extension
string. The result is stored via `repl_executor_set_point_parameter_supported()`
(the executor no-ops `CMD_POINT_PARAMETER_FV` and falls back to a
camera-distance `glPointSize` approximation when unsupported) and mirrored
into `SceneRenderConfig.point_parameter_supported` so the star backdrop's
own direct call is gated identically.

**`GLR_NO_POINT_PARAMETER`** (environment variable, any non-empty value)
forces the unsupported path on capable hardware — the only override; there
is no build flag (it replaced the old compile-time `NO_POINT_PARAMETER`
macro). When point attenuation ends up off, `glr_ctrl_init_gl()` logs one
line to stderr that distinguishes the two causes:

* env override — `"glPointParameterfv disabled via GLR_NO_POINT_PARAMETER=..."`
  (and notes whether the hardware would otherwise support it);
* genuine lack — `"glPointParameterfv unsupported by this GL context
  (GL_VERSION ...)"` (and points at the env var for forced testing).

Detection MUST run before `repl_apply_init_bootstrap()` in the same
function: on unsupported hardware the injected `point_attenuation` bootstrap
entry has to be skipped entirely rather than invoking the missing entry
point.

The second case is the **GPU profiler's timer queries** — the profile
panel's GPU column, measured by `src/support/gpuprof.c`. Detection:

```
has_timestamp = glutExtensionSupported("GL_ARB_timer_query")
              || GL_VERSION >= 3.3
advertised    = has_timestamp
              || glutExtensionSupported("GL_EXT_timer_query")
```

The entry points are runtime-loaded in `glr_ctrl_init_gl()` (same
core-then-ARB/EXT-suffix loader pattern as `glPointParameterfv`) and
injected into gpuprof as a function-pointer table, so the support module
stays GL-header-free. Two measurement modes, picked at init by what
loaded:

* **timestamp mode** (preferred; `glQueryCounter(GL_TIMESTAMP)`, ARB /
  GL 3.3 only — typical on Linux/Mesa contexts): one marker per section
  transition; interval deltas tile the GPU timeline exactly, so
  per-section times are additive and Frame Total is a true window.
  `glQueryCounter` is only loaded when `has_timestamp` is advertised —
  GLX's GetProcAddress can return a callable stub for *any* name, so
  load-and-see is not a safe capability test.
* **elapsed mode** (fallback; `GL_TIME_ELAPSED_EXT` brackets — the only
  option on Apple's GL 2.1, which exposes `EXT_timer_query` but not
  `ARB_timer_query`): bracket windows overlap on pipelined and
  tile-deferred GPUs, so per-section sums can exceed the wall-clock
  frame time; read the column as a relative-hotspot signal there (the
  full caveat lives in `src/support/gpuprof.h`).

Either way results are harvested asynchronously — a 4-deep ring of
per-frame query slots polled with `GL_QUERY_RESULT_AVAILABLE`, read 1–3
frames later — never a `glFinish`. How much gets queried per frame
follows the profile panel: hidden → no queries at all, ON → top-level
sections, DETAILS → the full GPU subset (`glr_prof_set_gpu_capture_mode`,
policy table in `src/app/glr_prof.c`).

**`GLR_NO_GPU_PROF`** (environment variable, any non-empty value)
disables GPU timing entirely — the panel's GPU column reads `--`, and
the Max column falls back to plain CPU. When GPU timing ends up off,
`glr_ctrl_init_gl()` logs one stderr line distinguishing the env
override, a context that advertises timer queries but yields no loadable
entry points, and a context with no timer-query support at all.

### Dynamic Reshape Projection (export + code panel)

The exported standalone C file's `reshape()` and the live code panel's
footer chrome must show the projection the scene is *currently* applying
(perspective in 3D, ortho in 2D), not a hardcoded `gluPerspective`. This
is the canonical pattern for a **per-frame, GL-derived value that becomes
emitted source text** consumed by GL-free modules. Two cooperating
mechanisms:

1. **Scene caches what it applied (Tenet 3).**
   `scene_apply_projection()` writes a jitter-free `SceneProjectionDesc`
   into a file static every frame; `scene_get_active_projection()` reads
   it. The continuous perspective↔ortho blend is *snapped to the
   dominant side* (`mix < 0.5` ⇒ ortho) because `reshape()` emits one
   discrete mode, never an interpolated matrix. Scene exposes data; it
   does not format text or know about export.

2. **Controller-installed projection bridge** (same shape as
   `ReplExportCameraBridge`). `src/repl/export.c` is GL-free, so it owns
   no projection math. `ReplExportProjectionBridge.fill_reshape_block`
   is installed by `glr_ctrl.c` next to the camera-distance source; its
   adapter reads `scene_get_active_projection()` and formats the C
   lines. No bridge installed (scene_demo, tests) ⇒
   `repl_export_reshape_projection_lines()` returns the canonical
   perspective default (correct `0.1, 200.0` near/far).

**Rule — where a per-frame dynamic value is resolved.** Apply this test
to *any* value that is (a) recomputed per frame from live REPL/scene
state and (b) read by more than one consumer in the frame loop:

> **If a per-frame value has more than one consumer, the controller
> resolves it once into the frame snapshot; consumers read the snapshot.
> Never let two consumers re-resolve it independently.**

The reason is structural, not specific to any one value: the code
panel's row-count/follow-scroll pass and its render pass sit on
*opposite sides* of `scene_render_3d_scene()` in
`glr_ctrl_display_frame()` (snapshot/follow-scroll → scene render →
panel render). Anything resolved live in both passes can observe two
different values across that boundary whenever a transition lands on
that frame — here a 2D/3D switch would let row-count see one
`gluPerspective(...)` line while render emits two `glOrtho(...)` lines,
skewing scroll-follow and row hit mapping. "Deterministic within a
frame" is *not* sufficient — the inputs themselves change mid-frame at
the scene-render boundary. This is just `UiRenderSnapshot`'s existing
contract ("UI render code reads only from the snapshot") restated for
the case where the value is computed rather than copied.

⚠️ **Do not generalize the `"static float g_angle = 0.0f;"` precedent.**
That special-case resolves at the consumer site, which is safe *only*
because its single consumer is the file writer (one pass, off the frame
loop). It is the wrong model for any value the code panel reads — copy
the snapshot shape below, not the `g_angle` shape.

**Dynamic-footer sentinel mechanism.** `g_footer_pre_init[]` is iterated
verbatim by three consumers (the file writer in `src/repl/export.c` and
the code panel's row-count *and* render passes in
`src/ui/app/repl_code_panel.c`). A line whose count or text is dynamic is
stored as a unique sentinel constant
(`REPL_EXPORT_RESHAPE_PROJ_SENTINEL`); every consumer special-cases it.
Per the rule above:

* **Code panel (per frame):** the controller resolves the block once in
  `glr_ctrl_build_ui_snapshot()` into
  `UiRenderSnapshot.reshape_proj_lines/_count`; both panel passes read
  that frozen copy and never touch the resolver. This is the canonical
  shape — UI reads the snapshot only (the symmetric counterpart of
  `SceneRenderConfig`). The block is the *previous* frame's scene
  projection (snapshot is built before scene render); a one-frame text
  lag during a transition is invisible and, crucially, internally
  consistent. snapshot.h hardcodes `UI_RESHAPE_PROJ_LINES/_LINE_MAX`
  for UI-layer purity, with `STATIC_ASSERT` equivalence to the
  `REPL_EXPORT_PROJ_*` source-of-truth in `glr_ctrl.c` (same pattern as
  the scene-tab dims).
* **File save (discrete action):** `repl_export_save_output()` calls
  `repl_export_reshape_projection_lines()` directly — a single pass on
  the Ctrl+S thread, not split across scene render, so it correctly
  captures the projection in effect at save time. (Routing this through
  a controller-owned `ReplExportLayout`-style export context is the
  documented next step if save is ever folded into the frame path.)

`scene_get_active_projection()` is the *nearest-steady* projection: the
continuous blend is snapped to the dominant side (`mix < 0.5` ⇒ ortho).
It is deliberately not the live blended 16-float matrix — `reshape()`
emits one discrete mode, not an interpolation; a faithful mid-transition
matrix export would need a different, explicitly-named contract.

Adding another dynamic footer line follows the same recipe: sentinel
constant in `export.h`, one resolver, controller resolves once into the
snapshot for the panel, special-case in the consumers.

**Build-enforced**, not convention-only (both in the
`check-state-ownership` gate):

* `check-ui-no-export-resolver` — no `src/ui/` file may call
  `repl_export_reshape_projection_lines()`; the panel reads the
  snapshot-frozen block. This is the structural backstop for the rule
  above: the mistake fails the build, not just review.
* `check-repl-export-via-bridge` — `src/repl/export.c` may not include
  `scene/`/`app/` headers or call `scene_*`/`glr_*`; it pulls
  app/scene-derived values only through controller-installed bridges
  (`ReplExportProjectionBridge`, `ReplExportCameraBridge`,
  `ReplExportConfig`). Complements `check-gl-boundaries` (which already
  bars GL *calls* in the REPL pipeline) and `check-repl-export-no-ui-layout`.

### 2D Orthographic Scale (GL_FEEDBACK probe + zoom)

An orthographic projection has no inherent scale — unlike perspective,
moving the camera toward the scene changes nothing on screen. So the 2D
view must *pick* an eye distance whose on-screen size it reproduces, and
zoom must rescale that pick rather than dolly a camera the projection
ignores. All of this lives in `src/scene/render.c`; the controller feeds
only `cam_dist` and `projection_mix` (the 2D↔3D blend) through
`SceneRenderConfig`.

**The probe runs once per *entry* into 2D — never on zoom.**
`scene_update_ortho_ref()` calls `scene_probe_eye_dist()` (a
`GL_FEEDBACK` pass that replays the user geometry through a wide ortho
box and returns the *depth-center* — the midpoint of the drawn
geometry's eye-distance span) on exactly one frame: the rising edge
where ortho starts contributing (`ortho_now && !ortho_active`, i.e. the
instant a 3D→2D switch begins, or startup directly in 2D). That single
sample is frozen into `SceneRendererState.ortho_ref_dist`, together with
the camera distance at that moment (`ortho_ref_cam_dist`). For the entire
2D dwell after that — including every zoom frame — neither branch of the
edge test fires, so there is no feedback pass at all. One feedback pass
per round-trip into 2D, full stop. (This is the default
`GLR_ORTHO_REF_FROZEN` mode; see `config.h`.)

**Zoom rescales the frozen reference by arithmetic, not a re-probe.**
`scene_effective_ortho_ref()` returns
`ortho_ref_dist + (cam_dist - ortho_ref_cam_dist)` — the frozen
depth-center plus the live camera-distance delta accrued since the
freeze. The mouse wheel already drives `cam_dist`
(`glr_camera_add_zoom_velocity` → `glr_camera_tick`), so this alone makes
the ortho box grow/shrink with the wheel; no other wiring is needed. Both
projection sites — `scene_compute_active_projection()` (the cached
`SceneProjectionDesc`) and `scene_apply_projection()` (each AA sample) —
read this one helper so they can't diverge, and it clamps to a positive
floor so a deep zoom-in can't collapse or invert the box. Regression:
`test_scene_ortho_zoom_rescales` in `tests/test_scene_render.c`.

**Why a delta and not a re-probe.** Moving the camera by Δ is a rigid
translation: it shifts *every* vertex's eye-distance by exactly Δ, so
`frozen_ref + Δ` reconstructs precisely what a fresh probe at the new
distance would measure — but without the cost (the ~96K-float feedback
buffer walk happens once at the switch, not on every wheel tick) and,
crucially, without *breathing*. A live re-probe would also track
animation: `t`-driven geometry moving in and out of the depth span would
wobble the ortho scale every frame even when the user isn't touching
anything. Freezing the intrinsic depth-center and adding only the camera
delta gives zoom-tracking without animation-induced wobble. (The
non-default `GLR_ORTHO_REF_PERFRAME` knob re-probes every ortho frame and
accepts the breathing in exchange for tracking live scene motion — even
that is per-frame, not keyed to zoom.)

**Wheel feel.** Two independent `config.h` knobs, shared by 2D *and* 3D
zoom (the wheel path is mode-agnostic): `GLR_WHEEL_ZOOM_STEP` (per-notch
velocity impulse) sets magnitude, and `CAM_DECAY_ZOOM` sets smoothness.
One notch travels
`GLR_WHEEL_ZOOM_STEP / (1 - CAM_DECAY_ZOOM)` distance units, eased over
~`1 / (1 - CAM_DECAY_ZOOM)` frames, with `(1 - CAM_DECAY_ZOOM)` of the
motion on the first frame — lower decay is snappier but more "stepped",
higher is smoother but coasts longer. Rapid notches stack onto the
velocity, so fast scrolls still travel quickly.

### Mesh Export (PLY via GL_FEEDBACK)

The `.ply` exporter (F11 / File → Export .ply / `--export-ply <file>`)
captures the **live flat program** once through
`glRenderMode(GL_FEEDBACK)` and writes an ASCII PLY mesh. Everything the
scene draws — user `glVertex` polygons, GLU-tessellated polygons, and the
GLUT solids (teapot/sphere/cube/cone/torus, which emit no REPL-tracked
vertices) — is captured through this **one** path, so the export can't
drift from what renders.

**Two-module split — GL capture vs. pure writer.** The GL-coupled half is
`src/app/glr_mesh_export.c` (`glr_export_mesh_ply`); the parsing/writing
half is `src/support/mesh_ply.c`, which calls **no** GL function and
includes **no** GL header — it only reads a plain float buffer, so it is
fully unit-testable with synthetic buffers and no GL context
(`tests/test_mesh_ply.c`). The pure writer redefines the OpenGL feedback
token values (`MESH_PLY_TOK_*`) locally; `glr_mesh_export.c`
`STATIC_ASSERT`s them against the real `GL_*_TOKEN` macros so any drift is
a compile error.

**Fixed capture transform → invertible window coords.** The capture pass
installs a known, scene-independent transform so the pure writer can run
the projection backwards to world space: identity modelview (no camera),
a containing `glOrtho(-R, R, …)` with `R = 1000` (clips nothing a
hand-typed scene reaches at ~1e-4 float precision), a `1024²` viewport,
and `glDepthRange(0, 1)`. The writer inverts exactly this
(`MeshPlyCapture` carries `ortho_r` / viewport / depth-range) — note
`glOrtho` maps world `z → -z/R`, so the depth inversion negates. State is
saved/restored (`glPushAttrib(GL_ALL_ATTRIB_BITS)` + both matrix stacks
pushed explicitly, since `glPushAttrib` doesn't cover them), and feedback
produces no fragments, so the visible frame is undisturbed.

**Raw color, all faces.** Feedback honors lighting / polygon-mode / cull,
so the pass forces `GL_FILL`, disables `GL_CULL_FACE`, and disables
`GL_LIGHTING` — *and* the executor suppresses the program's own
`glEnable(GL_LIGHTING / GL_CULL_FACE)` in export mode
(`ReplExecutionOptions.encode_feedback_normals`). Without that suppression
a scene that re-enables lighting would feed back per-vertex *lit* colors
(not the authored `glColor` material) and cull back faces — the bug that
motivated the encode flag. Vertex color is written linear (pass-through)
by default; `--export-ply-srgb` (`MeshPlyOptions.srgb_decode`) applies the
sRGB→linear EOTF to RGB (alpha untouched) for color-managed viewers.

**Authored normals via the texcoord channel.** Feedback's `GL_3D_COLOR`
mode returns only position + color, so a synthesized geometric normal is
all that's recoverable. To carry the *authored* per-vertex normal, the
export pass uses `GL_3D_COLOR_TEXTURE` (11 floats/vertex) and the executor
mirrors each vertex's world-space normal — inverse-transpose of the
begin-time modelview 3×3 — into the texcoord `(s,t,r)` under an identity
texture matrix (so it round-trips un-projected). Whether a given run of
texcoords *is* a normal is signalled **out of band** by bracketing user
`glBegin`/`glEnd` with `glPassThrough(MESH_PLY_PASS_NORMALS /
_NO_NORMALS)` — a passthrough value can't collide with real vertex data
the way an in-band tag could. Solids / tess (no authored normal) stay in
the default NO_NORMALS mode and the writer synthesizes + smooths a
geometric normal instead. (`glPassThrough` and `glGetFloatv` are illegal
between `glBegin`/`glEnd`, so the modelview is snapshotted once at
`glBegin`; it is constant within a primitive because transforms are
rejected mid-primitive.)

**The pure writer is two-pass.** Pass 1 parses the token stream into
transient `corners[]` (3 per triangle, n-gons fan-triangulated) +
per-triangle face normals, and collects any **deferred edge endpoints**
(see below). Pass 2 builds the output vertex set — welded by
`(quantized pos, 8-bit color)` when `weld && smooth_normals`, else 1:1
flat — resolves per-vertex normals (authored wins, else
face-normal-averaged), then resolves edges and writes
`vertex`/`face`(/`edge`) elements.

**Line edges.** Line geometry is exported as a PLY `edge`
element (`src/support/mesh_ply.c`; see
[`plans/done/ply-line-edge-export.md`](plans/done/ply-line-edge-export.md)):
`glBegin(GL_LINES/LINE_STRIP/LINE_LOOP)` arrive as `GL_LINE` /
`GL_LINE_RESET` feedback tokens. Their endpoints are collected in pass 1
(the weld table doesn't exist yet) and resolved in pass 2 against a
key→index table seeded with the face vertices.

Edges **always weld** — endpoints coincident with a face vertex or
another endpoint collapse onto it; the rest append as new vertices (with
a degenerate `(0,0,0)` normal, since they touch no face) — and this holds
even on the flat face path, so a flat export still gets a coherent edge
index set. Edges are stored undirected (`a ≤ b`) and deduped by sort +
unique; `mesh_ply_write` reports the deduped count via an `out_edges`
out-param (the return value stays the triangle count).

**Coverage gap.** Real feedback needs a live GL context, so the
capture/encode contract can only be exercised end-to-end with a display
(the stub `glRenderMode` returns 0). The pure writer is fully covered with
synthetic buffers; the executor's encode contract is covered in stub mode
via `gl_stub_counts` (`test_export_normal_encoding` in
`tests/test_repl_executor.c` asserts lighting/cull suppression +
texcoord/passthrough emission). Verifying real feedback *values* needs a
real GL context — Xvfb (Linux) or the **OSMesa backend** below, which renders
this exact path with no display (`--export-ply` headless matches a native
capture to ~1e-4).

### Headless Rendering & Screenshots (OSMesa)

`make ... FREEGLUT_OSMESA=1` builds against a freeglut **OSMesa** backend that
renders into a CPU buffer with no window system at all (no Cocoa/GLX/X11) — a
software (llvmpipe/swrast) **compatibility** context, so fixed-function GL, the
GLUT solids, GLU tessellation, and `glRenderMode(GL_FEEDBACK)` all work. This is
the load-bearing path for headless CI: the real-GL tests (`make gl-tests`) and
`--export-ply` run with no display, and pixel readback is byte-identical to a
native render.

**It is a build-mode swap, not a source change.** `gl_includes.h`'s non-Apple
branch already includes the Mesa headers (`<GL/gl.h>`/`<GL/glu.h>`/
`<GL/freeglut.h>` — on macOS the Cocoa build *already* compiles against
Homebrew's `/opt/homebrew/include/GL` headers and only links Apple's GL
framework). So OSMesa mode just (a) builds the vendored freeglut with
`-DFREEGLUT_OSMESA=ON` instead of `-DFREEGLUT_COCOA=ON`, and (b) links Mesa
`libGL`/`libGLU` + `libOSMesa` instead of the Apple frameworks (system GL/GLU +
`libOSMesa` on Linux). Build dir and objects are suffixed (`build-osmesa/`,
`build/<cfg>-osmesa/`) so the OSMesa and native builds never collide. The
`-std=c99` sources and every guard are untouched. Mechanics live in the Makefile
(`FREEGLUT_OSMESA` ⇒ `FREEGLUT_CMAKE_BACKEND`, the per-platform link block, the
vendored-archive prereq gate) — see `CLAUDE.md`'s *Headless OSMesa build*.

The backend lives in the **vendored** freeglut only, so OSMesa mode requires
re-vendoring from a freeglut that carries it
(`FREEGLUT_REPO=<path-or-url> scripts/vendor-freeglut.sh <ref>`; the source is
recorded in `third_party/freeglut/VENDORED.txt`). Two fixes in that fork make it
usable as a library consumer:

- **Teardown.** An app that exits without an explicit `glutDestroyWindow()`
  destroys its window from freeglut's `atexit(fgDeinitialize)` sweep. On
  Gallium/swrast that crashed twice (a redundant `OSMesaMakeCurrent` re-bind
  during destroy faulting in `st_framebuffers_purge`, then `OSMesaDestroyContext`
  called through a driver vtable the runtime had already finalized at
  `__cxa_finalize`). The backend now skips the redundant re-bind and, via a
  LIFO-ordered `atexit` marker, leaks the context at process exit rather than
  destroying it through dead state. Runtime `glutDestroyWindow` is unaffected.

- **`SIGUSR1` frame capture.** `kill -USR1 <pid>` snapshots the current frame to
  a numbered PPM — no app-side code, even when the app is idle. The signal
  handler is async-signal-safe (it only sets a `volatile sig_atomic_t` flag);
  the flag is serviced on the **main thread** at two safe points — the buffer
  swap path (`fgPlatformGlutSwapBuffers`, which already `glFinish()`es each
  frame) for actively-animating apps, and the main-loop tick
  (`fgPlatformProcessSingleEvent`, which `SIGUSR1` wakes out of its `nanosleep`)
  for idle ones, reading the last completed frame so no redraw is needed.
  Capture reads the colour buffer **directly** via `OSMesaGetColorBuffer` (no
  `glReadPixels` round-trip), packs RGBA→RGB, and emits rows top-to-bottom to
  invert OSMesa's bottom-up origin. Output prefix from `FREEGLUT_CAPTURE_FILE`
  (default `freeglut`), files `<prefix>-NNNN.ppm`; convert with e.g.
  `magick shot-0000.ppm shot.png`. POSIX only (no `SIGUSR1` on Windows).

**Animation clock & the `--time` / `GLR_TIME` start offset.** The predefined
`t` variable is a *fixed-timestep* clock: while animation is playing (the
default — `time_playing` initialises to `1`; Ctrl+T *pauses*), the controller's
60 Hz timer advances `t` by exactly `GLR_FRAME_DT_SECS` (1/60 s) **per rendered
frame**, decoupled from wall-clock (`glr_ctrl.c` comment, *"motion speed stays
decoupled from redraw rate"*). So under the slow software OSMesa renderer `t`
lags real time, but every frame is a clean 1/60 s step — capture every frame and
play back at 60 fps for smooth real-time motion. `t` starts at `0`; `--time
<secs>` (or `GLR_TIME`, with the flag winning) sets the initial value via
`repl_set_time()` → `repl_state_time_set()` (`src/repl/state.c`), read in
`main()` *after* any `--example` load (which resets `t`) so the override sticks.
This lets a headless capture begin from a later point in an animation's timeline
rather than always from `t = 0`.

**Record mode → GIF/MP4 (`FREEGLUT_CAPTURE_FRAMES`).** `scripts/record-gif.sh`
drives a headless run and assembles the frames with `ffmpeg` into a GIF + MP4.
Its knob is **duration** (clip length, invariant of `--fps`); it computes
`N = round(duration × fps)`, passes it as `FREEGLUT_CAPTURE_FRAMES=N`, and the
backend captures every rendered frame and `exit(0)`s after N. Because `t` is a
fixed timestep, the N frames are deterministic (`t0 + i/60`) and identical across
machines — generate slowly (Mac ~2.7 fps, gracemont ~21 fps), play back at any
fps. The record + `SIGUSR1` capture are both serviced from the backend's
**main-loop tick** (`fgPlatformProcessSingleEvent`), *not* the swap path: a
single-buffered window's `glutSwapBuffers()` short-circuits before the platform
swap, and `fghRedrawWindow()` exposes no per-display hook, so the per-iteration
tick (1:1 with the display under the software renderer) is the only per-frame
hook a backend has without editing core freeglut. Record mode uses a cheap
colour-buffer content signature to skip the pre-first-render buffer and trigger
on the first real frame. This keeps the whole feature inside the OSMesa backend
files (clean for upstreaming — no core freeglut change).

### Startup & Audio-Worker Diagnostics

Two always-on stderr diagnostics localise startup stalls and
audio-thread hitches (notably on slow Linux disks). Both follow the
project's one-line-stderr convention (same as the point-parameter log
above); neither is gated off by default — the point is to see them
when a stall happens.

* **Init trace** (`gl_repl.c`). `main()` calls `init_trace(<phase>)` at
  each startup phase; it prints `[init +N.NNNs] <phase>` with
  wall-clock seconds (`gettimeofday`, not the per-platform timebase in
  `src/support/cpuprof.c` — ms granularity is enough and this stays portable/C99)
  elapsed since the first call. Two granularity levels share one
  stream:

  *Baseline phases* (always emitted): `start`, `glutInit begin`,
  `window created`, `GL init done`, `REPL bootstrap done`,
  `glr_audio_init begin/done`, `audio playlist started`, `entering
  main loop`. The only synchronous audio work on the `main()` path is
  `ma_engine_init()` (it opens the OS audio device); `--no-audio`
  isolates it.

  *Detailed phases* (gated on the `--detailed-prof` CLI flag or
  `GLR_DETAILED_PROF` env var, any non-empty value; default off):
  `glutInit done` (splits the glutInit runtime), `playlist scan done
  (N tracks)` (opendir/readdir over `assets/`), `playlist start
  requested` (after the synchronous `load_state` INI read and worker
  poke), and the post-`glutMainLoop` `frame N display callback` /
  `frame N render done` / `frame N swap done` triples from
  `display_func` for the first two frames. The frame-1 triple splits
  the otherwise-invisible time between `glutMainLoop()` and the OS
  showing pixels (GLUT solid-shape display-list compilation, macOS
  first-drawable wait, GL stack lazy init); the frame-2 triple is the
  steady-state control that reveals whether spending was first-frame-
  only (the expected case) or a real regression. Gated via
  `init_trace_detail()` plus inline flag checks on the snprintf-using
  sites so the default boot does zero extra work for these phases.

* **Worker hitch detector** (`src/app/glr_audio.c`). The audio worker
  (`audio_worker_main`) is event-driven: it sleeps on
  `pthread_cond_wait`, wakes to run exactly one blocking lifecycle op
  (`worker_load` → `ma_sound_init_from_file`; `worker_uninit_all` →
  `ma_sound_uninit` stream page-flush; `worker_advance`;
  `worker_save_state`), then sleeps again. The dispatch span is timed
  with `clock_gettime(CLOCK_MONOTONIC)` **after the mutex is released**
  so only the blocking work counts, and any op at/over the threshold
  logs `repl_audio: worker hitch: <op>[+save] took N ms (threshold
  M ms)`. Threshold via `GLR_AUDIO_HITCH_MS` (default 50; `0` disables;
  read once and cached in a static). `AWR_QUIT` is intentionally
  outside the timed span — a slow final save/uninit at shutdown is not
  a runtime hitch. These stalls delay track change / resume only; the
  miniaudio device-callback thread is owned by miniaudio, not the
  REPL, so this detector does not (and cannot) observe playback
  underruns there.

### Music Asset Resolution

The playlist is `*.mp3` files discovered at startup and played in
filename order. `build_mp3_playlist()` (`gl_repl.c`) concatenates **three
sources**, each scanned by `scan_dir_into()` and sorted independently so
every source keeps its own filename order:

1. **Primary assets dir.** `./assets` relative to the working directory
   by default. Overridden by `--assets <dir>` (highest precedence) or the
   `GLR_ASSETS_DIR` env var — `--assets` beats env beats default,
   resolved in `main()` and passed into `build_mp3_playlist()`. This is
   the only source the override touches.
2. **Bundled-beside-the-executable.** `<exe>/../Resources/assets`, with
   the executable path from `executable_dir()` (`_NSGetExecutablePath` on
   macOS, `/proc/self/exe` on Linux). This is the macOS `.app` case:
   `make app` copies `assets/sample.mp3` into `Contents/Resources/assets/`,
   so a Finder-launched bundle (cwd `/`, where source 1 finds nothing)
   still ships with music. The bundle subfolder name is fixed; the
   override does not change it.
3. **Per-user music folder.** `user_music_dir()` —
   `~/Library/Application Support/gl-repl/Music` (macOS) or the XDG data
   home (`$XDG_DATA_HOME/gl-repl/music`, else `~/.local/share/...`)
   elsewhere — created on first run by `ensure_dir()` (a `mkdir -p`) and
   announced once on stderr (`repl_audio: add more music in <dir>`) so
   users have a place to drop their own tracks.

If all three yield zero `.mp3`s, it falls back to the single-file
`AUDIO_DEFAULT_MUSIC` (`assets/song.mp3`); `--no-audio` skips audio
entirely. The whole model lives in `gl_repl.c`'s file-private statics —
no module touches it. The platform branches in `executable_dir` /
`user_music_dir` are `#ifdef`-guarded and stay C99/portable; the Windows
arms they still need are tracked in
[`plans/in-review/windows-port-mingw.md`](plans/in-review/windows-port-mingw.md)
(§1F). `--assets` / `GLR_ASSETS_DIR` are pure string + `opendir`, so they
need no per-platform code.

## Keyboard Shortcut Definition Sites

Keyboard shortcuts are **not** defined in one place today. They are
spread across four layers and resolved in a fixed priority order by the
controller. This section is the map — and the argument for a future
centralized keymap.

### Dispatch order (who wins a contested key)

Set in `glr_ctrl_keyboard` / `glr_ctrl_special` (`src/app/glr_ctrl.c`).
An earlier layer that consumes a key shadows every later one:

* **ASCII keys:** Cmd→Ctrl normalize → rename modal → file-prompt modal
  → Esc router (color-picker / help close) → config menu (`` ` ``) →
  **cfg ASCII shortcut** → replay → save (Ctrl+S) → debug dump (Ctrl+P)
  → accum jitter (Ctrl+= / Ctrl+−) → post-process filter (Ctrl+N) →
  code focus (Ctrl+Shift+F) → tutorial ack → quit (Ctrl+Q) →
  **editor (`editor_handle_key`)** as the fallback.
* **Special keys (F-keys / arrows):** replay → **cfg special (F2–F10)**
  → audio (Ctrl+Left/Right) → help tab/scroll → help toggle (F1) →
  scene cycle (F11/F12) → **editor (`editor_handle_special`)**.

Because the cfg layer runs *before* the editor, the config table wins
any contested Ctrl-letter; the editor only sees what no earlier layer
claimed.

### The four definition sites

| Layer | Where | What it binds |
|---|---|---|
| **Config table** (closest thing to a registry) | `g_cfg_items[]` in `src/app/glr_actions.c`; dispatched by `glr_cfg_handle_ascii_shortcut` / `cfg_match_row` (Ctrl, Ctrl+Shift) and `glr_cfg_handle_special_shortcut` (F2–F10) | Every config toggle/cycle. Each row's `(key_code, is_special, modifiers)` declares its shortcut: F-key = `is_special=1, key_code=GLUT_KEY_F<n>` (and **Shift+F<n> steps the cycle backward**); Ctrl = `is_special=0, key_code=KEY_CTRL_<x>`; Ctrl+Shift adds `GLUT_ACTIVE_SHIFT` (two-pass match: pass A prefers a Shift-required row, pass B the plain row) |
| **Editor** | `editor_handle_key` / `editor_handle_special` in `src/editor/input.c`; search keys in `src/editor/search.c`; modal capture in `inline_rename.c` / `inline_file_prompt.c` | Text / cursor / selection: `;` commit, Enter, Tab, Esc, Backspace/Delete, arrows, Home/End/PageUp/Down, Ctrl+A/E (cursor), Ctrl+Z/Y (undo/redo), Ctrl+C/X/V (clipboard), Ctrl+D/L (delete/clear), Ctrl+F (search), Ctrl+\ (reformat), Ctrl+/ (comment toggle), printable chars |
| **Controller router** | `glr_ctrl_keyboard` / `glr_ctrl_special` + `glr_ctrl_router_*` in `src/app/glr_ctrl.c` | Cross-subsystem: Ctrl+S (save), Ctrl+P (debug dump), Ctrl+Q (quit), Ctrl+N (post-process filter), Ctrl+K (replay jump-to-cursor), Ctrl+= / Ctrl+− (accum samples), Ctrl+Shift+F (code focus), Ctrl+Left/Right (audio track), F1 (help), F11/F12 (scene cycle), Esc (close picker/help) |
| **Peer subsystems** | `src/subsystems/replay/replay_input.c`; tutorial SET-step ack in `src/subsystems/tutorial/tutorial_runner.c` | Active-mode keys that shadow the editor while the subsystem holds focus: replay m/M, space, arrows, Esc; tutorial Enter/Tab/Space during a showcase step |

Key-code constants live in `include/keys.h` (`KEY_CTRL_*`); F-key codes
come from GLUT (`GLUT_KEY_F<n>`). macOS Cmd+letter is folded to
Ctrl+letter by `editor_input_normalize_super_to_ctrl` at the top of
`glr_ctrl_keyboard`, so every downstream layer sees Cmd+B as Ctrl+B.

The F1 help "Keys" tab and CLAUDE.md's *Key Controls* table are
maintained by hand **except** the F-key section of the help, which is
generated from the config table via the `ReplHelpFkeyProvider`
(`glr_ctrl_help_fkey_label` reads each F-key row's label by `key_code`).

### Toward a centralized keymap

The config table is already a declarative registry for *config*
shortcuts; the editor / router / peer keys are still imperative
`if (key == …)` chains. A future cleanup could lift every binding into
one descriptor table — `(key, modifiers, owning layer, action)` —
that the dispatchers consume in priority order, making the whole keymap
inspectable in one place and auto-derivable for the help overlay and the
docs. Two byte-level traps any such table must encode (they constrain
which keys are even bindable): GLUT delivers Ctrl+letter as control
bytes 1–26, so **Ctrl+H / Ctrl+I / Ctrl+J / Ctrl+M alias
Backspace / Tab / LF / CR** and cannot be used; and **Ctrl+Shift+<letter>
is indistinguishable from Ctrl+<letter>** at the byte level, so a Shift
binding must explicitly read `glutGetModifiers()` /
`editor_input_active_modifiers()` (as cfg pass A and the code-focus
router do).

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
* replay fade rendering now lives in the replay peer
  (`src/subsystems/replay/replay_render.c`); the scene calls it as a
  fade pass but no longer owns the GL code

Neutral scene modules such as `src/scene/grid.c`, `src/scene/axes.c`,
`src/scene/backdrop.c`, and `src/scene/lights.c` should remain free of REPL
state access. REPL-aware overlays now live under `src/scene/guides/`
and consume the explicit `SceneGuideSnapshot` rather than pulling globals
directly.

### Edit Overlays: polygon outlines on geometry

`src/subsystems/edit_overlays/edit_overlays.c` draws the "Vertex
outlines" (F7, `show_vertex_outlines`) and cursor "highlight current
polygon" (`highlight_current_poly`) overlays. `edit_overlays_render_outlines`
sets the shared overlay GL state once — lighting off, depth test on with
`glDepthMask(GL_FALSE)`, `glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)`, and
`glEnable(GL_POLYGON_OFFSET_LINE)` with the negative
`REPL_OUTLINE_POLYGON_OFFSET_{FACTOR,UNITS}` (pulls the wireframe in
front of the filled surface so edges read cleanly without z-fighting) —
then runs **three** walk passes over the flat program, each tracking the
modelview with `apply_tracked_transform` / `unwind_transform_stack`:

1. `render_outlines_glbegin_pass` — re-issues `glBegin`/`glVertex` blocks
   as line geometry from the REPL-tracked vertices.
2. `render_outlines_tess_pass` — the same for tessellated polygons
   (`gluTess*` contours), drawn as `GL_LINE_LOOP`.
3. `render_outlines_glut_pass` — for `glutSolid*` shapes.

These passes consume `OverlayWalkCtx.program`, populated once per frame from
`repl_state_flat_program_view()` by the controller's overlay snapshot pack.
With accumulation AA the outline hook runs once per jitter sample, so the flat
program walks repeat per sample, but the source interpreter, expression
evaluator, and flattener are not re-entered.

The third pass exists because **GLUT solid shapes emit no REPL-tracked
vertices** — the geometry is generated inside GLU/freeglut, so the first
two passes have nothing to trace. Instead, each shape is *re-drawn* under
the already-active `glPolygonMode(GL_LINE)` + polygon offset, letting the
GL pipeline rasterize the wireframe itself. The actual `glutSolid*` call
goes through `repl_executor_draw_glut_solid()` (shared with the live
render loop in `src/repl/executor.c`, so the dispatch stays in one place
and the GLUT-symbol call site stays inside the executor TU). The
membership predicate is `repl_cmd_is_glut_solid()` in `src/repl/command.h`
— the single source that also feeds `repl_cmd_starts_geometry_emit` and
`repl_cmd_consumes_current_color` (a new `glutSolid*` `CmdType` joins all
three at once; `test_is_glut_solid_predicate` in `tests/test_replay_walk.c`
pins the set). Cursor-on-the-line picks `SCENE_CLR_OUTLINE_ACTIVE` at a
thicker line; otherwise the standing outline uses `SCENE_CLR_OUTLINE_EDGE`.
Coverage: `test_draw_glut_solid_dispatch` (executor helper, stub counts)
and section 14b of `tests/test_repl_core_internal.c` (drives the full pass
and asserts the shape is redrawn + polygon-mode/offset toggled, gated on
`GL_STUBS`).

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
`src/ui/app/snapshot.h`) that the controller builds once via
`glr_ctrl_build_ui_snapshot()` and passes to every `ui_*_render*()`
entry point. Render code does not call `repl_state_*()` directly. The
`check-ui-no-repl-state-read` Makefile guard enforces the snapshot-shaped
signature for audited renderers.

`UiRenderSnapshot` carries:

* by-value value-type slices (code_panel, replay, search, autocomplete,
  status, …) — small structs cheap to copy. Note that
  scene-presentation policy and most render config now live **app-side**
  on `glr_state` (`src/app/glr_state.c`), not on `ReplRuntimeState`; the
  controller reads them from there when filling the snapshot. Only the
  REPL-owned render *tail* (`ReplRenderState`: per-light state + clear
  color) remains a REPL slice.
* pointer-shaped read-only views (`ReplVariableView`, `EditorInputView`,
  `ReplImportExportView`, `FlatProgramView`, `ReplPredefView`)
* document/flat metadata (`document_cmds`, `document_count`, `edit_line`
  — sourced editor-side via `editor_state_edit_line()`,
  `flat_program_count`, …)
* user-scene names + slot-used flags
* the controller-pushed editor snapshot pointers
  (`editor_transformers`, `editor_highlights`, `editor_virtual_lines`)
* per-frame derived metadata so the render path never re-derives:
  `selection_active / selection_lo / selection_hi`,
  `active_indent_chars`, `trailing_indent_chars`, `in_begin_block`,
  `current_begin_mode`

Slices that would have been heavy to copy are deliberately excluded:
`EditorClipboardState` (~1.88 MB with the lines sidecar) is not on the
snapshot — the per-row selection band reads `selection_lo/_hi` instead.

**Two selection models, one clipboard.** `selection_lo/_hi` above is
the *line-range* selection used by gutter drag and the multi-line
clipboard (`anchor_idx`/`end_idx` on `EditorSelectionState`). The
*input-buffer* selection is a separate character-range model on
`EditorInputState.anchor_pos`, scoped to the active edit row only
— shift+arrow, double-click word, drag-on-edit-row, and partial-line
copy/cut/paste all drive that anchor. The two share one tagged
clipboard object (`EditorClipboardState` carries an `EditorClipboardKind`
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
discoveries (e.g. the editor cursor pixel computed during the generic
text-panel pass) flow back through per-frame `Ui*Output` structs that
the controller actualizes after the render call (Phase J4 introduced
`UiCodePanelOutput`; the pattern is hard-guarded by
`check-output-actualization`). The previously-noted render-path live
reads have been converted: `ui_repl_code_panel_build_layout` now takes a
`const UiRenderSnapshot *` and is driven by the controller
(`glr_ctrl.c`), `ui_repl_code_panel_apply_follow_scroll` is gone, and
`replay_code_panel_get_command_display_text` takes an explicit
`SourceTextView` supplied by the controller's annotation-prep pass
rather than reading live state.

### UI Color Theming

All 2D UI chrome resolves color through `src/ui/core/theme.h` (header-only,
the `gl_2d.h` pattern) instead of scattered `glColor*` literals. It
defines ~19 semantic `UI_TOK_*` tokens and a
`g_ui_theme_table[UiTheme][UI_TOK_COUNT]` with six rows (green default,
plus warm / cyan / amber / violet / mono from the design-rework
bundle). Neutral chrome columns are identical across rows; only the
three accent-derived tokens (`UI_TOK_ACCENT`,
`UI_TOK_DROPDOWN_ITEM_HOVER_BG`, `UI_TOK_ACCENT_GLOW_BG`) vary per
scheme. Inline `ui_clr(tok)` / `ui_clr_a(tok, alpha)` set the GL color;
the token is a compile-time constant so the lookup folds to a fixed
`.rodata` read.

Color falls into three buckets:

1. **Theme token** — accent + shared neutral chrome (surfaces, borders,
   dividers, text tiers, hover/selection): `ui_clr(UI_TOK_*)`.
2. **Named constant** — fixed, non-theme one-offs that must keep their
   hue in every scheme: the blue inline-rename modal, the amber status
   banner, the ephemeral example-tab amber, the variable-row data
   palette, dim/stale text tiers, the `#000` menubar rule. A local
   `static const` documented at the use site.
3. **Left as-is** — computed/domain palettes that must not follow the
   accent: `color_picker.c` HSV math, `repl_code_panel.c`
  syntax-highlight palette, the `cpuprof.c` FPS gauge (red must
   keep meaning "over budget"), `text_panel.c` `k_clr_*` editor
   sub-palette. Each carries a one-line pointer back to theme.h.

**Selecting the scheme.** `UI_THEME_DEFAULT` in `config.h` is the
single compile-time knob: a bare integer (`0` green … `5` mono — kept
type-free so `config.h` stays clear of UI types per its dependency
note) used to initialize `g_ui_theme`. It is `#ifndef`-guarded and
build-overridable, e.g. `make gl-repl CPPFLAGS=-DUI_THEME_DEFAULT=1`;
`theme.h` `STATIC_ASSERT`s the value is in range against the `UiTheme`
enum. The `ui_theme_select()` / `ui_theme_active()` seam keeps call
sites stable for a future runtime switcher (e.g. a `GlrConfigKey`
cycle) that would relocate the active index into one `.c` TU.
`tests/test_ui_theme.c` (header-only) guards table integrity: no
zeroed token, neutral tokens stable across rows, green accent ==
`#6fb36f`, and the dropdown hover is green (the Issue-1 regression).

## Replay Architecture

Replay is REPL-owned. The scene may render the current visual effect, but it
should not own replay policy.

R1 target from `done/push-architecture-refinement.md` (landed):

* controller builds a `ReplayFadePlan` snapshot once per frame (batches,
  alpha, skip limits, baseline predef values)
* scene iterates the snapshot and owns the GL pass orchestration without
  calling `replay_*` or `repl_state_*`
* accumulation-AA settings are `SceneRenderConfig` fields set by the controller
* 2D replay HUD lives in `src/ui/subsystems/replay_hud.c`, driven by config fields
* `scene_*.c` files contain no `repl_state_*` or `replay_*` calls; once
  the relevant Phase 2 slice is complete, Makefile checks keep that true

## Boundary Rules

### Live OpenGL / GLU calls

Allowed:

```text
scene_*.c
ui_*.c
src/repl/executor.c
gl_repl.c        GLUT/window lifecycle and buffer swap; future `glr` shell
```

Avoid live GL calls in all other `repl_*` files. Text emission of GL command
names in parser/export/example/spec code is not a live GL call.

### GLUT calls

Allowed:

```text
gl_repl.c        GLUT callback registration, glutInit, buffer swap
                (the future `glr` shell takes over after the R8 rename)
src/app/glr_ctrl.c      GLUT modifier reads + cross-layer input routing
                (took over from the deleted repl_editor.c in Phase J1)
editor_input.c  glutGetModifiers via editor_get_modifiers (gated behind
                editor_input_enable_glut_modifier_reads so tests stay safe)
src/repl/executor.c GLUT solid shapes (glutSolidCube/Sphere/Torus/Teapot/Cone)
                and glutBitmapCharacter for label() text. (Its GLU
                tessellator setup — gluNewTess/gluTessCallback — is GLU,
                not GLUT.) The glutSolid* call site is centralized in
                `repl_executor_draw_glut_solid()`; the edit-overlays
                outline pass re-draws shapes through that helper rather
                than naming GLUT symbols itself.
```

### Controller-only scene wiring

After controller extraction, ordinary `repl_*` model files should not include
`scene_*.h`. `src/app/glr_ctrl.c` is the scene/UI frame-rendering exception.
`check-controller-boundaries` enforces this; cross-layer constants used by
both layers (e.g. `CFG_DEFAULT_MULTISAMPLE`, `REPL_OUTLINE_POLYGON_OFFSET_*`)
live in neutral headers (`src/app/glr_defaults.h`, `config.h`,
`src/scene/render_types.h`) that both sides include via existing transitive
paths.

There are no remaining `ui_*` include exceptions among `repl_*` model
files. `src/repl/export.c` is UI-free — it pulls app/scene-derived
values only through controller-installed bridges, guarded by
`check-repl-export-no-ui-layout` and `check-repl-export-via-bridge`.
The former `repl_actions.c` now lives at `src/app/glr_actions.c`, an
app-shell file that may legitimately include `ui_*` headers (so it is
not a boundary exception). The `repl_editor.c` exception is gone — that
file is deleted (Phase J1).

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

builds and runs **without** `src/app/glr_ctrl.c`, `src/editor/*`,
`src/ui/*` renderers, or any app-owned state — and, since the decoupling
landed, with a **stub-free link boundary**.

### Status: 17 → 0 stubs (complete)

`feature/decouple-repl-from-gl-repl-alt.md` (7 steps) and
`feature/source-document-port.md` (8 phases) both shipped.
`tools/repl_demo/stubs.c` — once a dependency ledger of 17 externally
visible symbols — is now **empty** (a documentation-only, intentionally
non-empty TU). It stays in the build as a *canary*: adding a new stub
there is the visible signal that a REPL-pipeline TU has acquired a fresh
app/editor/UI symbol dependency. `check-repl-demo-stubs-shrinking`
ratchets the count and `check-repl-demo-no-editor` forbids editor/UI/app
symbols in the demo link set.

The current `REPL_DEMO_DEP_SRCS` link set is REPL-pipeline plus peer replay
annotation support:
`src/repl/*` (parser, command_store, compile, apply, flatten, executor,
eval, export, scenes, example_loader, load, autonormal, command_spec,
source_scope, core, state, format) plus `src/subsystems/replay/replay_annotations.c`,
the replay / tutorial peer-state TUs, `src/support/cpuprof.c`, the GL stub counters, and crucially
**`tools/repl_demo/source_document.c`** — the editor-free backend for the
source-document port. No `src/editor/*`, no `src/ui/*`, no `src/app/*`.

### The four boundary mechanisms that achieve zero stubs

Every former stub was an edge from the REPL pipeline into another owner.
Each was cut by routing the dependency through a neutral seam that the
full app fills and the demo leaves unset:

1. **Source-document port** (`source_document.h`). Source-text reads /
   mutations go through `source_document_*`; the full app links
   `glr_source_document.c` (→ `EditorState`), the demo links
   `tools/repl_demo/source_document.c`. This cleared `editor_feed_line` /
   `editor_load_line_to_input` from the pipeline (alongside
   `repl_load_apply_line`, below) and let `src/editor/state.c` *leave* the
   demo link set entirely. See *Editor-owned text* above.

2. **`ReplHostEffects` bridge** (`src/repl/core.h`). A single
   controller-installed table of host callbacks — `status`,
   `status_error`, `example_presentation_reset`, `input_reset`,
   `insert_mode_off`, `scroll_to_line`,
   `tutorial_teardown`, `edit_line_get`, `edit_line_set`. Pipeline TUs call
   `repl_set_status()` /
   `repl_dispatch_*()`; the controller installs the table at startup. The
   demo installs only its edit-line hooks and leaves the status/editor/
   tutorial hooks unset, so those dispatchers are no-ops. This consolidated
   the old per-effect installers (the `set_status` sink, the autocomplete
   registration, the UI-chrome sync) into one struct and cleared
   `ui_state_status_set`, `ui_state_code_panel_mut`, the reset stubs, and
   the former `tutorial_teardown` demo stub.

3. **Export bridges + layout input** (`src/repl/export.h`). `export.c` is
   GL-free and app-free; app/scene-derived values arrive through
   controller-installed bridges: `ReplExportConfigBridge` (`@cfg`
   emission/parse — also fronted by the typed live-cfg wrappers
   `repl_cfg_get_int` / `_set_int` / `_known` and the
   `repl_export_extract_cfg_slug` parser in `src/repl/export.c`, used by
  `src/subsystems/tutorial/tutorial_runner.c` for SET-step apply / REQUIRE-step probe /
   cfg-baseline snapshot/restore), `ReplExportCameraBridge` (camera
   blocks — used by both the importer *and* the example loader),
   `ReplExportProjectionBridge` (the dynamic `reshape()` body — see
   *Dynamic Reshape Projection*), and the `ReplExportLayout` struct
   (viewport / code-panel geometry passed as an explicit export input
   instead of calling `ui_layout_*`). The demo installs none, so `@cfg` /
   camera / projection are no-ops there and `src/app/glr_config.c`,
   `src/app/glr_camera.c`, `src/ui/core/layout.c` all leave the demo link set.
   `check-repl-export-via-bridge` / `check-repl-export-no-ui-layout`
   guard this.

4. **Split lifecycle reset + dispatcher relocation.**
   `repl_state_reset_program()` (REPL-only) is separated from
   `glr_ctrl_reset_all()` (full-world, in `src/app/`), and
  `repl_compile_dispatch()` moved out of the former editor service shim into
   `src/repl/compile.c`. Pure structured-block validators were extracted
   from the editor compile wrappers, and the non-editor
   `repl_load_apply_line()` (`src/repl/load.c`) replaced `editor_feed_line`
   on the example/import/tutorial paths.

### App-frame state moved out of the REPL

Step 7 of the decouple plan relocated the scene-presentation policy and
most render config out of `ReplRuntimeState` into the app-side owner
`src/app/glr_state.c` (`glr_state.h`). REPL-pipeline TUs do not include
`glr_state.h` (`check-repl-state-no-glr-state`); app / editor / UI / scene
code may. Only the REPL-owned render *tail* (`ReplRenderState`: per-light
state + clear color) stayed a REPL slice.

### Guards that keep the boundary closed

All in the `check-state-ownership` gate: `check-repl-demo-no-editor`,
`check-repl-demo-stubs-shrinking`, `check-source-document-port-owners`,
`check-repl-no-direct-editor` (invariant β), `check-repl-no-direct-buffer-read`,
`check-no-store-text-api`, `check-no-feed-line-in-pipeline`,
`check-no-load-line-to-input-in-pipeline`,
`check-repl-no-direct-tutorial-runner`, `check-repl-state-no-glr-state`,
`check-repl-export-via-bridge`.

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
* New app lifecycle/window wiring: `gl_repl.c` for now, future `glr` shell
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
`src/repl/command.h`; `gl_repl.h` only re-exports it transitively via
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
`src/app/glr_completion.c` and `src/repl/help_text.c` both read this table. If the
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

### 4. `src/subsystems/replay/replay_annotations.c` — replay display format

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
source-document line text (`source_text_line(view, cmd_idx)` via the
neutral port — `GLCmd.source[]` was removed in the editor-owns-text
refactor, and `export.c` no longer reaches into `EditorState` directly)
verbatim into the exported `display()` body, and `repl_export_load_from_file`
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
make gl-repl           # must be clean (no new warnings)
make test-stubs       # all tests must pass
make gl-repl USE_GL_STUBS=1   # verify stub build still links if step 6 changed

# Spot-check the new command end-to-end:
# - F1 overlay shows it with description in the expected group
# - Type the prefix; Tab fills the rest and the parameter hint shows
# - Replay (Ctrl+G) shows the command annotated correctly
# - Save (Ctrl+S) → reload (./gl-repl output.c) → command appears identical
# - For commands with a custom export helper: gcc -c output.c against
#   vanilla freeglut succeeds, and on-screen output matches the REPL
```

## Adding A New Tutorial Step Kind

Tutorial step kinds (`TutorialStepKind` in `src/repl/tutorials.h`) name the
contract between a catalog entry and the runtime: what extra fields the
step carries, what UI it shows, what user action advances it, and which
guard rails apply. As of 2026-05 the shipped kinds are:

| Kind          | Carrier fields              | Advance signal                            |
|---------------|-----------------------------|-------------------------------------------|
| `COMMAND`     | `expected`                  | User commits a line matching `expected`.  |
| `SET`         | `cfg_slug`, `cfg_value` (`_name`) | Ack key (Enter / Tab / Space) after auto-apply. |
| `REQUIRE`     | `cfg_slug`, `cfg_value` (`_name`) | Live cfg slug matches target (notify from `glr_config_set`). |
| `REQUIRE_VAR` | `var_name`, `var_target`    | Live predef variable matches target (notify at the tail of the predef-writeback commit, after editor post-effects). |

Use this section as a checklist when adding a new kind. The
`REQUIRE_VAR` rollout is the most recent worked example — its commits
land in roughly the order below.

### 1. Catalog layer (`src/repl/tutorials.{h,c}`)

- Add the new enum value to `TutorialStepKind`.
- Add any extra fields to `TutorialStep`. **Place new fields AFTER all
  existing ones** so positional initializers in `STEP_APPEND` / `STEP_AT`
  / `STEP_SET` / `STEP_REQUIRE` / `STEP_SENTINEL` keep zero-initializing
  to the new defaults — adding fields mid-struct silently shifts other
  fields' values.
- Add a `STEP_<KIND>(label, comment, ...)` macro for catalog authors.
  Keep the same layout as the sibling macros and zero-init unused
  trailing fields explicitly so a `// CHECK(...)`-style audit confirms
  the row's intent at a glance.
- Extend the per-field accessor shims at the bottom of the header
  (e.g. `repl_tutorial_step_var_name`). Each shim is one walk through
  `repl_tutorial_step_get`; bulk readers (the menu paint path) hold the
  step pointer directly to avoid O(N²) walks.
- Extend `repl_tutorial_validate_entry` (in `tutorials.c`) with a kind
  branch. Enforce the new shape rules: which carrier fields must be
  non-empty, which existing fields must be NULL (e.g. `expected == NULL`
  for non-COMMAND kinds), and reject reserved / structurally-bad
  values up front. The validator runs before any state mutation in
  `tutorial_start`, so a malformed step cannot leave a half-applied
  transient scene.

### 2. Runner (`src/subsystems/tutorial/tutorial_runner.c`)

- Add a `tutorial_<kind>_matches_target(...)` predicate. For
  cfg-shaped kinds it reads via `repl_cfg_get_int` / `_resolve_text`;
  for variable-shaped kinds it reads via
  `repl_eval_predef_view()` after a `repl_eval_find_predef_var_idx`
  lookup. Apply any tolerance (e.g. `TUTORIAL_VAR_EPS` from
  `src/subsystems/tutorial/tutorial.h`) here so the boundary policy
  lives in one place.
- Add `tutorial_set_status_<kind>(...)` mirroring the existing
  `tutorial_set_status_require` / `_ack_set` helpers. Keep the prose
  describing *both* satisfaction paths if the kind has more than one
  (the REQUIRE_VAR message names typing AND the slider).
- Add `tutorial_enter_step_<kind>(idx, step, instruction_line, state)`.
  Mirror the existing helpers: set `expected_commit_line` (use `-1`
  unless the kind pins a typing row), return `TUTORIAL_STEP_AUTOADVANCE`
  if the target is already satisfied on entry, otherwise park the
  cursor past the instruction comment, emit the status hint, refresh
  autocomplete, and return `TUTORIAL_STEP_PAUSED`.
- Add a `case` to the switch in `tutorial_enter_step`. The validator
  is supposed to keep unknown kinds out, but the default branch still
  calls `tutorial_teardown()` for safety.
- Extend `tutorial_notify_state_changed`. Read the current step's
  kind and dispatch to the matching predicate; advance via
  `tutorial_advance_step(...)` on match. The function takes no args,
  so the call sites that fire it (see below) don't need to know what
  changed — only that *something* did.
- If the new kind allows typed commits to mutate the document (like
  REQUIRE_VAR), update `tutorial_guard_source_change` to whitelist it
  alongside `TUTORIAL_STEP_KIND_COMMAND`; otherwise the
  freeze-during-non-COMMAND rule will reject every keystroke.
- If the kind has no expected-commit-line dance (REQUIRE / REQUIRE_VAR),
  the commit-side advance must stay a no-op for it: the notify hook is
  the authoritative advance. The mechanism is `tutorial_advance_if_commit_ok`
  (in `src/editor/input.c`), which advances ONLY when
  `tutorial_note_expected_commit_applied()` returns 1 — i.e. a pending
  COMMAND expected-command attempt was in flight. A free-form REQUIRE_VAR
  commit sets no pending record, so it returns 0 and the commit path does
  not advance. This is load-bearing: the REQUIRE_VAR notify fires *inside*
  the commit and may advance onto a COMMAND step; a second commit-side
  advance would then skip that COMMAND step entirely (instruction comment
  shown, command never typed). Checking the current step kind instead of
  the return value is NOT sufficient — by the time the commit returns the
  step is already the next (COMMAND) one. (`tutorial_advance_after_successful_commit`
  also keeps an internal REQUIRE_VAR bypass for direct callers / safety.)
- If the kind needs a *runtime-derived* entry variant, decide it in
  `tutorial_enter_step` before emitting the instruction comment.
  REQUIRE_VAR does this: a step whose `var_name` does not exist yet is
  a DECLARATION step. The satisfying `float name = ...;` is a decl,
  which the compiler relocates to the document top (above any comment —
  see `compile_insert_pos` / `decl_pos`), so a separate locked instruction
  comment line above it would be stranded and desync locked-line tracking.
  The runner therefore skips `tutorial_emit_instruction_comment` for
  declaration steps; the instruction instead rides the autocomplete ghost
  as `float name = target; <catalog comment>` (synthesized in
  `tutorial_shadow_suffix`), so the catalog comment commits as a TRAILING
  comment on the decl line and travels with it to the top — no separate
  comment to track. The cursor parks on the trailing row
  (`instruction_line`, since nothing was inserted) rather than
  `instruction_line + 1`. The tutorial does NOT pre-declare its
  variables — the user declares them, which is the point of the step.

### 3. Notify call sites

The notify hook is `tutorial_notify_state_changed(void)`. New kinds
share the function — they only add new *call sites* where the watched
state can change. Today's sites:

- **`src/app/glr_config.c::glr_config_set`** — fires for cfg-slug-shaped
  kinds (SET / REQUIRE).
- **`src/editor/commit.c::notify_tutorial_if_predef_changed`** — fires
  once if any predef op landed in the commit. Covers REQUIRE_VAR for
  typed `name = expr;` assignments, `float n = 5;` declarations-with-
  initializer, and variable-panel slider writebacks (the slider flows
  through `editor_commit_apply_external_change`; typed commits flow
  through `editor_commit_apply_plan`). Both entry points call it at
  their TAIL — after `apply_post_effects` and the status publish — NOT
  from inside `apply_compiled_change_full`. The notify can advance the
  step, which inserts the next instruction comment and re-parks the
  cursor; firing it mid-apply let the in-flight commit's own
  `cursor_target` clobber that re-park afterward, stranding the cursor
  on the freshly-inserted locked comment (read-only + the empty input
  overlay hid the comment).

If a new kind watches state that no existing site touches, add a
single call to `tutorial_notify_state_changed()` at the writeback
chokepoint for that state, AFTER any cursor/post-effect bookkeeping the
same operation performs, so the advance sees a settled document.

### 4. Editor-side input precheck & ghost text

- `src/editor/input.c::tutorial_precheck_current_input` is the
  `;`/Enter route's gatekeeper. It assumes COMMAND semantics by
  default (matched commit must land on `tutorial_expected_commit_line`).
  Add a short-circuit `return 1` for the new kind early in the
  function (after the empty-input and noncommand-reject checks) when
  the kind allows free-form commits — without this, typed commits get
  blocked with "Move cursor to the tutorial insertion line".
- `tutorial_reject_noncommand_commit_with_hint` (in
  `tutorial_runner.c`) decides whether a typed commit attempt is
  hard-rejected with a kind-specific hint. Return 1 for kinds that
  forbid typed commits (SET / REQUIRE) and 0 for kinds that allow
  them (COMMAND / REQUIRE_VAR).
- `tutorial_handle_ack_key` consumes Enter / Tab / Space only when
  the current step is SET. Extend if a new kind also wants ack-key
  advancement.
- For ghost text: `tutorial_shadow_suffix` returns the untyped
  portion of `expected`. If the new kind has no fixed `expected` but
  still benefits from passive guidance, synthesize an expected string
  at the top of the function and let the existing strict-prefix logic
  produce the suffix. REQUIRE_VAR synthesizes `float name = target;
  <catalog comment>` while the variable is still undeclared — the
  declaration the user must type, carrying the step's catalog comment as
  a trailing comment so it lands as a code comment on the relocated decl
  line — and the bare `name = target` once the variable exists. The same
  declared-ness test gates whether the runner emits a separate
  instruction comment (it does not, for the declaration form).
- `src/app/glr_completion.c::update_autocomplete` only computes the
  shadow when `edit_line == tutorial_expected_commit_line()`. If the
  new kind has no pinned commit line, add a kind-specific branch that
  also enables the shadow path (REQUIRE_VAR sets `show_tutorial_ghost`
  unconditionally while its step is active).

### 5. Catalog content

Ship at least one tutorial that exercises the new kind so the menu
flyout, instruction-comment fade, and notify wiring are all exercised
in production — purely synthetic test fixtures miss the menu, the
status hint, and the autocomplete provider. Adding a tutorial means:

- Pick a tag (`TUTORIAL_TAG_*`) and a subheading consistent with the
  per-tag contiguous-runs invariant enforced by
  `test_catalog_subheading_metadata` (see CLAUDE.md's "Tutorials Menu"
  section). The simplest path is to place the new entry between two
  same-subheading entries in catalog order.
- The catalog validator `expected_is_single_command` rejects
  `float ...;` declarations in a COMMAND `expected` (CMD_VAR_DECLARE is
  relocated to the top of non-decl code by `editor_try_commit_float_decl`,
  breaking `pending.commit_line`). To teach a declaration like
  `float n = 5;`, use a REQUIRE_VAR step whose variable does not exist
  yet (the "Variable Slider" tutorial's step 0): the runner detects the
  undeclared var and treats it as a declaration step (no separate locked
  comment; the instruction rides the ghost as a trailing comment on the
  decl line — see §2), and the typed decl-with-initializer satisfies the
  target via its DECLARE predef op. Word the step's catalog comment to
  read as a trailing description of the variable (it commits as
  `float n = 5; <that comment>`), not as a standalone "type ..." line.

### 6. Tests (`tests/test_tutorial_runner.c`)

Add at least:

- Validator acceptance + per-rule rejection (missing carrier, wrong
  shape, reserved-name collisions). Use designated-initializer
  `TutorialStep[]` fixtures and call `repl_tutorial_validate_entry`
  directly — no need to enter the runner.
- Auto-advance on already-satisfied entry (or, if the kind cannot
  be pre-satisfied through the test fixture, an indirect proof that
  the runner *pauses* on the next step rather than advancing through
  it — see `test_require_var_pauses_on_next_step_after_match`).
- Each satisfaction path the kind supports (typed commit, slider drag,
  F-key, …). For each, drive the live entry point (`editor_handle_key`,
  `glr_ctrl_router_*`, `glr_config_set`) rather than the runner internals
  so the wiring at the notify chokepoint is exercised end-to-end.
- Negative case: an unrelated change that should NOT advance.
- For floating-point or tolerance-keyed predicates, an explicit
  epsilon-boundary case sized to the production constant so a future
  tune surfaces here rather than silently changing UX.
- If you added ghost text: empty-input full-ghost case + strict-prefix
  suffix case.

### 7. CLAUDE.md

Update the `## Tutorials Menu` and tutorial file-layout descriptions
in `CLAUDE.md` to mention the new kind. The validator drift test
already requires non-zero `.tags`, but any new step-shape invariant
(e.g. "REQUIRE_VAR var_name must be non-reserved") needs a one-line
mention so future catalog authors don't rediscover it from a test
failure.

## Open Refactor Edges

Completed (Phase 1 + most of Phase 2):

- ✅ Controller extraction, explicit `SceneRenderConfig` handoff,
  focus/guide snapshot construction, scene-local accumulation jitter, and
  app-shell shim removal (`gl_repl.c` calls `glr_ctrl_*` directly).
- ✅ **R1** — Replay/HUD migration: controller builds `ReplayFadePlan`; scene
  iterates it; 2D HUD lives in `src/ui/subsystems/replay_hud.c`. Scene files contain zero
  `replay_*` and `repl_state_*` calls.
- ✅ **R2** — UI → REPL mutation holes closed end-to-end:
  - `src/ui/app/panels.c` is hit-test only (`check-ui-panels-no-mutators`).
  - The color picker now lives across `src/subsystems/color_picker/color_picker_state.c` (peer state +
    lifecycle + writeback through `editor_commit_apply_external_change`)
    and `src/ui/subsystems/color_picker.c` (pure renderer + hit-test over a
    `ColorPickerView`); the picker UI carries no live state reads, no
    parser/compile/apply, no `set_status`. Locked in by
    `check-color-picker-ui-isolation`.
  - The legacy `ui_help_overlay` is gone — split into the generic
    `src/ui/core/tabbed_overlay.c` renderer (knows nothing about REPL), the
    REPL-side `src/repl/help_text.c` producer that walks
    `k_func_completions[]` to assemble the F1 overlay's per-command
    rows, and the `glr_ctrl` adapter that maps that neutral data to
    `UiOverlayContent`. Adding a new GL command + `help_desc` + `help_group` to the
    spec entry now auto-populates F1.
  - All `ui_*.c` files have zero `_mut()` calls
    (`check-ui-returns-hits-only` baseline 0/0).
- ✅ **R3** — layout geometry (`ui_layout_scene_rect` /
  `ui_layout_code_panel_rect`) owns its own module; it has since settled at
  `src/ui/core/layout.c` / `src/ui/core/layout.h`, and `repl_export` takes
  viewport/panel geometry as an explicit `ReplExportLayout` input rather
  than calling the layout helpers (see *Standalone REPL Demo Coupling*).
- ✅ **R4** — `src/app/glr_ctrl.c` no longer includes `src/repl/core_internal.h`;
  `src/repl/pipeline.h` exists; `repl_eval_predef_view()` hides
  `g_predef_vars`. R4d (public-API audit) landed; `bench_repl.c` no
  longer includes `src/repl/core_internal.h` either, so no non-test/REPL
  TU pulls it.
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

- ✅ **R10-phase1** — Phase J1 obsoleted the original framing
  (`repl_editor.c/h` deleted). `src/repl/core.h` no longer carries any
  GLUT-flavored input-dispatch declarations; its remaining cross-module
  entry points are the neutral `repl_dispatch_*` host-effect hooks, which
  belong there. Nothing left to relocate.
- ❌ **R10-phase2..phase5** — Dissolve `src/repl/core.c` (~896 lines; it
  grew with the `ReplHostEffects` install/dispatch surface): move
  `repl_parse_and_normalize*` / `normalize_with_indent` /
  `parse_and_normalize_impl` to `src/repl/parser.c`; move `collect_visible_vars`
  to `src/repl/source_scope.c`; finish the reformatter split (the editor-side
  wrapper `editor_reformat_commands` already lives in `src/editor/reformat.c`,
  but the pure `repl_reformat_program()` pass still sits in `core.c`); move
  `load_initial_commands` / `scroll_to_display_function` to `src/repl/scenes.c`;
  move `current_begin_mode` / `count_vertices` to `src/repl/executor.c`; move
  debug dumps to `src/repl/state.c` or `repl_debug.c`. The `ReplHostEffects`
  install/dispatch layer is a candidate for its own small TU.
- ✅ **R11 (tail)** — The `bench_repl.c` `src/repl/core_internal.h`
  exception is gone (bench no longer includes it); no surviving
  allowlist of that shape remains.
- ❌ **R12** — Consolidate truly public REPL APIs into one concise public
  header, grouped by implementation owner; keep internals out.
- ❌ **R8** — Rename `gl_repl.c` / `gl_repl.h` into the `glr_*` shell
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

* `GLCmd.source[]` removed; per-line text owned by `EditorBuffer`.
* Parser returns `ReplParsedLine`; commit-store APIs are text-aware.
* Text sidecars added to undo snapshots, user scenes, and clipboard.
* Color picker rebuilt as a controller-pushed `UiTransformer` with
  store `replace_one(... line)` writeback (no more `set_color` API).
* Cross-line `UiHighlight` snapshot replaces inline `repl_find_feeding_*`
  calls in render.
* Replay annotations move from inline row injection to controller-pushed
  `UiVirtualLine` rows; layout / scroll / hit-test / render share one
  source-of-truth count.

The deferred sub-task is the color-scheme + syntax-keyword extraction
(also Step 6); revisit when a configurable theme has a real consumer.

Three further decoupling tracks have since landed and are reflected
throughout this document:

* **`feature/edit-line-ownership.md`** — the document cursor moved from
  `ReplState` to `EditorState.document.edit_line_idx`; the REPL pipeline
  takes edit-line as an explicit parameter or via the `ReplHostEffects`
  `edit_line_get`/`_set` hooks. See *Document cursor ownership* above.
* **`feature/source-document-port.md`** — REPL source-text access is now
  the neutral `source_document.h` port; `glr_source_document.c` backs it
  in the full app and `tools/repl_demo/source_document.c` in the demo. See
  *Editor-owned text* above.
* **`feature/decouple-repl-from-gl-repl-alt.md`** — `repl_demo` reached a
  stub-free link boundary (17 → 0); the later tutorial teardown edge was
  routed through `ReplHostEffects` to keep that boundary at zero. See
  *Standalone REPL Demo Coupling* above. Step 7 also relocated app-frame
  presentation/render policy out of `ReplRuntimeState` into
  `src/app/glr_state.c`.

## Known REPL Corner Cases & Coverage Gaps

The REPL pipeline has a handful of corner cases that deserve focused
regression tests. Each item below points at the load-bearing code so future
work can either close the gap or document the intentional behaviour.

### Documented but uncovered

- **Func alias slot exhaustion.** `editor_try_commit_func_def` (in
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

## Historical Decoupling & Host Bridges

To preserve structural separation and keep the REPL compiler and pipeline engine (`src/repl/`) entirely decoupled from visual rendering and the host editor environment (`src/app/`, `src/editor/`), the project utilizes explicit bridge interfaces (`ReplHostEffects`, `ReplExportLayout`, `glr_actions_install_export_cfg_bridge`, etc.) installed by the frame controller during initialization:

### 1. App-Service Bootstrapping
Dump-only CLI paths (e.g., `--dump-code` and `--dump-flat`) bypass normal OpenGL initialization (`glr_ctrl_init_gl`), but they still need to load and export REPL state correctly. This requires the idempotent `glr_ctrl_install_app_services()` installer to execute prior to loading commands to avoid dropping `@cfg` blocks during import.

### 2. Host-Effect Bridges
The host-effect bridge (`ReplHostEffects`) installed by the controller routes core pipeline actions (such as status updates, example resets, input resets, scrolling, follow-scroll, and cursor parking) back to the UI, editor state, and peer subsystems. This prevents core REPL code from directly linking editor or tutorial symbols, allowing alternative drivers (like `tools/editor_demo`) to link successfully with minimal stubbing.

### 3. Export Bridges
* **Config Bridge:** Installed via `glr_actions_install_export_cfg_bridge()` so the exporter and scenes modules can read, write, and parse scene-specific `@cfg` configurations without referencing visual/controller configurations directly.
* **Camera Bridge:** Installed via `glr_camera_export_install_bridge()` so the exporter can serialize the current camera 3D pose (`// camera` blocks) without coupling core files to the camera state module (`glr_camera.c`).
* **Reshape-Projection Bridge:** Allows the exporter or code-panel geometry calculations to query perspective or orthographic viewing projections dynamically without hard-coding OpenGL matrix operations.
* **Camera-Distance Source:** Injects the current camera distance into the command executor so that the dynamic point-attenuation fallback (scaling `glPointSize` manually when `glPointParameterfv` is unsupported) can function without linking `glr_camera.c` into the executor's link set.

### 4. Global State Reset Separation
The controller side owns `glr_ctrl_reset_all()`. This ensures that when a wholesale replacement or program load occurs, the editor, UI, and peer subsystems are cleared simultaneously with the core REPL document state, maintaining strict visual and behavioral parity.
