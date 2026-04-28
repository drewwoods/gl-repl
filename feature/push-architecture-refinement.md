# Plan: Extract a Controller Layer (Option B, Phase 1)

## Context

The immediate-mode REPL codebase has grown large enough that "where does this
belong?" is getting answered ad hoc. The earlier push-architecture plan
([`feature/push-architecture.md`](push-architecture.md)) tried to fix this by
introducing generic scene callbacks, a separate `ReplGeometryRenderPlan`, and
renaming three transitional `scene_*` files to `repl_geometry_*`.

After review, that approach is too much framework boilerplate for a
one-frontend sample. It adds callback indirection, duplicates fields between a
scene config and a parallel REPL plan, and forces file moves on overlay code
that has one caller.

This plan is the desired direction: **Option B**, a controller layer between
`repl_*` and `scene_*`/`ui_*`. The useful isolation is REPL model/controller vs.
rendering views, not a pure scene plugin boundary. Scene modules may consume
`FlatProgramView`, `CmdType`, and command provenance when those values arrive
through per-frame configs/snapshots. They should not rebuild configs from REPL
globals or mutate REPL state directly.

Phase 1 focuses on peeling frame orchestration and scene config construction out
of `repl_core.c` into a new controller translation unit named
`imrepl_ctrl.c`. The current `sample.c` / `sample.h` app-shell names are left
alone in this phase; renaming them to `imrepl.c` / `imrepl.h` is a later
mechanical cleanup. Replay fade simplification and broader input/UI coupling
cleanup remain follow-ups; the 2D replay HUD relocation is complete.

Implementation status: Phase 1 steps 1-8 are complete and merged. Post-Phase 1
cleanup (app-shell shim removal) has also been completed. Phase 2 is in active
progress as of 2026-04-28:

- ✅ R1 (Replay/HUD migration): Complete
- ✅ R2 (UI → REPL mutation hole): Complete
- ✅ R3 (Extract layout geometry): Complete
- ⚠️ R4 (Controller off repl_core_internal.h): Partially complete
  - R4a/R4b complete; R4c narrowed to tests + `repl_` implementation code; R4d pending
- ❌ R5–R12: Not started

Some pieces have landed, but the strict end-state checks described below should
not be assumed to pass until their prerequisite Phase 2 slices are complete.

## Review Corrections

This document incorporates the gaps found while comparing it with
`ARCHITECTURE.md`, `MODULES.md`, and the current source:

- The superseded plan is `feature/push-architecture.md`, not this file.
- `ARCHITECTURE.md` and `MODULES.md` now present Option B as the target.
- The config-builder move and explicit `scene_render_3d_scene(&cfg)` signature
  are one step, avoiding a temporary `scene_render.c -> imrepl_ctrl.h` dependency.
- Phase 1 keeps the existing narrow `SceneExecuteProgramFn` execution hook; it
  does not add the older generic `SceneUserDraw` callback model.
- `scene_render_types.h` is no longer "unchanged": focus/guide snapshots move
  through config, and scene-local jitter removes config jitter fields.
- Boundary checks account for existing UI include exceptions in
  `repl_editor.c`, `repl_actions.c`, and `repl_export.c`.
- Replay fade batches remain an explicit follow-up, so Phase 1 does not pretend
  every direct `repl_replay_*` use disappears immediately.

## Revised Tenets

1. **REPL does not own scene decoration or UI.** The REPL parses, stores,
   flattens, replays, exports, and owns user-geometry execution through
   `repl_executor.c`.
2. **Scene owns the 3D stage, not the editor.** Scene sets viewport,
   projection, camera, clear, accumulation, baseline lighting, grid, axes,
   backdrop, light indicators, orbit target, and 3D overlay passes from config.
3. **UI owns 2D editor chrome.** Code panel, menus, popups, color picker, help,
   profile HUD, status banner, and replay HUD live in `ui_*`.
4. **The controller wires the frame.** `imrepl_ctrl.c` builds scene/UI inputs from
   REPL state, calls `scene_render_3d_scene(&cfg)`, then drives UI rendering.
5. **No generic scene plugin callback.** Phase 1 keeps the current narrow
   execution hook (`SceneExecuteProgramFn`) to minimize churn. A later cleanup
   may replace it with a direct `repl_execute_program()` call if that is simpler.
6. **Live GL allowlist stays small.** `scene_*.c`, `ui_*.c`,
   `repl_executor.c`, and `sample.c` in Phase 1. After the app-shell rename,
   `imrepl.c` replaces `sample.c` in that list.
7. **REPL-aware overlays stay in `scene_*`.** `scene_overlays.c`,
   `scene_geometry_guides.c`, and `scene_transform_guides.c` remain in place.
   They consume `SceneRenderConfig`, `SceneGuideSnapshot`, `FlatProgramView`,
   and cursor/source snapshots built by the controller.

## Scope

In scope:

- Create `imrepl_ctrl.c` / `imrepl_ctrl.h`, deliberately outside the `repl_*`
  namespace.
- Move display, reshape, GL-init controller wiring, and scene config building
  out of `repl_core.c`.
- Change `scene_render_3d_scene(void)` to
  `scene_render_3d_scene(const SceneRenderConfig *config)`.
- Stop `scene_render.c` from rebuilding `SceneRenderConfig`.
- Move focus vertex and guide snapshot assembly out of `scene_render.c` and
  into the controller-built config.
- Make accumulation jitter scene-local instead of writing to
  `repl_state_render_mut()->accum_jitter_*`.
- Keep clear color resolution in scene code, applied once before the accum
  loop by reading `config->flat_program`.
- Add boundary checks that match the actual Phase 1 scope.
- Update architecture/module docs to match Option B.

Out of scope:

- Replay fade-ring simplification and baseline-restore redesign.
- Full `SceneRenderConfig` slimming after replay/HUD cleanup.
- Removing existing UI-layout include exceptions from input/action/export code.
- Renaming `sample.c` / `sample.h` to `imrepl.c` / `imrepl.h`.
- Renaming `scene_overlays.c`, `scene_geometry_guides.c`, or
  `scene_transform_guides.c`.

## Critical Files

| File | Role in this plan |
|------|-------------------|
| `repl_core.c` | Source for legacy display/reshape/init wrappers and thin forwarding hooks. Per-frame orchestration now lives in `imrepl_ctrl.c`. |
| `repl_core.h` | Keep legacy `repl_display_func`, `repl_reshape_func`, and `repl_init_gl` declarations as wrappers for `sample.c`; update comments after the move. |
| `imrepl_ctrl.c` | Controller translation unit. Owns frame display, reshape, GL-init wiring, scene config construction, focus/guide snapshot construction, and UI render ordering. |
| `imrepl_ctrl.h` | New controller public surface: `imrepl_ctrl_init_gl`, `imrepl_ctrl_display_frame`, `imrepl_ctrl_reshape`, and any test-visible scene-config builder if needed. |
| `scene_render.c` | Consumes explicit `SceneRenderConfig *`; no config rebuild; no scene-side jitter writes; focus/guide snapshot reads come from config. Replay fade/HUD code remains transitional. |
| `scene_render.h` | Update `scene_render_3d_scene` signature; remove `scene_render_config_build` declaration. |
| `scene_render_types.h` | Carries focus/guide snapshots; scene-local jitter no longer lives in config. |
| `scene_guides_shared.h` | May remain the shared guide snapshot type used by controller and scene guide renderers. |
| `sample.c` | Prefer no churn: keep calling legacy `repl_*` wrappers. Switch to `imrepl_ctrl_*` only if the wrapper path becomes awkward. |
| `sample.h` | Legacy shared type/constant header. Do not rename in Phase 1; that would touch most files and should be a separate mechanical commit. |
| `Makefile` | Existing boundary checks are already in place; extend them instead of adding brittle duplicate checks. |

`scene_overlays.c`, `scene_geometry_guides.c`, `scene_transform_guides.c`,
`scene_grid.c`, `scene_axes.c`, `scene_backdrop.c`, and `scene_lights.c` are not
renamed in this plan.

## Step-by-Step Refactor

Each step should build and test before moving on.

### Step 1 - Create the controller skeleton

Files: new `imrepl_ctrl.c`, `imrepl_ctrl.h`, `Makefile`.

- Define:

```c
void imrepl_ctrl_init_gl(void);
void imrepl_ctrl_display_frame(void);
void imrepl_ctrl_reshape(int w, int h);
```

- Add `imrepl_ctrl.c` to `SRCS` and `CORE_TEST_SRCS`.
- Include only what the empty skeleton needs at first.
- Build green with no behavior change.

Exit criterion: new translation unit compiles and links.

### Step 2 - Move display, reshape, and GL-init controller wiring

Files: `imrepl_ctrl.c`, `repl_core.c`, `repl_core.h`.

- Move the body of `display_func` from `repl_core.c` to
  `imrepl_ctrl_display_frame`.
- Move the body of `reshape_func` to `imrepl_ctrl_reshape`.
- Move `init_gl` wiring to `imrepl_ctrl_init_gl`, including
  `scene_render_init_gl()`, `repl_executor_init_resources()`, and
  `apply_init_bootstrap()`.
- Keep legacy wrappers in `repl_core.c`:

```c
void repl_display_func(void) { imrepl_ctrl_display_frame(); }
void repl_reshape_func(int w, int h) { imrepl_ctrl_reshape(w, h); }
void repl_init_gl(void) { imrepl_ctrl_init_gl(); }
```

- Move scene/UI render includes needed by display/init out of `repl_core.c`.
- Do not change `sample.c` unless necessary.

Exit criterion: `repl_core.c` no longer includes `scene_*.h` or UI render
headers for frame display.

### Step 3 - Move scene config build and pass config explicitly

Files: `imrepl_ctrl.c`, `scene_render.c`, `scene_render.h`,
`scene_render_types.h`.

- Move `scene_render_config_build` and the current execute adapter/reset adapter
  into `imrepl_ctrl.c`.
- Rename the builder to `imrepl_ctrl_build_scene_config` once it is no longer
  public scene API.
- In the same step, change:

```c
void scene_render_3d_scene(void);
```

to:

```c
void scene_render_3d_scene(const SceneRenderConfig *config);
```

- Update `imrepl_ctrl_display_frame` to build the config once and pass it to the
  scene.
- Update `render_3d_scene_pass` to take `const SceneRenderConfig *config`
  instead of calling the builder.
- Remove `scene_render_config_build` from `scene_render.h`.
- Do not add a temporary call from `scene_render.c` back into `imrepl_ctrl.c`.

Exit criterion: `scene_render.c` never calls `scene_render_config_build`, and
the scene renderer receives explicit per-frame config.

### Step 4 - Snapshot scene-side REPL reads that belong in config

Files: `imrepl_ctrl.c`, `scene_render.c`, `scene_render_types.h`,
`scene_guides_shared.h` if needed.

