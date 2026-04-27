# Plan: Extract a Controller Layer (Option B, Phase 1)

## Context

The immediate-mode REPL codebase has grown large enough that "where does this belong?" is getting answered ad-hoc. The original push-architecture plan (`feature/push-architecture-refinement.md`) tried to fix this by introducing generic scene callbacks, separate `ReplGeometryRenderPlan`, and renaming three transitional `scene_*` files to `repl_geometry_*`. After review we agreed that approach is too much boilerplate for a one-frontend codebase: it adds callback indirection, duplicates fields between scene config and a parallel REPL plan, and forces file moves on ~1400 lines of overlay code that has exactly one caller.

This plan switches to **Option B**: a controller layer between `repl_*` and `scene_*`/`ui_*`. The valuable isolation is REPL-vs-everything-else, not scene-vs-REPL. Scene modules may know about `FlatProgramView`/`CmdType` (via header includes), but they consume per-frame configs the controller builds and never read REPL globals or call REPL state APIs directly.

This Phase 1 plan focuses narrowly on **peeling scene-related logic out of `repl_core.c` into a new controller translation unit**. Boundary checks, replay fade simplification, 2D HUD relocation, and the deeper scene-purity work are scoped as follow-ups.

## Revised Tenets (Option B)

1. **REPL doesn't know about scene decoration or UI.** The REPL parses, stores, flattens, replays, exports. It owns its own GL emission for *user geometry* through `repl_executor.c` (the deliberately narrow live-GL gate). It does not own viewport, projection, camera, clear, accumulation, decorators (grid/axes/backdrop/lights/orbit), or any 2D editor chrome.
2. **The scene owns the 3D stage, not the actor.** Scene sets up viewport/projection/camera/clear/accumulation/lighting baseline and draws decorators. Scene calls `repl_execute_program()` as a library to draw the actor — no callback indirection. Scene reads only the per-frame `SceneRenderConfig`/`FrameRenderContext` it was given.
3. **The UI owns 2D editor chrome.** Code panel, menus, popups, color picker, help, profile HUD, status banner.
4. **The controller wires them.** A new translation unit (`repl_app.c`/`repl_app.h`, owning what `repl_core::display_func` does today) builds `SceneRenderConfig` from REPL state, resolves clear color, calls `scene_render_3d_scene(&cfg)`, then drives UI rendering. The controller is the only place that touches both REPL accessors and scene/UI render entrypoints.
5. **Live GL allowlist stays small.** `scene_*.c`, `ui_*.c`, `repl_executor.c`, `sample.c`. No new entries.
6. **REPL-aware overlays stay in `scene_*`.** `scene_overlays.c`, `scene_geometry_guides.c`, `scene_transform_guides.c` keep their names and homes. They consume `SceneRenderConfig`/`SceneGuideSnapshot` like the pure scene modules, just with `FlatProgramView` and cursor scope fields added to those structs. No file moves in this plan.

## What's In Scope For This Plan

