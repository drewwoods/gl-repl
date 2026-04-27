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
mechanical cleanup. Replay fade simplification, 2D replay HUD relocation, and
broader input/UI coupling cleanup remain follow-ups.

Implementation status: steps 1-7 have already landed in the repository and are
validated. This document now records the completed Phase 1 slice plus the
remaining follow-ups, with the documentation pass as the only Phase 1 step
still being actively aligned.

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
- Replay fade batches and the 2D replay HUD are explicit follow-ups, so Phase 1
  does not pretend every direct `repl_replay_*` use disappears immediately.

## Revised Tenets

1. **REPL does not own scene decoration or UI.** The REPL parses, stores,
   flattens, replays, exports, and owns user-geometry execution through
   `repl_executor.c`.
2. **Scene owns the 3D stage, not the editor.** Scene sets viewport,
   projection, camera, clear, accumulation, baseline lighting, grid, axes,
   backdrop, light indicators, orbit target, and 3D overlay passes from config.
3. **UI owns 2D editor chrome.** Code panel, menus, popups, color picker, help,
   profile HUD, status banner, and future 2D replay HUD live in `ui_*`.
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
- 2D replay HUD relocation to `ui_replay_hud.c`.
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

## Future Refactors

1. **Replay fade-ring simplification and baseline restore relocation.** Collapse
   `ReplayFadeBatch[24]` and skip-limit aging into a smaller
   `ReplReplayHighlight` model if the visual result remains acceptable. Move
   surviving baseline restore policy out of scene rendering where practical.
2. **2D replay HUD relocation.** Move `draw_replay_hud()` out of
   `scene_render.c` into `ui_replay_hud.c`; drop HUD-only fields from
   `SceneRenderConfig`.
3. **Scene config slimming.** After replay/HUD cleanup, audit the remaining
   config fields and remove anything no longer used.
4. **Pure-scene grep guard.** Lock `scene_grid.c`, `scene_axes.c`,
   `scene_backdrop.c`, and `scene_lights.c` against direct REPL state access.
5. **Input/UI boundary cleanup.** Remove or narrow the `ui_*` include exceptions
   in `repl_editor.c`, `repl_actions.c`, and `repl_export.c`.
6. **App-shell rename.** Rename `sample.c` / `sample.h` to
   `imrepl.c` / `imrepl.h` in a dedicated mechanical pass. Update includes,
   build rules, and the live-GL allowlist at the same time. Keep this separate
   from controller extraction because `sample.h` is broadly included.

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