- Move focus vertex construction out of `scene_render.c`; the controller builds
  a `SceneFocusVertex` from edit-line/source state and stores it in config.
  `scene_prepare_frame_context` copies it into `FrameRenderContext`.
- Move `SceneGuideSnapshot` construction out of `scene_render.c`; the controller
  fills it from editor input, source commands, flat program, predef vars, and
  presentation state.
- Change transform/geometry guide rendering to consume the snapshot already on
  the config/context.
- Change `draw_replay_tess_preview` to consume `config->flat_program` and
  `config->user_lighting_enabled` instead of calling `repl_state_flat_program_*`.
- Leave replay fade batches, baseline restore, and `draw_replay_hud` as
  documented follow-ups.

Exit criterion: direct document/editor/predef/flat-program state reads are gone
from non-replay scene paths.

### Step 5 - Keep clear color as a scene one-shot

Files: `scene_render.c`, `repl_executor.c` if verification finds a regression.

- Keep `scene_apply_clear_color` in scene code.
- It should walk `config->flat_program` and call `glClearColor` exactly once
  before the accumulation loop.
- Verify `repl_executor.c` does not call `glClearColor` while executing user
  geometry. Current code only updates `repl_state_render()->clear_color` for
  `CMD_CLEAR_COLOR`, so no executor change should be needed.

Exit criterion: clear color is applied once per frame before scene clearing.

### Step 6 - Make accumulation jitter scene-local

Files: `scene_render.c`, `scene_render_types.h`, tests that construct configs.

- Remove `accum_jitter_x/y` from `SceneRenderConfig`.
- Keep the jitter table local to `scene_render.c`.
- Pass jitter into projection via local pass variables or `FrameRenderContext`.
- Delete writes to `repl_state_render_mut()->accum_jitter_*`.
- Do not remove `ReplRenderState` jitter fields in this phase unless all tests
  and export/debug assumptions are audited.

Exit criterion: `scene_render.c` does not call `repl_state_render_mut()` for
jitter or any other scene-local loop variable.

### Step 7 - Boundary checks

Files: `Makefile`.

Extend the existing checks rather than adding overlapping targets.

Add a controller include check:

```makefile
check-controller-boundaries:
	@echo "Checking controller boundaries..."
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"scene_' repl_*.c imrepl_ctrl.c \
		| grep -v '^imrepl_ctrl\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: scene headers included outside imrepl_ctrl.c:"; \
		echo "$$bad"; exit 1; \
	fi
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"ui_' repl_*.c imrepl_ctrl.c \
		| grep -vE '^(imrepl_ctrl|repl_(actions|editor|export))\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: new ui headers included outside approved exceptions:"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "Controller boundaries OK"
```

Add a focused scene mutation check after Step 6:

```makefile
check-scene-no-repl-state-mut:
	@echo "Checking scene renderers do not mutate REPL state..."
	@bad=$$(grep -nE 'repl_state_[A-Za-z0-9_]*_mut[[:space:]]*\(' scene_*.c || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: scene files mutate REPL state:"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "Scene mutation boundary OK"
```

Wire both into `test` alongside the existing `check-gl-boundaries` and
`check-layer-coupling`.

Do not add a broad "no `repl_state_*` in scene" check in Phase 1; replay
follow-ups still have documented direct calls.

Exit criterion: `make check-controller-boundaries`,
`make check-scene-no-repl-state-mut`, and `make test` pass.

### Step 8 - Documentation pass

Files: `ARCHITECTURE.md`, `MODULES.md`, this file.

- Keep `ARCHITECTURE.md` and `MODULES.md` aligned with Option B.
- Mark `feature/push-architecture.md` as superseded by this plan if it remains
  in the tree.
- Document the live-GL allowlist and the controller include exceptions.
- Keep future replay/HUD/config-slimming work listed as follow-ups.

Exit criterion: docs match the implemented Phase 1 state and no longer promote
the old callback/`ReplGeometryRenderPlan` direction.

## Completed Post-Phase 1

1. **App-shell shim removal.** Removed trivial forwarding functions from
   `repl_core.c` (`repl_display_func`, `repl_reshape_func`, `repl_init_gl`).
   `sample.c` now directly calls `imrepl_ctrl_*` functions, eliminating an
   unnecessary indirection layer. This was a deliberate cleanup: Phase 1 kept
   the shims to avoid sample.c churn, but they added no value once controller
   was extracted.

## Future Refactors

1. **Replay fade-ring simplification and baseline restore relocation.** Collapse
   `ReplayFadeBatch[24]` and skip-limit aging into a smaller
   `ReplReplayHighlight` model if the visual result remains acceptable. Move
   surviving baseline restore policy out of scene rendering where practical.
2. **Completed: 2D replay HUD relocation.** `draw_replay_hud()` now lives in
  `ui_replay_hud.c`.
3. **Scene config slimming.** After replay cleanup, audit the remaining
   config fields and remove anything no longer used.
4. **Pure-scene grep guard.** Lock `scene_grid.c`, `scene_axes.c`,
   `scene_backdrop.c`, and `scene_lights.c` against direct REPL state access.
5. **Input/UI boundary cleanup.** Remove or narrow the `ui_*` include exceptions
   in `repl_editor.c`, `repl_actions.c`, and `repl_export.c`.
6. **App-shell rename.** Rename `sample.c` / `sample.h` to
   `imrepl.c` / `imrepl.h` in a dedicated mechanical pass. Update includes,
   build rules, and the live-GL allowlist at the same time. Keep this separate
   from controller extraction because `sample.h` is broadly included.

## Expansion: Why scene_render.c deals with replay, and why execute_fn can't absorb it

A natural question when reading the replay cross-talk: the executor already
renders the main geometry pass — why isn't it responsible for the fade
batches too? The answer is that replay rendering is actually three distinct
things that belong in three distinct layers. Only one of them is geometry
emission.

### What "replay rendering" in scene_render.c actually is

| Code | What it really is |
|------|-------------------|
| Main geometry pass clamped to `replay_base_limit` | Already executor-driven — just `execute_fn(1.0f, 0, pc, ...)`. No problem. |
| Fade batch loop (`render_3d_scene_pass` lines 424–480) | Multiple *additional* calls to `execute_fn`, each with different alpha/skip-limit, plus full GL state setup (push attrib, re-setup lighting, materials, blend) between each one. |
| `repl_replay_restore_baseline_predef_values()` | Resets user-declared vars to their replay-start values before each fade pass and each AA sample — so animated geometry looks consistent across passes. |
| `ui_replay_hud_render()` | Pure 2D UI, now in `ui_replay_hud.c`. |
| `draw_replay_tess_preview()` | 3D wireframe overlay — same family as `scene_overlays.c`. |

### Why execute_fn can't absorb the fade passes

`execute_fn` is a single-pass emitter. The fade batch loop calls it **N times**
for N fade batches, each with a different `alpha_scale` and
`skip_geom_before_pc`. Between each call, the scene does:

```c
glPushAttrib(GL_ALL_ATTRIB_BITS);
scene_lights_setup(&frame_ctx);   /* re-setup lighting per pass */
glEnable(GL_BLEND);
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);
/* ... */
execute_fn(batch.alpha, skip_limits[batch_idx], batch.new_pc, program, data);
/* ... */
glPopAttrib();
```

That GL state management — push/pop attribs, lighting re-setup, blend mode per
pass — is scene work. If the executor looped over batches internally it would
need to reproduce the lighting model the scene owns, inverting the dependency:
the executor would have to understand how the scene is configured rather than
just emitting geometry into whatever GL state the scene prepared. The executor's
role as a "narrow live-GL gate" breaks down the moment it starts managing
multi-pass render orchestration.

The accumulation-buffer AA loop has the same problem: it also calls
`repl_replay_restore_baseline_predef_values()` before each sample so that
animated geometry is consistent across AA jitter passes. The AA loop lives in
scene code because AA is a rendering-quality concern, but it needs the baseline
vars for the same reason the fade passes do.

### The correct decomposition

The real tangle is that fade *policy* (which batches exist, what alpha each
gets, what the skip limits and baseline vars are) lives mixed with fade
*orchestration* (GL state management, calling execute N times). Policy is
REPL/replay business; orchestration is scene business.

```
Controller:   read repl_replay_* once per frame;
              build ReplayFadePlan {
                batches[], batch_count,
                skip_limits[], alpha[],
                baseline_predef_vals[MAX_PREDEF_VARS]
              }
              put plan on SceneRenderConfig

Scene:        iterate plan -> for each batch:
                push attrib, setup GL state,
                restore baseline from plan snapshot,
                call execute_fn(batch.alpha, skip, pc, ...)
              orchestrates GL; doesn't know why the batches exist;
              never calls repl_replay_* or repl_state_*

Executor:     emit one geometry pass from the flat program
              stays single-pass, narrow gate
```

The baseline-restore is the key detail: instead of calling
`repl_replay_restore_baseline_predef_values()` at render time, the controller
bakes the baseline into the plan snapshot (`baseline_predef_vals[]`). Scene
restores from the snapshot before each fade pass and each AA sample. After
this, `repl_replay_*` and `repl_state_*` are never called from `scene_render.c`.

### Where the remaining replay code belongs after R1

- `ui_replay_hud_render()` → `ui_replay_hud.c` (2D UI overlay)
- `draw_replay_tess_preview()` → `scene_overlays.c` or stays in `scene_render.c`,
  driven by a config flag already on `SceneRenderConfig` (`replay_tess_preview`)
- Fade batch orchestration → stays in `scene_render.c`, but driven by the
  `ReplayFadePlan` snapshot instead of live `repl_replay_*` calls
- Fade batch *policy* → `repl_replay.c` / controller, built once per frame

This is what R1 in the Phase 2 recommendations means by "have the controller
build a `ReplayFadePlan` snapshot." The scene keeps the multi-pass
orchestration (which is legitimate scene work); it just stops owning the
*reason* the passes exist.

## Expansion: Where GLUT callbacks and input routing belong

`repl_editor.c` currently contains `keyboard_func`, `special_func`,
`mouse_func`, and related GLUT callbacks. These are registered with GLUT in
`sample.c` and handle all user input. The problem is that `keyboard_func` is
doing three distinct jobs at the same time, only one of which actually belongs
in `repl_editor.c`.

### The three jobs currently mixed in keyboard_func

1. **Raw GLUT event receipt** — being the function pointer registered with
   `glutKeyboardFunc`. Belongs in `sample.c`.
2. **Cross-layer routing** — deciding which subsystem gets this event based on
   current app state: is help open? is inline rename active? is the color picker
   focused? This is what the `ui_panels.h`, `ui_menu_bar.h`, and
   `ui_variable_panel.h` includes in `repl_editor.c` are actually for. It is
   also the source of the "known boundary exceptions" called out in
   `ARCHITECTURE.md`. Belongs in `imrepl_ctrl.c`.