- Create the controller translation unit; move display orchestration and scene config building out of `repl_core.c`.
- `scene_render_3d_scene()` takes an explicit `const SceneRenderConfig *` argument.
- Move `scene_apply_clear_color()` so it consumes the `FlatProgramView` already on the config (no new namespace, no REPL-side resolver function — clear color resolution stays in scene because it's a frame-setup concern that operates on data the controller already passed in).
- Stop `scene_render.c` from mutating `repl_state_render_mut()->accum_jitter_*`. Jitter becomes scene-local loop state.
- Two grep-based boundary checks added to the Makefile and wired into `make test`.

(Replay baseline-restore stays in `scene_render.c` for now. It moves when the replay fade-ring simplification plan lands, since both touch the same loop.)

## What's Explicitly Out Of Scope (future plans)

- **Replay fade-ring simplification.** Collapsing the `ReplayFadeBatch[24]` ring into a single `ReplReplayHighlight` is its own focused change. Step-back, baseline restore, and variable-expansion code stay untouched in this plan and the next.
- **2D replay HUD relocation.** `draw_replay_hud()` (scene_render.c:237–369) is 2D code in a 3D file; move to `ui_replay_hud.c` separately.
- **Scene config slimming.** Reducing the ~40-field `SceneRenderConfig` is for after the controller exists and we can see what's actually used.
- **Pure-scene grep guard for `scene_grid/axes/backdrop/lights`.** Those four files are already clean per the exploration; locking them in mechanically is a small follow-up.
- **Renaming `scene_overlays`/`scene_geometry_guides`/`scene_transform_guides`.** Stays in `scene_*`. Permanent home.

## Critical Files

| File | Role in this plan |
|------|-------------------|
| `repl_core.c` | Source for: `display_func` (line 651), `scene_render_config_build` (line 555), `scene_execute_adapter` (~line 535), `scene_execute_reset_adapter` (line 550), `reshape_func` (line 714). All move to `repl_app.c`. |
| `repl_core.h` | Drop forward declarations of moved functions; add include for new `repl_app.h` only where needed (likely just `sample.c`). |
| `repl_app.c` (new) | New translation unit owning the controller. |
| `repl_app.h` (new) | Public surface: `repl_app_init`, `repl_app_display_frame`, `repl_app_reshape`, plus the few symbols `sample.c` needs. |
| `scene_render.c` | `scene_render_3d_scene(void)` becomes `scene_render_3d_scene(const SceneRenderConfig *)`. Internal `scene_render_config_build` call at line 633 deleted. `scene_apply_clear_color` (line 423) keeps its job but moves to be called once before the accum loop. Jitter table stays here; jitter writeback to `repl_state_render_mut` (lines 645–646, 650–651) deleted — jitter becomes a local in `scene_render_3d_scene`. `repl_replay_restore_baseline_predef_values()` calls inside `scene_render_3d_scene` (lines 644, 656) deleted; controller does it once before the call. |
| `scene_render.h` | Update signature of `scene_render_3d_scene`; remove public declaration of `scene_render_config_build` (it lives in the controller now). |
| `scene_render_types.h` | Unchanged this plan. (Slimming deferred.) |
| `sample.c` | Replace `repl_display_func()` / `repl_keyboard_func()` / etc. wiring with `repl_app_*` equivalents OR keep the existing `repl_*` names as thin shims that forward into `repl_app_*`. Pick whichever produces fewer churn diffs. |
| `Makefile` | Add `check-live-gl-allowlist` and `check-repl-no-scene-ui` targets; wire into `test`. |

`scene_overlays.c`, `scene_geometry_guides.c`, `scene_transform_guides.c`, `scene_grid.c`, `scene_axes.c`, `scene_backdrop.c`, `scene_lights.c`: **not modified in this plan**. The exploration confirmed scene_grid/axes/backdrop/lights are already clean; the three REPL-aware scene files already consume `SceneRenderConfig`/`SceneGuideSnapshot` and don't read REPL state directly (only `repl_eval_expr` / `repl_eval_parse_exprs`, which are pure expression-eval calls, not state reads — leave them).

## Step-by-Step Refactor

Each step is shippable on its own. After each step: `make test`, plus a manual smoke test of the `./sample` binary covering startup, a built-in example, replay play/pause/step-back, save/load.

### Step 1 — Create the controller skeleton

Files: new `repl_app.c`, `repl_app.h`.

- Define `repl_app.h` with three forward declarations: `void repl_app_init(void)`, `void repl_app_display_frame(void)`, `void repl_app_reshape(int w, int h)`. (Input handlers stay where they are for now — they live in `repl_editor.c` and aren't part of the scene seam.)
- Define `repl_app.c` containing nothing yet but the includes (`repl_core.h`, `repl_state.h`, `repl_replay.h`, `repl_flatten.h`, `repl_executor.h`, `scene_render.h`, all `ui_*.h` panels, `prof.h`).
- Add `repl_app.c` to the Makefile object list.
- Build green; tests still pass. No behavior change.

**Exit criterion:** new TU compiles, links, is empty.

### Step 2 — Move `display_func` body into `repl_app_display_frame`

Files: `repl_app.c`, `repl_core.c`, `repl_core.h`, `sample.c`.

- Copy `repl_core.c::display_func` (lines 651–712) verbatim into `repl_app.c` as `repl_app_display_frame`. Keep `repl_display_func()` in `repl_core.c` as a one-liner wrapper that calls `repl_app_display_frame()`. (Wrapper is so we don't have to change `sample.c`'s GLUT registration this step.)
- Same for `reshape_func` → `repl_app_reshape`. Wrap.
- All scene/ui includes that `display_func` needed move out of `repl_core.c` into `repl_app.c`. Trim `repl_core.c`'s header includes accordingly: scene_render.h, ui_panels.h, ui_menu_bar.h, ui_autocomplete_panel.h, ui_help_overlay.h, ui_variable_panel.h, ui_profile_panel.h.
- `repl_core.h` declarations for `repl_display_func` / `repl_reshape_func` stay (they're the public API `sample.c` already calls).

**Exit criterion:** `repl_core.c` no longer includes any `scene_*.h` or `ui_*.h`. The display function still runs from there but only as a forwarding shim. Visual behavior unchanged.

### Step 3 — Move `scene_render_config_build` into the controller

Files: `repl_app.c`, `repl_core.c`, `scene_render.h`.

- Move `scene_render_config_build` (repl_core.c:555–645) and the two adapter functions `scene_execute_adapter` / `scene_execute_reset_adapter` into `repl_app.c` as static functions. Rename the public one to `repl_app_build_scene_config(SceneRenderConfig *out)` while you're there — it isn't a scene function, it's a controller function that produces a scene input.
- Remove `scene_render_config_build`'s declaration from `scene_render.h`.
- Update the only other caller (`scene_render.c::scene_render_3d_scene` at line 633 calls it once for viewport rect) — that caller will go away in step 4. For now, give `scene_render.c` a temporary fallback: if no config has been pushed yet, call a thin shim `repl_app_build_scene_config_global()` exposed from `repl_app.h`. (We'll delete this shim in step 4.)
- `repl_core.c`'s ~13 `repl_state_*` accessors used only by config build move with the function. `repl_core.c` shrinks substantially.

**Exit criterion:** `scene_render_config_build` lives in the controller. `repl_core.c` has no more scene-side build code.

### Step 4 — `scene_render_3d_scene` takes an explicit config

Files: `scene_render.c`, `scene_render.h`, `repl_app.c`.

- Change signature: `void scene_render_3d_scene(const SceneRenderConfig *config)`.
- Delete the call to `scene_render_config_build` at scene_render.c:633. Delete the call to `scene_render_config_build` inside `render_3d_scene_pass` at line 440 — that pass now takes `const SceneRenderConfig *` too. The frame context build (`scene_prepare_frame_context`) stays inside the per-pass function.
- `repl_app_display_frame` builds the config once via `repl_app_build_scene_config(&cfg)` and passes `&cfg` to `scene_render_3d_scene`.
- Delete the temporary `repl_app_build_scene_config_global()` shim from step 3.

**Exit criterion:** `scene_render.c` does not call back into the controller for config. The function takes an explicit config argument.

### Step 5 — Move clear-color application to be a one-shot before the accum loop

Files: `scene_render.c`, `repl_app.c`.

- `scene_apply_clear_color` (scene_render.c:423) currently walks `flat_program` and calls `glClearColor`. Today it runs once inside `scene_render_3d_scene` before the accum loop (line 636). That's already structurally correct.
- The only change: ensure `repl_executor.c` does NOT call `glClearColor` while running inside the scene's user-geometry execution path. Verify `CMD_CLEAR_COLOR` handling in the executor; if it currently calls `glClearColor` mid-frame, gate it behind a flag or treat the command as state-only. (Investigate first; may be a no-op already.)
- No other change. We are explicitly NOT introducing a `repl_geometry_resolve_clear_color()` REPL-side function — the original plan's seam was unnecessary because `FlatProgramView` is already on the config and scene can read it once.

**Exit criterion:** clear color is applied exactly once per frame, before the accum loop, by scene code reading the `FlatProgramView` on the config it was given. `glClearColor` is not called inside `repl_executor.c` during user geometry execution.

### Step 6 — Move accum jitter ownership fully into `scene_render.c`

Files: `scene_render.c`, controller.

- Delete the `accum_jitter_x`/`accum_jitter_y` fields from `SceneRenderConfig` if they're not read elsewhere. (Verify: search shows them on `repl_state_render` and copied into config at repl_core.c:578–579, then read inside `scene_apply_projection` at scene_render.c:103–106. Only scene_render reads them.)
- The jitter table (scene_render.c:26–43) is already local to `scene_render.c`. Make `g_jitter_table` indexed by `sample_idx` directly inside `scene_render_3d_scene` and pass jitter into `render_3d_scene_pass` as parameters (or via `FrameRenderContext`).
- Delete the writes at scene_render.c:645–646 and 650–651 that mutate `repl_state_render_mut()->accum_jitter_*`. Scene no longer mutates REPL state for its own loop variable.
- Drop `accum_jitter_x/y` from `SceneRenderConfig`.

**Exit criterion:** `scene_render.c` does not call any `repl_state_*_mut` function. Grep for `repl_state_.*_mut` in `scene_*.c` returns nothing except whatever is still legitimately needed (audit).

### Step 7 — Boundary checks

Files: `Makefile`.

Add two grep-based targets:

```makefile
check-live-gl-allowlist:
	@echo "Checking live GL call allowlist..."
	@bad=$$(grep -lE '\b(gl[A-Z][a-z][a-zA-Z]*|glu[A-Z][a-z][a-zA-Z]*)\(' \
		repl_*.c \
		| grep -v '^repl_executor\.c$$' \
		| grep -v '^repl_command_spec\.c$$' \
		| grep -v '^repl_examples\.c$$' \
		| grep -v '^repl_export\.c$$' \
		| grep -v '^repl_replay_annotations\.c$$' \
		| grep -v '^repl_parser\.c$$' \
		| grep -v '^repl_state\.c$$' \
		| grep -v '^repl_autocomplete\.c$$' \
		); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: live GL calls outside allowlist:"; echo "$$bad"; exit 1; \
	fi
	@echo "Live GL allowlist OK."

check-repl-no-scene-ui:
	@echo "Checking repl_*.c does not include scene_*.h or ui_*.h..."
	@bad=$$(grep -lE '#\s*include\s*"(scene_|ui_)' repl_*.c \
		| grep -v '^repl_app\.c$$'); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: scene_/ui_ included from repl_* outside controller:"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "REPL controller boundary OK."

check-boundaries: check-live-gl-allowlist check-repl-no-scene-ui

test: check-boundaries
```

The exclusion list for `check-live-gl-allowlist` enumerates the `repl_*.c` files that contain the *string* `glFoo(` as text (parser, command spec, examples, export, replay annotations) but never make live calls. Verify each excluded file actually has zero live GL calls; tighten the exclusions later if any are wrong. The single allowed live-GL `repl_*.c` is `repl_executor.c`.

`check-repl-no-scene-ui` enforces that only `repl_app.c` (the controller) may include scene/UI headers from inside `repl_*`. After step 2 this check passes; it then guards against future drift.

**Exit criterion:** `make check-boundaries` is green and `make test` runs it.

### Step 8 — Documentation pass

Files: `MODULES.md`, `ARCHITECTURE.md`, `CLAUDE.md`.

- Replace the "REPL geometry render plan / generic scene callback" sections with the controller model: `repl_app.c` is the seam.
- Update the "Where to put new code" guide:
  - New geometry execution behavior → `repl_executor.c`
  - New decorator → `scene_*`
  - New 3D REPL-aware overlay → still in `scene_*` (consumes `FlatProgramView` from config)
  - New 2D UI → `ui_*`
  - New per-frame wiring → `repl_app.c`
- Document the live-GL allowlist (4 entries) and the controller-only scene/UI include rule.
- Replace "REPL doesn't know about rendering" with "REPL doesn't know about scene decoration or UI."
- Mark the `feature/push-architecture-refinement.md` plan as superseded by `feature/controller-extraction.md` (this plan, copied into the feature directory after approval).

**Exit criterion:** docs match the implemented architecture; the older push-architecture plan is annotated as superseded.

## Future Refactors (Out Of Scope, In Likely Order)

1. **Replay fade-ring simplification + baseline-restore relocation.** Collapse `ReplayFadeBatch[24]` + aging + skip-limits + per-batch baseline-restore into a single `ReplReplayHighlight` (kind/range/alpha). At the same time, move the surviving `repl_replay_restore_baseline_predef_values()` calls out of `scene_render.c` and into the controller (single per-frame restore, possibly per-pass via a `SceneRenderConfig::on_begin_pass` hook if accum AA + animated `t` needs it). Keep step-back, baseline-restore, variable-expansion, replay annotations — all untouched. Delete `repl_bench_fade_install`/`clear`. ~300 lines net deletion.
2. **2D replay HUD relocation.** Move `draw_replay_hud()` (scene_render.c:237–369) into a new `ui_replay_hud.c`. Drop `replay_pc`, `replay_total_cmds`, `replay_state_val`, `replay_speed`, `replay_expand_args`, `replay_mode`, `code_panel_layout` from `SceneRenderConfig`. ~140 lines move.
3. **`SceneRenderConfig` slimming.** With (1) and (2) done, the scene config has shed: replay HUD fields, `replay_has_fades`/`replay_base_limit` (replaced by `ReplReplayHighlight`), `accum_jitter_x/y` (already gone in this plan). Audit remaining fields and shrink.
4. **Pure-scene grep guard for the four neutral files.** `scene_grid.c`, `scene_axes.c`, `scene_backdrop.c`, `scene_lights.c` already pass; lock in mechanically.
5. **`SceneGuideSnapshot` in scene_guides_shared.h.** Audit fields once overlays consume the slimmer scene config; drop anything no longer used.

None of these are required for the controller extraction to land cleanly.

## Verification

End-to-end test plan, run after each step and again at the end:

1. **Build matrix.**
   - `make clean && make sample` — default freeglut build
   - `make sample USE_GL_STUBS=1` — stub-mode build (catches missing symbols even on machines without GL dev libs)
   - `make glut` — system GLUT path on macOS
2. **Test suite.** `make test`. Existing 21 test executables must all pass. After step 8, this also runs `check-boundaries`.
3. **Boundary checks.** `make check-boundaries` standalone passes. Try deliberately introducing a `#include "scene_render.h"` in `repl_editor.c` — the check should fail loudly. Revert.
4. **Manual smoke test of `./sample`.**
   - Startup with no args. Verify clear color, grid, axes, backdrop, lights all render. Verify no `repl_state` mutation comes from the scene side (run with `--dump-code` if useful).
   - Load each built-in example via F12; verify all render correctly.
   - Toggle wireframe (F-key), accumulation AA (`--noaccum` startup arg vs. default), grid theme cycling (F3), axes theme cycling (F4).
   - Replay path: Ctrl+G to start replay; Space pause; `<` step back across an animated `t`-driven block (e.g. the rotating-cube example); verify variable values are restored correctly. `e` to toggle expand-args. `m` to toggle vertex/polygon mode. Esc to stop.
   - Camera path: orbit drag, pan, zoom, momentum spin-down. Verify orbit-target gizmo shows during motion and fades out.
   - Save: Ctrl+S, then exit, then `./sample output.c`, then verify scene reloads identically.
5. **Visual regression spot-check.** Side-by-side `./sample` from `main` vs. the refactor branch on three examples (a simple cube, the tessellator example, a for-loop animation). Pause replay at the same PC on both; visually diff. No expected differences.

## Rollback

Each step is a self-contained commit. If a step regresses behavior, revert that single commit; downstream steps depend in order but each individual revert is mechanical (no schema migrations, no on-disk format changes — the save/load text format is untouched).
