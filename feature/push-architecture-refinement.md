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

Implementation status: Phase 1 steps 1-8 are complete and merged. Post-Phase 1
cleanup (app-shell shim removal) has also been completed. This document records
the final Phase 1 state and documents remaining follow-ups.

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

#### R1. Finish the replay/HUD migration (largest single cleanup)

Already listed under "Future Refactors" 1 and 2, but the impact is bigger than
the current framing reads. Three concrete moves, in this order:

1. Move `draw_replay_hud()` from `scene_render.c` to a new `ui_replay_hud.c`.
   Drop the HUD-only fields from `SceneRenderConfig` (`replay_pc`,
   `replay_total_cmds`, `replay_state_val`, `replay_speed`,
   `replay_expand_args`, plus the `code_panel_layout` value used only by the
   HUD layout fallback).
2. Have the controller build a `ReplayFadePlan` snapshot once per frame and
   put it on the config: `{ batches, batch_count, skip_limits[], baseline_predef[] }`.
   The scene iterates the plan; no `repl_replay_fade_batches_view`,
   `repl_replay_compute_fade_skip_limits`, or
   `repl_replay_restore_baseline_predef_values` calls in scene code.
3. Move the accumulation-AA settings (`use_accum`, `accum_aa_enabled`,
   `accum_samples`) onto the config. The controller already reads them
   indirectly; the scene then stops calling `repl_state_render()` entirely.

After R1, `scene_render.c` has **zero** `repl_state_*` and `repl_replay_*`
reads. That is the real architectural payoff — it lets the boundary check flip
from "no `_mut`" to a flat-out ban on `repl_*_state_*` and `repl_replay_*`
symbols in `scene_*.c`.

#### R2. Close the UI → REPL mutation hole

`ARCHITECTURE.md` already states that UI mutations route through `repl_actions`
/ `repl_command_store` / `repl_var_drag`. The code does not match the doc:

| File | `_mut()` call sites | Notable |
|------|---------------------|---------|
| `ui_color_picker.c` | ~30 | Direct writes to `args[0..3]` and `source` of source cmds |
| `ui_panels.c` | ~25 | Cursor blink reset, replay state mutation |
| `ui_help_overlay.c` | 1 | `repl_state_help_mut()` for tab toggle |

Add narrow store/action APIs and convert call sites:

- `repl_command_store_set_color(line_idx, r, g, b, a)` and
  `repl_command_store_set_clear_color(line_idx, r, g, b, a)`. These can also
  push undo snapshots, which the picker currently bypasses.
- `repl_action_blink_cursor_reset()` (replaces `*repl_state_code_panel_mut()->cursor_visible = 1`
  and `->blink_tick = 0` patterns in `ui_panels.c`).
- `repl_replay_set_state(...)` / `repl_replay_set_pc(...)` for the replay
  fields touched by `ui_panels.c`.

Once converted, `ui_*.c` should have zero `repl_state_*_mut()` calls.

#### R3. Extract layout out of `ui_panels.c`

`ui_panels_scene_rect` and `ui_panels_code_panel_rect` (`ui_panels.c:50–110`)
are pure functions of window size + layout mode + `panel_frac`. They contain
no GL. Move both to a new `repl_layout.c` (or `imrepl_layout.c`). The
controller currently includes `ui_panels.h` only to call them when building
`SceneRenderConfig`; after the move, the controller's UI-header dependency
disappears for non-rendering reasons. `ui_variable_panel.c`,
`ui_profile_panel.c`, and `ui_panels.c` itself update their includes; no
behavior change.

#### R4. Stop reaching into `repl_core_internal.h` from the controller

`imrepl_ctrl.c:3` includes `repl_core_internal.h` and pulls
`recompute_autonormals`, `flatten_commands`, `update_render_state_strings`,
`update_cam_lines`, `apply_init_bootstrap`, `ensure_init_bootstrap_ready`,
`repl_copy_predef_values`, `repl_restore_predef_values`. By name, these are
pipeline operations, not test internals. Promote them to a small
`repl_pipeline.h` (or fold into `repl_core.h`). Keep `_internal.h` strictly for
test-only surfaces. Same hygiene fix for the controller reading `g_predef_vars`
and `g_num_predef_vars` globals at `imrepl_ctrl.c:65–66` — wrap behind a
`repl_eval_predef_view()` accessor that returns `{ vars*, count }`.

#### R5. Slim and group `SceneRenderConfig`

After R1 lands, the struct loses ~6 fields. The remainder (122-line flat
struct in `scene_render_types.h`) is naturally three groups:

- `SceneCameraView`  — `cam_dist/rx/ry/tx/ty/tz`, `cam_motion_glow`,
  multisample, line smooth.
- `SceneOverlayFlags` — `wireframe`, grid/axes themes, show_guides/vpoints/
  vnums/normals/light_indicators/vertex_outlines, backdrop_mode.
- `SceneGuideInputs` — `flat_program`, `focus`, `guide_snapshot`,
  `cursor_block_*`, `edit_line_idx`, `cursor_func_scope_mask`,
  `cursor_call_src_cmd_idx`, `alpha_scale`.

This is mechanical and makes `test_scene_render` setups much clearer; future
config additions land in the right "drawer" instead of being appended to the
end.

#### R6. Split the typed-state facade by ownership

Convert `repl_state.h` from "one big pile of accessors" into two headers:

- `repl_state_views.h` — read-only accessors (`repl_state_render()`,
  `repl_state_replay()`, etc.). Safe to include from views.
- `repl_state_owners.h` — mutating accessors (`*_mut()`, `*_reset()`,
  `*_set_*()`). Includable only from owner modules and the controller.

Then add a Makefile rule that bans `#include "repl_state_owners.h"` from
`scene_*.c` and `ui_*.c`. This is the structural fix that prevents R2-style
regressions from creeping back. (It does require touching every current
include of `repl_state.h`; do it after R1/R2 so the move and the cleanup land
together.)

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

R1 → R2 → R3 → R5 (config slimming) → R6 (header split) → R7 (guards) →
R4 (controller internal cleanup) → R8 (rename) → R9 (export split,
optional). R1 is highest leverage and unblocks R5 + R6. R2 is independently
valuable and can be parallelized with R1.

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