3. **REPL-internal input handling** — once routing decides "this goes to the
   REPL editor", dispatching within the REPL layer: commit chain, search,
   autocomplete, undo, navigation. This is the only part that legitimately
   belongs in `repl_editor.c`.

The architecture violation is not that GLUT callbacks are in `repl_editor.c` —
it is that #2 lives there. Cross-layer routing requires knowing about all layers
simultaneously, which is exactly the controller's job. `repl_editor.c` is
currently misnamed: it is an app-level input multiplexer masquerading as an
editor module.

### Recommended split

#### `sample.c` — raw receipt only

Registers GLUT callbacks and forwards directly to the controller. No logic:

```c
glutKeyboardFunc(sample_keyboard);

static void sample_keyboard(unsigned char key, int x, int y) {
    imrepl_ctrl_keyboard(key, x, y);
}
```

#### `imrepl_ctrl.c` — cross-layer routing

Owns the priority chain. Calls handlers in focus order, stops at the first
that returns 1 (consumed):

```c
void imrepl_ctrl_keyboard(unsigned char key, int x, int y) {
    if (ui_help_overlay_handle_key(key))      return;
    if (repl_inline_rename_handle_key(key))   return;
    if (repl_search_handle_key(key))          return;
    if (ui_color_picker_handle_key(key))      return;
    if (ui_menu_bar_handle_key(key))          return;
    repl_editor_handle_key(key, x, y);
}
```

The controller knows which layer-input surfaces exist; it does not know what
any of them *do* with the event. It never calls `glut*` itself — that stays
in `sample.c`. With the routing moved here, `repl_editor.c` can legally drop
its `ui_*.h` includes, closing the boundary exceptions listed in
`ARCHITECTURE.md`.

#### Each module — focused `handle_key` surface

Each UI component and REPL overlay exposes a consumed/not-consumed handler:

```c
/* ui_help_overlay.h  */  int ui_help_overlay_handle_key(unsigned char key);
/* repl_search.h      */  int repl_search_handle_key(unsigned char key);
/* ui_color_picker.h  */  int ui_color_picker_handle_key(unsigned char key);
```

Return 1 if the event was consumed (priority chain stops), 0 if not (chain
continues). Modules that are currently inactive return 0 immediately. No module
knows about any other module.

#### `repl_editor.c` — REPL-internal input handling only

After the refactor, `repl_editor.c` is genuinely a REPL editor module:

- Exports `repl_editor_handle_key`, `repl_editor_handle_special`,
  `repl_editor_handle_mouse` with consumed/not-consumed signatures.
- Keeps the commit chain, undo, navigation, input buffer editing, autocomplete.
- Drops `#include "ui_panels.h"`, `#include "ui_menu_bar.h"`,
  `#include "ui_variable_panel.h"`.
- Routes internally to REPL subsystems (commit handlers, search queries,
  inline rename) but is never the routing authority for UI overlays.

### Mouse routing follows the same pattern, with hit-testing first

Mouse events require spatial routing in addition to state-based priority:

```c
void imrepl_ctrl_mouse(int button, int state, int x, int y) {
    /* Overlays get first refusal regardless of click position */
    if (ui_color_picker_mouse(button, state, x, y))   return;
    if (ui_menu_bar_mouse(button, state, x, y))        return;
    if (ui_variable_panel_mouse(button, state, x, y))  return;

    /* Hit-test to route spatially */
    if (editor_point_in_code_panel(x, y))
        ui_panels_handle_code_panel_click(x, y);
    else
        repl_camera_controls_mouse(button, state, x, y);
}
```

The controller calls geometry helpers (`editor_point_in_code_panel`,
`ui_panels_scene_rect`) for spatial dispatch — this is the correct and only
reason the controller needs `ui_panels.h` for input handling. After R3
(extract layout), the scene-rect call in the frame builder moves out; this
mouse-routing usage stays.

### On the three design options

| Option | Verdict |
|--------|---------|
| Each layer has its own routing core (`ui_input.c`, `scene_input.c`, etc.) | Partially right. The REPL layer does have internal routing in `repl_editor.c`. A separate `ui_input.c` just adds indirection — the controller already knows which UI overlays exist. |
| Expose handlers to the controller; it does the routing | **Correct for cross-layer routing.** Priority chain lives in the controller; each module exposes a focused handle function. |
| Controller consumes inputs and updates model state | No. The controller routes; handlers own the behavior. The controller decides *who*; the handler decides *what*. It should not know that Ctrl+Z means undo. |

### Relationship to the input/UI boundary exception

`ARCHITECTURE.md` lists `repl_editor.c`, `repl_actions.c`, and `repl_export.c`
as known `ui_*` include exceptions outside Phase 1 scope. Moving cross-layer
routing to `imrepl_ctrl.c` eliminates the `repl_editor.c` exception entirely:
the includes that caused it (`ui_panels.h`, `ui_menu_bar.h`,
`ui_variable_panel.h`) are only there for routing and hit-testing, both of
which move to the controller. The `repl_actions.c` and `repl_export.c`
exceptions are separate and require their own cleanup.

## Expansion: What remains in repl_core.c and why it should dissolve

`repl_core.c` was once the monolithic home of almost everything: display loop,
scene config build, camera, GLUT callbacks, parse pipeline, normalization,
reformat, user scenes, startup. Phase 1 extracted the frame controller into
`imrepl_ctrl.c` and stripped the shims. The question is whether the gutted
remainder should become the REPL's "main entry point" or be rationalised
further.

### What is actually left (665 lines)

| Content | Lines | Nature |
|---------|-------|--------|
| `normalize_with_indent`, `repl_normalize_from_parsed`, `repl_parse_and_normalize*`, `parse_and_normalize_impl` | ~90 | Parse+normalize pipeline — logically part of the parser |
| `repl_reformat_commands` | ~160 | Bulk reformatter (Ctrl+R) — walks all source cmds |
| `collect_visible_vars` | ~60 | Scope query — builds visible-var stack at a position |
| `load_initial_commands`, `scroll_to_display_function` | ~35 | Startup / session lifecycle |
| `repl_debug_dump_editor`, `repl_debug_dump_flat_commands` | ~60 | Test/debug dumps |
| `current_begin_mode`, `count_vertices` | ~20 | Small query helpers over flat/source cmds |
| Thin wrappers (`set_status`, `mark_normals_dirty`, `repl_flatten_commands`, etc.) | ~30 | One-liners forwarding to internal names |
| Empty `/* GLUT callbacks */` section | 2 | Dead — gutted in Phase 1 |

`repl_core.h` still declares `repl_keyboard_func`, `repl_special_func`,
`repl_mouse_func`, `repl_motion_func`, `repl_passive_motion_func`, and
`repl_mousewheel_func` — all stale since shim removal. `sample.c` now calls
`imrepl_ctrl_*` directly.

### Why it should not become a "REPL main entry point"

The controller is the app entry point. Making `repl_core.c` a second entry
point recreates the problem Phase 1 solved. The REPL does not have one entry
point — it has focused modules for parsing, storing, flattening, executing,
editing, and scene management. `repl_core.c` becoming a coordinator would pull
cross-cutting knowledge back into a catch-all, reversing the direction of the
refactor.

### Natural owners for everything remaining

Each piece has an unambiguous home:

- **`repl_parse_and_normalize*`, `normalize_with_indent`** → `repl_parser.c`.
  Parse pipeline operations. `repl_parser.c` already owns `parse_command()`;
  normalization is the step immediately after in the same pipeline. `repl_parser.h`
  gains the declarations; `repl_core.h` loses them.
- **`collect_visible_vars`** → `repl_source_scope.c`. It builds a scope stack
  at a given source position — exactly what `repl_source_scope.c` exists for.
- **`repl_reformat_commands`** → a new `repl_reformat.c` / `repl_reformat.h`.
  It is an independent, self-contained operation (one public function, one
  private helper) triggered by Ctrl+R. No existing module owns "bulk source
  reformatting."
- **`load_initial_commands`, `scroll_to_display_function`** → `repl_scenes.c`.
  Session startup is workspace/scene lifecycle, which `repl_scenes.c` already owns.
- **`repl_debug_dump_*`** → `repl_state.c` or a thin `repl_debug.c`. Diagnostic
  code does not belong in the normalization home.
- **`current_begin_mode`, `count_vertices`** → `repl_executor.c`. They query
  the flat/source cmd arrays for execution-time state.
- **Thin wrappers** → eliminate. Internal functions are renamed to match their
  public names, or collapse into their natural modules.
- **Stale GLUT declarations** → delete from `repl_core.h` immediately. They
  are dead code and actively mislead readers about what `repl_core.c` contains.

After all moves, `repl_core.c` disappears. `repl_core.h` either dissolves into
focused headers (callers update includes over time) or becomes a minimal umbrella
that re-exports from the headers above during the transition.

### Why not rename rather than dissolve?

Renaming `repl_core.c` to `repl_normalization.c` or `repl_pipeline.c` admits
the remaining content is a coherent cluster. It is not — normalization, startup,
scope queries, debug dumps, and thin wrappers do not belong in one file just
because they survived Phase 1. Renaming `repl_core.h` would require touching
nearly every `.c` file in the tree for a cosmetic change. A phased dissolve
is self-justifying: each step moves code to where it logically belongs, and
each commit can be reviewed independently.

## Expansion: "facade as global allowed surface"

`repl_state.h` exposes ~50 typed accessors (`repl_state_render()`,
`repl_state_replay()`, `repl_state_document_cmds_mut()`, etc.). It was introduced
to replace raw extern globals — but the win was only at the type level, not
at the layering level. Any .c file that includes repl_state.h can read or
mutate any subsystem's state from anywhere. The compiler enforces "you used
the right type"; nothing enforces "you were allowed to reach into that
subsystem from here."

Concretely, three things follow:

1. `scene_render.c` **reads** repl_state_replay/presentation/viewport/render
   **directly** instead of receiving a snapshot. Architecturally this is
   identical to the pre-facade state of the world where it touched globals —
   it's just typed now. The controller exists precisely to broker that data,
   and when scene bypasses it the controller's job is half-done.
2. `ui_color_picker.c` **calls** `repl_state_document_cmds_mut()[line].args[0] = r;`
   — a UI renderer reaching across the layering rule and mutating REPL-owned
   source commands in place, with no undo hook, no validation, no observer.
   The facade made this look like a typed call (mut() returning a typed
   pointer) instead of what it really is: a UI module editing the REPL's
   source-of-truth datastructure. Pre-facade it would have been
   `g_cmds[line].args[0] = r` and just as wrong; the typed wrapper makes it
   feel sanctioned.
3. **The boundary checks in `Makefile` only catch include-graph violations and
   one specific mutation pattern**. They don't see "scene module read REPL
   replay state" because the include is repl_state.h, which is allowed
   everywhere. The architecture rule lives only in prose (ARCHITECTURE.md
   "scene * files consume `SceneRenderConfig`...they should not call
   repl_state_* directly").

The facade is still the right primitive — but it should be treated like unsafe
in Rust: necessary, not the default. The path forward is (a) audit each
`repl_state_*` call site outside `repl_*.c` and `imrepl_ctrl.c` and replace with
snapshot reads or narrow store/action APIs, (b) add a make
check-views-no-repl-state that greps `scene_*.c` and `ui_*.c` for `repl_state_`
and fails on hits not in an explicit allowlist, and (c) split the facade header
so that `repl_state_views.h` (read-only, snapshot-friendly) and
`repl_state_owners.h` (mutating, owner-only) make the wrong call hard to import
accidentally.

That is what makes (1) and (2) load-bearing rather than cosmetic — they
convert a layering rule from "documented" to "compiler-enforced".

## Phase 2 Recommendations (Post-Phase 1 Audit)


Phase 1 extracted the controller and routed the per-frame data path through
`SceneRenderConfig`. An audit of the actual call graph (`callgraph-static.mmd`)
and direct grep of `scene_*.c` / `ui_*.c` shows the layering rule is honored at
the include-graph level but bypassed at the call-graph level. The typed-state
facade in `repl_state.h` is treated as a globally-allowed surface: anything
that includes it can read or mutate any subsystem's state, so the boundary
checks pass while the cross-talk continues. The recommendations below convert
the documented rule into a compiler-enforced one.

### Why the typed-state facade became a layering hole

`repl_state.h` exposes ~50 typed accessors (`repl_state_render()`,
`repl_state_replay()`, `repl_state_document_cmds_mut()`, etc.). The conversion
from raw `extern` globals improved type safety but did not narrow access. Any
translation unit that includes `repl_state.h` can pull any subsystem's state.

Concrete consequences observed in the current tree:

- `scene_render.c` reads `repl_state_replay/presentation/viewport/render`
  directly inside `draw_replay_hud` and `scene_render_3d_scene`
  (lines 187, 212, 222, 576). Architecturally identical to the pre-facade
  globals — just typed.
- `ui_color_picker.c` mutates `repl_state_document_cmds_mut()[line].args[0..3]`
  and `.source` in ~30 places (lines 63–145, 307–393). A UI renderer is editing
  REPL-owned source commands in place, with no undo hook, no validation, no
  observer. The `_mut()` wrapper makes this look sanctioned when it is not.
- `Makefile` boundary checks see only the include graph plus one specific
  mutation pattern (`check-scene-no-repl-state-mut`). They do not flag a scene
  module reading replay state, because the include is `repl_state.h`, which is
  allowed everywhere.

The fix is to treat the facade like `unsafe` in Rust: necessary, not the
default. The recommendations below split the surface and add grep guards so
the wrong call becomes hard to make accidentally.

### Recommendations, ordered by leverage

#### R1. Replay/HUD migration

Already listed under "Future Refactors" 1 and 2, but the impact was bigger than
the original framing reads. When R1 is complete, `scene_*.c` files have zero
`repl_state_*` and `repl_replay_*` calls, replay fade policy is snapped by the
controller, and HUD rendering lives in `ui_replay_hud.c`. Some of this may
already be true in the current tree; keep the sub-step notes below as rationale
and as a useful audit trail until the whole Phase 2 checklist is closed.

The work is three self-contained steps; do them in order since each removes
one class of REPL read from scene code.

**R1a — Add accumulation-AA fields to `SceneRenderConfig`**

`scene_render_3d_scene()` currently reads `repl_state_render()` at line 576
to get `use_accum`, `accum_aa_enabled`, and `accum_samples` for the outer
accumulation loop. The controller already holds `repl_state_render()` when
building the config; it just needs to copy three more fields.

In `scene_render_types.h`, add to `SceneRenderConfig`:

```c
/* Accumulation-buffer AA */
int   use_accum;
int   accum_aa_enabled;
int   accum_samples;
```

In `imrepl_ctrl.c::imrepl_ctrl_build_scene_config()`, add:

```c
config->use_accum        = *render->use_accum;
config->accum_aa_enabled = *render->accum_aa_enabled;
config->accum_samples    = *render->accum_samples;
```

In `scene_render_3d_scene()`, replace the `repl_state_render()` call with the
config fields:

```c
if (config->use_accum && config->accum_aa_enabled && config->accum_samples > 1) {
    ...
    for (int sample_idx = 0; sample_idx < config->accum_samples; sample_idx++) {
```

Remove `#include "repl_state.h"` from `scene_render.c` only after the later
steps have cleared the remaining `repl_state_*` uses (lines 187, 212–223).

Exit criterion: `scene_render.c` no longer calls `repl_state_render()`.

---

**R1b — Build a `ReplayFadePlan` snapshot in the controller; scene iterates it**

`render_3d_scene_pass()` (scene_render.c:424–481) calls three `repl_replay_*`
functions and `repl_replay_restore_baseline_predef_values()` between batches.
The fix is to build the plan once in the controller so the scene only needs to
iterate it.

Add `ReplayFadePlan` to `scene_render_types.h`:

```c
#define REPLAY_FADE_PLAN_MAX  REPLAY_FADE_BATCH_MAX   /* borrow constant */

typedef struct {
    /* Geometry bounds for each fading batch */
    int   old_pc[REPLAY_FADE_PLAN_MAX];
    int   new_pc[REPLAY_FADE_PLAN_MAX];
    float alpha[REPLAY_FADE_PLAN_MAX];
    int   skip_limits[REPLAY_FADE_PLAN_MAX];
    int   batch_count;
    /* Predefined variable values at replay baseline — restored before each
     * fade pass so animated geometry renders consistently */
    float baseline_predef[MAX_PREDEF_VARS];
    int   baseline_count;
} ReplayFadePlan;
```

Add `replay_fade_plan` to `SceneRenderConfig`:

```c
ReplayFadePlan replay_fade_plan;
```

In `imrepl_ctrl.c::imrepl_ctrl_build_scene_config()`, build the plan after the
existing replay fields:

```c
if (config->replay_has_fades) {
    ReplayFadePlan *plan = &config->replay_fade_plan;
    ReplayFadeBatchView view = repl_replay_fade_batches_view();
    plan->batch_count = repl_replay_compute_fade_skip_limits(
        plan->skip_limits, REPLAY_FADE_PLAN_MAX);
    for (int i = 0; i < plan->batch_count && i < view.count; i++) {
        plan->old_pc[i] = view.batches[i].old_pc;
        plan->new_pc[i] = view.batches[i].new_pc;
        plan->alpha[i]  = repl_replay_batch_alpha(&view.batches[i]);
    }
    repl_replay_copy_baseline_predef_values(plan->baseline_predef, MAX_PREDEF_VARS);
    plan->baseline_count = MAX_PREDEF_VARS;
} else {
    config->replay_fade_plan.batch_count = 0;
}
```

In `render_3d_scene_pass()`, replace the live `repl_replay_*` calls with a
loop over `config->replay_fade_plan`:

```c
if (config->replay_has_fades && config->execute_fn) {
    const ReplayFadePlan *plan = &config->replay_fade_plan;
    /* ... push attrib, setup GL state ... */
    for (int bi = 0; bi < plan->batch_count; bi++) {
        if (plan->alpha[bi] <= 0.0f) continue;
        repl_restore_predef_values(plan->baseline_predef, plan->baseline_count);
        glColor4f(0.70f, 0.70f, 0.80f, plan->alpha[bi]);
        glPushMatrix();
        config->execute_fn(plan->alpha[bi], plan->skip_limits[bi],
                           plan->new_pc[bi], config->flat_program,
                           config->execute_user_data);
        glPopMatrix();
    }
    if (config->execute_reset_fn)
        config->execute_reset_fn(config->execute_user_data);
    /* ... pop attrib ... */
}
```

Note: `repl_restore_predef_values` is already public (used by
`imrepl_ctrl.c`); add it to `repl_pipeline.h` if R4 has landed. The scene
never calls `repl_replay_*` again after this change.

The same baseline restore applies before each accumulation-AA sample in
`scene_render_3d_scene()`. Replace the `repl_replay_restore_baseline_predef_values()`
calls there with:

```c
if (config->replaying)
    repl_restore_predef_values(config->replay_fade_plan.baseline_predef,
                               config->replay_fade_plan.baseline_count);
```

Exit criterion: `scene_render.c` no longer includes `repl_replay.h`. The three
`repl_replay_*` call sites (lines 431, 433, 454, 460) and the three baseline
restore calls (lines 460, 587, 597) are all gone.

---

**R1c — Completed: move `draw_replay_hud()` to `ui_replay_hud.c`**

`ui_replay_hud_render()` now owns the replay HUD, lives in `ui_replay_hud.c`,
and is called from `imrepl_ctrl_display_frame()` after `scene_render_3d_scene()`.
It consumes the existing replay HUD fields on `SceneRenderConfig` for now;
slimming those fields is deferred to the later scene-config cleanup.

The old `draw_replay_hud()` implementation and the
`if (config->replaying) draw_replay_hud(config)` call are gone from
`scene_render.c`. `scene_render.c` no longer needs the 2D HUD glue include.

Exit criterion: `make test_scene_render USE_GL_STUBS=1` passes and the replay
HUD renders from `ui_replay_hud.c`.

#### R2. Close the UI → REPL mutation hole

`ARCHITECTURE.md` already states that UI mutations route through `repl_actions`
/ `repl_command_store` / `repl_var_drag`. The code does not match the doc.
The three offenders and their fixes:

**R2a — `ui_color_picker.c`: direct source-command mutation (~30 sites)**

`color_picker_write_cmd()` (ui_color_picker.c:60–130) writes directly to
`repl_state_document_cmds_mut()[g_cp_line].args[0..3]`, `.num_args`, and
`.source`, then calls `repl_state_mark_flat_dirty()`. This bypasses
`repl_command_store`, bypasses undo, and requires `ui_color_picker.c` to
include `repl_state.h` and `repl_core_internal.h`.

The reads in `ui_color_picker_open()` (lines 137–146) also use `_mut()` purely
for read access — a symptom of there being no `const` version of
`repl_state_document_cmds()` at those call sites.

Add two functions to `repl_command_store.h`:

```c
/* Replace the color args and source text of an existing color command in
 * place. Marks the flat program dirty. Does not push an undo snapshot —
 * callers that want undo should call push_undo_snapshot() before opening the
 * picker session, not on every drag event. Returns 1 on success, 0 if
 * cmd_idx is out of range or the command type is not a color command. */
int repl_command_store_set_color(int cmd_idx,
                                 float r, float g, float b, float a,
                                 int has_alpha);

/* Variant for CMD_CLEAR_COLOR: clamps r/g/b to CP_CLEAR_MAX_V before
 * writing. Returns 1 on success. */
int repl_command_store_set_clear_color(int cmd_idx,
                                       float r, float g, float b, float a);
```

Implement both in `repl_command_store.c`. They contain the formatting logic
currently in `color_picker_write_cmd()` (the `repl_format_fits` calls for each
cmd type), write via `repl_state_document_cmd_at_mut()`, and call
`command_store_invalidate_after_mutation()`. The color picker's `_open()`
function switches to `repl_state_document_cmds()` (const) for reads.

`color_picker_write_cmd()` becomes a four-line call to one of the two new
functions. `ui_color_picker.c` drops `#include "repl_state.h"` and
`#include "repl_core_internal.h"`.

Undo note: the picker mutates on every drag event; pushing a snapshot on every
drag would flood the undo ring. The correct pattern is to push one snapshot
when the picker opens (`ui_color_picker_open()`), before any edits land. Add
that call there once the store function exists.

Exit criterion: `ui_color_picker.c` contains zero `repl_state_*_mut()` calls.
`grep -n "repl_state_" ui_color_picker.c` returns empty.

---

**R2b — `ui_panels.c`: cursor blink mutation and replay state mutation (3 sites)**

Two distinct patterns:

*Cursor blink reset* — lines 1123–1124 and 1240–1241:

```c
*repl_state_code_panel_mut()->cursor_visible = 1;
*repl_state_code_panel_mut()->blink_tick = 0;
```

These appear after navigation clicks and drag events. Add to `repl_actions.h`:

```c
/* Reset cursor blink: make the cursor visible and restart the blink timer.
 * Call after any navigation that moves the cursor so it stays visible long
 * enough for the user to locate it. */
void repl_action_cursor_blink_reset(void);
```

Implement in `repl_actions.c` (two lines). Replace both sites in `ui_panels.c`.

*Replay pin-button mutation* — line 1131 and 1148–1149:

```c
ReplReplayRuntimeState *replay = repl_state_replay_mut();
/* ... */
if (*replay->state == REPLAY_PLAYING)
    *replay->state = REPLAY_PAUSED;
else if (*replay->state == REPLAY_PAUSED)
    *replay->state = REPLAY_PLAYING;
```

Add to `repl_replay.h`:

```c
/* Toggle play/pause for the Replay pin button: PLAYING→PAUSED, PAUSED→PLAYING,
 * OFF or DONE→calls repl_replay_start(). Mirrors the button glyph logic. */
void repl_replay_toggle_play_pause(void);
```

Implement in `repl_replay.c`. Replace the three-branch switch in
`ui_panels.c::ui_panels_handle_code_panel_press()` (lines 1148–1154) with a
single `repl_replay_toggle_play_pause()` call. Remove the
`repl_state_replay_mut()` fetch on line 1131.

Exit criterion: `ui_panels.c` contains zero `_mut()` call sites that are
actual mutations (reads-via-mut handle are caught by converting to the
const accessor `repl_state_document_cmds()`). `grep "_mut()" ui_panels.c`
returns only legitimate read-via-const-accessor patterns or nothing.

---

**R2c — `ui_panels.c` reads via `_mut()`: switch to const accessor**

`ui_panels.c` calls `repl_state_document_cmds_mut()` in ~15 read-only
contexts (lines 397, 539–715) — it uses the mutable accessor where the const
one would do. This matters because R6 (facade split) will make
`repl_state_document_cmds_mut()` importable only from `repl_state_owners.h`,
so `ui_panels.c` would need the owners header just to read cmd types.

Replace all read-only uses of `repl_state_document_cmds_mut()[i]` in
`ui_panels.c` with `repl_state_document_cmds()[i]`. This requires no logic
change — `repl_state_document_cmds()` returns `const GLCmd *`; field reads
work identically.

Exit criterion: `grep "document_cmds_mut" ui_panels.c` returns empty.

---

**R2d — `ui_help_overlay.c`: single `_mut()` site**

`ui_help_overlay.c:19` fetches `repl_state_help_mut()` to toggle the active
tab. Add to `repl_actions.h`:

```c
/* Cycle the help overlay tab (Commands / Keys). Called by the overlay's own
 * tab-click handler; does not open or close the overlay. */
void repl_action_help_tab_next(void);
```

Implement in `repl_actions.c`. Replace the single `repl_state_help_mut()`
use in `ui_help_overlay.c`.

Exit criterion: `grep "_mut()" ui_help_overlay.c` returns empty.

---

**R2 combined exit criterion**

After R2a–R2d:

```
grep -rn "repl_state_[A-Za-z0-9_]*_mut()" ui_*.c
```

returns empty. `ui_*.c` files include only `repl_state.h` (read-only
accessors) or focused action/store headers; none include `repl_state_owners.h`
or `repl_core_internal.h`. The `check-ui-no-repl-state-mut` Makefile rule
(R7) passes immediately when added.

#### R3. Extract layout geometry out of `ui_panels.c`

`ui_panels_scene_rect` and `ui_panels_code_panel_rect` (`ui_panels.c:50–110`)
are pure window-geometry functions: no GL, no rendering, no UI state. They
compute pixel rectangles from window dimensions, the current layout mode, and
`panel_frac`. Because they live in `ui_panels.c`, every non-UI caller —
`imrepl_ctrl.c`, `repl_editor.c`, `repl_export.c`, and the test suite — must
include a UI render header to get layout math. That is the wrong dependency
direction: layout geometry belongs in a module that `scene_*`, `repl_*`, and
`ui_*` modules can all include without pulling in UI rendering.

There are three steps; do them in a single commit so the rename is atomic.

Status: completed on the current branch; `repl_layout.c` / `repl_layout.h` now own the rectangle helpers and the caller list below reflects the migrated state.

---

**R3a — Create `repl_layout.h` / `repl_layout.c`**

Create `repl_layout.h`:

```c
/* repl_layout.h - pure window layout geometry; no GL, no rendering state */
#ifndef REPL_LAYOUT_H
#define REPL_LAYOUT_H

/* Scene viewport rectangle, in window pixels (origin bottom-left).
 * Derives from window size, layout mode, and panel_frac. */
void repl_layout_scene_rect(int *x, int *y, int *w, int *h);

/* Code-panel rectangle, in window pixels. */
void repl_layout_code_panel_rect(int *x, int *y, int *w, int *h);

#endif /* REPL_LAYOUT_H */
```

Create `repl_layout.c`. Move the bodies of `ui_panels_scene_rect` and
`ui_panels_code_panel_rect` verbatim into `repl_layout_scene_rect` and
`repl_layout_code_panel_rect`. The implementations already read only
`repl_state.h` accessors (`repl_state_presentation()->code_panel_layout`,
`repl_state_code_panel()->panel_frac`, `repl_state_viewport()->window_w/h`)
— that include stays correct in the new file; layout is a REPL-model consumer,
not a UI renderer.

Add `repl_layout.c` to `SRCS` and `CORE_TEST_SRCS` in the `Makefile`.

---

**R3b — Update all callers (≈34 call sites)**

Rename every call from the old function name to the new one and update
includes. The full caller inventory:

| File | Old function called | New function |
|------|--------------------|--------------------|
| `imrepl_ctrl.c` (1) | `ui_panels_scene_rect` | `repl_layout_scene_rect` |
| `ui_profile_panel.c` (1) | `ui_panels_scene_rect` | `repl_layout_scene_rect` |
| `ui_variable_panel.c` (1) | `ui_panels_scene_rect` | `repl_layout_scene_rect` |
| `ui_panels.c` (internal uses) | both | both `repl_layout_*` |
| `repl_editor.c` (2) | `ui_panels_code_panel_rect` | `repl_layout_code_panel_rect` |
| `repl_export.c` (1) | `ui_panels_code_panel_rect` | `repl_layout_code_panel_rect` |
| `ui_autocomplete_panel.c` (1) | `ui_panels_code_panel_rect` | `repl_layout_code_panel_rect` |
| `ui_color_picker.c` (1) | `ui_panels_code_panel_rect` | `repl_layout_code_panel_rect` |
| `ui_menu_bar.c` (3) | `ui_panels_code_panel_rect` | `repl_layout_code_panel_rect` |
| `test_repl_editor.c` (4+5) | both | both `repl_layout_*` |
| `test_repl_code_panel_document.c` (1) | `ui_panels_code_panel_rect` | `repl_layout_code_panel_rect` |
| `test_repl_core_commit.c` (2) | `ui_panels_code_panel_rect` | `repl_layout_code_panel_rect` |

For each file: replace the old call name, replace `#include "ui_panels.h"` with
`#include "repl_layout.h"` when `ui_panels.h` was included *only* for the
layout functions. Files that include `ui_panels.h` for other reasons (hit-test
routing, code-panel rendering) keep the include and simply add
`#include "repl_layout.h"` as well.

`ui_panels.c` itself: add `#include "repl_layout.h"` and change its own
internal calls to `repl_layout_*`. Remove the old function bodies entirely —
do not leave stubs. Remove `ui_panels_scene_rect` and
`ui_panels_code_panel_rect` from `ui_panels.h`.

---

**R3c — Verify the controller's `ui_panels.h` dependency is correctly scoped**

After R3b, `imrepl_ctrl.c` still includes `ui_panels.h`. That is correct: the
controller needs it for mouse input routing (`ui_panels_handle_code_panel_click`,
`ui_panels_scene_rect` is gone but hit-test calls remain). The include is no
longer there for config building — `repl_layout_scene_rect` from `repl_layout.h`
handles that now. No behavior change, but the reason for the include is now
clearly input routing only, which is what `MODULES.md` already says the
controller-UI include is for.

---

Exit criterion: `grep -n "ui_panels_scene_rect\|ui_panels_code_panel_rect"
imrepl_ctrl.c repl_editor.c repl_export.c ui_autocomplete_panel.c
ui_color_picker.c ui_menu_bar.c` returns empty (all uses have migrated). `make
test` passes. `ui_panels.h` no longer declares either geometry function.

#### R4. Stop reaching into `repl_core_internal.h` from the controller

`imrepl_ctrl.c:3` includes `repl_core_internal.h`. That header is documented as
"test-visible internals", but the controller is not a test. It pulls eight
symbols from it:

| Symbol | What it does |
|--------|-------------|
| `recompute_autonormals` | Rebuild auto-normals if geometry is dirty |
| `flatten_commands` | Rebuild flat program from source commands |
| `update_render_state_strings` | Sync camera/state display strings |
| `update_cam_lines` | Update camera command lines for the code panel |
| `apply_init_bootstrap` | Apply startup config from CLI args or a loaded file |
| `ensure_init_bootstrap_ready` | Guard that bootstrap state is valid before first frame |
| `repl_copy_predef_values` | Snapshot the current predefined variable table |
| `repl_restore_predef_values` | Restore a predefined variable table from a snapshot |

Every one of these is a legitimate pipeline or lifecycle operation. Putting
them in `_internal.h` was a temporary shortcut; the fix is to keep the
controller, scene/UI views, app shell, and neutral utilities out of
`repl_core_internal.h` without turning every REPL implementation helper into
public API. `repl_core_internal.h` may remain a REPL-internal collaboration
header used by tests and `repl_*.c` implementation files. There are four steps.

---

**R4a — Create `repl_pipeline.h` for pipeline and lifecycle operations**

Create `repl_pipeline.h`:

```c
/* repl_pipeline.h - REPL pipeline and lifecycle operations for the controller.
 *
 * These are the per-frame pipeline entry points that imrepl_ctrl.c drives.
 * They are NOT test internals. Call sites outside imrepl_ctrl.c require a
 * comment explaining why.
 */
#ifndef REPL_PIPELINE_H
#define REPL_PIPELINE_H

/* Rebuild the flat program from source commands. No-op if already up to date.
 * Call once per frame before building SceneRenderConfig. */
void flatten_commands(void);

/* Rebuild auto-normals if the geometry has changed since last call. */
void recompute_autonormals(void);

/* Sync the camera and render-state display strings. Call after camera moves. */
void update_render_state_strings(void);
void update_cam_lines(void);

/* Startup bootstrap: apply any config queued from CLI args or --load file.
 * ensure_* is a no-op after the first call; apply_* consumes the queue. */
void ensure_init_bootstrap_ready(void);
void apply_init_bootstrap(void);

/* Predefined variable table snapshots.
 * copy: write the current table into dst (up to max_vals entries).
 * restore: write src back into the live table (count entries). */
void repl_copy_predef_values(float *dst, int max_vals);
void repl_restore_predef_values(const float *src, int count);

#endif /* REPL_PIPELINE_H */
```

In `imrepl_ctrl.c`, replace `#include "repl_core_internal.h"` with
`#include "repl_pipeline.h"` (plus `#include "repl_eval.h"` for R4b).

In `repl_core_internal.h`, remove the declarations for the eight symbols above.
To avoid breaking tests that include `_internal.h` and transitively picked up
these declarations, add a comment noting they moved, or add
`#include "repl_pipeline.h"` at the top of `_internal.h` so tests continue
to compile without changes.

No implementation files change in this step — only header membership.

---

**R4b — Add `repl_eval_predef_view()` to `repl_eval.h` to hide global access**

`imrepl_ctrl.c:65–66` reads two file-scoped globals from `repl_eval.c` directly:

```c
guide_snapshot.predef_vars     = g_predef_vars;
guide_snapshot.num_predef_vars = g_num_predef_vars;
```

These globals are `static` within `repl_eval.c` — the controller can only reach
them because `repl_core_internal.h` re-exposes them via `extern` declarations.
After R4a removes the `_internal.h` include, the controller loses access. Wrap
the read behind a proper accessor.

Add to `repl_eval.h`:

```c
typedef struct {
    const ExprVar *vars;
    int            count;
} ReplPredefView;

/* Read-only snapshot of the current predefined variable table.
 * The pointer is stable until the next declare/undeclare/restore call.
 * Do not cache across frames. */
ReplPredefView repl_eval_predef_view(void);
```

Implement in `repl_eval.c`:

```c
ReplPredefView repl_eval_predef_view(void) {
    return (ReplPredefView){ .vars = g_predef_vars, .count = g_num_predef_vars };
}
```

Replace `imrepl_ctrl.c:65–66` with:

```c
ReplPredefView predef = repl_eval_predef_view();
guide_snapshot.predef_vars     = predef.vars;
guide_snapshot.num_predef_vars = predef.count;
```

If R1b has already landed, `repl_restore_predef_values` is also called from
`scene_render.c` (to restore baseline vars before each fade pass). That call
site uses `repl_pipeline.h` — the scene does not call `repl_eval_predef_view`
and does not see the globals.

---

**R4c — Verify `repl_core_internal.h` is limited to tests and `repl_` code after R4a + R4b**

After both steps:

```sh
grep -rn '#include.*repl_core_internal' *.c
```

Allowed matches are:

- `test_*.c` files.
- Production implementation files whose basename starts with `repl_`.

Disallowed matches are `imrepl_ctrl.c`, `sample.c`, `scene_*.c`, `ui_*.c`,
neutral utilities such as `prof.c`, `cmd_format.c`, `gl_stub_counts.c`, and
app/benchmark files such as `bench_repl.c`. If one of those files needs a
symbol that only exists in `_internal.h`, either route the operation through an
existing public API or add a narrow public API only when that symbol is truly
part of the production REPL surface.

This step intentionally does **not** require migrating all `repl_*.c` users off
`repl_core_internal.h`. Doing that by promoting internal helpers into public
headers would bloat the API and make private REPL implementation details look
stable.

Remove the `extern g_predef_vars` / `extern g_num_predef_vars` declarations
from `repl_core_internal.h` — they were the only reason the controller
had to include that header at all.

---

**R4d — Verify only truly public APIs live in `repl_*.h` headers**

R4c allows `repl_*.c` implementation files to keep using
`repl_core_internal.h`; R4d makes the other half of that rule explicit. Do not
solve internal include pressure by dumping implementation-only declarations into
ordinary `repl_*.h` headers.

Audit each `repl_*.h` declaration:

- Keep declarations that are used by a legitimate production consumer outside
  the owning implementation file, or that form an intentional cross-module REPL
  contract.
- Move implementation-only helpers back to file-local `static` functions,
  `repl_core_internal.h`, or a clearly private owner header if one is
  unavoidable.
- Do not treat tests as a reason to make an API public. Tests that need
  implementation seams may include `repl_core_internal.h`.
- Do not expose file-scope globals, mutable state tables, or convenience hooks
  whose only purpose is to avoid passing through the real owner API.

Exit criterion: the non-internal `repl_*.h` headers contain only production
surface area that another owner is allowed to call, with concise comments about
ownership and lifecycle. Internal seams remain internal, even when multiple
`repl_*.c` files share them.

---

**Status (2026-04-28):** R4a and R4b are complete. R4c is **in progress**.
R4d is **not started**.

- ✅ `imrepl_ctrl.c` no longer includes `repl_core_internal.h`
- ✅ `repl_pipeline.h` created and integrated; controller uses it exclusively
- ✅ `repl_eval_predef_view()` accessor added; globals are hidden
- ⚠️ 20 non-test production files still include `repl_core_internal.h`:
  ```
  bench_repl.c, repl_actions.c, repl_autocomplete.c, repl_clipboard.c,
  repl_command_store.c, repl_commit.c, repl_core.c, repl_editor.c,
  repl_example_loader.c, repl_executor.c, repl_export.c, repl_flatten.c,
  repl_parser.c, repl_replay_annotations.c, repl_replay.c, repl_scenes.c,
  repl_search.c, repl_state.c, repl_undo.c, ui_panels.c
  ```
  Under the R4c rule, the 18 `repl_*.c` users are allowed if they are genuine
  REPL implementation collaborators. The out-of-policy users are
  `bench_repl.c` and `ui_panels.c`; those must stop including
  `repl_core_internal.h` or be documented as temporary R11 exceptions until R4c
  is finished.

---

Exit criterion: `grep "repl_core_internal" imrepl_ctrl.c` returns empty. ✅
`grep -rn '#include.*repl_core_internal' *.c` lists only `test_*.c` and
`repl_*.c` files. ❌
`make test`, `make sample`, and `make sample USE_GL_STUBS=1` all pass. ✅

#### R5. Slim and reorganize `SceneRenderConfig`

R1 removes six HUD-only fields (`replay_pc`, `replay_total_cmds`,
`replay_state_val`, `replay_speed`, `replay_expand_args`, `code_panel_layout`)
and adds three accum-AA fields and `ReplayFadePlan`. After that lands, the
struct in `scene_render_types.h` still has a 30-field "Existing fields
(legacy, preserved)" section that mixes camera, quality, display flags, grid
settings, and replay state without logical grouping. R5 is the cleanup pass:
reorganize the struct and retire the "legacy" label. No implementation files
change — callers use named-field assignment, so reordering fields in the struct
is safe.

Do this in two steps.

---

**R5a — Remove HUD-only fields when R5 begins**

When you slim `SceneRenderConfig`, remove these six declarations from
`SceneRenderConfig` and from every `imrepl_ctrl.c` assignment in the
config-build function:

```
replay_pc  replay_total_cmds  replay_state_val
replay_speed  replay_expand_args  code_panel_layout
```

`grep -n "replay_pc\|replay_total_cmds\|replay_state_val\|replay_speed\|replay_expand_args\|code_panel_layout" scene_render_types.h imrepl_ctrl.c`
should return empty once the slimming step is applied.

---

**R5b — Reorganize `SceneRenderConfig` into labeled sections**

Replace the current flat struct (which ends with the unlabeled "Existing
fields" block) with a consistently grouped version:

```c
typedef struct SceneRenderConfig {
    /* ── Execute hook ──────────────────────────────────────────── */
    SceneExecuteProgramFn execute_fn;
    void                 *execute_user_data;
    void (*execute_reset_fn)(void *user_data);

    /* ── Flat program (snapshot for overlays / outline pass) ───── */
    FlatProgramView flat_program;

    /* ── Animation ─────────────────────────────────────────────── */
    float anim_time;

    /* ── Viewport and scene rectangle ──────────────────────────── */
    int viewport_w;
    int viewport_h;
    int scene_x;
    int scene_y;
    int scene_w;
    int scene_h;

    /* ── Camera ────────────────────────────────────────────────── */
    float cam_dist;
    float cam_rx;
    float cam_ry;
    float cam_tx;
    float cam_ty;
    float cam_tz;
    float cam_motion_glow;

    /* ── Rendering quality ─────────────────────────────────────── */
    int multisample_enabled;
    int line_smooth_enabled;
    int use_accum;
    int accum_aa_enabled;
    int accum_samples;

    /* ── Lighting ──────────────────────────────────────────────── */
    int        user_lighting_enabled;
    SceneLight lights[MAX_LIGHTS];
    int        show_light_indicators;

    /* ── Environment ───────────────────────────────────────────── */
    int backdrop_mode;
    int wireframe;

    /* ── Grid and axes ─────────────────────────────────────────── */
    int   grid_theme;
    int   grid_extent_idx;
    int   grid_major_idx;
    int   axes_theme;
    float grid_major_steps[GRID_MAJOR_COUNT];
    float grid_extents[GRID_EXTENT_COUNT];

    /* ── 3D overlay flags ──────────────────────────────────────── */
    int show_guides;
    int show_vpoints;
    int show_vnums;
    int show_normals;
    int show_vertex_outlines;
    int show_current_poly;

    /* ── Cursor / editor block overlay ─────────────────────────── */
    int          cursor_block_begin_idx;   /* -1 = no active block */
    int          cursor_block_end_idx;
    int          cursor_block_source_line;
    int          edit_line_idx;
    unsigned int cursor_func_scope_mask;
    int          cursor_call_src_cmd_idx;  /* -1 = cursor not on CMD_CALL */

    /* ── Focus and guide snapshots ─────────────────────────────── */
    SceneFocusVertex   focus;
    SceneGuideSnapshot guide_snapshot;

    /* ── Replay ────────────────────────────────────────────────── */
    int            replaying;
    int            replay_mode;
    int            replay_tess_preview;
    int            replay_vertex_points;
    int            replay_has_fades;
    int            replay_base_limit;
    float          alpha_scale;
    ReplayFadePlan replay_fade_plan;
} SceneRenderConfig;
```

The fields are identical to the post-R1 struct; only order and section labels
change. The `imrepl_ctrl.c` config-build function groups its assignments in
the same order so readers can scan both files side-by-side.

Remove the `FrameRenderContext` field `camera_world_y` and
`camera_below_water_surface` if they are only used by the scene internally
(not read from config); keep them if they are set by the controller. Audit
with `grep -n "camera_world_y\|camera_below_water_surface"`.

---

Exit criterion: `scene_render_types.h` no longer contains the string
"legacy, preserved". The six HUD fields are gone. `make test` and `make
sample` both pass. `grep "replay_pc\b" scene_render_types.h` returns empty.

#### R6. Split the typed-state facade by ownership

`repl_state.h` currently exports ~100 declarations in a single header:
const struct accessors, `_mut()` struct accessors, focused read helpers,
focused write helpers, `_set_*()` functions, `_reset()` / `_clear()` functions,
and lifecycle operations. Any file that includes `repl_state.h` picks up all
of them. The Makefile checks that view files don't call `_mut()`, but they
still *see* those declarations — nothing stops a developer from adding a
`_mut()` call in a view file and having it compile without an include change.

The fix is to split the header at the ownership boundary so that including the
views header physically cannot provide the mutating surface. There are four
steps; do them in order.

---

**R6a — Create `repl_state_owners.h` for the mutating surface**

Create `repl_state_owners.h`. Move into it every declaration that lets a
caller mutate REPL state:

- All `*_mut()` struct accessors (`repl_state_document_mut()`,
  `repl_state_replay_mut()`, etc. — 23 functions)
- All raw mutable pointer accessors (`repl_state_document_cmds_mut()`,
  `repl_state_document_cmd_at_mut()`, `repl_state_flat_program_cmds_mut()`,
  `repl_state_flat_program_local_vars_mut()`, `repl_state_clipboard_cmds_mut()`)
- All `*_set_*()` focused setters (`repl_state_edit_line_set()`,
  `repl_state_input_set_text()`, `repl_state_cursor_pos_set()`,
  `repl_state_viewport_set_size()`, etc.)
- All `*_reset()` and `*_clear()` functions
- All state-modifying operations regardless of name:
  `repl_state_mark_flat_dirty()`, `repl_state_mark_normals_dirty()`,
  `repl_state_time_advance()`, `repl_state_time_reset_to_zero()`,
  `repl_state_status_set()`, `repl_state_status_tick()`,
  `repl_state_workspace_set_dir()`,
  `repl_state_refresh_workspace_header_lines()`,
  `repl_state_parse_workspace_header_line()`,
  `repl_state_init_defaults()`, `repl_state_reset_all()`
- All mutable buffer accessors that expose writable memory
  (`repl_state_input_buffer_mut()`, `repl_state_pending_newline_buffer_mut()`)

`repl_state_owners.h` includes `repl_state_views.h` (for the type
declarations and const-view functions it depends on).

**What stays in `repl_state_views.h`** (all remaining declarations):

- All `const Type *repl_state_X()` struct accessors (23 functions)
- All focused read-only helpers: `repl_state_document_count()`,
  `repl_state_document_capacity()`, `repl_state_edit_line()`,
  `repl_state_normals_dirty()`, `repl_state_flat_program_count()`,
  `repl_state_flat_program_dirty()`,
  `repl_state_flat_program_user_lighting_enabled()`,
  `repl_state_flat_program_view()`, `repl_state_input_text()`,
  `repl_state_input_len()`, `repl_state_cursor_pos()`,
  `repl_state_insert_mode()`, `repl_state_selection_anchor()`,
  `repl_state_selection_end_idx()`, `repl_state_clipboard_count()`,
  `repl_state_workspace_dir()`,
  `repl_state_pending_newline_len()`,
  `repl_state_flat_program_cmds()`, `repl_state_document_cmds()`,
  `repl_state_document_cmd_at()`
- Snapshot-value helpers that return a by-value copy: `repl_state_camera_snapshot()`,
  `repl_state_presentation_snapshot()`
- All the struct type definitions (`ReplDocumentState`, `ReplFlatProgramState`,
  etc.) and the `ReplRuntimeState` aggregate

`repl_state_views.h` must be self-contained: no forward-declaration tricks,
no reliance on including `repl_state_owners.h`.

**Compatibility shim**: update `repl_state.h` to:

```c
/* repl_state.h — compatibility shim; new code should include the specific
 * header it needs: repl_state_views.h or repl_state_owners.h */
#include "repl_state_views.h"
#include "repl_state_owners.h"
```

All existing `#include "repl_state.h"` sites continue to compile without
change, picking up both halves through the shim. No production code needs
updating in this step.

---

**R6b — Migrate `scene_*.c` and `ui_*.c` from `repl_state.h` to `repl_state_views.h`**

This step is only valid after R1 and R2 have landed — both remove all
`_mut()` and `repl_replay_*` calls from scene and UI files.

For each `scene_*.c` and `ui_*.c` file that currently includes `repl_state.h`,
change the include to `repl_state_views.h`. The file should now compile
cleanly, because after R1+R2 it no longer calls any of the mutating functions
that are only in `repl_state_owners.h`.

If a file fails to compile after the switch, that is a missed mutation from
R1 or R2 — fix it before committing R6b for that file, not by reverting to
`repl_state.h`.

Do the migration file-by-file, build after each, so failures are isolated.
Expected migration set:

```
scene_render.c      scene_grid.c      scene_axes.c
scene_backdrop.c    scene_lights.c    scene_overlays.c
scene_geometry_guides.c              scene_transform_guides.c
ui_panels.c         ui_menu_bar.c     ui_color_picker.c
ui_help_overlay.c   ui_variable_panel.c
ui_autocomplete_panel.c              ui_profile_panel.c
```

Owner modules (`repl_*.c`, `imrepl_ctrl.c`) continue to include
`repl_state.h` (the shim) or explicitly `repl_state_owners.h` — either works.

---

**R6c — Add the `check-views-no-owners` Makefile guard**

```makefile
check-views-no-owners:
	@echo "Checking scene/UI files do not include repl_state_owners.h..."
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"repl_state_owners\.h"' \
		scene_*.c ui_*.c || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: view files include repl_state_owners.h:"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "Facade ownership boundary OK"
```

Wire it into `make test`. After R6b, this check passes. From then on, any
developer who adds a `_mut()` call in a view file must explicitly add
`#include "repl_state_owners.h"` to make it compile — and the check
immediately flags the include. The wrong call is no longer invisible.

---

**R6d — Verify and document the ownership boundary**

After R6b and R6c:

```sh
grep -rn '#include.*repl_state' scene_*.c ui_*.c
```

Should show only `repl_state_views.h`. Any remaining `repl_state.h` includes
in those files should be converted; they likely survived because R1 or R2
isn't fully complete for that file.

Update `MODULES.md` boundary rules section to reflect the split: scene/UI
files include `repl_state_views.h`; owner and controller files include
`repl_state.h` or `repl_state_owners.h`.

**Note on the struct-of-pointers caveat**: `const ReplDocumentState *` has a
`GLCmd *cmds` field (non-const). A sufficiently motivated caller can still
reach through the const pointer to mutate the array. This is a known
limitation of the existing facade design. The split prevents accidental
mutations and makes intentional ones clearly wrong; it does not make them
physically impossible. A future hardening pass can add truly const field
variants to the view structs if the team decides it is worth the churn.

---

Exit criterion: `make check-views-no-owners` passes. `grep -rn '#include.*repl_state' scene_*.c ui_*.c` shows only `repl_state_views.h`. `make test`, `make sample`, and `make sample USE_GL_STUBS=1` all pass.

#### R7. Add view-side state read guards

After R1 + R6, add two greps to `make test`:

```makefile
check-pure-scene-no-repl-state:
	@bad=$$(grep -nE 'repl_(state|replay)_' \
		scene_grid.c scene_axes.c scene_backdrop.c scene_lights.c \
		scene_geometry_guides.c scene_transform_guides.c \
		scene_overlays.c scene_render.c || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: scene files reach into REPL state:"; \
		echo "$$bad"; exit 1; \
	fi

check-ui-no-repl-state-mut:
	@bad=$$(grep -nE 'repl_state_[A-Za-z0-9_]*_mut[[:space:]]*\(' ui_*.c || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: ui files mutate REPL state directly:"; \
		echo "$$bad"; exit 1; \
	fi
```

The pure-scene rule starts as a small allowlist (e.g.
`scene_render.c` exempt during R1 transition) and shrinks as cleanup lands.

#### R10. Dissolve repl_core.c into its natural owners (phased)

`repl_core.c` was the monolithic REPL home. Phase 1 extracted the frame
controller; what remains is an incoherent cluster of normalization, reformat,
scope queries, startup, debug dumps, and thin wrappers. It should dissolve, not
be renamed. Do it in phases so each commit is independently reviewable and the
include-churn from `repl_core.h` is amortized.

**Phase 1 of R10 — delete the dead stale content (one commit, zero risk):**

- Remove the empty `/* GLUT callbacks */` section from `repl_core.c`.
- Delete the stale `repl_keyboard_func`, `repl_special_func`,
  `repl_mouse_func`, `repl_motion_func`, `repl_passive_motion_func`, and
  `repl_mousewheel_func` declarations from `repl_core.h`. They are dead since
  the shim removal; `sample.c` calls `imrepl_ctrl_*` directly.

**Phase 2 of R10 — move the parse+normalize pipeline to the parser:**

- Move `repl_parse_and_normalize*`, `normalize_with_indent`,
  `repl_normalize_from_parsed`, and `parse_and_normalize_impl` into
  `repl_parser.c`. Add declarations to `repl_parser.h`. Remove from
  `repl_core.h`.
- Move `collect_visible_vars` into `repl_source_scope.c` /
  `repl_source_scope.h`. It builds a scope stack at a given position — the
  textbook scope-query module.
- Update callers (`repl_reformat_commands`, `repl_commit.c`, tests) to include
  the new headers instead of `repl_core.h` for these symbols.

**Phase 3 of R10 — extract the reformatter:**

- Move `repl_reformat_commands` (and its private helper `get_for_var_name`) to
  a new `repl_reformat.c` / `repl_reformat.h`. One public function, one private
  helper — self-contained. Remove from `repl_core.h`.
- `repl_editor.c` (which calls it for Ctrl+R) updates its include.

**Phase 4 of R10 — move startup and query helpers:**

- Move `load_initial_commands` and `scroll_to_display_function` into
  `repl_scenes.c`. Session startup belongs with workspace/scene lifecycle.
- Move `current_begin_mode` and `count_vertices` into `repl_executor.c`;
  they query flat/source cmd state for execution-time purposes.
- Move `repl_debug_dump_editor` and `repl_debug_dump_flat_commands` into
  `repl_state.c` or a thin `repl_debug.c`.
- Eliminate thin wrappers (`set_status`, `mark_normals_dirty`,
  `repl_flatten_commands`, `repl_advance_time`, etc.) by renaming internal
  functions to match their public names, or moving them into the module that
  owns the underlying operation.

**Phase 5 of R10 — dissolve repl_core.h:**

Once `repl_core.c` is empty, stop treating `repl_core.h` as the accidental
"include everything" header. R12 below defines the intentional replacement:
one concise public REPL API header grouped by implementation file, with
implementation detail kept out of the public surface.

R10 is independent of R1–R7 and can be done in parallel with them. Phase 1 of
R10 (deleting dead declarations) should be the very first commit in any session
touching `repl_core.c`, since it eliminates noise that misleads readers.

#### R11. Harden file-level boundary checks

The first-generation Makefile guards caught several broad layer violations, but
their file-level greps were too dependent on shell globs and too weak around
comment filtering. R11 keeps the checks cheap and grep-based, but makes them
more explicit about ownership and known transitional exceptions.

Current guard direction:

- Use `$(SRCS)` / `$(HDRS)`-derived file groups (`REPL_SRCS`, `SCENE_SRCS`,
  `UI_SRCS`) instead of raw `repl_*.c` / `scene_*.c` / `ui_*.c` shell globs.
  This prevents untracked scratch files from changing check behavior.
- Tighten GL/GLU/GLUT grep filters so comments and strings are ignored without
  dropping real code lines that contain `*` or `/`.
- Add `check-state-boundaries` to keep state-neutral modules
  (`cmd_format.c`, `prof.c`, `gl_stub_counts.c`) free of `repl_state.h` and
  `_internal` headers.
- Make scene state isolation enforceable now: `scene_*.c` must contain no
  `repl_state_*` or `repl_replay_*` calls.
- Keep shrinking transition allowlists for known R2/R4 holes. R4c permits
  `repl_core_internal.h` in tests and `repl_*.c` files only; any remaining
  `ui_*`, `scene_*`, app-shell, benchmark, or neutral-utility include is a
  temporary exception that must disappear or be explicitly justified.

R11 is not a substitute for R2/R4/R6/R7, and it does not mean Phase 2 is
complete. It is the guardrail that prevents new leaks while those steps shrink
the allowlists. The current Makefile may carry transitional allowlists; strict
no-exception variants should only be wired into `make test` once the relevant
Phase 2 prerequisite has landed. Each time a cleanup removes a known exception,
update `check-state-boundaries` in the same commit so the exception cannot come
back silently.

Exit criterion for the transitional R11 slice: `make check-gl-boundaries`,
`make check-layer-coupling`, `make check-controller-boundaries`,
`make check-scene-no-repl-state-mut`, and `make check-state-boundaries` all pass
with only explicitly documented allowlists, and the `test` target runs the
transitional checks. Exit criterion for the final Phase 2 guard state: the
allowlists are empty or reduced to permanent, documented architectural
exceptions.

#### R12. Consolidate public REPL APIs into one public header

The final public REPL surface should be one header, tentatively `repl.h`, not a
wide set of public `repl_*.h` files. This is separate from R4c/R4d: R4 keeps
internals out of view/controller/app code and prevents API bloat; R12 gives the
remaining public API a deliberate home.

Target shape:

- `repl.h` is the only header non-`repl_` production code includes for REPL
  public APIs.
- The header is organized into concise sections by implementation file or owner
  area, for example `repl_parser.c`, `repl_eval.c`, `repl_export.c`,
  `repl_replay.c`, and `repl_scenes.c`.
- Each section has a short purpose note and the declarations that are truly
  public. The current verbose per-header descriptions move to the matching
  implementation-section comments, private headers, or architecture docs.
- The combined header does not include `repl_core_internal.h`, expose mutable
  globals, or absorb implementation-only helper declarations.
- Existing per-module `repl_*.h` files either become private owner headers used
  only by `repl_*.c` and tests, or disappear after callers migrate to `repl.h`.

Suggested sequence:

1. Finish R4d so the public/private line is known before consolidation.
2. Let R10 finish moving `repl_core.c` responsibilities to natural owners.
3. Create `repl.h` with concise owner sections and migrate non-`repl_`
   production callers to it.
4. Move verbose module-header prose into the corresponding implementation
   sections, leaving only concise public comments in `repl.h`.
5. Remove or privatize obsolete per-module public headers.

Exit criterion: non-`repl_` production files include one REPL public header
(`repl.h`) for production REPL APIs. `repl_core_internal.h` remains excluded
from that header and stays limited to tests plus `repl_*.c` implementation
files.

#### R8. Defer `sample → imrepl` rename until after R1, R2, R5

The rename is mechanical but touches `sample.h`, which is broadly included.
R1, R2, and R5 each touch `scene_render.h` / `scene_render_types.h` and
several view modules; doing the rename first multiplies rebase pain. Land it
as the final commit before `imrepl.c` becomes the new live-GL allowlist
anchor.

#### R9. Optional: split `repl_export.c` (2827 lines)

Not architectural, just module hygiene. Natural split:
`repl_export_save.c` / `repl_export_load.c` / `repl_export_workspace.c`.
Skip if the file is not actively painful to navigate.

### Suggested ordering

```
R10-phase1  (delete stale GLUT decls — zero risk, do first)
R1          (replay/HUD migration — highest leverage, unblocks R5 + R6)
R2          (UI → REPL mutation hole — can run in parallel with R1)
R3          (extract layout from ui_panels)
R5          (slim SceneRenderConfig — requires R1)
R6          (split typed-state facade — requires R1 + R2)
R7          (add view-side grep guards — requires R6)
R11         (harden file-level guards; start now, shrink allowlists after R2/R4/R6)
R4          (controller off repl_core_internal.h; public-header audit)
R10-phase2  (parse+normalize → repl_parser, collect_visible_vars → repl_source_scope)
R10-phase3  (extract repl_reformat.c)
R10-phase4  (startup + query helpers to natural owners)
R10-phase5  (dissolve repl_core.h — feeds into R12)
R12         (single public repl.h — after R4d + R10)
R8          (sample → imrepl rename — mechanical, last)
R9          (optional: split repl_export.c)
```

R10 phases 2–5 are independent of R1–R7 and can be interleaved freely; only
R10-phase1 has a strong "do it first" signal because it removes noise that
misleads every reader. R4 and R10-phase2 are natural companions: R4 creates
`repl_pipeline.h` as a proper public surface for the symbols the controller
needs; R10-phase2 reorganises what remains in `repl_core.c` after those symbols
are promoted.

R12 should wait until R4d and most of R10 have settled; otherwise the combined
header will just preserve the current confusion in a bigger file.

### Phase 2 Recommendations Status (2026-04-28)

Implementation is actively in progress. Current completion:

| Recommendation | Status | Notes |
|---|---|---|
| **R1** — Replay/HUD migration | ✅ Complete | R1a, R1b, R1c all done; scene has zero `repl_replay_*` calls |
| **R2** — UI → REPL mutation hole | ✅ Complete | R2a–R2d all done; UI files route mutations through actions/stores |
| **R3** — Extract layout geometry | ✅ Complete | `repl_layout.h/c` created; ~34 call sites updated |
| **R4** — Controller off `repl_core_internal.h` | ⚠️ Partial | R4a/R4b done; R4c in progress (controller ✅, 2 out-of-policy includes remain); R4d pending |
| **R5** — Slim `SceneRenderConfig` | ❌ Not started | Requires R1 ✅ |
| **R6** — Split typed-state facade | ❌ Not started | Requires R1 ✅ + R2 ✅ |
| **R7** — View-side grep guards | ❌ Not started | Requires R6 |
| **R11** — Harden file-level guards | ❌ Not started | Can start now; shrink allowlists as R2/R4/R6 land |
| **R10-phase1** — Delete stale GLUT decls | ❌ Not started | Zero-risk, should be done first of R10 phases |
| **R10-phase2+** — Dissolve `repl_core.c` | ❌ Not started | Phased; depends on R4c completion |
| **R12** — Single public REPL header | ❌ Not started | Requires R4d public/private audit + R10 responsibility moves |
| **R8** — `sample → imrepl` rename | ❌ Not started | Mechanical; defer until after R5 |
| **R9** — Optional: split `repl_export.c` | ❌ Not started | Module hygiene only; skip if not painful |

**Next recommended steps:**
1. Complete R4c by removing the out-of-policy `repl_core_internal.h` includes
   from `bench_repl.c` and `ui_panels.c` or documenting them as temporary R11
   exceptions.
2. Start R4d and audit `repl_*.h` declarations so public headers do not absorb
   implementation-only seams.
3. Start R5 (slim `SceneRenderConfig`) — unblocked by R1 ✅
4. Start R6 (split state facade) — unblocked by R1 ✅ + R2 ✅

## Verification

Run after each step and again at the end:

1. Build matrix:
   - `make clean && make sample`
   - `make sample USE_GL_STUBS=1`
   - `make glut` on macOS when the system GLUT path is available
2. Test suite:
   - `make test`
   - focused tests affected by the step, especially `test_scene_render`,
     `test_repl_core_internal`, and replay/editor suites when touched
3. Boundary checks:
   - `make check-gl-boundaries`
   - `make check-layer-coupling`
   - `make check-controller-boundaries`
   - `make check-scene-no-repl-state-mut`
   - `make check-state-boundaries`
4. Manual smoke test of `./sample`:
   - startup with no args
   - load built-in examples via F12
   - toggle wireframe, grid theme, axes theme, accumulation AA
   - replay start/pause/step-back, expand args, vertex/polygon mode
   - camera orbit/pan/zoom and orbit-target fade
   - save, exit, reload `./sample output.c`

Do not claim repo-wide verification from a single sample build.

## Rollback

Keep each step as a self-contained commit. If a step regresses behavior, revert
that step before building later steps on top. The save/load text format is not
changed by this plan.
